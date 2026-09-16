/*
 * mgbx PPU tests: tile rendering math, scroll, window, sprite priority and
 * palettes, LY/mode progression, VBlank/STAT interrupts. Expected pixel
 * colors are hand-computed from tile data written into VRAM.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbx/mgbx.h"

#include <stdlib.h>
#include <string.h>

static struct mgbx *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbx()->create(&c) != EMU_OK)
        return NULL;
    return (struct mgbx *)c;
}

static struct mgbx *mk_core_with_rom(void)
{
    struct mgbx *gb = mk_core();
    if (gb == NULL)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = gb_make_rom(2, GB_CART_ROM_ONLY, 0, &rom_size);
    emu_result_t r = emu_core_mgbx()->load_rom(&gb->base, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_mgbx()->destroy(&gb->base);
        return NULL;
    }
    return gb;
}

/* loads a tile (index) at tile-data address with rows: each byte pair */
static void load_tile(gb_ppu *ppu, uint8_t index, const uint8_t rows[8][2])
{
    for (int r = 0; r < 8; r++) {
        ppu->vram[index * 16u + (uint16_t)r * 2u] = rows[r][0];
        ppu->vram[index * 16u + (uint16_t)r * 2u + 1u] = rows[r][1];
    }
}

static void ppu_bg_tile_render(void)
{
    /* tile 1: alternating columns (lo=0xAA hi=0x55) -> colors 1,2 per pixel */
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    static const uint8_t rows[8][2] = {
        { 0xAA, 0x55 }, { 0xAA, 0x55 }, { 0xAA, 0x55 }, { 0xAA, 0x55 },
        { 0xAA, 0x55 }, { 0xAA, 0x55 }, { 0xAA, 0x55 }, { 0xAA, 0x55 }
    };
    load_tile(ppu, 1, rows);
    /* map entry 0 -> tile 1 */
    ppu->vram[0x1800] = 0x01;
    ppu->lcdc = 0x91; /* on, BG on, tile data 0x8000, map 0x9800 */
    ppu->bgp = 0xE4;  /* identity palette */
    ppu->ly = 0;
    /* run the PPU to line 0 mode 3 to rasterize */
    for (int i = 0; i < 80; i++)
        gb_ppu_step(gb, 4);
    /* expected pixel colors: bit pattern 0xAA/0x55: for pixel x, bit 7-x:
     * lo bit = (0xAA >> (7-x)) & 1 -> x even: 1, x odd: 0
     * hi bit = (0x55 >> (7-x)) & 1 -> x even: 0, x odd: 1
     * color = (hi<<1)|lo -> even: 1, odd: 2 */
    for (int x = 0; x < 8; x++) {
        int want = (x % 2 == 0) ? 1 : 2;
        int got = (int)((ppu->fb[x] == 0xFF9BBC0Fu) ? 0 :
                        (ppu->fb[x] == 0xFF8BAC0Fu) ? 1 :
                        (ppu->fb[x] == 0xFF306230u) ? 2 : 3);
        T_CHECK_EQ(got, want);
    }
    emu_core_mgbx()->destroy(&gb->base);
}

static void ppu_signed_tile_data(void)
{
    /* LCDC.4 = 0: tile indices are signed with base 0x1000:
     * index 0xFF (-1) -> data at 0x1000 - 16 */
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    /* all-black tile at 0x0FF0: lo=hi=0xFF */
    for (int r = 0; r < 8; r++) {
        ppu->vram[0x0FF0u + (uint16_t)r * 2u] = 0xFF;
        ppu->vram[0x0FF0u + (uint16_t)r * 2u + 1u] = 0xFF;
    }
    ppu->vram[0x1800] = 0xFF; /* tile -1 */
    ppu->lcdc = 0x91 & (uint8_t)~0x10u; /* signed tile data */
    ppu->bgp = 0xE4;
    ppu->ly = 0;
    for (int i = 0; i < 80; i++)
        gb_ppu_step(gb, 4);
    /* expected: color 3 everywhere (0xE4 maps 3 -> 3) */
    T_CHECK_EQ(ppu->fb[0], 0xFF0F380Fu);
    T_CHECK_EQ(ppu->fb[7], 0xFF0F380Fu);
    /* x >= 8 uses other map entries (tile 0, unwritten -> color 0) */
    T_CHECK_EQ(ppu->fb[8], 0xFF9BBC0Fu);
    T_CHECK_EQ(ppu->fb[159], 0xFF9BBC0Fu);
    emu_core_mgbx()->destroy(&gb->base);
}

static void ppu_scroll(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    /* tile 0: left half color 3, right half color 0 */
    for (int r = 0; r < 8; r++) {
        ppu->vram[r * 2u] = 0xF0;
        ppu->vram[r * 2u + 1u] = 0xF0;
    }
    ppu->vram[0x1800] = 0x00;
    ppu->lcdc = 0x91;
    ppu->bgp = 0xE4;
    /* tile 0 pixels: x 0-3 -> color 3, x 4-7 -> color 0.
     * SCX = 1: screen x shows tile pixel (x+1): x=0..2 -> pixels 1..3 (c3);
     * x=3 -> pixel 4 (c0); x=7 -> eff 8 = tile 1 pixel 0 (tile 1 unwritten,
     * color 0). */
    ppu->scx = 1;
    ppu->ly = 0;
    for (int i = 0; i < 80; i++)
        gb_ppu_step(gb, 4);
    uint32_t c0 = 0xFF9BBC0Fu, c3 = 0xFF0F380Fu;
    T_CHECK_EQ(ppu->fb[0], c3);
    T_CHECK_EQ(ppu->fb[2], c3);
    T_CHECK_EQ(ppu->fb[3], c0);
    T_CHECK_EQ(ppu->fb[6], c0);
    T_CHECK_EQ(ppu->fb[7], c3);   /* tile 0 of map entry 1, pixel 0 */
    T_CHECK_EQ(ppu->fb[159], c3); /* eff 160: tile col 20, pixel 0 */
    emu_core_mgbx()->destroy(&gb->base);
}

static void ppu_sprites_priority(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    /* solid tile 1 (color 1) */
    for (int r = 0; r < 8; r++) {
        ppu->vram[16u + (uint16_t)r * 2u] = 0xFF;
        ppu->vram[16u + (uint16_t)r * 2u + 1u] = 0x00;
    }
    ppu->lcdc = 0x93; /* on, OBJ on, BG on */
    ppu->bgp = 0xE4;
    ppu->obp0 = 0xE4;
    ppu->ly = 0;
    /* BG: solid color 0 */
    /* sprite 0 at X=8 (screen x=0), tile 1; sprite 1 at X=4 (screen x=-4),
     * tile 1: overlaps screen x 0..3; lower X (sprite 1) wins where both
     * cover (x 0..3), sprite 0 visible at x 4..7 */
    ppu->oam[0] = 16; ppu->oam[1] = 8; ppu->oam[2] = 1; ppu->oam[3] = 0x00;
    ppu->oam[4] = 16; ppu->oam[5] = 4; ppu->oam[6] = 1; ppu->oam[7] = 0x00;
    for (int i = 0; i < 80; i++)
        gb_ppu_step(gb, 4);
    /* all pixels 0..7 drawn (color 1 = 0xFF8BAC0F) */
    for (int x = 0; x < 8; x++)
        T_CHECK_EQ(ppu->fb[x], 0xFF8BAC0Fu);
    emu_core_mgbx()->destroy(&gb->base);
}

static void ppu_sprite_bg_priority(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    /* BG tile: all color 1; sprite tile: all color 3 with priority flag */
    for (int r = 0; r < 8; r++) {
        ppu->vram[r * 2u] = 0xFF; /* BG color 1 (lo only) */
        ppu->vram[r * 2u + 1u] = 0x00;
        ppu->vram[16u + (uint16_t)r * 2u] = 0xFF; /* sprite color 3 */
        ppu->vram[16u + (uint16_t)r * 2u + 1u] = 0xFF;
    }
    ppu->lcdc = 0x93;
    ppu->bgp = 0xE4;
    ppu->obp0 = 0xE4;
    ppu->ly = 0;
    ppu->oam[0] = 16; ppu->oam[1] = 8; ppu->oam[2] = 1; ppu->oam[3] = 0x80;
    for (int i = 0; i < 80; i++)
        gb_ppu_step(gb, 4);
    /* sprite behind BG color 1: BG (color 1) shows through */
    T_CHECK_EQ(ppu->fb[0], 0xFF8BAC0Fu);
    emu_core_mgbx()->destroy(&gb->base);
}

static void ppu_vblank_and_ly(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    /* run a full frame of PPU dots */
    for (uint32_t i = 0; i < GB_CYCLES_PER_FRAME / 4u; i++)
        gb_ppu_step(gb, 4);
    /* VBlank interrupt must have been requested */
    T_CHECK(gb->mem.if_reg & 0x01u);
    /* after a full frame, LY wrapped back to 0 */
    T_CHECK_EQ(ppu->ly, 0);
    /* stat vblank source bit remains from config; mode must be back to 2 */
    T_CHECK_EQ(ppu->stat & 0x03u, 2u);
    emu_core_mgbx()->destroy(&gb->base);
}

static void ppu_lyc_interrupt(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    ppu->stat |= 0x40u; /* LYC interrupt enable */
    ppu->lyc = 3;
    gb->mem.if_reg = 0;
    for (uint32_t i = 0; i < 100u; i++)
        gb_ppu_step(gb, 4);
    /* 100 M-cycles = 400 dots: still line 0. LYC=3 not reached yet */
    T_CHECK((gb->mem.if_reg & 0x02u) == 0);
    /* a line is 456 dots = 114 M-cycles; run to the start of line 3 */
    for (uint32_t i = 0; i < 3u * 114u - 100u; i++)
        gb_ppu_step(gb, 4);
    T_CHECK_EQ(ppu->ly, 3);
    T_CHECK(gb->mem.if_reg & 0x02u); /* STAT interrupt raised */
    T_CHECK(ppu->stat & 0x04u);      /* coincidence flag */
    emu_core_mgbx()->destroy(&gb->base);
}

static void ppu_window(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    /* BG tile: all color 3; window tile: all color 1 */
    for (int r = 0; r < 8; r++) {
        ppu->vram[r * 2u] = 0xFF;
        ppu->vram[r * 2u + 1u] = 0xFF;
        ppu->vram[16u + (uint16_t)r * 2u] = 0xFF;
        ppu->vram[16u + (uint16_t)r * 2u + 1u] = 0x00;
    }
    ppu->vram[0x1800] = 0x00;         /* BG map entry */
    ppu->vram[0x1C00] = 0x01;         /* window map entry (9C00) */
    ppu->lcdc = 0x91 | 0x20u | 0x40u; /* window enable, window map 9C00 */
    ppu->bgp = 0xE4;
    ppu->wy = 0;
    ppu->wx = 7; /* window starts at x=0 */
    ppu->ly = 0;
    for (int i = 0; i < 80; i++)
        gb_ppu_step(gb, 4);
    T_CHECK_EQ(ppu->fb[0], 0xFF8BAC0Fu);   /* window color 1 */
    T_CHECK_EQ(ppu->fb[7], 0xFF8BAC0Fu);   /* still window tile 1 */
    /* x >= 8: window map entry 1 -> tile 0 = the all-color-3 BG tile */
    T_CHECK_EQ(ppu->fb[8], 0xFF0F380Fu);
    T_CHECK_EQ(ppu->fb[159], 0xFF0F380Fu); /* window spans full width */
    T_CHECK_EQ(ppu->win_line, 1);          /* window line counter advanced */
    emu_core_mgbx()->destroy(&gb->base);
}

static void ppu_lcd_off_blanks(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_ppu *ppu = &gb->ppu;
    for (int i = 0; i < 200; i++)
        gb_ppu_step(gb, 4);
    uint8_t ly_before = ppu->ly;
    T_CHECK(ly_before != 0 || ppu->dot != 0);
    /* turn LCD off */
    gb_ppu_write_reg(gb, 0xFF40, 0x11);
    T_CHECK_EQ(ppu->ly, 0);
    T_CHECK_EQ(ppu->dot, 0);
    /* frozen: no progress */
    for (int i = 0; i < 200; i++)
        gb_ppu_step(gb, 4);
    T_CHECK_EQ(ppu->ly, 0);
    T_CHECK_EQ(ppu->dot, 0);
    emu_core_mgbx()->destroy(&gb->base);
}

T_SUITE_BEGIN(gb_ppu)
{ "bg_tile_render", ppu_bg_tile_render },
{ "signed_tile_data", ppu_signed_tile_data },
{ "bg_scroll", ppu_scroll },
{ "sprite_x_priority", ppu_sprites_priority },
{ "sprite_bg_priority", ppu_sprite_bg_priority },
{ "vblank_and_ly_wrap", ppu_vblank_and_ly },
{ "lyc_interrupt", ppu_lyc_interrupt },
{ "window_render", ppu_window },
{ "lcd_off_blanks", ppu_lcd_off_blanks },
T_SUITE_END

T_SUITE_REG(gb_ppu)
