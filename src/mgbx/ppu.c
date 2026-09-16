/*
 * mgbx PPU: scanline renderer with dot-accurate mode timing.
 *
 * Per line: 80 dots OAM mode (2), 172 dots drawing (3), 204 dots HBlank (0).
 * Lines 0-143 visible, 144-153 VBlank. The scanline is rasterized when the
 * line enters mode 3; mid-scanline register changes therefore affect the
 * whole line (documented approximation).
 *
 * VRAM/OAM access conflicts are not modeled (CPU reads/writes always succeed).
 */
#include "mgbx.h"

#include <string.h>

enum { MODE_HBLANK = 0, MODE_VBLANK = 1, MODE_OAM = 2, MODE_DRAW = 3 };

/* DMG-style 4-shade palette (lightest to darkest) */
static const uint32_t shade_colors[4] = {
    0xFF9BBC0Fu, 0xFF8BAC0Fu, 0xFF306230u, 0xFF0F380Fu
};

static void check_lyc(struct mgbx *gb);

void gb_ppu_init(gb_ppu *ppu, uint32_t *fb)
{
    memset(ppu, 0, sizeof *ppu);
    ppu->fb = fb;
    gb_ppu_reset(ppu);
}

void gb_ppu_reset(gb_ppu *ppu)
{
    uint32_t *fb = ppu->fb;
    memset(ppu, 0, sizeof *ppu);
    ppu->fb = fb;
    ppu->lcdc = 0x91;
    ppu->stat = 0x85;
    ppu->bgp = 0xFC;
    ppu->obp0 = 0xFF;
    ppu->obp1 = 0xFF;
    ppu->wy = 0x00;
    ppu->wx = 0x00;
    for (size_t i = 0; i < GB_SCREEN_W * GB_SCREEN_H; i++)
        ppu->fb[i] = shade_colors[0];
}

uint8_t gb_ppu_vram_read(gb_ppu *ppu, uint16_t addr)
{
    return ppu->vram[addr - 0x8000u];
}

void gb_ppu_vram_write(gb_ppu *ppu, uint16_t addr, uint8_t v)
{
    ppu->vram[addr - 0x8000u] = v;
}

uint8_t gb_ppu_oam_read(gb_ppu *ppu, uint16_t addr)
{
    return ppu->oam[addr - 0xFE00u];
}

void gb_ppu_oam_write(gb_ppu *ppu, uint16_t addr, uint8_t v)
{
    ppu->oam[addr - 0xFE00u] = v;
}

uint8_t gb_ppu_read_reg(gb_ppu *ppu, uint16_t addr)
{
    switch (addr) {
    case 0xFF40: return ppu->lcdc;
    case 0xFF41: return (uint8_t)(ppu->stat | 0x80u);
    case 0xFF42: return ppu->scy;
    case 0xFF43: return ppu->scx;
    case 0xFF44: return ppu->ly;
    case 0xFF45: return ppu->lyc;
    case 0xFF47: return ppu->bgp;
    case 0xFF48: return (uint8_t)(ppu->obp0 | 0xC0u);
    case 0xFF49: return (uint8_t)(ppu->obp1 | 0xC0u);
    case 0xFF4A: return ppu->wy;
    case 0xFF4B: return ppu->wx;
    default: return 0xFFu;
    }
}

void gb_ppu_write_reg(struct mgbx *gb, uint16_t addr, uint8_t v)
{
    gb_ppu *ppu = &gb->ppu;
    switch (addr) {
    case 0xFF40:
        ppu->lcdc = v;
        if (!(v & 0x80u)) {
            /* LCD off: blank white, timing reset, window line counter reset */
            ppu->ly = 0;
            ppu->dot = 0;
            ppu->win_line = 0;
            ppu->stat = (uint8_t)((ppu->stat & 0x78u) | 0x80u);
            for (size_t i = 0; i < GB_SCREEN_W * GB_SCREEN_H; i++)
                ppu->fb[i] = shade_colors[0];
        }
        break;
    case 0xFF41:
        ppu->stat = (uint8_t)((ppu->stat & 0x07u) | (v & 0x78u) | 0x80u);
        break;
    case 0xFF42: ppu->scy = v; break;
    case 0xFF43: ppu->scx = v; break;
    case 0xFF45:
        ppu->lyc = v;
        check_lyc(gb);
        break;
    case 0xFF47: ppu->bgp = v; break;
    case 0xFF48: ppu->obp0 = v; break;
    case 0xFF49: ppu->obp1 = v; break;
    case 0xFF4A: ppu->wy = v; break;
    case 0xFF4B: ppu->wx = v; break;
    default: break;
    }
    (void)gb;
}

static void check_lyc(struct mgbx *gb)
{
    gb_ppu *ppu = &gb->ppu;
    uint8_t coincident = (uint8_t)(ppu->ly == ppu->lyc);
    ppu->stat = (uint8_t)((ppu->stat & ~0x04u) | (coincident ? 0x04u : 0u));
    if (coincident && (ppu->stat & 0x40u) && (ppu->lcdc & 0x80u))
        gb_request_interrupt(gb, 1);
}

static void set_mode(struct mgbx *gb, uint8_t mode)
{
    gb_ppu *ppu = &gb->ppu;
    ppu->stat = (uint8_t)((ppu->stat & ~0x03u) | mode);
    switch (mode) {
    case MODE_OAM:
        if (ppu->stat & 0x20u)
            gb_request_interrupt(gb, 1);
        break;
    case MODE_VBLANK:
        if (ppu->stat & 0x10u)
            gb_request_interrupt(gb, 1);
        break;
    case MODE_HBLANK:
        if (ppu->stat & 0x08u)
            gb_request_interrupt(gb, 1);
        break;
    default:
        break;
    }
}

/* renders one full line into line_pixels (shade indices 0..3) */
static void render_line(struct mgbx *gb)
{
    gb_ppu *ppu = &gb->ppu;
    uint8_t ly = ppu->ly;

    for (int x = 0; x < (int)GB_SCREEN_W; x++)
        ppu->line_pixels[x] = 0;

    int window_drawn = 0;

    /* background + window */
    if (ppu->lcdc & 0x01u) {
        uint16_t bg_map = (ppu->lcdc & 0x08u) ? 0x1C00u : 0x1800u;
        uint16_t win_map = (ppu->lcdc & 0x40u) ? 0x1C00u : 0x1800u;
        int win_active = (ppu->lcdc & 0x20u) && ly >= ppu->wy && ppu->wx <= 166u;

        for (int x = 0; x < (int)GB_SCREEN_W; x++) {
            uint16_t data_addr;
            uint8_t in_tile_row, tile_col;
            if (win_active && x + 7 >= (int)ppu->wx) {
                int wx_in = x - ((int)ppu->wx - 7);
                uint8_t map_row = (uint8_t)(ppu->win_line >> 3);
                in_tile_row = (uint8_t)(ppu->win_line & 7u);
                tile_col = (uint8_t)((uint32_t)wx_in >> 3);
                uint16_t map_addr =
                    (uint16_t)(win_map + (uint16_t)map_row * 32u + tile_col);
                uint8_t index = ppu->vram[map_addr];
                if (ppu->lcdc & 0x10u)
                    data_addr = (uint16_t)((uint16_t)index * 16u);
                else
                    data_addr = (uint16_t)(0x1000u + (int16_t)(int8_t)index * 16);
                window_drawn = 1;
                uint8_t bit = (uint8_t)(7u - ((uint32_t)wx_in & 7u));
                uint8_t lo = ppu->vram[data_addr + (uint16_t)in_tile_row * 2u];
                uint8_t hi = ppu->vram[data_addr + (uint16_t)in_tile_row * 2u + 1u];
                ppu->line_pixels[x] =
                    (uint8_t)((((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u));
            } else {
                uint16_t eff_y = (uint16_t)(ly + ppu->scy);
                uint16_t eff_x = (uint16_t)(x + ppu->scx);
                uint8_t map_row = (uint8_t)((eff_y >> 3) & 0xFFu);
                tile_col = (uint8_t)((eff_x >> 3) & 0xFFu);
                in_tile_row = (uint8_t)(eff_y & 7u);
                uint16_t map_addr =
                    (uint16_t)(bg_map + (uint16_t)map_row * 32u + tile_col);
                uint8_t index = ppu->vram[map_addr];
                if (ppu->lcdc & 0x10u)
                    data_addr = (uint16_t)((uint16_t)index * 16u);
                else
                    data_addr = (uint16_t)(0x1000u + (int16_t)(int8_t)index * 16);
                uint8_t bit = (uint8_t)(7u - (eff_x & 7u));
                uint8_t lo = ppu->vram[data_addr + (uint16_t)in_tile_row * 2u];
                uint8_t hi = ppu->vram[data_addr + (uint16_t)in_tile_row * 2u + 1u];
                ppu->line_pixels[x] =
                    (uint8_t)((((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u));
            }
        }
    }

    /* sprites: pick up to 10 by scan order, draw lower-X first */
    if (ppu->lcdc & 0x02u) {
        uint8_t height = (ppu->lcdc & 0x04u) ? 16u : 8u;
        int count = 0;
        uint8_t order[40];
        for (int i = 0; i < 40 && count < 10; i++) {
            uint8_t sy = ppu->oam[i * 4 + 0];
            int line = (int)ly + 16 - (int)sy;
            if (line >= 0 && line < (int)height) {
                order[count++] = (uint8_t)i;
            }
        }
        /* stable sort by X (lower X wins; ties keep OAM order) */
        for (int a = 1; a < count; a++) {
            uint8_t key = order[a];
            uint8_t kx = ppu->oam[key * 4 + 1];
            int b = a - 1;
            while (b >= 0 && ppu->oam[order[b] * 4 + 1] > kx) {
                order[b + 1] = order[b];
                b--;
            }
            order[b + 1] = key;
        }

        memset(ppu->sprite_drawn, 0, GB_SCREEN_W);
        for (int s = 0; s < count; s++) {
            uint8_t idx = order[s];
            const uint8_t *attr = &ppu->oam[idx * 4];
            int sx = attr[1] - 8;
            uint8_t tile = attr[2];
            uint8_t flags = attr[3];
            uint8_t pal = (flags & 0x10u) ? ppu->obp1 : ppu->obp0;
            int row = (int)ly + 16 - (int)attr[0];
            if (flags & 0x40u) /* Y flip */
                row = (int)height - 1 - row;
            uint8_t tile_row = (uint8_t)(row & 7u);
            uint8_t tile_sel = tile;
            if (height == 16u) {
                tile_sel = (uint8_t)(tile & 0xFEu);
                if (row >= 8)
                    tile_sel = (uint8_t)(tile | 1u);
            }
            uint16_t data_addr = (uint16_t)(tile_sel * 16u + (uint16_t)tile_row * 2u);
            uint8_t lo = ppu->vram[data_addr];
            uint8_t hi = ppu->vram[data_addr + 1u];

            for (int px = 0; px < 8; px++) {
                int x = sx + px;
                if (x < 0 || x >= (int)GB_SCREEN_W)
                    continue;
                if (ppu->sprite_drawn[x])
                    continue;
                uint8_t bit = (flags & 0x20u) ? (uint8_t)px : (uint8_t)(7u - px);
                uint8_t color2 =
                    (uint8_t)((((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u));
                if (color2 == 0)
                    continue;
                if ((flags & 0x80u) && ppu->line_pixels[x] != 0)
                    continue; /* OBJ behind BG colors 1-3 */
                ppu->line_pixels[x] = (uint8_t)(((pal >> (color2 * 2u)) & 3u) | 4u);
                ppu->sprite_drawn[x] = 1;
            }
        }
    }

    /* compose framebuffer row: values 0-3 are BG/window shades; values 4-7
     * are sprite shades stored as (pal_color | 4) */
    uint32_t *out = &ppu->fb[(size_t)ly * GB_SCREEN_W];
    for (int x = 0; x < (int)GB_SCREEN_W; x++) {
        uint8_t p = ppu->line_pixels[x];
        if (p & 4u)
            out[x] = shade_colors[p & 3u];
        else
            out[x] = shade_colors[ppu->bgp >> (p * 2u) & 3u];
    }
    if (window_drawn)
        ppu->win_line++;
}

void gb_ppu_step(struct mgbx *gb, uint32_t t_cycles)
{
    gb_ppu *ppu = &gb->ppu;
    if (!(ppu->lcdc & 0x80u))
        return; /* LCD off: frozen; framebuffer already blanked */

    for (uint32_t i = 0; i < t_cycles; i++) {
        ppu->dot++;
        if (ppu->dot == 456u) {
            /* end of line */
            ppu->dot = 0;
            if (ppu->ly == 153u) {
                ppu->ly = 0;
                check_lyc(gb);
                set_mode(gb, MODE_OAM);
            } else {
                ppu->ly++;
                check_lyc(gb);
                if (ppu->ly == 144u) {
                    set_mode(gb, MODE_VBLANK);
                    gb_request_interrupt(gb, 0);
                } else {
                    set_mode(gb, MODE_OAM);
                }
            }
            continue;
        }
        if (ppu->ly < 144u) {
            if (ppu->dot == 80u) {
                render_line(gb);
                set_mode(gb, MODE_DRAW);
            } else if (ppu->dot == 252u) {
                set_mode(gb, MODE_HBLANK);
            }
        }
    }
}
