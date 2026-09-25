/*
 * supercastpro: Tile Accelerator subset tests.
 *
 * Expected values derive from the documented TA model in
 * src/supercastpro/dc.h: parameter word layout, the screen mapping
 * sx = x + 320 / sy = 240 - y, the pixel-center rasterization rule and
 * the FB_W_* pixel pack byte orders - never from emulator internals.
 */
#include "../tests.h"
#include "../../src/supercastpro/dc.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/common/util.h"

/* ---- shared boot harness (same documented boot model as dc/dc.c) ----- */

static struct dc *ta_boot(void)
{
    /* self loop: bra self; nop */
    static const uint16_t prog[] = { 0xAFFEu, 0x0009u };
    size_t boot_off = 0x10000u;
    size_t total = boot_off + sizeof prog;
    uint8_t *img = calloc(1, total);
    if (img == NULL)
        return NULL;
    memcpy(img, "SEGA SEGAKATANA", 15);
    emu_store_le32(img + 0x300u, (uint32_t)(boot_off / 2048u));
    emu_store_le32(img + 0x308u, (uint32_t)sizeof prog);
    for (size_t i = 0; i < sizeof prog / 2; i++) {
        img[boot_off + i * 2] = (uint8_t)prog[i];
        img[boot_off + i * 2 + 1] = (uint8_t)(prog[i] >> 8);
    }
    emu_core_t *c = NULL;
    if (emu_core_supercastpro()->create(&c) != EMU_OK ||
        emu_core_supercastpro()->load_rom(c, img, total) != EMU_OK) {
        free(img);
        if (c != NULL)
            emu_core_supercastpro()->destroy(c);
        return NULL;
    }
    free(img);
    return (struct dc *)c;
}

static void ta_free(struct dc *d)
{
    emu_core_supercastpro()->destroy(&d->base);
}

/* Parameter stream builders (documented word layout). */
static void ta_word(struct dc *d, uint32_t w)
{
    dc_write32(d, DC_TA_BASE, w);
}

static void ta_vertex(struct dc *d, float x, float y, uint32_t color)
{
    union { float f; uint32_t u; } fx, fy, fz;
    fx.f = x;
    fy.f = y;
    fz.f = 0.5f;
    ta_word(d, 0xE0000000u); /* vertex tag (flags unmodeled) */
    ta_word(d, fx.u);        /* X */
    ta_word(d, fy.u);        /* Y */
    ta_word(d, fz.u);        /* Z (ignored by the subset) */
    ta_word(d, color);       /* ARGB8888 color bits */
}

static void ta_fb_w_rgb565(struct dc *d)
{
    /* pack 1 = RGB565, target at VRAM 0, stride 0 = w*bpp/4 */
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_W_CTRL, 0x00000001u);
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_W_SOF1, 0u);
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_W_LINESTRIDE, 0u);
}

static void ta_submit(struct dc *d)
{
    dc_write32(d, DC_PVR_BASE + DC_PVR_STARTRENDER, 1u);
}

static uint16_t ta_px565(struct dc *d, uint32_t x, uint32_t y)
{
    uint32_t off = (y * 640u + x) * 2u;
    return (uint16_t)(d->vram[off] | (d->vram[off + 1] << 8));
}

static void ta_start_opaque_poly0(struct dc *d)
{
    /* opaque polygon type 0: control + ISP + TSP = 3 words */
    ta_word(d, 0x88000000u);
    ta_word(d, 0x00000000u); /* ISP */
    ta_word(d, 0x00000000u); /* TSP */
}

/* ---- tests ----------------------------------------------------------------------------- */

static void ta_triangle_flat_opaque(void)
{
    struct dc *d = ta_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    ta_fb_w_rgb565(d);
    /* triangle (0,0) (100,0) (0,100) in TA coords -> screen
     * (320,240) (420,240) (320,140); green */
    ta_start_opaque_poly0(d);
    ta_vertex(d, 0.0f, 0.0f, 0xFF00FF00u);
    ta_vertex(d, 100.0f, 0.0f, 0xFF00FF00u);
    ta_vertex(d, 0.0f, 100.0f, 0xFF00FF00u);
    ta_submit(d);

    /* inside: (330,220): dx=10, dy=20 -> 30 < 100 */
    T_CHECK_EQ_U(ta_px565(d, 330, 220), 0x07E0u);
    /* outside right: dx = 130 */
    T_CHECK_EQ_U(ta_px565(d, 450, 220), 0x0000u);
    /* outside left of the x=320 edge */
    T_CHECK_EQ_U(ta_px565(d, 310, 220), 0x0000u);
    /* FIFO consumed by the pass */
    T_CHECK_EQ_U(d->ta.count, 0u);
    ta_free(d);
}

static void ta_strip_makes_two_triangles(void)
{
    struct dc *d = ta_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    ta_fb_w_rgb565(d);
    /* strip (0,0) (100,0) (0,100) (100,100): triangles cover both
     * halves of the square (320,240)-(420,140); red */
    ta_start_opaque_poly0(d);
    ta_vertex(d, 0.0f, 0.0f, 0xFFFF0000u);
    ta_vertex(d, 100.0f, 0.0f, 0xFFFF0000u);
    ta_vertex(d, 0.0f, 100.0f, 0xFFFF0000u);
    ta_vertex(d, 100.0f, 100.0f, 0xFFFF0000u);
    ta_submit(d);
    /* lower-left half (first triangle) */
    T_CHECK_EQ_U(ta_px565(d, 340, 230), 0xF800u);
    /* upper-right half (second triangle from the strip) */
    T_CHECK_EQ_U(ta_px565(d, 400, 150), 0xF800u);
    ta_free(d);
}

static void ta_sprite_axis_aligned(void)
{
    struct dc *d = ta_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    ta_fb_w_rgb565(d);
    /* opaque sprite: A base (-100,-50), B x edge (+100), C y edge
     * (+100) -> screen rect x [220,420), y [190,290); cyan */
    ta_word(d, 0x8D000000u);
    ta_word(d, 0u);
    ta_word(d, 0u);
    ta_vertex(d, -100.0f, -50.0f, 0xFF00FFFFu);
    ta_vertex(d, 100.0f, -50.0f, 0xFF00FFFFu);
    ta_vertex(d, -100.0f, 50.0f, 0xFF00FFFFu);
    ta_submit(d);
    T_CHECK_EQ_U(ta_px565(d, 220, 190), 0x07FFu);
    T_CHECK_EQ_U(ta_px565(d, 419, 289), 0x07FFu);
    /* half-open edges */
    T_CHECK_EQ_U(ta_px565(d, 420, 190), 0x0000u);
    T_CHECK_EQ_U(ta_px565(d, 219, 190), 0x0000u);
    T_CHECK_EQ_U(ta_px565(d, 220, 189), 0x0000u);
    ta_free(d);
}

static void ta_transparent_over_opaque(void)
{
    struct dc *d = ta_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    ta_fb_w_rgb565(d);
    /* KOS submission contract: opaque first, then transparent */
    ta_start_opaque_poly0(d);
    ta_vertex(d, 0.0f, 0.0f, 0xFF00FF00u);
    ta_vertex(d, 100.0f, 0.0f, 0xFF00FF00u);
    ta_vertex(d, 0.0f, 100.0f, 0xFF00FF00u);
    /* transparent sprite on top (0x95: header = 4 words) */
    ta_word(d, 0x95000000u);
    ta_word(d, 0u);
    ta_word(d, 0u);
    ta_word(d, 0u);
    ta_vertex(d, -50.0f, -20.0f, 0xFFFF0000u);
    ta_vertex(d, 50.0f, -20.0f, 0xFFFF0000u);
    ta_vertex(d, -50.0f, 20.0f, 0xFFFF0000u);
    ta_submit(d);
    /* overlap: transparent red wins by stream order */
    T_CHECK_EQ_U(ta_px565(d, 330, 220), 0xF800u);
    /* opaque-only region stays green */
    T_CHECK_EQ_U(ta_px565(d, 395, 235), 0x07E0u);
    ta_free(d);
}

static void ta_end_word_terminates(void)
{
    struct dc *d = ta_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    ta_fb_w_rgb565(d);
    ta_start_opaque_poly0(d);
    ta_vertex(d, 0.0f, 0.0f, 0xFF00FF00u);
    ta_vertex(d, 100.0f, 0.0f, 0xFF00FF00u);
    ta_vertex(d, 0.0f, 100.0f, 0xFF00FF00u);
    ta_word(d, 0x83000000u); /* end of parameter list */
    /* trailing words after the end marker must not be drawn */
    ta_start_opaque_poly0(d);
    ta_vertex(d, 0.0f, 0.0f, 0xFFFF0000u);
    ta_vertex(d, 100.0f, 0.0f, 0xFFFF0000u);
    ta_vertex(d, 0.0f, 100.0f, 0xFFFF0000u);
    ta_submit(d);
    T_CHECK_EQ_U(ta_px565(d, 330, 220), 0x07E0u);
    T_CHECK_EQ_U(ta_px565(d, 310, 220), 0x0000u);
    ta_free(d);
}

static void ta_fifo_overflow_flag(void)
{
    struct dc *d = ta_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    for (uint32_t i = 0; i < DC_TA_FIFO_WORDS + 10u; i++)
        ta_word(d, 0x83000000u);
    T_CHECK_EQ_U(d->ta.count, DC_TA_FIFO_WORDS);
    T_CHECK_EQ_U(d->ta.overflow, 1u);
    ta_submit(d); /* end word at index 0: parse stops, FIFO cleared */
    T_CHECK_EQ_U(d->ta.count, 0u);
    ta_free(d);
}

static void ta_argb8888_target_bytes(void)
{
    struct dc *d = ta_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* pack 4 = ARGB8888 target at VRAM 0x1000, stride 0. The stride
     * register (0x42) is written before SOF1 (0x44): a 32-bit store at
     * the unaligned stride offset spills into the SOF1 byte under the
     * raw register-file model. */
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_W_CTRL, 0x00000004u);
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_W_LINESTRIDE, 0u);
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_W_SOF1, 0x1000u);
    ta_word(d, 0x8D000000u);
    ta_word(d, 0u);
    ta_word(d, 0u);
    ta_vertex(d, -100.0f, -50.0f, 0x12345678u);
    ta_vertex(d, 100.0f, -50.0f, 0x12345678u);
    ta_vertex(d, -100.0f, 50.0f, 0x12345678u);
    ta_submit(d);
    /* byte order mirrors the scanout decode: B,G,R,A */
    uint32_t off = 0x1000u + (190u * 640u + 220u) * 4u;
    T_CHECK_EQ_U(d->vram[off + 0], 0x78u);
    T_CHECK_EQ_U(d->vram[off + 1], 0x56u);
    T_CHECK_EQ_U(d->vram[off + 2], 0x34u);
    T_CHECK_EQ_U(d->vram[off + 3], 0x12u);
    ta_free(d);
}

static void ta_state_roundtrip_with_pending_fifo(void)
{
    struct dc *ref = ta_boot();
    struct dc *cmp = ta_boot();
    if (ref == NULL || cmp == NULL) {
        T_FAIL("boot failed");
        return;
    }
    ta_fb_w_rgb565(ref);
    ta_fb_w_rgb565(cmp);
    ta_start_opaque_poly0(ref);
    ta_vertex(ref, 0.0f, 0.0f, 0xFF00FF00u);
    ta_vertex(ref, 100.0f, 0.0f, 0xFF00FF00u);
    ta_vertex(ref, 0.0f, 100.0f, 0xFF00FF00u);
    /* FIFO content pending; move it through a save state */
    size_t sz = emu_core_supercastpro()->state_size(&ref->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob != NULL) {
        T_CHECK_EQ(emu_core_supercastpro()->save_state(&ref->base, blob, sz),
                   EMU_OK);
        T_CHECK_EQ(emu_core_supercastpro()->load_state(&cmp->base, blob, sz),
                   EMU_OK);
        free(blob);
        T_CHECK_EQ_U(cmp->ta.count, 18u); /* 3 header + 3*5 vertex words */
        ta_submit(cmp);
        T_CHECK_EQ_U(ta_px565(cmp, 330, 220), 0x07E0u);
        T_CHECK_EQ_U(ta_px565(cmp, 450, 220), 0x0000u);
    }
    ta_free(ref);
    ta_free(cmp);
}

static void ta_scanout_shows_ta_output(void)
{
    struct dc *d = ta_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* write target and read scanout hit the same VRAM region */
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_R_CTRL, 0x00000000u); /* RGB565 */
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_R_SIZE,
               ((480u - 1u) << 10) | (640u / 4u - 1u));
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_R_SOF1, 0u);
    ta_fb_w_rgb565(d);
    ta_word(d, 0x8D000000u);
    ta_word(d, 0u);
    ta_word(d, 0u);
    ta_vertex(d, -100.0f, -50.0f, 0xFF00FFFFu);
    ta_vertex(d, 100.0f, -50.0f, 0xFF00FFFFu);
    ta_vertex(d, -100.0f, 50.0f, 0xFF00FFFFu);
    ta_submit(d);
    dc_render(d); /* scanout of the TA-written VRAM */
    T_CHECK_EQ_U(d->fb[190 * DC_SCREEN_W + 220], EMU_PIXEL(0, 255, 255));
    T_CHECK_EQ_U(d->fb[189 * DC_SCREEN_W + 220], 0xFF000000u);
    ta_free(d);
}

T_SUITE_BEGIN(dc_ta)
{ "ta_triangle_flat_opaque", ta_triangle_flat_opaque },
{ "ta_strip_makes_two_triangles", ta_strip_makes_two_triangles },
{ "ta_sprite_axis_aligned", ta_sprite_axis_aligned },
{ "ta_transparent_over_opaque", ta_transparent_over_opaque },
{ "ta_end_word_terminates", ta_end_word_terminates },
{ "ta_fifo_overflow_flag", ta_fifo_overflow_flag },
{ "ta_argb8888_target_bytes", ta_argb8888_target_bytes },
{ "ta_state_roundtrip_with_pending_fifo", ta_state_roundtrip_with_pending_fifo },
{ "ta_scanout_shows_ta_output", ta_scanout_shows_ta_output },
T_SUITE_END
T_SUITE_REG(dc_ta)
