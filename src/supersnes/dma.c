/*
 * supersnes DMA: 8 channels shared between general-purpose DMA ($420B)
 * and HDMA ($420C). Control register $43x0 per hardware: bit 7 direction
 * (0 = A->B), bit 6 indirect (HDMA), bit 4 fixed address, bit 3 decrement,
 * bits 0-2 transfer mode (0: 1 byte, 1/2: 2 bytes, 3/4: 4 bytes; 5-7 are
 * invalid on hardware and behave as 1 byte here). The byte-to-register
 * pattern of modes 2-4 is simplified to consecutive B-bus addresses
 * (documented approximation). Each transferred byte costs 8 master clocks
 * (slow-bus approximation).
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

static uint32_t dma_abus(snes_t *s, const snes_dma_channel *ch)
{
    (void)s;
    return ((uint32_t)ch->abank << 16) | ch->abus;
}

static uint8_t dma_unit(uint8_t mode)
{
    switch (mode) {
    case 1: case 2: return 2;
    case 3: case 4: return 4;
    default: return 1; /* 0 and the invalid 5-7 */
    }
}

uint32_t snes_dma_run(snes_t *s)
{
    uint32_t master = 0;
    for (int i = 0; i < 8; i++) {
        if (!(s->dma.mdmaen & (1u << i)))
            continue;
        snes_dma_channel *ch = &s->dma.ch[i];
        uint8_t dir = (uint8_t)(ch->params & 0x80u); /* 0 = A->B, 1 = B->A */
        uint8_t fixed = (uint8_t)(ch->params & 0x10u);
        uint8_t dec = (uint8_t)(ch->params & 0x08u);
        uint8_t unit = dma_unit((uint8_t)(ch->params & 7u));
        uint16_t count = ch->count == 0u ? 0x10000u : ch->count;

        while (count != 0u) {
            uint8_t done = 0;
            while (done < unit && count != 0u) {
                uint8_t b;
                if (dir == 0u) {
                    b = snes_bus_read(s, dma_abus(s, ch));
                    dma_bbus_write(s, (uint8_t)(ch->bbus + done), b);
                } else {
                    b = dma_bbus_read(s, (uint8_t)(ch->bbus + done));
                    snes_bus_write(s, dma_abus(s, ch), b);
                }
                master += 8u;
                /* address stepping: bit4 = fixed, bit3 = decrement */
                if (fixed)
                    ;
                else if (dec)
                    ch->abus = (uint16_t)(ch->abus - 1u);
                else
                    ch->abus = (uint16_t)(ch->abus + 1u);
                done++;
                count--;
            }
        }
    }
    s->dma.mdmaen = 0; /* channels self-clear after the transfer */
    return master;
}

/* ---- HDMA ---- */

void snes_hdma_init_frame(snes_t *s)
{
    for (int i = 0; i < 8; i++) {
        snes_dma_channel *ch = &s->dma.ch[i];
        /* at V=0 the A2 table pointer reloads from the value last
         * programmed through $43x2-4 */
        ch->abus = ch->hdma_reload_abus;
        ch->abank = ch->hdma_reload_abank;
        ch->hdma_finished = 0;
        ch->hdma_lines_left = 0;
        ch->hdma_repeat = 0;
        ch->hdma_xfer = 0;
        ch->hdma_ind_loaded = 0;
        ch->hdma_ind_addr = 0;
        ch->hdma_ind_bank = ch->ibank;
    }
}

static void hdma_channel_line(snes_t *s, int i)
{
    snes_dma_channel *ch = &s->dma.ch[i];
    if (!(s->dma.hdmaen & (1u << i)) || ch->hdma_finished)
        return;
    uint8_t unit = dma_unit((uint8_t)(ch->params & 7u));
    uint8_t indirect = (uint8_t)(ch->params & 0x40u);

    if (ch->hdma_lines_left == 0u) {
        /* read the block header: 1 byte for 1-byte units, 2 bytes
         * otherwise; bit 7/15 = repeat, low bits = line count */
        uint32_t lines;
        if (unit == 1u) {
            uint8_t h = snes_bus_read(s, dma_abus(s, ch));
            ch->abus = (uint16_t)(ch->abus + 1u);
            lines = h & 0x7Fu;
            ch->hdma_repeat = (uint8_t)((h & 0x80u) ? 1u : 0u);
        } else {
            uint8_t lo = snes_bus_read(s, dma_abus(s, ch));
            ch->abus = (uint16_t)(ch->abus + 1u);
            uint8_t hi = snes_bus_read(s, dma_abus(s, ch));
            ch->abus = (uint16_t)(ch->abus + 1u);
            uint16_t h = (uint16_t)(lo | ((uint16_t)hi << 8));
            lines = h & 0x7FFFu;
            ch->hdma_repeat = (uint8_t)((h & 0x8000u) ? 1u : 0u);
        }
        if (lines == 0u) {
            ch->hdma_finished = 1;
            return;
        }
        ch->hdma_lines_left = (uint16_t)lines;
        ch->hdma_xfer = 1; /* always transfer on the first line */
        ch->hdma_ind_loaded = 0;
    } else {
        ch->hdma_xfer = ch->hdma_repeat;
    }
    ch->hdma_lines_left--;

    if (!ch->hdma_xfer)
        return;

    if (indirect != 0u) {
        if (!ch->hdma_ind_loaded) {
            uint8_t lo = snes_bus_read(s, dma_abus(s, ch));
            ch->abus = (uint16_t)(ch->abus + 1u);
            uint8_t hi = snes_bus_read(s, dma_abus(s, ch));
            ch->abus = (uint16_t)(ch->abus + 1u);
            ch->hdma_ind_addr = (uint16_t)(lo | ((uint16_t)hi << 8));
            ch->hdma_ind_bank = ch->ibank;
            if (unit == 4u) {
                /* transfer modes 3/4 ("full indirect"): the table also
                 * carries an explicit bank byte; modes 0-2 use $43x7 */
                ch->hdma_ind_bank = snes_bus_read(s, dma_abus(s, ch));
                ch->abus = (uint16_t)(ch->abus + 1u);
            }
            ch->hdma_ind_loaded = 1;
        }
        uint32_t src = ((uint32_t)ch->hdma_ind_bank << 16) | ch->hdma_ind_addr;
        for (uint8_t d = 0; d < unit; d++) {
            uint8_t b = snes_bus_read(s, src + d);
            dma_bbus_write(s, (uint8_t)(ch->bbus + d), b);
        }
        ch->hdma_ind_addr = (uint16_t)(ch->hdma_ind_addr + unit);
    } else {
        for (uint8_t d = 0; d < unit; d++) {
            uint8_t b = snes_bus_read(s, dma_abus(s, ch));
            ch->abus = (uint16_t)(ch->abus + 1u);
            dma_bbus_write(s, (uint8_t)(ch->bbus + d), b);
        }
    }
    /* Note: the master-clock cost of HDMA is not subtracted from CPU
     * execution time (documented timing approximation). */
}

void snes_hdma_line(snes_t *s)
{
    for (int i = 0; i < 8; i++)
        hdma_channel_line(s, i);
}
