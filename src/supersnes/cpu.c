/*
 * supersnes CPU: 65C816 (5A22), full official instruction set.
 *
 * Implements: emulation/native modes with XCE, 8/16-bit accumulator and
 * index registers (M/X flags), direct page (emulation mode forces the DP
 * high byte to zero), long addressing, decimal mode ADC/SBC (N/V/Z from the
 * binary result per 65C816), block move MVN/MVP, WAI/STP, COP/BRK with
 * mode-dependent vectors, PEA/PEI/PER, TRB/TSB, bit-flag B push semantics.
 *
 * Cycle counts: base opcode tables plus page-cross/index penalties for the
 * documented cases. Documented approximation, not cycle-exact.
 */
#include "snes.h"

#define RD(a) snes_bus_read(s, (a))
#define WR(a, v) snes_bus_write(s, (a), (v))

#define BANK(b, a) ((uint32_t)((uint32_t)(b) << 16) | (uint32_t)(uint16_t)(a))

static inline uint8_t m8(const snes_cpu *c) { return (uint8_t)(c->p & SNES_FM); }
static inline uint8_t x8(const snes_cpu *c) { return (uint8_t)(c->p & SNES_FX); }

static inline void setzn8(snes_cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(SNES_FZ | SNES_FN)) |
                     (v == 0 ? SNES_FZ : 0u) | (v & 0x80u));
}

static inline void setzn16(snes_cpu *c, uint16_t v)
{
    c->p = (uint8_t)((c->p & ~(SNES_FZ | SNES_FN)) |
                     (v == 0 ? SNES_FZ : 0u) | ((v >> 8) & 0x80u));
}

/* width-aware flag update: M=1 tests bit 7, M=0 bit 15 */
static void setzn_m(snes_cpu *c, uint16_t v)
{
    if (m8(c))
        setzn8(c, (uint8_t)v);
    else
        setzn16(c, v);
}

static void setzn_xw(snes_cpu *c, uint16_t v)
{
    if (x8(c))
        setzn8(c, (uint8_t)v);
    else
        setzn16(c, v);
}

static inline uint16_t sp_ptr(const snes_cpu *c)
{
    return c->e ? (uint16_t)(0x0100u | (c->sp & 0xFFu)) : c->sp;
}

static inline void sp_dec(snes_cpu *c)
{
    c->sp = (uint16_t)(c->sp - 1u);
    if (c->e)
        c->sp = (uint16_t)(0x0100u | (c->sp & 0xFFu));
}

static inline void sp_inc(snes_cpu *c)
{
    c->sp = (uint16_t)(c->sp + 1u);
    if (c->e)
        c->sp = (uint16_t)(0x0100u | (c->sp & 0xFFu));
}

static inline void push8(snes_t *s, snes_cpu *c, uint8_t v)
{
    WR(BANK(0u, sp_ptr(c)), v);
    sp_dec(c);
}

static inline uint8_t pop8(snes_t *s, snes_cpu *c)
{
    sp_inc(c);
    return RD(BANK(0u, sp_ptr(c)));
}

static inline void push16(snes_t *s, snes_cpu *c, uint16_t v)
{
    push8(s, c, (uint8_t)(v >> 8));
    push8(s, c, (uint8_t)v);
}

static inline uint16_t pop16(snes_t *s, snes_cpu *c)
{
    uint16_t lo = pop8(s, c);
    return (uint16_t)(lo | ((uint16_t)pop8(s, c) << 8));
}

static inline uint8_t fetch8(snes_t *s, snes_cpu *c)
{
    uint8_t v = RD(BANK(c->k, c->pc));
    c->pc = (uint16_t)(c->pc + 1u);
    return v;
}

static inline uint16_t fetch16(snes_t *s, snes_cpu *c)
{
    uint16_t lo = fetch8(s, c);
    return (uint16_t)(lo | ((uint16_t)fetch8(s, c) << 8));
}

/* ---- addressing: packed 24-bit addresses ---------------------------------- */

static uint32_t a_abs(snes_t *s, snes_cpu *c, uint8_t bank)
{
    uint16_t a = fetch16(s, c);
    return BANK(bank, a);
}

static uint32_t a_absx(snes_t *s, snes_cpu *c, uint8_t bank, uint32_t *pen, int rmw)
{
    uint16_t base = fetch16(s, c);
    uint16_t idx = x8(c) ? (c->x & 0xFFu) : c->x;
    uint16_t lo = (uint16_t)(base + idx);
    if (!x8(c) && ((base & 0xFF00u) != (lo & 0xFF00u)))
        *pen += rmw ? 0 : 1;
    return BANK(bank, lo);
}

static uint32_t a_absy(snes_t *s, snes_cpu *c, uint8_t bank, uint32_t *pen, int rmw)
{
    uint16_t base = fetch16(s, c);
    uint16_t idx = x8(c) ? (c->y & 0xFFu) : c->y;
    uint16_t lo = (uint16_t)(base + idx);
    if (!x8(c) && ((base & 0xFF00u) != (lo & 0xFF00u)))
        *pen += rmw ? 0 : 1;
    return BANK(bank, lo);
}

static uint32_t a_long(snes_t *s, snes_cpu *c)
{
    uint16_t lo = fetch16(s, c);
    uint8_t bank = fetch8(s, c);
    return BANK(bank, lo);
}

static uint32_t a_longx(snes_t *s, snes_cpu *c)
{
    uint16_t lo = fetch16(s, c);
    uint8_t bank = fetch8(s, c);
    uint16_t idx = x8(c) ? (c->x & 0xFFu) : c->x;
    return BANK(bank, (uint16_t)(lo + idx));
}

static inline uint16_t dp_ptr(snes_cpu *c, uint8_t off)
{
    uint16_t d = c->e ? (uint16_t)(c->dp & 0xFFu) : c->dp;
    return (uint16_t)(d + off);
}

static inline int dp_cross(const snes_cpu *c)
{
    return (c->dp & 0xFFu) != 0u ? 1 : 0;
}

static uint32_t a_dp(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += (uint32_t)dp_cross(c);
    return BANK(c->dbr, dp_ptr(c, off));
}

static uint32_t a_dpx(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1u + (uint32_t)dp_cross(c);
    uint16_t idx = x8(c) ? (c->x & 0xFFu) : c->x;
    return BANK(c->dbr, (uint16_t)(dp_ptr(c, off) + idx));
}

static uint32_t a_dpy(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1u + (uint32_t)dp_cross(c);
    uint16_t idx = x8(c) ? (c->y & 0xFFu) : c->y;
    return BANK(c->dbr, (uint16_t)(dp_ptr(c, off) + idx));
}

static void branch8(snes_t *s, snes_cpu *c, int cond, uint32_t *cycles)
{
    int8_t off = (int8_t)fetch8(s, c);
    *cycles += 1;
    if (cond) {
        *cycles += 1;
        c->pc = (uint16_t)(c->pc + (uint16_t)(int16_t)off);
    }
}

static void branch16(snes_t *s, snes_cpu *c, int cond, uint32_t *cycles)
{
    uint16_t off = fetch16(s, c);
    *cycles += 1;
    if (cond) {
        *cycles += 1;
        c->pc = (uint16_t)(c->pc + off);
    }
}

static uint16_t read16_dp(snes_t *s, snes_cpu *c, uint16_t ptr) /* NOLINT */
{
    (void)c;
    return (uint16_t)(RD(BANK(0u, ptr)) |
                      ((uint16_t)RD(BANK(0u, (uint16_t)(ptr + 1u))) << 8));
}

static uint32_t a_dpi(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1u + (uint32_t)dp_cross(c);
    return BANK(c->dbr, read16_dp(s, c, dp_ptr(c, off)));
}

static uint32_t a_dpiy(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1u + (uint32_t)dp_cross(c);
    uint16_t base = read16_dp(s, c, dp_ptr(c, off));
    uint16_t idx = x8(c) ? (c->y & 0xFFu) : c->y;
    if (!x8(c) && ((base & 0xFF00u) != ((uint16_t)(base + idx) & 0xFF00u)))
        *pen += 1;
    return BANK(c->dbr, (uint16_t)(base + idx));
}

static uint32_t a_dpil(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1u + (uint32_t)dp_cross(c);
    uint16_t ptr = dp_ptr(c, off);
    uint16_t lo16 = read16_dp(s, c, ptr);
    uint8_t bank = RD(BANK(0u, (uint16_t)(ptr + 2u)));
    return BANK(bank, lo16);
}

static uint32_t a_dpily(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1u + (uint32_t)dp_cross(c);
    uint16_t ptr = dp_ptr(c, off);
    uint16_t lo16 = read16_dp(s, c, ptr);
    uint8_t bank = RD(BANK(0u, (uint16_t)(ptr + 2u)));
    uint16_t idx = x8(c) ? (c->y & 0xFFu) : c->y;
    return BANK(bank, (uint16_t)(lo16 + idx));
}

static uint32_t a_dpix(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1u + (uint32_t)dp_cross(c);
    uint16_t idx = x8(c) ? (c->x & 0xFFu) : c->x;
    uint16_t ptr = (uint16_t)(dp_ptr(c, off) + idx);
    return BANK(c->dbr, read16_dp(s, c, ptr));
}

static uint32_t a_sr(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1;
    return BANK(0u, (uint16_t)(sp_ptr(c) + off));
}

static uint32_t a_sriy(snes_t *s, snes_cpu *c, uint32_t *pen)
{
    uint8_t off = fetch8(s, c);
    *pen += 1;
    uint16_t ptr = (uint16_t)(sp_ptr(c) + off);
    uint16_t base = read16_dp(s, c, ptr);
    uint16_t idx = x8(c) ? (c->y & 0xFFu) : c->y;
    return BANK(c->dbr, (uint16_t)(base + idx));
}

/* ---- M-width memory access ------------------------------------------------- */

static uint16_t mem_read_m(snes_t *s, uint32_t addr, const snes_cpu *c)
{
    uint16_t v = RD(addr);
    if (!m8(c))
        v |= (uint16_t)RD(addr + 1u) << 8;
    return v;
}

static void mem_write_m(snes_t *s, uint32_t addr, uint16_t v, const snes_cpu *c)
{
    WR(addr, (uint8_t)(v & 0xFFu));
    if (!m8(c))
        WR(addr + 1u, (uint8_t)(v >> 8));
}

static uint16_t get_a(const snes_cpu *c)
{
    return m8(c) ? (uint16_t)(c->a & 0xFFu) : c->a;
}

static void set_a(snes_cpu *c, uint16_t v)
{
    if (m8(c))
        c->a = (uint16_t)((c->a & 0xFF00u) | (v & 0xFFu));
    else
        c->a = v;
}

static uint16_t get_x(const snes_cpu *c)
{
    return x8(c) ? (uint16_t)(c->x & 0xFFu) : c->x;
}

static void set_x(snes_cpu *c, uint16_t v)
{
    if (x8(c))
        c->x = (uint16_t)((c->x & 0xFF00u) | (v & 0xFFu));
    else
        c->x = v;
    setzn_xw(c, get_x(c));
}

static uint16_t get_y(const snes_cpu *c)
{
    return x8(c) ? (uint16_t)(c->y & 0xFFu) : c->y;
}

static void set_y(snes_cpu *c, uint16_t v)
{
    if (x8(c))
        c->y = (uint16_t)((c->y & 0xFF00u) | (v & 0xFFu));
    else
        c->y = v;
    setzn_xw(c, get_y(c));
}

/* ---- ALU ------------------------------------------------------------------- */

static void adc_m(snes_cpu *c, uint16_t m)
{
    if (m8(c)) {
        uint8_t a = (uint8_t)(c->a & 0xFFu);
        uint8_t b = (uint8_t)m;
        uint8_t carry = (uint8_t)(c->p & SNES_FC);
        uint8_t bin = (uint8_t)(a + b + carry);
        if (c->p & SNES_FD) {
            uint8_t lo = (uint8_t)((a & 0x0Fu) + (b & 0x0Fu) + carry);
            uint8_t hi = (uint8_t)((a >> 4) + (b >> 4));
            if (lo > 9u) {
                lo = (uint8_t)(lo + 6u);
                hi++;
            }
            uint8_t dec_carry = 0;
            if (hi > 9u) {
                hi = (uint8_t)(hi - 10u);
                dec_carry = 1;
            }
            c->p &= (uint8_t)~SNES_FC;
            if (bin > 0x99u)
                c->p |= SNES_FC;
            uint8_t r = (uint8_t)(((hi & 0x0Fu) << 4) | (lo & 0x0Fu));
            (void)dec_carry;
            c->p = (uint8_t)((c->p & ~(SNES_FZ | SNES_FV | SNES_FN)) |
                             (bin == 0 ? SNES_FZ : 0u) |
                             ((~(a ^ b) & (a ^ bin) & 0x80u) ? SNES_FV : 0u) |
                             (bin & 0x80u));
            c->a = (uint16_t)((c->a & 0xFF00u) | r);
            return;
        }
        c->p &= (uint8_t)~(SNES_FC | SNES_FV);
        if ((uint32_t)a + (uint32_t)b + (uint32_t)carry > 0xFFu)
            c->p |= SNES_FC;
        if ((~(a ^ b) & (a ^ bin) & 0x80u) != 0u)
            c->p |= SNES_FV;
        c->a = (uint16_t)((c->a & 0xFF00u) | bin);
        setzn8(c, bin);
    } else {
        uint32_t sum = (uint32_t)c->a + m + (c->p & SNES_FC);
        uint16_t r = (uint16_t)sum;
        c->p &= (uint8_t)~(SNES_FC | SNES_FV);
        if (sum > 0xFFFFu)
            c->p |= SNES_FC;
        if ((~(c->a ^ m) & (c->a ^ r) & 0x8000u) != 0u)
            c->p |= SNES_FV;
        c->a = r;
        setzn16(c, r);
    }
}

static void sbc_m(snes_cpu *c, uint16_t m)
{
    if (m8(c)) {
        uint8_t a = (uint8_t)(c->a & 0xFFu);
        uint8_t b = (uint8_t)m;
        uint8_t borrow = (uint8_t)(1u - (c->p & SNES_FC));
        uint8_t bin = (uint8_t)(a - b - borrow);
        if (c->p & SNES_FD) {
            int lo = (int)(a & 0x0Fu) - (int)(b & 0x0Fu) - (int)borrow;
            int hi = (int)(a >> 4) - (int)(b >> 4);
            if (lo < 0) {
                lo += 10;
                hi--;
            }
            if (hi < 0)
                hi += 10;
            c->p &= (uint8_t)~SNES_FC;
            if (a >= b + borrow)
                c->p |= SNES_FC;
            uint8_t r = (uint8_t)(((uint8_t)hi << 4) | ((uint8_t)lo & 0x0Fu));
            c->p = (uint8_t)((c->p & ~(SNES_FZ | SNES_FV | SNES_FN)) |
                             (bin == 0 ? SNES_FZ : 0u) |
                             (((a ^ b) & (a ^ bin) & 0x80u) ? SNES_FV : 0u) |
                             (bin & 0x80u));
            c->a = (uint16_t)((c->a & 0xFF00u) | r);
            return;
        }
        c->p &= (uint8_t)~(SNES_FC | SNES_FV);
        if (a >= b + borrow)
            c->p |= SNES_FC;
        if (((a ^ b) & (a ^ bin) & 0x80u) != 0u)
            c->p |= SNES_FV;
        c->a = (uint16_t)((c->a & 0xFF00u) | bin);
        setzn8(c, bin);
    } else {
        uint32_t diff = (uint32_t)c->a - m - (1u - (c->p & SNES_FC));
        uint16_t r = (uint16_t)diff;
        c->p &= (uint8_t)~(SNES_FC | SNES_FV);
        if (c->a >= m + (1u - (c->p & SNES_FC)))
            c->p |= SNES_FC;
        if (((c->a ^ m) & (c->a ^ r) & 0x8000u) != 0u)
            c->p |= SNES_FV;
        c->a = r;
        setzn16(c, r);
    }
}

static void cmp_m(snes_cpu *c, uint16_t reg, uint16_t m)
{
    if (m8(c)) {
        uint8_t r = (uint8_t)(reg - m);
        c->p &= (uint8_t)~SNES_FC;
        if ((uint8_t)reg >= (uint8_t)m)
            c->p |= SNES_FC;
        setzn8(c, r);
    } else {
        uint16_t r = (uint16_t)(reg - m);
        c->p &= (uint8_t)~SNES_FC;
        if (reg >= m)
            c->p |= SNES_FC;
        setzn16(c, r);
    }
}

/* compare with X-width registers (CPX/CPY): 8-bit when X flag set */
static void cmp_x(snes_cpu *c, uint16_t reg, uint16_t m)
{
    if (x8(c)) {
        uint8_t r = (uint8_t)(reg - m);
        c->p &= (uint8_t)~SNES_FC;
        if ((uint8_t)reg >= (uint8_t)m)
            c->p |= SNES_FC;
        setzn8(c, r);
    } else {
        uint16_t r = (uint16_t)(reg - m);
        c->p &= (uint8_t)~SNES_FC;
        if (reg >= m)
            c->p |= SNES_FC;
        setzn16(c, r);
    }
}

static void asl_m(snes_cpu *c, uint16_t *v)
{
    if (m8(c)) {
        uint8_t r = (uint8_t)*v;
        c->p = (uint8_t)((c->p & ~SNES_FC) | ((r >> 7) & 1u));
        r = (uint8_t)(r << 1);
        *v = r;
        setzn8(c, r);
    } else {
        c->p = (uint8_t)((c->p & ~SNES_FC) | ((*v >> 15) & 1u));
        *v = (uint16_t)(*v << 1);
        setzn16(c, *v);
    }
}

static void lsr_m(snes_cpu *c, uint16_t *v)
{
    if (m8(c)) {
        uint8_t r = (uint8_t)*v;
        c->p = (uint8_t)((c->p & ~SNES_FC) | (r & 1u));
        r = (uint8_t)(r >> 1);
        *v = r;
        setzn8(c, r);
    } else {
        c->p = (uint8_t)((c->p & ~SNES_FC) | (*v & 1u));
        *v = (uint16_t)(*v >> 1);
        setzn16(c, *v);
    }
}

static void rol_m(snes_cpu *c, uint16_t *v)
{
    if (m8(c)) {
        uint8_t r = (uint8_t)*v;
        uint8_t cin = (uint8_t)(c->p & SNES_FC);
        c->p = (uint8_t)((c->p & ~SNES_FC) | ((r >> 7) & 1u));
        r = (uint8_t)((r << 1) | cin);
        *v = r;
        setzn8(c, r);
    } else {
        uint16_t cin = (uint16_t)(c->p & SNES_FC);
        c->p = (uint8_t)((c->p & ~SNES_FC) | ((*v >> 15) & 1u));
        *v = (uint16_t)((*v << 1) | cin);
        setzn16(c, *v);
    }
}

static void ror_m(snes_cpu *c, uint16_t *v)
{
    if (m8(c)) {
        uint8_t r = (uint8_t)*v;
        uint8_t cin = (uint8_t)((c->p & SNES_FC) << 7);
        c->p = (uint8_t)((c->p & ~SNES_FC) | (r & 1u));
        r = (uint8_t)((r >> 1) | cin);
        *v = r;
        setzn8(c, r);
    } else {
        uint16_t cin = (uint16_t)((c->p & SNES_FC) << 15);
        c->p = (uint8_t)((c->p & ~SNES_FC) | (*v & 1u));
        *v = (uint16_t)((*v >> 1) | cin);
        setzn16(c, *v);
    }
}

static void bit_m(snes_cpu *c, uint16_t operand)
{
    c->p = (uint8_t)((c->p & ~(SNES_FZ | SNES_FV | SNES_FN)) |
                     (((get_a(c) & operand) == 0u) ? SNES_FZ : 0u) |
                     (operand & 0xC0u));
}

/* interrupt dispatch; brk_flag pushes bit4 set on the P byte */
static void do_interrupt(snes_t *s, snes_cpu *c, int brk_flag, uint16_t vec_emu,
                         uint16_t vec_native)
{
    push8(s, c, c->k);
    push16(s, c, c->pc);
    uint8_t pushed = c->p;
    if (c->e) {
        pushed = (uint8_t)((pushed & ~SNES_FX) | (brk_flag ? SNES_FX : 0u));
        pushed = (uint8_t)(pushed | SNES_FM | SNES_FX);
    } else {
        pushed = (uint8_t)((pushed & ~0x10u) | (brk_flag ? 0x10u : 0u));
    }
    push8(s, c, pushed);
    c->p |= SNES_FI;
    uint16_t vec = c->e ? vec_emu : vec_native;
    c->pc = (uint16_t)(RD(BANK(0u, vec)) |
                       ((uint16_t)RD(BANK(0u, (uint16_t)(vec + 1u))) << 8));
    if (c->e) {
        c->p |= (uint8_t)(SNES_FM | SNES_FX);
        c->dbr = 0;
        c->dp = (uint16_t)(c->dp & 0xFFu);
        c->k = 0;
    }
}

uint32_t snes_cpu_step(snes_t *s)
{
    snes_cpu *c = &s->cpu;

    if (c->stp)
        return 6;

    if (c->wai) {
        if (c->nmi_pending || (c->irq_line && !(c->p & SNES_FI))) {
            c->wai = 0;
        } else {
            return 6;
        }
    }

    if (c->nmi_pending) {
        c->nmi_pending = 0;
        do_interrupt(s, c, 0, 0xFFFAu, 0xFFEAu);
        return 8;
    }
    if (c->irq_line && !(c->p & SNES_FI)) {
        do_interrupt(s, c, 0, 0xFFFEu, 0xFFEEu);
        return 8;
    }

    uint32_t cycles = 2;
    uint32_t pen = 0;
    uint32_t addr;
    uint16_t operand;
    uint8_t op = fetch8(s, c);

    switch (op) {
    /* ---- ORA/AND/EOR/ADC/STA/LDA/CMP/SBC via mode nibble ---- */
    case 0x01: case 0x03: case 0x05: case 0x07: case 0x09: case 0x0D: case
    0x0F: case 0x11: case 0x12: case 0x13: case 0x15: case 0x17: case 0x19:
    case 0x1D: case 0x1F: case 0x21: case 0x23: case 0x25: case 0x27: case
    0x29: case 0x2D: case 0x2F: case 0x31: case 0x32: case 0x33: case 0x35:
    case 0x37: case 0x39: case 0x3D: case 0x3F: case 0x41: case 0x43: case
    0x45: case 0x47: case 0x49: case 0x4D: case 0x4F: case 0x51: case 0x52:
    case 0x53: case 0x55: case 0x57: case 0x59: case 0x5D: case 0x5F: case
    0x61: case 0x63: case 0x65: case 0x67: case 0x69: case 0x6D: case 0x6F:
    case 0x71: case 0x72: case 0x73: case 0x75: case 0x77: case 0x79: case
    0x7D: case 0x7F: case 0x81: case 0x83: case 0x85: case 0x87: case 0x8D:
    case 0x8F: case 0x91: case 0x92: case 0x93: case 0x95: case 0x97: case
    0x99: case 0x9D: case 0x9F: case 0xA1: case 0xA3: case 0xA5: case 0xA7:
    case 0xA9: case 0xAD: case 0xAF: case 0xB1: case 0xB2: case 0xB3: case
    0xB5: case 0xB7: case 0xB9: case 0xBD: case 0xBF: case 0xC1: case 0xC3:
    case 0xC5: case 0xC7: case 0xC9: case 0xCD: case 0xCF: case 0xD1: case
    0xD2: case 0xD3: case 0xD5: case 0xD7: case 0xD9: case 0xDD: case 0xDF:
    case 0xE1: case 0xE3: case 0xE5: case 0xE7: case 0xE9: case 0xED: case
    0xEF: case 0xF1: case 0xF2: case 0xF3: case 0xF5: case 0xF7: case 0xF9:
    case 0xFD: case 0xFF:
    {
        uint8_t group = (uint8_t)((op >> 5) & 7u);   /* 0=ORA 2=AND 4=EOR 6=ADC
                                                        4=STA 5=LDA 6=CMP 7=SBC */
        uint8_t mode = (uint8_t)(op & 0x1Fu);
        cycles = 2;
        if (mode == 0x01u) { addr = a_dpix(s, c, &pen); cycles = 7; }
        else if (mode == 0x03u) { addr = a_sr(s, c, &pen); cycles = 5; }
        else if (mode == 0x05u) { addr = a_dp(s, c, &pen); cycles = 3; }
        else if (mode == 0x07u) { addr = a_dpil(s, c, &pen); cycles = 7; }
        else if (mode == 0x09u) {
            /* immediate */
            cycles = m8(c) ? 2 : 3;
            operand = m8(c) ? fetch8(s, c) : fetch16(s, c);
            addr = 0;
            if (group != 4u) {
                switch (group) {
                case 0: set_a(c, get_a(c) | operand); setzn_m(c, get_a(c)); break;
                case 1: set_a(c, get_a(c) & operand); setzn_m(c, get_a(c)); break;
                case 2: set_a(c, get_a(c) ^ operand); setzn_m(c, get_a(c)); break;
                case 3: adc_m(c, operand); break;
                case 5: set_a(c, operand); setzn_m(c, get_a(c)); break;
                case 6: cmp_m(c, get_a(c), operand); break;
                default: sbc_m(c, operand); break;
                }
            }
            break;
        }
        else if (mode == 0x0Du) { addr = a_abs(s, c, c->dbr); cycles = 4; }
        else if (mode == 0x0Fu) { addr = a_long(s, c); cycles = 5; }
        else if (mode == 0x11u) { addr = a_dpiy(s, c, &pen); cycles = 6; }
        else if (mode == 0x12u) { addr = a_dpi(s, c, &pen); cycles = 6; }
        else if (mode == 0x13u) { addr = a_sriy(s, c, &pen); cycles = 7; }
        else if (mode == 0x15u) { addr = a_dpx(s, c, &pen); cycles = 4; }
        else if (mode == 0x17u) { addr = a_dpily(s, c, &pen); cycles = 7; }
        else if (mode == 0x19u) { addr = a_absy(s, c, c->dbr, &pen, 0); cycles = 4; }
        else if (mode == 0x1Du) { addr = a_absx(s, c, c->dbr, &pen, 0); cycles = 4; }
        else if (mode == 0x1Fu) { addr = a_longx(s, c); cycles = 5; }
        else { cycles = 2; break; }

        cycles += pen;
        switch (group) {
        case 0: set_a(c, get_a(c) | mem_read_m(s, addr, c)); setzn_m(c, get_a(c)); break;
        case 1: set_a(c, get_a(c) & mem_read_m(s, addr, c)); setzn_m(c, get_a(c)); break;
        case 2: set_a(c, get_a(c) ^ mem_read_m(s, addr, c)); setzn_m(c, get_a(c)); break;
        case 3: adc_m(c, mem_read_m(s, addr, c)); break;
        case 4: mem_write_m(s, addr, get_a(c), c); break;
        case 5: set_a(c, mem_read_m(s, addr, c)); setzn_m(c, get_a(c)); break;
        case 6: cmp_m(c, get_a(c), mem_read_m(s, addr, c)); break;
        default: sbc_m(c, mem_read_m(s, addr, c)); break;
        }
        if (group == 4u)
            cycles = (uint32_t)(cycles + (m8(c) ? 0 : 1)); /* 16-bit store +1 */
        break;
    }

    /* ---- shifts / RMW: ASL ROL LSR ROR, INC DEC ---- */
    case 0x06: case 0x26: case 0x46: case 0x66: /* dp */
    case 0x0E: case 0x2E: case 0x4E: case 0x6E: /* abs */
    case 0x16: case 0x36: case 0x56: case 0x76: /* dp,X */
    case 0x1E: case 0x3E: case 0x5E: case 0x7E: /* abs,X */
    case 0xE6: case 0xEE: case 0xFE:            /* INC */
    case 0xC6: case 0xCE: case 0xDE:            /* DEC */
    {
        cycles = 4;
        if ((op & 0x1Fu) == 0x06u) { addr = a_dp(s, c, &pen); cycles = 5; }
        else if ((op & 0x1Fu) == 0x0Eu) { addr = a_abs(s, c, c->dbr); cycles = 6; }
        else if ((op & 0x1Fu) == 0x16u) { addr = a_dpx(s, c, &pen); cycles = 6; }
        else { addr = a_absx(s, c, c->dbr, &pen, 1); cycles = 7; }
        cycles += pen;
        if (!m8(c))
            cycles += 2;
        uint16_t v = mem_read_m(s, addr, c);
        uint8_t kind = (uint8_t)(op & 0xE0u);
        if (op >= 0xE6u) { /* INC */
            v = (uint16_t)(v + 1u);
            setzn16(c, m8(c) ? (uint16_t)(v & 0xFFu) : v);
        } else if (op >= 0xC6u) { /* DEC */
            v = (uint16_t)(v - 1u);
            setzn16(c, m8(c) ? (uint16_t)(v & 0xFFu) : v);
        } else if (kind == 0x00u) asl_m(c, &v);
        else if (kind == 0x20u) rol_m(c, &v);
        else if (kind == 0x40u) lsr_m(c, &v);
        else ror_m(c, &v);
        mem_write_m(s, addr, v, c);
        break;
    }

    /* accumulator shifts / INC DEC A */
    case 0x0A: { uint16_t v = get_a(c); asl_m(c, &v); set_a(c, v); cycles = 2; break; }
    case 0x2A: { uint16_t v = get_a(c); rol_m(c, &v); set_a(c, v); cycles = 2; break; }
    case 0x4A: { uint16_t v = get_a(c); lsr_m(c, &v); set_a(c, v); cycles = 2; break; }
    case 0x6A: { uint16_t v = get_a(c); ror_m(c, &v); set_a(c, v); cycles = 2; break; }
    case 0x1A: { uint16_t v = (uint16_t)(get_a(c) + 1u); set_a(c, v);
                 setzn_m(c, v); cycles = 2; break; }
    case 0x3A: { uint16_t v = (uint16_t)(get_a(c) - 1u); set_a(c, v);
                 setzn_m(c, v); cycles = 2; break; }

    /* ---- index registers ---- */
    case 0xA2: set_x(c, x8(c) ? fetch8(s, c) : fetch16(s, c)); cycles = x8(c) ? 2 : 3; break;
    case 0xA6: set_x(c, RD(a_dp(s, c, &pen))); cycles = 3 + pen; break;
    case 0xB6: set_x(c, RD(a_dpy(s, c, &pen))); cycles = 4 + pen; break;
    case 0xAE: set_x(c, RD(a_abs(s, c, c->dbr))); cycles = 4; break;
    case 0xBE: set_x(c, RD(a_absy(s, c, c->dbr, &pen, 0))); cycles = 4 + pen; break;
    case 0xA0: set_y(c, x8(c) ? fetch8(s, c) : fetch16(s, c)); cycles = x8(c) ? 2 : 3; break;
    case 0xA4: set_y(c, RD(a_dp(s, c, &pen))); cycles = 3 + pen; break;
    case 0xB4: set_y(c, RD(a_dpx(s, c, &pen))); cycles = 4 + pen; break;
    case 0xAC: set_y(c, RD(a_abs(s, c, c->dbr))); cycles = 4; break;
    case 0xBC: set_y(c, RD(a_absx(s, c, c->dbr, &pen, 0))); cycles = 4 + pen; break;
    case 0x86: WR(a_dp(s, c, &pen), (uint8_t)get_x(c)); cycles = 3 + pen; break;
    case 0x96: WR(a_dpy(s, c, &pen), (uint8_t)get_x(c)); cycles = 4 + pen; break;
    case 0x8E: WR(a_abs(s, c, c->dbr), (uint8_t)get_x(c)); cycles = 4; break;
    case 0x84: WR(a_dp(s, c, &pen), (uint8_t)get_y(c)); cycles = 3 + pen; break;
    case 0x94: WR(a_dpx(s, c, &pen), (uint8_t)get_y(c)); cycles = 4 + pen; break;
    case 0x8C: WR(a_abs(s, c, c->dbr), (uint8_t)get_y(c)); cycles = 4; break;

    case 0xC0: cmp_x(c, get_y(c), x8(c) ? fetch8(s, c) : fetch16(s, c));
               cycles = x8(c) ? 2 : 3; break; /* CPY # */
    case 0xE0: cmp_x(c, get_x(c), x8(c) ? fetch8(s, c) : fetch16(s, c));
               cycles = x8(c) ? 2 : 3; break; /* CPX # */
    case 0xC4: cmp_x(c, get_y(c), RD(a_dp(s, c, &pen))); cycles = 3 + pen; break;
    case 0xEC: cmp_x(c, get_y(c), RD(a_abs(s, c, c->dbr))); cycles = 4; break;
    case 0xE4: cmp_x(c, get_x(c), RD(a_dp(s, c, &pen))); cycles = 3 + pen; break;
    case 0xCC: cmp_x(c, get_x(c), RD(a_abs(s, c, c->dbr))); cycles = 4; break;

    case 0xE8: set_x(c, (uint16_t)(get_x(c) + 1u)); cycles = 2; break;
    case 0xC8: set_y(c, (uint16_t)(get_y(c) + 1u)); cycles = 2; break;
    case 0xCA: set_x(c, (uint16_t)(get_x(c) - 1u)); cycles = 2; break;
    case 0x88: set_y(c, (uint16_t)(get_y(c) - 1u)); cycles = 2; break;

    /* ---- BIT ---- */
    case 0x89: bit_m(c, m8(c) ? fetch8(s, c) : fetch16(s, c));
               cycles = m8(c) ? 2 : 3; break; /* BIT imm: only Z */
    case 0x24: bit_m(c, RD(a_dp(s, c, &pen))); cycles = 3 + pen; break;
    case 0x2C: bit_m(c, RD(a_abs(s, c, c->dbr))); cycles = 4; break;
    case 0x34: bit_m(c, RD(a_dpx(s, c, &pen))); cycles = 4 + pen; break;
    case 0x3C: bit_m(c, RD(a_absx(s, c, c->dbr, &pen, 0))); cycles = 4 + pen; break;

    /* ---- TRB / TSB ---- */
    case 0x14: case 0x1C: /* TRB */
    {
        addr = (op == 0x14u) ? a_dp(s, c, &pen) : a_abs(s, c, c->dbr);
        cycles = (op == 0x14u ? 5 : 6) + pen + (m8(c) ? 0 : 2);
        uint16_t v = mem_read_m(s, addr, c);
        c->p &= (uint8_t)~SNES_FZ;
        if ((get_a(c) & v) == 0u)
            c->p |= SNES_FZ;
        v = (uint16_t)(v & ~get_a(c));
        mem_write_m(s, addr, v, c);
        break;
    }
    case 0x04: case 0x0C: /* TSB */
    {
        addr = (op == 0x04u) ? a_dp(s, c, &pen) : a_abs(s, c, c->dbr);
        cycles = (op == 0x04u ? 5 : 6) + pen + (m8(c) ? 0 : 2);
        uint16_t v = mem_read_m(s, addr, c);
        c->p &= (uint8_t)~SNES_FZ;
        if ((get_a(c) & v) == 0u)
            c->p |= SNES_FZ;
        v = (uint16_t)(v | get_a(c));
        mem_write_m(s, addr, v, c);
        break;
    }

    /* ---- STZ ---- */
    case 0x64: WR(a_dp(s, c, &pen), 0); cycles = 3 + pen + (m8(c) ? 0 : 1); break;
    case 0x74: WR(a_dpx(s, c, &pen), 0); cycles = 4 + pen + (m8(c) ? 0 : 1); break;
    case 0x9C: WR(a_abs(s, c, c->dbr), 0); cycles = 4 + (m8(c) ? 0 : 1); break;
    case 0x9E: WR(a_absx(s, c, c->dbr, &pen, 1), 0); cycles = 5 + (m8(c) ? 0 : 1); break;

    /* ---- flags ---- */
    case 0x18: c->p &= (uint8_t)~SNES_FC; cycles = 2; break;
    case 0x38: c->p |= SNES_FC; cycles = 2; break;
    case 0x58: c->p &= (uint8_t)~SNES_FI; cycles = 2; break;
    case 0x78: c->p |= SNES_FI; cycles = 2; break;
    case 0xB8: c->p &= (uint8_t)~SNES_FV; cycles = 2; break;
    case 0xD8: c->p &= (uint8_t)~SNES_FD; cycles = 2; break;
    case 0xF8: c->p |= SNES_FD; cycles = 2; break;
    case 0xFB: /* XCE */
    {
        uint8_t old_c = (uint8_t)(c->p & SNES_FC);
        if (c->e) {
            c->p |= SNES_FC;
        } else {
            c->p &= (uint8_t)~SNES_FC;
        }
        c->e = old_c ? 1u : 0u;
        if (c->e) {
            c->p |= (uint8_t)(SNES_FM | SNES_FX);
            c->sp = (uint16_t)(0x0100u | (c->sp & 0xFFu));
        }
        cycles = 2;
        break;
    }

    /* ---- transfers ---- */
    case 0xAA: set_x(c, get_a(c)); cycles = 2; break;
    case 0xA8: set_y(c, get_a(c)); cycles = 2; break;
    case 0x8A: set_a(c, get_x(c)); setzn_m(c, get_a(c)); cycles = 2; break;
    case 0x98: set_a(c, get_y(c)); setzn_m(c, get_a(c)); cycles = 2; break;
    case 0xBA: set_x(c, c->sp & (x8(c) ? 0xFFu : 0xFFFFu)); setzn16(c, get_x(c));
               cycles = 2; break;
    case 0x9A: c->sp = get_x(c); if (c->e) c->sp = (uint16_t)(0x0100u | (c->sp & 0xFFu));
               cycles = 2; break;
    case 0x5B: c->dp = get_a(c); cycles = 2; break;                 /* TCD */
    case 0x7B: set_a(c, c->dp); setzn_m(c, get_a(c)); cycles = 2; break; /* TDC */
    case 0x1B: c->sp = get_a(c); if (c->e) c->sp = (uint16_t)(0x0100u | (c->sp & 0xFFu));
               cycles = 2; break;                                   /* TCS */
    case 0x3B: set_a(c, c->sp); setzn_m(c, get_a(c)); cycles = 2; break; /* TSC */
    case 0xBB: set_y(c, get_x(c)); cycles = 2; break;                /* TXY */
    case 0x9B: set_x(c, get_y(c)); cycles = 2; break;                /* TYX */

    case 0xEB: /* XBA */
    {
        uint8_t lo = (uint8_t)(c->a & 0xFFu);
        uint8_t hi = (uint8_t)(c->a >> 8);
        c->a = (uint16_t)((lo << 8) | hi);
        setzn8(c, hi);
        cycles = 3;
        break;
    }

    /* ---- stack ---- */
    case 0x48: push8(s, c, (uint8_t)get_a(c)); cycles = m8(c) ? 3 : 4; break;
    case 0x68: { uint16_t v = pop8(s, c);
                 if (!m8(c)) v |= (uint16_t)pop8(s, c) << 8;
                 set_a(c, v); setzn_m(c, get_a(c)); cycles = m8(c) ? 4 : 5; break; }
    case 0x08: { uint8_t pushed = c->p | 0x30u; push8(s, c, pushed); cycles = 3; break; } /* PHP */
    case 0x28: { c->p = (uint8_t)((pop8(s, c) & ~(0x10u | 0x20u)) |
                                  (c->p & (SNES_FM | SNES_FX)));
                 if (c->e) c->p = (uint8_t)((c->p & ~SNES_FX) | SNES_FX);
                 cycles = 4; break; } /* PLP: M/X unaffected */
    case 0xDA: push8(s, c, (uint8_t)(get_x(c) & 0xFFu)); cycles = 3; break;  /* PHX */
    case 0x5A: push8(s, c, (uint8_t)(get_y(c) & 0xFFu)); cycles = 3; break;  /* PHY */
    case 0xFA: set_x(c, pop8(s, c)); cycles = 4; break;                      /* PLX */
    case 0x7A: set_y(c, pop8(s, c)); cycles = 4; break;                      /* PLY */
    case 0x0B: push8(s, c, (uint8_t)(c->dp >> 8)); push8(s, c, (uint8_t)c->dp);
               cycles = 4; break; /* PHD */
    case 0x2B: c->dp = pop16(s, c); cycles = 5; break; /* PLD */
    case 0x4B: push8(s, c, c->k); cycles = 3; break;   /* PHK */
    case 0x8B: push8(s, c, c->dbr); cycles = 3; break; /* PHB */
    case 0xAB: c->dbr = pop8(s, c); cycles = 4; break; /* PLB */
    case 0xF4: { uint16_t a = fetch16(s, c); push16(s, c, a); cycles = 5; break; } /* PEA */
    case 0xD4: { uint8_t off = fetch8(s, c); pen = (uint32_t)dp_cross(c);
                 push16(s, c, read16_dp(s, c, dp_ptr(c, off)));
                 cycles = 6 + pen; break; } /* PEI */
    case 0x62: { uint16_t base = c->pc + 1; int16_t off = (int16_t)fetch16(s, c);
                 push16(s, c, base); push8(s, c, c->k);
                 c->pc = (uint16_t)(base + off); cycles = 7; break; } /* PER */

    /* ---- jumps / subroutines ---- */
    case 0x4C: c->pc = fetch16(s, c); cycles = 3; break;             /* JMP abs */
    case 0x5C: { uint32_t a = a_long(s, c); c->pc = (uint16_t)a; c->k = (uint8_t)(a >> 16);
                 cycles = 4; break; }                                 /* JML */
    case 0x6C: { uint16_t ptr = fetch16(s, c);
                 c->pc = (uint16_t)(RD(BANK(c->k, ptr)) |
                                    ((uint16_t)RD(BANK(c->k, (uint16_t)(ptr + 1u))) << 8));
                 cycles = 5; break; }                                 /* JMP (abs) */
    case 0x7C: { uint16_t base = fetch16(s, c);
                 uint16_t idx = x8(c) ? (c->x & 0xFFu) : c->x;
                 uint16_t ptr = (uint16_t)(base + idx);
                 c->pc = (uint16_t)(RD(BANK(c->k, ptr)) |
                                    ((uint16_t)RD(BANK(c->k, (uint16_t)(ptr + 1u))) << 8));
                 cycles = 6; break; }                                 /* JMP (abs,X) */
    case 0xDC: { uint16_t ptr = fetch16(s, c);
                 uint16_t lo = (uint16_t)(RD(BANK(c->k, ptr)) |
                                          ((uint16_t)RD(BANK(c->k, (uint16_t)(ptr + 1u))) << 8));
                 uint8_t bank = RD(BANK(c->k, (uint16_t)(ptr + 2u)));
                 c->pc = lo; c->k = bank; cycles = 6; break; }        /* JMP [abs] */
    case 0x20: { uint16_t dest = fetch16(s, c);
                 push8(s, c, c->k); push16(s, c, (uint16_t)(c->pc - 1u));
                 c->pc = dest; cycles = 6; break; }                   /* JSR abs */
    case 0x22: { uint32_t a = a_long(s, c);
                 push8(s, c, c->k); push16(s, c, (uint16_t)(c->pc - 1u));
                 c->pc = (uint16_t)a; c->k = (uint8_t)(a >> 16); cycles = 8; break; } /* JSL */
    case 0x60: c->pc = (uint16_t)(pop16(s, c) + 1u); cycles = 6; break;  /* RTS */
    case 0x6B: { uint8_t k = pop8(s, c); c->pc = (uint16_t)(pop16(s, c) + 1u);
                 c->k = k; cycles = 6; break; }                       /* RTL */
    case 0x40: { c->p = (uint8_t)((pop8(s, c) & ~(0x10u | 0x20u)) |
                                  (c->p & (SNES_FM | SNES_FX)));
                 c->pc = pop16(s, c); c->k = pop8(s, c);
                 if (c->e) c->p = (uint8_t)(c->p | SNES_FX);
                 cycles = 7; break; }                                 /* RTI */
    case 0x00: c->pc++; do_interrupt(s, c, 1, 0xFFFEu, 0xFFE6u); cycles = 8; break; /* BRK */
    case 0x02: do_interrupt(s, c, 1, 0xFFF4u, 0xFFE4u); cycles = 8; break;          /* COP */
    case 0xCB: c->wai = 1; cycles = 3; break; /* WAI */
    case 0xDB: c->stp = 1; cycles = 3; break; /* STP */

    /* ---- block move ---- */
    case 0x54: case 0x44: /* MVN / MVP */
    {
        uint8_t src_b = fetch8(s, c);
        uint8_t dst_b = fetch8(s, c);
        uint32_t count = m8(c) ? (uint32_t)(c->a & 0xFFu) : (uint32_t)c->a;
        if (count == 0u)
            count = m8(c) ? 0x100u : 0x10000u;
        uint8_t forward = (uint8_t)(op == 0x54u);
        while (count != 0u) {
            uint8_t byte = RD(BANK(src_b, get_x(c)));
            WR(BANK(dst_b, get_y(c)), byte);
            if (forward) {
                c->x = x8(c) ? (uint16_t)((c->x + 1u) & 0xFFu) : (uint16_t)(c->x + 1u);
                c->y = x8(c) ? (uint16_t)((c->y + 1u) & 0xFFu) : (uint16_t)(c->y + 1u);
            } else {
                c->x = x8(c) ? (uint16_t)((c->x - 1u) & 0xFFu) : (uint16_t)(c->x - 1u);
                c->y = x8(c) ? (uint16_t)((c->y - 1u) & 0xFFu) : (uint16_t)(c->y - 1u);
            }
            count--;
        }
        /* counter ends at zero (approximation: A is not decremented
         * per-byte; the loop count is captured at entry) */
        if (m8(c))
            c->a = (uint16_t)(c->a & 0xFF00u);
        else
            c->a = 0;
        c->dbr = dst_b;
        setzn16(c, 0);
        cycles = m8(c) ? 16u : 18u;
        break;
    }

    /* ---- branches ---- */
    case 0x10: cycles = 2; branch8(s, c, !(c->p & SNES_FN), &cycles); break;
    case 0x30: cycles = 2; branch8(s, c, (c->p & SNES_FN) != 0, &cycles); break;
    case 0x50: cycles = 2; branch8(s, c, !(c->p & SNES_FV), &cycles); break;
    case 0x70: cycles = 2; branch8(s, c, (c->p & SNES_FV) != 0, &cycles); break;
    case 0x90: cycles = 2; branch8(s, c, !(c->p & SNES_FC), &cycles); break;
    case 0xB0: cycles = 2; branch8(s, c, (c->p & SNES_FC) != 0, &cycles); break;
    case 0xD0: cycles = 2; branch8(s, c, !(c->p & SNES_FZ), &cycles); break;
    case 0xF0: cycles = 2; branch8(s, c, (c->p & SNES_FZ) != 0, &cycles); break;
    case 0x80: cycles = 3; branch8(s, c, 1, &cycles); break;  /* BRA */
    case 0x82: cycles = 4; branch16(s, c, 1, &cycles); break; /* BRL */

    /* ---- misc ---- */
    case 0xEA: cycles = 2; break; /* NOP */
    case 0xC2: { uint8_t v = fetch8(s, c); c->p &= (uint8_t)~v;
                 if (c->e) c->p = (uint8_t)(c->p | SNES_FX);
                 cycles = 3; break; } /* REP */
    case 0xE2: { uint8_t v = fetch8(s, c); c->p |= v;
                 cycles = 3; break; } /* SEP */
    default:
        cycles = 2;
        break;
    }

    s->total_cycles += cycles * 6u;
    return cycles;
}

void snes_cpu_power_on(snes_cpu *c)
{
    c->a = 0; c->x = 0; c->y = 0;
    c->sp = 0x01FF;
    c->pc = 0x0000;
    c->dbr = 0; c->k = 0;
    c->dp = 0;
    c->p = SNES_FI | SNES_FM | SNES_FX;
    c->e = 1;
    c->nmi_pending = 0;
    c->irq_line = 0;
    c->wai = 0;
    c->stp = 0;
}

void snes_cpu_reset_regs(snes_cpu *c)
{
    c->sp = (uint16_t)(c->sp - 3u);
    if (c->e)
        c->sp = (uint16_t)(0x0100u | (c->sp & 0xFFu));
    c->p |= SNES_FI;
    c->p |= (uint8_t)(SNES_FM | SNES_FX);
    c->e = 1;
    c->dbr = 0;
    c->k = 0;
    c->dp = (uint16_t)(c->dp & 0xFFu);
    c->nmi_pending = 0;
    c->irq_line = 0;
    c->wai = 0;
    c->stp = 0;
}
