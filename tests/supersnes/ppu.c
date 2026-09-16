/*
 * supersnes %s tests.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "supersnes/snes.h"

#include <stdlib.h>
#include <string.h>

static struct snes *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_supersnes()->create(&c) != EMU_OK)
        return NULL;
    return (struct snes *)c;
}

static struct snes *mk_core_with_rom(void)
{
    struct snes *s = mk_core();
    if (s == NULL)
        return NULL;
    size_t sz = 0;
    uint8_t *rom = snes_make_lorom(16, &sz);
    emu_result_t r = emu_core_supersnes()->load_rom(&s->base, rom, sz);
    free(rom);
    if (r != EMU_OK) {
        emu_core_supersnes()->destroy(&s->base);
        return NULL;
    }
    return s;
}

static void ppu_cgram_and_palette(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    snes_ppu_write(s, 0x2121, 0x00);
    snes_ppu_write(s, 0x2122, 0x1F); /* low: R=31 */
    snes_ppu_write(s, 0x2122, 0x00); /* high */
    T_CHECK_EQ(s->ppu.cgram[0], 0x1F);
    T_CHECK_EQ(s->ppu.cgram[1], 0x00);
    emu_core_supersnes()->destroy(&s->base);
}
static void ppu_mode1_text_bg(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* build a 4bpp tile 1: all pixels color 3 (planes 0-3 all 0xFF) */
    for (int b = 0; b < 32; b++)
        s->ppu.vram[(uint32_t)b + 32u] = 0xFF; /* char base 0, tile 1 */
    /* tilemap word 0 = tile 1, palette 1: 0x0001 | 0x0400 = 0x0401 */
    s->ppu.vram[0] = 0x01;
    s->ppu.vram[1] = 0x04;
    /* all planes set -> color 15; BG1 pal 1 -> palette index 0x10+0x0F = 0x1F */
    s->ppu.cgram[0x1F * 2] = 0xFF;     /* white */
    s->ppu.cgram[0x1F * 2 + 1] = 0x7F;
    s->ppu.bgmode = 0x01;
    s->ppu.bg_map[0] = 0x00;   /* map at VRAM 0 */
    s->ppu.bg_char_base[0] = 0; /* chars at VRAM 0 */
    s->ppu.tm = 0x01;
    s->ppu.inidisp = 0x0F;      /* brightness on */
    s->ppu.line = 0;
    /* render line 0 via ppu_run: 1364 dots */
    snes_ppu_run(s, 1364u * 4u);
    /* pixel 0: color 3 -> white */
    T_CHECK_EQ(s->ppu.fb[0], EMU_PIXEL(0xFF, 0xFF, 0xFF));
    emu_core_supersnes()->destroy(&s->base);
}
static void ppu_mode7_transform(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* Identity transform: A=0x0100 (1.0), D=0x0100, B=C=0, center 0.
     * Screen pixel (128,128) maps to VRAM (0,0)? with H=V=0 and center
     * subtract 128: sx-128 + ... identity: x = sx-128 + 128*256>>8? verify
     * with known point: set H=V such that screen center hits tile 0. */
    s->ppu.bgmode = 0x07;
    s->ppu.m7a = 0x0100; /* 1.0 */
    s->ppu.m7d = 0x0100;
    s->ppu.m7b = 0;
    s->ppu.m7c = 0;
    s->ppu.m7x = 128; /* center offset: H=128 */
    s->ppu.m7y = 128;
    s->ppu.tm = 0x01;
    s->ppu.inidisp = 0x0F;
    /* mode 7 tilemap: 64x64 1-byte entries at VRAM 0; tile data 8bpp 64B */
    s->ppu.vram[0] = 5; /* tilemap (0,0) -> tile 5 */
    for (int i = 0; i < 64; i++)
        s->ppu.vram[5u * 64u + (uint32_t)i] = 0x15;
    s->ppu.cgram[0x15 * 2] = 0xFF;
    s->ppu.cgram[0x15 * 2 + 1] = 0x7F;
    s->ppu.line = 0;
    /* pixel (0,0): x = A*(0-128) + 0*(0-128) + (H<<8)
     *            = 256*(-128) + 128*256 = 0 -> VRAM pixel (0,0) -> tile 5
     *            color 0x15 (white) */
    snes_ppu_run(s, 1364u * 4u);
    T_CHECK_EQ(s->ppu.fb[0], EMU_PIXEL(0xFF, 0xFF, 0xFF));
    emu_core_supersnes()->destroy(&s->base);
}

T_SUITE_BEGIN(snes_ppu)
{ "cgram_palette", ppu_cgram_and_palette },
{ "mode1_text_bg", ppu_mode1_text_bg },
{ "mode7_transform", ppu_mode7_transform },
T_SUITE_END
T_SUITE_REG(snes_ppu)
