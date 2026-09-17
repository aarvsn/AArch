/*
 * finalburn: Sega VDP 315-5313 (mode-4 Genesis graphics).
 *
 * Implemented: VRAM/CRAM/VSRAM access with auto-increment, planes A/B and
 * window with priority, sprites (H32/H40 limits, link walk, flips),
 * whole-plane and per-line H-scroll, whole-plane and per-column V-scroll,
 * backdrop, DMA (68K->VDP, VRAM fill, VRAM copy) with approximate timing,
 * V-int/H-int, status reads, HV counters, display blanking.
 *
 * Approximations (documented in README): FIFO is modeled as always empty,
 * H-scroll cell mode falls back to per-line, shadow/highlight and interlace
 * are not implemented, sprite masking uses first-come priority, sprite
 * size field uses the top-nibble convention (width-1 in bits 7-6,
 * height-1 in bits 5-4).
 */
#include "fb_md.h"

#include <string.h>

#define FB_CRAM_WORDS  64
#define FB_VSRAM_WORDS 40

/*
 * Command word decode:
 *   (w1 & 0xC000) == 0x8000 -> register write (single word, reg = w1[12:8])
 *   otherwise a two-word data command:
 *     w1 bit14: 1 = write, 0 = read
 *     w1 bit15 or w2 bit5 (0x20): CRAM space
 *     w2 bit4 (0x10): VSRAM space
 *     w2 bit7 (0x80): start DMA; w2 bits 6-5 via reg 23 give the DMA type
 * Classic constants: VRAM R 0x0000 / W 0x4000, CRAM W 0xC000,
 * VSRAM R/W = 0x0000/0x4000 with w2 0x10, CRAM R = 0x0000 with w2 0x20.
 */
#define CMD_RW    0x01u /* 1 = write, 0 = read */
#define CMD_CRAM  0x02u
#define CMD_VSRAM 0x04u

void fb_vdp_init(struct fb_vdp *v, uint32_t *fb)
{
    memset(v, 0, sizeof *v);
    v->fb = fb;
    fb_vdp_reset(v);
}

void fb_vdp_reset(struct fb_vdp *v)
{
    uint32_t *fb = v->fb;
    memset(v, 0, sizeof *v);
    v->fb = fb;
    v->regs[15] = 1; /* auto-increment 1 */
    v->hint_counter = 0xFF;
}

/* ---- palette -------------------------------------------------------------- */

/* CRAM word layout: 0000BBB0GGG0RRR (R bits 0-2, G bits 4-6, B bits 8-10) */
static uint32_t cram_color(const struct fb_vdp *v, uint16_t idx)
{
    uint16_t w = (uint16_t)(v->cram[(idx & 0x3Fu) * 2] |
                            (v->cram[(idx & 0x3Fu) * 2 + 1] << 8));
    uint32_t r = (w & 7u) * 255u / 7u;
    uint32_t g = ((w >> 4) & 7u) * 255u / 7u;
    uint32_t b = ((w >> 8) & 7u) * 255u / 7u;
    return EMU_PIXEL(r, g, b);
}

/* ---- tile/plane helpers ---------------------------------------------------- */

/* fetch one 4bpp tile pixel (big-endian bit order: MSB = leftmost) */
static uint8_t tile_pixel(const struct fb_vdp *v, uint32_t tilebase,
                          uint16_t tile, uint8_t px, uint8_t py)
{
    uint32_t off = (tilebase + tile * 32u + py * 4u) & 0xFFFFu;
    uint8_t p0 = (uint8_t)((v->vram[off] >> (7 - px)) & 1u);
    uint8_t p1 = (uint8_t)((v->vram[off + 1] >> (7 - px)) & 1u);
    uint8_t p2 = (uint8_t)((v->vram[off + 2] >> (7 - px)) & 1u);
    uint8_t p3 = (uint8_t)((v->vram[off + 3] >> (7 - px)) & 1u);
    return (uint8_t)(p0 | (p1 << 1) | (p2 << 2) | (p3 << 3));
}

struct pix {
    uint8_t color; /* 0-15 with palette in high nibble; 0 = transparent */
    uint8_t prio;
};

/* decode a name-table entry into color/prio */
static struct pix entry_pixel(const struct fb_vdp *v, uint32_t namebase,
                              uint32_t tilebase, uint16_t col, uint16_t row,
                              uint16_t wcells, uint16_t hcells, int x,
                              int y_of_cell)
{
    struct pix p = { 0, 0 };
    uint32_t idx = ((uint32_t)row * wcells + col) &
                   ((uint32_t)wcells * hcells - 1u);
    uint32_t off = (namebase + idx * 2u) & 0xFFFEu;
    uint16_t entry = (uint16_t)(v->vram[off] | (v->vram[off + 1] << 8));
    uint16_t tile = (uint16_t)(entry & 0x7FFu);
    uint8_t px = (uint8_t)(x & 7u);
    uint8_t py = (uint8_t)(y_of_cell & 7u);
    if (entry & 0x0800u)
        px = (uint8_t)(7 - px);
    if (entry & 0x1000u)
        py = (uint8_t)(7 - py);
    p.color = tile_pixel(v, tilebase, tile, px, py);
    p.prio = (uint8_t)((entry >> 15) & 1u);
    if (p.color)
        p.color = (uint8_t)(p.color | (((entry >> 13) & 3u) << 4));
    return p;
}

static struct pix plane_pixel(const struct fb_vdp *v, int plane, int x,
                              int line)
{
    uint16_t namebase, tilebase;
    uint16_t wcells, hcells;
    uint16_t hscroll, vscroll;
    uint8_t size_reg = (uint8_t)v->regs[16];
    uint16_t col, row;

    wcells = (uint16_t)(32u << ((size_reg >> 0) & 3u));
    hcells = (uint16_t)(32u << ((size_reg >> 4) & 3u));
    if (plane == 0) { /* plane A */
        namebase = (uint16_t)((v->regs[2] & 0x7u) << 10);
        tilebase = (uint16_t)((v->regs[2] & 0x7u) << 13);
        hscroll = v->hscroll_a;
    } else { /* plane B */
        namebase = (uint16_t)((v->regs[4] & 0x7u) << 10);
        tilebase = (uint16_t)((v->regs[4] & 0x7u) << 13);
        hscroll = v->hscroll_b;
    }
    hscroll &= 0x3FFu;

    /* vscroll: whole-plane (vsram[0]) or per 2-cell column */
    if (v->regs[11] & 0x04u) {
        int vcol = x >> 4;
        uint16_t idx = (uint16_t)((vcol < FB_VSRAM_WORDS) ? vcol
                                                          : FB_VSRAM_WORDS - 1);
        vscroll = (uint16_t)(v->vsram[idx * 2] | (v->vsram[idx * 2 + 1] << 8));
    } else {
        vscroll = (uint16_t)(v->vsram[0] | (v->vsram[1] << 8));
    }
    vscroll &= 0x3FFu;

    col = (uint16_t)(((x + hscroll) >> 3) & (wcells - 1u));
    row = (uint16_t)(((line + vscroll) >> 3) & (hcells - 1u));
    return entry_pixel(v, namebase, tilebase, col, row, wcells, hcells,
                       x + hscroll, line + vscroll);
}

/* window coverage */
static int window_covers_line(const struct fb_vdp *v, int line)
{
    uint8_t wv = (uint8_t)v->regs[18];
    if ((wv & 0x80u) == 0)
        return 1; /* full height */
    if (wv & 0x40u)
        return line < (int)(wv & 0x1Fu) * 8; /* top portion */
    return line >= (int)(wv & 0x1Fu) * 8;    /* bottom portion */
}

static int window_covers_x(const struct fb_vdp *v, int x)
{
    uint8_t wh = (uint8_t)v->regs[17];
    if ((wh & 0x80u) == 0)
        return 1; /* full width */
    if (wh & 0x40u)
        return x < (int)(wh & 0x1Fu) * 8; /* left portion */
    return x >= (int)(wh & 0x1Fu) * 8;    /* right portion */
}

static struct pix window_pixel(const struct fb_vdp *v, int x, int line)
{
    uint8_t size_reg = (uint8_t)v->regs[16];
    uint16_t wcells = (uint16_t)(32u << ((size_reg >> 0) & 3u));
    uint16_t hcells = (uint16_t)(32u << ((size_reg >> 4) & 3u));
    uint16_t namebase = (uint16_t)((v->regs[3] & 0x3Fu) << 10);
    uint16_t tilebase = (uint16_t)((v->regs[3] & 0x7u) << 13);
    uint16_t col = (uint16_t)((x >> 3) & (wcells - 1u));
    uint16_t row = (uint16_t)((line >> 3) & (hcells - 1u));
    return entry_pixel(v, namebase, tilebase, col, row, wcells, hcells, x, line);
}

/* ---- sprites --------------------------------------------------------------- */

static void render_sprites(struct fb_vdp *v, int line, int width,
                           uint8_t *spr_color, uint8_t *spr_prio)
{
    int limit = (width == 320) ? 80 : 64;
    int per_line = (width == 320) ? 20 : 16;
    uint32_t table = (uint32_t)(v->regs[5] & 0x7Fu) << 9;
    uint16_t entry_off = 0;
    int used = 0;
    int total_px = 0;

    memset(spr_color, 0, (size_t)width);
    memset(spr_prio, 0, (size_t)width);

    for (int i = 0; i < limit; i++) {
        uint32_t off = (table + entry_off * 8u) & 0xFFFEu;
        int sy = (int)v->vram[off] - 128;
        uint8_t size = v->vram[off + 1];
        uint8_t link = (uint8_t)(v->vram[off + 2] & 0x7Fu);
        uint8_t f3 = v->vram[off + 3];
        uint16_t xval = (uint16_t)((v->vram[off + 4] << 8) | v->vram[off + 5]);
        uint16_t tile = (uint16_t)(((uint16_t)(f3 & 7u) << 8) | v->vram[off + 6]);
        uint8_t pal = (uint8_t)((f3 >> 4) & 3u);
        uint8_t prio = (uint8_t)((f3 >> 7) & 1u);
        uint8_t vf = (uint8_t)((f3 >> 6) & 1u);
        uint8_t hf = (uint8_t)((f3 >> 3) & 1u);
        int w = (int)(((size >> 6) & 3u) * 8u + 8u);
        int h = (int)(((size >> 4) & 3u) * 8u + 8u);
        uint32_t tilebase = (uint32_t)(v->regs[6] & 0x7u) << 13;

        int rel = line - sy;
        if (rel >= 0 && rel < h && used < per_line) {
            used++;
            int spr_row = rel / 8;
            int row_in = rel % 8;
            if (vf)
                spr_row = (h / 8 - 1) - spr_row;
            for (int cx = 0; cx < w / 8; cx++) {
                for (int px = 0; px < 8; px++) {
                    int col_in = cx * 8 + px;
                    int xx = (int)xval + col_in;
                    if (xx < 0 || xx >= width)
                        continue;
                    if (total_px >= width) {
                        v->sprite_overflow = 1;
                        break;
                    }
                    uint8_t lx = (uint8_t)px;
                    if (hf)
                        lx = (uint8_t)(7 - px);
                    int cxx = (int)(cx & 3u); /* column within sprite */
                    if (hf)
                        cxx = (int)(((w / 8) - 1) & 3u) - cxx;
                    uint16_t t = (uint16_t)(tile + (uint16_t)spr_row * (w / 8) +
                                            (uint16_t)cxx);
                    uint8_t color = tile_pixel(v, tilebase, t, lx,
                                               (uint8_t)row_in);
                    if (color) {
                        if (spr_color[xx]) {
                            v->sprite_collision = 1;
                            continue; /* first-come keeps the pixel */
                        }
                        spr_color[xx] = (uint8_t)(color | (pal << 4));
                        spr_prio[xx] = prio;
                        total_px++;
                    }
                }
            }
        }
        if (link == 0)
            break;
        entry_off = link;
    }
}

/* ---- per-line rendering ---------------------------------------------------- */

static void render_line(struct fb_vdp *v, int line, int width)
{
    uint32_t *fbrow = v->fb + (uint32_t)line * FB_SCREEN_W;
    uint8_t spr_color[FB_SCREEN_W];
    uint8_t spr_prio[FB_SCREEN_W];
    uint16_t backdrop = (uint16_t)(v->regs[7] & 0x3Fu);
    uint8_t display_en = (uint8_t)((v->regs[1] & 0x40u) ? 1 : 0);

    if (!display_en) {
        uint32_t bg = cram_color(v, backdrop);
        for (int x = 0; x < width; x++)
            fbrow[x] = bg;
        return;
    }

    /* latch hscroll from the table (whole-plane and per-line modes) */
    render_sprites(v, line, width, spr_color, spr_prio);

    for (int x = 0; x < width; x++) {
        struct pix a, b, s, top;
        int use_window = window_covers_line(v, line) && window_covers_x(v, x);

        if (use_window) {
            a = window_pixel(v, x, line);
        } else {
            a = plane_pixel(v, 0, x, line);
        }
        b = plane_pixel(v, 1, x, line);

        /* choose the plane top layer */
        if (a.color == 0) {
            top = b;
        } else if (b.color == 0) {
            top = a;
        } else {
            top = a.prio ? a : b;
        }
        s.color = spr_color[x];
        s.prio = spr_prio[x];

        uint8_t color, pal;
        if (s.color != 0) {
            if (!top.prio || s.prio) {
                color = (uint8_t)(s.color & 0x0Fu);
                pal = (uint8_t)(s.color >> 4);
            } else {
                color = (uint8_t)(top.color & 0x0Fu);
                pal = (uint8_t)(top.color >> 4);
            }
        } else if (top.color != 0) {
            color = (uint8_t)(top.color & 0x0Fu);
            pal = (uint8_t)(top.color >> 4);
        } else {
            color = (uint8_t)(backdrop & 0x0Fu);
            pal = (uint8_t)(backdrop >> 4);
        }
        fbrow[x] = cram_color(v, (uint16_t)(pal * 16u + color));
    }
}

static void latch_hscroll(struct fb_vdp *v)
{
    uint32_t base = (uint32_t)(v->regs[13] & 0x7Fu) << 10;
    uint32_t off = (base + (uint32_t)v->line * 16u) & 0xFFFEu;
    uint16_t a = (uint16_t)(v->vram[off] | (v->vram[off + 1] << 8));
    uint16_t b = (uint16_t)(v->vram[off + 2] | (v->vram[off + 3] << 8));
    v->hscroll_a = a;
    v->hscroll_b = b;
}

/* ---- register/data/status ports --------------------------------------------- */

static void vdp_write_word(struct fb_md *md, uint16_t val)
{
    struct fb_vdp *v = &md->vdp;
    uint8_t space = (uint8_t)((v->mode & CMD_CRAM ? 2u : 0u) |
                              (v->mode & CMD_VSRAM ? 1u : 0u));
    uint32_t addr = v->addr;

    if (space == 0) { /* VRAM: byte-granular */
        v->vram[addr & 0xFFFFu] = (uint8_t)(val & 0xFFu);
        v->vram[(addr + 1) & 0xFFFFu] = (uint8_t)(val >> 8);
    } else if (space == 2) { /* CRAM (9-bit color: 0BBB0GGG0RRR) */
        uint32_t off = (addr & 0x7Eu);
        v->cram[off] = (uint8_t)(val & 0x77u);
        v->cram[off + 1] = (uint8_t)((val >> 8) & 0x07u);
    } else { /* VSRAM */
        uint32_t off = addr & 0x7Eu;
        if (off < FB_VSRAM_WORDS * 2u) {
            v->vsram[off] = (uint8_t)(val & 0xFFu);
            v->vsram[off + 1] = (uint8_t)(val >> 8);
        }
    }
    v->addr = (addr + v->regs[15]) & 0xFFFFu;
    (void)md;
}

static uint16_t vdp_read_word(struct fb_md *md)
{
    struct fb_vdp *v = &md->vdp;
    uint8_t space = (uint8_t)((v->mode & CMD_CRAM ? 2u : 0u) |
                              (v->mode & CMD_VSRAM ? 1u : 0u));
    uint32_t addr = v->addr;
    uint16_t val = v->read_buf;

    if (space == 0) {
        v->read_buf = (uint16_t)(v->vram[addr & 0xFFFFu] |
                                 (v->vram[(addr + 1) & 0xFFFFu] << 8));
    } else if (space == 2) {
        uint32_t off = addr & 0x7Eu;
        v->read_buf = (uint16_t)(v->cram[off] | (v->cram[off + 1] << 8));
    } else {
        uint32_t off = addr & 0x7Eu;
        v->read_buf = (uint16_t)(v->vsram[off] | (v->vsram[off + 1] << 8));
    }
    v->addr = (addr + v->regs[15]) & 0xFFFFu;
    return val;
}

static void vdp_start_dma(struct fb_md *md)
{
    struct fb_vdp *v = &md->vdp;
    uint16_t len = (uint16_t)(v->regs[19] | (v->regs[20] << 8));
    uint32_t src = (uint32_t)(v->regs[21] | (v->regs[22] << 8)) |
                   ((uint32_t)(v->regs[23] & 0x01u) << 16);
    uint8_t type = (uint8_t)((v->regs[23] >> 6) & 3u);

    v->dma_src = src;
    v->dma_left = len ? (uint32_t)len : 0x10000u;
    if (type == 1) /* fill: source low byte = fill value */
        v->dma_fill_val = (uint8_t)(v->regs[21] & 0xFFu);
    v->dma_type = type;
    v->dma_active = 1;
}

void fb_vdp_run_dma(struct fb_md *md, int32_t cycles)
{
    struct fb_vdp *v = &md->vdp;
    if (!v->dma_active)
        return;
    int display_active = (v->line < FB_VBLANK_LINE) && (v->regs[1] & 0x40u);
    int32_t rate = display_active ? 16 : 8; /* 68K cycles per word (approx) */

    while (v->dma_active && cycles >= rate) {
        cycles -= rate;
        uint8_t space = (uint8_t)((v->mode & CMD_CRAM ? 2u : 0u) |
                                  (v->mode & CMD_VSRAM ? 1u : 0u));
        if (v->dma_type == 0) { /* 68K -> VDP */
            uint16_t val = (uint16_t)(fb_md_68k_read16(md, v->dma_src));
            v->dma_src = (v->dma_src + 2u) & 0xFFFFFFu;
            vdp_write_word(md, val);
        } else if (v->dma_type == 1) { /* VRAM fill (byte writes) */
            (void)space;
            v->vram[v->addr & 0xFFFFu] = v->dma_fill_val;
            v->addr = (v->addr + v->regs[15]) & 0xFFFFu;
        } else { /* VRAM copy */
            uint8_t sb = v->vram[v->dma_src & 0xFFFFu];
            v->vram[v->addr & 0xFFFFu] = sb;
            v->dma_src = (v->dma_src + 1u) & 0xFFFFu;
            v->addr = (v->addr + v->regs[15]) & 0xFFFFu;
        }
        if (--v->dma_left == 0)
            v->dma_active = 0;
    }
}

/* 68K stalls on VDP access while DMA is active (simplified: drain 8 cycles) */
static void vdp_dma_penalty(struct fb_md *md)
{
    if (md->vdp.dma_active)
        fb_vdp_run_dma(md, 8);
}

uint16_t fb_vdp_read16(struct fb_md *md, uint32_t addr)
{
    struct fb_vdp *v = &md->vdp;
    vdp_dma_penalty(md);
    switch ((addr >> 1) & 7u) {
    case 0: /* data port */
        return vdp_read_word(md);
    case 2: { /* control port: status read */
        uint16_t st = (uint16_t)(0x0200u | (v->vint_68k ? 0x80u : 0u) |
                                 (v->sprite_overflow ? 0x40u : 0u) |
                                 (v->sprite_collision ? 0x20u : 0u));
        v->vint_68k = 0;
        v->hint_68k = 0;
        v->vint_z80 = 0;
        v->cmd_pending = 0;
        fb_m68k_set_irq(md, md->vdp.irq_line_ym ? 2 : 0);
        return st;
    }
    case 4: { /* HV counter: H in low, V in high (word read) */
        uint16_t h = (uint16_t)((v->line_cycle >> 1) & 0xFFu);
        uint16_t vv = (uint16_t)(v->line & 0xFFu);
        return (uint16_t)((vv << 8) | h);
    }
    default:
        return 0;
    }
}

uint8_t fb_vdp_read8(struct fb_md *md, uint32_t addr)
{
    uint16_t w = fb_vdp_read16(md, addr & ~1u);
    return (uint8_t)((addr & 1u) ? w : (w >> 8));
}

void fb_vdp_write16(struct fb_md *md, uint32_t addr, uint16_t val)
{
    struct fb_vdp *v = &md->vdp;
    vdp_dma_penalty(md);
    switch ((addr >> 1) & 7u) {
    case 0: /* data port */
        if (v->mode & CMD_RW)
            vdp_write_word(md, val);
        break;
    case 2: { /* control port */
        if ((val & 0xC000u) == 0x8000u) {
            /* register write: single word, cancels any pending command */
            uint8_t reg = (uint8_t)((val >> 8) & 0x1Fu);
            v->cmd_pending = 0;
            if (reg < 24)
                v->regs[reg] = val & 0xFFu;
            break;
        }
        if (!v->cmd_pending) {
            v->cmd_pending = 1;
            v->cmd_w1 = val;
        } else {
            v->cmd_pending = 0;
            uint8_t rw = (uint8_t)((v->cmd_w1 >> 14) & 1u);   /* 1 = write */
            uint8_t cram = (uint8_t)(((v->cmd_w1 >> 15) & 1u) |
                                     ((val >> 5) & 1u));
            uint8_t vsram = (uint8_t)((val >> 4) & 1u);
            uint32_t addr_new = (uint32_t)(v->cmd_w1 & 0x3FFFu) |
                                ((uint32_t)(val & 3u) << 14);
            v->mode = (uint8_t)(rw | (cram << 1) | (vsram << 2));
            v->addr = addr_new;
            if (val & 0x80u) { /* DMA flag */
                vdp_start_dma(md);
            }
        }
        break;
    }
    case 3: /* unmapped ($C0000E) */
        break;
    default:
        break;
    }
}

void fb_vdp_write8(struct fb_md *md, uint32_t addr, uint8_t val)
{
    struct fb_vdp *v = &md->vdp;
    if ((addr & 0x1F) == 0x11) { /* PSG */
        fb_psg_write(&md->psg, val);
        return;
    }
    switch ((addr >> 1) & 7u) {
    case 0: { /* data port byte write */
        uint8_t space = (uint8_t)((v->mode & CMD_CRAM ? 2u : 0u) |
                                  (v->mode & CMD_VSRAM ? 1u : 0u));
        if (space == 0) { /* VRAM: single byte */
            v->vram[v->addr & 0xFFFFu] = val;
            v->addr = (v->addr + v->regs[15]) & 0xFFFFu;
        } else {
            /* CRAM/VSRAM byte writes duplicate into both halves */
            uint16_t w = (uint16_t)((val << 8) | val);
            fb_vdp_write16(md, addr & ~1u, w);
        }
        break;
    }
    case 2: { /* control port byte write: half of a command word */
        uint16_t full;
        if (!v->cmd_pending) {
            v->cmd_pending = 1;
            v->cmd_w1 = (uint16_t)(val << 8);
            break;
        }
        v->cmd_pending = 0;
        full = (uint16_t)(v->cmd_w1 | val);
        if ((full & 0xC000u) == 0x8000u) {
            uint8_t reg = (uint8_t)((full >> 8) & 0x1Fu);
            if (reg < 24)
                v->regs[reg] = full & 0xFFu;
            break;
        }
        {
            uint8_t rw = (uint8_t)((v->cmd_w1 >> 14) & 1u);
            uint8_t cram = (uint8_t)(((v->cmd_w1 >> 15) & 1u) | ((full >> 5) & 1u));
            uint8_t vsram = (uint8_t)((full >> 4) & 1u);
            v->mode = (uint8_t)(rw | (cram << 1) | (vsram << 2));
            v->addr = (uint32_t)(v->cmd_w1 & 0x3FFFu) | ((uint32_t)(full & 3u) << 14);
            if (full & 0x80u)
                vdp_start_dma(md);
        }
        break;
    }
    default:
        fb_vdp_write16(md, addr & ~1u, val);
        break;
    }
}

/* ---- per-frame timing ------------------------------------------------------- */

void fb_vdp_end_of_line(struct fb_md *md)
{
    struct fb_vdp *v = &md->vdp;
    int width = (v->regs[12] & 1u) ? 320 : 256;
    int line = v->line;

    if (line < FB_VBLANK_LINE) {
        latch_hscroll(v);
        render_line(v, line, width);
    }

    /* H-interrupt counter (decrements every line) */
    uint16_t hc = v->hint_counter;
    if (hc == 0)
        hc = 0xFFFFu; /* 0 reload treated as disabled */
    hc--;
    if (hc == 0) {
        if (v->regs[0] & 0x10u)
            v->hint_68k = 1;
        hc = v->regs[10];
        if (hc == 0)
            hc = 0xFFFFu;
    }
    v->hint_counter = hc;

    v->line++;
    if (v->line >= FB_LINES)
        v->line = 0;

    /* V-interrupt at the start of vblank */
    if (line + 1 == FB_VBLANK_LINE && (v->regs[1] & 0x20u)) {
        v->vint_68k = 1;
        v->vint_z80 = 1;
    }

    /* recompute the 68K interrupt level */
    uint8_t lvl = 0;
    if (v->vint_68k)
        lvl = 6;
    if (v->hint_68k && lvl < 4)
        lvl = 4;
    if (md->ym.irq && lvl < 2)
        lvl = 2;
    fb_m68k_set_irq(md, lvl);
    md->z80.int_pending = v->vint_z80;
    v->line_cycle = 0;
}
