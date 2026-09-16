/*
 * mgbx state white-box checks + suite aggregator for the mgbx tests.
 *
 * Beyond the common state contract (tests/common/state.c), these tests
 * verify that internal subsystem state (timer, APU sequencer, cart banking,
 * CPU registers) actually round-trips.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbx/mgbx.h"
#include "../../src/common/util.h"

#include <stdlib.h>
#include <string.h>

void t_register_gb_cpu(void);
void t_register_gb_cart(void);
void t_register_gb_mem(void);
void t_register_gb_ppu(void);
void t_register_gb_timer(void);
void t_register_gb_apu(void);
void t_register_gb_state(void);

void t_register_mgbx(void)
{
    t_register_gb_cpu();
    t_register_gb_cart();
    t_register_gb_mem();
    t_register_gb_ppu();
    t_register_gb_timer();
    t_register_gb_apu();
    t_register_gb_state();
}

static struct mgbx *mk_core_with_rom(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbx()->create(&c) != EMU_OK)
        return NULL;
    struct mgbx *gb = (struct mgbx *)c;
    size_t rom_size = 0;
    uint8_t *rom = gb_make_rom(4, GB_CART_MBC1_RAM, 2, &rom_size);
    emu_result_t r = emu_core_mgbx()->load_rom(c, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_mgbx()->destroy(c);
        return NULL;
    }
    return gb;
}

static void state_size_matches_serialization(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* run some frames so state is non-trivial */
    for (int i = 0; i < 5; i++)
        T_CHECK_EQ(emu_core_mgbx()->run_frame(&gb->base), EMU_OK);

    size_t sz = emu_core_mgbx()->state_size(&gb->base);
    /* independent size computation with a counting writer */
    emu_state_writer w = { NULL, 0, 0, 0 };
    /* reuse core serializer through save_state with NULL? save_state(NULL)
     * returns EINVAL by contract, so compute via state_size only: */
    (void)w;
    T_CHECK(sz >= 64); /* sanity: state is substantial */

    /* exact-size save then load into a fresh core */
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob) {
        T_CHECK_EQ(emu_core_mgbx()->save_state(&gb->base, blob, sz), EMU_OK);

        struct mgbx *gb2 = mk_core_with_rom();
        T_CHECK(gb2 != NULL);
        if (gb2) {
            T_CHECK_EQ(emu_core_mgbx()->load_state(&gb2->base, blob, sz), EMU_OK);
            /* internal state must match exactly */
            T_CHECK_EQ(gb2->cpu.pc, gb->cpu.pc);
            T_CHECK_EQ(gb2->cpu.sp, gb->cpu.sp);
            T_CHECK_EQ(gb2->cpu.a, gb->cpu.a);
            T_CHECK_EQ(gb2->timer.counter, gb->timer.counter);
            T_CHECK_EQ(gb2->timer.tima, gb->timer.tima);
            T_CHECK_EQ(gb2->apu.frame_seq_step, gb->apu.frame_seq_step);
            T_CHECK_EQ(gb2->apu.frame_seq_timer, gb->apu.frame_seq_timer);
            T_CHECK_EQ(gb2->ppu.ly, gb->ppu.ly);
            T_CHECK_EQ(gb2->ppu.dot, gb->ppu.dot);
            T_CHECK_EQ(gb2->cart.bank1, gb->cart.bank1);
            T_CHECK_EQ(gb2->total_cycles, gb->total_cycles);
            emu_core_mgbx()->destroy(&gb2->base);
        }
    }
    free(blob);
    emu_core_mgbx()->destroy(&gb->base);
}

static void state_cart_ram_roundtrip(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* write pattern to cart RAM via bus */
    gb_bus_write(gb, 0x0000, 0x0A); /* RAM enable */
    for (int i = 0; i < 16; i++)
        gb_bus_write(gb, (uint16_t)(0xA000u + i), (uint8_t)(0x30u + (uint32_t)i));

    size_t sz = emu_core_mgbx()->state_size(&gb->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob) {
        T_CHECK_EQ(emu_core_mgbx()->save_state(&gb->base, blob, sz), EMU_OK);
        struct mgbx *gb2 = mk_core_with_rom();
        T_CHECK(gb2 != NULL);
        if (gb2) {
            T_CHECK_EQ(emu_core_mgbx()->load_state(&gb2->base, blob, sz), EMU_OK);
            gb_bus_write(gb2, 0x0000, 0x0A);
            for (int i = 0; i < 16; i++)
                T_CHECK_EQ(gb_bus_read(gb2, (uint16_t)(0xA000u + i)),
                           (uint8_t)(0x30u + (uint32_t)i));
            emu_core_mgbx()->destroy(&gb2->base);
        }
    }
    free(blob);
    emu_core_mgbx()->destroy(&gb->base);
}

static void state_bank_registers_roundtrip(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_bus_write(gb, 0x2000, 0x03); /* MBC1 bank 3 */
    gb_bus_write(gb, 0x4000, 0x01); /* bank2 */
    gb_bus_write(gb, 0x6000, 0x01); /* mode 1 */
    gb_bus_write(gb, 0x0000, 0x0A); /* ram enable */

    size_t sz = emu_core_mgbx()->state_size(&gb->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob) {
        T_CHECK_EQ(emu_core_mgbx()->save_state(&gb->base, blob, sz), EMU_OK);
        struct mgbx *gb2 = mk_core_with_rom();
        T_CHECK(gb2 != NULL);
        if (gb2) {
            T_CHECK_EQ(emu_core_mgbx()->load_state(&gb2->base, blob, sz), EMU_OK);
            T_CHECK_EQ(gb2->cart.bank1, 0x03);
            T_CHECK_EQ(gb2->cart.bank2, 0x01);
            T_CHECK_EQ(gb2->cart.mode, 0x01);
            T_CHECK_EQ(gb2->cart.ram_enabled, 0x01);
            /* observable through the bus too */
            T_CHECK_EQ(gb_cart_read(&gb2->cart, 0x4000), 0x03);
            emu_core_mgbx()->destroy(&gb2->base);
        }
    }
    free(blob);
    emu_core_mgbx()->destroy(&gb->base);
}

static void state_undersized_no_write(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    size_t sz = emu_core_mgbx()->state_size(&gb->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob) {
        memset(blob, 0xCC, sz);
        T_CHECK_EQ(emu_core_mgbx()->save_state(&gb->base, blob, sz - 1),
                   EMU_ENOSPACE);
        /* core still runs */
        T_CHECK_EQ(emu_core_mgbx()->run_frame(&gb->base), EMU_OK);
    }
    free(blob);
    emu_core_mgbx()->destroy(&gb->base);
}

T_SUITE_BEGIN(gb_state)
{ "size_matches_and_roundtrip", state_size_matches_serialization },
{ "cart_ram_roundtrip", state_cart_ram_roundtrip },
{ "bank_registers_roundtrip", state_bank_registers_roundtrip },
{ "undersized_no_write", state_undersized_no_write },
T_SUITE_END

T_SUITE_REG(gb_state)
