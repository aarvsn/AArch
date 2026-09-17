/*
 * beatle-psx GPU tests. Expected values derive from the GPU specification:
 * 15-bit color layout (5-5-5), fill/copy semantics, drawing area/offset
 * clipping, mask bits, semi-transparency formulas and display conversion.
 * Commands are written through the public GP0/GP1 port path (white-box
 * state checks read VRAM directly).
 */
#include "../tests.h"
#include "../testutil.h"
#include "psxh.h"
#include "beatle-psx/psx.h"

#include <stdlib.h>
#include <string.h>

static void gp0(struct psx *p, uint32_t w)
{
    psx_gpu_write_gp0(&p->gpu, p, w);
}

static void gp1(struct psx *p, uint32_t w)
{
    psx_gpu_write_gp1(&p->gpu, p, w);
}

static uint16_t vram(const struct psx *p, uint32_t x, uint32_t y)
{
    return p->gpu.vram[y * PSX_VRAM_W + x];
}

static void psx_gpu_fill(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* fill 64x32 at (32,16) with pure red (0x0000FF -> 15bit 0x001F) */
    gp0(p, 0x020000FFu);
    gp0(p, (16u << 16) | 32u);
    gp0(p, (32u << 16) | 64u);
    T_CHECK_EQ(vram(p, 32, 16), 0x001Fu);
    T_CHECK_EQ(vram(p, 95, 47), 0x001Fu);
    T_CHECK_EQ(vram(p, 96, 16), 0u);       /* right edge excluded */
    T_CHECK_EQ(vram(p, 32, 48), 0u);       /* bottom edge excluded */
    T_CHECK_EQ(vram(p, 0, 0), 0u);         /* outside untouched */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gpu_mono_tri(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* opaque monochrome triangle (10,10) (50,10) (10,50), red */
    gp0(p, 0x200000FFu);       /* cmd 20h, color r=0xFF */
    gp0(p, (10u << 16) | 10u);
    gp0(p, (10u << 16) | 50u);
    gp0(p, (50u << 16) | 10u);
    T_CHECK_EQ(vram(p, 12, 12), 0x001Fu);  /* inside: 5-bit red 0x1F */
    T_CHECK_EQ(vram(p, 200, 200), 0u);     /* outside */
    T_CHECK_EQ(vram(p, 48, 48), 0u);       /* beyond hypotenuse x+y>60 */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gpu_offset_clip(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* drawing offset (+5,+3): 11-bit fields encode value = raw - 1024 */
    gp0(p, 0xE5000000u);
    gp0(p, (1027u << 11) | 1029u);
    /* 8x8 monochrome rect at (0,0) blue: lands at (5,3) */
    gp0(p, 0x70FF0000u);       /* cmd 70h, color b=0xFF */
    gp0(p, 0u);                /* coord only (size implied) */
    T_CHECK_EQ(vram(p, 5, 3), 0x7C00u);
    T_CHECK_EQ(vram(p, 12, 10), 0x7C00u);
    T_CHECK_EQ(vram(p, 0, 0), 0u);
    /* drawing area clip: restrict to x<=10,y<=10 then draw over the edge */
    gp0(p, 0xE3000000u);       /* area top-left (0,0) */
    gp0(p, 0u);
    gp0(p, 0xE4000000u);       /* area bottom-right (10,10) */
    gp0(p, (10u << 10) | 10u);
    gp0(p, 0x70FF0000u);       /* 8x8 rect at (8,8) -> would span to (15,15) */
    gp0(p, (8u << 16) | 8u);
    T_CHECK_EQ(vram(p, 10, 10), 0x7C00u);  /* clipped at area edge */
    T_CHECK_EQ(vram(p, 11, 11), 0u);       /* outside area not drawn */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gpu_mask(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* set mask bit when drawing + do not draw over masked */
    gp0(p, 0xE6000000u);       /* E6: mask settings */
    gp0(p, 3u);                /* bit0 set-mask, bit1 check */
    gp0(p, 0x700000FFu);       /* 8x8 red rect at (0,0) */
    gp0(p, 0u);
    T_CHECK((vram(p, 3, 3) & 0x8000u) != 0);
    T_CHECK_EQ(vram(p, 3, 3) & 0x7FFFu, 0x001Fu);
    /* green rect over the same area must be rejected */
    gp0(p, 0x7000FF00u);       /* green */
    gp0(p, 0u);
    T_CHECK_EQ(vram(p, 3, 3) & 0x7FFFu, 0x001Fu); /* unchanged (masked) */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gpu_semi_transparent(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* backdrop = 0x080808 gray -> 5-bit per channel = 1 -> VRAM 0x0421 */
    gp0(p, 0x02080808u);
    gp0(p, 0u);
    gp0(p, (16u << 16) | 16u);
    T_CHECK_EQ(vram(p, 0, 0), 0x0421u);
    /* semi-transparent rect mode 0 (B/2+F/2): B=(1,1,1), F=blue (0,0,31):
     * r = 1/2+0/2 = 0; g = 0; b = 1/2+31/2 = 0+15 = 15 -> 0x3C00 */
    gp0(p, 0xE1000000u);       /* E1: semi mode 0 (bits 5-6 = 0) */
    gp0(p, 0u);
    gp0(p, 0x62FF0000u);       /* cmd 62h: semi mono rect variable, blue */
    gp0(p, 0u);                /* at (0,0) */
    gp0(p, (8u << 16) | 8u);   /* size */
    T_CHECK_EQ(vram(p, 2, 2), 0x3C00u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gpu_vram_transfers(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* CPU -> VRAM: 2x1 pixels at (100, 50): red, blue */
    gp0(p, 0xA0000000u);
    gp0(p, (50u << 16) | 100u);
    gp0(p, (1u << 16) | 2u);   /* w=2 h=1 */
    gp0(p, 0x7C00001Fu);       /* halfwords: lo=0x001F (red), hi=0x7C00 */
    T_CHECK_EQ(vram(p, 100, 50), 0x001Fu);
    T_CHECK_EQ(vram(p, 101, 50), 0x7C00u);
    /* VRAM -> CPU: read back 2 halfwords from (100,50) */
    gp0(p, 0xC0000000u);
    gp0(p, (50u << 16) | 100u);
    gp0(p, (1u << 16) | 2u);
    T_CHECK(p->gpu.read_pending);
    uint32_t word = psx_gpu_read(&p->gpu);
    T_CHECK_EQ(word, 0x7C00001Fu);
    T_CHECK(!p->gpu.read_pending);
    /* VRAM -> VRAM: copy the 2 pixels to (200,60) */
    gp0(p, 0x80000000u);
    gp0(p, (50u << 16) | 100u);
    gp0(p, (60u << 16) | 200u);
    gp0(p, (1u << 16) | 2u);
    T_CHECK_EQ(vram(p, 200, 60), 0x001Fu);
    T_CHECK_EQ(vram(p, 201, 60), 0x7C00u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gpu_display_scanout(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* paint VRAM at (0,0) region red */
    gp0(p, 0x020000FFu);
    gp0(p, 0u);
    gp0(p, (240u << 16) | 320u);
    /* display area at (0,0), 320x240, enabled */
    gp1(p, 0x05000000u);       /* display address (0,0) */
    gp1(p, 0x06000000u | (320u << 12)); /* x1=0, x2=320 */
    gp1(p, 0x07000000u | (240u << 10)); /* y1=0, y2=240 */
    gp1(p, 0x03000000u);       /* display enable */
    uint32_t fb[PSX_FB_W * PSX_FB_H];
    psx_gpu_render(&p->gpu, fb);
    /* 5-bit red 0x1F converts to 8-bit 0xFF (with bit replication) */
    T_CHECK_EQ(fb[0] & 0x00FFFFFFu, 0x0000FFu);
    T_CHECK_EQ(fb[PSX_FB_W * PSX_FB_H - 1] & 0x00FFFFFFu, 0x0000FFu);
    /* display off -> black */
    gp1(p, 0x03000001u);
    psx_gpu_render(&p->gpu, fb);
    T_CHECK_EQ(fb[0], 0u);
    emu_core_beatle_psx()->destroy(&p->base);
}

T_SUITE_BEGIN(beatle_psx_gpu)
{ "fill", psx_gpu_fill },
{ "mono_tri", psx_gpu_mono_tri },
{ "offset_clip", psx_gpu_offset_clip },
{ "mask", psx_gpu_mask },
{ "semi_transparent", psx_gpu_semi_transparent },
{ "vram_transfers", psx_gpu_vram_transfers },
{ "display_scanout", psx_gpu_display_scanout },
T_SUITE_END

T_SUITE_REG(beatle_psx_gpu)
