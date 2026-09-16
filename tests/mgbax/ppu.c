/*
 * mgbax PPU tests. Per the GBA specification: mode 3 = 16bpp bitmap at
 * VRAM 0, mode 4 = 8bpp paletted bitmap with two pages, mode 0 = text BGs,
 * sprites from OAM with 4bpp/8bpp tiles at VRAM 0x04000-0x07FFF (OBJ),
 * colors are BGR555 expanded to XRGB8888. Forced blank (DISPCNT bit 7)
 * shows the backdrop.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbax/gba.h"

#include <stdlib.h>
#include <string.h>

#define WHITE 0xFFFFFFFFu
#define BLACK 0xFF000000u
#define BLUE 0xFFFF0000u /* XRGB8888 macro layout: blue in bits 16-23 */
#define GREEN 0xFF00FF00u

static struct gba *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbax()->create(&c) != EMU_OK)
        return NULL;
    return (struct gba *)c;
}

static struct gba *mk_rom_core(void)
{
    struct gba *g = mk_core();
    if (g == NULL)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    if (emu_core_mgbax()->load_rom(&g->base, rom, rom_size) != EMU_OK) {
        free(rom);
        emu_core_mgbax()->destroy(&g->base);
        return NULL;
    }
    free(rom);
    return g;
}

static void ppu_mode3_bitmap(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* DISPCNT: mode 3 (0x0003) | BG2 enable (0x0400) */
    gba_io_write16(g, 0x04000000u, 0x0403u);
    /* pixel (0,0) white, (1,0) black-ish (color 0 -> backdrop) */
    gba_mem_write16(g, 0x06000000u, 0x7FFFu);
    gba_mem_write16(g, 0x06000002u, 0x0000u);
    gba_ppu_render_line(g, 0);
    T_CHECK_EQ(g->ppu.fb[0], WHITE);
    T_CHECK_EQ(g->ppu.fb[1], BLACK); /* backdrop entry 0 */
    /* 5-bit -> 8-bit expansion: 0x400 = B=16 -> (16<<3)|(16>>2) = 132 */
    gba_io_write16(g, 0x04000000u, 0x0403u);
    gba_mem_write16(g, 0x06000004u, 0x7C00u);
    gba_ppu_render_line(g, 0);
    T_CHECK_EQ(g->ppu.fb[2], BLUE);
    emu_core_mgbax()->destroy(&g->base);
}

static void ppu_mode4_pages(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_mem_write16(g, 0x05000002u, 0x7FFFu); /* pal[1] white */
    gba_mem_write16(g, 0x05000004u, 0x03E0u); /* pal[2] green */
    /* page A pixel (2,0) = 1 */
    gba_mem_write8(g, 0x06000002u, 1u);
    /* DISPCNT: mode 4 | BG2 */
    gba_io_write16(g, 0x04000000u, 0x0404u);
    gba_ppu_render_line(g, 0);
    T_CHECK_EQ(g->ppu.fb[2], WHITE);
    /* page flip (bit 4): page B pixel (2,0) = 2 */
    gba_mem_write8(g, 0x0600A002u, 2u);
    gba_io_write16(g, 0x04000000u, 0x0414u);
    gba_ppu_render_line(g, 0);
    T_CHECK_EQ(g->ppu.fb[2], GREEN);
    /* unset pixel -> backdrop */
    T_CHECK_EQ(g->ppu.fb[3], BLACK);
    emu_core_mgbax()->destroy(&g->base);
}

static void ppu_mode0_text_bg(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_mem_write16(g, 0x05000002u, 0x7FFFu); /* pal[1] white */
    /* BG0CNT: char base 0, map base 16 (0x8000), 4bpp, size 0 */
    gba_io_write16(g, 0x04000008u, 0x1000u);
    /* map entry 0: tile 1, palette 0, no flips */
    gba_mem_write16(g, 0x06008000u, 0x0001u);
    /* tile 1 row 0: pixel 0 = color 1 (4bpp low nibble) */
    gba_mem_write8(g, 0x06000020u, 0x01u);
    /* DISPCNT: mode 0 | BG0 */
    gba_io_write16(g, 0x04000000u, 0x0100u);
    gba_ppu_render_line(g, 0);
    T_CHECK_EQ(g->ppu.fb[0], WHITE);
    T_CHECK_EQ(g->ppu.fb[1], BLACK); /* pixel 1 = color 0: transparent */
    /* horizontal scroll by a full tile: pixel 0 now samples map entry 1
     * (tile 0 = all transparent -> backdrop) */
    gba_io_write16(g, 0x04000010u, 8u); /* BG0HOFS = 8 */
    gba_ppu_render_line(g, 0);
    T_CHECK_EQ(g->ppu.fb[0], BLACK);
    emu_core_mgbax()->destroy(&g->base);
}

static void ppu_sprite_4bpp(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* OAM entry 0: y=0, square shape, x=4, size 0 (8x8), tile 0, 4bpp,
     * palette bank 0 */
    gba_mem_write16(g, 0x07000000u, 0x0000u); /* attr0: y=0, shape 0 */
    gba_mem_write16(g, 0x07000002u, 0x0004u); /* attr1: x=4 */
    gba_mem_write16(g, 0x07000004u, 0x0000u); /* attr2: tile 0, pal 0 */
    /* sprite tile 0 row 0 pixel 0 = color 1 (OBJ tile 0 at VRAM 0x10000) */
    gba_mem_write8(g, 0x06010000u, 0x01u);
    gba_mem_write16(g, 0x05000202u, 0x03E0u); /* OBJ palette entry 257 */
    /* park unused sprites off-screen (zeroed OAM = on-screen at (0,0)) */
    for (int i = 1; i < 128; i++)
        g->mem.oam[(size_t)i * 8u] = 0xA0u; /* y = 160 */
    /* DISPCNT: mode 0 | BG0 | OBJ enable */
    gba_io_write16(g, 0x04000000u, 0x1100u);
    gba_ppu_render_line(g, 0);
    T_CHECK_EQ(g->ppu.fb[4], GREEN); /* x=4 */
    T_CHECK_EQ(g->ppu.fb[0], BLACK); /* outside sprite */
    T_CHECK_EQ(g->ppu.fb[5], BLACK); /* pixel 1 = color 0: transparent */
    emu_core_mgbax()->destroy(&g->base);
}

static void ppu_forced_blank(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_mem_write16(g, 0x05000000u, 0x7C00u); /* backdrop blue */
    gba_mem_write16(g, 0x06000000u, 0x7FFFu); /* mode 3 white pixel */
    gba_io_write16(g, 0x04000000u, 0x0183u); /* mode 3 | BG2 | blank */
    gba_ppu_render_line(g, 1);
    T_CHECK_EQ(g->ppu.fb[240 + 0], BLUE); /* white pixel ignored */
    T_CHECK_EQ(g->ppu.fb[240 + 1], BLUE);
    emu_core_mgbax()->destroy(&g->base);
}

static void ppu_vcount_dispstat(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->ppu.vcount = 5;
    T_CHECK_EQ(gba_io_read16(g, 0x04000006u), 5u);
    T_CHECK_EQ(gba_io_read16(g, 0x04000004u) & 1u, 0u); /* not vblank */
    g->ppu.vcount = 160;
    T_CHECK_EQ(gba_io_read16(g, 0x04000004u) & 1u, 1u); /* vblank flag */
    emu_core_mgbax()->destroy(&g->base);
}

/* ---- frame determinism + audio ------------------------------------------------- */

#define DET_AUDIO_CAP 8192
static int16_t det_audio[DET_AUDIO_CAP];
static size_t det_audio_len;
static int det_audio_frames;

static void det_audio_cb(void *user, const int16_t *samples, size_t count)
{
    (void)user;
    det_audio_frames++;
    for (size_t i = 0; i < count && det_audio_len < DET_AUDIO_CAP; i++)
        det_audio[det_audio_len++] = samples[i];
}

static struct gba *mk_det_core(void)
{
    struct gba *g = mk_rom_core();
    if (g == NULL)
        return NULL;
    /* square 1: duty 3, envelope volume 7, raw freq 0x400, restart */
    gba_io_write16(g, 0x04000060u, 0xF080u); /* duty 2, env vol 7 */
    gba_io_write16(g, 0x04000064u, 0x8400u);
    gba_io_write16(g, 0x04000088u, 0x0800u); /* bias */
    /* mode 3 bitmap, BG2 on */
    gba_io_write16(g, 0x04000000u, 0x0403u);
    /* timer 0: prescale 1024, enabled, IRQ on overflow */
    gba_io_write16(g, 0x04000100u, 0xFF00u);
    gba_io_write16(g, 0x04000102u, 0x00C0u | 0x0080u);
    g->mem.ie = 0x0008u; /* timer 0 overflow IRQ enabled */
    return g;
}

static void ppu_frame_determinism_audio(void)
{
    /* Two cores fed identical register setup: N run_frame calls must
     * produce identical framebuffer CRCs and deterministic, correctly
     * sized stereo audio. The timer IRQ fires through the HLE dispatcher
     * with no handler installed, which must leave the CPU running. */
    struct gba *g1 = mk_det_core();
    struct gba *g2 = mk_det_core();
    T_CHECK(g1 != NULL && g2 != NULL);
    if (!g1 || !g2) {
        if (g1)
            emu_core_mgbax()->destroy(&g1->base);
        if (g2)
            emu_core_mgbax()->destroy(&g2->base);
        return;
    }
    det_audio_len = 0;
    det_audio_frames = 0;
    emu_core_mgbax()->set_audio_callback(&g1->base, det_audio_cb, NULL);
    emu_core_mgbax()->set_audio_callback(&g2->base, det_audio_cb, NULL);

    for (int f = 0; f < 3; f++) {
        T_CHECK_EQ(emu_core_mgbax()->run_frame(&g1->base), EMU_OK);
        T_CHECK_EQ(emu_core_mgbax()->run_frame(&g2->base), EMU_OK);
        uint32_t c1 = emu_crc32(emu_core_mgbax()->framebuffer(&g1->base, NULL, NULL),
                                GBA_SCREEN_W * GBA_SCREEN_H);
        uint32_t c2 = emu_crc32(emu_core_mgbax()->framebuffer(&g2->base, NULL, NULL),
                                GBA_SCREEN_W * GBA_SCREEN_H);
        T_CHECK_EQ(c1, c2);
    }
    /* audio: 3 frames x ~549 stereo samples, non-zero (PSG active) */
    T_CHECK(det_audio_frames == 6); /* both cores deliver */
    T_CHECK(det_audio_len > 2048);
    T_CHECK(det_audio_len % 2 == 0); /* stereo pairs */
    {
        int nonzero = 0;
        int16_t lo = 0x7FFF, hi = -0x8000;
        for (size_t i = 0; i < det_audio_len; i++) {
            if (det_audio[i] != 0)
                nonzero++;
            if (det_audio[i] < lo)
                lo = det_audio[i];
            if (det_audio[i] > hi)
                hi = det_audio[i];
        }
        T_CHECK(nonzero > 0);
        T_CHECK(lo >= -8192); /* volume 7 x 1024 gain x 1 channel */
        T_CHECK(hi <= 8191);
    }
    /* Determinism: g1 and g2 are state-identical and ran the same frames,
     * so their interleaved streams must be byte-identical halves. */
    {
        size_t ref_len = det_audio_len / 2; /* one core's worth */
        T_CHECK(memcmp(det_audio + ref_len, ref_len + det_audio,
                       ref_len * sizeof det_audio[0]) == 0);
    }
    emu_core_mgbax()->destroy(&g1->base);
    emu_core_mgbax()->destroy(&g2->base);
}

T_SUITE_BEGIN(gba_ppu)
{ "mode3_bitmap", ppu_mode3_bitmap },
{ "mode4_pages", ppu_mode4_pages },
{ "mode0_text_bg", ppu_mode0_text_bg },
{ "sprite_4bpp", ppu_sprite_4bpp },
{ "forced_blank", ppu_forced_blank },
{ "vcount_dispstat", ppu_vcount_dispstat },
{ "frame_determinism_audio", ppu_frame_determinism_audio },
T_SUITE_END

T_SUITE_REG(gba_ppu)
