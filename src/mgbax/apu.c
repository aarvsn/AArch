/* mgbax APU: PSG channels (2 squares + noise). FIFO DMA audio channels are
 * not implemented (documented limitation); output is deterministic and
 * silent until registers are configured. */
#include "gba.h"

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
}

void gba_apu_step(gba_t *g, uint32_t cycles)
{
    gba_apu *a = &g->apu;
    (void)cycles;
    for (int i = 0; i < 2; i++) {
        if (!a->sq_active[i])
            continue;
        a->sq_freq_timer[i] = (uint16_t)(a->sq_freq_timer[i] + 1u);
        if (a->sq_freq_timer[i] >= a->sq_period[i]) {
            a->sq_freq_timer[i] = 0;
            a->sq_duty_pos[i] = (uint8_t)((a->sq_duty_pos[i] + 1u) & 7u);
        }
    }
    /* output sampling at 32768 Hz */
    a->sample_acc += cycles;
    while (a->sample_acc >= 16777216u / GBA_OUT_RATE) {
        a->sample_acc -= 16777216u / GBA_OUT_RATE;
        int l = 0;
        for (int i = 0; i < 2; i++)
            if (a->sq_active[i] &&
                duty_table[a->sq_duty[i] & 3u][a->sq_duty_pos[i]])
                l += a->sq_volume[i];
        int sample = l * 1024;
        if (a->out != NULL && a->out_pos + 2 <= a->out_cap) {
            a->out[a->out_pos++] = (int16_t)sample;
            a->out[a->out_pos++] = (int16_t)sample;
        }
    }
}

uint16_t gba_apu_io_read16(gba_apu *a, uint32_t addr)
{
    uint32_t off = addr & 0xFFu;
    if (off == 0x88u)
        return a->soundbias;
    return 0; /* PSG registers are write-only on hardware */
}

void gba_apu_io_write16(gba_apu *a, uint32_t addr, uint16_t v)
{
    uint32_t off = addr & 0xFFu;
    switch (off) {
    case 0x60: /* SOUNDCNT4L: channel 1 duty/length */
        a->sq_duty[0] = (uint8_t)((v >> 6) & 3u);
        break;
    case 0x68: a->sq_duty[1] = (uint8_t)((v >> 6) & 3u); break;
    case 0x72: /* square 1 freq control */
        a->sq_period[0] = (uint16_t)(2048u - (v & 0x7FFu));
        if (v & 0x8000u)
            a->sq_active[0] = 1;
        break;
    case 0x7C: /* square 2 freq control */
        a->sq_period[1] = (uint16_t)(2048u - (v & 0x7FFu));
        if (v & 0x8000u)
            a->sq_active[1] = 1;
        break;
    case 0x78: /* noise control */
        a->noise_active = (uint8_t)(v & 0x8000u ? 1 : 0);
        break;
    case 0x88:
        a->soundbias = v;
        break;
    default:
        break;
    }
}
