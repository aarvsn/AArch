/*
 * mgbax APU: direct-sound FIFO channels A/B plus the PSG (2 squares + noise).
 *
 * FIFO channels: SOUNDFIFO A ($40000A0) and B ($40000A4) queue up to 32
 * bytes of 8-bit signed PCM. One byte is popped into the channel's current
 * sample whenever the timer selected in SOUNDCNT_H (timer 0/1) overflows.
 * When a FIFO drops to 16 or fewer queued bytes a DMA request is raised and
 * serviced by the matching trigger=3 DMA channel (A: ch1/ch2, B: ch2/ch3),
 * transferring 4 words with fixed destination.
 *
 * Mixing: per output sample the PSG sum (gated by SOUNDCNT_X bit7, scaled
 * by SOUNDCNT_L per-side volume (v+1)/8 and SOUNDCNT_H 25/50/100%) is added
 * to FIFO A/B scaled by their SOUNDCNT_H volume bits and routed by their
 * left/right enable bits. Saturated to int16. SOUNDBIAS is a DC offset and
 * is not applied to the digital output (documented). The 4-bit PSG master
 * volume in SOUNDCNT_L is modeled as (v+1)/8 gain per side.
 */
#include "gba.h"

#include <stddef.h>
#include <string.h>

static const uint8_t duty_table[4][8] = {
    { 0, 0, 1, 1, 0, 0, 0, 0 },
    { 1, 1, 0, 0, 0, 0, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }
};

void gba_apu_init(gba_apu *a, int16_t *out, size_t cap)
{
    memset(a, 0, sizeof *a);
    a->out = out;
    a->out_cap = cap;
    gba_apu_reset(a);
}

void gba_apu_reset(gba_apu *a)
{
    int16_t *out = a->out;
    size_t cap = a->out_cap;
    memset(a, 0, sizeof *a);
    a->out = out;
    a->out_cap = cap;
    a->noise_lfsr = 0x7FFFu;
    a->sq_period[0] = 2048u; /* raw freq 0: 2048 - 0 */
    a->sq_period[1] = 2048u;
}

/* ---- FIFO helpers ---- */

static void fifo_append(gba_apu *a, int f, const uint8_t *bytes, int n)
{
    for (int i = 0; i < n; i++) {
        if (a->fifo_count[f] >= GBA_FIFO_DEPTH)
            break; /* hardware discards writes to a full FIFO */
        a->fifo[f][a->fifo_count[f]++] = bytes[i];
    }
}

/* Raw 4-byte little-endian append used by the DMA refill path. Never
 * generates further DMA requests (no recursion). */
void gba_apu_fifo_write32(gba_t *g, int fifo, uint32_t v)
{
    uint8_t b[4];
    if (fifo < 0 || fifo > 1)
        return;
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
    fifo_append(&g->apu, fifo, b, 4);
}

/* CPU-side append: after the write the DMA request level is evaluated
 * (request when the FIFO holds 16 or fewer bytes). */
static void fifo_cpu_appended(gba_t *g, int f)
{
    if (g->apu.fifo_count[f] <= 16u)
        gba_dma_fifo_request(g, f);
}

/* pop one byte for FIFO f (called on selected-timer overflow) */
static void fifo_pop(gba_t *g, int f)
{
    gba_apu *a = &g->apu;
    if (a->fifo_count[f] == 0u) {
        a->fifo_cur[f] = 0; /* empty FIFO outputs silence */
        a->fifo_has[f] = 0;
        return;
    }
    a->fifo_cur[f] = (int8_t)a->fifo[f][0];
    a->fifo_has[f] = 1;
    a->fifo_count[f]--;
    if (a->fifo_count[f] > 0)
        memmove(a->fifo[f], a->fifo[f] + 1, a->fifo_count[f]);
    if (a->fifo_count[f] <= 16u)
        gba_dma_fifo_request(g, f);
}

/* ---- PSG ---- */

static void noise_restart(gba_apu *a)
{
    a->noise_active = 1;
    a->noise_volume = a->noise_env_vol;
    a->noise_timer = 0;
    a->noise_lfsr = 0x7FFFu;
}

static void sq_restart(gba_apu *a, int i)
{
    a->sq_active[i] = 1;
    a->sq_volume[i] = a->sq_env_vol[i];
    a->sq_freq_timer[i] = 0;
    a->sq_duty_pos[i] = 0;
}

/* ---- stepping ---- */

void gba_apu_step(gba_t *g, uint32_t cycles)
{
    gba_apu *a = &g->apu;
    int i;

    for (i = 0; i < 2; i++) {
        if (!a->sq_active[i])
            continue;
        /* frequency timer counts CPU cycles (2048 - raw) per spec */
        a->sq_freq_timer[i] = (uint16_t)(a->sq_freq_timer[i] + cycles);
        while (a->sq_freq_timer[i] >= a->sq_period[i]) {
            a->sq_freq_timer[i] =
                (uint16_t)(a->sq_freq_timer[i] - a->sq_period[i]);
            a->sq_duty_pos[i] = (uint8_t)((a->sq_duty_pos[i] + 1u) & 7u);
        }
    }
    /* noise: LFSR shift clock = 524288 Hz / (R+1) / 2^S
     * -> period in CPU cycles = 32 * (R+1) << S (noise_period precomputed) */
    if (a->noise_active && a->noise_period > 0u) {
        a->noise_timer += cycles;
        while (a->noise_timer >= a->noise_period) {
            a->noise_timer -= a->noise_period;
            uint16_t bit = (uint16_t)(((a->noise_lfsr ^ (a->noise_lfsr >> 1u)) & 1u)
                                      << 15u);
            a->noise_lfsr = (uint16_t)((a->noise_lfsr >> 1u) | bit);
            if (a->noise_width7)
                a->noise_lfsr = (uint16_t)((a->noise_lfsr & (uint16_t)~0x0080u) |
                                           (uint16_t)(bit >> 8u));
        }
    }

    /* FIFO sampling: consume timer overflow notifications */
    {
        uint8_t ovf = g->timers.ovf_bits;
        g->timers.ovf_bits = 0;
        if (ovf != 0u) {
            for (int t = 0; t < 2; t++) {
                if (!(ovf & (1u << t)))
                    continue;
                for (int f = 0; f < 2; f++) {
                    uint32_t sel = (a->soundcnt_h >> (f == 0 ? 10u : 14u)) & 1u;
                    if (sel == (uint32_t)t)
                        fifo_pop(g, f);
                }
            }
        }
    }

    /* output sampling at 32768 Hz (every 512 CPU cycles) */
    a->sample_acc += cycles;
    while (a->sample_acc >= 16777216u / GBA_OUT_RATE) {
        a->sample_acc -= 16777216u / GBA_OUT_RATE;
        int l = 0, r = 0;

        /* PSG: per-channel routing (SOUNDCNT_L), then per-side master
         * volume (v+1)/8, then SOUNDCNT_H 25/50/100%, all gated by the
         * SOUNDCNT_X master enable. Channel scale: 1 unit = 1/1024. */
        if (a->soundcnt_x & 0x80u) {
            int pl = 0, pr = 0;
            if (a->sq_active[0] &&
                duty_table[a->sq_duty[0] & 3u][a->sq_duty_pos[0]]) {
                if (a->soundcnt_l & (1u << 8u))
                    pr += a->sq_volume[0];
                if (a->soundcnt_l & (1u << 12u))
                    pl += a->sq_volume[0];
            }
            if (a->sq_active[1] &&
                duty_table[a->sq_duty[1] & 3u][a->sq_duty_pos[1]]) {
                if (a->soundcnt_l & (1u << 9u))
                    pr += a->sq_volume[1];
                if (a->soundcnt_l & (1u << 13u))
                    pl += a->sq_volume[1];
            }
            if (a->noise_active && (a->noise_lfsr & 1u) == 0u) {
                if (a->soundcnt_l & (1u << 11u))
                    pr += a->noise_volume;
                if (a->soundcnt_l & (1u << 15u))
                    pl += a->noise_volume;
            }
            {
                uint32_t lv = (a->soundcnt_l >> 4) & 7u;
                uint32_t rv = a->soundcnt_l & 7u;
                pl = (pl * (int)(lv + 1u)) >> 3;
                pr = (pr * (int)(rv + 1u)) >> 3;
            }
            /* SOUNDCNT_H PSG volume: 0=25%, 1=50%, 2=100% */
            switch (a->soundcnt_h & 3u) {
            case 0: pl >>= 2; pr >>= 2; break;
            case 1: pl >>= 1; pr >>= 1; break;
            default: break;
            }
            l += pl;
            r += pr;
        }
        l *= 1024;
        r *= 1024;

        /* FIFO A (soundcnt_h bits: 2=volume, 8=en R, 9=en L) */
        {
            int sa = a->fifo_has[0] ? (a->fifo_cur[0] << 8) : 0;
            if (!(a->soundcnt_h & 0x0004u))
                sa >>= 1; /* 50% volume */
            if (a->soundcnt_h & 0x0100u)
                r += sa;
            if (a->soundcnt_h & 0x0200u)
                l += sa;
        }
        /* FIFO B (bits: 3=volume, 12=en R, 13=en L) */
        {
            int sb = a->fifo_has[1] ? (a->fifo_cur[1] << 8) : 0;
            if (!(a->soundcnt_h & 0x0008u))
                sb >>= 1;
            if (a->soundcnt_h & 0x1000u)
                r += sb;
            if (a->soundcnt_h & 0x2000u)
                l += sb;
        }

        /* saturate */
        if (l > 32767)
            l = 32767;
        if (l < -32768)
            l = -32768;
        if (r > 32767)
            r = 32767;
        if (r < -32768)
            r = -32768;

        if (a->out != NULL && a->out_pos + 2 <= a->out_cap) {
            a->out[a->out_pos++] = (int16_t)l;
            a->out[a->out_pos++] = (int16_t)r;
        }
    }
}

/* ---- IO ---- */

uint16_t gba_apu_io_read16(gba_apu *a, uint32_t addr)
{
    uint32_t off = addr & 0xFFu;
    if (off == 0x80u)
        return a->soundcnt_l;
    if (off == 0x82u)
        return a->soundcnt_h;
    if (off == 0x84u) {
        /* SOUNDCNT_X: bit7 master enable; bits 0-3 channel-active flags */
        uint16_t v = (uint16_t)((a->soundcnt_x & 0x80u) << 8u);
        if (a->sq_active[0])
            v |= 1u << 0u;
        if (a->sq_active[1])
            v |= 1u << 1u;
        if (a->noise_active)
            v |= 1u << 3u;
        return v;
    }
    if (off == 0x88u)
        return a->soundbias;
    return 0; /* PSG length/frequency registers are write-only on hardware */
}

static uint8_t fifo_index_for_offset(uint32_t off)
{
    /* 0xA0-0xA7 -> FIFO A, 0xA8-0xAF -> FIFO B (mirrored layout) */
    return (uint8_t)(((off - 0xA0u) >> 2u) & 1u);
}

void gba_apu_io_write8(gba_apu *a, uint32_t addr, uint8_t v)
{
    uint32_t off = addr & 0xFFu;
    gba_t *g = (gba_t *)((char *)a - offsetof(gba_t, apu));
    if (off < 0xA0u || off > 0xAFu)
        return; /* PSG registers accept 16-bit writes on hardware */
    int f = fifo_index_for_offset(off);
    fifo_append(a, f, &v, 1);
    fifo_cpu_appended(g, f);
}

void gba_apu_io_write16(gba_apu *a, uint32_t addr, uint16_t v)
{
    uint32_t off = addr & 0xFFu;
    gba_t *g = (gba_t *)((char *)a - offsetof(gba_t, apu));
    switch (off) {
    case 0x80: /* SOUNDCNT_L: PSG routing + master volume */
        a->soundcnt_l = v;
        break;
    case 0x82: /* SOUNDCNT_H */
    {
        uint16_t old = a->soundcnt_h;
        a->soundcnt_h = v;
        /* FIFO reset bits: writing 1 clears the FIFO contents */
        if (v & 0x0800u) {
            a->fifo_count[0] = 0;
            a->fifo_cur[0] = 0;
            a->fifo_has[0] = 0;
        }
        if (v & 0x8000u) {
            a->fifo_count[1] = 0;
            a->fifo_cur[1] = 0;
            a->fifo_has[1] = 0;
        }
        /* request refill if FIFOs were just cleared and DMA is waiting */
        if (!(old & 0x0800u) && (v & 0x0800u))
            fifo_cpu_appended(g, 0);
        if (!(old & 0x8000u) && (v & 0x8000u))
            fifo_cpu_appended(g, 1);
        break;
    }
    case 0x84: /* SOUNDCNT_X: bit7 master enable */
        a->soundcnt_x = (uint8_t)(v & 0x80u);
        break;
    case 0x60: /* SOUND1CNT_L: length / duty (sweep bits 8-14 unimplemented) */
        a->sq_duty[0] = (uint8_t)((v >> 6) & 3u);
        a->sq_len[0] = (uint8_t)(v & 0x3Fu);
        break;
    case 0x62: /* SOUND1CNT_H (NR12): envelope step/dir + initial volume */
        a->sq_env_vol[0] = (uint8_t)((v >> 12) & 0xFu);
        break;
    case 0x64: /* SOUND1CNT_X: frequency / restart */
        a->sq_period[0] = (uint16_t)(2048u - (v & 0x7FFu));
        if (v & 0x8000u)
            sq_restart(a, 0);
        break;
    case 0x68: /* SOUND2CNT_L (NR21+NR22): length / duty / envelope */
        a->sq_duty[1] = (uint8_t)((v >> 6) & 3u);
        a->sq_env_vol[1] = (uint8_t)((v >> 12) & 0xFu);
        a->sq_len[1] = (uint8_t)(v & 0x3Fu);
        break;
    case 0x6C: /* SOUND2CNT_H: frequency / restart */
        a->sq_period[1] = (uint16_t)(2048u - (v & 0x7FFu));
        if (v & 0x8000u)
            sq_restart(a, 1);
        break;
    case 0x78: /* SOUND4CNT_L (NR41+NR42): length / envelope */
        a->noise_env_vol = (uint8_t)((v >> 12) & 0xFu);
        a->noise_len = (uint8_t)(v & 0x3Fu);
        break;
    case 0x7C: /* SOUND4CNT_H: LFSR config / restart */
    {
        uint32_t ratio = (v & 7u) + 1u;       /* R: bits 0-2 */
        uint32_t shift = (v >> 4) & 0xFu;     /* S: bits 4-7 (15 invalid) */
        if (shift > 14u)
            shift = 14u; /* deterministic mapping for the invalid value */
        a->noise_width7 = (uint8_t)((v >> 3) & 1u);
        /* LFSR shift clock = 524288 Hz / R / 2^S -> 32*R<<S CPU cycles */
        a->noise_period = 32u * ratio << shift;
        if (v & 0x8000u) {
            a->noise_len_en = (uint8_t)((v >> 14) & 1u);
            noise_restart(a);
        }
        break;
    }
    case 0x88:
        a->soundbias = v;
        break;
    default:
        if (off >= 0xA0u && off <= 0xAFu) {
            /* SOUNDFIFO A/B: 16-bit writes append two bytes (LE) */
            uint8_t b[2];
            int f = fifo_index_for_offset(off);
            b[0] = (uint8_t)(v & 0xFFu);
            b[1] = (uint8_t)(v >> 8);
            fifo_append(a, f, b, 2);
            fifo_cpu_appended(g, f);
        }
        break;
    }
}
