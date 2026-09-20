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

/*
 * All 12 registry entries now have dedicated suites or are covered by
 * the shared API/state contract tests; the skeleton contract
 * (EMU_ENOTIMPL run_frame) applied only while cores were stubs.
 */
static void test_registry_complete(void)
{
    size_t count = 0;
    const emu_core_info_t *reg = emu_core_registry(&count);
    /* 4 original cores + 7 distinct milestone-3 cores (beatle-nes-redux
     * covers the NES re-run) = 11 */
    T_CHECK_EQ_U(count, 11u);
    for (size_t i = 0; i < count; i++) {
        T_CHECK(reg[i].name != NULL);
        T_CHECK(reg[i].note != NULL);
        T_CHECK(reg[i].status == EMU_STATUS_WORKING ||
                reg[i].status == EMU_STATUS_PARTIAL);
    }
}

T_SUITE_BEGIN(skeletons)
{ "registry_complete", test_registry_complete },
T_SUITE_END
T_SUITE_REG(skeletons)
