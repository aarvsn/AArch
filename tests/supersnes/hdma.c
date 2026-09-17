/*
 * supersnes HDMA tests: per-scanline direct and indirect transfers,
 * repeat blocks, table termination, V=0 reload, and the register path
 * ($420B / $420C through the IO map, including the NMITIMEN-preservation
 * regression for $420B writes).
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "supersnes/snes.h"

#include <stdlib.h>
#include <string.h>

static struct snes *mk_core_with_rom(void)
{
    emu_core_t *c = NULL;
    if (emu_core_supersnes()->create(&c) != EMU_OK)
        return NULL;
    size_t sz = 0;
    uint8_t *rom = snes_make_lorom(16, &sz);
    emu_result_t r = emu_core_supersnes()->load_rom(c, rom, sz);
    free(rom);
    if (r != EMU_OK) {
        emu_core_supersnes()->destroy(c);
        return NULL;
    }
    return (struct snes *)c;
}

/* Direct mode 1 (unit 2) -> $2118/$2119 word writes: 2-line block,
 * no repeat, then terminator. */
static void hdma_direct_to_vram(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* table at WRAM $1200: header 0x0002 (2 lines, no repeat), one unit
     * of data D0/D1 (repeat=0 transfers only on the first line), then a
     * terminator directly after the transferred bytes */
    s->mem.wram[0x1200] = 0x02;
    s->mem.wram[0x1201] = 0x00;
    s->mem.wram[0x1202] = 0x10;
    s->mem.wram[0x1203] = 0x11;
    s->mem.wram[0x1204] = 0x00;
    s->mem.wram[0x1205] = 0x00;

    s->dma.ch[0].params = 0x01; /* mode 1: unit 2, direct, increment */
    s->dma.ch[0].bbus = 0x18;
    s->dma.ch[0].abank = 0x7E;
    s->dma.ch[0].abus = 0x1200;
    s->dma.ch[0].hdma_reload_abus = 0x1200;
    s->dma.ch[0].hdma_reload_abank = 0x7E;
    s->dma.hdmaen = 0x01;
    s->ppu.vmadd = 0x0020;

    snes_hdma_init_frame(s);
    snes_hdma_line(s); /* line 1: header + transfer D0/D1 */
    T_CHECK_EQ(s->ppu.vram[0x40], 0x10);
    T_CHECK_EQ(s->ppu.vram[0x41], 0x11);
    T_CHECK_EQ(s->dma.ch[0].hdma_lines_left, 1u);
    snes_hdma_line(s); /* line 2: repeat clear -> no transfer, no advance */
    T_CHECK_EQ(s->dma.ch[0].hdma_xfer, 0u);
    T_CHECK_EQ(s->dma.ch[0].abus, 0x1204u);
    snes_hdma_line(s); /* new block: terminator -> finished */
    T_CHECK_EQ(s->dma.ch[0].hdma_finished, 1u);
    /* table pointer consumed header(2) + data(2) + terminator(2) */
    T_CHECK_EQ(s->dma.ch[0].abus, 0x1206u);
    emu_core_supersnes()->destroy(&s->base);
}

/* Repeat block, mode 0 (unit 1) -> $2119 word commits. */
static void hdma_repeat_bytes(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    s->mem.wram[0x1400] = 0x82; /* repeat, 2 lines */
    s->mem.wram[0x1401] = 0x30;
    s->mem.wram[0x1402] = 0x31;
    s->mem.wram[0x1403] = 0x00; /* terminator */

    s->dma.ch[0].params = 0x00; /* mode 0: unit 1 */
    s->dma.ch[0].bbus = 0x19;
    s->dma.ch[0].abank = 0x7E;
    s->dma.ch[0].abus = 0x1400;
    s->dma.ch[0].hdma_reload_abus = 0x1400;
    s->dma.ch[0].hdma_reload_abank = 0x7E;
    s->dma.hdmaen = 0x01;
    s->ppu.vmadd = 0x0100;

    snes_hdma_init_frame(s);
    snes_hdma_line(s);
    T_CHECK_EQ(s->ppu.vram[0x201], 0x30); /* $2119 commits high byte */
    T_CHECK_EQ(s->dma.ch[0].hdma_xfer, 1u);
    snes_hdma_line(s); /* repeat: transfers again */
    T_CHECK_EQ(s->dma.ch[0].hdma_xfer, 1u);
    T_CHECK_EQ(s->ppu.vram[0x203], 0x31);
    snes_hdma_line(s); /* terminator */
    T_CHECK_EQ(s->dma.ch[0].hdma_finished, 1u);
    T_CHECK_EQ(s->dma.ch[0].abus, 0x1404u);
    emu_core_supersnes()->destroy(&s->base);
}

/* Indirect mode 1: table holds the header + a 16-bit indirect address;
 * data comes from (ibank, indirect address), advancing per line. */
static void hdma_indirect(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* table at WRAM $1500: header 0x8002 (repeat, 2 lines), indirect
     * address 0x1600 (bank from $43x7 = 0x7E) */
    s->mem.wram[0x1500] = 0x02;
    s->mem.wram[0x1501] = 0x80;
    s->mem.wram[0x1502] = 0x00;
    s->mem.wram[0x1503] = 0x16;
    s->mem.wram[0x1600] = 0x40; /* data: 2 lines x 2 bytes */
    s->mem.wram[0x1601] = 0x41;
    s->mem.wram[0x1602] = 0x42;
    s->mem.wram[0x1603] = 0x43;

    s->dma.ch[0].params = 0x41; /* mode 1 + indirect (bit 6) */
    s->dma.ch[0].bbus = 0x18;
    s->dma.ch[0].abank = 0x7E;
    s->dma.ch[0].abus = 0x1500;
    s->dma.ch[0].hdma_reload_abus = 0x1500;
    s->dma.ch[0].hdma_reload_abank = 0x7E;
    s->dma.ch[0].ibank = 0x7E;
    s->dma.hdmaen = 0x01;
    s->ppu.vmadd = 0x0300;

    snes_hdma_init_frame(s);
    snes_hdma_line(s); /* header + indirect load + transfer 40/41 */
    T_CHECK_EQ(s->ppu.vram[0x600], 0x40);
    T_CHECK_EQ(s->ppu.vram[0x601], 0x41);
    T_CHECK_EQ(s->dma.ch[0].hdma_ind_addr, 0x1602u);
    T_CHECK_EQ(s->dma.ch[0].hdma_ind_bank, 0x7Eu);
    snes_hdma_line(s); /* repeat: transfer 42/43 from the next word */
    T_CHECK_EQ(s->ppu.vram[0x602], 0x42);
    T_CHECK_EQ(s->ppu.vram[0x603], 0x43);
    /* table pointer: header(2) + indirect address(2), data not in table */
    T_CHECK_EQ(s->dma.ch[0].abus, 0x1504u);
    emu_core_supersnes()->destroy(&s->base);
}

/* Register path: DMA channel programming through $43x0-6, HDMA enable via
 * $420C, and the GP DMA $420B regression (transfer executes immediately;
 * NMITIMEN must not be clobbered by $420B/$420C writes). */
static void hdma_register_path(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    s->mem.wram[0x1700] = 0x50;
    s->mem.wram[0x1701] = 0x51;

    /* NMI enable on ($4200 bit 7) */
    snes_bus_write(s, 0x004200u, 0x80u);
    /* GP DMA channel 0: mode 1 -> $2118/9, src WRAM $1700, count 2 */
    snes_bus_write(s, 0x004300u, 0x01u);
    snes_bus_write(s, 0x004301u, 0x18u);
    snes_bus_write(s, 0x004302u, 0x00u);
    snes_bus_write(s, 0x004303u, 0x17u);
    snes_bus_write(s, 0x004304u, 0x7Eu);
    snes_bus_write(s, 0x004305u, 0x02u);
    snes_bus_write(s, 0x004306u, 0x00u);
    s->ppu.vmadd = 0x0400;
    snes_bus_write(s, 0x00420Bu, 0x01u); /* run GP DMA now */
    T_CHECK_EQ(s->ppu.vram[0x800], 0x50);
    T_CHECK_EQ(s->ppu.vram[0x801], 0x51);
    T_CHECK_EQ(s->dma.mdmaen, 0u);      /* self-cleared */
    T_CHECK_EQ(s->mem.nmitimen & 0x80u, 0x80u); /* NMI enable preserved */

    /* HDMA through the register path: mode 0 (unit 1) to $2119, one line */
    snes_bus_write(s, 0x004300u, 0x00u);
    snes_bus_write(s, 0x004301u, 0x19u);
    snes_bus_write(s, 0x004302u, 0x00u);
    snes_bus_write(s, 0x004303u, 0x17u);
    s->mem.wram[0x1700] = 0x01; /* header byte: 1 line, no repeat */
    s->mem.wram[0x1701] = 0x60;
    s->ppu.vmadd = 0x0500;
    snes_bus_write(s, 0x00420Cu, 0x01u);
    T_CHECK_EQ(s->dma.hdmaen, 0x01u);
    T_CHECK_EQ(s->mem.nmitimen & 0x80u, 0x80u); /* still preserved */
    snes_hdma_init_frame(s);
    snes_hdma_line(s);
    T_CHECK_EQ(s->ppu.vram[0xA01], 0x60); /* $2119 commits high byte */
    emu_core_supersnes()->destroy(&s->base);
}

/* V=0 reload: the table pointer returns to the value programmed through
 * $43x2-4 and the per-frame state resets. */
static void hdma_reload_at_frame_start(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    s->mem.wram[0x1800] = 0x83; /* repeat, 3 lines */
    s->mem.wram[0x1801] = 0xAA;
    s->dma.ch[0].params = 0x00;
    s->dma.ch[0].bbus = 0x19;
    s->dma.ch[0].abank = 0x7E;
    s->dma.ch[0].abus = 0x1800;
    s->dma.ch[0].hdma_reload_abus = 0x1800;
    s->dma.ch[0].hdma_reload_abank = 0x7E;
    s->dma.hdmaen = 0x01;
    s->ppu.vmadd = 0x0600;

    snes_hdma_init_frame(s);
    snes_hdma_line(s); /* line 1: header + data transfer */
    snes_hdma_line(s); /* line 2: repeat -> transfer next byte */
    T_CHECK_EQ(s->dma.ch[0].abus, 0x1803u); /* header + 2 data bytes */
    T_CHECK_EQ(s->dma.ch[0].hdma_lines_left, 1u);

    snes_hdma_init_frame(s); /* frame wrap */
    T_CHECK_EQ(s->dma.ch[0].abus, 0x1800u); /* reloaded */
    T_CHECK_EQ(s->dma.ch[0].abank, 0x7Eu);
    T_CHECK_EQ(s->dma.ch[0].hdma_lines_left, 0u);
    T_CHECK_EQ(s->dma.ch[0].hdma_finished, 0u);
    T_CHECK_EQ(s->dma.ch[0].hdma_ind_loaded, 0u);
    emu_core_supersnes()->destroy(&s->base);
}

T_SUITE_BEGIN(snes_hdma)
{ "direct_to_vram", hdma_direct_to_vram },
{ "repeat_bytes", hdma_repeat_bytes },
{ "indirect", hdma_indirect },
{ "register_path", hdma_register_path },
{ "reload_at_frame_start", hdma_reload_at_frame_start },
T_SUITE_END

T_SUITE_REG(snes_hdma)
