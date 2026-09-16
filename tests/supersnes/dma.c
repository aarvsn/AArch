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

static void dma_cpu_to_vram(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* channel 0: mode 0 (1 byte), A bus = WRAM $7E:1000, B = $2119 (VRAM
     * high byte: commits the word per VMAIN bit7 = 0) */
    s->dma.ch[0].params = 0x00;
    s->dma.ch[0].bbus = 0x19;
    s->dma.ch[0].abank = 0x7E;
    s->dma.ch[0].abus = 0x1000;
    s->dma.ch[0].count = 4;
    for (int i = 0; i < 4; i++)
        s->mem.wram[0x1000 + (uint32_t)i] = (uint8_t)(0x40u + (uint32_t)i);
    s->ppu.vmadd = 0x0000;
    s->dma.mdmaen = 0x01;
    uint32_t stolen = snes_dma_run(s);
    T_CHECK_EQ(stolen, 32u); /* 4 bytes x 8 master clocks */
    /* each $2119 write commits a word (byte = data, low latch = 0) and
     * increments: high bytes land at consecutive words */
    T_CHECK_EQ(s->ppu.vram[1], 0x40);
    T_CHECK_EQ(s->ppu.vram[3], 0x41);
    T_CHECK_EQ(s->ppu.vram[5], 0x42);
    T_CHECK_EQ(s->ppu.vram[7], 0x43);
    T_CHECK_EQ(s->dma.mdmaen & 1u, 0); /* channel disabled after transfer */
    emu_core_supersnes()->destroy(&s->base);
}
static void dma_16bit_units(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* mode 1: two bytes per unit; count = 2 units -> 4 bytes; destination
     * $2118/2119 pairs: vram low/high */
    s->dma.ch[0].params = 0x01;
    s->dma.ch[0].bbus = 0x18;
    s->dma.ch[0].abank = 0x7E;
    s->dma.ch[0].abus = 0x1100;
    s->dma.ch[0].count = 4; /* 4 bytes = 2 words */
    for (int i = 0; i < 4; i++)
        s->mem.wram[0x1100 + (uint32_t)i] = (uint8_t)(0xA0u + (uint32_t)i);
    s->ppu.vmadd = 0x0010;
    s->dma.mdmaen = 0x01;
    snes_dma_run(s);
    /* $2118 latches the low byte, $2119 commits the word: consecutive
     * words 0xA1A0, 0xA3A2 land at VRAM words 0x10, 0x11 */
    T_CHECK_EQ(s->ppu.vram[0x20], 0xA0);
    T_CHECK_EQ(s->ppu.vram[0x21], 0xA1);
    T_CHECK_EQ(s->ppu.vram[0x22], 0xA2);
    T_CHECK_EQ(s->ppu.vram[0x23], 0xA3);
    emu_core_supersnes()->destroy(&s->base);
}

T_SUITE_BEGIN(snes_dma)
{ "cpu_to_vram", dma_cpu_to_vram },
{ "sixteen_bit_units", dma_16bit_units },
T_SUITE_END
T_SUITE_REG(snes_dma)
