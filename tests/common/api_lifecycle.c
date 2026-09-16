/*
 * COMMON suite: API lifecycle for every enabled core.
 *
 * Contract being tested (see include/emu/emu.h):
 *  - create/destroy without leaks (ASan verifies)
 *  - NULL/zero-size ROM rejected with EMU_EINVAL
 *  - run_frame without ROM returns EMU_ENOROM
 *  - framebuffer dimensions match the vtable
 *  - reset/set_input/set_audio_callback are safe at any time
 *  - load_state of a bogus blob returns EMU_EBADSTATE
 */
#include "../tests.h"
#include "emu/emu.h"

#include <string.h>

typedef const emu_core_vtable_t *(*core_lookup_fn)(void);

static void lifecycle_create_destroy(const emu_core_vtable_t *vt)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(vt->create(&c), EMU_OK);
    T_CHECK(c != NULL);
    T_CHECK_EQ(c->vtable, vt);
    vt->reset(c);
    vt->set_input(c, 0);
    vt->set_audio_callback(c, NULL, NULL);
    (void)vt->state_size(c);
    vt->destroy(c);
}

static void lifecycle_null_rom(const emu_core_vtable_t *vt)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(vt->create(&c), EMU_OK);
    T_CHECK_EQ(vt->load_rom(c, NULL, 100), EMU_EINVAL);
    static const uint8_t byte = 0;
    T_CHECK_EQ(vt->load_rom(c, &byte, 0), EMU_EINVAL);
    vt->destroy(c);
}

static void lifecycle_run_without_rom(const emu_core_vtable_t *vt)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(vt->create(&c), EMU_OK);
    T_CHECK_EQ(vt->run_frame(c), EMU_ENOROM);
    vt->destroy(c);
}

static void lifecycle_framebuffer_dims(const emu_core_vtable_t *vt)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(vt->create(&c), EMU_OK);
    uint32_t w = 999, h = 999;
    const uint32_t *fb = vt->framebuffer(c, &w, &h);
    T_CHECK(fb != NULL);
    T_CHECK_EQ_U(w, vt->fb_width);
    T_CHECK_EQ_U(h, vt->fb_height);
    vt->destroy(c);
}

static void lifecycle_bogus_state_without_rom(const emu_core_vtable_t *vt)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(vt->create(&c), EMU_OK);
    static const uint8_t garbage[16] = { 0 };
    T_CHECK_EQ(vt->load_state(c, garbage, sizeof garbage), EMU_EBADSTATE);
    T_CHECK_EQ(vt->load_state(c, NULL, 10), EMU_EINVAL);
    T_CHECK_EQ(vt->save_state(c, NULL, 16), EMU_EINVAL);
    vt->destroy(c);
}

static void lifecycle_vtable_consistency(const emu_core_vtable_t *vt)
{
    T_CHECK(vt->name != NULL && vt->name[0] != '\0');
    T_CHECK(vt->system != NULL);
    T_CHECK(vt->fb_width > 0 && vt->fb_width <= 512);
    T_CHECK(vt->fb_height > 0 && vt->fb_height <= 512);
    T_CHECK(vt->sample_rate >= 8000 && vt->sample_rate <= 192000);
    T_CHECK(vt->create && vt->destroy && vt->load_rom && vt->reset &&
            vt->run_frame && vt->framebuffer && vt->set_input &&
            vt->set_audio_callback && vt->state_size && vt->save_state &&
            vt->load_state);
}

/* Define one lifecycle suite per core. */
#define DEFINE_LIFECYCLE_SUITE(suite, lookup)                                 \
    static void lc_##suite##_create_destroy(void)                             \
    { lifecycle_create_destroy(lookup()); }                                   \
    static void lc_##suite##_null_rom(void)                                   \
    { lifecycle_null_rom(lookup()); }                                         \
    static void lc_##suite##_run_without_rom(void)                            \
    { lifecycle_run_without_rom(lookup()); }                                  \
    static void lc_##suite##_framebuffer_dims(void)                           \
    { lifecycle_framebuffer_dims(lookup()); }                                 \
    static void lc_##suite##_bogus_state(void)                                \
    { lifecycle_bogus_state_without_rom(lookup()); }                          \
    static void lc_##suite##_vtable(void)                                     \
    { lifecycle_vtable_consistency(lookup()); }                               \
    T_SUITE_BEGIN(lc_##suite)                                                 \
    { "create_destroy",   lc_##suite##_create_destroy },                      \
    { "null_rom_rejected", lc_##suite##_null_rom },                           \
    { "run_without_rom",  lc_##suite##_run_without_rom },                     \
    { "framebuffer_dims", lc_##suite##_framebuffer_dims },                    \
    { "bogus_state_rejected", lc_##suite##_bogus_state },                     \
    { "vtable_consistency", lc_##suite##_vtable },                            \
    T_SUITE_END

#if EMU_BUILD_MGBX
DEFINE_LIFECYCLE_SUITE(mgbx, emu_core_mgbx)
void t_register_lc_mgbx(void)
{
    static const t_suite s = { "lc_mgbx", lc_mgbx_tests,
                               sizeof lc_mgbx_tests / sizeof lc_mgbx_tests[0] };
    t_add_suite(&s);
}
#endif

#if EMU_BUILD_BEATLE_NES_REDUX
DEFINE_LIFECYCLE_SUITE(nes, emu_core_beatle_nes_redux)
void t_register_lc_nes(void)
{
    static const t_suite s = { "lc_nes", lc_nes_tests,
                               sizeof lc_nes_tests / sizeof lc_nes_tests[0] };
    t_add_suite(&s);
}
#endif

#if EMU_BUILD_SUPERSNES
DEFINE_LIFECYCLE_SUITE(snes, emu_core_supersnes)
void t_register_lc_snes(void)
{
    static const t_suite s = { "lc_snes", lc_snes_tests,
                               sizeof lc_snes_tests / sizeof lc_snes_tests[0] };
    t_add_suite(&s);
}
#endif

#if EMU_BUILD_MGBAX
DEFINE_LIFECYCLE_SUITE(gba, emu_core_mgbax)
void t_register_lc_gba(void)
{
    static const t_suite s = { "lc_gba", lc_gba_tests,
                               sizeof lc_gba_tests / sizeof lc_gba_tests[0] };
    t_add_suite(&s);
}
#endif

void t_register_common(void)
{
#if EMU_BUILD_MGBX
    t_register_lc_mgbx();
#endif
#if EMU_BUILD_BEATLE_NES_REDUX
    t_register_lc_nes();
#endif
#if EMU_BUILD_SUPERSNES
    t_register_lc_snes();
#endif
#if EMU_BUILD_MGBAX
    t_register_lc_gba();
#endif
}
