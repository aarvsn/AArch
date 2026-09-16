/*
 * beatle-nes-redux PPU.
 *
 * Register interface (loopy v/t/x/w), dot-based counters with VBlank/NMI at
 * the documented dots, a per-line background fetch walk that drives mapper
 * CHR accesses (MMC3 A12 edges), simplified sprite evaluation at dot 257,
 * and scanline rasterization with correct BG/sprite priority and sprite 0
 * hit.
 *
 * Documented approximations (this milestone):
 *  - mid-scanline register changes are not reflected within a line
 *  - sprite evaluation happens once per line at dot 257; overflow flag is
 *    set without the hardware's buggy evaluation behavior
 *  - grayscale bit implemented; color emphasis bits are not
 *  - open bus returns the last value written to any PPU register
 */
#include "nes.h"

#include <string.h>

static const uint32_t nes_palette_base[64] = {
    0xFF666666u, 0xFF002A88u, 0xFF1412A7u, 0xFF3B00A4u, 0xFF5C007Eu, 0xFF6E0040u, 0xFF6C0600u, 0xFF561D00u,
    0xFF333500u, 0xFF0B4800u, 0xFF005200u, 0xFF004F08u, 0xFF00404Du, 0xFF000000u, 0xFF000000u, 0xFF000000u,
    0xFFADADADu, 0xFF155FD9u, 0xFF4240FFu, 0xFF7527FEu, 0xFFA01ACCu, 0xFFB71E7Bu, 0xFFB53120u, 0xFF994E00u,
    0xFF6B6D00u, 0xFF388700u, 0xFF0C9300u, 0xFF008F32u, 0xFF007C8Du, 0xFF000000u, 0xFF000000u, 0xFF000000u,
    0xFFFFFEFFu, 0xFF64B0FFu, 0xFF9290FFu, 0xFFC676FFu, 0xFFF36AFFu, 0xFFFE6ECCu, 0xFFFE8170u, 0xFFEA9E22u,
    0xFFBCBE00u, 0xFF88D800u, 0xFF5CE430u, 0xFF45E082u, 0xFF48CDDEu, 0xFF4F4F4Fu, 0xFF000000u, 0xFF000000u,
    0xFFFFFEFFu, 0xFFC0DFFFu, 0xFFD3D2FFu, 0xFFE8C8FFu, 0xFFFBC2FFu, 0xFFFEC4EAu, 0xFFFECCC5u, 0xFFF7D8A5u,
    0xFFE4E594u, 0xFFCFEF96u, 0xFFBDF4ABu, 0xFFB3F3CCu, 0xFFB5EBF2u, 0xFFB8B8B8u, 0xFF000000u, 0xFF000000u
};

void nes_ppu_init(nes_ppu *p, uint32_t *fb)
{
    memset(p, 0, sizeof *p);
    p->fb = fb;
    nes_ppu_reset(p);
}

void nes_ppu_reset(nes_ppu *p)
{
    uint32_t *fb = p->fb;
    memset(p, 0, sizeof *p);
    p->fb = fb;
    p->scanline = 261;
    p->spr0_slot = 0xFF;
    for (int i = 0; i < 32; i++)
        p->palette[i] = 0x0F;
}

/* ---- VRAM access with mirroring ---- */

static uint16_t nt_mirror(nes_t *n, uint16_t addr)
{
    addr &= 0x0FFFu;
    uint8_t m = n->cart.mirroring; /* 0 horizontal, 1 vertical */
    if (m == 1u)
        return (uint16_t)((addr & 0x03FFu) | (addr & 0x0400u));
    return (uint16_t)((addr & 0x03FFu) | ((addr & 0x0800u) >> 1));
}

static uint8_t ppu_mem_read(nes_t *n, uint16_t addr)
{
    addr &= 0x3FFFu;
    if (addr < 0x2000u)
        return nes_cart_chr_read(&n->cart, addr);
    if (addr < 0x3F00u)
        return n->ppu.vram[nt_mirror(n, addr)];
    return 0;
}

static void ppu_mem_write(nes_t *n, uint16_t addr, uint8_t v)
{
    addr &= 0x3FFFu;
    if (addr < 0x2000u) {
        nes_cart_chr_write(&n->cart, addr, v);
        return;
    }
    if (addr < 0x3F00u) {
        n->ppu.vram[nt_mirror(n, addr)] = v;
        return;
    }
    addr &= 0x1Fu;
    if ((addr & 0x13u) == 0x10u)
        addr &= 0x0Fu; /* $3F10/14/18/1C mirror $3F00/04/08/0C */
    n->ppu.palette[addr] = (uint8_t)(v & 0x3Fu);
}

/* ---- register access ---- */

static void update_nmi(nes_t *n);

uint8_t nes_ppu_read(nes_t *n, uint16_t addr)
{
    nes_ppu *p = &n->ppu;
    uint8_t r;
    switch (addr & 7u) {
    case 2:
        r = (uint8_t)((p->status & 0xE0u) | (p->open_bus & 0x1Fu));
        p->status &= (uint8_t)~0x80u; /* reading clears vblank */
        p->w = 0;
        update_nmi(n);
        return r;
    case 4:
        return p->oam[p->oam_addr];
    case 7: {
        uint16_t a = p->v & 0x3FFFu;
        if (a >= 0x3F00u) {
            uint8_t idx = (uint8_t)(a & 0x1Fu);
            if ((idx & 0x13u) == 0x10u)
                idx &= 0x0Fu;
            r = (uint8_t)((p->palette[idx] & 0x3Fu) | (p->open_bus & 0xC0u));
            p->read_buffer = ppu_mem_read(n, (uint16_t)(a & 0x2FFFu));
        } else {
            r = p->read_buffer;
            p->read_buffer = ppu_mem_read(n, a);
        }
        p->v = (uint16_t)(p->v + ((p->ctrl & 4u) ? 32u : 1u));
        return r;
    }
    default:
        return p->open_bus;
    }
}

void nes_ppu_write(nes_t *n, uint16_t addr, uint8_t v)
{
    nes_ppu *p = &n->ppu;
    p->open_bus = v;
    switch (addr & 7u) {
    case 0:
        p->ctrl = v;
        p->t = (uint16_t)((p->t & ~0x0C00u) | (((uint16_t)v & 3u) << 10));
        break;
    case 1:
        p->mask = v;
        break;
    case 3:
        p->oam_addr = v;
        break;
    case 4:
        p->oam[p->oam_addr] = v;
        p->oam_addr++;
        break;
    case 5:
        if (p->w == 0) {
            p->fine_x = (uint8_t)(v & 7u);
            p->t = (uint16_t)((p->t & ~0x001Fu) | ((uint16_t)(v >> 3) & 0x1Fu));
            p->w = 1;
        } else {
            p->t = (uint16_t)((p->t & ~0x73E0u) | ((uint16_t)(v & 7u) << 12) |
                              ((uint16_t)(v >> 3) << 5));
            p->w = 0;
        }
        break;
    case 6:
        if (p->w == 0) {
            p->t = (uint16_t)((p->t & 0x00FFu) | ((uint16_t)(v & 0x3Fu) << 8));
            p->w = 1;
        } else {
            p->t = (uint16_t)((p->t & 0xFF00u) | v);
            p->v = p->t;
            p->w = 0;
        }
        break;
    case 7:
        ppu_mem_write(n, p->v & 0x3FFFu, v);
        p->v = (uint16_t)(p->v + ((p->ctrl & 4u) ? 32u : 1u));
        break;
    default:
        break;
    }
}

/* ---- NMI edge ---- */

static void update_nmi(nes_t *n)
{
    nes_ppu *p = &n->ppu;
    uint8_t level = (uint8_t)(((p->status & 0x80u) && (p->ctrl & 0x80u)) ? 1 : 0);
    if (level && !p->nmi_out_prev)
        n->cpu.nmi_pending = 1;
    p->nmi_line = level;
    p->nmi_out_prev = level;
}

/* ---- background fetch walk ---- */

typedef struct {
    uint16_t nt_base;
    uint8_t coarse_y;
    uint8_t fine_y;
    uint16_t bg_base;
    uint8_t coarse_x_start;
    uint8_t at_latch[34];
    uint8_t pt_latch_lo[34];
    uint8_t pt_latch_hi[34];
} bg_walk;

static void bg_walk_line(nes_t *n, bg_walk *w)
{
    nes_ppu *p = &n->ppu;
    w->nt_base = (uint16_t)(0x2000u + (p->v & 0x0C00u));
    w->coarse_y = (uint8_t)((p->v >> 5) & 0x1Fu);
    w->fine_y = (uint8_t)((p->v >> 12) & 7u);
    w->bg_base = (uint16_t)((p->ctrl & 0x10u) ? 0x1000u : 0x0000u);
    w->coarse_x_start = (uint8_t)(p->v & 0x1Fu);

    for (int i = 0; i < 34; i++) {
        uint16_t cx = (uint16_t)((w->coarse_x_start + (uint16_t)i) & 0x1Fu);
        uint16_t nt = (uint16_t)(w->nt_base + (uint16_t)w->coarse_y * 32u + cx);
        if (w->coarse_x_start + (uint16_t)i >= 0x20u)
            nt ^= 0x0400u; /* horizontal nametable wrap */
        uint8_t tile = ppu_mem_read(n, nt);
        uint16_t at_addr =
            (uint16_t)(w->nt_base + 0x03C0u + (uint16_t)((w->coarse_y >> 2) * 8u) +
                       (cx >> 2));
        if (w->coarse_x_start + (uint16_t)i >= 0x20u)
            at_addr ^= 0x0400u;
        uint8_t at = ppu_mem_read(n, at_addr);
        uint8_t shift = (uint8_t)(((w->coarse_y & 2u) << 1) | (cx & 2u));
        w->at_latch[i] = (uint8_t)((at >> shift) & 3u);
        uint16_t pt_addr = (uint16_t)(w->bg_base + (uint16_t)tile * 16u + w->fine_y);
        w->pt_latch_lo[i] = nes_cart_chr_read(&n->cart, pt_addr);
        w->pt_latch_hi[i] = nes_cart_chr_read(&n->cart, (uint16_t)(pt_addr + 8u));
    }
}

/* ---- sprites ---- */

static void sprite_evaluate(nes_t *n)
{
    nes_ppu *p = &n->ppu;
    uint8_t height = (uint8_t)((p->ctrl & 0x20u) ? 16u : 8u);
    uint8_t line = (uint8_t)((p->scanline + 1u) & 0xFFu); /* next line */
    p->spr.count = 0;
    p->spr0_slot = 0xFF;
    p->status &= (uint8_t)~0x20u;

    for (int i = 0; i < 64; i++) {
        int diff = (int)line - (int)p->oam[i * 4 + 0];
        if (diff >= 0 && diff < (int)height) {
            if (p->spr.count == 8u) {
                p->status |= 0x20u; /* overflow (no HW bug emulation) */
                break;
            }
            uint8_t slot = p->spr.count;
            uint8_t tile = p->oam[i * 4 + 1];
            uint8_t attr = p->oam[i * 4 + 2];
            uint8_t row = (uint8_t)diff;
            if (attr & 0x80u)
                row = (uint8_t)(height - 1u - row);
            uint16_t base = (uint16_t)((p->ctrl & 0x08u) ? 0x1000u : 0x0000u);
            uint8_t tile_sel = tile;
            if (height == 16u) {
                tile_sel = (uint8_t)(tile & 0xFEu);
                if (row >= 8u) {
                    tile_sel = (uint8_t)(tile | 1u);
                    row &= 7u;
                }
            }
            uint16_t addr = (uint16_t)(base + (uint16_t)tile_sel * 16u + (row & 7u));
            p->spr.pat_lo[slot] = nes_cart_chr_read(&n->cart, addr);
            p->spr.pat_hi[slot] = nes_cart_chr_read(&n->cart, (uint16_t)(addr + 8u));
            p->spr.attr[slot] = attr;
            p->spr.spr_x[slot] = p->oam[i * 4 + 3];
            if (i == 0)
                p->spr0_slot = slot;
            p->spr.count++;
        }
    }
}

/* ---- rasterize current scanline ---- */

static void render_line(nes_t *n)
{
    nes_ppu *p = &n->ppu;
    uint8_t show_bg = (uint8_t)((p->mask & 0x08u) != 0u);
    uint8_t show_spr = (uint8_t)((p->mask & 0x10u) != 0u);
    uint8_t bg_left = (uint8_t)((p->mask & 0x02u) != 0u);
    uint8_t spr_left = (uint8_t)((p->mask & 0x04u) != 0u);

    bg_walk w;
    bg_walk_line(n, &w);

    for (int x = 0; x < 256; x++) {
        p->line_bg[x] = 0;
        p->line_spr[x] = 0;
        p->line_spr0[x] = 0;
    }

    if (show_bg) {
        for (int x = 0; x < 256; x++) {
            if (x < 8 && !bg_left)
                continue;
            uint32_t fx = (uint32_t)p->fine_x + (uint32_t)x;
            uint32_t tile_i = fx >> 3;
            uint8_t bit = (uint8_t)(7u - (fx & 7u));
            uint8_t lo = w.pt_latch_lo[tile_i];
            uint8_t hi = w.pt_latch_hi[tile_i];
            uint8_t color = (uint8_t)((((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u));
            if (color == 0)
                continue;
            p->line_bg[x] = (uint8_t)((w.at_latch[tile_i] << 2) | color);
        }
    }

    if (show_spr) {
        for (uint8_t s = 0; s < p->spr.count; s++) {
            uint8_t attr = p->spr.attr[s];
            for (int px = 0; px < 8; px++) {
                int x = (int)p->spr.spr_x[s] + px;
                if (x >= 256)
                    break;
                if (x < 0)
                    continue;
                if (x < 8 && !spr_left)
                    continue;
                if (p->line_spr[x] & 0x80u)
                    continue; /* earlier (lower-index) sprite already here */
                uint8_t bit = (uint8_t)((attr & 0x40u) ? (uint8_t)px : (uint8_t)(7u - px));
                uint8_t lo = p->spr.pat_lo[s];
                uint8_t hi = p->spr.pat_hi[s];
                uint8_t color = (uint8_t)((((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u));
                if (color == 0)
                    continue;
                p->line_spr[x] = (uint8_t)(0x80u | ((attr & 0x20u) ? 0x40u : 0u) |
                                           0x10u | ((attr & 3u) << 2) | color);
                if (s == p->spr0_slot)
                    p->line_spr0[x] = 1;
            }
        }
    }

    /* compose */
    uint32_t *out = &p->fb[(size_t)p->scanline * NES_SCREEN_W];
    for (int x = 0; x < 256; x++) {
        uint8_t bg = p->line_bg[x];
        uint8_t spr = p->line_spr[x];
        uint8_t use_spr = (uint8_t)(((spr & 0x80u) != 0u) &&
                                    ((bg == 0u) || ((spr & 0x40u) == 0u)));
        if (show_spr && use_spr) {
            /* sprite 0 hit: opaque sprite-0 pixel over opaque BG */
            if (p->line_spr0[x] && bg != 0u && show_bg && x != 255 &&
                (bg_left || x >= 8) && (spr_left || x >= 8))
                p->status |= 0x40u;
        }
        uint8_t idx = (show_spr && use_spr) ? (uint8_t)(spr & 0x1Fu) : bg;
        if (!show_bg && !show_spr)
            idx = 0; /* rendering disabled: backdrop color */
        uint32_t px = nes_palette_base[p->palette[idx & 0x1Fu] & 0x3Fu];
        if (p->mask & 1u) /* grayscale */
            px &= 0xFF303030u;
        out[x] = px;
    }
}

/* ---- per-dot stepping ---- */

static uint8_t rendering_enabled(const nes_ppu *p)
{
    return (uint8_t)((p->mask & 0x18u) != 0u);
}

static void inc_y(nes_ppu *p)
{
    if ((p->v & 0x7000u) != 0x7000u) {
        p->v += 0x1000u;
    } else {
        p->v &= (uint16_t)~0x7000u;
        uint8_t y = (uint8_t)((p->v >> 5) & 0x1Fu);
        if (y == 29u) {
            y = 0;
            p->v ^= 0x0800u;
        } else if (y == 31u) {
            y = 0;
        } else {
            y++;
        }
        p->v = (uint16_t)((p->v & ~0x03E0u) | ((uint16_t)y << 5));
    }
}

static void ppu_tick(nes_t *n)
{
    nes_ppu *p = &n->ppu;
    uint8_t rend = rendering_enabled(p);

    if (p->scanline < 240u || p->scanline == 261u) {
        if (rend) {
            if (p->scanline == 261u && p->dot == 1u) {
                p->status &= (uint8_t)~0xE0u; /* clear vblank, spr0, overflow */
                update_nmi(n);
            }
            /* rasterize the line that is ending, before v is advanced */
            if (p->dot == 255u && p->scanline < 240u)
                render_line(n);
            if (p->dot == 256u && p->scanline < 240u)
                inc_y(p);
            if (p->dot == 257u) {
                /* horizontal bits: v.x = t.x */
                p->v = (uint16_t)((p->v & ~0x041Fu) | (p->t & 0x041Fu));
                if (p->scanline < 240u)
                    sprite_evaluate(n);
            }
            if (p->scanline == 261u && p->dot >= 280u && p->dot <= 304u)
                p->v = (uint16_t)((p->v & ~0x7BE0u) | (p->t & 0x7BE0u));
        }
    }

    if (p->scanline == 241u && p->dot == 1u) {
        p->status |= 0x80u;
        p->vblank_event = 1;
        update_nmi(n);
    }

    /* odd-frame dot skip: on odd frames with rendering enabled the
     * pre-render line is one dot short */
    if (p->scanline == 261u && p->dot == 339u && rend && p->odd_frame) {
        p->dot = 0;
        p->scanline = 0;
        p->odd_frame ^= 1;
        return;
    }

    p->dot++;
    if (p->dot > 340u) {
        p->dot = 0;
        p->scanline++;
        if (p->scanline > 261u) {
            p->scanline = 0;
            p->odd_frame ^= 1;
        }
    }
}

void nes_ppu_run(nes_t *n, uint32_t cpu_cycles)
{
    uint32_t dots = cpu_cycles * 3u;
    for (uint32_t i = 0; i < dots; i++)
        ppu_tick(n);
}
