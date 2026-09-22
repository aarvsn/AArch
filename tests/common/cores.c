/*
 * Registry-completeness test. Every core named in the registry must be
 * at least "partial": dedicated per-core suites live in tests/<core>/,
 * and the shared API/state contract tests cover the common interface.
 */
#include "tests.h"
#include "emu/emu.h"

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

T_SUITE_BEGIN(registry)
{ "registry_complete", test_registry_complete },
T_SUITE_END
T_SUITE_REG(registry)
