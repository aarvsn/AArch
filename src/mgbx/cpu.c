/*
 * mgbx CPU: Sharp SM83 (LR35902), full official instruction set.
 *
 * Flags live in F: Z=0x80 N=0x40 H=0x20 C=0x10 (low nibble always 0).
 * Cycle counts are T-cycles (4 per M-cycle). Interrupt dispatch (20 T-cycles)
 * happens at instruction boundaries when IME is set.
 *
 * Hardware behaviors implemented: SM83 half-carry rules, DAA, ADD SP,e8 flag
 * quirks (H/C from unsigned low-byte arithmetic), CB prefix, RLCA/RLA group
 * leaving Z unchanged, HALT with IME=0 halt bug, STOP, EI delay semantics.
 *
 * Unofficial base opcodes execute as 4-cycle NOPs (documented limitation).
 */
#include "mgbx.h"

#define FZ 0x80u
#define FN 0x40u
#define FH 0x20u
#define FC 0x10u

static inline uint16_t bc(const gb_cpu *c) { return (uint16_t)((c->b << 8) | c->c); }
static inline uint16_t de(const gb_cpu *c) { return (uint16_t)((c->d << 8) | c->e); }
static inline uint16_t hl(const gb_cpu *c) { return (uint16_t)((c->h << 8) | c->l); }
static inline uint16_t af(const gb_cpu *c) { return (uint16_t)((c->a << 8) | (c->f & 0xF0u)); }
static inline void set_bc(gb_cpu *c, uint16_t v) { c->b = (uint8_t)(v >> 8); c->c = (uint8_t)v; }
static inline void set_de(gb_cpu *c, uint16_t v) { c->d = (uint8_t)(v >> 8); c->e = (uint8_t)v; }
static inline void set_hl(gb_cpu *c, uint16_t v) { c->h = (uint8_t)(v >> 8); c->l = (uint8_t)v; }

static inline uint8_t rd(struct mgbx *gb, uint16_t a) { return gb_bus_read(gb, a); }
static inline void wr(struct mgbx *gb, uint16_t a, uint8_t v) { gb_bus_write(gb, a, v); }

static inline void push16(struct mgbx *gb, gb_cpu *c, uint16_t v)
{
    c->sp = (uint16_t)(c->sp - 1);
    wr(gb, c->sp, (uint8_t)(v >> 8));
    c->sp = (uint16_t)(c->sp - 1);
    wr(gb, c->sp, (uint8_t)v);
}

static inline uint16_t pop16(struct mgbx *gb, gb_cpu *c)
{
    uint16_t lo = rd(gb, c->sp);
    c->sp = (uint16_t)(c->sp + 1);
    uint16_t hi = rd(gb, c->sp);
    c->sp = (uint16_t)(c->sp + 1);
    return (uint16_t)((hi << 8) | lo);
}

/* returns 1 if an interrupt was dispatched (20 T-cycles must be added) */
static int dispatch_interrupt(struct mgbx *gb)
{
    gb_cpu *c = &gb->cpu;
    uint8_t pending = (uint8_t)(gb->mem.if_reg & gb->mem.ie & 0x1Fu);
    if (pending == 0)
        return 0;
    for (uint8_t bit = 0; bit < 5; bit++) {
        if (pending & (1u << bit)) {
            gb->mem.if_reg &= (uint8_t)~(1u << bit);
            push16(gb, c, c->pc);
            c->pc = (uint16_t)(0x0040u + bit * 8u);
            c->ime = 0;
            c->ime_pending = 0;
            c->halted = 0;
            c->stopped = 0;
            return 1;
        }
    }
    return 0;
}

/* ---- 8-bit arithmetic helpers ------------------------------------------- */

static uint8_t add8(gb_cpu *c, uint8_t a, uint8_t b, uint8_t carry)
{
    uint32_t r = (uint32_t)a + b + carry;
    uint8_t res = (uint8_t)r;
    c->f = 0;
    if (res == 0)
        c->f |= FZ;
    if ((int)((a & 0xFu) + (b & 0xFu) + carry) > 0xF)
        c->f |= FH;
    if (r > 0xFFu)
        c->f |= FC;
    return res;
}

static uint8_t sub8(gb_cpu *c, uint8_t a, uint8_t b, uint8_t borrow, int keep)
{
    int r = (int)a - (int)b - (int)borrow;
    uint8_t res = (uint8_t)r;
    c->f = FN;
    if (res == 0)
        c->f |= FZ;
    if ((int)(a & 0xFu) - (int)(b & 0xFu) - (int)borrow < 0)
        c->f |= FH;
    if (r < 0)
        c->f |= FC;
    if (keep)
        c->a = res;
    return res;
}

static uint8_t and8(gb_cpu *c, uint8_t a, uint8_t b)
{
    uint8_t r = (uint8_t)(a & b);
    c->f = (uint8_t)(FH | (r == 0 ? FZ : 0));
    return r;
}

static uint8_t or8(gb_cpu *c, uint8_t a, uint8_t b)
{
    uint8_t r = (uint8_t)(a | b);
    c->f = (uint8_t)(r == 0 ? FZ : 0);
    return r;
}

static uint8_t xor8(gb_cpu *c, uint8_t a, uint8_t b)
{
    uint8_t r = (uint8_t)(a ^ b);
    c->f = (uint8_t)(r == 0 ? FZ : 0);
    return r;
}

static uint8_t inc8(gb_cpu *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    c->f = (uint8_t)((c->f & FC) | (r == 0 ? FZ : 0) |
                     ((int)(v & 0xFu) + 1 > 0xF ? FH : 0));
    return r;
}

static uint8_t dec8(gb_cpu *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    c->f = (uint8_t)((c->f & FC) | FN | (r == 0 ? FZ : 0) |
                     ((int)(v & 0xFu) - 1 < 0 ? FH : 0));
    return r;
}

static void add_hl(gb_cpu *c, uint16_t v)
{
    uint16_t hlv = hl(c);
    uint32_t r = (uint32_t)hlv + v;
    c->f = (uint8_t)((c->f & FZ) |
                     ((int)((hlv & 0xFFFu) + (v & 0xFFFu)) > 0xFFF ? FH : 0) |
                     (r > 0xFFFFu ? FC : 0));
    set_hl(c, (uint16_t)r);
}

/* ADD SP,e8 / LD HL,SP+e8: flags from unsigned low-byte arithmetic. */
static uint16_t add_sp_e8(gb_cpu *c, uint8_t e, int write_hl)
{
    uint16_t sp = c->sp;
    uint16_t result = (uint16_t)(sp + (uint16_t)(uint8_t)e -
                                 ((e & 0x80u) ? 0x100u : 0u));
    uint8_t h = (uint8_t)(((sp & 0xFu) + (e & 0xFu)) > 0xFu);
    uint8_t ca = (uint8_t)(((sp & 0xFFu) + e) > 0xFFu);
    c->f = (uint8_t)((h ? FH : 0) | (ca ? FC : 0));
    if (write_hl)
        set_hl(c, result);
    else
        c->sp = result;
    return result;
}

static void daa(gb_cpu *c)
{
    uint8_t a = c->a;
    uint8_t adjust = 0;
    if (!(c->f & FN)) {
        if ((c->f & FH) || (a & 0x0Fu) > 9u)
            adjust |= 0x06u;
        if ((c->f & FC) || a > 0x99u) {
            adjust |= 0x60u;
            c->f |= FC;
        }
    } else {
        if (c->f & FH)
            adjust |= 0x06u;
        if (c->f & FC)
            adjust |= 0x60u;
    }
    a = (uint8_t)(a + adjust);
    c->f &= (uint8_t)~FH;
    if (a == 0)
        c->f |= FZ;
    else
        c->f &= (uint8_t)~FZ;
    c->a = a;
}

/* ---- CB prefix operations ------------------------------------------------ */

static uint8_t cb_shift(gb_cpu *c, uint8_t op, uint8_t v)
{
    uint8_t r = v;
    uint8_t cout = 0;
    switch (op) {
    case 0: /* RLC */
        cout = (uint8_t)(v >> 7);
        r = (uint8_t)((v << 1) | cout);
        break;
    case 1: /* RRC */
        cout = (uint8_t)(v & 1u);
        r = (uint8_t)((v >> 1) | (cout << 7));
        break;
    case 2: /* RL */
        cout = (uint8_t)(v >> 7);
        r = (uint8_t)((v << 1) | ((c->f & FC) ? 1u : 0u));
        break;
    case 3: /* RR */
        cout = (uint8_t)(v & 1u);
        r = (uint8_t)((v >> 1) | ((c->f & FC) ? 0x80u : 0u));
        break;
    case 4: /* SLA */
        cout = (uint8_t)(v >> 7);
        r = (uint8_t)(v << 1);
        break;
    case 5: /* SRA */
        cout = (uint8_t)(v & 1u);
        r = (uint8_t)((v >> 1) | (v & 0x80u));
        break;
    default: /* SRL (op 7) */
        cout = (uint8_t)(v & 1u);
        r = (uint8_t)(v >> 1);
        break;
    }
    c->f = (uint8_t)((r == 0 ? FZ : 0) | (cout ? FC : 0));
    return r;
}

/* returns extra T-cycles for (HL) forms: 4 for BIT, 8 for writes */
static uint32_t cb_run(struct mgbx *gb, gb_cpu *c, uint8_t op)
{
    uint8_t x = (uint8_t)(op >> 6);
    uint8_t y = (uint8_t)((op >> 3) & 7u);
    uint8_t z = (uint8_t)(op & 7u);

    if (z == 6) { /* (HL) */
        uint16_t a = hl(c);
        uint8_t v = rd(gb, a);
        switch (x) {
        case 0:
            if (y == 6) { /* SWAP */
                uint8_t r = (uint8_t)((v << 4) | (v >> 4));
                c->f = (uint8_t)(r == 0 ? FZ : 0);
                wr(gb, a, r);
            } else {
                wr(gb, a, cb_shift(c, y, v));
            }
            return 8;
        case 1: /* BIT */
            c->f = (uint8_t)((c->f & FC) | FH | (((v >> y) & 1u) ? 0 : FZ));
            return 4;
        case 2: /* RES */
            wr(gb, a, (uint8_t)(v & (uint8_t)~(1u << y)));
            return 8;
        default: /* SET */
            wr(gb, a, (uint8_t)(v | (1u << y)));
            return 8;
        }
    }

    uint8_t *regs[8] = { &c->b, &c->c, &c->d, &c->e, &c->h, &c->l, NULL, &c->a };
    uint8_t *p = regs[z];
    uint8_t v = *p;
    switch (x) {
    case 0:
        if (y == 6) { /* SWAP */
            uint8_t r = (uint8_t)((v << 4) | (v >> 4));
            c->f = (uint8_t)(r == 0 ? FZ : 0);
            *p = r;
        } else {
            *p = cb_shift(c, y, v);
        }
        break;
    case 1: /* BIT */
        c->f = (uint8_t)((c->f & FC) | FH | (((v >> y) & 1u) ? 0 : FZ));
        break;
    case 2: /* RES */
        *p = (uint8_t)(v & (uint8_t)~(1u << y));
        break;
    default: /* SET */
        *p = (uint8_t)(v | (1u << y));
        break;
    }
    return 0;
}

static inline int cond(gb_cpu *c, uint8_t idx)
{
    switch (idx) {
    case 0: return !(c->f & FZ);      /* NZ */
    case 1: return (c->f & FZ) != 0;  /* Z  */
    case 2: return !(c->f & FC);      /* NC */
    default: return (c->f & FC) != 0; /* C  */
    }
}

/* register slot -> current value ((HL) reads the bus) */
static inline uint8_t regval(struct mgbx *gb, gb_cpu *c, uint8_t idx)
{
    switch (idx) {
    case 0: return c->b;
    case 1: return c->c;
    case 2: return c->d;
    case 3: return c->e;
    case 4: return c->h;
    case 5: return c->l;
    case 6: return rd(gb, hl(c));
    default: return c->a;
    }
}

static inline void setreg(struct mgbx *gb, gb_cpu *c, uint8_t idx, uint8_t v)
{
    switch (idx) {
    case 0: c->b = v; break;
    case 1: c->c = v; break;
    case 2: c->d = v; break;
    case 3: c->e = v; break;
    case 4: c->h = v; break;
    case 5: c->l = v; break;
    case 6: wr(gb, hl(c), v); break;
    default: c->a = v; break;
    }
}

/* extra T-cycles when the operand is (HL): 4 (one more M-cycle) */
static inline uint32_t hlpen(uint8_t idx) { return idx == 6 ? 4u : 0u; }

/* executes one opcode; returns T-cycles */
static uint32_t exec(struct mgbx *gb, gb_cpu *c, uint8_t op)
{
    uint16_t tmp;
    switch (op) {
    /* ---- 8-bit loads LD r,r' (0x40-0x7F) ---- */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
    case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C:
    case 0x4D: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x57: case 0x58: case 0x59: case 0x5A:
    case 0x5B: case 0x5C: case 0x5D: case 0x5F: case 0x60: case 0x61:
    case 0x62: case 0x63: case 0x64: case 0x65: case 0x67: case 0x68:
    case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6F:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D:
    case 0x7F:
    {
        uint8_t dst = (uint8_t)((op >> 3) & 7u);
        uint8_t src = (uint8_t)(op & 7u);
        setreg(gb, c, dst, regval(gb, c, src));
        return 4 + hlpen(src);
    }
    case 0x46: case 0x4E: case 0x56: case 0x5E:
    case 0x66: case 0x6E: case 0x76: /* 0x76 handled below; excluded here */
        if (op != 0x76) { /* LD r,(HL) */
            setreg(gb, c, (uint8_t)((op >> 3) & 7u), rd(gb, hl(c)));
            return 8;
        }
        /* fall through: HALT */
        c->halted = (c->ime != 0 || (gb->mem.if_reg & gb->mem.ie & 0x1Fu) == 0);
        if (!c->halted)
            c->halt_bug = 1;
        return 4;
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x77: /* LD (HL),r */
        wr(gb, hl(c), regval(gb, c, (uint8_t)(op & 7u)));
        return 8;
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: /* LD r,d8 */
        setreg(gb, c, (uint8_t)((op >> 3) & 7u), rd(gb, c->pc++));
        return 8;
    case 0x36: /* LD (HL),d8 */
        wr(gb, hl(c), rd(gb, c->pc++));
        return 12;
    case 0x0A: c->a = rd(gb, bc(c)); return 8;
    case 0x1A: c->a = rd(gb, de(c)); return 8;
    case 0xFA: /* LD A,(a16) */
        tmp = rd(gb, c->pc++);
        tmp |= (uint16_t)(rd(gb, c->pc++) << 8);
        c->a = rd(gb, tmp);
        return 16;
    case 0xEA: /* LD (a16),A */
        tmp = rd(gb, c->pc++);
        tmp |= (uint16_t)(rd(gb, c->pc++) << 8);
        wr(gb, tmp, c->a);
        return 16;
    case 0x02: wr(gb, bc(c), c->a); return 8;
    case 0x12: wr(gb, de(c), c->a); return 8;
    case 0x22: wr(gb, hl(c), c->a); set_hl(c, (uint16_t)(hl(c) + 1)); return 8;
    case 0x32: wr(gb, hl(c), c->a); set_hl(c, (uint16_t)(hl(c) - 1)); return 8;
    case 0x2A: /* LDI A,(HL) */
        c->a = rd(gb, hl(c));
        set_hl(c, (uint16_t)(hl(c) + 1));
        return 8;
    case 0x3A: /* LDD A,(HL) */
        c->a = rd(gb, hl(c));
        set_hl(c, (uint16_t)(hl(c) - 1));
        return 8;
    case 0xE2: wr(gb, (uint16_t)(0xFF00u | c->c), c->a); return 8;
    case 0xF2: c->a = rd(gb, (uint16_t)(0xFF00u | c->c)); return 8;
    case 0xE0: wr(gb, (uint16_t)(0xFF00u | rd(gb, c->pc++)), c->a); return 12;
    case 0xF0: c->a = rd(gb, (uint16_t)(0xFF00u | rd(gb, c->pc++))); return 12;
    case 0x3E: c->a = rd(gb, c->pc++); return 8;

    /* ---- 16-bit loads ---- */
    case 0x01: c->c = rd(gb, c->pc++); c->b = rd(gb, c->pc++); return 12;
    case 0x11: c->e = rd(gb, c->pc++); c->d = rd(gb, c->pc++); return 12;
    case 0x21: c->l = rd(gb, c->pc++); c->h = rd(gb, c->pc++); return 12;
    case 0x31:
        c->sp = rd(gb, c->pc++);
        c->sp = (uint16_t)(c->sp | ((uint16_t)rd(gb, c->pc++) << 8));
        return 12;
    case 0xF9: c->sp = hl(c); return 8;
    case 0xF8: add_sp_e8(c, rd(gb, c->pc++), 1); return 12; /* LD HL,SP+e8 */
    case 0x08: /* LD (a16),SP */
    {
        uint16_t a = rd(gb, c->pc++);
        a |= (uint16_t)(rd(gb, c->pc++) << 8);
        wr(gb, a, (uint8_t)(c->sp & 0xFFu));
        wr(gb, (uint16_t)(a + 1), (uint8_t)(c->sp >> 8));
        return 20;
    }
    case 0xC5: push16(gb, c, bc(c)); return 16;
    case 0xD5: push16(gb, c, de(c)); return 16;
    case 0xE5: push16(gb, c, hl(c)); return 16;
    case 0xF5: push16(gb, c, af(c)); return 16;
    case 0xC1: set_bc(c, pop16(gb, c)); return 12;
    case 0xD1: set_de(c, pop16(gb, c)); return 12;
    case 0xE1: set_hl(c, pop16(gb, c)); return 12;
    case 0xF1: {
        uint16_t v = pop16(gb, c);
        c->a = (uint8_t)(v >> 8);
        c->f = (uint8_t)(v & 0xF0u);
        return 12;
    }

    /* ---- 8-bit ALU (0x80-0xBF) ---- */
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87: /* ADD */
        c->a = add8(c, c->a, regval(gb, c, (uint8_t)(op & 7u)), 0);
        return 4 + hlpen((uint8_t)(op & 7u));
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: /* ADC */
        c->a = add8(c, c->a, regval(gb, c, (uint8_t)(op & 7u)),
                    (uint8_t)((c->f & FC) ? 1 : 0));
        return 4 + hlpen((uint8_t)(op & 7u));
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97: /* SUB */
        sub8(c, c->a, regval(gb, c, (uint8_t)(op & 7u)), 0, 1);
        return 4 + hlpen((uint8_t)(op & 7u));
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: /* SBC */
        sub8(c, c->a, regval(gb, c, (uint8_t)(op & 7u)),
             (uint8_t)((c->f & FC) ? 1 : 0), 1);
        return 4 + hlpen((uint8_t)(op & 7u));
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: /* AND */
        c->a = and8(c, c->a, regval(gb, c, (uint8_t)(op & 7u)));
        return 4 + hlpen((uint8_t)(op & 7u));
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: /* XOR */
        c->a = xor8(c, c->a, regval(gb, c, (uint8_t)(op & 7u)));
        return 4 + hlpen((uint8_t)(op & 7u));
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: /* OR */
        c->a = or8(c, c->a, regval(gb, c, (uint8_t)(op & 7u)));
        return 4 + hlpen((uint8_t)(op & 7u));
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: /* CP */
        sub8(c, c->a, regval(gb, c, (uint8_t)(op & 7u)), 0, 0);
        return 4 + hlpen((uint8_t)(op & 7u));
    case 0xC6: c->a = add8(c, c->a, rd(gb, c->pc++), 0); return 8;
    case 0xCE: c->a = add8(c, c->a, rd(gb, c->pc++), (uint8_t)((c->f & FC) ? 1 : 0)); return 8;
    case 0xD6: sub8(c, c->a, rd(gb, c->pc++), 0, 1); return 8;
    case 0xDE: sub8(c, c->a, rd(gb, c->pc++), (uint8_t)((c->f & FC) ? 1 : 0), 1); return 8;
    case 0xE6: c->a = and8(c, c->a, rd(gb, c->pc++)); return 8;
    case 0xF6: c->a = or8(c, c->a, rd(gb, c->pc++)); return 8;
    case 0xEE: c->a = xor8(c, c->a, rd(gb, c->pc++)); return 8;
    case 0xFE: sub8(c, c->a, rd(gb, c->pc++), 0, 0); return 8;
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x3C: /* INC r */
    {
        uint8_t r = (uint8_t)((op >> 3) & 7u);
        setreg(gb, c, r, inc8(c, regval(gb, c, r)));
        return 4 + hlpen(r);
    }
    case 0x34: /* INC (HL) */
        wr(gb, hl(c), inc8(c, rd(gb, hl(c))));
        return 12;
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x3D: /* DEC r */
    {
        uint8_t r = (uint8_t)((op >> 3) & 7u);
        setreg(gb, c, r, dec8(c, regval(gb, c, r)));
        return 4 + hlpen(r);
    }
    case 0x35: /* DEC (HL) */
        wr(gb, hl(c), dec8(c, rd(gb, hl(c))));
        return 12;
    case 0x27: daa(c); return 4;
    case 0x2F: /* CPL */
        c->a = (uint8_t)~c->a;
        c->f |= (uint8_t)(FN | FH);
        return 4;
    case 0x3F: /* CCF */
        c->f = (uint8_t)((c->f & FZ) | ((c->f & FC) ? 0u : FC));
        return 4;
    case 0x37: /* SCF */
        c->f = (uint8_t)((c->f & FZ) | FC);
        return 4;
    case 0x07: /* RLCA (SM83: Z unchanged) */
    {
        uint8_t cout = (uint8_t)(c->a >> 7);
        c->a = (uint8_t)((c->a << 1) | cout);
        c->f = (uint8_t)((c->f & FZ) | (cout ? FC : 0));
        return 4;
    }
    case 0x0F: /* RRCA */
    {
        uint8_t cout = (uint8_t)(c->a & 1u);
        c->a = (uint8_t)((c->a >> 1) | (cout << 7));
        c->f = (uint8_t)((c->f & FZ) | (cout ? FC : 0));
        return 4;
    }
    case 0x17: /* RLA */
    {
        uint8_t cout = (uint8_t)(c->a >> 7);
        c->a = (uint8_t)((c->a << 1) | ((c->f & FC) ? 1u : 0u));
        c->f = (uint8_t)((c->f & FZ) | (cout ? FC : 0));
        return 4;
    }
    case 0x1F: /* RRA */
    {
        uint8_t cout = (uint8_t)(c->a & 1u);
        c->a = (uint8_t)((c->a >> 1) | ((c->f & FC) ? 0x80u : 0u));
        c->f = (uint8_t)((c->f & FZ) | (cout ? FC : 0));
        return 4;
    }

    /* ---- 16-bit arithmetic ---- */
    case 0x09: add_hl(c, bc(c)); return 8;
    case 0x19: add_hl(c, de(c)); return 8;
    case 0x29: add_hl(c, hl(c)); return 8;
    case 0x39: add_hl(c, c->sp); return 8;
    case 0x03: set_bc(c, (uint16_t)(bc(c) + 1)); return 8;
    case 0x13: set_de(c, (uint16_t)(de(c) + 1)); return 8;
    case 0x23: set_hl(c, (uint16_t)(hl(c) + 1)); return 8;
    case 0x33: c->sp = (uint16_t)(c->sp + 1); return 8;
    case 0x0B: set_bc(c, (uint16_t)(bc(c) - 1)); return 8;
    case 0x1B: set_de(c, (uint16_t)(de(c) - 1)); return 8;
    case 0x2B: set_hl(c, (uint16_t)(hl(c) - 1)); return 8;
    case 0x3B: c->sp = (uint16_t)(c->sp - 1); return 8;

    /* ---- jumps / calls / returns ---- */
    case 0x18: { /* JR e8 */
        int8_t e = (int8_t)rd(gb, c->pc++);
        c->pc = (uint16_t)(c->pc + (uint16_t)(int16_t)e);
        return 12;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: { /* JR cc */
        int8_t e = (int8_t)rd(gb, c->pc++);
        if (cond(c, (uint8_t)((op >> 3) & 3u))) {
            c->pc = (uint16_t)(c->pc + (uint16_t)(int16_t)e);
            return 12;
        }
        return 8;
    }
    case 0xC3:
        tmp = rd(gb, c->pc++);
        tmp |= (uint16_t)(rd(gb, c->pc++) << 8);
        c->pc = tmp;
        return 16;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: { /* JP cc */
        tmp = rd(gb, c->pc++);
        tmp |= (uint16_t)(rd(gb, c->pc++) << 8);
        if (cond(c, (uint8_t)((op >> 3) & 3u))) {
            c->pc = tmp;
            return 16;
        }
        return 12;
    }
    case 0xE9: c->pc = hl(c); return 4;
    case 0xCD:
        tmp = rd(gb, c->pc++);
        tmp |= (uint16_t)(rd(gb, c->pc++) << 8);
        push16(gb, c, c->pc);
        c->pc = tmp;
        return 24;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: { /* CALL cc */
        tmp = rd(gb, c->pc++);
        tmp |= (uint16_t)(rd(gb, c->pc++) << 8);
        if (cond(c, (uint8_t)((op >> 3) & 3u))) {
            push16(gb, c, c->pc);
            c->pc = tmp;
            return 24;
        }
        return 12;
    }
    case 0xC9: c->pc = pop16(gb, c); return 16;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: /* RET cc */
        if (cond(c, (uint8_t)((op >> 3) & 3u))) {
            c->pc = pop16(gb, c);
            return 20;
        }
        return 8;
    case 0xD9: /* RETI */
        c->pc = pop16(gb, c);
        c->ime = 1;
        return 16;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF: /* RST */
        push16(gb, c, c->pc);
        c->pc = (uint16_t)((op & 0x38u));
        return 16;

    /* ---- interrupts / misc ---- */
    case 0xFB: /* EI */
        c->ime_pending = 1;
        return 4;
    case 0xF3: /* DI */
        c->ime = 0;
        c->ime_pending = 0;
        return 4;
    case 0x10: /* STOP (byte after is consumed) */
        c->stopped = 1;
        c->pc = (uint16_t)(c->pc + 1);
        return 4;
    case 0xCB: {
        uint8_t cbop = rd(gb, c->pc++);
        return 8 + cb_run(gb, c, cbop);
    }
    default: /* NOP and unimplemented/unofficial opcodes */
        return 4;
    }
}

uint32_t gb_cpu_step(struct mgbx *gb)
{
    gb_cpu *c = &gb->cpu;

    /* OAM DMA: one M-cycle per call, one byte per M-cycle */
    if (gb->mem.dma_active) {
        gb_ppu_oam_write(&gb->ppu, gb->mem.dma_index, gb->mem.dma_value);
        gb->mem.dma_index++;
        if (gb->mem.dma_index >= 160) {
            gb->mem.dma_active = 0;
        } else {
            gb->mem.dma_value = gb_bus_read(
                gb, (uint16_t)(gb->mem.oam_dma_page * 0x100u + gb->mem.dma_index));
        }
        return 4;
    }

    if (c->stopped) {
        if ((gb->mem.if_reg & gb->mem.ie & 0x1Fu) != 0) {
            c->stopped = 0;
            if (c->ime && dispatch_interrupt(gb))
                return 20;
        }
        return 4;
    }

    if (c->halted) {
        if ((gb->mem.if_reg & gb->mem.ie & 0x1Fu) != 0) {
            c->halted = 0;
            if (c->ime && dispatch_interrupt(gb))
                return 20;
        }
        return 4;
    }

    /* EI delay: IME effective for the instruction about to execute */
    if (c->ime_pending) {
        c->ime_pending = 0;
        c->ime = 1;
    }

    uint8_t op = rd(gb, c->pc);
    if (c->halt_bug) {
        c->halt_bug = 0; /* byte after HALT fetched twice: PC stays */
    } else {
        c->pc = (uint16_t)(c->pc + 1);
    }
    uint32_t t = exec(gb, c, op);
    gb->total_cycles += t;

    if (c->ime && !c->halted && !c->stopped) {
        if (dispatch_interrupt(gb))
            t += 20;
    }
    return t;
}

void gb_cpu_reset_for_no_bootrom(gb_cpu *c)
{
    /* documented DMG post-boot register state (no boot ROM is bundled) */
    c->a = 0x01; c->f = 0xB0;
    c->b = 0x00; c->c = 0x13;
    c->d = 0x00; c->e = 0xD8;
    c->h = 0x01; c->l = 0x4D;
    c->sp = 0xFFFE;
    c->pc = 0x0100;
    c->ime = 0;
    c->ime_pending = 0;
    c->halted = 0;
    c->stopped = 0;
    c->halt_bug = 0;
}
