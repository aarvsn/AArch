/*
 * COMMON suite: save-state contract for every enabled core.
 *
 * Contract (see src/common/util.h):
 *  - state_size() is stable and equals the exact byte count save_state() writes
 *  - saving is deterministic (two saves produce identical blobs)
 *  - undersized buffer => EMU_ENOSPACE, core remains functional afterwards
 *  - NULL buffer => EMU_EINVAL
 *  - round-trip: fresh core + load_state(S7) + 1 frame == reference core
 *    after 8 frames (compared by framebuffer CRC)
 *  - truncated blob => EMU_EBADSTATE
 *  - repeated save/load cycles never diverge from a plain run
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"

#include <stdlib.h>
#include <string.h>

typedef const emu_core_vtable_t *(*core_lookup_fn)(void);
typedef uint8_t *(*rom_maker_fn)(size_t *size_out);

/* adapters: common suite uses default ROM configurations per system */
#if EMU_BUILD_MGBX
#include "../testutil.h"
#include "mgbx/mgbx.h"
static uint8_t *rom_gb(size_t *sz) { return gb_make_rom(4, GB_CART_ROM_ONLY, 0, sz); }
#endif
#if EMU_BUILD_BEATLE_NES_REDUX
#include "../testutil.h"
static uint8_t *rom_nes(size_t *sz) { return nes_make_rom(2, 1, 0, 0, sz); }
#endif
#if EMU_BUILD_SUPERSNES
#include "../testutil.h"
static uint8_t *rom_snes(size_t *sz) { return snes_make_lorom(16, sz); }
#endif
#if EMU_BUILD_MGBAX
#include "../testutil.h"
static uint8_t *rom_gba(size_t *sz) { return gba_make_rom(0x1000, 0x00, sz); }
#endif

static uint32_t fb_crc(emu_core_t *c)
{
    uint32_t w = 0, h = 0;
    const uint32_t *fb = c->vtable->framebuffer(c, &w, &h);
    return emu_crc32(fb, (size_t)w * h);
}

/* Loads the synthetic ROM and runs `frames` frames; aborts suite on failure. */
static emu_core_t *make_core(core_lookup_fn lookup, rom_maker_fn make_rom,
                             uint32_t frames)
{
    emu_core_t *c = NULL;
    if (lookup()->create(&c) != EMU_OK)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = make_rom(&rom_size);
    emu_result_t r = lookup()->load_rom(c, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        c->vtable->destroy(c);
        return NULL;
    }
    for (uint32_t i = 0; i < frames; i++) {
        if (c->vtable->run_frame(c) != EMU_OK) {
            c->vtable->destroy(c);
            return NULL;
        }
    }
    return c;
}

static void t_size_stable(core_lookup_fn lookup, rom_maker_fn make_rom)
{
    emu_core_t *c = make_core(lookup, make_rom, 3);
    T_CHECK(c != NULL);
    if (c == NULL)
        return;
    size_t sz = c->vtable->state_size(c);
    T_CHECK(sz > 0);
    T_CHECK_EQ_U(c->vtable->state_size(c), sz);

    uint8_t *a = malloc(sz);
    uint8_t *b = malloc(sz);
    T_CHECK(a && b);
    if (a && b) {
        T_CHECK_EQ(c->vtable->save_state(c, a, sz), EMU_OK);
        T_CHECK_EQ(c->vtable->save_state(c, b, sz), EMU_OK);
        T_CHECK(memcmp(a, b, sz) == 0); /* deterministic blob */
    }
    free(a);
    free(b);
    c->vtable->destroy(c);
}

static void t_undersized(core_lookup_fn lookup, rom_maker_fn make_rom)
{
    emu_core_t *c = make_core(lookup, make_rom, 3);
    T_CHECK(c != NULL);
    if (c == NULL)
        return;
    size_t sz = c->vtable->state_size(c);
    uint8_t *buf = malloc(sz);
    T_CHECK(buf != NULL);
    if (buf) {
        T_CHECK_EQ(c->vtable->save_state(c, buf, sz - 1), EMU_ENOSPACE);
        T_CHECK_EQ(c->vtable->save_state(c, buf, 0), EMU_ENOSPACE);
        /* core must remain fully functional after failed saves */
        T_CHECK_EQ(c->vtable->run_frame(c), EMU_OK);
        T_CHECK_EQ(c->vtable->save_state(c, buf, sz), EMU_OK);
    }
    free(buf);
    c->vtable->destroy(c);
}

static void t_null_buf(core_lookup_fn lookup, rom_maker_fn make_rom)
{
    emu_core_t *c = make_core(lookup, make_rom, 1);
    T_CHECK(c != NULL);
    if (c == NULL)
        return;
    T_CHECK_EQ(c->vtable->save_state(c, NULL, 4096), EMU_EINVAL);
    c->vtable->destroy(c);
}

static void t_roundtrip(core_lookup_fn lookup, rom_maker_fn make_rom)
{
    /* reference: run 7 frames, save, run 1 more frame */
    emu_core_t *ref = make_core(lookup, make_rom, 7);
    T_CHECK(ref != NULL);
    if (ref == NULL)
        return;
    size_t sz = ref->vtable->state_size(ref);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    T_CHECK_EQ(ref->vtable->save_state(ref, blob, sz), EMU_OK);
    T_CHECK_EQ(ref->vtable->run_frame(ref), EMU_OK);
    uint32_t want_crc = fb_crc(ref);

    /* roundtrip: fresh core, load blob, run 1 frame */
    emu_core_t *c = make_core(lookup, make_rom, 0);
    T_CHECK(c != NULL);
    if (c != NULL) {
        T_CHECK_EQ(c->vtable->load_state(c, blob, sz), EMU_OK);
        T_CHECK_EQ(c->vtable->run_frame(c), EMU_OK);
        T_CHECK_EQ_U(fb_crc(c), want_crc);

        /* truncated blob rejected */
        T_CHECK_EQ(c->vtable->load_state(c, blob, sz - 1), EMU_EBADSTATE);
        /* corrupted magic rejected */
        blob[0] ^= 0xFFu;
        T_CHECK_EQ(c->vtable->load_state(c, blob, sz), EMU_EBADSTATE);
    }
    free(blob);
    ref->vtable->destroy(ref);
    if (c)
        c->vtable->destroy(c);
}

static void t_repeated_cycles(core_lookup_fn lookup, rom_maker_fn make_rom)
{
    /* core A: 10 plain frames; core B: save/load cycle between every frame */
    emu_core_t *a = make_core(lookup, make_rom, 10);
    emu_core_t *b = make_core(lookup, make_rom, 0);
    T_CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        if (a) a->vtable->destroy(a);
        if (b) b->vtable->destroy(b);
        return;
    }
    size_t sz = b->vtable->state_size(b);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob) {
        for (int i = 0; i < 10; i++) {
            T_CHECK_EQ(a->vtable->run_frame(a), EMU_OK);
            T_CHECK_EQ(b->vtable->save_state(b, blob, sz), EMU_OK);
            T_CHECK_EQ(b->vtable->load_state(b, blob, sz), EMU_OK);
            T_CHECK_EQ(b->vtable->run_frame(b), EMU_OK);
        }
        T_CHECK_EQ_U(fb_crc(b), fb_crc(a));
    }
    free(blob);
    a->vtable->destroy(a);
    b->vtable->destroy(b);
}

#define DEFINE_STATE_SUITE(suite, lookup, maker)                              \
    static void st_##suite##_size(void)                                       \
    { t_size_stable(lookup, maker); }                                         \
    static void st_##suite##_undersized(void)                                 \
    { t_undersized(lookup, maker); }                                          \
    static void st_##suite##_null(void)                                       \
    { t_null_buf(lookup, maker); }                                            \
    static void st_##suite##_roundtrip(void)                                  \
    { t_roundtrip(lookup, maker); }                                           \
    static void st_##suite##_cycles(void)                                     \
    { t_repeated_cycles(lookup, maker); }                                     \
    T_SUITE_BEGIN(st_##suite)                                                 \
    { "size_stable_and_deterministic", st_##suite##_size },                   \
    { "undersized_rejected", st_##suite##_undersized },                       \
    { "null_buf_rejected", st_##suite##_null },                               \
    { "roundtrip_matches_reference", st_##suite##_roundtrip },                \
    { "repeated_cycles_stable", st_##suite##_cycles },                        \
    T_SUITE_END

#if EMU_BUILD_MGBX
DEFINE_STATE_SUITE(mgbx, emu_core_mgbx, rom_gb)
void t_register_st_mgbx(void)
{
    static const t_suite s = { "st_mgbx", st_mgbx_tests,
                               sizeof st_mgbx_tests / sizeof st_mgbx_tests[0] };
    t_add_suite(&s);
}
#endif

#if EMU_BUILD_BEATLE_NES_REDUX
DEFINE_STATE_SUITE(nes, emu_core_beatle_nes_redux, rom_nes)
void t_register_st_nes(void)
{
    static const t_suite s = { "st_nes", st_nes_tests,
                               sizeof st_nes_tests / sizeof st_nes_tests[0] };
    t_add_suite(&s);
}
#endif

#if EMU_BUILD_SUPERSNES
DEFINE_STATE_SUITE(snes, emu_core_supersnes, rom_snes)
void t_register_st_snes(void)
{
    static const t_suite s = { "st_snes", st_snes_tests,
                               sizeof st_snes_tests / sizeof st_snes_tests[0] };
    t_add_suite(&s);
}
#endif

#if EMU_BUILD_MGBAX
DEFINE_STATE_SUITE(gba, emu_core_mgbax, rom_gba)
void t_register_st_gba(void)
{
    static const t_suite s = { "st_gba", st_gba_tests,
                               sizeof st_gba_tests / sizeof st_gba_tests[0] };
    t_add_suite(&s);
}
#endif

void t_register_state(void)
{
#if EMU_BUILD_MGBX
    t_register_st_mgbx();
#endif
#if EMU_BUILD_BEATLE_NES_REDUX
    t_register_st_nes();
#endif
#if EMU_BUILD_SUPERSNES
    t_register_st_snes();
#endif
#if EMU_BUILD_MGBAX
    t_register_st_gba();
#endif
}
