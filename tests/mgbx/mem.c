/*
 * mgbx memory/bus tests: region mapping, WRAM echo, open areas, I/O
 * dispatch, OAM DMA copy behavior.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbx/mgbx.h"

#include <stdlib.h>
#include <string.h>

static struct mgbx *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbx()->create(&c) != EMU_OK)
        return NULL;
    return (struct mgbx *)c;
}

static void mem_regions(void)
{
    struct mgbx *gb = mk_core();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* WRAM */
    gb_bus_write(gb, 0xC123, 0x5A);
    T_CHECK_EQ(gb_bus_read(gb, 0xC123), 0x5A);
    /* echo of WRAM */
    T_CHECK_EQ(gb_bus_read(gb, 0xE123), 0x5A);
    gb_bus_write(gb, 0xF000, 0x77);
    T_CHECK_EQ(gb_bus_read(gb, 0xD000), 0x77);
    /* HRAM */
    gb_bus_write(gb, 0xFF90, 0x42);
    T_CHECK_EQ(gb_bus_read(gb, 0xFF90), 0x42);
    /* VRAM */
    gb_bus_write(gb, 0x8010, 0x99);
    T_CHECK_EQ(gb_bus_read(gb, 0x8010), 0x99);
    /* prohibited region reads 0 */
    T_CHECK_EQ(gb_bus_read(gb, 0xFEB0), 0x00);
    /* IE register */
    gb_bus_write(gb, 0xFFFF, 0x1F);
    T_CHECK_EQ(gb_bus_read(gb, 0xFFFF), 0x1F);
    emu_core_mgbx()->destroy(&gb->base);
}

static void mem_io_masks(void)
{
    struct mgbx *gb = mk_core();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* IF upper bits read as 1; writes masked to 0x1F */
    gb_bus_write(gb, 0xFF0F, 0xFF);
    T_CHECK_EQ(gb_bus_read(gb, 0xFF0F), 0xFFu - 0u);
    T_CHECK_EQ(gb->mem.if_reg, 0x1F);
    gb_bus_write(gb, 0xFF0F, 0xE0);
    T_CHECK_EQ(gb->mem.if_reg, 0x00);
    /* P1 upper bits read 1 */
    T_CHECK_EQ(gb_bus_read(gb, 0xFF00) & 0xC0u, 0xC0u);
    /* TAC upper bits read 1 */
    T_CHECK_EQ(gb_bus_read(gb, 0xFF07) & 0xF8u, 0xF8u);
    /* serial SC: only bits 7/0 stored; bits 1..6 read as 1 */
    gb_bus_write(gb, 0xFF02, 0x81);
    T_CHECK_EQ(gb_bus_read(gb, 0xFF02), 0xFFu & 0xFFu); /* 0x81 | 0x7E */
    emu_core_mgbx()->destroy(&gb->base);
}

static void mem_oam_dma(void)
{
    struct mgbx *gb = mk_core();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* stage 160 bytes at 0xC000 and request DMA from page 0xC0 */
    for (int i = 0; i < 160; i++)
        gb->mem.wram[i] = (uint8_t)(0x10u + (uint32_t)i);
    gb_bus_write(gb, 0xFF46, 0xC0);
    T_CHECK_EQ(gb->mem.dma_active, 1);
    /* 160 M-cycles complete the copy */
    for (int i = 0; i < 160; i++)
        gb_cpu_step(gb);
    T_CHECK_EQ(gb->mem.dma_active, 0);
    for (int i = 0; i < 160; i++)
        T_CHECK_EQ(gb->ppu.oam[i], (uint8_t)(0x10u + (uint32_t)i));
    emu_core_mgbx()->destroy(&gb->base);
}

static void mem_oam_dma_stalls_cpu(void)
{
    struct mgbx *gb = mk_core();
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* program: LD A,0x42 at 0x150 */
    size_t rom_size = 0;
    uint8_t *rom = gb_make_rom(2, GB_CART_ROM_ONLY, 0, &rom_size);
    static const uint8_t prog[] = { 0x3E, 0x42, 0x18, 0xFE };
    memcpy(&rom[0x0150], prog, sizeof prog);
    T_CHECK_EQ(emu_core_mgbx()->load_rom(&gb->base, rom, rom_size), EMU_OK);
    free(rom);
    gb_cpu_step(gb); /* NOP at 0x100 */
    gb_cpu_step(gb); /* JP 0x150 */

    gb->mem.wram[0] = 0x55;
    gb_bus_write(gb, 0xFF46, 0xC0);
    /* while DMA runs, CPU makes no progress */
    uint16_t pc_before = gb->cpu.pc;
    for (int i = 0; i < 160; i++)
        gb_cpu_step(gb);
    T_CHECK_EQ(gb->mem.dma_active, 0);
    T_CHECK_EQ(gb->cpu.pc, pc_before); /* PC unchanged after full DMA */
    T_CHECK_EQ(gb->ppu.oam[0], 0x55);
    gb_cpu_step(gb); /* first real instruction after DMA */
    T_CHECK_EQ(gb->cpu.a, 0x42);
    emu_core_mgbx()->destroy(&gb->base);
}

T_SUITE_BEGIN(gb_mem)
{ "regions_and_echo", mem_regions },
{ "io_register_masks", mem_io_masks },
{ "oam_dma_copy", mem_oam_dma },
{ "oam_dma_stall", mem_oam_dma_stalls_cpu },
T_SUITE_END

T_SUITE_REG(gb_mem)
