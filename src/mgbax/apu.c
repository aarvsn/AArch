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
    a->sq_period[0] = 2048u; /* raw freq 0: 2048 - 0 */
    a->sq_period[1] = 2048u;
}

void gba_apu_step(gba_t *g, uint32_t cycles)
{
    gba_apu *a = &g->apu;
    for (int i = 0; i < 2; i++) {
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
    /* output sampling at 32768 Hz (every 512 CPU cycles) */
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
    case 0x60: /* SOUND1CNT_L: length / duty / envelope */
        a->sq_duty[0] = (uint8_t)((v >> 6) & 3u);
        a->sq_env_vol[0] = (uint8_t)((v >> 12) & 7u);
        a->sq_len[0] = (uint8_t)(v & 0x3Fu);
        break;
    case 0x64: /* SOUND1CNT_X: frequency / restart */
        a->sq_period[0] = (uint16_t)(2048u - (v & 0x7FFu));
        if (v & 0x8000u) {
            a->sq_active[0] = 1;
            a->sq_volume[0] = a->sq_env_vol[0];
            a->sq_freq_timer[0] = 0;
            a->sq_duty_pos[0] = 0;
        }
        break;
    case 0x68: /* SOUND2CNT_L: length / duty / envelope */
        a->sq_duty[1] = (uint8_t)((v >> 6) & 3u);
        a->sq_env_vol[1] = (uint8_t)((v >> 12) & 7u);
        a->sq_len[1] = (uint8_t)(v & 0x3Fu);
        break;
    case 0x6C: /* SOUND2CNT_H: frequency / restart */
        a->sq_period[1] = (uint16_t)(2048u - (v & 0x7FFu));
        if (v & 0x8000u) {
            a->sq_active[1] = 1;
            a->sq_volume[1] = a->sq_env_vol[1];
            a->sq_freq_timer[1] = 0;
            a->sq_duty_pos[1] = 0;
        }
        break;
    case 0x78: /* SOUND4CNT_L: length / envelope */
        a->noise_env_vol = (uint8_t)((v >> 12) & 7u);
        a->noise_len = (uint8_t)(v & 0x3Fu);
        break;
    case 0x7C: /* SOUND4CNT_H: frequency / restart */
        a->noise_period = (uint16_t)(v & 0x7FFu);
        if (v & 0x8000u)
            a->noise_active = 1;
        break;
    case 0x88:
        a->soundbias = v;
        break;
    default:
        break;
    }
}
