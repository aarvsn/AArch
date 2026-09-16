/*
 * mgbx APU: 4-channel PSG (2 square, 1 wave, 1 noise) with frame sequencer,
 * length counters, envelopes, sweep (CH1), and NR52 power control.
 *
 * Output: stereo int16 at 32768 Hz (one sample every 128 T-cycles, exact
 * integer divisor of the 4194304 Hz system clock). Mixer is a plain integer
 * sum scaled by NR50/NR51; no high-pass filtering (documented).
 *
 * Deviations from hardware (documented): wave RAM is always CPU-accessible,
 * power-on delay after NR52 enable is not modeled, and wave channel reads
 * while active return the current sample byte rather than 0xFF.
 */
#include "mgbx.h"

#include <string.h>

#define APU_SAMPLE_RATE_DIV 128u /* T-cycles per output sample */

static const uint8_t duty_table[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 }
};

static const uint16_t noise_divisor[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

void gb_apu_init(gb_apu *apu, int16_t *out, size_t cap)
{
    memset(apu, 0, sizeof *apu);
    apu->out = out;
    apu->out_cap = cap;
    gb_apu_reset(apu);
}

void gb_apu_reset(gb_apu *apu)
{
    int16_t *out = apu->out;
    size_t cap = apu->out_cap;
    memset(apu, 0, sizeof *apu);
    apu->out = out;
    apu->out_cap = cap;
    apu->nr50 = 0x77;
    apu->nr51 = 0xF3;
    apu->enabled = 1;
    apu->noise.lfsr = 0x7FFFu;
}

static void chan_len_tick(gb_apu *apu)
{
    for (int i = 0; i < 2; i++) {
        if (apu->sq[i].len_enable && apu->sq[i].len_counter > 0) {
            apu->sq[i].len_counter--;
            if (apu->sq[i].len_counter == 0)
                apu->sq[i].active = 0;
        }
    }
    if (apu->wave.len_enable && apu->wave.len_counter > 0) {
        apu->wave.len_counter--;
        if (apu->wave.len_counter == 0)
            apu->wave.active = 0;
    }
    if (apu->noise.len_enable && apu->noise.len_counter > 0) {
        apu->noise.len_counter--;
        if (apu->noise.len_counter == 0)
            apu->noise.active = 0;
    }
}

static void envelope_tick(gb_apu *apu)
{
    for (int i = 0; i < 2; i++) {
        if (!apu->sq[i].active)
            continue;
        if (apu->sq[i].env_period == 0)
            continue;
        if (apu->sq[i].env_timer > 0)
            apu->sq[i].env_timer--;
        if (apu->sq[i].env_timer == 0) {
            apu->sq[i].env_timer = apu->sq[i].env_period;
            if (apu->sq[i].env_direction && apu->sq[i].volume < 15)
                apu->sq[i].volume++;
            else if (!apu->sq[i].env_direction && apu->sq[i].volume > 0)
                apu->sq[i].volume--;
        }
    }
    if (!apu->noise.active || apu->noise.env_period == 0)
        return;
    if (apu->noise.env_timer > 0)
        apu->noise.env_timer--;
    if (apu->noise.env_timer == 0) {
        apu->noise.env_timer = apu->noise.env_period;
        if (apu->noise.env_direction && apu->noise.volume < 15)
            apu->noise.volume++;
        else if (!apu->noise.env_direction && apu->noise.volume > 0)
            apu->noise.volume--;
    }
}

static uint16_t sweep_calc(gb_apu *apu)
{
    uint16_t freq = apu->sq[0].freq;
    uint16_t delta = (uint16_t)(freq >> apu->sq[0].sweep_shift);
    if (apu->sq[0].sweep_negate)
        return (uint16_t)(freq - delta);
    return (uint16_t)(freq + delta);
}

static void sweep_tick(gb_apu *apu)
{
    if (apu->sq[0].sweep_period == 0)
        return;
    if (apu->sq[0].sweep_timer > 0)
        apu->sq[0].sweep_timer--;
    if (apu->sq[0].sweep_timer != 0)
        return;
    apu->sq[0].sweep_timer = apu->sq[0].sweep_period;
    if (apu->sq[0].sweep_enabled && apu->sq[0].sweep_period != 0) {
        uint16_t nf = sweep_calc(apu);
        if (nf > 2047u) {
            apu->sq[0].active = 0;
            return;
        }
        if (apu->sq[0].sweep_shift != 0) {
            apu->sq[0].freq = nf;
            /* second overflow check with the new frequency */
            nf = sweep_calc(apu);
            if (nf > 2047u)
                apu->sq[0].active = 0;
        }
    }
}

static void chan_trigger_square(gb_apu *apu, int i)
{
    apu->sq[i].active = apu->sq[i].dac_enable;
    apu->sq[i].duty_pos = 0;
    apu->sq[i].freq_timer = (2048u - apu->sq[i].freq) * 4u;
    if (apu->sq[i].len_counter == 0)
        apu->sq[i].len_counter = 64;
    apu->sq[i].volume = apu->sq[i].env_volume;
    apu->sq[i].env_timer = apu->sq[i].env_period ? apu->sq[i].env_period : 8u;
    if (i == 0) {
        apu->sq[0].sweep_timer = apu->sq[0].sweep_period ? apu->sq[0].sweep_period : 8u;
        apu->sq[0].sweep_enabled =
            (uint8_t)(apu->sq[0].sweep_period != 0 || apu->sq[0].sweep_shift != 0);
        if (apu->sq[0].sweep_shift != 0) {
            uint16_t nf = sweep_calc(apu);
            if (nf > 2047u)
                apu->sq[0].active = 0;
        }
    }
}

static void chan_trigger_wave(gb_apu *apu)
{
    apu->wave.active = apu->wave.dac_enable;
    apu->wave.freq_timer = (2048u - apu->wave.freq) * 2u;
    apu->wave.pos = 0;
    if (apu->wave.len_counter == 0)
        apu->wave.len_counter = 256;
}

static void chan_trigger_noise(gb_apu *apu)
{
    apu->noise.active = apu->noise.dac_enable;
    apu->noise.lfsr = 0x7FFFu;
    apu->noise.freq_timer = (uint32_t)noise_divisor[apu->noise.divisor_code & 7u]
                            << apu->noise.shift_clock;
    if (apu->noise.len_counter == 0)
        apu->noise.len_counter = 64;
    apu->noise.volume = apu->noise.env_volume;
    apu->noise.env_timer = apu->noise.env_period ? apu->noise.env_period : 8u;
}

void gb_apu_step(struct mgbx *gb, uint32_t t_cycles)
{
    gb_apu *apu = &gb->apu;

    /* frame sequencer: 512 Hz, 8 steps */
    apu->frame_seq_timer = (uint16_t)(apu->frame_seq_timer + t_cycles);
    if (apu->frame_seq_timer >= 8192u) {
        apu->frame_seq_timer = (uint16_t)(apu->frame_seq_timer - 8192u);
        uint8_t step = apu->frame_seq_step;
        apu->frame_seq_step = (uint8_t)((step + 1u) & 7u);
        if (step == 0 || step == 4)
            chan_len_tick(apu);
        else if (step == 2 || step == 6) {
            chan_len_tick(apu);
            sweep_tick(apu);
        } else if (step == 7)
            envelope_tick(apu);
    }

    /* channel timers (T-cycle based, count up to period) */
    for (int i = 0; i < 2; i++) {
        if (!apu->sq[i].active)
            continue;
        uint32_t period = (2048u - apu->sq[i].freq) * 4u;
        apu->sq[i].freq_timer += t_cycles;
        while (period != 0u && apu->sq[i].freq_timer >= period) {
            apu->sq[i].freq_timer -= period;
            apu->sq[i].duty_pos = (uint8_t)((apu->sq[i].duty_pos + 1u) & 7u);
        }
    }
    if (apu->wave.active) {
        uint32_t period = (2048u - apu->wave.freq) * 2u;
        apu->wave.freq_timer += t_cycles;
        while (period != 0u && apu->wave.freq_timer >= period) {
            apu->wave.freq_timer -= period;
            apu->wave.pos = (uint8_t)((apu->wave.pos + 1u) & 31u);
            uint8_t b = apu->wave_ram[apu->wave.pos >> 1];
            apu->wave.sample = (apu->wave.pos & 1u) ? (uint8_t)(b & 0x0Fu)
                                                    : (uint8_t)(b >> 4);
        }
    }
    if (apu->noise.active) {
        uint32_t period = (uint32_t)noise_divisor[apu->noise.divisor_code & 7u]
                          << apu->noise.shift_clock;
        apu->noise.freq_timer += t_cycles;
        while (period != 0u && apu->noise.freq_timer >= period) {
            apu->noise.freq_timer -= period;
            uint16_t lfsr = apu->noise.lfsr;
            uint16_t feedback = (uint16_t)(((lfsr ^ (lfsr >> 1)) & 1u) << 14);
            lfsr = (uint16_t)((lfsr >> 1) | feedback);
            if (apu->noise.width_mode)
                lfsr = (uint16_t)((lfsr & (uint16_t)~0x40u) | (feedback >> 8));
            apu->noise.lfsr = lfsr;
        }
    }

    /* output sampling */
    apu->sample_acc += t_cycles;
    while (apu->sample_acc >= APU_SAMPLE_RATE_DIV) {
        apu->sample_acc -= APU_SAMPLE_RATE_DIV;
        int l = 0, r = 0;
        if (apu->enabled) {
            int outv[4];
            outv[0] = (apu->sq[0].active &&
                       duty_table[apu->sq[0].duty & 3u][apu->sq[0].duty_pos])
                          ? apu->sq[0].volume : 0;
            outv[1] = (apu->sq[1].active &&
                       duty_table[apu->sq[1].duty & 3u][apu->sq[1].duty_pos])
                          ? apu->sq[1].volume : 0;
            int wscale = apu->wave.volume_shift;
            outv[2] = apu->wave.active
                          ? (wscale == 0 ? 0
                                         : (wscale == 1 ? apu->wave.sample
                                                        : (wscale == 2 ? apu->wave.sample >> 1
                                                                       : apu->wave.sample >> 2)))
                          : 0;
            outv[3] = (apu->noise.active && !(apu->noise.lfsr & 1u))
                          ? apu->noise.volume : 0;
            for (int i = 0; i < 4; i++) {
                if (apu->nr51 & (1u << i))
                    r += outv[i];
                if (apu->nr51 & (1u << (i + 4)))
                    l += outv[i];
            }
        }
        int lv = (apu->nr50 >> 4) & 7u;
        int rv = apu->nr50 & 7u;
        int sample_l = l * (lv + 1) * 6826 / 100;
        int sample_r = r * (rv + 1) * 6826 / 100;
        if (apu->out != NULL && apu->out_pos + 2 <= apu->out_cap) {
            apu->out[apu->out_pos++] = (int16_t)sample_l;
            apu->out[apu->out_pos++] = (int16_t)sample_r;
        }
    }
}

uint8_t gb_apu_read(gb_apu *apu, uint16_t addr)
{
    switch (addr) {
    case 0xFF10: return (uint8_t)(apu->sq[0].sweep_period | (apu->sq[0].sweep_negate ? 8u : 0u) | 0x80u);
    case 0xFF11: return (uint8_t)((apu->sq[0].duty << 6) | 0x3Fu);
    case 0xFF12: return (uint8_t)((apu->sq[0].env_volume << 4) |
                                  (apu->sq[0].env_direction ? 8u : 0u) |
                                  apu->sq[0].env_period);
    case 0xFF13: return 0xFFu;
    case 0xFF14: return (uint8_t)(apu->sq[0].len_enable ? 0x40u : 0u) | 0xBFu;
    case 0xFF16: return (uint8_t)((apu->sq[1].duty << 6) | 0x3Fu);
    case 0xFF17: return (uint8_t)((apu->sq[1].env_volume << 4) |
                                  (apu->sq[1].env_direction ? 8u : 0u) |
                                  apu->sq[1].env_period);
    case 0xFF18: return 0xFFu;
    case 0xFF19: return (uint8_t)(apu->sq[1].len_enable ? 0x40u : 0u) | 0xBFu;
    case 0xFF1A: return (uint8_t)(apu->wave.dac_enable ? 0x80u : 0u) | 0x7Fu;
    case 0xFF1B: return 0xFFu;
    case 0xFF1C: return (uint8_t)((apu->wave.volume_shift << 5) | 0x9Fu);
    case 0xFF1D: return 0xFFu;
    case 0xFF1E: return (uint8_t)(apu->wave.len_enable ? 0x40u : 0u) | 0xBFu;
    case 0xFF20: return 0xFFu;
    case 0xFF21: return (uint8_t)((apu->noise.env_volume << 4) |
                                  (apu->noise.env_direction ? 8u : 0u) |
                                  apu->noise.env_period);
    case 0xFF22: return (uint8_t)((apu->noise.shift_clock << 4) |
                                  (apu->noise.width_mode ? 8u : 0u) |
                                  apu->noise.divisor_code);
    case 0xFF23: return (uint8_t)(apu->noise.len_enable ? 0x40u : 0u) | 0xBFu;
    case 0xFF24: return apu->nr50;
    case 0xFF25: return apu->nr51;
    case 0xFF26: {
        uint8_t v = (uint8_t)(0x70u | (apu->enabled ? 0x80u : 0u));
        if (apu->sq[0].active) v |= 0x01u;
        if (apu->sq[1].active) v |= 0x02u;
        if (apu->wave.active) v |= 0x04u;
        if (apu->noise.active) v |= 0x08u;
        return v;
    }
    default:
        if (addr >= 0xFF30u && addr <= 0xFF3Fu)
            return apu->wave_ram[addr - 0xFF30u];
        return 0xFFu;
    }
}

void gb_apu_write(gb_apu *apu, uint16_t addr, uint8_t v)
{
    /* global power gate: all registers except NR52 and wave RAM are
     * read-only while off */
    if (apu->enabled == 0 && addr != 0xFF26u &&
        !(addr >= 0xFF30u && addr <= 0xFF3Fu))
        return;

    switch (addr) {
    case 0xFF10:
        apu->sq[0].sweep_shift = (uint8_t)(v & 7u);
        apu->sq[0].sweep_negate = (uint8_t)((v & 8u) != 0);
        apu->sq[0].sweep_period = (uint8_t)((v >> 4) & 7u);
        break;
    case 0xFF11:
        apu->sq[0].duty = (uint8_t)((v >> 6) & 3u);
        apu->sq[0].len_counter = (uint8_t)(64u - (v & 0x3Fu));
        break;
    case 0xFF12:
        apu->sq[0].env_volume = (uint8_t)((v >> 4) & 0x0Fu);
        apu->sq[0].env_direction = (uint8_t)((v & 8u) != 0);
        apu->sq[0].env_period = (uint8_t)(v & 7u);
        apu->sq[0].dac_enable = (uint8_t)((v & 0xF8u) != 0);
        if (!apu->sq[0].dac_enable)
            apu->sq[0].active = 0;
        break;
    case 0xFF13: apu->sq[0].freq = (uint16_t)((apu->sq[0].freq & 0x0700u) | v); break;
    case 0xFF14:
        apu->sq[0].freq = (uint16_t)(apu->sq[0].freq | ((uint16_t)(v & 7u) << 8));
        apu->sq[0].len_enable = (uint8_t)((v & 0x40u) != 0);
        if (v & 0x80u)
            chan_trigger_square(apu, 0);
        break;
    case 0xFF16:
        apu->sq[1].duty = (uint8_t)((v >> 6) & 3u);
        apu->sq[1].len_counter = (uint8_t)(64u - (v & 0x3Fu));
        break;
    case 0xFF17:
        apu->sq[1].env_volume = (uint8_t)((v >> 4) & 0x0Fu);
        apu->sq[1].env_direction = (uint8_t)((v & 8u) != 0);
        apu->sq[1].env_period = (uint8_t)(v & 7u);
        apu->sq[1].dac_enable = (uint8_t)((v & 0xF8u) != 0);
        if (!apu->sq[1].dac_enable)
            apu->sq[1].active = 0;
        break;
    case 0xFF18: apu->sq[1].freq = (uint16_t)((apu->sq[1].freq & 0x0700u) | v); break;
    case 0xFF19:
        apu->sq[1].freq = (uint16_t)(apu->sq[1].freq | ((uint16_t)(v & 7u) << 8));
        apu->sq[1].len_enable = (uint8_t)((v & 0x40u) != 0);
        if (v & 0x80u)
            chan_trigger_square(apu, 1);
        break;
    case 0xFF1A:
        apu->wave.dac_enable = (uint8_t)((v & 0x80u) != 0);
        if (!apu->wave.dac_enable)
            apu->wave.active = 0;
        break;
    case 0xFF1B: apu->wave.len_counter = (uint16_t)(256u - v); break;
    case 0xFF1C: apu->wave.volume_shift = (uint8_t)((v >> 5) & 3u); break;
    case 0xFF1D: apu->wave.freq = (uint16_t)((apu->wave.freq & 0x0700u) | v); break;
    case 0xFF1E:
        apu->wave.freq = (uint16_t)(apu->wave.freq | ((uint16_t)(v & 7u) << 8));
        apu->wave.len_enable = (uint8_t)((v & 0x40u) != 0);
        if (v & 0x80u)
            chan_trigger_wave(apu);
        break;
    case 0xFF20: apu->noise.len_counter = (uint8_t)(64u - (v & 0x3Fu)); break;
    case 0xFF21:
        apu->noise.env_volume = (uint8_t)((v >> 4) & 0x0Fu);
        apu->noise.env_direction = (uint8_t)((v & 8u) != 0);
        apu->noise.env_period = (uint8_t)(v & 7u);
        apu->noise.dac_enable = (uint8_t)((v & 0xF8u) != 0);
        if (!apu->noise.dac_enable)
            apu->noise.active = 0;
        break;
    case 0xFF22:
        apu->noise.shift_clock = (uint8_t)((v >> 4) & 0x0Fu);
        apu->noise.width_mode = (uint8_t)((v & 8u) != 0);
        apu->noise.divisor_code = (uint8_t)(v & 7u);
        break;
    case 0xFF23:
        apu->noise.len_enable = (uint8_t)((v & 0x40u) != 0);
        if (v & 0x80u)
            chan_trigger_noise(apu);
        break;
    case 0xFF24: apu->nr50 = v; break;
    case 0xFF25: apu->nr51 = v; break;
    case 0xFF26:
        if (v & 0x80u) {
            if (!apu->enabled) {
                apu->enabled = 1;
                apu->frame_seq_timer = 0;
                apu->frame_seq_step = 0;
            }
        } else {
            apu->enabled = 0;
            memset(&apu->sq, 0, sizeof apu->sq);
            memset(&apu->wave, 0, sizeof apu->wave);
            memset(&apu->noise, 0, sizeof apu->noise);
        }
        break;
    default:
        if (addr >= 0xFF30u && addr <= 0xFF3Fu)
            apu->wave_ram[addr - 0xFF30u] = v;
        break;
    }
}
