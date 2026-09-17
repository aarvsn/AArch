/*
 * finalburn save-state and core-contract tests: explicit serialization
 * round-trips, undersized-buffer rejection, foreign-blob rejection,
 * API lifecycle, bus behavior, and frame-level determinism.
 */
#include "tests.h"
#include "fbh.h"
#include "finalburn/fb_md.h"
#include "emu/emu.h"

#include <stdlib.h>
#include <string.h>

static void test_state_roundtrip(void)
{
    size_t size = 0;
    uint8_t *rom = psg_rom_shared(&size);
    struct fb_md *a = fb_boot(rom, size);
    struct fb_md *b = fb_boot(rom, size);
    free(rom);
    if (!a || !b) { T_FAIL("boot failed"); return; }

    fb_frame(a);
    size_t sz = emu_core_finalburn()->state_size(&a->base);
    T_CHECK(sz > 0);
    uint8_t *blob = malloc(sz);
    T_CHECK_EQ_U(emu_core_finalburn()->save_state(&a->base, blob, sz), EMU_OK);
    /* run ahead on A */
    fb_frame(a);
    fb_frame(a);
    /* restore B to the saved point */
    T_CHECK_EQ_U(emu_core_finalburn()->load_state(&b->base, blob, sz), EMU_OK);
    fb_frame(b);
    fb_frame(b);
    /* trajectories must match */
    uint32_t wa = 0, ha = 0, wb = 0, hb = 0;
    const uint32_t *fa = emu_core_finalburn()->framebuffer(&a->base, &wa, &ha);
    const uint32_t *fbb = emu_core_finalburn()->framebuffer(&b->base, &wb, &hb);
    T_CHECK_EQ_U(emu_crc32(fa, wa * ha), emu_crc32(fbb, wb * hb));
    /* audio streams must match too */
    T_CHECK_EQ_U(a->audio_count, b->audio_count);
    int same = memcmp(a->audio, b->audio, a->audio_count * sizeof(int16_t)) == 0;
    T_CHECK(same);
    free(blob);
    emu_core_destroy(&a->base);
    emu_core_destroy(&b->base);
}

static void test_state_undersized_and_foreign(void)
{
    size_t size = 0;
    uint8_t *rom = psg_rom_shared(&size);
    struct fb_md *a = fb_boot(rom, size);
    free(rom);
    if (!a) { T_FAIL("boot failed"); return; }
    fb_frame(a);
    size_t sz = emu_core_finalburn()->state_size(&a->base);
    uint8_t *blob = malloc(sz);
    /* undersized: rejected, nothing written */
    T_CHECK_EQ_U(emu_core_finalburn()->save_state(&a->base, blob, sz - 1),
                 EMU_ENOSPACE);
    /* foreign blob */
    blob[0] ^= 0xFF;
    T_CHECK_EQ_U(emu_core_finalburn()->load_state(&a->base, blob, sz),
                 EMU_EBADSTATE);
    free(blob);
    emu_core_destroy(&a->base);
}

static void test_api_lifecycle(void)
{
    const emu_core_vtable_t *vt = emu_core_finalburn();
    emu_core_t *c = NULL;
    T_CHECK_EQ_U(vt->create(&c), EMU_OK);
    /* run_frame without ROM */
    T_CHECK_EQ_U(vt->run_frame(c), EMU_ENOROM);
    /* load invalid ROM */
    T_CHECK_EQ_U(vt->load_rom(c, NULL, 0), EMU_EINVAL);
    /* a small zero-filled image is a valid (if useless) cart: accepted */
    static uint8_t junk[0x400];
    T_CHECK_EQ_U(vt->load_rom(c, junk, sizeof junk), EMU_OK);
    /* oversized images are rejected */
    T_CHECK_EQ_U(vt->load_rom(c, junk, 9u * 1024u * 1024u), EMU_EBADROM);
    /* reset without ROM is fine */
    vt->reset(c);
    /* framebuffer geometry */
    uint32_t w = 0, h = 0;
    vt->framebuffer(c, &w, &h);
    T_CHECK_EQ_U(w, 320);
    T_CHECK_EQ_U(h, 224);
    emu_core_destroy(c);
    emu_core_destroy(NULL); /* NULL-safe */
}

static void test_bus_ram_and_rom(void)
{
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    rom[0x1000] = 0xAB;
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    /* ROM read */
    T_CHECK_EQ_U(fb_md_68k_read8(md, 0x001000u), 0xAB);
    /* RAM write/read at $FF0000 */
    fb_md_68k_write8(md, 0xFF0000u, 0x5A);
    T_CHECK_EQ_U(fb_md_68k_read8(md, 0xFF0000u), 0x5A);
    /* RAM mirror at $FFFF00? E00000 region mirrors the same RAM */
    fb_md_68k_write8(md, 0xE00001u, 0xC3);
    T_CHECK_EQ_U(fb_md_68k_read8(md, 0xFF0001u), 0xC3);
    /* open bus beyond the cart */
    T_CHECK_EQ_U(fb_md_68k_read8(md, 0x900000u), 0xFF);
    emu_core_destroy(&md->base);
}

static void test_rom_mirroring(void)
{
    /* 64 KiB power-of-two cart: $10000 reads mirror $0 */
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    size = 0x10000;
    rom[0] = 0x77;
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(fb_md_68k_read8(md, 0x010000u), 0x77);
    T_CHECK_EQ_U(fb_md_68k_read8(md, 0x030000u), 0x77);
    emu_core_destroy(&md->base);
}

static void test_sram_header(void)
{
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    rom[0x1B0] = 'R';
    rom[0x1B1] = 'A';
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    fb_cart_sram_write8(&md->cart, 0x200001u, 0x42);
    T_CHECK_EQ_U(fb_cart_sram_read8(&md->cart, 0x200001u), 0x42);
    emu_core_destroy(&md->base);
}

static void test_input_masks(void)
{
    const emu_core_vtable_t *vt = emu_core_finalburn();
    emu_core_t *c = NULL;
    vt->create(&c);
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    fb_rom_w16(rom, 0x400, 0x60FE);
    vt->load_rom(c, rom, size);
    free(rom);
    /* Up pressed (bit 0) -> pad1 bit0 low (active-low) */
    vt->set_input(c, 0x01);
    struct fb_md *md = (struct fb_md *)c;
    T_CHECK_EQ_U(md->pad1 & 0x01u, 0);
    /* TH=1 view: low nibble = U/D/L/R */
    md->pad_th = 1;
    uint8_t v = fb_md_68k_read8(md, 0xA10003u);
    T_CHECK_EQ_U(v & 0x0Fu, 0x0Eu); /* Up active-low */
    T_CHECK_EQ_U(v & 0x30u, 0x30u);
    vt->destroy(c);
}

/* shared with state test: same PSG-programmed ROM as the audio suite */
uint8_t *psg_rom_shared(size_t *size);

T_SUITE_BEGIN(finalburn_state)
{ "state_roundtrip_video_audio", test_state_roundtrip },
{ "state_undersized_and_foreign", test_state_undersized_and_foreign },
{ "api_lifecycle", test_api_lifecycle },
{ "bus_ram_rom_openbus", test_bus_ram_and_rom },
{ "rom_mirroring", test_rom_mirroring },
{ "sram_header", test_sram_header },
{ "input_masks", test_input_masks },
T_SUITE_END
T_SUITE_REG(finalburn_state)
