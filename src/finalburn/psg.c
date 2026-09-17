/*
 * finalburn: SN76489 Programmable Sound Generator (Sega PSG).
 *
 * Three tone channels + one noise channel. Tone frequency:
 *   f = 3579545 / (32 * (N + 1))   [N = 10-bit period register]
 * Noise LFSR: 15/16-bit shift register, four rate modes (2 highest tone
 * counters or fixed), white (x3 tap) or periodic.
 *
 * Volume: 4-bit attenuation, output = vol_table[15-att]. Envelope mode
 * (att bit 4) is not implemented - games on the Genesis write explicit
 * volumes (documented approximation).
 */
#include "fb_md.h"

static const uint8_t psg_vol[16] = {
    255, 181, 128, 91, 64, 45, 32, 23,
    16, 11, 8, 6, 4, 3, 2, 0
};

void fb_psg_init(struct fb_psg *p)
{
    fb_psg_reset(p);
}

void fb_psg_reset(struct fb_psg *p)
{
    for (int i = 0; i < 3; i++) {
        p->tone[i] = 0; /* register 0 -> tone divider runs at 1024 */
        p->tone_ctr[i] = 0;
        p->tone_out[i] = 0;
        p->vol[i] = 15; /* silent */
    }
    p->vol[3] = 15;
    p->noise_shift = 0x8000u;
    p->noise_mode = 0;
    p->noise_ctr = 1;
    p->noise_per = 16;
    p->noise_out = 0;
    p->latch_type = 0;
    p->latch_chan = 0;
}

void fb_psg_write(struct fb_psg *p, uint8_t v)
{
    if (v & 0x80u) {
        p->latch_chan = (uint8_t)((v >> 5) & 3u);
        p->latch_type = (uint8_t)((v >> 4) & 1u);
        if (p->latch_type == 0) {
            if (p->latch_chan < 3)
                p->tone[p->latch_chan] =
                    (uint16_t)((p->tone[p->latch_chan] & 0x3F0u) | (v & 0x0Fu));
            else {
                /* noise register write */
                p->noise_mode = (uint8_t)(v & 7u);
                static const uint16_t rates[4] = { 16, 32, 64, 16 };
                p->noise_per = rates[(v >> 1) & 3];
            }
        } else {
            p->vol[p->latch_chan] = (uint8_t)(v & 0x0Fu);
        }
    } else {
        if (p->latch_type == 0 && p->latch_chan < 3)
            p->tone[p->latch_chan] =
                (uint16_t)((p->tone[p->latch_chan] & 0x0Fu) |
                           ((uint16_t)(v & 0x3Fu) << 4));
    }
}

void fb_psg_tick(struct fb_psg *p)
{
    /* tone channels: counter is 10-bit, wraps at N+1 (N=0 -> 1024) */
    uint8_t tone2_toggled = 0;
    for (int i = 0; i < 3; i++) {
        uint16_t limit = p->tone[i];
        if (limit == 0)
            limit = 1023;
        else
            limit = (uint16_t)(limit - 1u);
        if (p->tone_ctr[i] >= limit) {
            p->tone_ctr[i] = 0;
            p->tone_out[i] ^= 1;
            if (i == 2)
                tone2_toggled = 1;
        } else {
            p->tone_ctr[i]++;
        }
    }
    /* noise: LFSR clocked by tone2 output (rate 0-2) or fixed period (rate 3) */
    {
        uint8_t rate_sel = (uint8_t)((p->noise_mode >> 1) & 3u);
        int do_shift = 0;
        if (rate_sel == 3) {
            if (p->noise_per <= 1)
                do_shift = 1;
            else if (--p->noise_ctr == 0) {
                p->noise_ctr = p->noise_per;
                do_shift = 1;
            }
        } else if (tone2_toggled) {
            do_shift = 1;
        }
        if (do_shift) {
            uint16_t s = p->noise_shift;
            uint16_t fb;
            if (p->noise_mode & 1u) /* white: bit0 ^ bit1 */
                fb = (uint16_t)(((s >> 0) ^ (s >> 1)) & 1u);
            else                    /* periodic: bit0 (1/16 duty pulse) */
                fb = (uint16_t)(s & 1u);
            s = (uint16_t)((s >> 1) | (fb << 15));
            p->noise_shift = s;
            p->noise_out = (uint8_t)(s & 1u);
        }
    }
}

int16_t fb_psg_sample(const struct fb_psg *p)
{
    int acc = 0;
    for (int i = 0; i < 3; i++) {
        if (p->tone_out[i] && p->vol[i] < 15)
            acc += psg_vol[p->vol[i]];
    }
    if (p->noise_out && p->vol[3] < 15)
        acc += psg_vol[p->vol[3]];
    /* 4x channels at 255 max -> scale to ~ 8191 (13-bit) range */
    return (int16_t)(acc * 8);
}
