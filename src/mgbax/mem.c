/*
 * mgbax memory system: region decode, mirrors, open-bus behavior
 * (documented simplification: unmapped reads return 0), IO dispatch.
 */
#include "gba.h"

uint8_t gba_mem_read8(gba_t *g, uint32_t addr)
{
    addr &= 0x0FFFFFFFu;
    uint8_t region = (uint8_t)(addr >> 24);
    switch (region) {
    case 0x00: /* BIOS: not bundled; HLE reads return 0 */
        return 0x00u;
    case 0x02: return g->mem.ewram[addr & 0x3FFFFu];
    case 0x03: return g->mem.iwram[addr & 0x7FFFu];
    case 0x04:
        return 0; /* 8-bit IO reads: handled at 16-bit level */
    case 0x05: return g->mem.pal[addr & 0x3FFu];
    case 0x06: {
        uint32_t off = addr & 0x1FFFFu;
        if (off >= 0x18000u)
            off -= 0x8000u; /* OBJ mirror */
        return g->mem.vram[off];
    }
    case 0x07: return g->mem.oam[addr & 0x3FFu];
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D:
        return gba_cart_read8(&g->cart, addr);
    case 0x0E: case 0x0F:
        return gba_cart_read8(&g->cart, 0x0E000000u | (addr & 0xFFFFu));
    default:
        return 0;
    }
}

uint16_t gba_mem_read16(gba_t *g, uint32_t addr)
{
    addr &= ~1u;
    if ((addr >> 24) == 0x04u && addr < 0x04000400u)
        return gba_io_read16(g, addr);
    return (uint16_t)((uint16_t)gba_mem_read8(g, addr) |
                      ((uint16_t)gba_mem_read8(g, addr + 1u) << 8));
}

uint32_t gba_mem_read32(gba_t *g, uint32_t addr)
{
    return (uint32_t)gba_mem_read16(g, addr) |
           ((uint32_t)gba_mem_read16(g, addr + 2u) << 16);
}

void gba_mem_write8(gba_t *g, uint32_t addr, uint8_t v)
{
    addr &= 0x0FFFFFFFu;
    uint8_t region = (uint8_t)(addr >> 24);
    switch (region) {
    case 0x02: g->mem.ewram[addr & 0x3FFFFu] = v; break;
    case 0x03: g->mem.iwram[addr & 0x7FFFu] = v; break;
    case 0x04: {
        /* 8-bit IO writes: only the SOUNDFIFO A/B region accepts byte
         * writes on hardware; other IO registers are 16-bit latches */
        uint32_t off = addr & 0x3FFu;
        if (off >= 0x060u && off < 0x0B0u)
            gba_apu_io_write8(&g->apu, 0x04000000u + addr, v);
        break;
    }
    case 0x05: g->mem.pal[addr & 0x3FFu] = v; break;
    case 0x06: {
        uint32_t off = addr & 0x1FFFFu;
        if (off >= 0x18000u)
            off -= 0x8000u;
        g->mem.vram[off] = v;
        break;
    }
    case 0x07: g->mem.oam[addr & 0x3FFu] = v; break;
    case 0x0E: case 0x0F:
        gba_cart_write8(&g->cart, 0x0E000000u | (addr & 0xFFFFu), v);
        break;
    default:
        break; /* ROM/BIOS writes ignored */
    }
}

void gba_mem_write16(gba_t *g, uint32_t addr, uint16_t v)
{
    addr &= ~1u;
    if ((addr >> 24) == 0x04u && addr < 0x04000400u) {
        gba_io_write16(g, addr, v);
        return;
    }
    gba_mem_write8(g, addr, (uint8_t)(v & 0xFFu));
    gba_mem_write8(g, addr + 1u, (uint8_t)(v >> 8));
}

void gba_mem_write32(gba_t *g, uint32_t addr, uint32_t v)
{
    /* SOUNDFIFO A/B: a 32-bit store is a single 4-byte append event
     * (the DMA request level is evaluated once, after the whole word) */
    if ((addr & 3u) == 0u && (addr >> 24) == 0x04u && addr < 0x04000400u) {
        uint32_t off = addr & 0x3FFu;
        if (off >= 0x0A0u && off < 0x0B0u) {
            int f = (int)((off >> 2) & 1u); /* 0A0/0A8 -> A, 0A4/0AC -> B */
            gba_apu_fifo_write32(g, f, v);
            if (g->apu.fifo_count[f] <= 16u)
                gba_dma_fifo_request(g, f);
            return;
        }
    }
    gba_mem_write16(g, addr, (uint16_t)(v & 0xFFFFu));
    gba_mem_write16(g, addr + 2u, (uint16_t)(v >> 16));
}

/* CPU-side word read: the access itself is word-aligned; the CPU applies
 * the ROR-by-(addr&3)*8 rotation for unaligned LDR per the ARM7TDMI bus
 * contract (see cpu.c). */
uint32_t gba_bus_read32(gba_t *g, uint32_t addr)
{
    return gba_mem_read32(g, addr & ~3u);
}

uint16_t gba_bus_read16(gba_t *g, uint32_t addr)
{
    return gba_mem_read16(g, addr);
}

void gba_request_irq(gba_t *g, uint16_t bits)
{
    g->mem.if_reg |= bits;
}

/* ---- IO dispatch ---- */

uint16_t gba_io_read16(gba_t *g, uint32_t addr)
{
    addr &= 0x3FFu;
    if (addr < 0x0040u)
        return gba_ppu_io_read16(g, 0x04000000u + addr);
    if (addr >= 0x00B0u && addr < 0x00E0u)
        return gba_dma_read_ctrl(&g->dma, 0x04000000u + addr);
    if (addr >= 0x0100u && addr < 0x0110u)
        return gba_timers_read(&g->timers, 0x04000000u + addr);
    if (addr >= 0x0060u && addr < 0x00B0u)
        return gba_apu_io_read16(&g->apu, 0x04000000u + addr);
    if (addr == 0x0130u) {
        /* KEYINPUT: active-low */
        uint16_t v = 0x03FFu;
        uint32_t b = g->buttons;
        if (b & 0x0001u) v &= (uint16_t)~0x0001u; /* A */
        if (b & 0x0002u) v &= (uint16_t)~0x0002u; /* B */
        if (b & 0x0004u) v &= (uint16_t)~0x0004u; /* Select */
        if (b & 0x0008u) v &= (uint16_t)~0x0008u; /* Start */
        if (b & 0x0010u) v &= (uint16_t)~0x0010u; /* Right */
        if (b & 0x0020u) v &= (uint16_t)~0x0020u; /* Left */
        if (b & 0x0040u) v &= (uint16_t)~0x0040u; /* Up */
        if (b & 0x0080u) v &= (uint16_t)~0x0080u; /* Down */
        if (b & 0x0100u) v &= (uint16_t)~0x0100u; /* R */
        if (b & 0x0200u) v &= (uint16_t)~0x0200u; /* L */
        return v;
    }
    if (addr == 0x0200u)
        return g->mem.ie;
    if (addr == 0x0202u)
        return g->mem.if_reg;
    if (addr == 0x0204u)
        return g->mem.waitcnt;
    if (addr == 0x0208u)
        return g->mem.imiu;
    if (addr < 0x0400u)
        return (uint16_t)(g->mem.io[addr] | (g->mem.io[addr + 1u] << 8));
    return 0;
}

void gba_io_write16(gba_t *g, uint32_t addr, uint16_t v)
{
    addr &= 0x3FFu;
    if (addr < 0x0040u) {
        gba_ppu_io_write16(g, 0x04000000u + addr, v);
        return;
    }
    if (addr >= 0x00B0u && addr < 0x00E0u) {
        gba_dma_write(g, 0x04000000u + addr, v);
        return;
    }
    if (addr >= 0x0100u && addr < 0x0110u) {
        gba_timers_write(g, 0x04000000u + addr, v);
        return;
    }
    if (addr >= 0x0060u && addr < 0x00B0u) {
        gba_apu_io_write16(&g->apu, 0x04000000u + addr, v);
        return;
    }
    switch (addr) {
    case 0x0200:
        g->mem.ie = v;
        break;
    case 0x0202:
        g->mem.if_reg &= (uint16_t)~v; /* acknowledge by writing 1s */
        break;
    case 0x0204:
        g->mem.waitcnt = v;
        break;
    case 0x0208:
        g->mem.imiu = (uint8_t)(v & 1u);
        break;
    default:
        break;
    }
    if (addr < 0x0400u) {
        g->mem.io[addr] = (uint8_t)(v & 0xFFu);
        g->mem.io[addr + 1u] = (uint8_t)(v >> 8);
    }
}

void gba_update_input_reg(gba_t *g)
{
    (void)g; /* KEYINPUT is computed on read */
}
