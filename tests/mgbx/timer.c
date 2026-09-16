/*
 * mgbx timer tests: DIV/TIMA rates computed independently from the
 * specification (1 MHz M-cycle counter, TIMA tick bits 8/2/4/6),
 * overflow/TMA reload, DIV-write edge quirk.
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

static void timer_div_rate(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* DIV increments every 64 M-cycles: 1024 M-cycles -> 16 increments */
    uint8_t d0 = gb_timer_read(&gb->timer, 0xFF04);
    for (int i = 0; i < 1024; i++)
        gb_timer_step(gb, 4);
    uint8_t d1 = gb_timer_read(&gb->timer, 0xFF04);
    T_CHECK_EQ((uint8_t)(d1 - d0), 16);
    emu_core_mgbx()->destroy(&gb->base);
}

static void timer_tima_4096(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* TAC = 0x04 (4096 Hz): TIMA increments every 1024 T = 256 m-cycles */
    gb_timer_write(gb, 0xFF07, 0x04);
    for (int i = 0; i < 256; i++)
        gb_timer_step(gb, 4);
    T_CHECK_EQ(gb_timer_read(&gb->timer, 0xFF05), 1);
    for (int i = 0; i < 256; i++)
        gb_timer_step(gb, 4);
    T_CHECK_EQ(gb_timer_read(&gb->timer, 0xFF05), 2);
    emu_core_mgbx()->destroy(&gb->base);
}

static void timer_tima_262144(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* TAC = 0x05 (262144 Hz): every 16 T = 4 m-cycles */
    gb_timer_write(gb, 0xFF07, 0x05);
    for (int i = 0; i < 4; i++)
        gb_timer_step(gb, 4);
    T_CHECK_EQ(gb_timer_read(&gb->timer, 0xFF05), 1);
    emu_core_mgbx()->destroy(&gb->base);
}

static void timer_overflow_tma_irq(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_timer_write(gb, 0xFF07, 0x05);
    gb_timer_write(gb, 0xFF06, 0x42); /* TMA = 0x42 */
    gb->mem.if_reg = 0;
    /* advance until counter bits 1..0 == 0b11: the next M-cycle is then
     * guaranteed to produce a falling edge on the TAC-selected bit 1 */
    while ((gb->timer.counter & 3u) != 3u)
        gb_timer_step(gb, 4);
    gb->timer.tima = 0xFF;
    gb_timer_step(gb, 4); /* falling edge: overflow, TIMA wraps to 0 */
    T_CHECK_EQ(gb_timer_read(&gb->timer, 0xFF05), 0);
    T_CHECK((gb->mem.if_reg & 0x04u) == 0); /* IRQ not yet: 4-cycle delay */
    gb_timer_step(gb, 4); /* reload happens now */
    T_CHECK_EQ(gb_timer_read(&gb->timer, 0xFF05), 0x42);
    T_CHECK(gb->mem.if_reg & 0x04u);
    emu_core_mgbx()->destroy(&gb->base);
}

static void timer_div_write_quirk(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* TAC 0x05 uses counter bit 2. Advance counter so bit 2 is 1, then
     * write DIV: the falling edge must tick TIMA once. */
    gb_timer_write(gb, 0xFF07, 0x05);
    for (int i = 0; i < 6; i++) /* counter bit 2 set after odd m-cycles */
        gb_timer_step(gb, 4);
    /* advance to a state where bit 2 == 1 */
    while (((gb->timer.counter >> 2) & 1u) == 0u)
        gb_timer_step(gb, 4);
    gb->timer.tima = 0;
    gb_timer_write(gb, 0xFF04, 0x00); /* DIV write resets counter */
    T_CHECK_EQ(gb_timer_read(&gb->timer, 0xFF05), 1); /* edge quirk ticked */
    emu_core_mgbx()->destroy(&gb->base);
}

static void timer_disabled_no_tick(void)
{
    struct mgbx *gb = mk_core_with_rom();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb_timer_write(gb, 0xFF07, 0x00); /* disabled, 4096 select */
    for (int i = 0; i < 2000; i++)
        gb_timer_step(gb, 4);
    T_CHECK_EQ(gb_timer_read(&gb->timer, 0xFF05), 0);
    T_CHECK_EQ(gb_timer_read(&gb->timer, 0xFF04), gb_timer_read(&gb->timer, 0xFF04));
    /* DIV still ticks */
    uint8_t d0 = gb_timer_read(&gb->timer, 0xFF04);
    for (int i = 0; i < 512; i++)
        gb_timer_step(gb, 4);
    T_CHECK(gb_timer_read(&gb->timer, 0xFF04) != d0);
    emu_core_mgbx()->destroy(&gb->base);
}

T_SUITE_BEGIN(gb_timer)
{ "div_rate", timer_div_rate },
{ "tima_4096hz", timer_tima_4096 },
{ "tima_262144hz", timer_tima_262144 },
{ "overflow_tma_irq", timer_overflow_tma_irq },
{ "div_write_edge_quirk", timer_div_write_quirk },
{ "disabled_no_tima_tick", timer_disabled_no_tick },
T_SUITE_END

T_SUITE_REG(gb_timer)
