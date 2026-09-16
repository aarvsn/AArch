/*
 * beatle-nes-redux PPU tests: register semantics, loopy scroll math,
 * VBlank/NMI, sprite 0 hit, palette mirroring, background rendering from
 * hand-built nametables/patterns.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "beatle-nes-redux/nes.h"

#include <stdlib.h>
#include <string.h>

static struct nes *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_beatle_nes_redux()->create(&c) != EMU_OK)
        return NULL;
    return (struct nes *)c;
}

static struct nes *mk_core_with_rom(void)
{
    struct nes *n = mk_core();
    if (n == NULL)
        return NULL;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(2, 1, 0, 0, &sz);
    emu_result_t r = emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz);
    free(rom);
    if (r != EMU_OK) {
        emu_core_beatle_nes_redux()->destroy(&n->base);
        return NULL;
    }
    return n;
}

static void ppu_vblank_timing(void)
{
    struct nes *n = mk_core_with_rom();
    T_CHECK(n != NULL);
    if (!n)
        return;
    /* start at a frame boundary and run one frame (29781 CPU cycles) */
    n->ppu.scanline = 0;
    n->ppu.dot = 0;
    nes_ppu_run(n, 29781);
    T_CHECK(n->ppu.status & 0x80u); /* vblank set */
    T_CHECK_EQ(n->ppu.scanline, 0); /* wrapped to next frame */
    T_CHECK_EQ(n->ppu.odd_frame, 1);
    /* vblank was requested: NMI pending only if ctrl.7; it is not */
    T_CHECK_EQ(n->cpu.nmi_pending, 0);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void ppu_nmi_edge(void)
{
    struct nes *n = mk_core_with_rom();
    T_CHECK(n != NULL);
    if (!n)
        return;
    nes_ppu_write(n, 0x2000, 0x80); /* NMI enable */
    nes_ppu_run(n, 29781);
    T_CHECK(n->cpu.nmi_pending); /* NMI was raised */
    T_CHECK(n->ppu.status & 0x80u);
    /* reading $2002 clears vblank and the NMI line */
    (void)nes_ppu_read(n, 0x2002);
    T_CHECK(!(n->ppu.status & 0x80u));
    T_CHECK_EQ(n->ppu.nmi_line, 0);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void ppu_scroll_loopy(void)
{
    struct nes *n = mk_core_with_rom();
    T_CHECK(n != NULL);
    if (!n)
        return;
    /* $2005 twice: X scroll = 0x18 (coarse 3, fine 0)? test t updates:
     * write 0x19 -> fine 1, coarse 3. then 0x2D -> fine y 5, coarse y 5. */
    nes_ppu_write(n, 0x2005, 0x19);
    T_CHECK_EQ(n->ppu.fine_x, 1);
    T_CHECK_EQ(n->ppu.t & 0x1Fu, 3u);
    nes_ppu_write(n, 0x2005, 0x2D);
    T_CHECK_EQ((n->ppu.t >> 12) & 7u, 5u);
    T_CHECK_EQ((n->ppu.t >> 5) & 0x1Fu, 5u);
    /* $2006 twice sets v = t */
    nes_ppu_write(n, 0x2006, 0x20);
    nes_ppu_write(n, 0x2006, 0x00);
    T_CHECK_EQ(n->ppu.v, n->ppu.t);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void ppu_data_read_buffered(void)
{
    struct nes *n = mk_core_with_rom();
    T_CHECK(n != NULL);
    if (!n)
        return;
    /* write pattern to VRAM $2000 via $2006/$2007, read back: first read
     * returns stale buffer, second returns the value */
    nes_ppu_write(n, 0x2006, 0x20);
    nes_ppu_write(n, 0x2006, 0x00);
    nes_ppu_write(n, 0x2007, 0xAB);
    nes_ppu_write(n, 0x2007, 0xCD);
    nes_ppu_write(n, 0x2006, 0x20);
    nes_ppu_write(n, 0x2006, 0x00);
    (void)nes_ppu_read(n, 0x2007); /* stale buffer */
    T_CHECK_EQ(nes_ppu_read(n, 0x2007), 0xAB);
    T_CHECK_EQ(nes_ppu_read(n, 0x2007), 0xCD);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void ppu_palette_mirror(void)
{
    struct nes *n = mk_core_with_rom();
    T_CHECK(n != NULL);
    if (!n)
        return;
    nes_ppu_write(n, 0x2006, 0x3F);
    nes_ppu_write(n, 0x2006, 0x10);
    nes_ppu_write(n, 0x2007, 0x21); /* $3F10 mirrors $3F00 */
    nes_ppu_write(n, 0x2006, 0x3F);
    nes_ppu_write(n, 0x2006, 0x00);
    T_CHECK_EQ(nes_ppu_read(n, 0x2007), 0x21);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void ppu_bg_render_pixel(void)
{
    struct nes *n = mk_core_with_rom();
    T_CHECK(n != NULL);
    if (!n)
        return;
    /* build a tile: pattern $0000: lo byte 0xFF, hi byte 0xAA ->
     * colors alternate 1,3. CHR is ROM here: write the buffer directly. */
    for (int r = 0; r < 8; r++) {
        n->cart.chr[(size_t)r] = 0xFF;
        n->cart.chr[(size_t)r + 8] = 0xAA;
    }
    /* nametable entry 0 = tile 0; attribute: quadrant for (0,0) = bits 0-1 */
    n->ppu.vram[0] = 0;
    n->ppu.vram[0x3C0] = 0xFF; /* all quadrants palette 3 */
    n->ppu.palette[0x0C] = 0x16; /* palette 3 color 1 */
    n->ppu.palette[0x0D] = 0x27; /* palette 3 color 2 -> idx 0x0D? (3<<2)|2=0x0E */
    n->ppu.palette[0x0E] = 0x27;
    /* enable BG rendering with pattern table 0, NT 0 */
    n->ppu.ctrl = 0x00;
    n->ppu.mask = 0x0A; /* BG on, left column visible */
    n->ppu.scanline = 0;
    n->ppu.dot = 0;
    n->ppu.v = 0;
    /* 86 CPU cycles = 258 dots: render fires at dot 255 of line 0 */
    nes_ppu_run(n, 86);
    /* pixel 0: tile 0 pixel 0: lo bit7=1, hi bit7=1 -> color 3 ->
     * palette index (3<<2)|3 = 0x0F */
    T_CHECK_EQ(n->ppu.palette[n->ppu.line_bg[0] & 0x1Fu], n->ppu.palette[0x0F]);
    T_CHECK_EQ(n->ppu.line_bg[0], 0x0F);
    /* pixel 1: bit6 of lo=1, hi=0 -> color 1 -> index 0x0D */
    T_CHECK_EQ(n->ppu.line_bg[1], 0x0D);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void ppu_sprite_zero_hit(void)
{
    struct nes *n = mk_core_with_rom();
    T_CHECK(n != NULL);
    if (!n)
        return;
    /* BG: opaque tile across the line; sprite 0 opaque at same x */
    for (int r = 0; r < 8; r++) {
        n->cart.chr[(size_t)r] = 0xFF;             /* BG tile 0: color 1 */
        n->cart.chr[(size_t)r + 8] = 0x00;
        n->cart.chr[16u + (size_t)r] = 0xFF;       /* sprite tile 1: color 1 */
        n->cart.chr[16u + (size_t)r + 8u] = 0x00;
    }
    n->ppu.vram[0] = 0;   /* BG tile 0 */
    n->ppu.oam[0] = 1;    /* sprite 0: y=1 -> on line 1 */
    n->ppu.oam[1] = 1;    /* tile 1 */
    n->ppu.oam[2] = 0;    /* attr */
    n->ppu.oam[3] = 8;    /* x=8 */
    n->ppu.ctrl = 0x00;
    n->ppu.mask = 0x18;   /* bg+spr on */
    n->ppu.scanline = 0;
    n->ppu.dot = 0;
    n->ppu.v = 0;
    /* run to line 1 dot 255: eval at (0,257) picks sprite 0 for line 1,
     * line 1 rasterizes at dot 255 -> sprite 0 over opaque BG -> hit */
    nes_ppu_run(n, 238); /* 714 dots */
    T_CHECK(n->ppu.status & 0x40u); /* sprite 0 hit */
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void ppu_sprite_priority(void)
{
    struct nes *n = mk_core_with_rom();
    T_CHECK(n != NULL);
    if (!n)
        return;
    /* two overlapping sprites: OAM index 1 (x=4) must beat index 0 (x=8)
     * in the overlap region x=8..11 */
    for (int r = 0; r < 8; r++) {
        n->cart.chr[16u + (size_t)r] = 0xFF;       /* tile 1 */
        n->cart.chr[16u + (size_t)r + 8u] = 0x00;
        n->cart.chr[32u + (size_t)r] = 0xFF;       /* tile 2 */
        n->cart.chr[32u + (size_t)r + 8u] = 0x00;
    }
    /* sprite 0: tile 1, x=8; sprite 1: tile 2 (different palette via attr
     * palette 1), x=4 */
    n->ppu.oam[0] = 1; n->ppu.oam[1] = 1; n->ppu.oam[2] = 0x01; n->ppu.oam[3] = 8;
    n->ppu.oam[4] = 1; n->ppu.oam[5] = 2; n->ppu.oam[6] = 0x02; n->ppu.oam[7] = 4;
    n->ppu.palette[0x09] = 0x11; /* pal 0? sprite 0 uses palette 0 (attr 1
                                    means palette 1: 0x10|1<<2|1 = 0x15) */
    n->ppu.palette[0x15] = 0x15;
    n->ppu.palette[0x19] = 0x19;
    n->ppu.ctrl = 0x00;
    n->ppu.mask = 0x14; /* sprites on, left column visible */
    n->ppu.scanline = 0;
    n->ppu.dot = 0;
    n->ppu.v = 0;
    nes_ppu_run(n, 238); /* through line 1 rasterization */
    /* x=8..11: both sprites overlap; sprite 0 has the lower OAM index and
     * wins. x=4..7: only sprite 1. */
    T_CHECK_EQ(n->ppu.line_spr[8] & 0x1Fu, 0x15u); /* sprite 0 pixel (pal 1) */
    T_CHECK_EQ(n->ppu.line_spr[4] & 0x1Fu, 0x19u); /* sprite 1 pixel (pal 2) */
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

T_SUITE_BEGIN(nes_ppu)
{ "vblank_timing", ppu_vblank_timing },
{ "nmi_edge", ppu_nmi_edge },
{ "scroll_loopy", ppu_scroll_loopy },
{ "data_read_buffered", ppu_data_read_buffered },
{ "palette_mirror", ppu_palette_mirror },
{ "bg_render_pixel", ppu_bg_render_pixel },
{ "sprite_zero_hit", ppu_sprite_zero_hit },
{ "sprite_priority", ppu_sprite_priority },
T_SUITE_END

T_SUITE_REG(nes_ppu)
