/*
 * finalburn VDP tests: register/data ports, palette, DMA, rendering,
 * interrupts. Expectations derived from the Mega Drive VDP documentation.
 */
#include "tests.h"
#include "fbh.h"
#include "finalburn/fb_md.h"
#include "emu/emu.h"

#include <stdlib.h>
#include <string.h>

/* control-port command word helpers (see vdp.c decode comment) */
static void vdp_cmd(struct fb_md *md, uint8_t rw, uint8_t cram, uint8_t vsram,
                    uint32_t addr, uint8_t dma)
{
    uint16_t w1 = (uint16_t)(((uint16_t)rw << 14) | (addr & 0x3FFFu) |
                             ((uint16_t)cram << 15));
    uint16_t w2 = (uint16_t)(((uint16_t)vsram << 4) | ((uint16_t)cram << 5) |
                             ((uint16_t)dma << 7) | ((addr >> 14) & 3u));
    fb_vdp_write16(md, 0xC00004u, w1);
    fb_vdp_write16(md, 0xC00004u, w2);
}

static void vdp_reg(struct fb_md *md, uint8_t reg, uint8_t val)
{
    fb_vdp_write16(md, 0xC00004u, (uint16_t)(0x8000u | ((uint16_t)reg << 8) | val));
}

static void test_reg_write_and_autoinc(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    vdp_reg(md, 15, 2); /* auto-increment 2 */
    T_CHECK_EQ_U(md->vdp.regs[15], 2);
    vdp_cmd(md, 1, 0, 0, 0x0000, 0); /* VRAM write at 0 */
    fb_vdp_write16(md, 0xC00000u, 0x1234);
    fb_vdp_write16(md, 0xC00000u, 0xBEEF);
    T_CHECK_EQ_U(md->vdp.vram[0], 0x34);
    T_CHECK_EQ_U(md->vdp.vram[1], 0x12);
    T_CHECK_EQ_U(md->vdp.vram[2], 0xEF);
    T_CHECK_EQ_U(md->vdp.vram[3], 0xBE);
    emu_core_destroy(&md->base);
}

static void test_cram_9bit(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    vdp_reg(md, 15, 2); /* auto-increment 2 -> next CRAM word */
    vdp_cmd(md, 1, 1, 0, 0x0000, 0); /* CRAM write */
    /* 9-bit color 0BBB0GGG0RRR: 0x777 = R7 G7 B7 = white */
    fb_vdp_write16(md, 0xC00000u, 0x0777);
    T_CHECK_EQ_U(md->vdp.cram[0], 0x77);
    T_CHECK_EQ_U(md->vdp.cram[1], 0x07);
    /* bit 3 of each nibble is forced off by the mask: 0xAA -> 0x22 */
    fb_vdp_write16(md, 0xC00000u, 0x0FAA);
    T_CHECK_EQ_U(md->vdp.cram[2], 0x22);
    T_CHECK_EQ_U(md->vdp.cram[3], 0x07); /* bit 3 of the top nibble masked */
    emu_core_destroy(&md->base);
}

static void test_backdrop_render(void)
{
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    fb_rom_w16(rom, 0x400, 0x60FE); /* BRA.S self */
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    vdp_reg(md, 7, 0x09); /* backdrop = palette 0, color 9 */
    vdp_cmd(md, 1, 1, 0, 0x0012, 0); /* CRAM word 9 = byte offset 0x12 */
    /* 9-bit word 0x0006 = R6 G0 B0 */
    fb_vdp_write16(md, 0xC00000u, 0x0006);
    fb_frame(md);
    uint32_t px = md->fb[0];
    T_CHECK_EQ_U(EMU_PIXEL_R(px), 6u * 255u / 7u);
    T_CHECK_EQ_U(EMU_PIXEL_G(px), 0);
    T_CHECK_EQ_U(EMU_PIXEL_B(px), 0);
    emu_core_destroy(&md->base);
}

static void test_dma_fill(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    vdp_reg(md, 15, 1);
    vdp_reg(md, 19, 0x0010); /* length 16 words */
    vdp_reg(md, 20, 0x0000);
    vdp_reg(md, 21, 0x00AA); /* fill value byte */
    vdp_reg(md, 22, 0x0000);
    vdp_reg(md, 23, 0x40);   /* DMA type 1 = VRAM fill */
    vdp_cmd(md, 1, 0, 0, 0x0100, 1); /* VRAM write at $100, DMA flag */
    T_CHECK_EQ_U(md->vdp.dma_active, 1);
    fb_vdp_run_dma(md, 16 * 8 + 8);
    for (int i = 0; i < 16; i++)
        T_CHECK_EQ_U(md->vdp.vram[0x100 + i], 0xAA);
    T_CHECK_EQ_U(md->vdp.dma_active, 0);
    emu_core_destroy(&md->base);
}

static void test_dma_68k_to_vram(void)
{
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    for (int i = 0; i < 8; i++)
        fb_rom_w16(rom, 0x600 + i * 2, (uint16_t)(0x5500 + i));
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    vdp_reg(md, 15, 2);
    vdp_reg(md, 19, 0x0008); /* 8 words */
    vdp_reg(md, 20, 0x0000);
    vdp_reg(md, 21, 0x00); /* source $6000: low byte 0, high byte 6 */
    vdp_reg(md, 22, 0x06);
    vdp_reg(md, 23, 0x00);   /* DMA type 0 = 68K -> VDP */
    vdp_cmd(md, 1, 0, 0, 0x0200, 1);
    fb_vdp_run_dma(md, 8 * 8 + 8);
    for (int i = 0; i < 8; i++) {
        uint16_t v = (uint16_t)(md->vdp.vram[0x200 + i * 2] |
                                (md->vdp.vram[0x200 + i * 2 + 1] << 8));
        T_CHECK_EQ_U(v, (uint16_t)(0x5500 + i));
    }
    emu_core_destroy(&md->base);
}

static void test_vint_flag(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    vdp_reg(md, 1, 0x20); /* IE0: enable V-int */
    md->vdp.line = FB_VBLANK_LINE - 1;
    fb_vdp_end_of_line(md);
    T_CHECK_EQ_U(md->vdp.vint_68k, 1);
    uint16_t st = fb_vdp_read16(md, 0xC00004u);
    T_CHECK_EQ_U(st & 0x80u, 0x80u);
    T_CHECK_EQ_U(md->vdp.vint_68k, 0);
    emu_core_destroy(&md->base);
}

static void test_vram_read_back(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    vdp_reg(md, 15, 2);
    vdp_cmd(md, 1, 0, 0, 0x0400, 0);
    fb_vdp_write16(md, 0xC00000u, 0xCAFE);
    vdp_cmd(md, 0, 0, 0, 0x0400, 0); /* VRAM read at $400 */
    (void)fb_vdp_read16(md, 0xC00000u); /* first read returns the stale buffer */
    uint16_t v = fb_vdp_read16(md, 0xC00000u);
    T_CHECK_EQ_U(v, 0xCAFE);
    emu_core_destroy(&md->base);
}

static void test_sprite_pixel(void)
{
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    fb_rom_w16(rom, 0x400, 0x60FE);
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }

    /* sprite table at VRAM $0600, sprite tile at $0000 */
    vdp_reg(md, 5, 3); /* sprite table $0600: (regs[5] & 0x7F) << 9 */
    vdp_reg(md, 6, 0x00);      /* sprite tile base 0 */
    vdp_reg(md, 7, 0x00);      /* backdrop color 0 */

    /* tile 0: top-left pixel = color 1 (palette 0), rest transparent */
    md->vdp.vram[0] = 0x80u; /* byte 0: plane0 bit for px0 */

    /* sprite entry: Y = 128+10 -> sy=10, 1x1 cell, link 0, prio 0, pal 0,
     * tile 0, X = 40 */
    uint8_t *sat = md->vdp.vram + 0x600;
    sat[0] = (uint8_t)(128 + 10);
    sat[1] = 0x00; /* size 1x1 */
    sat[2] = 0x00; /* link 0 */
    sat[3] = 0x00; /* pal 0, prio 0, tile hi 0 */
    sat[4] = 0x00;
    sat[5] = 40;   /* X = 40 */
    sat[6] = 0x00; /* tile lo = 0 */
    sat[7] = 0x00;

    vdp_reg(md, 1, 0x40); /* display enable (reg1 bit6) */
    fb_frame(md);
    /* pixel at (40,10) should be palette color 1 -> CRAM index 1 (black) */
    /* set CRAM color 1 to a known value first was skipped: default 0 ->
     * verify via collision-free path: write CRAM index 1 = 0x0777 (white) */
    vdp_cmd(md, 1, 1, 0, 0x0002, 0); /* CRAM write at word 1 */
    fb_vdp_write16(md, 0xC00000u, 0x0777);
    fb_frame(md);
    uint32_t px = md->fb[10 * FB_SCREEN_W + 40];
    T_CHECK_EQ_U(EMU_PIXEL_R(px), 255);
    T_CHECK_EQ_U(EMU_PIXEL_G(px), 255);
    T_CHECK_EQ_U(EMU_PIXEL_B(px), 255);
    /* neighbor pixel stays backdrop (color 0) */
    uint32_t px2 = md->fb[10 * FB_SCREEN_W + 41];
    T_CHECK_EQ_U(EMU_PIXEL_R(px2), 0);
    emu_core_destroy(&md->base);
}

T_SUITE_BEGIN(finalburn_vdp)
{ "reg_write_and_autoincrement", test_reg_write_and_autoinc },
{ "cram_9bit_color", test_cram_9bit },
{ "backdrop_render", test_backdrop_render },
{ "dma_fill", test_dma_fill },
{ "dma_68k_to_vram", test_dma_68k_to_vram },
{ "vint_flag_and_clear", test_vint_flag },
{ "vram_read_back", test_vram_read_back },
{ "sprite_pixel", test_sprite_pixel },
T_SUITE_END
T_SUITE_REG(finalburn_vdp)
