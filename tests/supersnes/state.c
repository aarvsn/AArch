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

static void snes_state_roundtrip(void)
{
    struct snes *ref = mk_core_with_rom();
    T_CHECK(ref != NULL);
    if (!ref)
        return;
    for (int i = 0; i < 3; i++)
        T_CHECK_EQ(emu_core_supersnes()->run_frame(&ref->base), EMU_OK);
    size_t sz = emu_core_supersnes()->state_size(&ref->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob) {
        T_CHECK_EQ(emu_core_supersnes()->save_state(&ref->base, blob, sz), EMU_OK);
        struct snes *s = mk_core_with_rom();
        T_CHECK(s != NULL);
        if (s) {
            T_CHECK_EQ(emu_core_supersnes()->load_state(&s->base, blob, sz), EMU_OK);
            T_CHECK_EQ(s->cpu.pc, ref->cpu.pc);
            T_CHECK_EQ(s->cpu.a, ref->cpu.a);
            T_CHECK_EQ(s->ppu.line, ref->ppu.line);
            T_CHECK(memcmp(s->mem.wram, ref->mem.wram, 0x2000) == 0);
            emu_core_supersnes()->destroy(&s->base);
        }
    }
    free(blob);
    emu_core_supersnes()->destroy(&ref->base);
}

T_SUITE_BEGIN(snes_state)
{ "roundtrip", snes_state_roundtrip },
T_SUITE_END
T_SUITE_REG(snes_state)
