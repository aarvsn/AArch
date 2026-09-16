/*
 * beatle-nes-redux APU: 2 pulse, triangle, noise, DMC, frame sequencer.
 *
 * Documented simplifications (this milestone):
 *  - DMC CPU stalls for memory reads are not modeled (the reader fetches
 *    without stealing CPU cycles); the IRQ, loop, and output unit behave
 *    per spec
 *  - the mixer is a plain integer sum (no non-linear DAC modeling, no
 *    high-pass filter); output is fully deterministic
 *  - the frame sequencer uses rounded integer cycle targets
 */
#include "nes.h"

#include <string.h>

#define NES_OUT_RATE 48000u

static const uint8_t duty_table[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 0, 0, 1, 1, 1, 1, 1 }
};

static const uint16_t noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

/* DMC rate table, NTSC, in CPU cycles per output bit */
static const uint16_t dmc_rate_table[16] = {
    428, 380, 340, 320, 286, 214, 190, 160, 142, 128, 106, 84, 72, 54, 36, 18
};

static const uint8_t length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

void nes_apu_init(nes_apu *a, int16_t *out, size_t cap)
{
    memset(a, 0, sizeof *a);
    a->out = out;
    a->out_cap = cap;
    nes_apu_reset(a);
}

void nes_apu_reset(nes_apu *a)
{
    int16_t *out = a->out;
    size_t cap = a->out_cap;
    memset(a, 0, sizeof *a);
    a->out = out;
    a->out_cap = cap;
    a->sample_step = (uint32_t)(((uint64_t)NES_OUT_RATE << 16) / NES_CPU_HZ);
    a->noise.lfsr = 1;
}

/* ---- channel helpers ---- */

static void pulse_clock_timer(nes_apu *a, int i)
{
    if (a->pulse[i].timer == 0) {
        a->pulse[i].timer = a->pulse[i].period;
        a->pulse[i].duty_pos = (uint8_t)((a->pulse[i].duty_pos + 1u) & 7u);
    } else {
        a->pulse[i].timer--;
    }
}

static uint16_t pulse_target(nes_apu *a, int i)
{
    uint16_t delta = (uint16_t)(a->pulse[i].freq >> a->pulse[i].sweep_shift);
    if (a->pulse[i].sweep_negate) {
        delta = (uint16_t)(i == 0 ? delta - 1u : delta);
        return (uint16_t)(a->pulse[i].freq - delta);
    }
    return (uint16_t)(a->pulse[i].freq + delta);
}

static void pulse_sweep_clock(nes_apu *a, int i)
{
    if (a->pulse[i].sweep_reload) {
        a->pulse[i].sweep_timer = a->pulse[i].sweep_period;
        a->pulse[i].sweep_reload = 0;
        return;
    }
    if (a->pulse[i].sweep_timer == 0) {
        a->pulse[i].sweep_timer = a->pulse[i].sweep_period;
        if (a->pulse[i].sweep_enable && a->pulse[i].sweep_period != 0 &&
            a->pulse[i].freq >= 8u) {
            uint16_t t = pulse_target(a, i);
            if (t <= 0x7FFu && a->pulse[i].sweep_shift != 0u) {
                a->pulse[i].freq = t;
                a->pulse[i].period = (uint16_t)((t + 1u) * 2u);
            }
        }
    } else {
        a->pulse[i].sweep_timer--;
    }
}

static void envelope_clock(uint8_t *timer, uint8_t period,
                           uint8_t *volume, uint8_t direction)
{
    if (*timer == 0) {
        *timer = period;
        if (direction && *volume < 15u)
            (*volume)++;
        else if (!direction && *volume > 0u)
            (*volume)--;
    } else {
        (*timer)--;
    }
}

static void quarter_frame(nes_apu *a)
{
    for (int i = 0; i < 2; i++)
        envelope_clock(&a->pulse[i].env_timer, a->pulse[i].env_period,
                       &a->pulse[i].volume, a->pulse[i].env_direction);
    envelope_clock(&a->noise.env_timer, a->noise.env_period,
                   &a->noise.volume, a->noise.env_direction);

    /* triangle linear counter */
    if (a->tri.linear_reload_flag) {
        a->tri.linear_counter = a->tri.linear_reload;
    } else if (a->tri.linear_counter > 0u) {
        a->tri.linear_counter--;
    }
    if (!a->tri.linear_ctrl)
        a->tri.linear_reload_flag = 0;
}

static void half_frame(nes_apu *a)
{
    for (int i = 0; i < 2; i++) {
        if (a->pulse[i].len_enable && a->pulse[i].len_counter > 0u)
            a->pulse[i].len_counter--;
        pulse_sweep_clock(a, i);
    }
    if (a->tri.len_enable && a->tri.len_counter > 0u)
        a->tri.len_counter--;
    if (a->noise.len_enable && a->noise.len_counter > 0u)
        a->noise.len_counter--;
}

void nes_apu_run(nes_t *n, uint32_t cpu_cycles)
{
    nes_apu *a = &n->apu;
    for (uint32_t cyc = 0; cyc < cpu_cycles; cyc++) {
        /* timers */
        pulse_clock_timer(a, 0);
        pulse_clock_timer(a, 1);
        if (a->tri.timer == 0) {
            a->tri.timer = a->tri.period;
            if (a->tri.linear_counter > 0u && a->tri.len_counter > 0u)
                a->tri.seq_pos = (uint8_t)((a->tri.seq_pos + 1u) & 31u);
        } else {
            a->tri.timer--;
        }
        if (a->noise.timer == 0) {
            a->noise.timer = a->noise.period;
            uint16_t fb = (uint16_t)((a->noise.lfsr ^
                                      (a->noise.mode ? (a->noise.lfsr >> 6u)
                                                     : (a->noise.lfsr >> 1u))) & 1u);
            a->noise.lfsr = (uint16_t)((a->noise.lfsr >> 1) | (fb << 14));
        } else {
            a->noise.timer--;
        }
        /* DMC output unit: 8 CPU cycles per output bit */
        if (a->dmc.enabled && a->dmc.timer > 0u) {
            a->dmc.timer--;
            if (a->dmc.timer == 0) {
                a->dmc.timer = a->dmc.rate;
                if (!a->dmc.silence) {
                    uint8_t bit = (uint8_t)(a->dmc.shift & 1u);
                    if (a->dmc.output_level <= 125u)
                        a->dmc.output_level += bit;
                    else if (a->dmc.output_level >= 2u)
                        a->dmc.output_level -= (uint8_t)(bit ^ 1u);
                }
                a->dmc.shift = (uint8_t)(a->dmc.shift >> 1);
                a->dmc.bits_remaining--;
                if (a->dmc.bits_remaining == 0u) {
                    if (a->dmc.buffer_empty) {
                        a->dmc.silence = 1;
                    } else {
                        a->dmc.silence = 0;
                        a->dmc.shift = a->dmc.sample_buffer;
                        a->dmc.buffer_empty = 1;
                    }
                    if (a->dmc.bytes_remaining == 0u) {
                        if (a->dmc.loop) {
                            a->dmc.addr_counter = a->dmc.sample_addr;
                            a->dmc.bytes_remaining = a->dmc.sample_len;
                        } else if (a->dmc.irq_enable) {
                            a->dmc.irq_flag = 1;
                        }
                    }
                }
            }
        }
        /* DMC reader: fetch when buffer empty and bytes remain (no stall;
         * documented simplification) */
        if (a->dmc.enabled && a->dmc.buffer_empty && a->dmc.bytes_remaining > 0u) {
            a->dmc.sample_buffer = nes_bus_read(n, a->dmc.addr_counter);
            a->dmc.buffer_empty = 0;
            a->dmc.addr_counter++;
            if (a->dmc.addr_counter == 0u)
                a->dmc.addr_counter = 0x8000u;
            a->dmc.bytes_remaining--;
        }

        /* frame sequencer */
        a->frame_cycles++;
        uint32_t target;
        if (a->mode5) {
            static const uint32_t seq5[5] = { 7457u, 14913u, 22371u, 29829u, 37281u };
            target = seq5[a->frame_step];
        } else {
            static const uint32_t seq4[4] = { 7457u, 14913u, 22371u, 29829u };
            target = seq4[a->frame_step];
        }
        if (a->frame_cycles >= target) {
            /* targets are absolute within the frame; reset only on wrap */
            if (a->mode5) {
                /* 5-step: quarter+half at steps 0 and 2, quarter at 1 */
                if (a->frame_step < 3u)
                    quarter_frame(a);
                if (a->frame_step == 0u || a->frame_step == 2u)
                    half_frame(a);
                a->frame_step = (uint8_t)((a->frame_step + 1u) % 5u);
                if (a->frame_step == 0u)
                    a->frame_cycles = 0;
            } else {
                if (a->frame_step < 3u)
                    quarter_frame(a);
                if (a->frame_step == 1u || a->frame_step == 3u)
                    half_frame(a);
                a->frame_step++;
                if (a->frame_step > 3u) {
                    a->frame_step = 0;
                    a->frame_cycles = 0;
                    if (a->irq_inhibit == 0u)
                        a->frame_irq = 1;
                }
            }
        }

        /* output sampling */
        a->sample_acc += a->sample_step;
        if ((a->sample_acc >> 16) >= 1u) {
            a->sample_acc &= 0xFFFFu;
            int p0 = 0, p1 = 0, tri = 0, noi = 0, dmc = 0;
            if (a->pulse[0].enabled && a->pulse[0].dac &&
                a->pulse[0].len_counter > 0u && a->pulse[0].freq >= 8u &&
                (a->pulse[0].sweep_negate ||
                 pulse_target(a, 0) <= 0x7FFu))
                p0 = duty_table[a->pulse[0].duty & 3u][a->pulse[0].duty_pos]
                         ? (a->pulse[0].constant_volume ? a->pulse[0].env_volume
                                                        : a->pulse[0].volume)
                         : 0;
            if (a->pulse[1].enabled && a->pulse[1].dac &&
                a->pulse[1].len_counter > 0u && a->pulse[1].freq >= 8u &&
                (a->pulse[1].sweep_negate ||
                 pulse_target(a, 1) <= 0x7FFu))
                p1 = duty_table[a->pulse[1].duty & 3u][a->pulse[1].duty_pos]
                         ? (a->pulse[1].constant_volume ? a->pulse[1].env_volume
                                                        : a->pulse[1].volume)
                         : 0;
            if (a->tri.enabled && a->tri.len_counter > 0u &&
                a->tri.linear_counter > 0u)
                tri = a->tri.seq_pos < 16u ? 15u - a->tri.seq_pos
                                           : a->tri.seq_pos - 15u;
            if (a->noise.enabled && a->noise.dac && a->noise.len_counter > 0u)
                noi = ((a->noise.lfsr & 1u) == 0u)
                          ? (a->noise.constant_volume ? a->noise.env_volume
                                                      : a->noise.volume)
                          : 0;
            if (a->dmc.enabled && !a->dmc.silence)
                dmc = a->dmc.output_level;

            int sample = (p0 + p1) * 600 + tri * 480 + noi * 400 + dmc * 560;
            if (a->out != NULL && a->out_pos + 2 <= a->out_cap) {
                a->out[a->out_pos++] = (int16_t)sample;
                a->out[a->out_pos++] = (int16_t)sample;
            }
        }
    }
}

uint8_t nes_apu_read_status(nes_apu *a)
{
    uint8_t v = 0;
    if (a->pulse[0].len_counter > 0u) v |= 0x01u;
    if (a->pulse[1].len_counter > 0u) v |= 0x02u;
    if (a->tri.len_counter > 0u) v |= 0x04u;
    if (a->noise.len_counter > 0u) v |= 0x08u;
    if (a->dmc.bytes_remaining > 0u) v |= 0x10u;
    if (a->frame_irq) v |= 0x40u;
    if (a->dmc.irq_flag) v |= 0x80u;
    a->frame_irq = 0; /* reading clears frame IRQ */
    return v;
}

void nes_apu_write(nes_apu *a, uint16_t addr, uint8_t v)
{
    switch (addr) {
    case 0x4000:
        a->pulse[0].duty = (uint8_t)(v >> 6);
        a->pulse[0].len_enable = (uint8_t)(!(v & 0x20u));
        a->pulse[0].constant_volume = (uint8_t)(v & 0x10u);
        a->pulse[0].env_volume = (uint8_t)(v & 0x0Fu);
        a->pulse[0].volume = (uint8_t)(v & 0x0Fu);
        a->pulse[0].env_period = (uint8_t)(v & 0x0Fu);
        a->pulse[0].env_direction = 0;
        a->pulse[0].dac = (uint8_t)(((v >> 4) & 3u) != 0u);
        break;
    case 0x4001:
        a->pulse[0].sweep_enable = (uint8_t)(v >> 7);
        a->pulse[0].sweep_period = (uint8_t)((v >> 4) & 7u);
        a->pulse[0].sweep_negate = (uint8_t)(v & 8u);
        a->pulse[0].sweep_shift = (uint8_t)(v & 7u);
        a->pulse[0].sweep_reload = 1;
        break;
    case 0x4002:
        a->pulse[0].freq = (uint16_t)((a->pulse[0].freq & 0x0700u) | v);
        a->pulse[0].period = (uint16_t)((a->pulse[0].freq + 1u) * 2u);
        break;
    case 0x4003:
        a->pulse[0].freq = (uint16_t)((a->pulse[0].freq & 0x00FFu) |
                                      ((uint16_t)(v & 7u) << 8));
        a->pulse[0].period = (uint16_t)((a->pulse[0].freq + 1u) * 2u);
        if (a->pulse[0].enabled)
            a->pulse[0].len_counter = length_table[v >> 3];
        a->pulse[0].duty_pos = 0;
        a->pulse[0].volume = a->pulse[0].env_volume;
        a->pulse[0].env_timer = a->pulse[0].env_period;
        break;
    case 0x4004:
        a->pulse[1].duty = (uint8_t)(v >> 6);
        a->pulse[1].len_enable = (uint8_t)(!(v & 0x20u));
        a->pulse[1].constant_volume = (uint8_t)(v & 0x10u);
        a->pulse[1].env_volume = (uint8_t)(v & 0x0Fu);
        a->pulse[1].volume = (uint8_t)(v & 0x0Fu);
        a->pulse[1].env_period = (uint8_t)(v & 0x0Fu);
        a->pulse[1].env_direction = 0;
        a->pulse[1].dac = (uint8_t)(((v >> 4) & 3u) != 0u);
        break;
    case 0x4005:
        a->pulse[1].sweep_enable = (uint8_t)(v >> 7);
        a->pulse[1].sweep_period = (uint8_t)((v >> 4) & 7u);
        a->pulse[1].sweep_negate = (uint8_t)(v & 8u);
        a->pulse[1].sweep_shift = (uint8_t)(v & 7u);
        a->pulse[1].sweep_reload = 1;
        break;
    case 0x4006:
        a->pulse[1].freq = (uint16_t)((a->pulse[1].freq & 0x0700u) | v);
        a->pulse[1].period = (uint16_t)((a->pulse[1].freq + 1u) * 2u);
        break;
    case 0x4007:
        a->pulse[1].freq = (uint16_t)((a->pulse[1].freq & 0x00FFu) |
                                      ((uint16_t)(v & 7u) << 8));
        a->pulse[1].period = (uint16_t)((a->pulse[1].freq + 1u) * 2u);
        if (a->pulse[1].enabled)
            a->pulse[1].len_counter = length_table[v >> 3];
        a->pulse[1].duty_pos = 0;
        a->pulse[1].volume = a->pulse[1].env_volume;
        a->pulse[1].env_timer = a->pulse[1].env_period;
        break;
    case 0x4008:
        a->tri.linear_ctrl = (uint8_t)(v >> 7);
        a->tri.linear_reload = (uint8_t)(v & 0x7Fu);
        break;
    case 0x400A:
        a->tri.period = (uint16_t)((a->tri.period & 0x0700u) | v);
        break;
    case 0x400B:
        a->tri.period = (uint16_t)((a->tri.period & 0x00FFu) |
                                   ((uint16_t)(v & 7u) << 8));
        if (a->tri.enabled)
            a->tri.len_counter = length_table[v >> 3];
        a->tri.linear_reload_flag = 1;
        break;
    case 0x400C:
        a->noise.len_enable = (uint8_t)(!(v & 0x20u));
        a->noise.constant_volume = (uint8_t)(v & 0x10u);
        a->noise.env_volume = (uint8_t)(v & 0x0Fu);
        a->noise.volume = (uint8_t)(v & 0x0Fu);
        a->noise.env_period = (uint8_t)(v & 0x0Fu);
        a->noise.env_direction = 0;
        a->noise.dac = (uint8_t)(((v >> 4) & 3u) != 0u);
        break;
    case 0x400E:
        a->noise.mode = (uint8_t)(v >> 7);
        a->noise.period = noise_period[v & 0x0Fu];
        break;
    case 0x400F:
        if (a->noise.enabled)
            a->noise.len_counter = length_table[v >> 3];
        a->noise.volume = a->noise.env_volume;
        a->noise.env_timer = a->noise.env_period;
        break;
    case 0x4010:
        a->dmc.irq_enable = (uint8_t)(v & 0x80u);
        if (!(v & 0x80u))
            a->dmc.irq_flag = 0;
        a->dmc.loop = (uint8_t)(v & 0x40u);
        a->dmc.rate = dmc_rate_table[v & 0x0Fu];
        break;
    case 0x4011:
        a->dmc.output_level = (uint8_t)(v & 0x7Fu);
        break;
    case 0x4012:
        a->dmc.sample_addr = (uint16_t)(0xC000u + (uint16_t)v * 64u);
        break;
    case 0x4013:
        a->dmc.sample_len = (uint16_t)((uint16_t)v * 16u + 1u);
        break;
    case 0x4015:
        if (!(v & 0x10u))
            a->dmc.bytes_remaining = 0;
        if (!(v & 0x80u))
            a->dmc.irq_flag = 0;
        a->pulse[0].enabled = (uint8_t)(v & 1u);
        a->pulse[1].enabled = (uint8_t)(v & 2u);
        a->tri.enabled = (uint8_t)(v & 4u);
        a->noise.enabled = (uint8_t)(v & 8u);
        a->dmc.enabled = (uint8_t)(v & 0x10u);
        if (a->dmc.enabled && a->dmc.bytes_remaining == 0u) {
            a->dmc.addr_counter = a->dmc.sample_addr;
            a->dmc.bytes_remaining = a->dmc.sample_len;
        }
        break;
    case 0x4017:
        a->mode5 = (uint8_t)(v >> 7);
        a->irq_inhibit = (uint8_t)(v & 0x40u);
        if (a->irq_inhibit)
            a->frame_irq = 0;
        a->frame_cycles = 0;
        a->frame_step = 0;
        if (a->mode5)
            half_frame(a); /* 5-step: immediate quarter+half clocks */
        break;
    default:
        break;
    }
}
