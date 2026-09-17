/*
 * mgbax DMA: 4 channels. Immediate (channel 0) and VBlank/HBlank-triggered
 * channels are serviced by the frame loop via trigger flags; the special
 * trigger (3) services sound-FIFO requests via gba_dma_fifo_request. Word/
 * halfword units, increment/reload address control, repeat mode.
 */
#include "gba.h"

#include <string.h>

void gba_dma_reset(gba_dma *d)
{
    memset(d, 0, sizeof *d);
}

uint16_t gba_dma_read_ctrl(gba_dma *d, uint32_t addr)
{
    uint8_t ch = (uint8_t)(((addr - 0x040000B0u) / 12u) & 3u);
    uint32_t off = addr - (0x040000B0u + (uint32_t)ch * 12u);
    switch (off) {
    case 0: return (uint16_t)(d->sad[ch] & 0xFFFFu);
    case 2: return (uint16_t)(d->sad[ch] >> 16);
    case 4: return (uint16_t)(d->dad[ch] & 0xFFFFu);
    case 6: return (uint16_t)(d->dad[ch] >> 16);
    case 8: return d->count[ch];
    default: return d->ctrl[ch];
    }
}

static uint32_t dma_transfer(gba_t *g, int ch)
{
    gba_dma *d = &g->dma;
    uint16_t ctrl = d->ctrl[ch];
    int word = (ctrl & (1u << 10u)) != 0;
    int src_fixed = ((ctrl >> 7u) & 3u) == 2u;
    int dst_fixed = ((ctrl >> 5u) & 3u) == 2u;
    int dst_reload = ((ctrl >> 5u) & 3u) == 3u;
    uint32_t count = d->count[ch] == 0u ? (ch == 0u ? 0x4000u : 0x10000u)
                                        : (uint32_t)d->count[ch];
    uint32_t sad = d->sad[ch];
    uint32_t dad = d->dad[ch];
    uint32_t cycles = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (word) {
            uint32_t v = gba_bus_read32(g, sad);
            gba_mem_write32(g, dad, v);
            cycles += 6u;
            if (!src_fixed)
                sad += 4u;
            if (!dst_fixed)
                dad += 4u;
        } else {
            uint16_t v = gba_bus_read16(g, sad);
            gba_mem_write16(g, dad, v);
            cycles += 4u;
            if (!src_fixed)
                sad += 2u;
            if (!dst_fixed)
                dad += 2u;
        }
    }
    d->sad[ch] = sad;
    if (dst_reload)
        d->dad[ch] = d->dad_latch[ch]; /* DST_RELOAD: restore enabled DAD */
    else
        d->dad[ch] = dad;
    d->count[ch] = (uint16_t)(count & 0xFFFFu);
    if (ctrl & (1u << 9u)) {
        d->count[ch] = d->count_latch[ch]; /* repeat: restore latch */
    } else {
        d->ctrl[ch] = (uint16_t)(ctrl & (uint16_t)~0x8000u); /* disable */
        d->enabled[ch] = 0;
    }
    return cycles;
}

uint32_t gba_dma_run(gba_t *g, uint16_t trigger_flags)
{
    uint32_t cycles = 0;
    for (int ch = 0; ch < 4; ch++) {
        if (!g->dma.enabled[ch])
            continue;
        uint16_t ctrl = g->dma.ctrl[ch];
        uint16_t trigger = (uint16_t)((ctrl >> 12) & 3u);
        uint16_t want;
        switch (trigger) {
        case 0: want = 0xFFFFu; break;          /* immediate: handled on write */
        case 1: want = 1u; break;               /* VBlank */
        case 2: want = 2u; break;               /* HBlank */
        default: continue;                      /* special: FIFO requests */
        }
        if (!(trigger_flags & want))
            continue;
        cycles += dma_transfer(g, ch);
    }
    return cycles;
}

/* Sound-FIFO DMA request (SOUNDFIFO A/B). Per the hardware contract only
 * channels 1/2 can service FIFO A and channels 2/3 FIFO B; the request
 * transfers exactly 4 words with fixed destination into the FIFO. The
 * transfer count register is ignored. Destination control bits are ignored
 * (the FIFO consumes the data). */
void gba_dma_fifo_request(gba_t *g, int fifo)
{
    static const int ch_by_fifo[2][2] = { { 1, 2 }, { 2, 3 } };
    if (fifo < 0 || fifo > 1)
        return;
    for (int ci = 0; ci < 2; ci++) {
        int ch = ch_by_fifo[fifo][ci];
        gba_dma *d = &g->dma;
        if (!d->enabled[ch])
            continue;
        if (((d->ctrl[ch] >> 12) & 3u) != 3u)
            continue; /* not a FIFO DMA channel */
        {
            uint32_t sad = d->sad[ch];
            int src_fixed = ((d->ctrl[ch] >> 7) & 3u) == 2u;
            for (int i = 0; i < 4; i++) {
                uint32_t v = gba_bus_read32(g, sad);
                gba_apu_fifo_write32(g, fifo, v);
                if (!src_fixed)
                    sad += 4u;
            }
            d->sad[ch] = sad;
        }
        return; /* the highest-priority matching channel services it */
    }
}

void gba_dma_write(gba_t *g, uint32_t addr, uint16_t v)
{
    uint8_t ch = (uint8_t)(((addr - 0x040000B0u) / 12u) & 3u);
    uint32_t off = addr - (0x040000B0u + (uint32_t)ch * 12u);
    switch (off) {
    case 0: g->dma.sad[ch] = (g->dma.sad[ch] & 0xFFFF0000u) | v; return;
    case 2: g->dma.sad[ch] = (g->dma.sad[ch] & 0x0000FFFFu) | ((uint32_t)v << 16); return;
    case 4: g->dma.dad[ch] = (g->dma.dad[ch] & 0xFFFF0000u) | v; return;
    case 6: g->dma.dad[ch] = (g->dma.dad[ch] & 0x0000FFFFu) | ((uint32_t)v << 16); return;
    case 8:
        g->dma.count_latch[ch] = v;
        g->dma.count[ch] = v;
        return;
    case 10:
        break; /* control: handled below */
    default:
        return;
    }
    {
        uint16_t old = g->dma.ctrl[ch];
        g->dma.ctrl[ch] = v;
        if (!(old & 0x8000u) && (v & 0x8000u)) {
            g->dma.enabled[ch] = 1;
            g->dma.count[ch] = g->dma.count_latch[ch];
            g->dma.dad_latch[ch] = g->dma.dad[ch];
            if (((v >> 12) & 3u) == 0u) {
                uint32_t cycles = dma_transfer(g, ch);
                g->total_cycles += cycles;
            }
        }
    }
}
