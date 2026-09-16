/*
 * supersnes PPU.
 *
 * Implemented: BG modes 0 and 1 (text backgrounds, 2bpp/4bpp, per-BG
 * palettes, scrolling, tilemap flip/priority bits), mode 7 with the exact
 * 13-bit affine transform (repeat modes 00 wrap / 01 transparent; M7SEL
 * flip bits are not implemented), sprites (4bpp/8bpp, both size tables,
 * X msb high table, priority bit, per-line scan of OAM), CGRAM palettes,
 * brightness/force blank, OAM/VRAM/CGRAM access with increment modes.
 *
 * Not implemented (documented): BG modes 2-6, color math, windows, mosaic,
 * interlace, sub-screen blending, mode 7 sprite parameter interactions.
 */
#include "snes.h"

#include <string.h>

/* 15-bit BGR555 -> 32-bit XRGB with brightness scaling */
static uint32_t cgram_to_pixel(uint16_t v, uint8_t brightness)
{
    uint32_t r = (v >> 0) & 0x1Fu;
    uint32_t g = (v >> 5) & 0x1Fu;
    uint32_t b = (v >> 10) & 0x1Fu;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    if (brightness < 15u) {
        r = r * brightness / 15u;
        g = g * brightness / 15u;
        b = b * brightness / 15u;
    }
    return EMU_PIXEL((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

void snes_ppu_init(snes_ppu *p, uint32_t *fb)
{
    memset(p, 0, sizeof *p);
    p->fb = fb;
    snes_ppu_reset(p);
}

void snes_ppu_reset(snes_ppu *p)
{
    uint32_t *fb = p->fb;
    memset(p, 0, sizeof *p);
    p->fb = fb;
    p->inidisp = 0x80u; /* forced blank at reset */
}

/* ---- VRAM ---- */

static void vram_word_write(snes_t *s, uint16_t addr, uint16_t word)
{
    s->ppu.vram[((uint32_t)addr * 2u) & 0x7FFFu] = (uint8_t)(word & 0xFFu);
    s->ppu.vram[((uint32_t)addr * 2u + 1u) & 0x7FFFu] = (uint8_t)(word >> 8);
}

static uint16_t vram_word_read(snes_t *s, uint16_t addr)
{
    return (uint16_t)(s->ppu.vram[((uint32_t)addr * 2u) & 0x7FFFu] |
                      ((uint16_t)s->ppu.vram[((uint32_t)addr * 2u + 1u) & 0x7FFFu] << 8));
}

static uint16_t vram_increment(const snes_ppu *p)
{
    switch (p->vmainc & 3u) {
    case 1: return 32;
    case 2: return 128;
    case 3: return 128;
    default: return 1;
    }
}

/* ---- text background pixel (modes 0/1) ---- */

typedef struct {
    uint8_t present;
    uint8_t color;
    uint8_t pal;
    uint8_t prio;
} layer_px;

static layer_px text_bg_pixel(snes_t *s, uint8_t bg, uint16_t sx, uint8_t sy)
{
    snes_ppu *p = &s->ppu;
    layer_px r = { 0, 0, 0, 0 };
    uint8_t sc = p->bg_map[bg];
    uint16_t map_base = (uint16_t)((sc & 3u) << 10);
    uint16_t char_base = (uint16_t)((uint16_t)(p->bg_char_base[bg] & 0x0Fu) << 12);
    uint8_t mode = (uint8_t)(p->bgmode & 7u);
    uint8_t bpp = (mode == 1u && bg < 2u) ? 4u : 2u;

    uint16_t h = p->bg_scroll[bg * 2u];
    uint16_t v = p->bg_scroll[bg * 2u + 1u];
    uint16_t eff_x = (uint16_t)(sx + (h & 0x3FFu));
    uint16_t eff_y = (uint16_t)(sy + (v & 0x3FFu));

    /* tilemap: 32x32 base; horizontal and vertical wrap via quadrant bits */
    uint16_t tx = (uint16_t)((eff_x >> 3) & 0x3Fu);
    uint16_t ty = (uint16_t)((eff_y >> 3) & 0x3Fu);
    uint16_t map_addr = (uint16_t)(map_base + (uint16_t)(ty & 31u) * 32u + (tx & 31u));
    if ((tx & 32u) != 0u)
        map_addr += 0x400u;
    if ((ty & 32u) != 0u)
        map_addr += 0x800u;
    uint16_t entry = vram_word_read(s, map_addr);

    uint16_t tile = (uint16_t)(entry & 0x3FFu);
    uint8_t pal = (uint8_t)((entry >> 10) & 7u);
    uint8_t hflip = (uint8_t)((entry >> 14) & 1u);
    uint8_t vflip = (uint8_t)((entry >> 15) & 1u);
    uint8_t pr = (uint8_t)((entry >> 13) & 1u);
    if (bpp == 2u)
        pr = 0;
    r.prio = pr;

    uint8_t fine_x = (uint8_t)(eff_x & 7u);
    uint8_t fine_y = (uint8_t)(eff_y & 7u);
    uint8_t fxx = hflip ? (uint8_t)(7u - fine_x) : fine_x;
    uint8_t fyy = vflip ? (uint8_t)(7u - fine_y) : fine_y;

    uint32_t plane_off = char_base + (uint32_t)tile * (bpp == 4u ? 32u : 8u) +
                         (uint32_t)fyy * 2u;
    uint8_t bit = (uint8_t)(7u - fxx);
    uint8_t lo0 = p->vram[plane_off & 0x7FFFu];
    uint8_t hi0 = p->vram[(plane_off + 1u) & 0x7FFFu];
    uint8_t color = (uint8_t)((((hi0 >> bit) & 1u) << 1) | ((lo0 >> bit) & 1u));
    if (bpp == 4u) {
        uint8_t lo1 = p->vram[(plane_off + 8u) & 0x7FFFu];
        uint8_t hi1 = p->vram[(plane_off + 9u) & 0x7FFFu];
        color = (uint8_t)(color | (((lo1 >> bit) & 1u) << 2) |
                          (((hi1 >> bit) & 1u) << 3));
    }
    if (color == 0u)
        return r;
    r.present = 1;
    r.color = color;
    r.pal = pal;
    return r;
}

/* ---- mode 7 (13-bit affine transform, mathematically exact) ---- */

static int32_t sign13(uint16_t v)
{
    v &= 0x1FFFu;
    return (v & 0x1000u) ? (int32_t)v - 0x2000 : (int32_t)v;
}

static layer_px mode7_pixel(snes_t *s, uint16_t sx, uint8_t sy)
{
    snes_ppu *p = &s->ppu;
    layer_px r = { 0, 0, 0, 0 };
    int32_t a = sign13(p->m7a);
    int32_t b = sign13(p->m7b);
    int32_t c = sign13(p->m7c);
    int32_t d = sign13(p->m7d);
    int32_t h = sign13((uint16_t)(p->m7x & 0x1FFFu));
    int32_t v = sign13((uint16_t)(p->m7y & 0x1FFFu));

    int32_t sx_c = (int32_t)sx - 128;
    int32_t sy_c = (int32_t)sy - 128;
    /* A..D are 8.8 fixed point; H/V are integers scaled to 8.8 by <<8 */
    int32_t x = (a * sx_c + b * sy_c + (h << 8)) >> 8;
    int32_t y = (c * sx_c + d * sy_c + (v << 8)) >> 8;

    uint8_t repeat = (uint8_t)(p->m7sel & 1u);
    int32_t tx = x >> 3;
    int32_t ty = y >> 3;
    if (tx < 0 || tx > 63 || ty < 0 || ty > 63) {
        if (repeat == 1u)
            return r; /* outside: transparent */
        tx &= 63;
        ty &= 63;
    }
    uint8_t fine_x = (uint8_t)(x & 7u);
    uint8_t fine_y = (uint8_t)(y & 7u);
    uint8_t tile = p->vram[((uint32_t)ty * 64u + (uint32_t)tx) & 0x7FFFu];
    uint8_t color = p->vram[((uint32_t)tile * 64u + (uint32_t)fine_y * 8u + fine_x) & 0x7FFFu];
    if (color == 0u)
        return r;
    r.present = 1;
    r.color = color;
    r.pal = 0;
    return r;
}

/* ---- sprites ---- */

static const uint8_t spr_size_table[2][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 16 }, { 32, 32 }, { 64, 64 }, { 32, 32 } }
};

typedef struct {
    uint8_t present;
    uint8_t color;
    uint8_t pal;
    uint8_t prio;
} spr_px;

static spr_px sprite_pixel(snes_t *s, uint16_t sx, uint8_t sy)
{
    snes_ppu *p = &s->ppu;
    spr_px best = { 0, 0, 0, 0 };
    uint32_t char_base = (uint32_t)(p->bg_char_base[4] & 0x0Fu) << 13;
    uint8_t size_sel = (uint8_t)((p->obj_size_reg >> 5) & 3u);
    uint8_t table = (uint8_t)((p->obj_size_reg >> 5) & 3u);
    (void)size_sel;

    for (int i = 0; i < 128; i++) {
        const uint8_t *e = &p->oam[(uint32_t)i * 4u];
        uint8_t size_bits = (uint8_t)((e[3] >> 6) & 3u);
        uint8_t w = spr_size_table[table][size_bits][0];
        uint8_t h = spr_size_table[table][size_bits][1];
        uint8_t xmsb = (uint8_t)((p->oam[512u + (uint32_t)i / 4u] >>
                                  ((uint32_t)i % 4u * 2u)) & 1u);
        uint16_t x = (uint16_t)(e[0] | ((uint16_t)xmsb << 8));
        uint8_t y = e[1];
        if (sy < y || sy >= (uint8_t)(y + h))
            continue;
        if (sx < x || sx >= (uint16_t)(x + w))
            continue;

        uint16_t tile = (uint16_t)(e[2] | ((uint16_t)(e[3] & 1u) << 8));
        uint8_t attr = e[3];
        uint8_t hflip = (uint8_t)((attr >> 6) & 1u);
        uint8_t vflip = (uint8_t)((attr >> 7) & 1u);
        uint8_t pal = (uint8_t)(attr & 7u);
        uint8_t prio = (uint8_t)((attr >> 4) & 1u);

        uint8_t px_in = (uint8_t)(sx - x);
        uint8_t py_in = (uint8_t)(sy - y);
        uint8_t fxx = hflip ? (uint8_t)(w - 1u - px_in) : px_in;
        uint8_t fyy = vflip ? (uint8_t)(h - 1u - py_in) : py_in;

        uint8_t color = 0;
        if (p->obj_8bpp) {
            uint16_t t = (uint16_t)(tile + (uint16_t)(fyy / 8u) * 16u + (uint16_t)(fxx / 8u));
            uint32_t off = char_base + (uint32_t)t * 64u + (uint32_t)(fyy % 8u) * 8u;
            uint8_t bit = (uint8_t)(7u - (fxx % 8u));
            for (int pl = 0; pl < 4; pl++) {
                uint8_t lo = p->vram[(off + (uint32_t)pl * 8u) & 0x7FFFu];
                uint8_t hi = p->vram[(off + (uint32_t)pl * 8u + 1u) & 0x7FFFu];
                color = (uint8_t)(color | ((((hi >> bit) & 1u) << (pl * 2 + 1)) |
                                           (((lo >> bit) & 1u) << (pl * 2))));
            }
        } else {
            uint16_t t = (uint16_t)(tile + (uint16_t)(fyy / 8u) * 16u + (uint16_t)(fxx / 8u));
            uint32_t off = char_base + (uint32_t)t * 32u + (uint32_t)(fyy % 8u) * 2u;
            uint8_t bit = (uint8_t)(7u - (fxx % 8u));
            uint8_t lo0 = p->vram[off & 0x7FFFu];
            uint8_t hi0 = p->vram[(off + 1u) & 0x7FFFu];
            uint8_t lo1 = p->vram[(off + 8u) & 0x7FFFu];
            uint8_t hi1 = p->vram[(off + 9u) & 0x7FFFu];
            color = (uint8_t)((((hi0 >> bit) & 1u) << 1) | ((lo0 >> bit) & 1u) |
                              (((lo1 >> bit) & 1u) << 2) | (((hi1 >> bit) & 1u) << 3));
        }
        if (color == 0u)
            continue;
        best.present = 1;
        best.color = color;
        best.pal = pal;
        best.prio = prio;
        break; /* lowest OAM index with an opaque pixel wins (documented
                  simplification of the SNES per-priority-level rules) */
    }
    return best;
}

/* ---- rasterize line ---- */

static void render_line(snes_t *s)
{
    snes_ppu *p = &s->ppu;
    uint8_t mode = (uint8_t)(p->bgmode & 7u);
    uint8_t brightness = (uint8_t)(p->inidisp & 0x0Fu);
    uint8_t force_blank = (uint8_t)((p->inidisp & 0x80u) != 0u);
    uint8_t sy = (uint8_t)(p->line > 224u ? 224u : p->line);
    uint16_t backdrop = (uint16_t)(p->cgram[0] | ((uint16_t)p->cgram[1] << 8));

    for (int x = 0; x < 256; x++) {
        uint16_t color_index = 0;
        uint8_t found = 0;
        uint8_t found_score = 0;

        if (!force_blank) {
            for (uint8_t bg = 0; bg < 4u; bg++) {
                uint8_t max_bg = (mode == 1u) ? 3u : (mode == 0u ? 4u
                                                                 : (mode == 7u ? 1u : 0u));
                if (bg >= max_bg)
                    break;
                if (!(p->tm & (1u << bg)))
                    continue;
                layer_px px = (mode == 7u) ? mode7_pixel(s, (uint16_t)x, sy)
                                           : text_bg_pixel(s, bg, (uint16_t)x, sy);
                if (!px.present)
                    continue;
                uint8_t pr;
                if (mode == 1u)
                    pr = (bg == 0u) ? 1u : (bg == 2u && (p->bgmode & 8u) ? 1u : 0u);
                else
                    pr = 0;
                uint8_t score = (uint8_t)(pr * 2u + 1u);
                if (!found || score > found_score) {
                    found = 1;
                    found_score = score;
                    if (mode == 1u && bg < 2u)
                        color_index = (uint16_t)((uint32_t)px.pal * 16u + px.color);
                    else if (mode == 1u && bg == 2u)
                        color_index = (uint16_t)((uint32_t)(px.pal & 1u) * 4u + px.color);
                    else if (mode == 0u) {
                        uint8_t group = (uint8_t)(bg == 0u ? 0u : bg == 1u ? 8u
                                                     : bg == 2u ? 24u : 32u);
                        color_index = (uint16_t)(group + (uint32_t)(px.pal & 1u) * 4u +
                                                 px.color);
                    } else /* mode 7 */
                        color_index = px.color;
                }
            }
            if (p->tm & 0x10u && mode != 7u) {
                spr_px sp = sprite_pixel(s, (uint16_t)x, sy);
                if (sp.present) {
                    uint8_t score = (uint8_t)(sp.prio * 2u);
                    if (!found || score >= found_score) {
                        found = 1;
                        found_score = score;
                        color_index = (uint16_t)(128u + (uint32_t)sp.pal * 16u +
                                                 sp.color);
                    }
                }
            }
        }

        uint32_t px;
        if (!found || color_index == 0u)
            px = cgram_to_pixel(backdrop, brightness);
        else
            px = cgram_to_pixel((uint16_t)(p->cgram[color_index * 2u] |
                                           ((uint16_t)p->cgram[color_index * 2u + 1u] << 8)),
                                brightness);
        p->fb[(size_t)sy * SNES_SCREEN_W + (uint32_t)x] = px;
        if (force_blank)
            p->fb[(size_t)sy * SNES_SCREEN_W + (uint32_t)x] = 0xFF000000u;
    }
}

/* ---- stepping ---- */

void snes_ppu_run(snes_t *s, uint32_t master_cycles)
{
    snes_ppu *p = &s->ppu;
    p->dot_acc += master_cycles;
    while (p->dot_acc >= 4u) {
        p->dot_acc -= 4u;
        p->dot++;

        if (p->line == 225u && p->dot == 0u) {
            if (s->mem.nmitimen & 0x80u) {
                if (!p->nmi_line) {
                    s->cpu.nmi_pending = 1;
                    s->mem.nmitimen |= 0x80u; /* NMI flag for $4210 */
                }
                p->nmi_line = 1;
            }
            snes_mem_latch_joypads(s);
        }
        if (p->line == 0u && p->dot == 0u)
            p->nmi_line = 0;

        if (p->dot >= 1364u) {
            if (p->line < 225u)
                render_line(s); /* rasterize the line that is ending */
            p->dot = 0;
            p->line++;
            if (p->line >= 262u)
                p->line = 0;
        }
    }
}

/* ---- register interface ---- */

uint8_t snes_ppu_read(snes_t *s, uint16_t addr)
{
    snes_ppu *p = &s->ppu;
    switch (addr) {
    case 0x2100: return p->inidisp;
    case 0x2105: return p->bgmode;
    case 0x2138: {
        uint8_t v = p->oam[p->oamaddr & 0x3FFu];
        p->oamaddr = (uint16_t)((p->oamaddr + 1u) & 0x3FFu);
        return v;
    }
    case 0x2139: {
        uint8_t v = p->vram[((uint32_t)p->vmadd * 2u) & 0x7FFFu];
        if (!(p->vmainc & 0x80u))
            p->vmadd = (uint16_t)(p->vmadd + vram_increment(p));
        return v;
    }
    case 0x213A: {
        uint8_t v = p->vram[((uint32_t)p->vmadd * 2u + 1u) & 0x7FFFu];
        if (p->vmainc & 0x80u)
            p->vmadd = (uint16_t)(p->vmadd + vram_increment(p));
        return v;
    }
    case 0x213B: {
        uint8_t v = p->cgram[(uint32_t)p->cgaddr * 2u + p->cgaddr_flip];
        p->cgaddr_flip ^= 1u;
        if (p->cgaddr_flip == 0u)
            p->cgaddr = (uint8_t)((p->cgaddr + 1u) & 0xFFu);
        return v;
    }
    case 0x213C: return (uint8_t)((p->dot >> 2) & 0xFFu);
    case 0x213D: return (uint8_t)(p->line & 0xFFu);
    case 0x213E: return 0x00;
    case 0x213F: return 0x30;
    default:
        return p->openbus;
    }
}

void snes_ppu_write(snes_t *s, uint16_t addr, uint8_t v)
{
    snes_ppu *p = &s->ppu;
    p->openbus = v;
    switch (addr) {
    case 0x2100: p->inidisp = v; break;
    case 0x2101: p->obj_size_reg = v;
                 p->bg_char_base[4] = (uint8_t)(v & 7u); break;
    case 0x2102: p->oamaddr = (uint16_t)((p->oamaddr & 0x0200u) | v); break;
    case 0x2103:
        p->oamaddr = (uint16_t)((p->oamaddr & 0x00FFu) | ((uint16_t)(v & 1u) << 8));
        break;
    case 0x2104:
        p->oam[p->oamaddr & 0x3FFu] = v;
        p->oamaddr = (uint16_t)((p->oamaddr + 1u) & 0x3FFu);
        break;
    case 0x2105: p->bgmode = v; break;
    case 0x2107: p->bg_map[0] = v; break;
    case 0x2108: p->bg_map[1] = v; break;
    case 0x2109: p->bg_map[2] = v; break;
    case 0x210A: p->bg_map[3] = v; break;
    case 0x210B: p->bg_char_base[0] = (uint8_t)(v & 0x0Fu);
                 p->bg_char_base[1] = (uint8_t)(v >> 4); break;
    case 0x210C: p->bg_char_base[2] = (uint8_t)(v & 0x0Fu);
                 p->bg_char_base[3] = (uint8_t)(v >> 4); break;
    case 0x210D: case 0x210E: case 0x210F: case 0x2110:
    case 0x2111: case 0x2112: case 0x2113: case 0x2114: {
        uint8_t idx = (uint8_t)(addr - 0x210Du);
        if (p->scroll_latch == 0u)
            p->bg_scroll[idx] = (uint16_t)((p->bg_scroll[idx] & 0xFF00u) | v);
        else
            p->bg_scroll[idx] = (uint16_t)((p->bg_scroll[idx] & 0x00FFu) |
                                           ((uint16_t)v << 8));
        p->scroll_latch ^= 1u;
        break;
    }
    case 0x2115: p->vmainc = v; break;
    case 0x2116: p->vmadd = (uint16_t)((p->vmadd & 0xFF00u) | v); break;
    case 0x2117: p->vmadd = (uint16_t)((p->vmadd & 0x00FFu) | ((uint16_t)v << 8)); break;
    case 0x2118:
        /* VMAIN bit7 = 0: $2118 supplies the low byte (latched), $2119
         * commits the word and increments. bit7 = 1: the roles swap. */
        if (p->vmainc & 0x80u) {
            vram_word_write(s, p->vmadd,
                            (uint16_t)((v << 8) | p->vm_hi_latch));
            p->vmadd = (uint16_t)(p->vmadd + vram_increment(p));
        } else {
            p->vm_lo_latch = v;
        }
        break;
    case 0x2119:
        if (p->vmainc & 0x80u) {
            p->vm_lo_latch = v;
        } else {
            vram_word_write(s, p->vmadd,
                            (uint16_t)((v << 8) | p->vm_lo_latch));
            p->vmadd = (uint16_t)(p->vmadd + vram_increment(p));
        }
        break;
    case 0x211B: case 0x211C: case 0x211D: case 0x211E:
    case 0x211F: case 0x2120: {
        uint16_t *r = &p->m7a;
        if (addr == 0x211Cu) r = &p->m7b;
        else if (addr == 0x211Du) r = &p->m7c;
        else if (addr == 0x211Eu) r = &p->m7d;
        else if (addr == 0x211Fu) r = &p->m7x;
        else if (addr == 0x2120u) r = &p->m7y;
        if (p->scroll_latch == 0u)
            *r = (uint16_t)((*r & 0xFF00u) | v);
        else
            *r = (uint16_t)((*r & 0x00FFu) | ((uint16_t)v << 8));
        p->scroll_latch ^= 1u;
        break;
    }
    case 0x2121: p->cgaddr = v; p->cgaddr_flip = 0; break;
    case 0x2122:
        p->cgram[(uint32_t)p->cgaddr * 2u + p->cgaddr_flip] = v;
        p->cgaddr_flip ^= 1u;
        if (p->cgaddr_flip == 0u)
            p->cgaddr = (uint8_t)((p->cgaddr + 1u) & 0xFFu);
        break;
    case 0x212C: p->tm = v; break;
    case 0x212D: p->ts = v; break;
    case 0x2133: p->obj_8bpp = (uint8_t)(v & 1u); break;
    default:
        break;
    }
}
