/*
 * supercastpro: AICA subset (see dc.h for the documented model).
 * ARM7TDMI running from wave RAM, 64 raw channel register blocks with
 * live decode, KYONEX/MCIPD/MCIRE doorbells and a saturated PCM mixer.
 */
#include "dc.h"

#include <math.h>
#include <string.h>

#include "../common/util.h"

/* ---- register decode helpers ------------------------------------------------------------ */

static uint32_t aica_sa(const struct dc_aica *a, uint32_t ch)
{
    return (((uint32_t)(a->ch[ch][0] & 0x07FFu) << 16) | a->ch[ch][1]) &
           0x1FFFFFu;
}

static int aica_octave(const struct dc_aica *a, uint32_t ch)
{
    uint16_t r = a->ch[ch][9]; /* +0x24 */
    return ((int16_t)(r & 0xF800u)) >> 11; /* signed 5-bit */
}

static int aica_fsc(const struct dc_aica *a, uint32_t ch)
{
    return a->ch[ch][9] & 0x07FFu;
}

/* ---- SH-4 / ARM7 shared register access --------------------------------------------------- */

static uint32_t aica_reg_read(struct dc *d, uint32_t off)
{
    struct dc_aica *a = &d->aica;
    if (off < 0x2800u) {
        uint32_t ch = off >> 7;
        if (ch >= DC_AICA_CH)
            return 0;
        uint32_t r = (off >> 2) & 0x1Fu;
        if (r == 4u)
            return (uint32_t)(a->pos[ch] >> 16); /* CA readback */
        return a->ch[ch][r];
    }
    /* common window */
    if (off == DC_AICA_MCIRE)
        return a->mci_flag ? DC_AICA_MCIRE_BIT : 0u;
    return 0; /* KYONEX/MCIPD are write-only in this model */
}

static void aica_reg_write(struct dc *d, uint32_t off, uint32_t v,
                           int from_arm7)
{
    struct dc_aica *a = &d->aica;
    if (off < 0x2800u) {
        uint32_t ch = off >> 7;
        if (ch >= DC_AICA_CH)
            return;
        uint32_t r = (off >> 2) & 0x1Fu;
        a->ch[ch][r] = (uint16_t)v;
        return;
    }
    if (off == DC_AICA_KYONEX) {
        if ((v & DC_AICA_KYONEX_BIT) == 0u)
            return;
        for (uint32_t i = 0; i < DC_AICA_CH; i++) {
            if (a->ch[i][12] & 1u) { /* KYONB set: key on, reset phase */
                a->active[i] = 1;
                a->pos[i] = (uint64_t)aica_sa(a, i) << 16;
            } else {
                a->active[i] = 0;
            }
        }
        return;
    }
    if (off == DC_AICA_MCIPD) {
        if (v & DC_AICA_MCIPD_BIT)
            arm_irq(&a->cpu); /* SH-4 -> ARM7 doorbell pulse */
        return;
    }
    if (off == DC_AICA_MCIRE) {
        /* ARM7 write bit 9 sets the flag; SH-4 write bit 9 clears it. */
        if (v & DC_AICA_MCIRE_BIT)
            a->mci_flag = from_arm7 ? 1u : 0u;
        return;
    }
}

uint32_t dc_aica_reg_read(struct dc *d, uint32_t off)
{
    return aica_reg_read(d, off);
}

void dc_aica_reg_write(struct dc *d, uint32_t off, uint32_t v)
{
    aica_reg_write(d, off, v, 0);
}

/* ---- ARM7 bus (wave RAM 0x00000000, local registers 0x00800000) ---- */

static uint32_t a7_off(uint32_t addr)
{
    return addr - DC_AICA_ARM_WINDOW;
}

static uint8_t a7_rb(void *u, uint32_t addr)
{
    struct dc *d = u;
    if (addr < DC_AICA_SIZE)
        return d->aica_ram[addr];
    if (addr >= DC_AICA_ARM_WINDOW && a7_off(addr) < DC_AICA_REG_SIZE)
        return (uint8_t)aica_reg_read(d, a7_off(addr));
    return 0;
}

static uint16_t a7_rh(void *u, uint32_t addr)
{
    struct dc *d = u;
    if (addr < DC_AICA_SIZE - 1u)
        return (uint16_t)(d->aica_ram[addr] | (d->aica_ram[addr + 1u] << 8));
    if (addr >= DC_AICA_ARM_WINDOW && a7_off(addr) < DC_AICA_REG_SIZE)
        return (uint16_t)aica_reg_read(d, a7_off(addr));
    return 0;
}

static uint32_t a7_rw(void *u, uint32_t addr)
{
    struct dc *d = u;
    if (addr < DC_AICA_SIZE - 3u)
        return (uint32_t)d->aica_ram[addr] |
               ((uint32_t)d->aica_ram[addr + 1u] << 8) |
               ((uint32_t)d->aica_ram[addr + 2u] << 16) |
               ((uint32_t)d->aica_ram[addr + 3u] << 24);
    if (addr >= DC_AICA_ARM_WINDOW && a7_off(addr) < DC_AICA_REG_SIZE)
        return aica_reg_read(d, a7_off(addr));
    return 0;
}

static void a7_wb(void *u, uint32_t addr, uint8_t v)
{
    struct dc *d = u;
    if (addr < DC_AICA_SIZE) {
        d->aica_ram[addr] = v;
        return;
    }
    if (addr >= DC_AICA_ARM_WINDOW && a7_off(addr) < DC_AICA_REG_SIZE)
        aica_reg_write(d, a7_off(addr), v, 1);
}

static void a7_wh(void *u, uint32_t addr, uint16_t v)
{
    struct dc *d = u;
    if (addr < DC_AICA_SIZE - 1u) {
        d->aica_ram[addr] = (uint8_t)v;
        d->aica_ram[addr + 1u] = (uint8_t)(v >> 8);
        return;
    }
    if (addr >= DC_AICA_ARM_WINDOW && a7_off(addr) < DC_AICA_REG_SIZE)
        aica_reg_write(d, a7_off(addr), v, 1);
}

static void a7_ww(void *u, uint32_t addr, uint32_t v)
{
    struct dc *d = u;
    if (addr < DC_AICA_SIZE - 3u) {
        d->aica_ram[addr] = (uint8_t)v;
        d->aica_ram[addr + 1u] = (uint8_t)(v >> 8);
        d->aica_ram[addr + 2u] = (uint8_t)(v >> 16);
        d->aica_ram[addr + 3u] = (uint8_t)(v >> 24);
        return;
    }
    if (addr >= DC_AICA_ARM_WINDOW && a7_off(addr) < DC_AICA_REG_SIZE)
        aica_reg_write(d, a7_off(addr), v, 1);
}

static arm_bus_t aica_arm_bus = {
    NULL, a7_rb, a7_rh, a7_rw, a7_wb, a7_wh, a7_ww
};

void dc_aica_arm_init(struct dc *d)
{
    aica_arm_bus.user = d;
    arm_init(&d->aica.cpu, &aica_arm_bus, NULL); /* ARMv4T: no v5te */
    arm_reset(&d->aica.cpu);
}

void dc_aica_arm_step(struct dc *d, uint32_t cycles)
{
    uint32_t done = 0;
    while (done < cycles) {
        uint32_t c = arm_step(&d->aica.cpu);
        if (c == 0u)
            c = 1u; /* guard against a zero-cycle instruction */
        done += c;
    }
}

/* ---- mixer ------------------------------------------------------------------------------ */

static int16_t aica_sample(const struct dc *d, uint32_t ch, uint32_t byte)
{
    uint16_t fmt = (d->aica.ch[ch][0] >> 11) & 7u;
    byte &= 0x1FFFFFu;
    if (fmt == DC_AICA_FMT_PCM16) {
        uint32_t a = byte & 0x1FFFFEu;
        uint16_t raw =
            (uint16_t)(d->aica_ram[a] | (d->aica_ram[(a + 1u) & 0x1FFFFFu] << 8));
        return (int16_t)raw;
    }
    if (fmt == DC_AICA_FMT_PCM8) {
        /* unsigned 8-bit -> signed 16-bit */
        return (int16_t)(((int32_t)d->aica_ram[byte] - 128) << 8);
    }
    return 0; /* ADPCM: decoded as silence (documented pending) */
}

void dc_aica_mix(struct dc *d, int16_t *out, size_t frames)
{
    struct dc_aica *a = &d->aica;
    memset(out, 0, frames * 2u * sizeof(int16_t));
    for (uint32_t ch = 0; ch < DC_AICA_CH; ch++) {
        if (!a->active[ch])
            continue;
        uint32_t lea = a->ch[ch][3];
        uint32_t lsa = a->ch[ch][2];
        int loop = (a->ch[ch][12] & 0x4000u) != 0u;
        uint32_t fmt = (a->ch[ch][0] >> 11) & 7u;
        /* bytes per sample for the format (ADPCM pending: 1) */
        uint32_t bps = (fmt == DC_AICA_FMT_PCM16) ? 2u : 1u;
        /* pitch: rate = 44100 * 2^oct * (1 + fsc/1024); the position is
         * in bytes, so it advances bps * rate/44100 per output sample. */
        double rate = pow(2.0, aica_octave(a, ch)) *
                      (1.0 + (double)aica_fsc(a, ch) / 1024.0);
        uint64_t step = (uint64_t)(65536.0 * rate * (double)bps);
        if (step == 0u)
            step = 1u;
        uint32_t vol = a->ch[ch][14] & 0xFFu;     /* 0 = 0 dB, 255 = off */
        uint32_t pan = a->ch[ch][15] & 0xFFu;     /* 0 L, 128 C, 255 R */
        uint32_t amp = 255u - vol;
        uint32_t gl = (pan <= 128u) ? 255u : (255u - pan) * 2u;
        uint32_t gr = (pan >= 128u) ? 255u : pan * 2u;

        for (size_t f = 0; f < frames; f++) {
            int16_t s = aica_sample(d, ch, (uint32_t)(a->pos[ch] >> 16));
            int32_t sl = (int32_t)s * (int32_t)amp * (int32_t)gl / 65025;
            int32_t sr = (int32_t)s * (int32_t)amp * (int32_t)gr / 65025;
            int32_t l = (int32_t)out[f * 2u] + sl;
            int32_t r = (int32_t)out[f * 2u + 1u] + sr;
            out[f * 2u] =
                (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
            out[f * 2u + 1u] =
                (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
            a->pos[ch] += step;
            if ((a->pos[ch] >> 16) >= lea) { /* LEA is exclusive */
                if (loop)
                    a->pos[ch] = (uint64_t)lsa << 16;
                else {
                    a->pos[ch] = (uint64_t)lea << 16;
                    a->active[ch] = 0;
                    break;
                }
            }
        }
    }
}
