/*
 * mgbax PPU: BG modes 0 (text), 1 (text + affine), 2 (affine), 3 (bitmap),
 * 4 (page-flipped 8bpp bitmap). Sprites: 4bpp/8bpp, all sizes, 1D/2D
 * mapping, flips, priority (OBJ-over-BG simplified to OBJ-first: documented).
 * Not implemented (documented): affine sprites, windows, alpha blending,
 * brightness, mosaic, mode 5.
 */
#include "gba.h"

#include <string.h>

static inline uint32_t bgr555_to_rgb32(uint16_t v)
{
    uint32_t r = (v >> 0) & 0x1Fu;
    uint32_t g = (v >> 5) & 0x1Fu;
    uint32_t b = (v >> 10) & 0x1Fu;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

void gba_ppu_init(gba_ppu *p, uint32_t *fb)
{
    memset(p, 0, sizeof *p);
    p->fb = fb;
    gba_ppu_reset(p);
}

void gba_ppu_reset(gba_ppu *p)
{
    uint32_t *fb = p->fb;
    memset(p, 0, sizeof *p);
    p->fb = fb;
    for (size_t i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; i++)
        fb[i] = 0xFF000000u;
}

static uint16_t pal_read(gba_t *g, uint16_t idx)
{
    return (uint16_t)(g->mem.pal[(uint32_t)idx * 2u] |
                      ((uint16_t)g->mem.pal[(uint32_t)idx * 2u + 1u] << 8));
}

static uint16_t io16(gba_t *g, uint32_t reg)
{
    return (uint16_t)(g->mem.io[reg & 0x3FFu] |
                      ((uint16_t)g->mem.io[(reg & 0x3FFu) + 1u] << 8));
}

/* ---- text background (modes 0/1) ---- */

static uint16_t text_bg_pixel(gba_t *g, uint8_t bg, uint16_t sx, uint16_t sy)
{
    uint16_t cnt = g->ppu.bgcnt[bg];
    uint32_t char_base = (uint32_t)(cnt & 3u) << 14;
    uint32_t map_base = (uint32_t)((cnt >> 8) & 0x1Fu) << 11;
    uint8_t bpp = (cnt & (1u << 7u)) ? 8u : 4u;
    uint16_t size = (uint16_t)((cnt >> 14) & 3u);
    uint16_t map_w = (uint16_t)(32u << (size & 1u));
    uint16_t map_h = (uint16_t)(32u << ((size >> 1u) & 1u));

    uint16_t hofs = g->ppu.bg_hofs[bg] & 0x1FFu;
    uint16_t vofs = g->ppu.bg_vofs[bg] & 0x1FFu;
    uint16_t fx = (uint16_t)(sx + hofs);
    uint16_t fy = (uint16_t)(sy + vofs);
    uint16_t tx = (uint16_t)((fx >> 3) % map_w);
    uint16_t ty = (uint16_t)((fy >> 3) % map_h);
    uint8_t fine_x = (uint8_t)(fx & 7u);
    uint8_t fine_y = (uint8_t)(fy & 7u);

    uint32_t map_addr = map_base + (uint32_t)ty * (uint32_t)map_w * 2u +
                        (uint32_t)tx * 2u;
    uint16_t entry = (uint16_t)(g->mem.vram[map_addr & 0xFFFFu] |
                                ((uint16_t)g->mem.vram[(map_addr + 1u) & 0xFFFFu] << 8));
    uint16_t tile = entry & 0x3FFu;
    uint8_t pal = (uint8_t)((entry >> 12) & 0xFu);
    uint8_t hflip = (uint8_t)((entry >> 10) & 1u);
    uint8_t vflip = (uint8_t)((entry >> 11) & 1u);
    uint8_t fxx = hflip ? (uint8_t)(7u - fine_x) : fine_x;
    uint8_t fyy = vflip ? (uint8_t)(7u - fine_y) : fine_y;

    uint32_t tile_off = char_base + (uint32_t)tile * (bpp == 8u ? 64u : 32u) +
                        (uint32_t)fyy * (bpp == 8u ? 8u : 4u);
    uint8_t color;
    if (bpp == 4u) {
        uint8_t b = g->mem.vram[(tile_off + (uint32_t)(fxx >> 1)) & 0xFFFFu];
        color = (fxx & 1u) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0xFu);
    } else {
        color = g->mem.vram[(tile_off + fxx) & 0xFFFFu];
    }
    if (color == 0u)
        return 0xFFFFu; /* transparent */
    uint16_t pal_idx = (bpp == 4u)
                           ? (uint16_t)((pal == 0u ? 0u : 32u + (uint32_t)pal * 16u) + color)
                           : color;
    return pal_idx;
}

/* ---- affine background (modes 1/2) ---- */

static uint16_t affine_bg_pixel(gba_t *g, uint8_t aff, uint16_t sx, uint16_t sy)
{
    uint8_t bg = (uint8_t)(aff + 1u); /* affine BGs are BG2/BG3 */
    uint16_t cnt = g->ppu.bgcnt[bg];
    uint32_t map_base = (uint32_t)((cnt >> 8) & 0x1Fu) << 11;
    uint32_t char_base = (uint32_t)(cnt & 3u) << 14;
    uint16_t size = (uint16_t)((cnt >> 14) & 3u);
    uint16_t map_wh = (uint16_t)(16u << size);

    /* Per-spec affine transform, .8 fixed point with the +0.5 sample offset:
     * tex = PA*(sx-120) + PB*(sy-80) + refX  (in 1/256 pixel units + ref)
     * The reference registers are 20.8 signed fixed point (28 bits used). */
    int64_t x = (int64_t)g->ppu.bgpa * (int32_t)(sx - 120) +
                (int64_t)g->ppu.bgpb * (int32_t)(sy - 80) +
                (int64_t)g->ppu.bgx[aff] + 128;
    int64_t y = (int64_t)g->ppu.bgpc * (int32_t)(sx - 120) +
                (int64_t)g->ppu.bgpd * (int32_t)(sy - 80) +
                (int64_t)g->ppu.bgy[aff] + 128;
    int32_t tex_x = (int32_t)(x >> 8);
    int32_t tex_y = (int32_t)(y >> 8);
    int32_t map_lim = (int32_t)map_wh * 8;
    if (tex_x < 0 || tex_x >= map_lim || tex_y < 0 ||
        tex_y >= map_lim)
        return 0xFFFFu; /* outside: transparent (wrap unimplemented) */
    uint32_t tile_x = (uint32_t)tex_x >> 3;
    uint32_t tile_y = (uint32_t)tex_y >> 3;
    uint8_t fine_x = (uint8_t)(tex_x & 7u);
    uint8_t fine_y = (uint8_t)(tex_y & 7u);
    uint32_t map_addr = map_base + tile_y * (uint32_t)map_wh + tile_x;
    uint8_t tile = g->mem.vram[map_addr & 0xFFFFu];
    return g->mem.vram[(char_base + (uint32_t)tile * 64u +
                        (uint32_t)fine_y * 8u + fine_x) & 0xFFFFu];
}

/* ---- sprites ---- */

static const uint8_t spr_h[3][4] = { { 8, 16, 32, 64 },
                                     { 8, 8, 16, 32 },
                                     { 16, 32, 32, 64 } };
static const uint8_t spr_w[3][4] = { { 8, 16, 32, 64 },
                                     { 16, 32, 32, 64 },
                                     { 8, 8, 16, 32 } };

/* returns palette index or 0xFFFF */
static uint16_t sprite_pixel(gba_t *g, uint16_t sx, uint16_t sy)
{
    uint16_t dispcnt = io16(g, 0x0000u);
    if (!(dispcnt & (1u << 12u)))
        return 0xFFFFu;
    uint8_t one_dim = (uint8_t)((dispcnt >> 6) & 1u);

    for (int i = 0; i < 128; i++) {
        const uint8_t *e = &g->mem.oam[(uint32_t)i * 8u];
        uint16_t y = (uint16_t)(e[0] | ((uint16_t)e[1] << 8));
        uint8_t shape = (uint8_t)((e[1] >> 6) & 3u);
        if (shape == 3u)
            continue; /* affine sprites: not implemented (documented) */
        uint8_t size = (uint8_t)((e[3] >> 6) & 3u);
        uint8_t h = spr_h[shape][size];
        uint8_t w = spr_w[shape][size];
        uint16_t dy = (uint16_t)((sy - y) & 0xFFu);
        if (dy >= h)
            continue;
        uint16_t x = (uint16_t)(e[2] | ((uint16_t)(e[3] & 1u) << 8));
        uint16_t dx = (uint16_t)((sx - x) & 0x1FFu);
        if (dx >= w)
            continue;
        uint8_t prio = (uint8_t)((e[3] >> 2) & 3u);
        (void)prio;
        uint8_t hflip = (uint8_t)((e[3] >> 4) & 1u);
        uint8_t vflip = (uint8_t)((e[3] >> 5) & 1u);
        uint8_t color_mode = (uint8_t)((e[5] & 0x20u) ? 8u : 4u);
        uint8_t pal_bank = (uint8_t)(e[5] & 0xFu);
        uint16_t tile = (uint16_t)(e[4] | ((uint16_t)(e[5] & 1u) << 8));
        uint8_t px = hflip ? (uint8_t)(w - 1u - dx) : (uint8_t)dx;
        uint8_t py = vflip ? (uint8_t)(h - 1u - dy) : (uint8_t)dy;
        uint32_t tile_row = (uint32_t)(py / 8u);
        uint32_t tile_col;
        if (one_dim)
            tile_col = (uint32_t)(px / 8u) + tile_row * (uint32_t)(w / 8u);
        else
            tile_col = (uint32_t)(px / 8u) + tile_row * 32u;
        uint32_t t = (uint32_t)tile + tile_col;
        uint8_t fxx = (uint8_t)(px % 8u);
        uint8_t fyy = (uint8_t)(py % 8u);
        uint32_t tile_off = 0x10000u + t * (color_mode == 8u ? 64u : 32u) +
                            (uint32_t)fyy * (color_mode == 8u ? 8u : 4u);
        /* OBJ tiles live at VRAM 0x10000-0x17FFF; 0x18000+ mirrors down */
        uint32_t voff = tile_off & 0x1FFFFu;
        if (voff >= 0x18000u)
            voff -= 0x8000u;
        uint8_t color;
        if (color_mode == 4u) {
            uint8_t b = g->mem.vram[(voff + (uint32_t)(fxx >> 1)) & 0x1FFFFu];
            color = (fxx & 1u) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0xFu);
        } else {
            color = g->mem.vram[(voff + fxx) & 0x1FFFFu];
        }
        if (color == 0u)
            continue;
        return (color_mode == 8u)
                   ? (uint16_t)(256u + color)
                   : (uint16_t)(256u + (uint32_t)pal_bank * 16u + color);
    }
    return 0xFFFFu;
}

/* ---- line render ---- */

void gba_ppu_render_line(gba_t *g, uint16_t line)
{
    gba_ppu *p = &g->ppu;
    uint16_t dispcnt = io16(g, 0x0000u);
    uint8_t mode = (uint8_t)(dispcnt & 7u);
    uint8_t bg_en = (uint8_t)(dispcnt >> 8);
    uint32_t *out = &p->fb[(size_t)line * GBA_SCREEN_W];
    uint16_t backdrop = pal_read(g, 0);

    for (uint16_t x = 0; x < GBA_SCREEN_W; x++) {
        uint16_t pal_idx = 0;
        uint8_t found = 0;
        if (!(dispcnt & (1u << 7u))) {
            if (mode == 3u) {
                uint32_t off = ((uint32_t)line * 240u + x) * 2u;
                uint16_t v = (uint16_t)(g->mem.vram[off] |
                                        ((uint16_t)g->mem.vram[off + 1u] << 8));
                out[x] = bgr555_to_rgb32(v);
                continue;
            }
            if (mode == 4u) {
                uint32_t page = (dispcnt & (1u << 4u)) ? 0xA000u : 0u;
                uint8_t c = g->mem.vram[page + (uint32_t)line * 240u + x];
                out[x] = (c == 0u) ? bgr555_to_rgb32(backdrop)
                                   : bgr555_to_rgb32(pal_read(g, c));
                continue;
            }
            uint8_t n_bg = (mode == 0u) ? 4u : (mode == 1u ? 3u : 2u);
            uint8_t aff_start = (mode == 1u) ? 2u : (mode == 2u ? 0u : 4u);
            for (int bgn = n_bg - 1; bgn >= 0; bgn--) {
                if (!(bg_en & (1u << bgn)))
                    continue;
                uint16_t c;
                if ((mode == 1u || mode == 2u) && bgn >= aff_start)
                    c = affine_bg_pixel(g, (uint8_t)(bgn - 2u), x, line);
                else
                    c = text_bg_pixel(g, (uint8_t)bgn, x, line);
                if (c != 0xFFFFu) {
                    pal_idx = c;
                    found = 1;
                    break;
                }
            }
            if (mode <= 2u) {
                uint16_t sc = sprite_pixel(g, x, line);
                if (sc != 0xFFFFu) {
                    pal_idx = sc;
                    found = 1;
                }
            }
        }
        out[x] = bgr555_to_rgb32(found ? pal_read(g, pal_idx) : backdrop);
    }
}

/* ---- IO ---- */

uint16_t gba_ppu_io_read16(gba_t *g, uint32_t addr)
{
    uint32_t off = addr & 0x3Fu;
    switch (off) {
    case 0x04: /* DISPSTAT */
        return (uint16_t)(g->ppu.dispstat | ((g->ppu.vcount == 160u) ? 1u : 0u));
    case 0x06: /* VCOUNT */
        return g->ppu.vcount;
    default:
        return io16(g, addr & 0x3FFu);
    }
}

void gba_ppu_io_write16(gba_t *g, uint32_t addr, uint16_t v)
{
    uint32_t off = addr & 0x3Fu;
    g->mem.io[off] = (uint8_t)(v & 0xFFu);
    g->mem.io[off + 1u] = (uint8_t)(v >> 8);
    switch (off) {
    case 0x08: case 0x0A: case 0x0C: case 0x0E:
        g->ppu.bgcnt[(off - 0x08u) / 2u] = v;
        break;
    case 0x10: case 0x12: case 0x14: case 0x16:
        g->ppu.bg_hofs[(off - 0x10u) / 2u] = v;
        break;
    case 0x18: case 0x1A: case 0x1C: case 0x1E:
        g->ppu.bg_vofs[(off - 0x18u) / 2u] = v;
        break;
    case 0x20: g->ppu.bgpa = (int16_t)v; break;
    case 0x22: g->ppu.bgpb = (int16_t)v; break;
    case 0x24: g->ppu.bgpc = (int16_t)v; break;
    case 0x26: g->ppu.bgpd = (int16_t)v; break;
    case 0x28: case 0x2A: {
        uint8_t which = (uint8_t)((off - 0x28u) / 4u);
        int shift = (off & 2u) ? 16 : 0;
        int32_t mask = (int32_t)(0x000FFFFFu << shift);
        g->ppu.bgx[which] = (g->ppu.bgx[which] & ~mask) |
                            ((int32_t)((uint32_t)v << shift));
        break;
    }
    case 0x2C: case 0x2E: {
        uint8_t which = (uint8_t)((off - 0x2Cu) / 4u);
        int shift = (off & 2u) ? 16 : 0;
        int32_t mask = (int32_t)(0x000FFFFF << shift);
        g->ppu.bgy[which] = (g->ppu.bgy[which] & ~mask) |
                            ((int32_t)((uint32_t)v << shift));
        break;
    }
    default:
        break;
    }
}
