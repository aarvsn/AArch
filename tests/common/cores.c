/*
 * Skeleton-core contract tests: skeletons validate formats, reject bad
 * ones, and refuse to emulate (EMU_ENOTIMPL) rather than pretending.
 * Cores upgraded beyond skeleton status lose their entries here as they
 * gain dedicated suites (see tests/<core>/).
 */
#include "tests.h"
#include "emu/emu.h"

#include <stdlib.h>
#include <string.h>

static void run_skel(const char *name, const uint8_t *good, size_t good_size,
                     const uint8_t *bad, size_t bad_size)
{
    size_t count = 0;
    const emu_core_info_t *reg = emu_core_registry(&count);
    const emu_core_vtable_t *vt = NULL;
    for (size_t i = 0; i < count; i++)
        if (strcmp(reg[i].name, name) == 0)
            vt = reg[i].vtable();
    if (vt == NULL)
        return; /* core not compiled into this build: nothing to test */
    emu_core_t *c = NULL;
    T_CHECK_EQ_U(vt->create(&c), EMU_OK);
    T_CHECK_EQ_U(vt->load_rom(c, NULL, 0), EMU_EINVAL);
    if (bad != NULL && bad_size > 0)
        T_CHECK_EQ_U(vt->load_rom(c, bad, bad_size), EMU_EBADROM);
    T_CHECK_EQ_U(vt->load_rom(c, good, good_size), EMU_OK);
    T_CHECK_EQ_U(vt->run_frame(c), EMU_ENOTIMPL);
    T_CHECK_EQ_U(vt->state_size(c), 0);
    uint8_t tiny[8] = { 0 };
    T_CHECK_EQ_U(vt->save_state(c, tiny, sizeof tiny), EMU_ENOTIMPL);
    T_CHECK_EQ_U(vt->load_state(c, tiny, sizeof tiny), EMU_ENOTIMPL);
    emu_core_destroy(c); /* regression: base.vtable must be set (NULL deref before fix) */
}

static void test_ds_skeleton(void)
{
    static const uint8_t logo8[8] = { 0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A,
                                      0xA2, 0x21 };
    size_t size = 0x400;
    uint8_t *img = calloc(1, size);
    memcpy(img + 0x160, logo8, 8);
    uint8_t bad[0x400] = { 0 };
    run_skel("mds-a", img, size, bad, sizeof bad);
    free(img);
}

static void test_saturn_dc_skeleton(void)
{
    size_t size = 0x1000;
    uint8_t *img = calloc(1, size);
    memcpy(img, "SEGA SEGASATURN", 15);
    run_skel("supersaturn", img, size, NULL, 0);
    memset(img, 0, size);
    memcpy(img, "SEGA SEGAKATANA", 15);
    run_skel("supercastpro", img, size, NULL, 0);
    free(img);
}

static void test_n64_skeleton(void)
{
    uint8_t z64[0x1000] = { 0 };
    z64[0] = 0x80;
    z64[1] = 0x37;
    z64[2] = 0x12;
    z64[3] = 0x40;
    run_skel("m64-b", z64, sizeof z64, NULL, 0);
}

T_SUITE_BEGIN(skeletons)
{ "ds_skeleton_contract", test_ds_skeleton },
{ "saturn_dc_skeleton_contract", test_saturn_dc_skeleton },
{ "n64_skeleton_contract", test_n64_skeleton },
T_SUITE_END
T_SUITE_REG(skeletons)
