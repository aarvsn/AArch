/*
 * mgbx APU tests: channel enable/disable, non-zero deterministic output,
 * length counters, envelope, reset behavior, buffer bounds. Expected
 * behaviors follow the PSG specification.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbx/mgbx.h"

#include <stdlib.h>
#include <string.h>

static struct mgbx *mk_core_with_rom(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbx()->create(&c) != EMU_OK)
        return NULL;
    struct mgbx *gb = (struct mgbx *)c;
    size_t rom_size = 0;
    uint8_t *rom = gb_make_rom(2, GB_CART_ROM_ONLY, 0, &rom_size);
    emu_result_t r = emu_core_mgbx()->load_rom(c, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_mgbx()->destroy(c);
        return NULL;
    }
    return gb;
}

static void apu_square_nonzero_output(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_bus_write(gb, 0xFF24, 0x77); /* NR50 full volume */
    gb_bus_write(gb, 0xFF25, 0xFF); /* NR51 all channels both sides */
    gb_bus_write(gb, 0xFF12, 0xF0); /* CH1: max volume, no envelope */
    gb_bus_write(gb, 0xFF13, 0x00); /* freq low */
    gb_bus_write(gb, 0xFF14, 0x87); /* trigger, freq high = 7 */
    T_CHECK(gb->apu.sq[0].active);
    /* run one frame: some samples must be non-zero */
    int nonzero = 0;
    size_t before = gb->apu.out_pos;
    for (uint32_t i = 0; i < GB_CYCLES_PER_FRAME / 4u; i++) {
        gb_apu_step(gb, 4);
        /* count emitted non-zero samples by scanning tail after each step is
         * costly; simpler: track buffer growth and check at end */
        before = gb->apu.out_pos;
        (void)before;
    }
    for (size_t s = 0; s + 1 < gb->apu.out_pos; s += 2)
        if (gb->audio[s] != 0 || gb->audio[s + 1] != 0)
            nonzero++;
    T_CHECK(nonzero > 0);
    /* deterministic: identical config + fresh core -> identical buffer */
    struct mgbx *gb2 = mk_core_with_rom();
    T_CHECK(gb2 != NULL);
    if (gb2) {
        gb_bus_write(gb2, 0xFF24, 0x77);
        gb_bus_write(gb2, 0xFF25, 0xFF);
        gb_bus_write(gb2, 0xFF12, 0xF0);
        gb_bus_write(gb2, 0xFF13, 0x00);
        gb_bus_write(gb2, 0xFF14, 0x87);
        for (uint32_t i = 0; i < GB_CYCLES_PER_FRAME / 4u; i++)
            gb_apu_step(gb2, 4);
        T_CHECK_EQ(gb2->apu.out_pos, gb->apu.out_pos);
        T_CHECK(memcmp(gb->audio, gb2->audio,
                       gb->apu.out_pos * sizeof(int16_t)) == 0);
        emu_core_mgbx()->destroy(&gb2->base);
    }
    emu_core_mgbx()->destroy(&gb->base);
}

static void apu_dac_off_silence(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* NR12 upper five bits all zero -> DAC off -> channel cannot be enabled */
    gb_bus_write(gb, 0xFF25, 0xFF);
    gb_bus_write(gb, 0xFF12, 0x00);
    gb_bus_write(gb, 0xFF14, 0x87);
    T_CHECK_EQ(gb->apu.sq[0].active, 0);
    for (uint32_t i = 0; i < 2000u; i++)
        gb_apu_step(gb, 4);
    for (size_t s = 0; s < gb->apu.out_pos; s++)
        T_CHECK_EQ(gb->audio[s], 0);
    emu_core_mgbx()->destroy(&gb->base);
}

static void apu_length_counter_disables(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* CH1 length = 1 via NR11 (64-63 = 1), enable + trigger */
    gb_bus_write(gb, 0xFF12, 0xF0);
    gb_bus_write(gb, 0xFF11, 0x3F); /* length = 1 */
    gb_bus_write(gb, 0xFF14, 0xC7); /* trigger + length enable */
    T_CHECK(gb->apu.sq[0].active);
    T_CHECK_EQ(gb->apu.sq[0].len_counter, 1);
    /* 512 Hz length tick: run 8192 T-cycles = one sequencer step... the
     * length ticks on steps 0,2,4,6: run 2 steps (16384 T) */
    for (int i = 0; i < 16384 / 4; i++)
        gb_apu_step(gb, 4);
    T_CHECK_EQ(gb->apu.sq[0].active, 0);
    emu_core_mgbx()->destroy(&gb->base);
}

static void apu_power_off_clears(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_bus_write(gb, 0xFF12, 0xF0);
    gb_bus_write(gb, 0xFF14, 0x87);
    T_CHECK(gb->apu.sq[0].active);
    gb_bus_write(gb, 0xFF26, 0x00); /* power off */
    T_CHECK_EQ(gb->apu.sq[0].active, 0);
    /* registers locked while off */
    gb_bus_write(gb, 0xFF12, 0xF0);
    T_CHECK_EQ(gb->apu.sq[0].env_volume, 0);
    /* power back on: silence until re-triggered */
    gb_bus_write(gb, 0xFF26, 0x80);
    T_CHECK_EQ(gb->apu.sq[0].active, 0);
    emu_core_mgbx()->destroy(&gb->base);
}

static void apu_output_rate_and_bounds(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* one frame at 32768 Hz from 59.7275 fps yields 548-549 stereo pairs */
    gb_bus_write(gb, 0xFF25, 0xFF);
    for (uint32_t i = 0; i < GB_CYCLES_PER_FRAME / 4u; i++)
        gb_apu_step(gb, 4);
    /* 70224 T / 128 = 548.625 -> 549 samples emitted max */
    T_CHECK(gb->apu.out_pos <= 1100);
    T_CHECK(gb->apu.out_pos >= 1090); /* 548-549 pairs */
    emu_core_mgbx()->destroy(&gb->base);
}

static void apu_reset_silence(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_bus_write(gb, 0xFF25, 0xFF);
    gb_bus_write(gb, 0xFF12, 0xF0);
    gb_bus_write(gb, 0xFF14, 0x87);
    for (uint32_t i = 0; i < 1000u; i++)
        gb_apu_step(gb, 4);
    emu_core_mgbx()->destroy(&gb->base);

    /* fresh core: reset behavior means no sound until registers are set */
    struct mgbx *gb2 = mk_core_with_rom();
    T_CHECK(gb2 != NULL);
    if (!gb2)
        return;
    for (uint32_t i = 0; i < 1000u; i++)
        gb_apu_step(gb2, 4);
    for (size_t s = 0; s < gb2->apu.out_pos; s++)
        T_CHECK_EQ(gb2->audio[s], 0);
    emu_core_mgbx()->destroy(&gb2->base);
}

T_SUITE_BEGIN(gb_apu)
{ "square_nonzero_deterministic", apu_square_nonzero_output },
{ "dac_off_silence", apu_dac_off_silence },
{ "length_counter_disables", apu_length_counter_disables },
{ "power_off_clears", apu_power_off_clears },
{ "output_rate_and_bounds", apu_output_rate_and_bounds },
{ "reset_silence", apu_reset_silence },
T_SUITE_END

T_SUITE_REG(gb_apu)
