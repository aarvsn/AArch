/*
 * mgbax memory system tests: region decode, mirrors, IO registers.
 * Expected values derive from the GBA memory map specification.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbax/gba.h"

#include <stdlib.h>
#include <string.h>

static struct gba *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbax()->create(&c) != EMU_OK)
        return NULL;
    return (struct gba *)c;
}

static struct gba *mk_rom_core(void)
{
    struct gba *g = mk_core();
    if (g == NULL)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0xA5, &rom_size);
    if (emu_core_mgbax()->load_rom(&g->base, rom, rom_size) != EMU_OK) {
        free(rom);
        emu_core_mgbax()->destroy(&g->base);
        return NULL;
    }
    free(rom);
    return g;
}

static void mem_ewram(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_mem_write8(g, 0x02000000u, 0x12u);
    gba_mem_write16(g, 0x02000002u, 0x3456u);
    gba_mem_write32(g, 0x02000004u, 0x89ABCDEFu);
    T_CHECK_EQ(gba_mem_read8(g, 0x02000000u), 0x12u);
    T_CHECK_EQ(gba_mem_read16(g, 0x02000002u), 0x3456u);
    T_CHECK_EQ(gba_mem_read32(g, 0x02000004u), 0x89ABCDEFu);
    /* 256 KiB wraparound mirror: 0x02040000 wraps to 0x02000000 */
    T_CHECK_EQ(gba_mem_read8(g, 0x02040000u), 0x12u);
    gba_mem_write8(g, 0x02040001u, 0x77u);
    T_CHECK_EQ(gba_mem_read8(g, 0x02000001u), 0x77u);
    emu_core_mgbax()->destroy(&g->base);
}

static void mem_iwram(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_mem_write16(g, 0x03007FFEu, 0xBEEFu);
    T_CHECK_EQ(gba_mem_read16(g, 0x03007FFEu), 0xBEEFu);
    /* 32 KiB wraparound: 0x03008000 aliases 0x03000000 */
    gba_mem_write8(g, 0x03008000u, 0x5Au);
    T_CHECK_EQ(gba_mem_read8(g, 0x03000000u), 0x5Au);
    emu_core_mgbax()->destroy(&g->base);
}

static void mem_vram_palette_oam(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* VRAM: 96 KiB; 0x06018000-0x06017FFF region aliases OBJ area */
    gba_mem_write16(g, 0x06010000u, 0x1234u);
    T_CHECK_EQ(gba_mem_read16(g, 0x06018000u), 0x1234u);
    gba_mem_write16(g, 0x06018002u, 0xCAFEu);
    T_CHECK_EQ(gba_mem_read16(g, 0x06010002u), 0xCAFEu);
    /* Palette 1 KiB */
    gba_mem_write16(g, 0x05000000u, 0x7FFFu);
    T_CHECK_EQ(gba_mem_read16(g, 0x05000000u), 0x7FFFu);
    /* OAM 1 KiB */
    gba_mem_write16(g, 0x07000000u, 0x00FFu);
    T_CHECK_EQ(gba_mem_read16(g, 0x07000000u), 0x00FFu);
    emu_core_mgbax()->destroy(&g->base);
}

static void mem_rom_sram(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* ROM readable at 0x08000000 (filled with 0xA5) */
    T_CHECK_EQ(gba_mem_read8(g, 0x08000000u), 0xA5u);
    T_CHECK_EQ(gba_mem_read8(g, 0x08000FFFu), 0xA5u);
    /* ROM writes are ignored */
    gba_mem_write8(g, 0x08000000u, 0x00u);
    T_CHECK_EQ(gba_mem_read8(g, 0x08000000u), 0xA5u);
    /* SRAM at 0x0E000000, 8-bit */
    gba_mem_write8(g, 0x0E000010u, 0x42u);
    T_CHECK_EQ(gba_mem_read8(g, 0x0E000010u), 0x42u);
    gba_mem_write8(g, 0x0F000011u, 0x43u); /* 0x0F aliases SRAM too */
    T_CHECK_EQ(gba_mem_read8(g, 0x0E000011u), 0x43u);
    emu_core_mgbax()->destroy(&g->base);
}

static void mem_io_keyinput(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* KEYINPUT is active low: 0x03FF = no buttons */
    T_CHECK_EQ(gba_io_read16(g, 0x04000130u), 0x03FFu);
    emu_core_mgbax()->set_input(&g->base, 0x0001u); /* A */
    T_CHECK_EQ(gba_io_read16(g, 0x04000130u), 0x03FEu);
    emu_core_mgbax()->set_input(&g->base, 0x0009u); /* A + Start */
    T_CHECK_EQ(gba_io_read16(g, 0x04000130u), 0x03F6u);
    emu_core_mgbax()->set_input(&g->base, 0x03FFu);
    T_CHECK_EQ(gba_io_read16(g, 0x04000130u), 0x0000u);
    emu_core_mgbax()->destroy(&g->base);
}

static void mem_io_irq_regs(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* IE readback */
    gba_io_write16(g, 0x04000200u, 0x0009u);
    T_CHECK_EQ(gba_io_read16(g, 0x04000200u), 0x0009u);
    /* IF: set via request, cleared by writing 1s (acknowledge) */
    gba_request_irq(g, 0x0009u);
    T_CHECK_EQ(gba_io_read16(g, 0x04000202u), 0x0009u);
    gba_io_write16(g, 0x04000202u, 0x0001u);
    T_CHECK_EQ(gba_io_read16(g, 0x04000202u), 0x0008u);
    gba_io_write16(g, 0x04000202u, 0x0000u); /* writing 0s does nothing */
    T_CHECK_EQ(gba_io_read16(g, 0x04000202u), 0x0008u);
    /* IME and WAITCNT readback */
    gba_io_write16(g, 0x04000208u, 0x0001u);
    T_CHECK_EQ(gba_io_read16(g, 0x04000208u), 0x0001u);
    gba_io_write16(g, 0x04000204u, 0x4317u);
    T_CHECK_EQ(gba_io_read16(g, 0x04000204u), 0x4317u);
    emu_core_mgbax()->destroy(&g->base);
}

static void mem_unaligned_access(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_mem_write32(g, 0x02000000u, 0xAABBCCDDu);
    /* halfword reads force bit0 low */
    T_CHECK_EQ(gba_mem_read16(g, 0x02000001u), 0xCCDDu); /* bit0 masked */
    /* bus word reads are performed word-aligned (the CPU applies the
     * unaligned LDR rotation on top) */
    T_CHECK_EQ(gba_bus_read32(g, 0x02000002u), 0xAABBCCDDu);
    emu_core_mgbax()->destroy(&g->base);
}

T_SUITE_BEGIN(gba_mem)
{ "ewram_roundtrip", mem_ewram },
{ "iwram_roundtrip", mem_iwram },
{ "vram_palette_oam", mem_vram_palette_oam },
{ "rom_sram", mem_rom_sram },
{ "io_keyinput", mem_io_keyinput },
{ "io_irq_regs", mem_io_irq_regs },
{ "unaligned_access", mem_unaligned_access },
T_SUITE_END

T_SUITE_REG(gba_mem)
