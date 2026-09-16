/*
 * beatle-nes-redux state white-box checks + NES suite aggregator.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "beatle-nes-redux/nes.h"

#include <stdlib.h>
#include <string.h>

void t_register_nes_cpu(void);
void t_register_nes_cart(void);
void t_register_nes_ppu(void);
void t_register_nes_apu(void);
void t_register_nes_state(void);

void t_register_nes(void)
{
    t_register_nes_cpu();
    t_register_nes_cart();
    t_register_nes_ppu();
    t_register_nes_apu();
    t_register_nes_state();
}

static struct nes *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_beatle_nes_redux()->create(&c) != EMU_OK)
        return NULL;
    return (struct nes *)c;
}

static struct nes *mk_core_with_rom(void)
{
    struct nes *n = mk_core();
    if (n == NULL)
        return NULL;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(2, 1, 0, 0, &sz);
    emu_result_t r = emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz);
    free(rom);
    if (r != EMU_OK) {
        emu_core_beatle_nes_redux()->destroy(&n->base);
        return NULL;
    }
    return n;
}

static void state_full_roundtrip(void)
{
    struct nes *ref = mk_core_with_rom();
    T_CHECK(ref != NULL);
    if (!ref)
        return;
    /* run some frames so state is non-trivial */
    for (int i = 0; i < 4; i++)
        T_CHECK_EQ(emu_core_beatle_nes_redux()->run_frame(&ref->base), EMU_OK);

    size_t sz = emu_core_beatle_nes_redux()->state_size(&ref->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob) {
        T_CHECK_EQ(emu_core_beatle_nes_redux()->save_state(&ref->base, blob, sz),
                   EMU_OK);
        struct nes *n = mk_core_with_rom();
        T_CHECK(n != NULL);
        if (n) {
            T_CHECK_EQ(emu_core_beatle_nes_redux()->load_state(&n->base, blob, sz),
                       EMU_OK);
            T_CHECK_EQ(n->cpu.pc, ref->cpu.pc);
            T_CHECK_EQ(n->cpu.a, ref->cpu.a);
            T_CHECK_EQ(n->cpu.s, ref->cpu.s);
            T_CHECK_EQ(n->ppu.scanline, ref->ppu.scanline);
            T_CHECK_EQ(n->ppu.dot, ref->ppu.dot);
            T_CHECK_EQ(n->ppu.v, ref->ppu.v);
            T_CHECK_EQ(n->ppu.odd_frame, ref->ppu.odd_frame);
            T_CHECK_EQ(n->apu.frame_step, ref->apu.frame_step);
            T_CHECK_EQ(n->apu.frame_cycles, ref->apu.frame_cycles);
            emu_core_beatle_nes_redux()->destroy(&n->base);
        }
    }
    free(blob);
    emu_core_beatle_nes_redux()->destroy(&ref->base);
}

T_SUITE_BEGIN(nes_state)
{ "full_roundtrip", state_full_roundtrip },
T_SUITE_END

T_SUITE_REG(nes_state)
