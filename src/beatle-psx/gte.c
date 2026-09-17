/*
 * beatle-psx: GTE geometry coprocessor (COP2).
 *
 * All commands follow the nocash PSX-SPX specification, including the
 * hardware UNR division table for RTPS/RTPT, the saturation/FLAG contract,
 * the IRGB/ORGB color conversion, and the MVMVA cv=2 (FC) two-part sum bug.
 * Integer-only: MAC values use 64-bit intermediates with 44-bit overflow
 * detection per the FLAG register definition.
 */
#include "psx.h"

/* ---- register index constants ------------------------------------------------- */
#define GTE_VXY0  0
#define GTE_VZ0   1
#define GTE_VXY1  2
#define GTE_VZ1   3
#define GTE_VXY2  4
#define GTE_VZ2   5
#define GTE_RGBC  6
#define GTE_OTZ   7
#define GTE_IR0   8
#define GTE_IR1   9
#define GTE_IR2  10
#define GTE_IR3  11
#define GTE_SXY0 12
#define GTE_SXY1 13
#define GTE_SXY2 14
#define GTE_SXYP 15
#define GTE_SZ0  16
#define GTE_SZ1  17
#define GTE_SZ2  18
#define GTE_SZ3  19
#define GTE_RGB0 20
#define GTE_RGB1 21
#define GTE_RGB2 22
#define GTE_MAC0 24
#define GTE_MAC1 25
#define GTE_MAC2 26
#define GTE_MAC3 27
#define GTE_IRGB 28
#define GTE_ORGB 29
#define GTE_LZCS 30
#define GTE_LZCR 31

#define CR_RT11 0   /* control regs: cr[i] = cop2r(32+i) */
#define CR_TRX  5
#define CR_TRY  6
#define CR_TRZ  7
#define CR_RBK  13
#define CR_GBK  14
#define CR_BBK  15
#define CR_RFC  21
#define CR_GFC  22
#define CR_BFC  23
#define CR_OFX  24
#define CR_OFY  25
#define CR_H    26
#define CR_DQA  27
#define CR_DQB  28
#define CR_ZSF3 29
#define CR_ZSF4 30
#define CR_FLAG 31

/* UNR division table (spec-defined; 257 entries). */
static const uint8_t gte_unr_table[257] = {
    0xFF,0xFD,0xFB,0xF9,0xF7,0xF5,0xF3,0xF1,0xEF,0xEE,0xEC,0xEA,0xE8,0xE6,0xE4,0xE3,
    0xE1,0xDF,0xDD,0xDC,0xDA,0xD8,0xD6,0xD5,0xD3,0xD1,0xD0,0xCE,0xCD,0xCB,0xC9,0xC8,
    0xC6,0xC5,0xC3,0xC1,0xC0,0xBE,0xBD,0xBB,0xBA,0xB8,0xB7,0xB5,0xB4,0xB2,0xB1,0xB0,
    0xAE,0xAD,0xAB,0xAA,0xA9,0xA7,0xA6,0xA4,0xA3,0xA2,0xA0,0x9F,0x9E,0x9C,0x9B,0x9A,
    0x99,0x97,0x96,0x95,0x94,0x92,0x91,0x90,0x8F,0x8D,0x8C,0x8B,0x8A,0x89,0x87,0x86,
    0x85,0x84,0x83,0x82,0x81,0x7F,0x7E,0x7D,0x7C,0x7B,0x7A,0x79,0x78,0x77,0x75,0x74,
    0x73,0x72,0x71,0x70,0x6F,0x6E,0x6D,0x6C,0x6B,0x6A,0x69,0x68,0x67,0x66,0x65,0x64,
    0x63,0x62,0x61,0x60,0x5F,0x5E,0x5D,0x5D,0x5C,0x5B,0x5A,0x59,0x58,0x57,0x56,0x55,
    0x54,0x53,0x53,0x52,0x51,0x50,0x4F,0x4E,0x4D,0x4D,0x4C,0x4B,0x4A,0x49,0x48,0x48,
    0x47,0x46,0x45,0x44,0x43,0x43,0x42,0x41,0x40,0x3F,0x3F,0x3E,0x3D,0x3C,0x3C,0x3B,
    0x3A,0x39,0x39,0x38,0x37,0x36,0x36,0x35,0x34,0x33,0x33,0x32,0x31,0x31,0x30,0x2F,
    0x2E,0x2E,0x2D,0x2C,0x2C,0x2B,0x2A,0x2A,0x29,0x28,0x28,0x27,0x26,0x26,0x25,0x24,
    0x24,0x23,0x22,0x22,0x21,0x20,0x20,0x1F,0x1E,0x1E,0x1D,0x1D,0x1C,0x1B,0x1B,0x1A,
    0x19,0x19,0x18,0x18,0x17,0x16,0x16,0x15,0x15,0x14,0x14,0x13,0x12,0x12,0x11,0x11,
    0x10,0x0F,0x0F,0x0E,0x0E,0x0D,0x0D,0x0C,0x0C,0x0B,0x0A,0x0A,0x09,0x09,0x08,0x08,
    0x07,0x07,0x06,0x06,0x05,0x05,0x04,0x04,0x03,0x03,0x02,0x02,0x01,0x01,0x00,0x00,
    0x00
};

/* ---- helpers --------------------------------------------------------------------- */

typedef struct {
    psx_gte_t *g;
    uint32_t flag; /* working FLAG value (bits 12-31) */
    uint32_t sf;   /* 0 or 1 */
    uint32_t lm;   /* 0 or 1 */
} gte_ctx;

/* Set MACn from a 44-bit intermediate; detect 44-bit overflow (FLAG 30-25). */
static inline int32_t set_mac(gte_ctx *x, int n, int64_t v)
{
    uint32_t pos_bit = (n == 1) ? (1u << 30) :
                       (n == 2) ? (1u << 29) : (1u << 28);
    if (v > (int64_t)0x7FFFFFFFFFFll)
        x->flag |= pos_bit;
    else if (v < -(int64_t)0x80000000000ll)
        x->flag |= pos_bit >> 3;
    int32_t mac = (int32_t)(uint32_t)((uint64_t)v & 0xFFFFFFFFu);
    x->g->dr[GTE_MAC0 + (uint32_t)n] = (uint32_t)mac;
    return mac;
}

/* IRn from MACn with saturation; FLAG bits 24/23/22. */
static inline int16_t mac_to_ir(gte_ctx *x, int n, int32_t mac)
{
    int32_t lo = x->lm ? 0 : -0x8000;
    int32_t hi = 0x7FFF;
    uint32_t bit = (n == 1) ? (1u << 24) : (n == 2) ? (1u << 23) : (1u << 22);
    int32_t v = mac;
    if (v < lo) {
        v = lo;
        x->flag |= bit;
    } else if (v > hi) {
        v = hi;
        x->flag |= bit;
    }
    uint32_t r = (uint32_t)(int16_t)v;
    x->g->dr[GTE_IR1 + (uint32_t)(n - 1)] = r;
    return (int16_t)v;
}

/* Color FIFO channel: MAC/16 saturated to 00..FF; FLAG bits 21/20/19. */
static inline uint8_t color_channel(gte_ctx *x, int32_t mac, uint32_t bit)
{
    int32_t v = mac >> 4;
    if (v < 0) {
        x->flag |= bit;
        return 0;
    }
    if (v > 0xFF) {
        x->flag |= bit;
        return 0xFF;
    }
    return (uint8_t)v;
}

static void color_fifo_push(gte_ctx *x, uint8_t r, uint8_t gg, uint8_t b,
                            uint8_t code)
{
    psx_gte_t *g = x->g;
    g->dr[GTE_RGB0] = g->dr[GTE_RGB1];
    g->dr[GTE_RGB1] = g->dr[GTE_RGB2];
    g->dr[GTE_RGB2] = ((uint32_t)code << 24) | ((uint32_t)b << 16) |
                      ((uint32_t)gg << 8) | (uint32_t)r;
}

/* "Color FIFO = [MAC1/16..], [IR1,IR2,IR3] = [MAC1..]" (final stage). */
static void fifo_from_mac(gte_ctx *x, uint8_t code)
{
    psx_gte_t *g = x->g;
    int32_t m1 = (int32_t)g->dr[GTE_MAC1];
    int32_t m2 = (int32_t)g->dr[GTE_MAC2];
    int32_t m3 = (int32_t)g->dr[GTE_MAC3];
    uint8_t r = color_channel(x, m1, 1u << 21);
    uint8_t gg = color_channel(x, m2, 1u << 20);
    uint8_t b = color_channel(x, m3, 1u << 19);
    color_fifo_push(x, r, gg, b, code);
    mac_to_ir(x, 1, m1);
    mac_to_ir(x, 2, m2);
    mac_to_ir(x, 3, m3);
}

/* MAC = MAC SAR (sf*12) (final step of NCDx/NCCx/CC/CDP/DPCS/INTPL). */
static void final_sar(gte_ctx *x)
{
    psx_gte_t *g = x->g;
    uint32_t sh = x->sf ? 12u : 0u;
    set_mac(x, 1, (int64_t)(int32_t)g->dr[GTE_MAC1] >> sh);
    set_mac(x, 2, (int64_t)(int32_t)g->dr[GTE_MAC2] >> sh);
    set_mac(x, 3, (int64_t)(int32_t)g->dr[GTE_MAC3] >> sh);
}

static inline int64_t mac_row(int32_t t, int32_t m1, int32_t v1, int32_t m2,
                              int32_t v2, int32_t m3, int32_t v3)
{
    return (int64_t)t * 4096ll + (int64_t)m1 * v1 + (int64_t)m2 * v2 +
           (int64_t)m3 * v3;
}

static inline int16_t mat_elem(psx_gte_t *g, uint32_t base, uint32_t idx)
{
    uint32_t r = g->cr[base + (idx >> 1)];
    return (idx & 1u) ? (int16_t)(r >> 16) : (int16_t)(r & 0xFFFFu);
}

static inline int32_t sar_mac(int64_t v, uint32_t sf)
{
    return (int32_t)(v >> (sf ? 12 : 0));
}

/* ---- register access ---------------------------------------------------------------- */

uint32_t psx_gte_read(psx_gte_t *g, uint32_t reg)
{
    if (reg < 32u) {
        switch (reg) {
        case GTE_ORGB: {
            uint32_t o = 0;
            for (int i = 0; i < 3; i++) {
                int32_t v = (int16_t)g->dr[GTE_IR1 + (uint32_t)i];
                uint32_t ch = (v < 0) ? 0u
                              : ((uint32_t)v > 0x0F80u
                                     ? 0x1Fu
                                     : ((uint32_t)v >> 7) & 0x1Fu);
                o |= ch << (5u * (uint32_t)i);
            }
            return o;
        }
        case GTE_LZCR: {
            uint32_t v = g->dr[GTE_LZCS];
            uint32_t n = 0;
            if (v == 0)
                return 32u;
            if (v & 0x80000000u) {
                while (((v << n) & 0x80000000u) != 0)
                    n++;
            } else {
                while (((v << n) & 0x80000000u) == 0)
                    n++;
            }
            return n;
        }
        default:
            return g->dr[reg];
        }
    }
    reg -= 32u;
    if (reg == CR_H)
        return (uint32_t)(int32_t)(int16_t)g->cr[CR_H]; /* read bug per spec */
    return g->cr[reg];
}

void psx_gte_write(psx_gte_t *g, uint32_t reg, uint32_t v)
{
    if (reg < 32u) {
        if (reg == GTE_IRGB) {
            g->dr[GTE_IR1] = (v & 0x1Fu) * 0x80u;
            g->dr[GTE_IR2] = ((v >> 5) & 0x1Fu) * 0x80u;
            g->dr[GTE_IR3] = ((v >> 10) & 0x1Fu) * 0x80u;
            g->dr[GTE_IRGB] = v & 0x7FFFu;
            return;
        }
        if (reg == GTE_SXYP) { /* move-on-write */
            g->dr[GTE_SXY0] = g->dr[GTE_SXY1];
            g->dr[GTE_SXY1] = g->dr[GTE_SXY2];
        }
        if (reg >= GTE_SZ0 && reg <= GTE_SZ3)
            g->dr[reg] = v & 0xFFFFu; /* unsigned 16-bit FIFOs */
        else
            g->dr[reg] = v;
        return;
    }
    reg -= 32u;
    if (reg == CR_FLAG)
        return; /* read-only */
    g->cr[reg] = v;
}

/* ---- RTPS / RTPT ----------------------------------------------------------------------- */

/* n = ((H*20000h/SZ3)+1)/2 via the UNR table; saturate at 1FFFFh. */
static uint32_t gte_divide(gte_ctx *x, uint32_t h, uint32_t sz3)
{
    if (h < sz3 * 2u && sz3 != 0) {
        uint32_t z = 0;
        while (((sz3 << z) & 0x8000u) == 0)
            z++; /* z = leading zeroes of the 16-bit SZ3 */
        uint32_t n = h << z;
        uint32_t d = sz3 << z; /* 8000h..FFFFh */
        uint32_t u = (uint32_t)gte_unr_table[(d - 0x7FC0u) >> 7] + 0x101u;
        d = (0x2000080u - (d * u)) >> 8;
        d = (0x80u + (d * u)) >> 8;
        uint64_t prod = (uint64_t)n * (uint64_t)d; /* 33-bit intermediate */
        uint32_t res = (uint32_t)((prod + 0x8000u) >> 16);
        if (res > 0x1FFFFu)
            res = 0x1FFFFu;
        return res;
    }
    x->flag |= (1u << 17) | (1u << 31); /* divide overflow */
    return 0x1FFFFu;
}

static void gte_rtps(gte_ctx *x, int vertex)
{
    psx_gte_t *g = x->g;
    uint32_t vxy = g->dr[GTE_VXY0 + 2u * (uint32_t)vertex];
    uint32_t vzs = g->dr[GTE_VZ0 + 2u * (uint32_t)vertex];
    int32_t vx = (int16_t)(vxy & 0xFFFFu);
    int32_t vy = (int16_t)(vxy >> 16);
    int32_t vz = (int16_t)vzs;

    int64_t r1 = mac_row((int32_t)g->cr[CR_TRX], mat_elem(g, CR_RT11, 0), vx,
                         mat_elem(g, CR_RT11, 1), vy, mat_elem(g, CR_RT11, 2),
                         vz);
    int64_t r2 = mac_row((int32_t)g->cr[CR_TRY], mat_elem(g, CR_RT11, 3), vx,
                         mat_elem(g, CR_RT11, 4), vy, mat_elem(g, CR_RT11, 5),
                         vz);
    int64_t r3 = mac_row((int32_t)g->cr[CR_TRZ], mat_elem(g, CR_RT11, 6), vx,
                         mat_elem(g, CR_RT11, 7), vy, mat_elem(g, CR_RT11, 8),
                         vz);

    /* IR1 = MAC1 = (...) SAR (sf*12): the MAC register holds the shifted
     * value per spec. RTPS/RTPT saturate IR1-3 regardless of lm. */
    int32_t mac1 = set_mac(x, 1, sar_mac(r1, x->sf));
    int32_t mac2 = set_mac(x, 2, sar_mac(r2, x->sf));
    int32_t mac3 = set_mac(x, 3, sar_mac(r3, x->sf));

    uint32_t save_lm = x->lm;
    x->lm = 0;
    mac_to_ir(x, 1, mac1);
    mac_to_ir(x, 2, mac2);
    mac_to_ir(x, 3, mac3);
    x->lm = save_lm;

    /* SZ3 = MAC3 SAR ((1-sf)*12), clamped to 0..FFFF (FLAG bit18). */
    int32_t sz3 = x->sf ? mac3 : (mac3 >> 12);
    if (sz3 < 0) {
        sz3 = 0;
        x->flag |= (1u << 18);
    } else if (sz3 > 0xFFFF) {
        sz3 = 0xFFFF;
        x->flag |= (1u << 18);
    }
    g->dr[GTE_SZ0] = g->dr[GTE_SZ1];
    g->dr[GTE_SZ1] = g->dr[GTE_SZ2];
    g->dr[GTE_SZ2] = g->dr[GTE_SZ3];
    g->dr[GTE_SZ3] = (uint32_t)sz3;

    uint32_t hval = g->cr[CR_H] & 0xFFFFu; /* H is unsigned in calculations */
    uint32_t div = gte_divide(x, hval, (uint32_t)sz3);

    /* SX2 = (div*IR1 + OFX) / 10000h with MAC0 overflow flags. */
    int64_t mac0 = (int64_t)div * (int16_t)g->dr[GTE_IR1] +
                   (int32_t)g->cr[CR_OFX];
    if (mac0 > 0x7FFFFFFFll) {
        mac0 = 0x7FFFFFFFll;
        x->flag |= (1u << 16);
    } else if (mac0 < -0x80000000ll) {
        mac0 = -0x80000000ll;
        x->flag |= (1u << 15);
    }
    g->dr[GTE_MAC0] = (uint32_t)(int32_t)mac0;
    int32_t sx = (int32_t)(mac0 / 0x10000ll);
    if (sx < -0x400) {
        sx = -0x400;
        x->flag |= (1u << 14);
    } else if (sx > 0x3FF) {
        sx = 0x3FF;
        x->flag |= (1u << 14);
    }

    mac0 = (int64_t)div * (int16_t)g->dr[GTE_IR2] + (int32_t)g->cr[CR_OFY];
    if (mac0 > 0x7FFFFFFFll) {
        mac0 = 0x7FFFFFFFll;
        x->flag |= (1u << 16);
    } else if (mac0 < -0x80000000ll) {
        mac0 = -0x80000000ll;
        x->flag |= (1u << 15);
    }
    g->dr[GTE_MAC0] = (uint32_t)(int32_t)mac0;
    int32_t sy = (int32_t)(mac0 / 0x10000ll);
    if (sy < -0x400) {
        sy = -0x400;
        x->flag |= (1u << 13);
    } else if (sy > 0x3FF) {
        sy = 0x3FF;
        x->flag |= (1u << 13);
    }

    g->dr[GTE_SXY0] = g->dr[GTE_SXY1];
    g->dr[GTE_SXY1] = g->dr[GTE_SXY2];
    g->dr[GTE_SXY2] = ((uint32_t)(uint16_t)sy << 16) | (uint32_t)(uint16_t)sx;

    if (vertex == 2) { /* depth cueing IR0 only for the last vertex */
        int64_t m0 = (int64_t)div * (int16_t)g->cr[CR_DQA] +
                     (int32_t)g->cr[CR_DQB];
        int32_t ir0 = (int32_t)(m0 / 0x1000ll);
        if (ir0 < 0) {
            ir0 = 0;
            x->flag |= (1u << 12);
        } else if (ir0 > 0x1000) {
            ir0 = 0x1000;
            x->flag |= (1u << 12);
        }
        g->dr[GTE_IR0] = (uint32_t)ir0;
    }
}

/* ---- MVMVA ---------------------------------------------------------------------------- */

static void gte_mvmva(gte_ctx *x, uint32_t mx, uint32_t v, uint32_t cv)
{
    psx_gte_t *g = x->g;
    int32_t v1, v2, v3;
    if (v == 0u) {
        v1 = (int16_t)(g->dr[GTE_VXY0] & 0xFFFFu);
        v2 = (int16_t)(g->dr[GTE_VXY0] >> 16);
        v3 = (int16_t)g->dr[GTE_VZ0];
    } else if (v == 1u) {
        v1 = (int16_t)(g->dr[GTE_VXY1] & 0xFFFFu);
        v2 = (int16_t)(g->dr[GTE_VXY1] >> 16);
        v3 = (int16_t)g->dr[GTE_VZ1];
    } else if (v == 2u) {
        v1 = (int16_t)(g->dr[GTE_VXY2] & 0xFFFFu);
        v2 = (int16_t)(g->dr[GTE_VXY2] >> 16);
        v3 = (int16_t)g->dr[GTE_VZ2];
    } else {
        v1 = (int16_t)g->dr[GTE_IR1];
        v2 = (int16_t)g->dr[GTE_IR2];
        v3 = (int16_t)g->dr[GTE_IR3];
    }

    int32_t t1, t2, t3;
    switch (cv) {
    case 0:
        t1 = (int32_t)g->cr[CR_TRX];
        t2 = (int32_t)g->cr[CR_TRY];
        t3 = (int32_t)g->cr[CR_TRZ];
        break;
    case 1:
        t1 = (int32_t)g->cr[CR_RBK];
        t2 = (int32_t)g->cr[CR_GBK];
        t3 = (int32_t)g->cr[CR_BBK];
        break;
    case 2:
        t1 = (int32_t)g->cr[CR_RFC];
        t2 = (int32_t)g->cr[CR_GFC];
        t3 = (int32_t)g->cr[CR_BFC];
        break;
    default:
        t1 = t2 = t3 = 0;
        break;
    }

    int32_t m11, m12, m13, m21, m22, m23, m31, m32, m33;
    if (mx == 3u) {
        /* Garbage matrix (spec): -R*10h, +R*10h, IR0, RT13x3, RT22x3. */
        int32_t rr = (int32_t)(g->dr[GTE_RGBC] & 0xFFu) * 16;
        m11 = -rr;
        m12 = rr;
        m13 = (int16_t)g->dr[GTE_IR0];
        m21 = mat_elem(g, CR_RT11, 2);
        m22 = m21;
        m23 = m21;
        m31 = mat_elem(g, CR_RT11, 4);
        m32 = m31;
        m33 = m31;
    } else {
        uint32_t base = (mx == 0u) ? CR_RT11 : (mx == 1u) ? 8u : 16u;
        m11 = mat_elem(g, base, 0);
        m12 = mat_elem(g, base, 1);
        m13 = mat_elem(g, base, 2);
        m21 = mat_elem(g, base, 3);
        m22 = mat_elem(g, base, 4);
        m23 = mat_elem(g, base, 5);
        m31 = mat_elem(g, base, 6);
        m32 = mat_elem(g, base, 7);
        m33 = mat_elem(g, base, 8);
    }

    if (cv == 2u) {
        /* Hardware bug (spec): the sum splits into two parts; the second
         * part overwrites the first. Part 1 saturates IR as if lm=0,
         * part 2 uses the lm setting. */
        uint32_t save_lm = x->lm;
        int64_t p1 = mac_row(t1, m11, v1, 0, 0, 0, 0) >> (x->sf ? 12 : 0);
        set_mac(x, 1, p1);
        x->lm = 0;
        mac_to_ir(x, 1, (int32_t)(uint32_t)((uint64_t)p1 & 0xFFFFFFFFu));
        x->lm = save_lm;
        int64_t p2 = ((int64_t)m12 * v2 + (int64_t)m13 * v3) >>
                     (x->sf ? 12 : 0);
        mac_to_ir(x, 1, set_mac(x, 1, p2));

        int64_t q1 = mac_row(t2, m21, v1, 0, 0, 0, 0) >> (x->sf ? 12 : 0);
        set_mac(x, 2, q1);
        x->lm = 0;
        mac_to_ir(x, 2, (int32_t)(uint32_t)((uint64_t)q1 & 0xFFFFFFFFu));
        x->lm = save_lm;
        int64_t q2 = ((int64_t)m22 * v2 + (int64_t)m23 * v3) >>
                     (x->sf ? 12 : 0);
        mac_to_ir(x, 2, set_mac(x, 2, q2));

        int64_t s1 = mac_row(t3, m31, v1, 0, 0, 0, 0) >> (x->sf ? 12 : 0);
        set_mac(x, 3, s1);
        x->lm = 0;
        mac_to_ir(x, 3, (int32_t)(uint32_t)((uint64_t)s1 & 0xFFFFFFFFu));
        x->lm = save_lm;
        int64_t s2 = ((int64_t)m32 * v2 + (int64_t)m33 * v3) >>
                     (x->sf ? 12 : 0);
        mac_to_ir(x, 3, set_mac(x, 3, s2));
        return;
    }

    mac_to_ir(x, 1, set_mac(x, 1, sar_mac(mac_row(t1, m11, v1, m12, v2, m13, v3),
                                          x->sf)));
    mac_to_ir(x, 2, set_mac(x, 2, sar_mac(mac_row(t2, m21, v1, m22, v2, m23, v3),
                                          x->sf)));
    mac_to_ir(x, 3, set_mac(x, 3, sar_mac(mac_row(t3, m31, v1, m32, v2, m33, v3),
                                          x->sf)));
}

/* ---- lighting pipeline ------------------------------------------------------------------ */

/* Stage 1+2 (and optional stage 3) of the color commands. */
static void gte_light_vertex(gte_ctx *x, int vertex, int with_color)
{
    psx_gte_t *g = x->g;
    int32_t vx, vy, vz;
    if (vertex < 3) {
        uint32_t vxy = g->dr[GTE_VXY0 + 2u * (uint32_t)vertex];
        uint32_t vzs = g->dr[GTE_VZ0 + 2u * (uint32_t)vertex];
        vx = (int16_t)(vxy & 0xFFFFu);
        vy = (int16_t)(vxy >> 16);
        vz = (int16_t)vzs;
    } else { /* vertex == 3: use IR as input vector (CC/CDP) */
        vx = (int16_t)g->dr[GTE_IR1];
        vy = (int16_t)g->dr[GTE_IR2];
        vz = (int16_t)g->dr[GTE_IR3];
    }

    /* Stage 1: IR = (LLM*V) SAR (sf*12) */
    int32_t mac1 = set_mac(x, 1, sar_mac(mac_row(0, mat_elem(g, 8, 0), vx,
                                                 mat_elem(g, 8, 1), vy,
                                                 mat_elem(g, 8, 2), vz),
                                         x->sf));
    int32_t mac2 = set_mac(x, 2, sar_mac(mac_row(0, mat_elem(g, 8, 3), vx,
                                                 mat_elem(g, 8, 4), vy,
                                                 mat_elem(g, 8, 5), vz),
                                         x->sf));
    int32_t mac3 = set_mac(x, 3, sar_mac(mac_row(0, mat_elem(g, 8, 6), vx,
                                                 mat_elem(g, 8, 7), vy,
                                                 mat_elem(g, 8, 8), vz),
                                         x->sf));
    mac_to_ir(x, 1, mac1);
    mac_to_ir(x, 2, mac2);
    mac_to_ir(x, 3, mac3);

    /* Stage 2: MAC = (BK*1000h + LCM*IR) SAR (sf*12) */
    mac1 = set_mac(x, 1, sar_mac(mac_row((int32_t)g->cr[CR_RBK],
                                         mat_elem(g, 16, 0),
                                         (int16_t)g->dr[GTE_IR1],
                                         mat_elem(g, 16, 1),
                                         (int16_t)g->dr[GTE_IR2],
                                         mat_elem(g, 16, 2),
                                         (int16_t)g->dr[GTE_IR3]),
                                 x->sf));
    mac2 = set_mac(x, 2, sar_mac(mac_row((int32_t)g->cr[CR_GBK],
                                         mat_elem(g, 16, 3),
                                         (int16_t)g->dr[GTE_IR1],
                                         mat_elem(g, 16, 4),
                                         (int16_t)g->dr[GTE_IR2],
                                         mat_elem(g, 16, 5),
                                         (int16_t)g->dr[GTE_IR3]),
                                 x->sf));
    mac3 = set_mac(x, 3, sar_mac(mac_row((int32_t)g->cr[CR_BBK],
                                         mat_elem(g, 16, 6),
                                         (int16_t)g->dr[GTE_IR1],
                                         mat_elem(g, 16, 7),
                                         (int16_t)g->dr[GTE_IR2],
                                         mat_elem(g, 16, 8),
                                         (int16_t)g->dr[GTE_IR3]),
                                 x->sf));
    mac_to_ir(x, 1, mac1);
    mac_to_ir(x, 2, mac2);
    mac_to_ir(x, 3, mac3);

    if (with_color) {
        /* Stage 3: MAC = [R*IR1,G*IR2,B*IR3] SHL 4 (no SAR here) */
        uint32_t rgb = g->dr[GTE_RGBC];
        int32_t rr = (int32_t)(rgb & 0xFFu);
        int32_t gg = (int32_t)((rgb >> 8) & 0xFFu);
        int32_t bb = (int32_t)((rgb >> 16) & 0xFFu);
        set_mac(x, 1, (int64_t)rr * (int16_t)g->dr[GTE_IR1] * 16ll);
        set_mac(x, 2, (int64_t)gg * (int16_t)g->dr[GTE_IR2] * 16ll);
        set_mac(x, 3, (int64_t)bb * (int16_t)g->dr[GTE_IR3] * 16ll);
    }
}

/* "MAC+(FC-MAC)*IR0" depth-cue step. */
static void gte_dc_step(gte_ctx *x)
{
    psx_gte_t *g = x->g;
    int32_t mac1 = (int32_t)g->dr[GTE_MAC1];
    int32_t mac2 = (int32_t)g->dr[GTE_MAC2];
    int32_t mac3 = (int32_t)g->dr[GTE_MAC3];
    uint32_t sh = x->sf ? 12u : 0u;

    /* [IR1,IR2,IR3] = ((FC SHL 12) - MAC) SAR (sf*12), saturated lm=0. */
    int64_t i1 = (((int64_t)(int32_t)g->cr[CR_RFC] * 4096ll) - mac1) >> sh;
    int64_t i2 = (((int64_t)(int32_t)g->cr[CR_GFC] * 4096ll) - mac2) >> sh;
    int64_t i3 = (((int64_t)(int32_t)g->cr[CR_BFC] * 4096ll) - mac3) >> sh;
    int32_t ir1 = (i1 < -0x8000ll) ? -0x8000 : (i1 > 0x7FFFll) ? 0x7FFF
                                                               : (int32_t)i1;
    int32_t ir2 = (i2 < -0x8000ll) ? -0x8000 : (i2 > 0x7FFFll) ? 0x7FFF
                                                               : (int32_t)i2;
    int32_t ir3 = (i3 < -0x8000ll) ? -0x8000 : (i3 > 0x7FFFll) ? 0x7FFF
                                                               : (int32_t)i3;
    g->dr[GTE_IR1] = (uint32_t)(int16_t)ir1;
    g->dr[GTE_IR2] = (uint32_t)(int16_t)ir2;
    g->dr[GTE_IR3] = (uint32_t)(int16_t)ir3;

    set_mac(x, 1, (int64_t)ir1 * (int16_t)g->dr[GTE_IR0] + mac1);
    set_mac(x, 2, (int64_t)ir2 * (int16_t)g->dr[GTE_IR0] + mac2);
    set_mac(x, 3, (int64_t)ir3 * (int16_t)g->dr[GTE_IR0] + mac3);
}

/* ---- command dispatch -------------------------------------------------------------------- */

void psx_gte_execute(psx_gte_t *g, uint32_t op)
{
    gte_ctx x;
    x.g = g;
    x.flag = 0;
    x.sf = (op >> 19) & 1u;
    x.lm = (op >> 10) & 1u; /* lm is bit 10 (verified vs PsyQ encodings) */
    uint32_t cmd = op & 0x3Fu;
    uint8_t code = (uint8_t)(g->dr[GTE_RGBC] >> 24);

    switch (cmd) {
    case 0x01: /* RTPS */
        gte_rtps(&x, 0);
        break;
    case 0x30: /* RTPT: three vertices, FIFO pushes each, flags accumulate */
        gte_rtps(&x, 0);
        gte_rtps(&x, 1);
        gte_rtps(&x, 2);
        break;

    case 0x06: { /* NCLIP */
        int32_t sx0 = (int16_t)(g->dr[GTE_SXY0] & 0xFFFFu);
        int32_t sy0 = (int16_t)(g->dr[GTE_SXY0] >> 16);
        int32_t sx1 = (int16_t)(g->dr[GTE_SXY1] & 0xFFFFu);
        int32_t sy1 = (int16_t)(g->dr[GTE_SXY1] >> 16);
        int32_t sx2 = (int16_t)(g->dr[GTE_SXY2] & 0xFFFFu);
        int32_t sy2 = (int16_t)(g->dr[GTE_SXY2] >> 16);
        int64_t mac0 = (int64_t)sx0 * (sy1 - sy2) +
                       (int64_t)sx1 * (sy2 - sy0) +
                       (int64_t)sx2 * (sy0 - sy1);
        if (mac0 > 0x7FFFFFFFll) {
            mac0 = 0x7FFFFFFFll;
            x.flag |= (1u << 16);
        } else if (mac0 < -0x80000000ll) {
            mac0 = -0x80000000ll;
            x.flag |= (1u << 15);
        }
        g->dr[GTE_MAC0] = (uint32_t)(int32_t)mac0;
        break;
    }
    case 0x2D: { /* AVSZ3 */
        uint32_t sum = g->dr[GTE_SZ1] + g->dr[GTE_SZ2] + g->dr[GTE_SZ3];
        int64_t mac0 = (int64_t)(int16_t)g->cr[CR_ZSF3] * sum;
        if (mac0 > 0x7FFFFFFFll) {
            mac0 = 0x7FFFFFFFll;
            x.flag |= (1u << 16);
        } else if (mac0 < -0x80000000ll) {
            mac0 = -0x80000000ll;
            x.flag |= (1u << 15);
        }
        int32_t otz = (int32_t)(mac0 / 0x1000ll);
        if (otz < 0) {
            otz = 0;
            x.flag |= (1u << 18);
        } else if (otz > 0xFFFF) {
            otz = 0xFFFF;
            x.flag |= (1u << 18);
        }
        g->dr[GTE_MAC0] = (uint32_t)(int32_t)mac0;
        g->dr[GTE_OTZ] = (uint32_t)otz;
        break;
    }
    case 0x2E: { /* AVSZ4 */
        uint32_t sum = g->dr[GTE_SZ0] + g->dr[GTE_SZ1] + g->dr[GTE_SZ2] +
                       g->dr[GTE_SZ3];
        int64_t mac0 = (int64_t)(int16_t)g->cr[CR_ZSF4] * sum;
        if (mac0 > 0x7FFFFFFFll) {
            mac0 = 0x7FFFFFFFll;
            x.flag |= (1u << 16);
        } else if (mac0 < -0x80000000ll) {
            mac0 = -0x80000000ll;
            x.flag |= (1u << 15);
        }
        int32_t otz = (int32_t)(mac0 / 0x1000ll);
        if (otz < 0) {
            otz = 0;
            x.flag |= (1u << 18);
        } else if (otz > 0xFFFF) {
            otz = 0xFFFF;
            x.flag |= (1u << 18);
        }
        g->dr[GTE_MAC0] = (uint32_t)(int32_t)mac0;
        g->dr[GTE_OTZ] = (uint32_t)otz;
        break;
    }

    case 0x10: { /* DPCS */
        uint32_t rgb = g->dr[GTE_RGBC];
        set_mac(&x, 1, (int64_t)(rgb & 0xFFu) * 65536ll);
        set_mac(&x, 2, (int64_t)((rgb >> 8) & 0xFFu) * 65536ll);
        set_mac(&x, 3, (int64_t)((rgb >> 16) & 0xFFu) * 65536ll);
        gte_dc_step(&x);
        final_sar(&x);
        fifo_from_mac(&x, code);
        break;
    }
    case 0x11: { /* INTPL */
        set_mac(&x, 1, (int64_t)(int16_t)g->dr[GTE_IR1] * 4096ll);
        set_mac(&x, 2, (int64_t)(int16_t)g->dr[GTE_IR2] * 4096ll);
        set_mac(&x, 3, (int64_t)(int16_t)g->dr[GTE_IR3] * 4096ll);
        gte_dc_step(&x);
        final_sar(&x);
        fifo_from_mac(&x, code);
        break;
    }
    case 0x12: { /* MVMVA */
        uint32_t mx = (op >> 17) & 3u;
        uint32_t v = (op >> 15) & 3u;
        uint32_t cv = (op >> 13) & 3u;
        gte_mvmva(&x, mx, v, cv);
        break;
    }
    case 0x13: /* NCDS */
        gte_light_vertex(&x, 0, 1);
        gte_dc_step(&x);
        final_sar(&x);
        fifo_from_mac(&x, code);
        break;
    case 0x16: /* NCDT */
        for (int i = 0; i < 3; i++) {
            gte_light_vertex(&x, i, 1);
            gte_dc_step(&x);
            final_sar(&x);
            fifo_from_mac(&x, code);
        }
        break;
    case 0x1B: /* NCCS */
        gte_light_vertex(&x, 0, 1);
        final_sar(&x);
        fifo_from_mac(&x, code);
        break;
    case 0x1E: /* NCS */
        gte_light_vertex(&x, 0, 0);
        fifo_from_mac(&x, code);
        break;
    case 0x20: /* NCT */
        for (int i = 0; i < 3; i++) {
            gte_light_vertex(&x, i, 0);
            fifo_from_mac(&x, code);
        }
        break;
    case 0x3F: /* NCCT */
        for (int i = 0; i < 3; i++) {
            gte_light_vertex(&x, i, 1);
            final_sar(&x);
            fifo_from_mac(&x, code);
        }
        break;
    case 0x1C: /* CC */
        gte_light_vertex(&x, 3, 1);
        final_sar(&x);
        fifo_from_mac(&x, code);
        break;
    case 0x14: /* CDP */
        gte_light_vertex(&x, 3, 1);
        gte_dc_step(&x);
        final_sar(&x);
        fifo_from_mac(&x, code);
        break;
    case 0x29: { /* DCPL */
        uint32_t rgb = g->dr[GTE_RGBC];
        set_mac(&x, 1, (int64_t)(rgb & 0xFFu) * (int16_t)g->dr[GTE_IR1] * 16ll);
        set_mac(&x, 2,
                (int64_t)((rgb >> 8) & 0xFFu) * (int16_t)g->dr[GTE_IR2] * 16ll);
        set_mac(&x, 3,
                (int64_t)((rgb >> 16) & 0xFFu) * (int16_t)g->dr[GTE_IR3] * 16ll);
        gte_dc_step(&x);
        final_sar(&x);
        fifo_from_mac(&x, code);
        break;
    }
    case 0x2A: { /* DPCT */
        for (int i = 0; i < 3; i++) {
            uint32_t rgb = g->dr[GTE_RGB0]; /* reads FIFO bottom per spec */
            set_mac(&x, 1, (int64_t)(rgb & 0xFFu) * 65536ll);
            set_mac(&x, 2, (int64_t)((rgb >> 8) & 0xFFu) * 65536ll);
            set_mac(&x, 3, (int64_t)((rgb >> 16) & 0xFFu) * 65536ll);
            gte_dc_step(&x);
            final_sar(&x);
            fifo_from_mac(&x, code);
        }
        break;
    }
    case 0x28: { /* SQR */
        int32_t i1 = (int16_t)g->dr[GTE_IR1];
        int32_t i2 = (int16_t)g->dr[GTE_IR2];
        int32_t i3 = (int16_t)g->dr[GTE_IR3];
        int64_t r1 = (int64_t)i1 * i1;
        int64_t r2 = (int64_t)i2 * i2;
        int64_t r3 = (int64_t)i3 * i3;
        if (x.sf) {
            r1 >>= 12;
            r2 >>= 12;
            r3 >>= 12;
        }
        mac_to_ir(&x, 1, set_mac(&x, 1, r1));
        mac_to_ir(&x, 2, set_mac(&x, 2, r2));
        mac_to_ir(&x, 3, set_mac(&x, 3, r3));
        break;
    }
    case 0x0C: { /* OP: outer product (RT11/RT22/RT33 as vector) */
        int32_t d1 = (int16_t)(g->cr[CR_RT11] & 0xFFFFu);
        int32_t d2 = (int16_t)(g->cr[CR_RT11] >> 16);
        int32_t d3 = (int16_t)g->cr[CR_RT11 + 2u];
        int32_t i1 = (int16_t)g->dr[GTE_IR1];
        int32_t i2 = (int16_t)g->dr[GTE_IR2];
        int32_t i3 = (int16_t)g->dr[GTE_IR3];
        int64_t r1 = (int64_t)i3 * d2 - (int64_t)i2 * d3;
        int64_t r2 = (int64_t)i1 * d3 - (int64_t)i3 * d1;
        int64_t r3 = (int64_t)i2 * d1 - (int64_t)i1 * d2;
        if (x.sf) {
            r1 >>= 12;
            r2 >>= 12;
            r3 >>= 12;
        }
        mac_to_ir(&x, 1, set_mac(&x, 1, r1));
        mac_to_ir(&x, 2, set_mac(&x, 2, r2));
        mac_to_ir(&x, 3, set_mac(&x, 3, r3));
        break;
    }
    case 0x3D: /* GPF */
    case 0x3E: { /* GPL */
        if (cmd == 0x3Eu) {
            set_mac(&x, 1,
                    (int64_t)(int32_t)g->dr[GTE_MAC1] << (x.sf ? 12 : 0));
            set_mac(&x, 2,
                    (int64_t)(int32_t)g->dr[GTE_MAC2] << (x.sf ? 12 : 0));
            set_mac(&x, 3,
                    (int64_t)(int32_t)g->dr[GTE_MAC3] << (x.sf ? 12 : 0));
        } else {
            set_mac(&x, 1, 0);
            set_mac(&x, 2, 0);
            set_mac(&x, 3, 0);
        }
        int64_t r1 = (int64_t)(int16_t)g->dr[GTE_IR1] *
                         (int16_t)g->dr[GTE_IR0] +
                     (int32_t)g->dr[GTE_MAC1];
        int64_t r2 = (int64_t)(int16_t)g->dr[GTE_IR2] *
                         (int16_t)g->dr[GTE_IR0] +
                     (int32_t)g->dr[GTE_MAC2];
        int64_t r3 = (int64_t)(int16_t)g->dr[GTE_IR3] *
                         (int16_t)g->dr[GTE_IR0] +
                     (int32_t)g->dr[GTE_MAC3];
        mac_to_ir(&x, 1, set_mac(&x, 1, r1 >> (x.sf ? 12 : 0)));
        mac_to_ir(&x, 2, set_mac(&x, 2, r2 >> (x.sf ? 12 : 0)));
        mac_to_ir(&x, 3, set_mac(&x, 3, r3 >> (x.sf ? 12 : 0)));
        fifo_from_mac(&x, code);
        break;
    }
    default:
        /* N/A opcodes: no effect beyond FLAG reset (documented) */
        break;
    }

    g->cr[CR_FLAG] = x.flag & 0xFFFFF000u; /* bits 31-12 only */
}
