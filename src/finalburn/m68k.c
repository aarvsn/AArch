/*
 * finalburn: Motorola 68000 CPU core (subset).
 *
 * Implemented: full official MOVE families, immediate ALU group, bit ops,
 * MOVEP, quick forms, Bcc/DBcc/Scc, shifts/rotates (reg+mem), MUL/DIV,
 * LINK/UNLK, MOVEM, TAS, EXG/SWAP/EXT, PEA/LEA, stack ops, TRAP/CHK/TRAPV,
 * MOVE SR/CCR/USP, STOP/RTE/RTR/RESET, ABCD/SBCD/NBCD, exceptions
 * (illegal, line-A/F, div-by-zero, CHK, TRAPV, privilege, trace, TRAP,
 * interrupts IPL1-7, STOP wake), supervisor/user SP switching.
 *
 * Approximations (documented in README):
 *   - cycle counts are coarse (base + per-access penalty, MUL/DIV formulas)
 *   - unaligned word/long accesses are silently aligned instead of raising
 *     an address error
 *   - RESET is a no-op, bus error never raised
 */
#include "fb_md.h"

#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

#define OP_SIZE_B 1
#define OP_SIZE_W 2
#define OP_SIZE_L 4

typedef struct { uint8_t mode, reg; } ea_t;

static uint16_t fetch16(struct fb_md *md, struct fb_m68k *c)
{
    uint16_t v = fb_md_68k_read16(md, c->pc);
    c->pc += 2;
    c->cycles += 4;
    return v;
}

static uint32_t fetch32(struct fb_md *md, struct fb_m68k *c)
{
    uint32_t hi = fetch16(md, c);
    uint32_t lo = fetch16(md, c);
    return (hi << 16) | lo;
}

static void push16(struct fb_md *md, struct fb_m68k *c, uint16_t v)
{
    c->a[7] -= 2;
    fb_md_68k_write16(md, c->a[7], v);
    c->cycles += 4;
}

static void push32(struct fb_md *md, struct fb_m68k *c, uint32_t v)
{
    c->a[7] -= 4;
    fb_md_68k_write32(md, c->a[7], v);
    c->cycles += 8;
}

static uint16_t pop16(struct fb_md *md, struct fb_m68k *c)
{
    uint16_t v = fb_md_68k_read16(md, c->a[7]);
    c->a[7] += 2;
    c->cycles += 4;
    return v;
}

static uint32_t pop32(struct fb_md *md, struct fb_m68k *c)
{
    uint32_t v = fb_md_68k_read32(md, c->a[7]);
    c->a[7] += 4;
    c->cycles += 8;
    return v;
}

/* Switch S bit and exchange the active stack pointer. */
static void write_sr(struct fb_m68k *c, uint16_t v)
{
    int was_s = c->sr & FB_SR_S;
    int is_s = v & FB_SR_S;
    uint32_t sp = c->a[7];
    if (was_s && !is_s) {
        c->ssp = sp;
        c->a[7] = c->usp;
    } else if (!was_s && is_s) {
        c->usp = sp;
        c->a[7] = c->ssp;
    }
    c->sr = v & 0xA71Fu;
}

/*
 * Take an exception. ret_pc is the value pushed as the return PC (for
 * boundary exceptions: the next instruction; for mid-instruction faults the
 * caller passes ppc+2).
 */
static void take_exception(struct fb_md *md, uint32_t vector, uint32_t ret_pc,
                           uint8_t irq_level)
{
    struct fb_m68k *c = &md->m68k;
    uint16_t sr = c->sr;
    if (!(sr & FB_SR_S)) {
        c->usp = c->a[7];
        c->a[7] = c->ssp;
        sr |= FB_SR_S;
    }
    sr &= (uint16_t)~FB_SR_T;
    if (irq_level)
        sr = (uint16_t)((sr & ~FB_SR_I) | ((uint16_t)irq_level << 8));
    c->sr = sr;
    push32(md, c, ret_pc);
    push16(md, c, sr);
    c->pc = fb_md_68k_read32(md, vector * 4u);
    c->cycles += 10; /* exception processing approximation */
}

static void take_interrupt(struct fb_md *md, uint8_t level)
{
    /* Vector numbers 25..31 for IPL 1..7. */
    take_exception(md, 24u + level, md->m68k.pc, level);
}

/* ---- flags -------------------------------------------------------------- */

static void flags_nzvc_logic(struct fb_m68k *c, uint32_t res, uint32_t sign)
{
    uint16_t sr = c->sr & (uint16_t)~(FB_SR_N | FB_SR_Z | FB_SR_V | FB_SR_C);
    if (res == 0)
        sr |= FB_SR_Z;
    if (res & sign)
        sr |= FB_SR_N;
    c->sr = sr;
}

static void flags_add(struct fb_m68k *c, uint32_t a, uint32_t b, uint32_t r,
                      uint32_t sign)
{
    uint16_t sr = c->sr & (uint16_t)~(FB_SR_N | FB_SR_Z | FB_SR_V | FB_SR_C);
    uint32_t carry = (a & b) | (a & ~r) | (b & ~r);
    uint32_t over = (a & b & ~r) | (~a & ~b & r);
    if (carry & sign)
        sr |= FB_SR_C;
    if (over & sign)
        sr |= FB_SR_V;
    if (r == 0)
        sr |= FB_SR_Z;
    if (r & sign)
        sr |= FB_SR_N;
    if (carry & sign)
        sr |= FB_SR_X;
    c->sr = sr;
}

static void flags_sub(struct fb_m68k *c, uint32_t a, uint32_t b, uint32_t r,
                      uint32_t sign)
{
    uint16_t sr = c->sr & (uint16_t)~(FB_SR_N | FB_SR_Z | FB_SR_V | FB_SR_C);
    uint32_t carry = (~a & b) | (b & r) | (~a & r);
    uint32_t over = (a & ~b & ~r) | (~a & b & r);
    if (carry & sign)
        sr |= FB_SR_C;
    if (over & sign)
        sr |= FB_SR_V;
    if (r == 0)
        sr |= FB_SR_Z;
    if (r & sign)
        sr |= FB_SR_N;
    if (carry & sign)
        sr |= FB_SR_X;
    c->sr = sr;
}

/* CMP: like sub but X is not modified. */
static void flags_cmp(struct fb_m68k *c, uint32_t a, uint32_t b, uint32_t r,
                      uint32_t sign)
{
    flags_sub(c, a, b, r, sign);
    c->sr &= (uint16_t)~FB_SR_X;
}

static uint32_t sign_of(int size)
{
    switch (size) {
    case OP_SIZE_B: return 0x80u;
    case OP_SIZE_W: return 0x8000u;
    default:        return 0x80000000u;
    }
}

static uint32_t mask_of(int size)
{
    switch (size) {
    case OP_SIZE_B: return 0xFFu;
    case OP_SIZE_W: return 0xFFFFu;
    default:        return 0xFFFFFFFFu;
    }
}

/* ---- effective addresses ------------------------------------------------- */

static uint32_t ea_addr(struct fb_md *md, struct fb_m68k *c, ea_t e, int size,
                        int *cost)
{
    switch (e.mode) {
    case 2:
        return c->a[e.reg];
    case 3: {
        uint32_t a = c->a[e.reg];
        int d = (e.reg == 7 && size == OP_SIZE_B) ? 2 : size;
        c->a[e.reg] += (uint32_t)d;
        *cost += 4;
        return a;
    }
    case 4: {
        int d = (e.reg == 7 && size == OP_SIZE_B) ? 2 : size;
        c->a[e.reg] -= (uint32_t)d;
        *cost += 4;
        return c->a[e.reg];
    }
    case 5: {
        int16_t disp = (int16_t)fetch16(md, c);
        *cost += 4;
        return c->a[e.reg] + (uint32_t)(int32_t)disp;
    }
    case 6: {
        uint16_t ext = fetch16(md, c);
        int8_t disp = (int8_t)(ext & 0xFF);
        uint8_t idx = (uint8_t)((ext >> 12) & 7);
        uint32_t idxval;
        *cost += 6;
        if (ext & 0x8000)
            idxval = c->a[idx];
        else
            idxval = (uint32_t)(int32_t)(int16_t)(c->d[idx] & 0xFFFFu);
        return c->a[e.reg] + (uint32_t)(int32_t)disp + idxval;
    }
    case 7:
        switch (e.reg) {
        case 0: {
            uint16_t ext = fetch16(md, c);
            *cost += 4;
            return (uint32_t)(int32_t)(int16_t)ext; /* absolute word */
        }
        case 1: {
            uint32_t a = fetch32(md, c);
            *cost += 8;
            return a;
        }
        case 2: {
            uint32_t base = c->pc;
            uint16_t ext = fetch16(md, c);
            *cost += 4;
            return base + (uint32_t)(int32_t)(int16_t)ext;
        }
        case 3: {
            uint32_t base = c->pc;
            uint16_t ext = fetch16(md, c);
            int8_t disp = (int8_t)(ext & 0xFF);
            uint8_t idx = (uint8_t)((ext >> 12) & 7);
            uint32_t idxval;
            *cost += 6;
            if (ext & 0x8000)
                idxval = c->a[idx];
            else
                idxval = (uint32_t)(int32_t)(int16_t)(c->d[idx] & 0xFFFFu);
            return base + (uint32_t)(int32_t)disp + idxval;
        }
        default:
            return 0; /* illegal; callers avoid */
        }
    default:
        return 0; /* Dn/An handled by callers */
    }
}

/* Read an operand through an EA. An (mode 1) must be handled by callers. */
static uint32_t ea_read(struct fb_md *md, struct fb_m68k *c, ea_t e, int size,
                        int *cost)
{
    if (e.mode == 0) {
        uint32_t v = c->d[e.reg];
        if (size == OP_SIZE_B)
            return v & 0xFFu;
        if (size == OP_SIZE_W)
            return v & 0xFFFFu;
        return v;
    }
    if (e.mode == 7 && e.reg == 4) { /* immediate */
        if (size == OP_SIZE_B)
            return fetch16(md, c) & 0xFFu;
        if (size == OP_SIZE_W)
            return fetch16(md, c);
        return fetch32(md, c);
    }
    uint32_t a = ea_addr(md, c, e, size, cost);
    if (size == OP_SIZE_B) {
        *cost += 4;
        return fb_md_68k_read8(md, a);
    }
    if (size == OP_SIZE_W) {
        *cost += 4;
        return fb_md_68k_read16(md, a);
    }
    *cost += 8;
    return fb_md_68k_read32(md, a);
}

/* Write an operand through an EA (memory modes + Dn). */
static void ea_write(struct fb_md *md, struct fb_m68k *c, ea_t e, int size,
                     uint32_t v, int *cost)
{
    if (e.mode == 0) {
        if (size == OP_SIZE_B)
            c->d[e.reg] = (c->d[e.reg] & 0xFFFFFF00u) | (v & 0xFFu);
        else if (size == OP_SIZE_W)
            c->d[e.reg] = (c->d[e.reg] & 0xFFFF0000u) | (v & 0xFFFFu);
        else
            c->d[e.reg] = v;
        return;
    }
    uint32_t a = ea_addr(md, c, e, size, cost);
    if (size == OP_SIZE_B) {
        *cost += 4;
        fb_md_68k_write8(md, a, (uint8_t)(v & 0xFFu));
    } else if (size == OP_SIZE_W) {
        *cost += 4;
        fb_md_68k_write16(md, a, (uint16_t)(v & 0xFFFFu));
    } else {
        *cost += 8;
        fb_md_68k_write32(md, a, v);
    }
}

/* ---- conditions ---------------------------------------------------------- */

static int cond(struct fb_m68k *c, uint8_t cc)
{
    uint16_t sr = c->sr;
    int C = (sr & FB_SR_C) != 0, V = (sr & FB_SR_V) != 0;
    int Z = (sr & FB_SR_Z) != 0, N = (sr & FB_SR_N) != 0;
    switch (cc) {
    case 0x0: return 1;              /* T   */
    case 0x1: return 0;              /* F   */
    case 0x2: return !C && !Z;       /* HI  */
    case 0x3: return C || Z;         /* LS  */
    case 0x4: return !C;             /* CC  */
    case 0x5: return C;              /* CS  */
    case 0x6: return !Z;             /* NE  */
    case 0x7: return Z;              /* EQ  */
    case 0x8: return !V;             /* VC  */
    case 0x9: return V;              /* VS  */
    case 0xA: return !N;             /* PL  */
    case 0xB: return N;              /* MI  */
    case 0xC: return N == V;         /* GE  */
    case 0xD: return N != V;         /* LT  */
    case 0xE: return !Z && N == V;   /* GT  */
    default:  return Z || N != V;    /* LE  */
    }
}

/* ---- privilege / illegal ------------------------------------------------- */

static void privilege_violation(struct fb_md *md, struct fb_m68k *c)
{
    take_exception(md, 8, c->ppc + 2, 0);
}

static void illegal(struct fb_md *md, struct fb_m68k *c)
{
    take_exception(md, 4, c->ppc + 2, 0);
}

/* ---- ALU helpers --------------------------------------------------------- */

/* line 8/C/D group: <op>.<size> <ea>,Dn  (direction 0) */
static void alu_ea_dn(struct fb_md *md, struct fb_m68k *c, uint16_t op,
                      int size, int kind)
{
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    uint8_t reg = (uint8_t)((op >> 9) & 7);
    uint32_t sign = sign_of(size);
    int cost = 0;
    uint32_t v = ea_read(md, c, e, size, &cost);
    uint32_t d, r;
    c->cycles += 4 + cost;
    if (e.mode == 1)
        goto illegal; /* An operand not allowed in this form */
    d = c->d[reg] & mask_of(size);
    switch (kind) {
    case 0: /* OR */
        r = d | v;
        flags_nzvc_logic(c, r, sign);
        c->d[reg] = r;
        break;
    case 1: /* AND */
        r = d & v;
        flags_nzvc_logic(c, r, sign);
        c->d[reg] = r;
        break;
    case 2: /* SUB */
        r = (d - v) & mask_of(size);
        flags_sub(c, d, v, r, sign);
        c->d[reg] = r;
        break;
    case 3: /* ADD */
        r = (d + v) & mask_of(size);
        flags_add(c, d, v, r, sign);
        c->d[reg] = r;
        break;
    default: /* CMP (kind 5) */
        r = (d - v) & mask_of(size);
        flags_cmp(c, d, v, r, sign);
        break;
    }
    return;
illegal:
    illegal(md, c);
}

/* line 8/C/D group: <op>.<size> Dn,<ea>  (direction 1, memory destination) */
static void alu_dn_ea(struct fb_md *md, struct fb_m68k *c, uint16_t op,
                      int size, int kind)
{
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    uint8_t reg = (uint8_t)((op >> 9) & 7);
    uint32_t sign = sign_of(size);
    int cost = 0;
    uint32_t v, r;
    if (e.mode < 2)
        goto illegal; /* must be a memory destination */
    v = ea_read(md, c, e, size, &cost);
    c->cycles += 4 + cost;
    switch (kind) {
    case 0:
        r = v | (c->d[reg] & mask_of(size));
        flags_nzvc_logic(c, r, sign);
        break;
    case 1:
        r = v & (c->d[reg] & mask_of(size));
        flags_nzvc_logic(c, r, sign);
        break;
    case 2:
        r = (v - (c->d[reg] & mask_of(size))) & mask_of(size);
        flags_sub(c, v, c->d[reg], r, sign);
        break;
    default: /* ADD */
        r = (v + (c->d[reg] & mask_of(size))) & mask_of(size);
        flags_add(c, v, c->d[reg], r, sign);
        break;
    }
    ea_write(md, c, e, size, r, &cost);
    return;
illegal:
    illegal(md, c);
}

/* ADDA/SUBA/CMPA: word operands sign-extend into the 32-bit An. */
static void alu_a(struct fb_md *md, struct fb_m68k *c, uint16_t op, int kind)
{
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    uint8_t reg = (uint8_t)((op >> 9) & 7);
    int cost = 0;
    uint32_t v = ea_read(md, c, e, OP_SIZE_W, &cost);
    c->cycles += 4 + cost;
    v = (uint32_t)(int32_t)(int16_t)v; /* word form only on the 68000 */
    if (e.mode == 1)
        goto illegal;
    if (kind == 0) { /* CMPA */
        uint32_t r = (c->a[reg] - v) & 0xFFFFFFFFu;
        flags_cmp(c, c->a[reg], v, r, 0x80000000u);
    } else if (kind == 1) { /* SUBA */
        c->a[reg] = (c->a[reg] - v) & 0xFFFFFFFFu;
    } else { /* ADDA */
        c->a[reg] = (c->a[reg] + v) & 0xFFFFFFFFu;
    }
    return;
illegal:
    illegal(md, c);
}

/* ---- main execution loop -------------------------------------------------- */

static void exec_op(struct fb_md *md);

int32_t fb_m68k_run(struct fb_md *md, int32_t budget)
{
    struct fb_m68k *c = &md->m68k;
    c->cycles = 0;
    while (c->cycles < budget) {
        if (c->halted)
            break;
        if (c->stopped) {
            uint8_t lvl = c->int_line;
            if (lvl && (lvl == 7 || lvl > FB_SR_IPL(c->sr))) {
                c->stopped = 0;
                take_interrupt(md, lvl);
            } else {
                /* Idle for the rest of the slice (bus clock keeps running). */
                c->cycles = budget;
                break;
            }
        } else {
            /* Instruction boundary: interrupts win over trace. */
            uint8_t lvl = c->int_line;
            if (lvl && (lvl == 7 || lvl > FB_SR_IPL(c->sr))) {
                take_interrupt(md, lvl);
            } else if (c->sr & FB_SR_T) {
                take_exception(md, 9, c->pc, 0);
            } else {
                exec_op(md);
            }
        }
    }
    return c->cycles;
}

/* The real reset entry point; reads SSP/PC vectors through the machine bus. */
void fb_m68k_reset_machine(struct fb_md *md)
{
    struct fb_m68k *c = &md->m68k;
    memset(c, 0, sizeof *c);
    c->sr = 0x2700u; /* supervisor, IPL 7 */
    c->ssp = fb_md_68k_read32(md, 0x000000);
    c->usp = 0;
    c->a[7] = c->ssp;
    c->pc = fb_md_68k_read32(md, 0x000004);
    c->int_line = 0;
}

void fb_m68k_set_irq(struct fb_md *md, uint8_t level)
{
    /* The caller always passes the recomputed highest pending level. */
    md->m68k.int_line = level;
}


/* ---- instruction dispatch ------------------------------------------------ */

/* MOVE family (lines 1-3). Returns 0 when the EA combination is illegal. */
static void op_move(struct fb_md *md, struct fb_m68k *c, uint16_t op, int size)
{
    ea_t src = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    ea_t dst = { (uint8_t)((op >> 6) & 7), (uint8_t)((op >> 9) & 7) };
    int cost = 0;
    if (dst.mode == 1) {
        uint32_t v;
        if (size == OP_SIZE_B)
            goto illegal;
        v = ea_read(md, c, src, size, &cost);
        c->cycles += 4 + cost;
        if (size == OP_SIZE_W)
            v = (uint32_t)(int32_t)(int16_t)v;
        c->a[dst.reg] = v;
        return;
    }
    uint32_t v = ea_read(md, c, src, size, &cost);
    c->cycles += 4 + cost;
    flags_nzvc_logic(c, v & mask_of(size), sign_of(size));
    ea_write(md, c, dst, size, v, &cost);
    return;
illegal:
    illegal(md, c);
}

/* MOVEM reg<->memory. to_regs: 1 = M->R, 0 = R->M. */
static void op_movem(struct fb_md *md, struct fb_m68k *c, uint16_t op,
                     int to_regs)
{
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    int size = (op & 0x40) ? OP_SIZE_L : OP_SIZE_W;
    int cost = 0;
    uint16_t mask = fetch16(md, c);
    if (e.mode < 2 || (e.mode == 7 && e.reg >= 4))
        goto illegal;
    uint32_t a = ea_addr(md, c, e, size, &cost);
    c->cycles += 4 + 4 + cost;
    if (to_regs) {
        for (int i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                if (size == OP_SIZE_W) {
                    uint16_t v = fb_md_68k_read16(md, a);
                    if (i < 8)
                        c->d[i] = (uint32_t)(int32_t)(int16_t)v;
                    else
                        c->a[i - 8] = (uint32_t)(int32_t)(int16_t)v;
                } else {
                    uint32_t v = fb_md_68k_read32(md, a);
                    if (i < 8)
                        c->d[i] = v;
                    else
                        c->a[i - 8] = v;
                }
                a += (uint32_t)size;
            }
        }
    } else if (e.mode == 4) { /* predecrement: mask reversed, A7 first */
        for (int i = 15; i >= 0; i--) {
            if (mask & (1u << i)) {
                a -= (uint32_t)size;
                int r = (i < 8) ? (15 - i) : (15 - i); /* A7..D0 mapping */
                /* bit0 = A7, bit8 = D0 */
                if (i < 8) {
                    if (size == OP_SIZE_W)
                        fb_md_68k_write16(md, a, (uint16_t)c->a[7 - i]);
                    else
                        fb_md_68k_write32(md, a, c->a[7 - i]);
                } else {
                    if (size == OP_SIZE_W)
                        fb_md_68k_write16(md, a, (uint16_t)(c->d[i - 8] & 0xFFFFu));
                    else
                        fb_md_68k_write32(md, a, c->d[i - 8]);
                }
                (void)r;
            }
        }
    } else {
        for (int i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                if (i < 8) {
                    if (size == OP_SIZE_W)
                        fb_md_68k_write16(md, a, (uint16_t)(c->d[i] & 0xFFFFu));
                    else
                        fb_md_68k_write32(md, a, c->d[i]);
                } else {
                    if (size == OP_SIZE_W)
                        fb_md_68k_write16(md, a, (uint16_t)(c->a[i - 8] & 0xFFFFu));
                    else
                        fb_md_68k_write32(md, a, c->a[i - 8]);
                }
                a += (uint32_t)size;
            }
        }
    }
    return;
illegal:
    illegal(md, c);
}

/* Bit operations (BTST/BCHG/BCLR/BSET). */
static void op_bit(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    int btype = (op >> 6) & 7;  /* 4 BTST, 5 BCHG, 6 BCLR, 7 BSET */
    uint8_t breg = (uint8_t)((op >> 9) & 7);
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    uint32_t bit;
    if (btype == 4 && breg == 0) {
        uint16_t imm = fetch16(md, c);
        bit = imm;
        c->cycles += 8;
    } else {
        bit = c->d[breg];
        c->cycles += 4;
    }
    if (e.mode == 0) { /* Dn, 32-bit */
        uint32_t m;
        bit &= 31;
        m = 1u << bit;
        if (c->d[e.reg] & m)
            c->sr &= (uint16_t)~FB_SR_Z;
        else
            c->sr |= FB_SR_Z;
        if (btype == 5)
            c->d[e.reg] ^= m;
        else if (btype == 6)
            c->d[e.reg] &= ~m;
        else if (btype == 7)
            c->d[e.reg] |= m;
    } else { /* memory byte */
        int cost = 0;
        uint32_t a;
        uint8_t v, m;
        bit &= 7;
        a = ea_addr(md, c, e, OP_SIZE_B, &cost);
        v = fb_md_68k_read8(md, a);
        m = (uint8_t)(1u << bit);
        if (v & m)
            c->sr &= (uint16_t)~FB_SR_Z;
        else
            c->sr |= FB_SR_Z;
        c->cycles += cost + 4;
        if (btype == 5)
            v ^= m;
        else if (btype == 6)
            v &= (uint8_t)~m;
        else if (btype == 7)
            v |= m;
        else
            return; /* BTST: no write-back */
        fb_md_68k_write8(md, a, v);
    }
}

/* Immediate group (ORI/ANDI/SUBI/ADDI/EORI/CMPI + CCR/SR forms). */
static void op_imm(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    uint8_t sub = (uint8_t)((op >> 9) & 7);
    int size;
    ea_t e;
    int cost = 0;
    uint32_t imm, v, r;
    switch ((op >> 6) & 3) {
    case 0: size = OP_SIZE_B; break;
    case 1: size = OP_SIZE_W; break;
    case 2: size = OP_SIZE_L; break;
    default: goto illegal;
    }
    if (sub == 4 || sub == 7)
        goto illegal; /* 100/111 unassigned */
    e.mode = (uint8_t)((op >> 3) & 7);
    e.reg = (uint8_t)(op & 7);

    if (e.mode == 7 && e.reg == 4 && sub != 2 && sub != 3 && sub != 6) {
        /* CCR/SR forms (ORI/ANDI/EORI to CCR or SR). */
        uint16_t imm16;
        if (size != OP_SIZE_B && size != OP_SIZE_W)
            goto illegal;
        imm16 = fetch16(md, c);
        c->cycles += 8;
        if (size == OP_SIZE_B) {
            uint8_t ccr = (uint8_t)(c->sr & 0x1Fu);
            if (sub == 0)
                ccr |= (uint8_t)imm16;
            else if (sub == 1)
                ccr &= (uint8_t)imm16;
            else
                ccr ^= (uint8_t)imm16;
            c->sr = (uint16_t)((c->sr & 0xFF00u) | ccr);
        } else {
            if (!(c->sr & FB_SR_S))
                goto priv;
            uint16_t sr = c->sr;
            if (sub == 0)
                sr |= imm16;
            else if (sub == 1)
                sr &= imm16;
            else
                sr ^= imm16;
            write_sr(c, sr);
        }
        return;
    }

    /* An destinations allowed only for SUBI/ADDI/CMPI, word/long. */
    if (e.mode == 1) {
        if ((sub != 2 && sub != 3 && sub != 6) || size == OP_SIZE_B)
            goto illegal;
    }

    if (size == OP_SIZE_B)
        imm = fetch16(md, c) & 0xFFu;
    else if (size == OP_SIZE_W)
        imm = fetch16(md, c);
    else
        imm = fetch32(md, c);

    if (e.mode == 1) {
        uint32_t a = c->a[e.reg];
        if (size == OP_SIZE_W)
            imm = (uint32_t)(int32_t)(int16_t)imm;
        switch (sub) {
        case 2: c->a[e.reg] = (a - imm) & 0xFFFFFFFFu; break;
        case 3: c->a[e.reg] = (a + imm) & 0xFFFFFFFFu; break;
        case 6: {
            uint32_t rr = (a - imm) & 0xFFFFFFFFu;
            flags_cmp(c, a, imm, rr, 0x80000000u);
            break;
        }
        default: goto illegal;
        }
        c->cycles += 8 + cost;
        return;
    }

    v = ea_read(md, c, e, size, &cost);
    c->cycles += 4 + cost;
    switch (sub) {
    case 0: /* ORI */
        r = v | imm;
        flags_nzvc_logic(c, r, sign_of(size));
        ea_write(md, c, e, size, r, &cost);
        break;
    case 1: /* ANDI */
        r = v & imm;
        flags_nzvc_logic(c, r, sign_of(size));
        ea_write(md, c, e, size, r, &cost);
        break;
    case 2: /* SUBI */
        r = (v - imm) & mask_of(size);
        flags_sub(c, v, imm, r, sign_of(size));
        ea_write(md, c, e, size, r, &cost);
        break;
    case 3: /* ADDI */
        r = (v + imm) & mask_of(size);
        flags_add(c, v, imm, r, sign_of(size));
        ea_write(md, c, e, size, r, &cost);
        break;
    case 5: /* EORI */
        r = v ^ imm;
        flags_nzvc_logic(c, r, sign_of(size));
        ea_write(md, c, e, size, r, &cost);
        break;
    default: /* CMPI (6) */
        r = (v - imm) & mask_of(size);
        flags_cmp(c, v, imm, r, sign_of(size));
        break;
    }
    return;
illegal:
    illegal(md, c);
    return;
priv:
    privilege_violation(md, c);
}

/* MOVEP: word/long transfer between Dn and consecutive byte slots at d16(An). */
static void op_movep(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    uint8_t dreg = (uint8_t)((op >> 9) & 7);
    uint8_t areg = (uint8_t)(op & 7);
    int to_mem = (op & 0x0080) != 0;
    int is_long = (op & 0x0040) != 0;
    uint16_t disp = fetch16(md, c);
    uint32_t a = c->a[areg] + (uint32_t)(int32_t)(int16_t)disp;
    c->cycles += 16;
    if (to_mem) {
        if (is_long) {
            uint32_t v = c->d[dreg];
            fb_md_68k_write8(md, a, (uint8_t)(v >> 24));
            fb_md_68k_write8(md, a + 2, (uint8_t)(v >> 16));
            fb_md_68k_write8(md, a + 4, (uint8_t)(v >> 8));
            fb_md_68k_write8(md, a + 6, (uint8_t)v);
        } else {
            uint16_t v = (uint16_t)c->d[dreg];
            fb_md_68k_write8(md, a, (uint8_t)(v >> 8));
            fb_md_68k_write8(md, a + 2, (uint8_t)v);
        }
    } else {
        uint32_t v = 0;
        if (is_long) {
            v = ((uint32_t)fb_md_68k_read8(md, a) << 24) |
                ((uint32_t)fb_md_68k_read8(md, a + 2) << 16) |
                ((uint32_t)fb_md_68k_read8(md, a + 4) << 8) |
                (uint32_t)fb_md_68k_read8(md, a + 6);
        } else {
            v = ((uint32_t)fb_md_68k_read8(md, a) << 8) |
                (uint32_t)fb_md_68k_read8(md, a + 2);
        }
        c->d[dreg] = v;
    }
}

static void op_quick(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    /* ADDQ/SUBQ: 0101 ddd0 ss eeeeee / 0101 ddd1 ss eeeeee */
    int is_sub = (op & 0x0100) != 0;
    uint8_t data = (uint8_t)((op >> 9) & 7);
    int size;
    ea_t e;
    int cost = 0;
    uint32_t v, r;
    switch ((op >> 6) & 3) {
    case 0: size = OP_SIZE_B; break;
    case 1: size = OP_SIZE_W; break;
    case 2: size = OP_SIZE_L; break;
    default: goto scc;
    }
    if (data == 0)
        data = 8;
    e.mode = (uint8_t)((op >> 3) & 7);
    e.reg = (uint8_t)(op & 7);
    if (e.mode == 1) {
        if (size == OP_SIZE_B)
            goto illegal;
        c->cycles += 4;
        if (is_sub)
            c->a[e.reg] = (c->a[e.reg] - data) & 0xFFFFFFFFu;
        else
            c->a[e.reg] = (c->a[e.reg] + data) & 0xFFFFFFFFu;
        return;
    }
    v = ea_read(md, c, e, size, &cost);
    c->cycles += 4 + cost;
    if (is_sub) {
        r = (v - data) & mask_of(size);
        flags_sub(c, v, data, r, sign_of(size));
    } else {
        r = (v + data) & mask_of(size);
        flags_add(c, v, data, r, sign_of(size));
    }
    ea_write(md, c, e, size, r, &cost);
    return;
scc:
    /* 0101 cccc 11 eeeeee (Scc) / 0101 cccc 11001 rrr (DBcc) */
    if (is_sub) {
        uint8_t cc = (uint8_t)((op >> 8) & 0xF);
        if (((op >> 3) & 7) == 1) { /* DBcc */
            uint8_t rr = (uint8_t)(op & 7);
            uint16_t disp = fetch16(md, c);
            c->cycles += 10;
            if (cond(c, cc))
                return;
            uint16_t vv = (uint16_t)(c->d[rr] & 0xFFFFu);
            vv = (uint16_t)(vv - 1u);
            c->d[rr] = (c->d[rr] & 0xFFFF0000u) | vv;
            if (vv != 0xFFFFu)
                c->pc = c->pc + (uint32_t)(int32_t)(int16_t)disp;
        } else { /* Scc */
            ea_t e2 = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int cost2 = 0;
            uint8_t v2 = cond(c, cc) ? 0xFF : 0x00;
            c->cycles += 4;
            if (e2.mode == 0) {
                c->d[e2.reg] = (c->d[e2.reg] & 0xFFFFFF00u) | v2;
            } else {
                ea_write(md, c, e2, OP_SIZE_B, v2, &cost2);
                c->cycles += cost2;
            }
        }
    } else {
        goto illegal; /* ADDQ/SUBQ with size 11 is invalid */
    }
    return;
illegal:
    illegal(md, c);
}

/* ---- shifts / rotates (register form) ------------------------------------- */

static void op_shift_reg(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    int type = (op >> 9) & 3;   /* 0 AS, 1 LS, 2 ROX, 3 RO */
    int left = (op & 0x0100) != 0;
    int size, width;
    uint32_t sign;
    uint8_t reg = (uint8_t)(op & 7);
    uint32_t v, r;
    uint32_t cnt;
    int i;
    switch ((op >> 6) & 3) {
    case 0: size = OP_SIZE_B; break;
    case 1: size = OP_SIZE_W; break;
    case 2: size = OP_SIZE_L; break;
    default: goto illegal;
    }
    width = size * 8;
    sign = 1u << (width - 1);
    if (op & 0x0020) {
        cnt = c->d[(op >> 9) & 7] & 63u;
        c->cycles += 6 + 2 * (int32_t)cnt;
    } else {
        cnt = (op >> 9) & 7;
        if (cnt == 0)
            cnt = 8;
        c->cycles += 6 + 2 * (int32_t)cnt;
    }
    v = c->d[reg] & mask_of(size);
    r = v;
    uint16_t sr = c->sr & (uint16_t)~(FB_SR_N | FB_SR_Z | FB_SR_V | FB_SR_C);
    if (cnt == 0) {
        if (type == 2) /* ROX with count 0: C = X */
            sr |= (c->sr & FB_SR_X) ? FB_SR_C : 0;
        c->sr = sr;
        flags_nzvc_logic(c, r, sign);
        c->sr = (c->sr & (uint16_t)~FB_SR_X) | (c->sr & FB_SR_X); /* X unchanged */
        return;
    }
    for (i = 0; (uint32_t)i < cnt; i++) {
        if (type == 0) { /* AS */
            if (left) {
                uint32_t newv = (r << 1) & mask_of(size);
                if (((r & sign) != 0) != ((newv & sign) != 0))
                    sr |= FB_SR_V;
                if (r & sign)
                    sr |= FB_SR_C | FB_SR_X;
                r = newv;
            } else {
                if (r & 1)
                    sr |= FB_SR_C | FB_SR_X;
                r = (r >> 1) | (r & sign);
                r &= mask_of(size);
            }
        } else if (type == 1) { /* LS */
            if (left) {
                if (r & sign)
                    sr |= FB_SR_C | FB_SR_X;
                r = (r << 1) & mask_of(size);
            } else {
                if (r & 1)
                    sr |= FB_SR_C | FB_SR_X;
                r >>= 1;
            }
        } else if (type == 2) { /* ROX */
            uint32_t x = (c->sr & FB_SR_X) ? 1u : 0u;
            if (left) {
                uint32_t top = (r >> (width - 1)) & 1u;
                if (top)
                    sr |= FB_SR_C;
                r = ((r << 1) | x) & mask_of(size);
                if (top)
                    sr |= FB_SR_X;
                else
                    sr &= (uint16_t)~FB_SR_X;
            } else {
                uint32_t bot = r & 1u;
                if (bot)
                    sr |= FB_SR_C;
                r = (r >> 1) | (x << (width - 1));
                if (bot)
                    sr |= FB_SR_X;
                else
                    sr &= (uint16_t)~FB_SR_X;
            }
        } else { /* RO */
            if (left) {
                uint32_t top = (r >> (width - 1)) & 1u;
                if (top)
                    sr |= FB_SR_C;
                r = ((r << 1) | top) & mask_of(size);
            } else {
                uint32_t bot = r & 1u;
                if (bot)
                    sr |= FB_SR_C;
                r = (r >> 1) | (bot << (width - 1));
            }
        }
    }
    if (r == 0)
        sr |= FB_SR_Z;
    if (r & sign)
        sr |= FB_SR_N;
    c->sr = sr;
    c->d[reg] = (c->d[reg] & ~mask_of(size)) | r;
    return;
illegal:
    illegal(md, c);
}

/* Memory form: single-bit shifts on a word operand. */
static void op_shift_mem(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    int type = (op >> 9) & 3;
    int left = (op & 0x0100) != 0;
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    int cost = 0;
    if (e.mode < 2)
        goto illegal;
    uint32_t v = ea_read(md, c, e, OP_SIZE_W, &cost);
    uint16_t sr = c->sr & (uint16_t)~(FB_SR_N | FB_SR_Z | FB_SR_V | FB_SR_C);
    c->cycles += 8 + cost;
    if (type == 0) { /* AS */
        if (left) {
            if (v & 0x8000u) {
                sr |= FB_SR_C | FB_SR_X;
                if (!(v & 0x4000u))
                    sr |= FB_SR_V;
            }
            v = (v << 1) & 0xFFFFu;
        } else {
            if (v & 1)
                sr |= FB_SR_C | FB_SR_X;
            v = (v >> 1) | (v & 0x8000u);
        }
    } else if (type == 1) { /* LS */
        if (left) {
            if (v & 0x8000u)
                sr |= FB_SR_C | FB_SR_X;
            v = (v << 1) & 0xFFFFu;
        } else {
            if (v & 1)
                sr |= FB_SR_C | FB_SR_X;
            v >>= 1;
        }
    } else if (type == 2) { /* ROX */
        uint32_t x = (c->sr & FB_SR_X) ? 1u : 0u;
        if (left) {
            if (v & 0x8000u)
                sr |= FB_SR_C | FB_SR_X;
            else
                sr &= (uint16_t)~FB_SR_X;
            v = (uint16_t)(((v << 1) | x) & 0xFFFFu);
        } else {
            if (v & 1)
                sr |= FB_SR_C | FB_SR_X;
            else
                sr &= (uint16_t)~FB_SR_X;
            v = (uint16_t)((v >> 1) | (x << 15));
        }
    } else { /* RO */
        if (left) {
            if (v & 0x8000u)
                sr |= FB_SR_C;
            v = (uint16_t)(((v << 1) | ((v >> 15) & 1u)) & 0xFFFFu);
        } else {
            if (v & 1)
                sr |= FB_SR_C;
            v = (uint16_t)((v >> 1) | ((v & 1u) << 15));
        }
    }
    if (v == 0)
        sr |= FB_SR_Z;
    if (v & 0x8000u)
        sr |= FB_SR_N;
    c->sr = sr;
    ea_write(md, c, e, OP_SIZE_W, v, &cost);
    return;
illegal:
    illegal(md, c);
}

/* ---- multiply / divide / BCD ---------------------------------------------- */

static void op_divu(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    uint8_t reg = (uint8_t)((op >> 9) & 7);
    int cost = 0;
    uint32_t divisor = ea_read(md, c, e, OP_SIZE_W, &cost);
    uint32_t dividend = c->d[reg];
    c->cycles += 60 + cost;
    if (e.mode == 1)
        goto illegal;
    if (divisor == 0) {
        take_exception(md, 5, c->ppc + 2, 0);
        return;
    }
    uint32_t q = dividend / divisor;
    if (q > 0xFFFFu) {
        c->sr |= FB_SR_V;
        return;
    }
    uint32_t r = dividend % divisor;
    uint32_t res = (r << 16) | q;
    c->d[reg] = res;
    uint16_t sr = c->sr & (uint16_t)~(FB_SR_N | FB_SR_Z | FB_SR_V | FB_SR_C);
    if (res == 0)
        sr |= FB_SR_Z;
    if (q & 0x8000u)
        sr |= FB_SR_N;
    c->sr = sr;
    return;
illegal:
    illegal(md, c);
}

static void op_divs(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    uint8_t reg = (uint8_t)((op >> 9) & 7);
    int cost = 0;
    int32_t divisor = (int32_t)(int16_t)ea_read(md, c, e, OP_SIZE_W, &cost);
    int32_t dividend = (int32_t)c->d[reg];
    c->cycles += 110 + cost;
    if (e.mode == 1)
        goto illegal;
    if (divisor == 0) {
        take_exception(md, 5, c->ppc + 2, 0);
        return;
    }
    if (dividend == (int32_t)0x80000000 && divisor == -1) {
        c->sr |= FB_SR_V;
        return;
    }
    int32_t q = dividend / divisor;
    if (q > 0x7FFF || q < -0x8000) {
        c->sr |= FB_SR_V;
        return;
    }
    int32_t r = dividend % divisor;
    uint32_t res = ((uint32_t)(uint16_t)(uint16_t)r << 16) |
                   (uint32_t)(uint16_t)q;
    c->d[reg] = res;
    uint16_t sr = c->sr & (uint16_t)~(FB_SR_N | FB_SR_Z | FB_SR_V | FB_SR_C);
    if (res == 0)
        sr |= FB_SR_Z;
    if (q & 0x8000)
        sr |= FB_SR_N;
    c->sr = sr;
    return;
illegal:
    illegal(md, c);
}

/* ABCD/SBCD via the two-nibble decimal method. kind 0 = ABCD, 1 = SBCD. */
static void op_bcd(struct fb_md *md, struct fb_m68k *c, uint16_t op, int kind)
{
    uint8_t rx = (uint8_t)((op >> 9) & 7); /* Dx: destination register */
    int mem = (op & 0x0008) != 0;
    uint8_t a, b;
    if (mem) { /* ABCD -(Ay),-(Ax): result at -(Ax), Ax = rrr */
        int ry = op & 7;
        c->a[ry] -= 1;
        b = fb_md_68k_read8(md, c->a[ry]);
        c->a[rx] -= 1;
        a = fb_md_68k_read8(md, c->a[rx]);
        c->cycles += 18;
    } else {
        a = (uint8_t)(c->d[rx] & 0xFFu);   /* Dx value */
        b = (uint8_t)(c->d[op & 7] & 0xFFu); /* Dy value */
        c->cycles += 6;
    }
    int xin = (c->sr & FB_SR_X) ? 1 : 0;
    int lo, hi, res, carry;
    if (kind == 0) { /* ABCD: Dx + Dy + X */
        lo = (a & 0xF) + (b & 0xF) + xin;
        if (lo > 9)
            lo += 6;
        hi = (a >> 4) + (b >> 4) + ((lo > 0xF) ? 1 : 0);
        lo &= 0xF;
        carry = (hi > 9) ? 1 : 0;
        if (carry)
            hi -= 10;
    } else { /* SBCD: Dx - Dy - X */
        lo = (a & 0xF) - (b & 0xF) - xin;
        hi = (a >> 4) - (b >> 4);
        if (lo < 0) {
            lo += 10;
            hi--;
        }
        carry = (hi < 0) ? 1 : 0;
        if (carry)
            hi += 10;
    }
    res = ((hi & 0xF) << 4) | (lo & 0xF);
    if (res != 0)
        c->sr &= (uint16_t)~FB_SR_Z; /* Z only cleared, never set */
    if (carry)
        c->sr |= FB_SR_C | FB_SR_X;
    else
        c->sr &= (uint16_t)~(FB_SR_C | FB_SR_X);
    if (mem) {
        fb_md_68k_write8(md, c->a[rx], (uint8_t)res);
    } else {
        c->d[rx] = (c->d[rx] & 0xFFFFFF00u) | (uint32_t)res;
    }
}

static void op_nbcd(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
    int cost = 0;
    uint32_t v = ea_read(md, c, e, OP_SIZE_B, &cost);
    c->cycles += 6 + cost;
    int xin = (c->sr & FB_SR_X) ? 1 : 0;
    int a = (int)(v & 0xFFu);
    int lo = -(a & 0xF) - xin;
    int hi = -(a >> 4);
    if (lo < 0) {
        lo += 10;
        hi--;
    }
    int carry = (hi < 0) ? 1 : 0;
    if (carry)
        hi += 10;
    int res = ((hi & 0xF) << 4) | (lo & 0xF);
    if (res != 0)
        c->sr &= (uint16_t)~FB_SR_Z;
    if (carry)
        c->sr |= FB_SR_C | FB_SR_X;
    else
        c->sr &= (uint16_t)~(FB_SR_C | FB_SR_X);
    ea_write(md, c, e, OP_SIZE_B, (uint32_t)res, &cost);
}

/* ADDX register and -(An) forms: 1101 xxx1 ss00 0yyy / 1101 xxx1 ss00 1yyy */
static void op_addx_reg(struct fb_md *md, struct fb_m68k *c, uint16_t op)
{
    uint8_t rx = (uint8_t)((op >> 9) & 7);
    int mem = (op & 0x0008) != 0;
    int size;
    uint32_t a, b, r, xin;
    switch ((op >> 6) & 3) {
    case 0: size = OP_SIZE_B; break;
    case 1: size = OP_SIZE_W; break;
    case 2: size = OP_SIZE_L; break;
    default: illegal(md, c); return;
    }
    xin = (c->sr & FB_SR_X) ? 1u : 0u;
    if (mem) {
        int ry = op & 7;
        c->a[ry] -= (uint32_t)size;
        c->a[rx] -= (uint32_t)size;
        a = fb_md_68k_read8(md, c->a[ry]);
        b = fb_md_68k_read8(md, c->a[rx]);
        if (size >= OP_SIZE_W) {
            a = (a << 8) | fb_md_68k_read8(md, c->a[ry] + 1);
            b = (b << 8) | fb_md_68k_read8(md, c->a[rx] + 1);
        }
        if (size == OP_SIZE_L) {
            a = (a << 16) | ((uint32_t)fb_md_68k_read8(md, c->a[ry] + 2) << 8) |
                fb_md_68k_read8(md, c->a[ry] + 3);
            b = (b << 16) | ((uint32_t)fb_md_68k_read8(md, c->a[rx] + 2) << 8) |
                fb_md_68k_read8(md, c->a[rx] + 3);
        }
        c->cycles += 18;
        r = (a + b + xin) & mask_of(size);
        if (r != 0)
            c->sr &= (uint16_t)~FB_SR_Z;
        else
            c->sr |= FB_SR_Z;
        c->sr &= (uint16_t)~(FB_SR_N | FB_SR_V | FB_SR_C | FB_SR_X);
        {
            uint32_t carry = (a & b) | (a & ~r) | (b & ~r);
            if (carry & sign_of(size))
                c->sr |= FB_SR_C | FB_SR_X;
            {
                uint32_t over = (a & b & ~r) | (~a & ~b & r);
                if (over & sign_of(size))
                    c->sr |= FB_SR_V;
            }
        }
        if (r & sign_of(size))
            c->sr |= FB_SR_N;
        if (size == OP_SIZE_B) {
            fb_md_68k_write8(md, c->a[ry], (uint8_t)r);
        } else if (size == OP_SIZE_W) {
            fb_md_68k_write8(md, c->a[ry], (uint8_t)(r >> 8));
            fb_md_68k_write8(md, c->a[ry] + 1, (uint8_t)r);
        } else {
            fb_md_68k_write8(md, c->a[ry], (uint8_t)(r >> 24));
            fb_md_68k_write8(md, c->a[ry] + 1, (uint8_t)(r >> 16));
            fb_md_68k_write8(md, c->a[ry] + 2, (uint8_t)(r >> 8));
            fb_md_68k_write8(md, c->a[ry] + 3, (uint8_t)r);
        }
    } else {
        a = c->d[op & 7] & mask_of(size);
        b = c->d[rx] & mask_of(size);
        r = (a + b + xin) & mask_of(size);
        c->cycles += 4;
        if (r != 0)
            c->sr &= (uint16_t)~FB_SR_Z;
        else
            c->sr |= FB_SR_Z;
        c->sr &= (uint16_t)~(FB_SR_N | FB_SR_V | FB_SR_C | FB_SR_X);
        {
            uint32_t carry = (a & b) | (a & ~r) | (b & ~r);
            if (carry & sign_of(size))
                c->sr |= FB_SR_C | FB_SR_X;
            {
                uint32_t over = (a & b & ~r) | (~a & ~b & r);
                if (over & sign_of(size))
                    c->sr |= FB_SR_V;
            }
        }
        if (r & sign_of(size))
            c->sr |= FB_SR_N;
        c->d[op & 7] = (c->d[op & 7] & ~mask_of(size)) | r;
    }
}

/* ---- the main switch ------------------------------------------------------- */

static void exec_op(struct fb_md *md)
{
    struct fb_m68k *c = &md->m68k;
    c->ppc = c->pc;
    uint16_t op = fetch16(md, c);
    uint8_t line = (uint8_t)(op >> 12);

    switch (line) {
    case 0x0:
        if ((op & 0x0188) == 0x0108 && (op & 0x0030) == 0x0020)
            op_movep(md, c, op);
        else if (((op >> 6) & 7) >= 4 && ((op & 0x0180) != 0x0080))
            op_bit(md, c, op);
        else if ((op & 0x0180) == 0x0080 && (op & 0x0600) == 0)
            op_bit(md, c, op); /* static BTST: 0000 000 100 eeeeee */
        else
            op_imm(md, c, op);
        break;

    case 0x1:
        op_move(md, c, op, OP_SIZE_B);
        break;
    case 0x2:
        op_move(md, c, op, OP_SIZE_L);
        break;
    case 0x3:
        op_move(md, c, op, OP_SIZE_W);
        break;

    case 0x4: {
        if ((op & 0xFFF8) == 0x4840) { /* SWAP */
            uint8_t r = (uint8_t)(op & 7);
            uint32_t v = c->d[r];
            c->d[r] = ((v >> 16) | (v << 16)) & 0xFFFFFFFFu;
            flags_nzvc_logic(c, c->d[r], 0x80000000u);
            c->cycles += 4;
        } else if ((op & 0xFFC0) == 0x4840) { /* PEA */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int cost = 0;
            uint32_t a = ea_addr(md, c, e, OP_SIZE_L, &cost);
            c->cycles += 8 + cost;
            push32(md, c, a);
        } else if ((op & 0xFE78) == 0x4808) { /* EXT */
            uint8_t r = (uint8_t)(op & 7);
            if (op & 0x40) { /* EXT.L */
                uint32_t v = (uint32_t)(int32_t)(int16_t)(c->d[r] & 0xFFFFu);
                c->d[r] = v;
                flags_nzvc_logic(c, v, 0x80000000u);
            } else { /* EXT.W */
                uint32_t v = (uint32_t)(int32_t)(int8_t)(c->d[r] & 0xFFu);
                c->d[r] = (c->d[r] & 0xFFFF0000u) | (v & 0xFFFFu);
                flags_nzvc_logic(c, v & 0xFFFFu, 0x8000u);
            }
            c->cycles += 4;
        } else if ((op & 0xFFC0) == 0x4E80) { /* JSR */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int cost = 0;
            uint32_t a = ea_addr(md, c, e, OP_SIZE_L, &cost);
            c->cycles += 8 + cost;
            push32(md, c, c->pc);
            c->pc = a;
        } else if ((op & 0xFFC0) == 0x4EC0) { /* JMP */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int cost = 0;
            uint32_t a = ea_addr(md, c, e, OP_SIZE_L, &cost);
            c->cycles += 8 + cost;
            c->pc = a;
        } else if ((op & 0xFFF8) == 0x4E50) { /* LINK */
            uint8_t r = (uint8_t)(op & 7);
            int16_t disp = (int16_t)fetch16(md, c);
            c->cycles += 16;
            push32(md, c, c->a[r]);
            c->a[r] = c->a[7];
            c->a[7] = (c->a[7] + (uint32_t)(int32_t)disp) & 0xFFFFFFFFu;
        } else if ((op & 0xFFF8) == 0x4E58) { /* UNLK */
            uint8_t r = (uint8_t)(op & 7);
            c->cycles += 12;
            c->a[7] = c->a[r];
            c->a[r] = fb_md_68k_read32(md, c->a[7]);
            c->a[7] += 4;
        } else if ((op & 0xFFF8) == 0x4E60) { /* MOVE An,USP */
            if (!(c->sr & FB_SR_S)) {
                privilege_violation(md, c);
            } else {
                c->usp = c->a[op & 7];
                c->cycles += 4;
            }
        } else if ((op & 0xFFF8) == 0x4E68) { /* MOVE USP,An */
            if (!(c->sr & FB_SR_S)) {
                privilege_violation(md, c);
            } else {
                c->a[op & 7] = c->usp;
                c->cycles += 4;
            }
        } else if (op == 0x4E71) { /* NOP */
            c->cycles += 4;
        } else if (op == 0x4E70) { /* RESET (no-op, documented) */
            if (!(c->sr & FB_SR_S))
                privilege_violation(md, c);
            else
                c->cycles += 132;
        } else if (op == 0x4E72) { /* STOP #imm */
            if (!(c->sr & FB_SR_S)) {
                privilege_violation(md, c);
            } else {
                uint16_t sr = fetch16(md, c);
                write_sr(c, sr);
                c->stopped = 1;
                c->cycles += 4;
            }
        } else if (op == 0x4E73) { /* RTE */
            if (!(c->sr & FB_SR_S)) {
                privilege_violation(md, c);
            } else {
                uint16_t sr = pop16(md, c);
                uint32_t pc = pop32(md, c);
                write_sr(c, sr);
                c->pc = pc;
                c->cycles += 20;
            }
        } else if (op == 0x4E75) { /* RTS */
            c->pc = pop32(md, c);
            c->cycles += 16;
        } else if (op == 0x4E76) { /* TRAPV */
            c->cycles += 4;
            if (c->sr & FB_SR_V)
                take_exception(md, 7, c->ppc + 2, 0);
        } else if ((op & 0xFFF0) == 0x4E40) { /* TRAP #n */
            take_exception(md, 32u + (op & 0xFu), c->ppc + 2, 0);
            c->cycles += 34;
        } else if (op == 0x4E77) { /* RTR */
            uint16_t ccr = pop16(md, c);
            uint32_t pc = pop32(md, c);
            c->sr = (uint16_t)((c->sr & 0xFF00u) | (ccr & 0x1Fu));
            c->pc = pc;
            c->cycles += 20;
        } else if ((op & 0xFF80) == 0x4C80) { /* MOVEM ea,reglist */
            op_movem(md, c, op, 1);
        } else if ((op & 0xFF80) == 0x4880) { /* MOVEM reglist,ea */
            op_movem(md, c, op, 0);
        } else if ((op & 0xFFC0) == 0x40C0) { /* MOVE SR,ea */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int cost = 0;
            c->cycles += 6;
            ea_write(md, c, e, OP_SIZE_W, c->sr, &cost);
            c->cycles += cost;
        } else if ((op & 0xFFC0) == 0x44C0) { /* MOVE ea,CCR */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int cost = 0;
            uint32_t v = ea_read(md, c, e, OP_SIZE_W, &cost);
            c->cycles += 12 + cost;
            c->sr = (uint16_t)((c->sr & 0xFF00u) | (v & 0x1Fu));
        } else if ((op & 0xFFC0) == 0x46C0) { /* MOVE ea,SR */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int cost = 0;
            if (!(c->sr & FB_SR_S)) {
                privilege_violation(md, c);
            } else {
                uint32_t v = ea_read(md, c, e, OP_SIZE_W, &cost);
                c->cycles += 12 + cost;
                write_sr(c, (uint16_t)v);
            }
        } else if ((op & 0xF1C0) == 0x4180) { /* CHK */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            uint8_t r = (uint8_t)((op >> 9) & 7);
            int cost = 0;
            int32_t src = (int32_t)(int16_t)ea_read(md, c, e, OP_SIZE_W, &cost);
            int32_t dn = (int32_t)(int16_t)(c->d[r] & 0xFFFFu);
            c->cycles += 10 + cost;
            if (dn < 0 || dn > src) {
                take_exception(md, 6, c->ppc + 2, 0);
            } else {
                c->sr = (uint16_t)((c->sr & ~FB_SR_N) |
                                   ((src & 0x8000u) ? FB_SR_N : 0));
            }
        } else if ((op & 0xF1C0) == 0x41C0) { /* LEA */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            uint8_t r = (uint8_t)((op >> 9) & 7);
            int cost = 0;
            if (e.mode == 0 || e.mode == 1 || e.mode == 3 || e.mode == 4) {
                illegal(md, c);
            } else {
                uint32_t a = ea_addr(md, c, e, OP_SIZE_L, &cost);
                c->cycles += 4 + cost;
                c->a[r] = a;
            }
        } else if (op == 0x4AFC) {
            illegal(md, c);
        } else if ((op & 0xFFC0) == 0x4AC0) { /* TAS */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int cost = 0;
            if (e.mode == 1) {
                illegal(md, c);
            } else {
                uint32_t v = ea_read(md, c, e, OP_SIZE_B, &cost);
                c->cycles += 10 + cost;
                flags_nzvc_logic(c, v, 0x80u);
                ea_write(md, c, e, OP_SIZE_B, 0x80u, &cost);
            }
        } else if ((op & 0xFFC0) == 0x4A00) { /* TST */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int size;
            int cost = 0;
            switch ((op >> 6) & 3) {
            case 0: size = OP_SIZE_B; break;
            case 1: size = OP_SIZE_W; break;
            case 2: size = OP_SIZE_L; break;
            default: illegal(md, c); goto done4;
            }
            if (e.mode == 1) {
                illegal(md, c);
                goto done4;
            }
            uint32_t v = ea_read(md, c, e, size, &cost);
            c->cycles += 4 + cost;
            flags_nzvc_logic(c, v & mask_of(size), sign_of(size));
        } else if ((op & 0xFFC0) == 0x4000) { /* NEGX */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int size;
            int cost = 0;
            switch ((op >> 6) & 3) {
            case 0: size = OP_SIZE_B; break;
            case 1: size = OP_SIZE_W; break;
            case 2: size = OP_SIZE_L; break;
            default: illegal(md, c); goto done4;
            }
            if (e.mode == 1) {
                illegal(md, c);
                goto done4;
            }
            uint32_t v = ea_read(md, c, e, size, &cost);
            uint32_t xin = (c->sr & FB_SR_X) ? 1u : 0u;
            uint32_t r = (0u - v - xin) & mask_of(size);
            c->cycles += 4 + cost;
            if (r != 0)
                c->sr &= (uint16_t)~FB_SR_Z;
            else
                c->sr |= FB_SR_Z;
            c->sr = (uint16_t)((c->sr & ~(FB_SR_C | FB_SR_V | FB_SR_N | FB_SR_X)));
            if (r & sign_of(size))
                c->sr |= FB_SR_N;
            if ((v != 0) || (xin != 0))
                c->sr |= FB_SR_C | FB_SR_X;
            ea_write(md, c, e, size, r, &cost);
        } else if ((op & 0xFFC0) == 0x4200) { /* CLR */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int size;
            int cost = 0;
            switch ((op >> 6) & 3) {
            case 0: size = OP_SIZE_B; break;
            case 1: size = OP_SIZE_W; break;
            case 2: size = OP_SIZE_L; break;
            default: illegal(md, c); goto done4;
            }
            if (e.mode == 1) {
                illegal(md, c);
                goto done4;
            }
            (void)ea_read(md, c, e, size, &cost);
            c->cycles += 4 + cost;
            c->sr = (uint16_t)((c->sr & ~(FB_SR_N | FB_SR_V | FB_SR_C)) | FB_SR_Z);
            ea_write(md, c, e, size, 0, &cost);
        } else if ((op & 0xFFC0) == 0x4400) { /* NEG */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int size;
            int cost = 0;
            switch ((op >> 6) & 3) {
            case 0: size = OP_SIZE_B; break;
            case 1: size = OP_SIZE_W; break;
            case 2: size = OP_SIZE_L; break;
            default: illegal(md, c); goto done4;
            }
            if (e.mode == 1) {
                illegal(md, c);
                goto done4;
            }
            uint32_t v = ea_read(md, c, e, size, &cost);
            uint32_t r = (0u - v) & mask_of(size);
            c->cycles += 4 + cost;
            flags_sub(c, 0, v, r, sign_of(size));
            ea_write(md, c, e, size, r, &cost);
        } else if ((op & 0xFFC0) == 0x4600) { /* NOT */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            int size;
            int cost = 0;
            switch ((op >> 6) & 3) {
            case 0: size = OP_SIZE_B; break;
            case 1: size = OP_SIZE_W; break;
            case 2: size = OP_SIZE_L; break;
            default: illegal(md, c); goto done4;
            }
            if (e.mode == 1) {
                illegal(md, c);
                goto done4;
            }
            uint32_t v = ea_read(md, c, e, size, &cost);
            uint32_t r = (~v) & mask_of(size);
            c->cycles += 4 + cost;
            flags_nzvc_logic(c, r, sign_of(size));
            ea_write(md, c, e, size, r, &cost);
        } else if ((op & 0xFFC0) == 0x4800) { /* NBCD */
            op_nbcd(md, c, op);
        } else {
            illegal(md, c);
        }
    done4:
        break;
    }

    case 0x5:
        op_quick(md, c, op);
        break;

    case 0x6: { /* Bcc / BRA / BSR */
        uint8_t cc = (uint8_t)((op >> 8) & 0xF);
        uint32_t target;
        if ((op & 0xFF) == 0) {
            int16_t disp = (int16_t)fetch16(md, c);
            target = c->pc + (uint32_t)(int32_t)disp;
            c->cycles += 10;
        } else {
            target = c->pc + (uint32_t)(int32_t)(int8_t)(op & 0xFF);
            c->cycles += 8;
        }
        if (cc == 1) { /* BF: never */
            break;
        }
        int take = (cc == 0) ? 1 : cond(c, cc);
        if (cc == 2) { /* BSR */
            push32(md, c, c->pc);
            c->pc = target;
        } else if (take) {
            c->pc = target;
        }
        break;
    }

    case 0x7: /* MOVEQ */
        if (op & 0x0100) {
            illegal(md, c);
        } else {
            uint32_t v = (uint32_t)(int32_t)(int8_t)(op & 0xFF);
            c->d[(op >> 9) & 7] = v;
            flags_nzvc_logic(c, v, 0x80000000u);
            c->cycles += 4;
        }
        break;

    case 0x8: /* OR / DIVU / DIVS / SBCD */
        if ((op & 0xF1C0) == 0x80C0) {
            op_divu(md, c, op);
        } else if ((op & 0xF1C0) == 0x81C0) {
            op_divs(md, c, op);
        } else if ((op & 0xF1F8) == 0x8100) {
            op_bcd(md, c, op, 1); /* SBCD */
        } else if (((op >> 6) & 3) == 3) {
            illegal(md, c);
        } else if (op & 0x0100) {
            alu_dn_ea(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 0);
        } else {
            alu_ea_dn(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 0);
        }
        break;

    case 0x9: /* SUB / SUBA / SUBX */
        if ((op & 0xF1C0) == 0x90C0) {
            alu_a(md, c, op, 1);
        } else if ((op & 0xF130) == 0x9100) { /* SUBX */
            uint8_t rx = (uint8_t)((op >> 9) & 7);
            int size, mem = (op & 0x0008) != 0;
            uint32_t a, b, r, xin;
            switch ((op >> 6) & 3) {
            case 0: size = OP_SIZE_B; break;
            case 1: size = OP_SIZE_W; break;
            case 2: size = OP_SIZE_L; break;
            default: illegal(md, c); goto done9;
            }
            xin = (c->sr & FB_SR_X) ? 1u : 0u;
            if (mem) {
                int ry = op & 7;
                c->a[ry] -= (uint32_t)size;
                c->a[rx] -= (uint32_t)size;
                a = fb_md_68k_read8(md, c->a[rx]);
                if (size >= OP_SIZE_W)
                    a = (a << 8) | fb_md_68k_read8(md, c->a[rx] + 1);
                if (size == OP_SIZE_L)
                    a = (a << 16) | ((uint32_t)fb_md_68k_read8(md, c->a[rx] + 2) << 8) |
                        fb_md_68k_read8(md, c->a[rx] + 3);
                b = fb_md_68k_read8(md, c->a[ry]);
                if (size >= OP_SIZE_W)
                    b = (b << 8) | fb_md_68k_read8(md, c->a[ry] + 1);
                if (size == OP_SIZE_L)
                    b = (b << 16) | ((uint32_t)fb_md_68k_read8(md, c->a[ry] + 2) << 8) |
                        fb_md_68k_read8(md, c->a[ry] + 3);
                c->cycles += 18;
                r = (b - a - xin) & mask_of(size);
                if (r != 0)
                    c->sr &= (uint16_t)~FB_SR_Z;
                else
                    c->sr |= FB_SR_Z;
                c->sr &= (uint16_t)~(FB_SR_N | FB_SR_V | FB_SR_C | FB_SR_X);
                if (r & sign_of(size))
                    c->sr |= FB_SR_N;
                {
                    uint32_t borrow = (~b & a) | (b & r) | (a & r);
                    if (borrow & sign_of(size))
                        c->sr |= FB_SR_C | FB_SR_X;
                    {
                        uint32_t over = (b & ~a & ~r) | (~b & a & r);
                        if (over & sign_of(size))
                            c->sr |= FB_SR_V;
                    }
                }
                /* write back byte-wise */
                if (size == OP_SIZE_B) {
                    fb_md_68k_write8(md, c->a[ry], (uint8_t)r);
                } else if (size == OP_SIZE_W) {
                    fb_md_68k_write8(md, c->a[ry], (uint8_t)(r >> 8));
                    fb_md_68k_write8(md, c->a[ry] + 1, (uint8_t)r);
                } else {
                    fb_md_68k_write8(md, c->a[ry], (uint8_t)(r >> 24));
                    fb_md_68k_write8(md, c->a[ry] + 1, (uint8_t)(r >> 16));
                    fb_md_68k_write8(md, c->a[ry] + 2, (uint8_t)(r >> 8));
                    fb_md_68k_write8(md, c->a[ry] + 3, (uint8_t)r);
                }
            } else {
                a = c->d[op & 7] & mask_of(size);
                b = c->d[rx] & mask_of(size);
                r = (b - a - xin) & mask_of(size);
                c->cycles += 4;
                if (r != 0)
                    c->sr &= (uint16_t)~FB_SR_Z;
                else
                    c->sr |= FB_SR_Z;
                c->sr &= (uint16_t)~(FB_SR_N | FB_SR_V | FB_SR_C | FB_SR_X);
                if (r & sign_of(size))
                    c->sr |= FB_SR_N;
                {
                    uint32_t borrow = (~b & a) | (b & r) | (a & r);
                    if (borrow & sign_of(size))
                        c->sr |= FB_SR_C | FB_SR_X;
                    {
                        uint32_t over = (b & ~a & ~r) | (~b & a & r);
                        if (over & sign_of(size))
                            c->sr |= FB_SR_V;
                    }
                }
                c->d[op & 7] = (c->d[op & 7] & ~mask_of(size)) | r;
            }
        done9:
            break;
        } else if (((op >> 6) & 3) == 3) {
            illegal(md, c);
        } else if (op & 0x0100) {
            alu_dn_ea(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 2);
        } else {
            alu_ea_dn(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 2);
        }
        break;

    case 0xA:
        take_exception(md, 10, c->ppc + 2, 0); /* line-A emulator */
        break;

    case 0xB: /* CMP / CMPA / EOR / CMPM */
        if (((op >> 6) & 3) == 3 && !(op & 0x0100)) {
            alu_a(md, c, op, 0); /* CMPA */
        } else if ((op & 0x0138) == 0x0108) { /* CMPM */
            uint8_t rx = (uint8_t)((op >> 9) & 7);
            int ry = op & 7;
            int size;
            uint32_t a, b, r;
            switch ((op >> 6) & 3) {
            case 0: size = OP_SIZE_B; break;
            case 1: size = OP_SIZE_W; break;
            case 2: size = OP_SIZE_L; break;
            default: illegal(md, c); goto doneB;
            }
            a = fb_md_68k_read8(md, c->a[ry]);
            if (size >= OP_SIZE_W)
                a = (a << 8) | fb_md_68k_read8(md, c->a[ry] + 1);
            if (size == OP_SIZE_L)
                a = (a << 16) | ((uint32_t)fb_md_68k_read8(md, c->a[ry] + 2) << 8) |
                    fb_md_68k_read8(md, c->a[ry] + 3);
            b = c->d[rx] & mask_of(size);
            r = (b - a) & mask_of(size);
            c->cycles += 12;
            flags_cmp(c, b, a, r, sign_of(size));
            c->a[ry] += (uint32_t)size;
        doneB:
            break;
        } else if (((op >> 6) & 3) == 3) {
            illegal(md, c);
        } else if (op & 0x0100) {
            alu_dn_ea(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 4); /* EOR */
        } else {
            alu_ea_dn(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 5); /* CMP */
        }
        break;

    case 0xC: /* AND / MULU / ABCD / EXG */
        if ((op & 0xF1C0) == 0xC0C0) { /* MULU */
            ea_t e = { (uint8_t)((op >> 3) & 7), (uint8_t)(op & 7) };
            uint8_t reg = (uint8_t)((op >> 9) & 7);
            int cost = 0;
            uint32_t v = ea_read(md, c, e, OP_SIZE_W, &cost);
            c->cycles += 40 + cost;
            if (e.mode == 1) {
                illegal(md, c);
                break;
            }
            uint32_t res = (c->d[reg] & 0xFFFFu) * v;
            c->d[reg] = res;
            flags_nzvc_logic(c, res, 0x80000000u);
        } else if ((op & 0xF1F8) == 0xC100) { /* ABCD */
            op_bcd(md, c, op, 0);
        } else if ((op & 0xF1C8) == 0xC140 || (op & 0xF1C8) == 0xC148 ||
                   (op & 0xF1C8) == 0xC188) { /* EXG */
            uint8_t rx = (uint8_t)((op >> 9) & 7);
            uint8_t ry = (uint8_t)(op & 7);
            c->cycles += 6;
            if ((op & 0xF1C8) == 0xC140) { /* Dx,Dy */
                uint32_t t = c->d[rx];
                c->d[rx] = c->d[ry];
                c->d[ry] = t;
            } else if ((op & 0xF1C8) == 0xC148) { /* Dx,Ay */
                uint32_t t = c->d[rx];
                c->d[rx] = c->a[ry];
                c->a[ry] = t;
            } else if ((op & 0xF1C8) == 0xC188) { /* An,An */
                uint32_t t = c->a[rx];
                c->a[rx] = c->a[ry];
                c->a[ry] = t;
            } else {
                illegal(md, c);
            }
        } else if (((op >> 6) & 3) == 3) {
            illegal(md, c);
        } else if (op & 0x0100) {
            alu_dn_ea(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 1);
        } else {
            alu_ea_dn(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 1);
        }
        break;

    case 0xD: /* ADD / ADDA / ADDX */
        if ((op & 0xF1C0) == 0xD0C0) {
            alu_a(md, c, op, 2);
        } else if ((op & 0xF130) == 0xD100) { /* ADDX (register form only here) */
            ea_t e = { 0, 0 };
            (void)e;
            /* Delegate to the SUBX code path with mirrored logic via op_addx */
            op_addx_reg(md, c, op);
        } else if (((op >> 6) & 3) == 3) {
            illegal(md, c);
        } else if (op & 0x0100) {
            alu_dn_ea(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 3);
        } else {
            alu_ea_dn(md, c, op, ((op >> 6) & 3) == 0 ? OP_SIZE_B : OP_SIZE_W, 3);
        }
        break;

    case 0xE:
        if ((op & 0xC0) == 0xC0)
            op_shift_mem(md, c, op);
        else
            op_shift_reg(md, c, op);
        break;

    case 0xF:
        take_exception(md, 11, c->ppc + 2, 0); /* line-F */
        break;

    default:
        illegal(md, c);
        break;
    }
}
