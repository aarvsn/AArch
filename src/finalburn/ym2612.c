/*
 * finalburn: YM2612 FM synthesis (subset, integer-only).
 *
 * Implemented: 6 channels x 4 operators, all 8 algorithms, self-feedback,
 * detune/multiple/total-level, AR/DR/SR/RR envelope with key scaling,
 * sustain level, timers A/B with IRQ, key on/off, DAC channel, L/R panning.
 *
 * Tuning anchor: FNUM=1024, OCT=4, MULT=1 -> 440 Hz (phase increment 8659
 * units per internal sample at master/144; 8659 * 53267 / 2^20 = 439.98 Hz).
 *
 * Approximations (documented in README): LFO and SSG-EG are ignored, CH3
 * special mode runs at its normal frequency, the dB envelope curve uses an
 * integer 0.75 dB/unit chain, timer rates are coarse, the sine walk has a
 * small (~1.7%) amplitude drift per cycle.
 */
#include "fb_md.h"

#include <string.h>

#define YM_ATT_MAX 128 /* Q8 attenuation ceiling (silent) */

/* 1024-entry full-cycle sine table, 13-bit signed. */
static int16_t ym_sine[1024];
/* dB attenuation table in Q14 gain, one entry per 0.75 dB unit. */
static uint16_t ym_gain[256];
/* envelope rate steps in Q8 attenuation units per internal sample */
static uint16_t ym_step[64];

static int ym_tables_ready;

static void ym_build_tables(void)
{
    if (ym_tables_ready)
        return;
    /*
     * Integer sine via a symplectic circle walk: rotate (x,y) by
     * 2*pi/1024 with k = 2*sin(pi/1024) in Q16 (402).
     */
    int64_t cx = 8191, cy = 0;
    for (int i = 0; i < 1024; i++) {
        ym_sine[i] = (int16_t)cy;
        int64_t nx = cx - ((cy * 402) >> 16);
        int64_t ny = cy + ((nx * 402) >> 16);
        cx = nx;
        cy = ny;
    }
    /* gain: 16384 at 0 dB, x0.9174 per 0.75 dB unit (15030/16384) */
    uint32_t g = 16384;
    for (int i = 0; i < 256; i++) {
        ym_gain[i] = (uint16_t)g;
        g = (g * 15030u) >> 14;
        if (i > 170)
            ym_gain[i] = 0;
    }
    for (int r = 0; r < 64; r++)
        ym_step[r] = (uint16_t)(r < 2 ? 0 : r * 13);
    ym_tables_ready = 1;
}

static uint32_t ym_phase_inc(const struct fb_ym_op *o)
{
    /* signed detune delta in fnum units (documented approximation) */
    static const int8_t dt_tab[8] = { 0, 28, 56, 85, -85, -56, -28, 0 };
    int32_t fnum = (int32_t)o->fnum +
                   ((int32_t)dt_tab[o->dt & 7] * (int32_t)o->fnum >> 8);
    if (fnum < 1)
        fnum = 1;
    uint32_t mult2 = (o->mult == 0) ? 1u : (uint32_t)o->mult * 2u;
    uint64_t inc = ((uint64_t)(uint32_t)fnum << o->block) * mult2 * 8659u;
    return (uint32_t)(inc >> 15);
}

static uint8_t ym_eff_rate(const struct fb_ym_op *o, uint8_t rate)
{
    uint32_t r = rate + ((rate * o->ks) >> 2);
    if (r > 63)
        r = 63;
    return (uint8_t)r;
}

static void ym_keyop(struct fb_ym_op *o, int on)
{
    if (on) {
        o->state = 1; /* attack */
        o->phase = 0;
    } else {
        o->state = 0; /* release */
    }
}

void fb_ym_init(struct fb_ym *y)
{
    memset(y, 0, sizeof *y);
    ym_build_tables();
    fb_ym_reset(y);
}

void fb_ym_reset(struct fb_ym *y)
{
    memset(y, 0, sizeof *y);
    /* envelopes start silent; key-on attacks from full attenuation */
    for (int ch = 0; ch < 6; ch++)
        for (int op = 0; op < 4; op++)
            y->chan[ch].op[op].att_q8 = YM_ATT_MAX * 256;
    y->last_l = 0;
    y->last_r = 0;
}

void fb_ym_write_addr(struct fb_ym *y, uint8_t bank, uint8_t reg)
{
    y->addr_bank = bank & 1u;
    y->reg_latched[bank & 1u] = reg;
}

static struct fb_ym_op *ym_op_reg(struct fb_ym *y, uint8_t reg)
{
    /* operator registers 0x30-0x9F: low nibble = operator, bits 2-1 = ch */
    uint8_t ch = (uint8_t)(((reg >> 1) & 3u) + (y->addr_bank ? 3u : 0u));
    uint8_t op = (uint8_t)(reg & 3u);
    return &y->chan[ch].op[op];
}

static void ym_update_freq(struct fb_ym *y, uint8_t ch)
{
    struct fb_ym_chan *c = &y->chan[ch];
    for (int i = 0; i < 4; i++)
        c->op[i].phase_inc = ym_phase_inc(&c->op[i]);
}

void fb_ym_write_data(struct fb_ym *y, uint8_t bank, uint8_t data)
{
    uint8_t v = data;
    uint8_t reg = y->reg_latched[bank & 1u];

    switch (reg) {
    case 0x22: /* LFO: ignored (documented) */
        break;
    case 0x24:
        y->timer_a_load = (uint16_t)((y->timer_a_load & 0x300u) | v);
        break;
    case 0x25:
        y->timer_a_load = (uint16_t)((y->timer_a_load & 0x0FFu) |
                                     ((uint16_t)(v & 3u) << 8));
        break;
    case 0x26:
        y->timer_b_load = v;
        break;
    case 0x27:
        if (v & 0x01u)
            y->timer_a_over = 0;
        if (v & 0x02u)
            y->timer_b_over = 0;
        y->timer_a_en = (uint8_t)((v & 0x04u) ? 1 : 0);
        y->timer_b_en = (uint8_t)((v & 0x08u) ? 1 : 0);
        break;
    case 0x28: { /* key on/off */
        uint8_t chn = (uint8_t)(v & 7u);
        if (chn >= 3 && chn <= 5) {
            uint8_t real = (uint8_t)(chn - 3);
            uint8_t mask = (uint8_t)((v >> 4) & 0xFu);
            for (int i = 0; i < 4; i++)
                ym_keyop(&y->chan[real].op[i], (mask >> i) & 1u);
        } else if (chn <= 2) {
            uint8_t mask = (uint8_t)((v >> 4) & 0xFu);
            for (int i = 0; i < 4; i++)
                ym_keyop(&y->chan[chn].op[i], (mask >> i) & 1u);
        }
        break;
    }
    case 0x2A:
        y->dac_data = v;
        break;
    case 0x2B:
        y->dac_en = (uint8_t)((v & 0x80u) ? 1 : 0);
        break;
    default:
        if (reg >= 0x30 && reg <= 0x9F) {
            struct fb_ym_op *o = ym_op_reg(y, reg);
            switch (reg & 0xF0u) {
            case 0x30u:
                o->dt = (uint8_t)((v >> 4) & 7u);
                o->mult = (uint8_t)(v & 0x0Fu);
                break;
            case 0x40u:
                o->tl = (uint8_t)(v & 0x7Fu);
                break;
            case 0x50u:
                o->ks = (uint8_t)((v >> 6) & 3u);
                o->ar = (uint8_t)(v & 0x1Fu);
                break;
            case 0x60u:
                /* AM bit ignored (LFO off) */
                o->dr = (uint8_t)(v & 0x1Fu);
                break;
            case 0x70u:
                o->sr = (uint8_t)(v & 0x1Fu);
                break;
            case 0x80u:
                o->sl = (uint8_t)(v >> 4);
                o->rr = (uint8_t)(v & 0x0Fu);
                break;
            default:
                break; /* 0x90 SSG-EG ignored */
            }
            ym_update_freq(y, (uint8_t)(((reg >> 1) & 3u) +
                                        (y->addr_bank ? 3u : 0u)));
        } else if (reg >= 0xA0 && reg <= 0xA2) {
            uint8_t ch = (uint8_t)((reg - 0xA0u) + (y->addr_bank ? 3u : 0u));
            for (int i = 0; i < 4; i++) {
                struct fb_ym_op *o = &y->chan[ch].op[i];
                o->fnum = (uint16_t)((o->fnum & 0x0700u) | v);
            }
            ym_update_freq(y, ch);
        } else if (reg >= 0xA4 && reg <= 0xA6) {
            uint8_t ch = (uint8_t)((reg - 0xA4u) + (y->addr_bank ? 3u : 0u));
            uint8_t block = (uint8_t)(v & 7u);
            uint8_t fhi = (uint8_t)((v >> 4) & 7u);
            for (int i = 0; i < 4; i++) {
                struct fb_ym_op *o = &y->chan[ch].op[i];
                o->block = block;
                o->fnum = (uint16_t)((o->fnum & 0x00FFu) | ((uint16_t)fhi << 8));
            }
            ym_update_freq(y, ch);
        } else if (reg >= 0xB0 && reg <= 0xB2) {
            uint8_t ch = (uint8_t)((reg - 0xB0u) + (y->addr_bank ? 3u : 0u));
            y->chan[ch].algorithm = (uint8_t)(v & 7u);
            y->chan[ch].feedback = (uint8_t)((v >> 4) & 7u);
        } else if (reg >= 0xB4 && reg <= 0xB6) {
            uint8_t ch = (uint8_t)((reg - 0xB4u) + (y->addr_bank ? 3u : 0u));
            y->chan[ch].pan_r = (uint8_t)((v >> 7) & 1u);
            y->chan[ch].pan_l = (uint8_t)((v >> 6) & 1u);
            y->chan[ch].fms = (uint8_t)((v >> 4) & 7u);
            y->chan[ch].ams = (uint8_t)(v & 3u);
        }
        break;
    }
}

/*
 * Algorithm connectivity: tab[slot] = bitmask of modulator sources
 * (bit0 = slot 1, bit1 = slot 2, bit2 = slot 3); slot 1 always carries
 * the 0x40 self-feedback marker. Per the YM2612 datasheet chart.
 */
static const uint8_t ym_conn[8][4] = {
    { 0x40, 0x01, 0x02, 0x04 }, /* 0: 1->2->3->4          */
    { 0x40, 0x00, 0x03, 0x04 }, /* 1: (1+2)->3->4         */
    { 0x40, 0x01, 0x02, 0x05 }, /* 2: 1->2, 1+(2->3)->4  */
    { 0x40, 0x01, 0x00, 0x06 }, /* 3: (1->2)+3 -> 4       */
    { 0x40, 0x01, 0x00, 0x00 }, /* 4: (1->2), 3, 4        */
    { 0x40, 0x00, 0x02, 0x02 }, /* 5: 1, (2->3), (2->4)   */
    { 0x40, 0x00, 0x00, 0x04 }, /* 6: 1, 2, (3->4)        */
    { 0x40, 0x00, 0x00, 0x00 }, /* 7: 1+2+3+4             */
};
static const uint8_t ym_out_mask[8] = {
    0x08, 0x08, 0x08, 0x08, 0x0E, 0x0D, 0x0B, 0x0F
};

void fb_ym_tick(struct fb_ym *y)
{
    /* timers (approximate rates: A per sample, B per 3 samples) */
    if (y->timer_a_en) {
        y->timer_a_ctr++;
        if (y->timer_a_ctr >= (uint16_t)(1024 - y->timer_a_load)) {
            y->timer_a_ctr = 0;
            y->timer_a_over = 1;
        }
    }
    y->tb_div++;
    if (y->tb_div >= 3) {
        y->tb_div = 0;
        if (y->timer_b_en) {
            y->timer_b_ctr++;
            if (y->timer_b_ctr >= (uint16_t)(256 - y->timer_b_load)) {
                y->timer_b_ctr = 0;
                y->timer_b_over = 1;
            }
        }
    }
    y->irq = (uint8_t)(((y->timer_a_over && y->timer_a_en) ||
                        (y->timer_b_over && y->timer_b_en))
                           ? 1
                           : 0);

    for (int ch = 0; ch < 6; ch++) {
        struct fb_ym_chan *c = &y->chan[ch];
        int32_t opout[4] = { 0, 0, 0, 0 };
        const uint8_t *tab = ym_conn[c->algorithm & 7u];

        /* envelope update for all operators */
        for (int i = 0; i < 4; i++) {
            struct fb_ym_op *o = &c->op[i];
            switch (o->state) {
            case 1: { /* attack */
                uint32_t step = ym_step[ym_eff_rate(o, o->ar)];
                if (step == 0) {
                    o->state = 2;
                } else if (o->att_q8 <= (int32_t)step) {
                    o->att_q8 = 0;
                    o->state = 2;
                } else {
                    o->att_q8 -= (int32_t)step;
                }
                break;
            }
            case 2: { /* decay */
                uint32_t step = ym_step[ym_eff_rate(o, o->dr)];
                int32_t sl = (int32_t)o->sl * 4 * 256;
                if (step == 0)
                    break;
                o->att_q8 += (int32_t)step;
                if (o->att_q8 >= sl) {
                    o->att_q8 = sl;
                    o->state = 3;
                }
                break;
            }
            case 3: { /* sustain */
                uint32_t step = ym_step[ym_eff_rate(o, o->sr)];
                if (step == 0)
                    break;
                o->att_q8 += (int32_t)step;
                if (o->att_q8 > YM_ATT_MAX * 256)
                    o->att_q8 = YM_ATT_MAX * 256;
                break;
            }
            default: { /* release */
                uint32_t step = ym_step[ym_eff_rate(o, o->rr)];
                if (step == 0)
                    break;
                o->att_q8 += (int32_t)step;
                if (o->att_q8 > YM_ATT_MAX * 256)
                    o->att_q8 = YM_ATT_MAX * 256;
                break;
            }
            }
        }

        /* sequential operator evaluation honoring the algorithm chain */
        for (int i = 0; i < 4; i++) {
            struct fb_ym_op *o = &c->op[i];
            int32_t mod = 0;
            if (i == 0) {
                if (c->feedback)
                    mod = (c->fb_hist[0] + c->fb_hist[1]) >> (7 - c->feedback);
            } else {
                if (tab[i] & 1u)
                    mod += (opout[0] < 0) ? -opout[0] : opout[0];
                if (tab[i] & 2u)
                    mod += (opout[1] < 0) ? -opout[1] : opout[1];
                if (tab[i] & 4u)
                    mod += (opout[2] < 0) ? -opout[2] : opout[2];
            }
            o->phase = (o->phase + o->phase_inc) & 0xFFFFFu;
            uint32_t att = (uint32_t)(o->att_q8 >> 8);
            uint32_t gidx = att + o->tl;
            if (gidx > 255)
                gidx = 255;
            uint32_t idx = ((o->phase >> 7) + (uint32_t)mod) & 0x3FFu;
            opout[i] = (ym_sine[idx] * (int32_t)ym_gain[gidx]) >> 14;
        }
        c->fb_hist[1] = c->fb_hist[0];
        c->fb_hist[0] = (int16_t)opout[0];

        int32_t sum = 0;
        uint8_t om = ym_out_mask[c->algorithm & 7u];
        for (int i = 0; i < 4; i++)
            if (om & (1u << i))
                sum += opout[i];
        c->out = (int16_t)(sum >> 2);
    }

    fb_ym_mix(y);
}

void fb_ym_mix(struct fb_ym *y)
{
    int32_t l = 0, r = 0;
    for (int ch = 0; ch < 6; ch++) {
        struct fb_ym_chan *c = &y->chan[ch];
        int32_t o = c->out;
        if (c->pan_l)
            l += o;
        if (c->pan_r)
            r += o;
    }
    l = (l * 6) >> 4; /* 6-channel headroom */
    r = (r * 6) >> 4;
    if (y->dac_en) {
        int32_t d = ((int32_t)y->dac_data - 0x80) * 64;
        l += d;
        r += d;
    }
    if (l > 32767)
        l = 32767;
    if (l < -32768)
        l = -32768;
    if (r > 32767)
        r = 32767;
    if (r < -32768)
        r = -32768;
    y->last_l = (int16_t)l;
    y->last_r = (int16_t)r;
}
