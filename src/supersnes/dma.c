/*
 * supersnes DMA: general purpose channels 0-7, transfer modes 0-3
 * (1 byte, 2 bytes, 2 bytes 32-bit units, 4 bytes). HDMA is stored but not
 * implemented (documented limitation). Each transferred byte costs 8 master
 * clocks (slow-bus approximation).
 */
#include "snes.h"

#include <string.h>

void snes_dma_reset(snes_dma *d)
{
    memset(d, 0, sizeof *d);
}

static uint8_t dma_bbus_read(snes_t *s, uint8_t bbus)
{
    uint32_t addr = 0x000000u | 0x2100u | bbus; /* bank 0, PPU regs */
    if (bbus >= 0x18u && bbus <= 0x19u)
        return snes_ppu_read(s, 0x2100u + (uint8_t)(bbus - 0x18u)); /* OAM */
    if (bbus == 0x22u)
        return snes_ppu_read(s, 0x2122u); /* CGRAM */
    if (bbus == 0x39u || bbus == 0x3Bu)
        return snes_ppu_read(s, 0x2100u + (uint8_t)(bbus - 0x18u)); /* VRAM */
    return snes_bus_read(s, addr);
}

static void dma_bbus_write(snes_t *s, uint8_t bbus, uint8_t v)
{
    snes_bus_write(s, 0x000000u | 0x2100u | bbus, v);
}

uint32_t snes_dma_run(snes_t *s)
{
    uint32_t master = 0;
    for (int i = 0; i < 8; i++) {
        if (!(s->dma.mdmaen & (1u << i)))
            continue;
        snes_dma_channel *ch = &s->dma.ch[i];
        uint8_t dir = (uint8_t)(ch->params & 0x80u); /* 0 = A->B, 1 = B->A */
        uint8_t step = (uint8_t)((ch->params >> 3) & 3u);
        uint8_t mode = (uint8_t)(ch->params & 7u);
        /* transfer unit sizes: 0: 1 byte; 1: 2 bytes (hi/lo pair);
         * 2,6: 2 bytes (word); 3,7: 4 bytes (long); 4: 4 bytes; 5: 2 bytes */
        uint8_t unit;
        switch (mode) {
        case 1: case 2: case 5: case 6: unit = 2; break;
        case 3: case 4: case 7: unit = 4; break;
        default: unit = 1; break;
        }
        uint16_t count = ch->count == 0u ? 0x10000u : ch->count;

        while (count != 0u) {
            uint8_t done = 0;
            while (done < unit && count != 0u) {
                uint8_t b;
                if (dir == 0u) {
                    b = snes_bus_read(s, ((uint32_t)ch->abank << 16) | ch->abus);
                    dma_bbus_write(s, (uint8_t)(ch->bbus + done), b);
                } else {
                    b = dma_bbus_read(s, (uint8_t)(ch->bbus + done));
                    snes_bus_write(s, ((uint32_t)ch->abank << 16) | ch->abus, b);
                }
                master += 8u;
                /* address stepping */
                int8_t delta = 1;
                if (step == 1u)
                    delta = 0;
                else if (step == 2u || step == 3u)
                    delta = -1;
                ch->abus = (uint16_t)(ch->abus + (uint16_t)delta);
                done++;
                count--;
            }
        }
    }
    s->dma.mdmaen &= 0xF0u; /* channels disabled after transfer */
    return master;
}
