/*
 * mgbax timers: 4 channels, prescalers 1/64/256/1024, cascade mode,
 * overflow IRQs. Timers tick on CPU-cycle counts delivered by the frame
 * loop; cascade chains ch0->1->2->3 per the GBA specification.
 */
#include <string.h>

#include "gba.h"

static const uint32_t prescale_div[4] = { 1u, 64u, 256u, 1024u };

void gba_timers_reset(gba_timers *t)
{
    memset(t, 0, sizeof *t);
}

/* overflow notifications are consumed by the APU each step; reset clears */

static void timer_overflow(gba_t *g, int ch)
{
    g->timers.counter[ch] = g->timers.reload[ch];
    g->timers.ovf_bits |= (uint8_t)(1u << ch); /* APU FIFO sampling clock */
    if (ch < 3 && (g->timers.ctrl[ch + 1u] & (1u << 2u))) {
        /* cascade into next channel */
        uint32_t v = (uint32_t)g->timers.counter[ch + 1u] + 1u;
        if (v > 0xFFFFu) {
            g->timers.counter[ch + 1u] = g->timers.reload[ch + 1u];
            g->timers.ovf_bits |= (uint8_t)(1u << (ch + 1u));
            if (g->timers.ctrl[ch + 1u] & (1u << 6u))
                gba_request_irq(g, (uint16_t)(0x0008u << (ch + 1u)));
        } else {
            g->timers.counter[ch + 1u] = (uint16_t)v;
        }
    }
    if (g->timers.ctrl[ch] & (1u << 6u))
        gba_request_irq(g, (uint16_t)(0x0008u << ch));
}

void gba_timers_step(gba_t *g, uint32_t cycles)
{
    for (int ch = 0; ch < 4; ch++) {
        uint16_t ctrl = g->timers.ctrl[ch];
        if (!(ctrl & (1u << 7u)))
            continue; /* disabled */
        if ((ctrl & (1u << 2u)) && ch != 0) {
            /* cascade: counts on overflow of the previous channel; the
             * overflow path above already incremented us. But cascade
             * chains must skip the prescaler path entirely. */
            continue;
        }
        uint32_t div = prescale_div[ctrl & 3u];
        g->timers.prescaler[ch] += cycles;
        while (g->timers.prescaler[ch] >= div) {
            g->timers.prescaler[ch] -= div;
            uint32_t v = (uint32_t)g->timers.counter[ch] + 1u;
            if (v > 0xFFFFu)
                timer_overflow(g, ch);
            else
                g->timers.counter[ch] = (uint16_t)v;
        }
    }
}

uint16_t gba_timers_read(gba_timers *t, uint32_t addr)
{
    uint8_t ch = (uint8_t)((addr >> 2) & 3u);
    if (addr & 2u)
        return t->ctrl[ch];
    return t->counter[ch];
}

void gba_timers_write(gba_t *g, uint32_t addr, uint16_t v)
{
    uint8_t ch = (uint8_t)((addr >> 2) & 3u);
    if (addr & 2u) {
        uint16_t old = g->timers.ctrl[ch];
        g->timers.ctrl[ch] = v;
        if (!(old & (1u << 7u)) && (v & (1u << 7u))) {
            /* enable: counter reloads */
            g->timers.counter[ch] = g->timers.reload[ch];
            g->timers.prescaler[ch] = 0;
        }
        return;
    }
    g->timers.reload[ch] = v;
    g->timers.counter[ch] = v;
}
