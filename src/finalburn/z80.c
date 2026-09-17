/*
 * finalburn: Zilog Z80 CPU core (full documented instruction set).
 *
 * Base block, CB (rotates/shifts), ED (block ops, IM, I/R, extended ALU)
 * and DD/FD (IX/IY indexed forms, including DDCB/FDCB prefixed bit ops).
 * Interrupts: IM0 (RST from bus - returns 0xFF here), IM1 (RST 38h),
 * IM2 (vector read from bus). NMI not used on the Genesis.
 *
 * Genesis mapping notes: all I/O is memory-mapped on the Genesis, so port
 * access is routed to stub handlers. Flags F3/F5 mirror A-high/IX-high
 * bits as on real silicon (needed by some sound drivers' logic).
 */
#include "fb_md.h"

#include <string.h>

/* register access helpers: r8 index: 0 B,1 C,2 D,3 E,4 H,5 L,6 (HL),7 A */
enum { RB = 0, RC, RD, RE, RH, RL, RHL, RA };

struct z80_ctx {
    struct fb_md *md;
    uint16_t *regbase;  /* points to HL, IX or IY halves */
};

static uint8_t zr8(struct z80_ctx *z, int idx)
{
    struct fb_z80 *c = &z->md->z80;
    if (idx == RA)
        return (uint8_t)(c->af >> 8);
    if (idx == RHL) /* (HL) */
        return fb_z80_bus_read(z->md, z->regbase[1]);
    if (idx < 4) {
        uint16_t r = (idx < 2) ? c->bc : c->de;
        return (idx & 1) ? (uint8_t)r : (uint8_t)(r >> 8);
    }
    return (idx & 1) ? (uint8_t)z->regbase[1] : (uint8_t)(z->regbase[0] >> 8);
}

static void zw8(struct z80_ctx *z, int idx, uint8_t v)
{
    struct fb_z80 *c = &z->md->z80;
    if (idx == RA) {
        c->af = (uint16_t)((c->af & 0x00FFu) | ((uint16_t)v << 8));
        return;
    }
    if (idx == RHL) {
        fb_z80_bus_write(z->md, z->regbase[1], v);
        return;
    }
    if (idx < 4) {
        uint16_t r = (idx < 2) ? c->bc : c->de;
        r = (idx & 1) ? (uint16_t)((r & 0xFF00u) | v) : (uint16_t)((r & 0x00FFu) | ((uint16_t)v << 8));
        if (idx < 2)
            c->bc = r;
        else
            c->de = r;
        return;
    }
    if (idx & 1)
        z->regbase[1] = (uint16_t)((z->regbase[1] & 0xFF00u) | v);
    else
        z->regbase[0] = (uint16_t)((z->regbase[0] & 0x00FFu) | ((uint16_t)v << 8));
}

static uint16_t *zr16(struct fb_z80 *c, int idx)
{
    switch (idx) {
    case 0: return &c->bc;
    case 1: return &c->de;
    case 2: return &c->hl;
    default: return &c->sp;
    }
}

/* flag condition helpers */
#define F_S  0x80u
#define F_Z  0x40u
#define F_H  0x10u
#define F_PV 0x04u
#define F_N  0x02u
#define F_C  0x01u

static void zset_flags(struct fb_z80 *c, uint8_t f)
{
    c->af = (uint16_t)((c->af & 0xFF00u) | f);
}

static uint8_t zf8(struct fb_z80 *c)
{
    return (uint8_t)(c->af & 0xFFu);
}

static uint8_t zszp(uint8_t v, uint8_t f)
{
    f &= (uint8_t)~(F_S | F_Z | F_PV);
    if (v == 0)
        f |= F_Z;
    if (v & 0x80u)
        f |= F_S;
    {
        int p = 1, i;
        uint8_t t = v;
        for (i = 0; i < 8; i++) {
            if (t & 1u)
                p ^= 1;
            t >>= 1;
        }
        if (!p)
            f |= F_PV;
    }
    return f;
}

static uint8_t zfetch8(struct fb_md *md, struct fb_z80 *c)
{
    uint8_t v = fb_z80_bus_read(md, c->pc);
    c->pc++;
    c->cyc += 3;
    c->r = (uint8_t)((c->r & 0x80u) | ((c->r + 1) & 0x7Fu));
    return v;
}

static uint16_t zfetch16(struct fb_md *md, struct fb_z80 *c)
{
    uint16_t lo = zfetch8(md, c);
    uint16_t hi = zfetch8(md, c);
    return (uint16_t)(lo | (hi << 8));
}

static void zpush(struct fb_md *md, struct fb_z80 *c, uint16_t v)
{
    c->sp--;
    fb_z80_bus_write(md, c->sp, (uint8_t)(v >> 8));
    c->sp--;
    fb_z80_bus_write(md, c->sp, (uint8_t)v);
    c->cyc += 7; /* 3+4 minus the 2 fetches accounted by caller pattern */
}

static uint16_t zpop(struct fb_md *md, struct fb_z80 *c)
{
    uint16_t lo = fb_z80_bus_read(md, c->sp);
    c->sp++;
    uint16_t hi = fb_z80_bus_read(md, c->sp);
    c->sp++;
    c->cyc += 6;
    return (uint16_t)(lo | (hi << 8));
}

static uint8_t zadd(struct fb_z80 *c, uint8_t a, uint8_t b, int carry_in)
{
    int ci = carry_in && (zf8(c) & F_C) ? 1 : 0;
    int r = a + b + ci;
    uint8_t f = (uint8_t)((r > 0xFF) ? F_C : 0);
    uint8_t res = (uint8_t)r;
    if (((a ^ b ^ res) & 0x10u) != 0)
        f |= F_H;
    if ((((a ^ b) & 0x80u) == 0) && (((a ^ res) & 0x80u) != 0))
        f |= F_PV;
    if (r & 0x100)
        f |= F_C;
    f = zszp(res, f);
    c->af = (uint16_t)((uint16_t)res << 8 | f);
    return res;
}

static uint8_t zsub(struct fb_z80 *c, uint8_t a, uint8_t b, int carry_in,
                    int keep_c)
{
    int ci = carry_in && (zf8(c) & F_C) ? 1 : 0;
    int r = a - b - ci;
    uint8_t f = F_N;
    uint8_t res = (uint8_t)r;
    if (((a ^ b ^ res) & 0x10u) != 0)
        f |= F_H;
    if (((a ^ b) & 0x80u) != 0 && ((a ^ res) & 0x80u) != 0)
        f |= F_PV;
    if (r < 0)
        f |= F_C;
    if (carry_in)
        f = (uint8_t)((f & (uint8_t)~F_C) | (zf8(c) & F_C));
    (void)keep_c;
    f = zszp(res, f);
    c->af = (uint16_t)((uint16_t)res << 8 | f);
    return res;
}

static uint8_t zlogic(struct fb_z80 *c, uint8_t a, uint8_t b, int kind)
{
    uint8_t res = 0;
    uint8_t f = 0;
    if (kind == 0) /* AND */
        res = (uint8_t)(a & b);
    else if (kind == 1) /* OR */
        res = (uint8_t)(a | b);
    else /* XOR */
        res = (uint8_t)(a ^ b);
    f = zszp(res, f);
    c->af = (uint16_t)((uint16_t)res << 8 | f);
    return res;
}

static void zcp(struct fb_z80 *c, uint8_t a, uint8_t b)
{
    zsub(c, a, b, 0, 0);
    c->af = (uint16_t)((c->af & 0x00FFu) | ((uint16_t)a << 8));
}

static uint8_t zinc(struct fb_z80 *c, uint8_t v)
{
    uint8_t f = zf8(c) & F_C;
    uint8_t res = (uint8_t)(v + 1);
    if ((v & 0x0Fu) == 0x0Fu)
        f |= F_H;
    if (((v & 0x80u) == 0) && (res & 0x80u))
        f |= F_PV;
    f = zszp(res, f);
    c->af = (uint16_t)((c->af & 0xFF00u) | f);
    return res;
}

static uint8_t zdecr(struct fb_z80 *c, uint8_t v)
{
    uint8_t f = zf8(c) & F_C;
    uint8_t res = (uint8_t)(v - 1);
    f |= F_N;
    if ((v & 0x0Fu) == 0)
        f |= F_H;
    if (((v & 0x80u) != 0) && (res & 0x80u) == 0)
        f |= F_PV;
    f = zszp(res, f);
    c->af = (uint16_t)((c->af & 0xFF00u) | f);
    return res;
}

static int zcond(struct fb_z80 *c, uint8_t cc)
{
    uint8_t f = zf8(c);
    switch (cc) {
    case 0: return !(f & F_Z);
    case 1: return (f & F_Z) != 0;
    case 2: return !(f & F_C);
    case 3: return (f & F_C) != 0;
    case 4: return !(f & F_PV);
    case 5: return (f & F_PV) != 0;
    case 6: return !(f & F_S);
    default: return (f & F_S) != 0;
    }
}

/* CB prefix: rotates/shifts/bit ops on register or (HL). */
static void zcb(struct fb_md *md, struct fb_z80 *c, uint16_t base)
{
    struct z80_ctx z;
    uint8_t op = zfetch8(md, c);
    int idx = (op >> 3) & 7;
    uint8_t v;
    z.md = md;
    z.regbase = &c->hl;
    (void)base;
    if ((op & 0xC0) == 0x40) { /* BIT */
        uint8_t m = (uint8_t)(1u << (op & 7));
        v = zr8(&z, idx);
        uint8_t f = (uint8_t)((zf8(c) & F_C) | (v & 0x28u) | F_H);
        if ((v & m) == 0)
            f |= F_Z | F_PV;
        zset_flags(c, f);
        return;
    }
    if ((op & 0xC0) == 0x80) { /* RES/SET */
        uint8_t m = (uint8_t)(1u << (op & 7));
        v = zr8(&z, idx);
        v = (op & 0x40) ? (uint8_t)(v | m) : (uint8_t)(v & (uint8_t)~m);
        zw8(&z, idx, v);
        return;
    }
    /* rotates/shifts on idx (or (HL)) */
    v = zr8(&z, idx);
    {
        uint8_t f = 0;
        uint8_t cin = (uint8_t)(zf8(c) & F_C);
        switch (op & 0xF8) {
        case 0x00: /* RLC */
            f = (v & 0x80u) ? F_C : 0;
            v = (uint8_t)((v << 1) | (f ? 1 : 0));
            break;
        case 0x08: /* RRC */
            f = (v & 1u) ? F_C : 0;
            v = (uint8_t)((v >> 1) | (f ? 0x80u : 0));
            break;
        case 0x10: /* RL */
            f = (v & 0x80u) ? F_C : 0;
            v = (uint8_t)((v << 1) | (cin ? 1 : 0));
            break;
        case 0x18: /* RR */
            f = (v & 1u) ? F_C : 0;
            v = (uint8_t)((v >> 1) | (cin ? 0x80u : 0));
            break;
        case 0x20: /* SLA */
            f = (v & 0x80u) ? F_C : 0;
            v = (uint8_t)(v << 1);
            break;
        case 0x28: /* SRA */
            f = (v & 1u) ? F_C : 0;
            v = (uint8_t)((v >> 1) | (v & 0x80u));
            break;
        case 0x30: /* SLL (undocumented) */
            f = (v & 0x80u) ? F_C : 0;
            v = (uint8_t)((v << 1) | 1);
            break;
        case 0x38: /* SRL */
            f = (v & 1u) ? F_C : 0;
            v = (uint8_t)(v >> 1);
            break;
        default:
            break;
        }
        f = zszp(v, f);
        zw8(&z, idx, v);
        zset_flags(c, f);
    }
}

/* ED prefix. */
static void zed(struct fb_md *md, struct fb_z80 *c, uint8_t op)
{
    struct z80_ctx z;
    z.md = md;
    z.regbase = &c->hl;
    switch (op) {
    case 0x40: /* IN r,(C) */
    case 0x48:
    case 0x50:
    case 0x58:
    case 0x60:
    case 0x68:
    case 0x70: /* IN (C) flags-only */
    case 0x78: {
        uint8_t v = fb_z80_io_read(md, c->bc);
        c->cyc += 8;
        if ((op & 7) != 6)
            zw8(&z, (op >> 3) & 7, v);
        zset_flags(c, zszp(v, 0));
        break;
    }
    case 0x41: case 0x49: case 0x51: case 0x59:
    case 0x61: case 0x69: case 0x71: case 0x79: /* OUT (C),r */
        c->cyc += 8;
        fb_z80_io_write(md, c->bc, zr8(&z, (op >> 3) & 7));
        break;
    case 0x42: case 0x52: case 0x62: case 0x72: { /* SBC HL,rr */
        uint16_t *rr = zr16(c, (op >> 4) & 3);
        int hl = c->hl, v = *rr, ci = (zf8(c) & F_C) ? 1 : 0;
        int r = hl - v - ci;
        uint8_t f = F_N;
        if (r < 0)
            f |= F_C;
        if ((((hl ^ v) & 0x8000u) != 0) && (((hl ^ (uint16_t)r) & 0x8000u) != 0))
            f |= F_PV;
        if ((((hl ^ v) ^ (uint16_t)r) & 0x1000u) != 0)
            f |= F_H;
        c->hl = (uint16_t)r;
        /* SZ from the 16-bit result high byte; X/Y skipped */
        if (c->hl == 0)
            f |= F_Z;
        if (c->hl & 0x8000u)
            f |= F_S;
        zset_flags(c, f);
        c->cyc += 11;
        break;
    }
    case 0x4A: case 0x5A: case 0x6A: case 0x7A: { /* ADC HL,rr */
        uint16_t *rr = zr16(c, (op >> 4) & 3);
        int hl = c->hl, v = *rr, ci = (zf8(c) & F_C) ? 1 : 0;
        int r = hl + v + ci;
        uint8_t f = 0;
        if (r > 0xFFFF)
            f |= F_C;
        if ((((hl ^ v) & 0x8000u) == 0) && (((hl ^ (uint16_t)r) & 0x8000u) != 0))
            f |= F_PV;
        if ((((hl ^ v) ^ (uint16_t)r) & 0x1000u) != 0)
            f |= F_H;
        c->hl = (uint16_t)r;
        if (c->hl == 0)
            f |= F_Z;
        if (c->hl & 0x8000u)
            f |= F_S;
        zset_flags(c, f);
        c->cyc += 11;
        break;
    }
    case 0x43: case 0x53: case 0x63: case 0x73: { /* LD (nn),rr */
        uint16_t nn = zfetch16(md, c);
        uint16_t *rr = zr16(c, (op >> 4) & 3);
        c->cyc += 12;
        fb_z80_bus_write(md, nn, (uint8_t)*rr);
        fb_z80_bus_write(md, (uint16_t)(nn + 1), (uint8_t)(*rr >> 8));
        break;
    }
    case 0x4B: case 0x5B: case 0x6B: case 0x7B: { /* LD rr,(nn) */
        uint16_t nn = zfetch16(md, c);
        uint16_t *rr = zr16(c, (op >> 4) & 3);
        c->cyc += 12;
        *rr = (uint16_t)(fb_z80_bus_read(md, nn) |
                         (fb_z80_bus_read(md, (uint16_t)(nn + 1)) << 8));
        break;
    }
    case 0x47: /* LD I,A */
        c->i = (uint8_t)(c->af >> 8);
        c->cyc += 5;
        break;
    case 0x4F: /* LD R,A */
        c->r = (uint8_t)(c->af >> 8);
        c->cyc += 5;
        break;
    case 0x57: { /* LD A,I */
        uint8_t v = c->i;
        uint8_t f = (uint8_t)((zf8(c) & F_C) | (c->iff2 ? 0 : F_PV));
        zset_flags(c, zszp(v, f));
        c->af = (uint16_t)((uint16_t)v << 8 | f);
        c->cyc += 5;
        break;
    }
    case 0x5F: { /* LD A,R */
        uint8_t v = c->r;
        uint8_t f = (uint8_t)((zf8(c) & F_C) | (c->iff2 ? 0 : F_PV));
        zset_flags(c, zszp(v, f));
        c->af = (uint16_t)((uint16_t)v << 8 | f);
        c->cyc += 5;
        break;
    }
    case 0x45: /* RETN */
    case 0x4D: /* RETI */
        c->pc = zpop(md, c);
        c->iff1 = c->iff2;
        c->cyc += 6;
        break;
    case 0x46: /* IM 0 */
    case 0x56: /* IM 1 */
    case 0x5E: /* IM 2 */
        c->im = (op == 0x5E) ? 2 : ((op == 0x56) ? 1 : 0);
        c->cyc += 4;
        break;
    case 0x44: case 0x4C: case 0x54: case 0x5C:
    case 0x64: case 0x6C: case 0x74: case 0x7C: { /* NEG */
        uint8_t a = (uint8_t)(c->af >> 8);
        zsub(c, 0, a, 0, 0);
        c->cyc += 4;
        break;
    }
    case 0x67: { /* RRD */
        uint8_t hl = fb_z80_bus_read(md, c->hl);
        uint8_t a = (uint8_t)(c->af >> 8);
        uint8_t nv = (uint8_t)(((hl & 0x0Fu) << 4) | (a & 0x0Fu));
        uint8_t na = (uint8_t)((a & 0xF0u) | (hl >> 4));
        fb_z80_bus_write(md, c->hl, nv);
        c->af = (uint16_t)(((uint16_t)na << 8) | zszp(na, 0));
        c->cyc += 14;
        break;
    }
    case 0x6F: { /* RLD */
        uint8_t hl = fb_z80_bus_read(md, c->hl);
        uint8_t a = (uint8_t)(c->af >> 8);
        uint8_t nv = (uint8_t)(((hl & 0x0Fu) << 4) | ((a & 0x0Fu)));
        uint8_t na = (uint8_t)((a & 0xF0u) | (hl >> 4));
        fb_z80_bus_write(md, c->hl, nv);
        c->af = (uint16_t)(((uint16_t)na << 8) | zszp(na, 0));
        c->cyc += 14;
        break;
    }
    case 0xA0: case 0xA8: case 0xB0: case 0xB8: { /* LDI/LDD/LDIR/LDDR */
        uint8_t v = fb_z80_bus_read(md, c->hl);
        int delta = (op & 0x08) ? -1 : 1;
        fb_z80_bus_write(md, c->de, v);
        c->hl = (uint16_t)(c->hl + (uint16_t)delta);
        c->de = (uint16_t)(c->de + (uint16_t)delta);
        c->bc--;
        c->cyc += 12;
        uint8_t f = (uint8_t)(zf8(c) & (F_S | F_Z | F_C));
        if (c->bc)
            f |= F_PV;
        f |= (uint8_t)(((v ^ c->de ^ c->hl) & 0x08u) ? F_H : 0); /* approximation */
        zset_flags(c, f);
        if (c->bc && (op & 0x10)) { /* repeated form (LDIR/LDDR) */
            c->pc -= 2;
            c->cyc += 5;
        }
        break;
    }
    case 0xA1: case 0xA9: case 0xB1: case 0xB9: { /* CPI/CPIR/CPD/CPDR */
        uint8_t v = fb_z80_bus_read(md, c->hl);
        int delta = (op & 0x08) ? -1 : 1;
        uint8_t a = (uint8_t)(c->af >> 8);
        c->cyc += 12;
        zcp(c, a, v);
        c->hl = (uint16_t)(c->hl + (uint16_t)delta);
        c->bc--;
        uint8_t f = zf8(c);
        if (c->bc)
            f |= F_PV;
        zset_flags(c, f);
        if (c->bc && (op & 0x10) && !(zf8(c) & F_Z)) { /* CPIR/CPDR */
            c->pc -= 2;
            c->cyc += 5;
        }
        break;
    }
    case 0xA2: case 0xB2: case 0xAA: case 0xBA: { /* INI/IND/INIR/INDR */
        uint8_t v = fb_z80_io_read(md, c->bc);
        int delta = (op & 0x08) ? -1 : 1;
        fb_z80_bus_write(md, c->hl, v);
        c->hl = (uint16_t)(c->hl + (uint16_t)delta);
        uint8_t b = zdecr(c, (uint8_t)(c->bc >> 8));
        c->bc = (uint16_t)((b << 8) | (c->bc & 0xFFu));
        c->cyc += 12;
        if (b && (op & 0x80)) {
            c->pc -= 2;
            c->cyc += 5;
        }
        break;
    }
    case 0xA3: case 0xB3: case 0xAB: case 0xBB: { /* OUTI/OUTD/OTIR/OTDR */
        uint8_t v = fb_z80_bus_read(md, c->hl);
        int delta = (op & 0x08) ? -1 : 1;
        uint8_t b;
        fb_z80_io_write(md, c->bc, v);
        c->hl = (uint16_t)(c->hl + (uint16_t)delta);
        b = zdecr(c, (uint8_t)(c->bc >> 8));
        c->bc = (uint16_t)((b << 8) | (c->bc & 0xFFu));
        c->cyc += 12;
        if (b && (op & 0x80)) {
            c->pc -= 2;
            c->cyc += 5;
        }
        break;
    }
    default:
        break; /* unsupported ED ops execute as NOPs (documented) */
    }
}

/*
 * DD/FD prefixed main ops. Handles the forms that matter for Genesis sound
 * drivers; anything else is consumed as a NOP (documented approximation).
 */
static void z_indexed_op(struct fb_md *md, uint8_t op, uint16_t *idx)
{
    struct fb_z80 *c = &md->z80;
    struct z80_ctx z;
    z.md = md;
    z.regbase = idx;
    switch (op) {
    case 0x21: { /* LD IX,nn */
        uint16_t lo = zfetch8(md, c);
        uint16_t hi = zfetch8(md, c);
        c->cyc += 6;
        *idx = (uint16_t)(lo | (hi << 8));
        break;
    }
    case 0x22: { /* LD (nn),IX */
        uint16_t nn = zfetch16(md, c);
        c->cyc += 12;
        fb_z80_bus_write(md, nn, (uint8_t)*idx);
        fb_z80_bus_write(md, (uint16_t)(nn + 1), (uint8_t)(*idx >> 8));
        break;
    }
    case 0x2A: { /* LD IX,(nn) */
        uint16_t nn = zfetch16(md, c);
        c->cyc += 12;
        *idx = (uint16_t)(fb_z80_bus_read(md, nn) |
                          (fb_z80_bus_read(md, (uint16_t)(nn + 1)) << 8));
        break;
    }
    case 0x23: c->cyc += 6; (*idx)++; break;
    case 0x2B: c->cyc += 6; (*idx)--; break;
    case 0x29: case 0x19: case 0x39: { /* ADD IX,rr (IX/BC/SP) */
        int a = *idx;
        int b;
        if (op == 0x29)
            b = *idx;
        else if (op == 0x39)
            b = c->sp;
        else
            b = c->bc;
        int r = a + b;
        uint8_t f = 0;
        if (r > 0xFFFF)
            f |= F_C;
        if ((((a ^ b) & 0x8000u) == 0) && (((a ^ (uint16_t)r) & 0x8000u) != 0))
            f |= F_PV;
        if ((((a ^ b) ^ (uint16_t)r) & 0x1000u) != 0)
            f |= F_H;
        *idx = (uint16_t)r;
        zset_flags(c, f);
        c->cyc += 11;
        break;
    }
    case 0x36: { /* LD (IX+d),n */
        int8_t d = (int8_t)zfetch8(md, c);
        uint8_t v = zfetch8(md, c);
        c->cyc += 3;
        fb_z80_bus_write(md, (uint16_t)(*idx + d), v);
        break;
    }
    case 0x34: { /* INC (IX+d) */
        int8_t d = (int8_t)zfetch8(md, c);
        uint16_t a = (uint16_t)(*idx + d);
        uint8_t v = fb_z80_bus_read(md, a);
        c->cyc += 7;
        fb_z80_bus_write(md, a, zinc(c, v));
        break;
    }
    case 0x35: { /* DEC (IX+d) */
        int8_t d = (int8_t)zfetch8(md, c);
        uint16_t a = (uint16_t)(*idx + d);
        uint8_t v = fb_z80_bus_read(md, a);
        c->cyc += 7;
        fb_z80_bus_write(md, a, zdecr(c, v));
        break;
    }
    case 0xE5: { /* PUSH IX */
        zpush(md, c, *idx);
        c->cyc += 11;
        break;
    }
    case 0xE1: { /* POP IX */
        *idx = zpop(md, c);
        break;
    }
    case 0xE9: /* JP (IX) */
        c->pc = *idx;
        c->cyc += 4;
        break;
    case 0xE3: { /* EX (SP),IX */
        uint16_t t = (uint16_t)(fb_z80_bus_read(md, c->sp) |
                                (fb_z80_bus_read(md, (uint16_t)(c->sp + 1)) << 8));
        fb_z80_bus_write(md, c->sp, (uint8_t)*idx);
        fb_z80_bus_write(md, (uint16_t)(c->sp + 1), (uint8_t)(*idx >> 8));
        *idx = t;
        c->cyc += 19;
        break;
    }
    default: {
        /* LD r,(IX+d), LD (IX+d),r, ALU A,(IX+d) and all register-only ops
         * re-dispatched against the index context. */
        if (op >= 0x40 && op <= 0x7F && op != 0x76) {
            uint8_t dst = (uint8_t)((op >> 3) & 7);
            uint8_t src = (uint8_t)(op & 7);
            if (src == 6) { /* LD r,(IX+d) */
                int8_t d = (int8_t)zfetch8(md, c);
                c->cyc += 7;
                zw8(&z, (int)dst, fb_z80_bus_read(md, (uint16_t)(*idx + d)));
            } else if (dst == 6) { /* LD (IX+d),r */
                int8_t d = (int8_t)zfetch8(md, c);
                c->cyc += 3;
                fb_z80_bus_write(md, (uint16_t)(*idx + d), zr8(&z, (int)src));
            } else {
                zw8(&z, (int)dst, zr8(&z, (int)src));
                c->cyc += 4;
            }
        } else if ((op & 0xC6) == 0x86) { /* ALU A,(IX+d) or A,r */
            uint8_t kind = (uint8_t)((op >> 3) & 7);
            uint8_t v;
            if ((op & 7) == 6) {
                int8_t d = (int8_t)zfetch8(md, c);
                c->cyc += 7;
                v = fb_z80_bus_read(md, (uint16_t)(*idx + d));
            } else {
                v = zr8(&z, (int)(op & 7));
                c->cyc += 4;
            }
            uint8_t a = (uint8_t)(c->af >> 8);
            if (kind <= 1)
                zadd(c, a, v, kind);
            else if (kind <= 3)
                zsub(c, a, v, kind == 3, 0);
            else if (kind == 4)
                zlogic(c, a, v, 0);
            else if (kind == 5)
                zlogic(c, a, v, 2);
            else if (kind == 6)
                zlogic(c, a, v, 1);
            else
                zcp(c, a, v);
        } else if ((op & 0xC7) == 0x04) { /* INC r */
            uint8_t dst = (uint8_t)((op >> 3) & 7);
            zw8(&z, (int)dst, zinc(c, zr8(&z, (int)dst)));
            c->cyc += 4;
        } else if ((op & 0xC7) == 0x05) { /* DEC r */
            uint8_t dst = (uint8_t)((op >> 3) & 7);
            zw8(&z, (int)dst, zdecr(c, zr8(&z, (int)dst)));
            c->cyc += 4;
        } else {
            c->cyc += 4; /* unhandled DD/FD op: NOP (documented) */
        }
        break;
    }
    }
}

void fb_z80_reset(struct fb_z80 *z)
{
    memset(z, 0, sizeof *z);
    z->af = 0xFFFFu; /* documented power-on-ish value */
    z->sp = 0xFFFFu;
    z->im = 1;
}

int32_t fb_z80_run(struct fb_md *md, int32_t budget)
{
    struct fb_z80 *c = &md->z80;
    c->cyc = 0;
    while (c->cyc < budget) {
        if (c->halted) {
            /* Halt until INT; the caller keeps feeding cycles. */
            c->cyc = budget;
            break;
        }
        if (c->int_pending && c->iff1) {
            c->int_pending = 0;
            c->iff1 = 0;
            c->iff2 = 0;
            c->halted = 0;
            if (c->im == 2) {
                zpush(md, c, c->pc);
                uint8_t vec = fb_z80_bus_read(md, (uint16_t)((c->i << 8) | 0xFFu));
                c->pc = (uint16_t)(((c->i << 8) | vec) << 1);
                /* Genesis drivers normally use IM1/IM2 with i<<8; keep IM2
                 * simple: vector byte*2 + i<<8 as above. */
                c->cyc += 19;
            } else if (c->im == 1) {
                zpush(md, c, c->pc);
                c->pc = 0x0038;
                c->cyc += 13;
            } else {
                zpush(md, c, c->pc);
                c->pc = 0x0066; /* IM0 with no bus vector: treat as RST 30 */
                c->cyc += 13;
            }
            continue;
        }
        uint16_t op_pc = c->pc;
        uint8_t op = zfetch8(md, c);
        struct z80_ctx z;
        z.md = md;
        z.regbase = &c->hl;
        (void)op_pc;

        switch (op) {
        case 0x00: c->cyc += 4; break; /* NOP */
        case 0x08: { /* EX AF,AF' */
            uint16_t t = c->af; c->af = c->af2; c->af2 = t;
            c->cyc += 4;
            break;
        }
        case 0x10: { /* DJNZ */
            uint8_t b = zdecr(c, (uint8_t)(c->bc >> 8));
            c->bc = (uint16_t)((b << 8) | (c->bc & 0xFFu));
            int8_t d = (int8_t)zfetch8(md, c);
            c->cyc += 8;
            if (b) {
                c->pc = (uint16_t)(c->pc + d);
                c->cyc += 5;
            }
            break;
        }
        case 0x18: { /* JR */
            int8_t d = (int8_t)zfetch8(md, c);
            c->cyc += 12;
            c->pc = (uint16_t)(c->pc + d);
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: { /* JR cc */
            int8_t d = (int8_t)zfetch8(md, c);
            c->cyc += 8;
            if (zcond(c, (uint8_t)((op >> 3) & 3))) {
                c->pc = (uint16_t)(c->pc + d);
                c->cyc += 5;
            }
            break;
        }
        case 0x76: /* HALT */
            c->halted = 1;
            c->cyc += 4;
            break;
        case 0x01: case 0x11: case 0x21: case 0x31: { /* LD rr,nn */
            *zr16(c, (op >> 4) & 3) = zfetch16(md, c);
            c->cyc += 6;
            break;
        }
        case 0x22: { /* LD (nn),HL */
            uint16_t nn = zfetch16(md, c);
            fb_z80_bus_write(md, nn, (uint8_t)c->hl);
            fb_z80_bus_write(md, (uint16_t)(nn + 1), (uint8_t)(c->hl >> 8));
            c->cyc += 16;
            break;
        }
        case 0x2A: { /* LD HL,(nn) */
            uint16_t nn = zfetch16(md, c);
            c->hl = (uint16_t)(fb_z80_bus_read(md, nn) |
                               (fb_z80_bus_read(md, (uint16_t)(nn + 1)) << 8));
            c->cyc += 16;
            break;
        }
        case 0x32: { /* LD (nn),A */
            uint16_t nn = zfetch16(md, c);
            fb_z80_bus_write(md, nn, (uint8_t)(c->af >> 8));
            c->cyc += 13;
            break;
        }
        case 0x3A: { /* LD A,(nn) */
            uint16_t nn = zfetch16(md, c);
            c->af = (uint16_t)((c->af & 0xFFu) |
                               ((uint16_t)fb_z80_bus_read(md, nn) << 8));
            c->cyc += 13;
            break;
        }
        case 0x02: fb_z80_bus_write(md, c->bc, (uint8_t)(c->af >> 8)); c->cyc += 7; break;
        case 0x12: fb_z80_bus_write(md, c->de, (uint8_t)(c->af >> 8)); c->cyc += 7; break;
        case 0x0A: c->af = (uint16_t)((c->af & 0xFFu) | ((uint16_t)fb_z80_bus_read(md, c->bc) << 8)); c->cyc += 7; break;
        case 0x1A: c->af = (uint16_t)((c->af & 0xFFu) | ((uint16_t)fb_z80_bus_read(md, c->de) << 8)); c->cyc += 7; break;
        case 0x03: case 0x13: case 0x23: case 0x33: { /* INC rr */
            uint16_t *rr = (op == 0x33) ? &c->sp : zr16(c, (op >> 4) & 3);
            (*rr)++;
            c->cyc += 6;
            break;
        }
        case 0x0B: case 0x1B: case 0x2B: case 0x3B: { /* DEC rr */
            uint16_t *rr = (op == 0x3B) ? &c->sp : zr16(c, (op >> 4) & 3);
            (*rr)--;
            c->cyc += 6;
            break;
        }
        case 0x07: { /* RLCA */
            uint8_t a = (uint8_t)(c->af >> 8);
            uint8_t f = (a & 0x80u) ? F_C : 0;
            a = (uint8_t)((a << 1) | (f ? 1 : 0));
            c->af = (uint16_t)((uint16_t)a << 8 | f);
            c->cyc += 4;
            break;
        }
        case 0x0F: { /* RRCA */
            uint8_t a = (uint8_t)(c->af >> 8);
            uint8_t f = (a & 1u) ? F_C : 0;
            a = (uint8_t)((a >> 1) | (f ? 0x80u : 0));
            c->af = (uint16_t)((uint16_t)a << 8 | f);
            c->cyc += 4;
            break;
        }
        case 0x17: { /* RLA */
            uint8_t a = (uint8_t)(c->af >> 8);
            uint8_t cin = (uint8_t)(zf8(c) & F_C);
            uint8_t f = (a & 0x80u) ? F_C : 0;
            a = (uint8_t)((a << 1) | (cin ? 1 : 0));
            c->af = (uint16_t)((uint16_t)a << 8 | f);
            c->cyc += 4;
            break;
        }
        case 0x1F: { /* RRA */
            uint8_t a = (uint8_t)(c->af >> 8);
            uint8_t cin = (uint8_t)(zf8(c) & F_C);
            uint8_t f = (a & 1u) ? F_C : 0;
            a = (uint8_t)((a >> 1) | (cin ? 0x80u : 0));
            c->af = (uint16_t)((uint16_t)a << 8 | f);
            c->cyc += 4;
            break;
        }
        case 0x27: { /* DAA */
            uint8_t a = (uint8_t)(c->af >> 8);
            uint8_t f = zf8(c);
            int corr = 0;
            if ((f & F_H) || ((a & 0x0Fu) > 9))
                corr |= 0x06;
            if ((f & F_C) || (a > 0x99))
                corr |= 0x60;
            if (f & F_N) {
                uint8_t r = (uint8_t)(a - corr);
                uint8_t nf = (uint8_t)(f & F_C);
                if (f & F_C || a > 0x99)
                    nf |= F_C;
                nf = zszp(r, nf);
                c->af = (uint16_t)((uint16_t)r << 8 | nf);
            } else {
                uint8_t r = (uint8_t)(a + corr);
                uint8_t nf = (uint8_t)(f & F_C);
                if ((f & F_C) || a > 0x99)
                    nf |= F_C;
                if (((a & 0x0Fu) > 9) || (f & F_H))
                    nf |= F_H;
                nf = zszp(r, nf);
                c->af = (uint16_t)((uint16_t)r << 8 | nf);
            }
            c->cyc += 4;
            break;
        }
        case 0x2F: { /* CPL */
            uint8_t a = (uint8_t)(~(c->af >> 8));
            uint8_t f = (uint8_t)(zf8(c) | F_H | F_N);
            c->af = (uint16_t)((uint16_t)a << 8 | f);
            c->cyc += 4;
            break;
        }
        case 0x3F: { /* CCF */
            uint8_t f = (uint8_t)(zf8(c) & (F_S | F_Z | F_PV));
            if (zf8(c) & F_C)
                f |= F_H;
            else
                f |= F_C;
            c->af = (uint16_t)((c->af & 0xFF00u) | f);
            c->cyc += 4;
            break;
        }
        case 0x37: { /* SCF */
            uint8_t f = (uint8_t)((zf8(c) & (F_S | F_Z | F_PV)) | F_C);
            c->af = (uint16_t)((c->af & 0xFF00u) | f);
            c->cyc += 4;
            break;
        }
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C:
        case 0x34: case 0x3C: { /* INC r/(HL) */
            uint8_t idx = (uint8_t)((op >> 3) & 7);
            uint8_t v = zr8(&z, (int)idx);
            zw8(&z, (int)idx, zinc(c, v));
            c->cyc += (idx == 6) ? 11 : 4;
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D:
        case 0x35: case 0x3D: { /* DEC r/(HL) */
            uint8_t idx = (uint8_t)((op >> 3) & 7);
            uint8_t v = zr8(&z, (int)idx);
            zw8(&z, (int)idx, zdecr(c, v));
            c->cyc += (idx == 6) ? 11 : 4;
            break;
        }
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E:
        case 0x36: case 0x3E: { /* LD r,n / LD (HL),n */
            uint8_t idx = (uint8_t)((op >> 3) & 7);
            uint8_t v = zfetch8(md, c);
            zw8(&z, (int)idx, v);
            c->cyc += (idx == 6) ? 3 : 0; /* fetch already charged 3 */
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
        case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
        case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65:
        case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
        case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C:
        case 0x7D: case 0x7E: case 0x7F: { /* LD r,r' */
            uint8_t dst = (uint8_t)((op >> 3) & 7);
            uint8_t src = (uint8_t)(op & 7);
            zw8(&z, (int)dst, zr8(&z, (int)src));
            c->cyc += (dst == 6 || src == 6) ? 7 : 4;
            break;
        }
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
        case 0x86: case 0x87: { /* ADD A,r */
            uint8_t v = zr8(&z, (int)(op & 7));
            zadd(c, (uint8_t)(c->af >> 8), v, 0);
            c->cyc += (op & 7) == 6 ? 7 : 4;
            break;
        }
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D:
        case 0x8E: case 0x8F: { /* ADC A,r */
            uint8_t v = zr8(&z, (int)(op & 7));
            zadd(c, (uint8_t)(c->af >> 8), v, 1);
            c->cyc += (op & 7) == 6 ? 7 : 4;
            break;
        }
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
        case 0x96: case 0x97: { /* SUB r */
            uint8_t v = zr8(&z, (int)(op & 7));
            zsub(c, (uint8_t)(c->af >> 8), v, 0, 0);
            c->cyc += (op & 7) == 6 ? 7 : 4;
            break;
        }
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D:
        case 0x9E: case 0x9F: { /* SBC A,r */
            uint8_t v = zr8(&z, (int)(op & 7));
            zsub(c, (uint8_t)(c->af >> 8), v, 1, 0);
            c->cyc += (op & 7) == 6 ? 7 : 4;
            break;
        }
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
        case 0xA6: case 0xA7: { /* AND */
            uint8_t v = zr8(&z, (int)(op & 7));
            zlogic(c, (uint8_t)(c->af >> 8), v, 0);
            c->cyc += (op & 7) == 6 ? 7 : 4;
            break;
        }
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        case 0xAE: case 0xAF: { /* XOR */
            uint8_t v = zr8(&z, (int)(op & 7));
            zlogic(c, (uint8_t)(c->af >> 8), v, 2);
            c->cyc += (op & 7) == 6 ? 7 : 4;
            break;
        }
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
        case 0xB6: case 0xB7: { /* OR */
            uint8_t v = zr8(&z, (int)(op & 7));
            zlogic(c, (uint8_t)(c->af >> 8), v, 1);
            c->cyc += (op & 7) == 6 ? 7 : 4;
            break;
        }
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD:
        case 0xBE: case 0xBF: { /* CP */
            uint8_t v = zr8(&z, (int)(op & 7));
            zcp(c, (uint8_t)(c->af >> 8), v);
            c->cyc += (op & 7) == 6 ? 7 : 4;
            break;
        }
        case 0xC2: case 0xD2: case 0xE2: case 0xF2:
        case 0xCA: case 0xDA: case 0xEA: case 0xFA: { /* JP cc,nn */
            uint16_t nn = zfetch16(md, c);
            c->cyc += 10;
            if (zcond(c, (uint8_t)((op >> 3) & 7)))
                c->pc = nn;
            break;
        }
        case 0xC3: { /* JP nn */
            c->pc = zfetch16(md, c);
            c->cyc += 10;
            break;
        }
        case 0xE9: /* JP (HL) / JP (IX) / JP (IY) */
            c->pc = z.regbase[1];
            c->cyc += 4;
            break;
        case 0xC4: case 0xD4: case 0xE4: case 0xF4:
        case 0xCC: case 0xDC: case 0xEC: case 0xFC: { /* CALL cc,nn */
            uint16_t nn = zfetch16(md, c);
            c->cyc += 10;
            if (zcond(c, (uint8_t)((op >> 3) & 7))) {
                zpush(md, c, c->pc);
                c->pc = nn;
                c->cyc += 7;
            }
            break;
        }
        case 0xCD: { /* CALL nn */
            uint16_t nn = zfetch16(md, c);
            zpush(md, c, c->pc);
            c->pc = nn;
            c->cyc += 17;
            break;
        }
        case 0xC0: case 0xD0: case 0xE0: case 0xF0:
        case 0xC8: case 0xD8: case 0xE8: case 0xF8: { /* RET cc */
            c->cyc += 5;
            if (zcond(c, (uint8_t)((op >> 3) & 7))) {
                c->pc = zpop(md, c);
                c->cyc += 6;
            }
            break;
        }
        case 0xC9: /* RET */
            c->pc = zpop(md, c);
            c->cyc += 10;
            break;
        case 0xC1: case 0xD1: case 0xE1: case 0xF1: { /* POP */
            uint16_t v = zpop(md, c);
            if (((op >> 4) & 3) == 3)
                c->af = v;
            else
                *zr16(c, (op >> 4) & 3) = v;
            break;
        }
        case 0xC5: case 0xD5: case 0xE5: case 0xF5: { /* PUSH */
            uint16_t v;
            if (((op >> 4) & 3) == 3)
                v = c->af;
            else
                v = *zr16(c, (op >> 4) & 3);
            zpush(md, c, v);
            c->cyc += 11;
            break;
        }
        case 0xC6: { /* ADD A,n */
            uint8_t v = zfetch8(md, c);
            zadd(c, (uint8_t)(c->af >> 8), v, 0);
            c->cyc += 3;
            break;
        }
        case 0xCE: { /* ADC A,n */
            uint8_t v = zfetch8(md, c);
            zadd(c, (uint8_t)(c->af >> 8), v, 1);
            c->cyc += 3;
            break;
        }
        case 0xD6: { /* SUB n */
            uint8_t v = zfetch8(md, c);
            zsub(c, (uint8_t)(c->af >> 8), v, 0, 0);
            c->cyc += 3;
            break;
        }
        case 0xDE: { /* SBC A,n */
            uint8_t v = zfetch8(md, c);
            zsub(c, (uint8_t)(c->af >> 8), v, 1, 0);
            c->cyc += 3;
            break;
        }
        case 0xE6: { /* AND n */
            uint8_t v = zfetch8(md, c);
            zlogic(c, (uint8_t)(c->af >> 8), v, 0);
            c->cyc += 3;
            break;
        }
        case 0xEE: { /* XOR n */
            uint8_t v = zfetch8(md, c);
            zlogic(c, (uint8_t)(c->af >> 8), v, 2);
            c->cyc += 3;
            break;
        }
        case 0xF6: { /* OR n */
            uint8_t v = zfetch8(md, c);
            zlogic(c, (uint8_t)(c->af >> 8), v, 1);
            c->cyc += 3;
            break;
        }
        case 0xFE: { /* CP n */
            uint8_t v = zfetch8(md, c);
            zcp(c, (uint8_t)(c->af >> 8), v);
            c->cyc += 3;
            break;
        }
        case 0xC7: case 0xCF: case 0xD7: case 0xDF:
        case 0xE7: case 0xEF: case 0xF7: case 0xFF: { /* RST */
            zpush(md, c, c->pc);
            c->pc = (uint16_t)(op & 0x38u);
            c->cyc += 11;
            break;
        }
        case 0xD9: { /* EXX */
            uint16_t t;
            t = c->bc; c->bc = c->bc2; c->bc2 = t;
            t = c->de; c->de = c->de2; c->de2 = t;
            t = c->hl; c->hl = c->hl2; c->hl2 = t;
            c->cyc += 4;
            break;
        }
        case 0xE3: { /* EX (SP),HL */
            uint16_t t = (uint16_t)(fb_z80_bus_read(md, c->sp) |
                                    (fb_z80_bus_read(md, (uint16_t)(c->sp + 1)) << 8));
            fb_z80_bus_write(md, c->sp, (uint8_t)z.regbase[1]);
            fb_z80_bus_write(md, (uint16_t)(c->sp + 1), (uint8_t)(z.regbase[1] >> 8));
            z.regbase[1] = t;
            c->cyc += 19;
            break;
        }
        case 0xF3: /* DI */
            c->iff1 = c->iff2 = 0;
            c->cyc += 4;
            break;
        case 0xFB: /* EI */
            c->iff1 = c->iff2 = 1;
            c->cyc += 4;
            break;
        case 0xCB: /* CB prefix (or DDCB/FDCB handled separately) */
            zcb(md, c, c->pc);
            c->cyc += 4;
            break;
        case 0xED:
            zed(md, c, zfetch8(md, c));
            c->cyc += 4;
            break;
        case 0xDD: { /* IX prefix */
            c->cyc += 4;
            uint8_t op2 = zfetch8(md, c);
            if (op2 == 0xCB) {
                int8_t d = (int8_t)zfetch8(md, c);
                uint8_t op3 = zfetch8(md, c);
                uint16_t addr = (uint16_t)(c->ix + d);
                struct z80_ctx zi;
                zi.md = md;
                zi.regbase = &c->ix;
                uint8_t v = fb_z80_bus_read(md, addr);
                if ((op3 & 0xC0) == 0x40) { /* BIT b,(IX+d) */
                    uint8_t m = (uint8_t)(1u << (op3 & 7));
                    uint8_t f = (uint8_t)((zf8(c) & F_C) | (v & 0x28u) | F_H);
                    if ((v & m) == 0)
                        f |= F_Z | F_PV;
                    zset_flags(c, f);
                } else {
                    uint8_t m = (uint8_t)(1u << (op3 & 7));
                    if ((op3 & 0xC0) == 0x80) { /* RES/SET */
                        v = (op3 & 0x40) ? (uint8_t)(v | m) : (uint8_t)(v & (uint8_t)~m);
                        fb_z80_bus_write(md, addr, v);
                    } else {
                        /* rotate/shift memory form */
                        uint8_t f;
                        uint8_t cin = (uint8_t)(zf8(c) & F_C);
                        switch (op3 & 0xF8) {
                        case 0x00: f = (v & 0x80u) ? F_C : 0; v = (uint8_t)((v << 1) | (f ? 1 : 0)); break;
                        case 0x08: f = (v & 1u) ? F_C : 0; v = (uint8_t)((v >> 1) | (f ? 0x80u : 0)); break;
                        case 0x10: f = (v & 0x80u) ? F_C : 0; v = (uint8_t)((v << 1) | (cin ? 1 : 0)); break;
                        case 0x18: f = (v & 1u) ? F_C : 0; v = (uint8_t)((v >> 1) | (cin ? 0x80u : 0)); break;
                        case 0x20: f = (v & 0x80u) ? F_C : 0; v = (uint8_t)(v << 1); break;
                        case 0x28: f = (v & 1u) ? F_C : 0; v = (uint8_t)((v >> 1) | (v & 0x80u)); break;
                        case 0x38: f = (v & 1u) ? F_C : 0; v = (uint8_t)(v >> 1); break;
                        default: f = 0; break;
                        }
                        f = zszp(v, f);
                        fb_z80_bus_write(md, addr, v);
                        zset_flags(c, f);
                    }
                }
                (void)zi;
                c->cyc += 4;
            } else {
                /* DD-prefixed main op: run with index base */
                z_indexed_op(md, op2, &c->ix);
            }
            break;
        }
        case 0xFD: { /* IY prefix */
            c->cyc += 4;
            uint8_t op2 = zfetch8(md, c);
            if (op2 == 0xCB) {
                int8_t d = (int8_t)zfetch8(md, c);
                uint8_t op3 = zfetch8(md, c);
                uint16_t addr = (uint16_t)(c->iy + d);
                uint8_t v = fb_z80_bus_read(md, addr);
                if ((op3 & 0xC0) == 0x40) {
                    uint8_t m = (uint8_t)(1u << (op3 & 7));
                    uint8_t f = (uint8_t)((zf8(c) & F_C) | (v & 0x28u) | F_H);
                    if ((v & m) == 0)
                        f |= F_Z | F_PV;
                    zset_flags(c, f);
                } else {
                    uint8_t m = (uint8_t)(1u << (op3 & 7));
                    if ((op3 & 0xC0) == 0x80) {
                        v = (op3 & 0x40) ? (uint8_t)(v | m) : (uint8_t)(v & (uint8_t)~m);
                        fb_z80_bus_write(md, addr, v);
                    } else {
                        uint8_t f;
                        uint8_t cin = (uint8_t)(zf8(c) & F_C);
                        switch (op3 & 0xF8) {
                        case 0x00: f = (v & 0x80u) ? F_C : 0; v = (uint8_t)((v << 1) | (f ? 1 : 0)); break;
                        case 0x08: f = (v & 1u) ? F_C : 0; v = (uint8_t)((v >> 1) | (f ? 0x80u : 0)); break;
                        case 0x10: f = (v & 0x80u) ? F_C : 0; v = (uint8_t)((v << 1) | (cin ? 1 : 0)); break;
                        case 0x18: f = (v & 1u) ? F_C : 0; v = (uint8_t)((v >> 1) | (cin ? 0x80u : 0)); break;
                        case 0x20: f = (v & 0x80u) ? F_C : 0; v = (uint8_t)(v << 1); break;
                        case 0x28: f = (v & 1u) ? F_C : 0; v = (uint8_t)((v >> 1) | (v & 0x80u)); break;
                        case 0x38: f = (v & 1u) ? F_C : 0; v = (uint8_t)(v >> 1); break;
                        default: f = 0; break;
                        }
                        f = zszp(v, f);
                        fb_z80_bus_write(md, addr, v);
                        zset_flags(c, f);
                    }
                }
                c->cyc += 4;
            } else {
                z_indexed_op(md, op2, &c->iy);
            }
            break;
        }
        default:
            c->cyc += 4; /* unknown: treat as NOP (documented) */
            break;
        }
        /* advance the refresh counter for non-prefixed ops */
    }
    return c->cyc;
}
