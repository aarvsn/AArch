/*
 * beatle-nes-redux CPU: 6502 core of the 2A03 (decimal mode disabled by
 * hardware — the D flag exists but never affects ADC/SBC; documented).
 *
 * Full official instruction set. Cycle counts are the standard 6502 tables
 * including page-cross penalties for absolute X/Y and (zp),Y loads, and
 * taken-branch / page-crossed-branch timing. Unofficial opcodes are not
 * implemented and execute as 2-cycle NOPs (documented limitation).
 *
 * Interrupt model (instruction granularity, documented approximation):
 * NMI is edge-detected (set by the PPU each step), IRQ is level (APU frame
 * / MMC3). Polling happens at instruction boundaries.
 */
#include "nes.h"

#define RD(a) nes_bus_read(n, (a))
#define WR(a, v) nes_bus_write(n, (a), (v))

static inline void set_zn(nes_cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(NES_FZ | NES_FN)) |
                     (v == 0 ? NES_FZ : 0u) | (v & 0x80u));
}

static inline void push8(nes_t *n, nes_cpu *c, uint8_t v)
{
    WR((uint16_t)(0x100u | c->s), v);
    c->s = (uint8_t)(c->s - 1);
}

static inline uint8_t pop8(nes_t *n, nes_cpu *c)
{
    c->s = (uint8_t)(c->s + 1);
    return RD((uint16_t)(0x100u | c->s));
}

static inline void push16(nes_t *n, nes_cpu *c, uint16_t v)
{
    push8(n, c, (uint8_t)(v >> 8));
    push8(n, c, (uint8_t)v);
}

static inline uint16_t pop16(nes_t *n, nes_cpu *c)
{
    uint16_t lo = pop8(n, c);
    return (uint16_t)(lo | ((uint16_t)pop8(n, c) << 8));
}

/* ---- addressing helpers (penalty in *pen) --------------------------------- */

static uint16_t a_imm(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    (void)n; (void)pen;
    return c->pc++;
}

static uint16_t a_zp(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    (void)n; (void)pen;
    return RD(c->pc++); /* zero-page address from the operand byte */
}

static uint16_t a_zpx(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    (void)n; (void)pen;
    return (uint8_t)(RD(c->pc++) + c->x);
}

static uint16_t a_zpy(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    (void)n; (void)pen;
    return (uint8_t)(RD(c->pc++) + c->y);
}

static uint16_t a_abs(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    (void)n; (void)pen;
    uint16_t lo = RD(c->pc++);
    uint16_t hi = RD(c->pc++);
    return (uint16_t)(lo | (hi << 8));
}

static uint16_t a_absx(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    uint16_t lo = RD(c->pc++);
    uint16_t hi = RD(c->pc++);
    uint16_t base = (uint16_t)(lo | (hi << 8));
    uint16_t addr = (uint16_t)(base + c->x);
    *pen += ((base & 0xFF00u) != (addr & 0xFF00u)) ? 1u : 0u;
    return addr;
}

static uint16_t a_absy(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    uint16_t lo = RD(c->pc++);
    uint16_t hi = RD(c->pc++);
    uint16_t base = (uint16_t)(lo | (hi << 8));
    uint16_t addr = (uint16_t)(base + c->y);
    *pen += ((base & 0xFF00u) != (addr & 0xFF00u)) ? 1u : 0u;
    return addr;
}

static uint16_t a_indx(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    (void)pen;
    uint8_t zp = (uint8_t)(RD(c->pc++) + c->x);
    uint16_t lo = RD(zp);
    uint16_t hi = RD((uint8_t)(zp + 1));
    return (uint16_t)(lo | (hi << 8));
}

static uint16_t a_indy(nes_t *n, nes_cpu *c, uint32_t *pen)
{
    uint8_t zp = RD(c->pc++);
    uint16_t base = (uint16_t)(RD(zp) | ((uint16_t)RD((uint8_t)(zp + 1)) << 8));
    uint16_t addr = (uint16_t)(base + c->y);
    *pen += ((base & 0xFF00u) != (addr & 0xFF00u)) ? 1u : 0u;
    return addr;
}

/* ---- operations ------------------------------------------------------------ */

static uint8_t op_adc(nes_cpu *c, uint8_t m)
{
    uint32_t sum = (uint32_t)c->a + m + ((c->p & NES_FC) ? 1u : 0u);
    uint8_t r = (uint8_t)sum;
    c->p &= (uint8_t)~(NES_FC | NES_FV);
    if (sum > 0xFFu)
        c->p |= NES_FC;
    if ((~(c->a ^ m) & (c->a ^ r) & 0x80u) != 0u)
        c->p |= NES_FV;
    c->a = r;
    set_zn(c, r);
    return r;
}

static uint8_t op_sbc(nes_cpu *c, uint8_t m)
{
    /* binary: A - M - !C == A + ~M + C */
    return op_adc(c, (uint8_t)~m);
}

static void op_asl(nes_cpu *c, uint8_t *v)
{
    c->p = (uint8_t)((c->p & ~NES_FC) | ((*v >> 7) & 1u));
    *v = (uint8_t)(*v << 1);
    set_zn(c, *v);
}

static void op_lsr(nes_cpu *c, uint8_t *v)
{
    c->p = (uint8_t)((c->p & ~NES_FC) | (*v & 1u));
    *v = (uint8_t)(*v >> 1);
    set_zn(c, *v);
}

static void op_rol(nes_cpu *c, uint8_t *v)
{
    uint8_t cin = (uint8_t)(c->p & NES_FC);
    c->p = (uint8_t)((c->p & ~NES_FC) | ((*v >> 7) & 1u));
    *v = (uint8_t)((*v << 1) | cin);
    set_zn(c, *v);
}

static void op_ror(nes_cpu *c, uint8_t *v)
{
    uint8_t cin = (uint8_t)((c->p & NES_FC) << 7);
    c->p = (uint8_t)((c->p & ~NES_FC) | (*v & 1u));
    *v = (uint8_t)((*v >> 1) | cin);
    set_zn(c, *v);
}

static void op_cmp(nes_cpu *c, uint8_t reg, uint8_t m)
{
    uint32_t diff = (uint32_t)reg - m;
    c->p &= (uint8_t)~NES_FC;
    if (reg >= m)
        c->p |= NES_FC;
    set_zn(c, (uint8_t)diff);
}

static void op_branch(nes_t *n, nes_cpu *c, int cond, uint32_t *cycles)
{
    int8_t off = (int8_t)RD(c->pc++);
    if (cond) {
        uint16_t target = (uint16_t)(c->pc + (uint16_t)(int16_t)off);
        if ((target & 0xFF00u) != (c->pc & 0xFF00u))
            *cycles += 1; /* page cross */
        *cycles += 1;     /* taken */
        c->pc = target;
    }
}

/* service pending interrupts at instruction boundary; returns cycles used */
static uint32_t service_interrupt(nes_t *n, nes_cpu *c)
{
    if (c->nmi_pending) {
        c->nmi_pending = 0;
        push16(n, c, c->pc);
        push8(n, c, (uint8_t)((c->p & ~NES_FB) | NES_FU));
        c->p |= NES_FI;
        uint16_t lo = RD(0xFFFA);
        uint16_t hi = RD(0xFFFB);
        c->pc = (uint16_t)(lo | (hi << 8));
        return 7;
    }
    if (c->irq_line && !(c->p & NES_FI)) {
        push16(n, c, c->pc);
        push8(n, c, (uint8_t)((c->p & ~NES_FB) | NES_FU));
        c->p |= NES_FI;
        uint16_t lo = RD(0xFFFE);
        uint16_t hi = RD(0xFFFF);
        c->pc = (uint16_t)(lo | (hi << 8));
        return 7;
    }
    return 0;
}

uint32_t nes_cpu_step(nes_t *n)
{
    nes_cpu *c = &n->cpu;

    /* OAM DMA stall: CPU suspended, APU/PPU keep running */
    if (c->dma_stall > 0) {
        uint32_t take = c->dma_stall > 512u ? 512u : c->dma_stall;
        c->dma_stall -= take;
        return take;
    }

    /* instruction-boundary interrupt poll */
    uint32_t icost = service_interrupt(n, c);
    if (icost > 0)
        return icost;

    uint8_t op = RD(c->pc++);
    uint32_t cycles = 2;
    uint32_t pen = 0;
    uint16_t addr;
    uint8_t m;

    switch (op) {
    /* --- loads / stores --- */
    case 0xA9: c->a = RD(a_imm(n, c, &pen)); set_zn(c, c->a); cycles = 2; break;
    case 0xA5: c->a = RD(a_zp(n, c, &pen)); set_zn(c, c->a); cycles = 3; break;
    case 0xB5: c->a = RD(a_zpx(n, c, &pen)); set_zn(c, c->a); cycles = 4; break;
    case 0xAD: c->a = RD(a_abs(n, c, &pen)); set_zn(c, c->a); cycles = 4; break;
    case 0xBD: c->a = RD(a_absx(n, c, &pen)); set_zn(c, c->a); cycles = 4 + pen; break;
    case 0xB9: c->a = RD(a_absy(n, c, &pen)); set_zn(c, c->a); cycles = 4 + pen; break;
    case 0xA1: c->a = RD(a_indx(n, c, &pen)); set_zn(c, c->a); cycles = 6; break;
    case 0xB1: c->a = RD(a_indy(n, c, &pen)); set_zn(c, c->a); cycles = 5 + pen; break;

    case 0xA2: c->x = RD(a_imm(n, c, &pen)); set_zn(c, c->x); cycles = 2; break;
    case 0xA6: c->x = RD(a_zp(n, c, &pen)); set_zn(c, c->x); cycles = 3; break;
    case 0xB6: c->x = RD(a_zpy(n, c, &pen)); set_zn(c, c->x); cycles = 4; break;
    case 0xAE: c->x = RD(a_abs(n, c, &pen)); set_zn(c, c->x); cycles = 4; break;
    case 0xBE: c->x = RD(a_absy(n, c, &pen)); set_zn(c, c->x); cycles = 4 + pen; break;

    case 0xA0: c->y = RD(a_imm(n, c, &pen)); set_zn(c, c->y); cycles = 2; break;
    case 0xA4: c->y = RD(a_zp(n, c, &pen)); set_zn(c, c->y); cycles = 3; break;
    case 0xB4: c->y = RD(a_zpx(n, c, &pen)); set_zn(c, c->y); cycles = 4; break;
    case 0xAC: c->y = RD(a_abs(n, c, &pen)); set_zn(c, c->y); cycles = 4; break;
    case 0xBC: c->y = RD(a_absx(n, c, &pen)); set_zn(c, c->y); cycles = 4 + pen; break;

    case 0x85: WR(a_zp(n, c, &pen), c->a); cycles = 3; break;
    case 0x95: WR(a_zpx(n, c, &pen), c->a); cycles = 4; break;
    case 0x8D: WR(a_abs(n, c, &pen), c->a); cycles = 4; break;
    case 0x9D: WR(a_absx(n, c, &pen), c->a); cycles = 5; break; /* always 5 */
    case 0x99: WR(a_absy(n, c, &pen), c->a); cycles = 5; break;
    case 0x81: WR(a_indx(n, c, &pen), c->a); cycles = 6; break;
    case 0x91: WR(a_indy(n, c, &pen), c->a); cycles = 6; break;

    case 0x86: WR(a_zp(n, c, &pen), c->x); cycles = 3; break;
    case 0x96: WR(a_zpy(n, c, &pen), c->x); cycles = 4; break;
    case 0x8E: WR(a_abs(n, c, &pen), c->x); cycles = 4; break;
    case 0x84: WR(a_zp(n, c, &pen), c->y); cycles = 3; break;
    case 0x94: WR(a_zpx(n, c, &pen), c->y); cycles = 4; break;
    case 0x8C: WR(a_abs(n, c, &pen), c->y); cycles = 4; break;

    /* --- ALU --- */
    case 0x69: op_adc(c, RD(a_imm(n, c, &pen))); cycles = 2; break;
    case 0x65: op_adc(c, RD(a_zp(n, c, &pen))); cycles = 3; break;
    case 0x75: op_adc(c, RD(a_zpx(n, c, &pen))); cycles = 4; break;
    case 0x6D: op_adc(c, RD(a_abs(n, c, &pen))); cycles = 4; break;
    case 0x7D: op_adc(c, RD(a_absx(n, c, &pen))); cycles = 4 + pen; break;
    case 0x79: op_adc(c, RD(a_absy(n, c, &pen))); cycles = 4 + pen; break;
    case 0x61: op_adc(c, RD(a_indx(n, c, &pen))); cycles = 6; break;
    case 0x71: op_adc(c, RD(a_indy(n, c, &pen))); cycles = 5 + pen; break;

    case 0xE9: op_sbc(c, RD(a_imm(n, c, &pen))); cycles = 2; break;
    case 0xE5: op_sbc(c, RD(a_zp(n, c, &pen))); cycles = 3; break;
    case 0xF5: op_sbc(c, RD(a_zpx(n, c, &pen))); cycles = 4; break;
    case 0xED: op_sbc(c, RD(a_abs(n, c, &pen))); cycles = 4; break;
    case 0xFD: op_sbc(c, RD(a_absx(n, c, &pen))); cycles = 4 + pen; break;
    case 0xF9: op_sbc(c, RD(a_absy(n, c, &pen))); cycles = 4 + pen; break;
    case 0xE1: op_sbc(c, RD(a_indx(n, c, &pen))); cycles = 6; break;
    case 0xF1: op_sbc(c, RD(a_indy(n, c, &pen))); cycles = 5 + pen; break;

    case 0x29: c->a &= RD(a_imm(n, c, &pen)); set_zn(c, c->a); cycles = 2; break;
    case 0x25: c->a &= RD(a_zp(n, c, &pen)); set_zn(c, c->a); cycles = 3; break;
    case 0x35: c->a &= RD(a_zpx(n, c, &pen)); set_zn(c, c->a); cycles = 4; break;
    case 0x2D: c->a &= RD(a_abs(n, c, &pen)); set_zn(c, c->a); cycles = 4; break;
    case 0x3D: c->a &= RD(a_absx(n, c, &pen)); set_zn(c, c->a); cycles = 4 + pen; break;
    case 0x39: c->a &= RD(a_absy(n, c, &pen)); set_zn(c, c->a); cycles = 4 + pen; break;
    case 0x21: c->a &= RD(a_indx(n, c, &pen)); set_zn(c, c->a); cycles = 6; break;
    case 0x31: c->a &= RD(a_indy(n, c, &pen)); set_zn(c, c->a); cycles = 5 + pen; break;

    case 0x09: c->a |= RD(a_imm(n, c, &pen)); set_zn(c, c->a); cycles = 2; break;
    case 0x05: c->a |= RD(a_zp(n, c, &pen)); set_zn(c, c->a); cycles = 3; break;
    case 0x15: c->a |= RD(a_zpx(n, c, &pen)); set_zn(c, c->a); cycles = 4; break;
    case 0x0D: c->a |= RD(a_abs(n, c, &pen)); set_zn(c, c->a); cycles = 4; break;
    case 0x1D: c->a |= RD(a_absx(n, c, &pen)); set_zn(c, c->a); cycles = 4 + pen; break;
    case 0x19: c->a |= RD(a_absy(n, c, &pen)); set_zn(c, c->a); cycles = 4 + pen; break;
    case 0x01: c->a |= RD(a_indx(n, c, &pen)); set_zn(c, c->a); cycles = 6; break;
    case 0x11: c->a |= RD(a_indy(n, c, &pen)); set_zn(c, c->a); cycles = 5 + pen; break;

    case 0x49: c->a ^= RD(a_imm(n, c, &pen)); set_zn(c, c->a); cycles = 2; break;
    case 0x45: c->a ^= RD(a_zp(n, c, &pen)); set_zn(c, c->a); cycles = 3; break;
    case 0x55: c->a ^= RD(a_zpx(n, c, &pen)); set_zn(c, c->a); cycles = 4; break;
    case 0x4D: c->a ^= RD(a_abs(n, c, &pen)); set_zn(c, c->a); cycles = 4; break;
    case 0x5D: c->a ^= RD(a_absx(n, c, &pen)); set_zn(c, c->a); cycles = 4 + pen; break;
    case 0x59: c->a ^= RD(a_absy(n, c, &pen)); set_zn(c, c->a); cycles = 4 + pen; break;
    case 0x41: c->a ^= RD(a_indx(n, c, &pen)); set_zn(c, c->a); cycles = 6; break;
    case 0x51: c->a ^= RD(a_indy(n, c, &pen)); set_zn(c, c->a); cycles = 5 + pen; break;

    case 0xC9: op_cmp(c, c->a, RD(a_imm(n, c, &pen))); cycles = 2; break;
    case 0xC5: op_cmp(c, c->a, RD(a_zp(n, c, &pen))); cycles = 3; break;
    case 0xD5: op_cmp(c, c->a, RD(a_zpx(n, c, &pen))); cycles = 4; break;
    case 0xCD: op_cmp(c, c->a, RD(a_abs(n, c, &pen))); cycles = 4; break;
    case 0xDD: op_cmp(c, c->a, RD(a_absx(n, c, &pen))); cycles = 4 + pen; break;
    case 0xD9: op_cmp(c, c->a, RD(a_absy(n, c, &pen))); cycles = 4 + pen; break;
    case 0xC1: op_cmp(c, c->a, RD(a_indx(n, c, &pen))); cycles = 6; break;
    case 0xD1: op_cmp(c, c->a, RD(a_indy(n, c, &pen))); cycles = 5 + pen; break;

    case 0xE0: op_cmp(c, c->x, RD(a_imm(n, c, &pen))); cycles = 2; break;
    case 0xE4: op_cmp(c, c->x, RD(a_zp(n, c, &pen))); cycles = 3; break;
    case 0xEC: op_cmp(c, c->x, RD(a_abs(n, c, &pen))); cycles = 4; break;
    case 0xC0: op_cmp(c, c->y, RD(a_imm(n, c, &pen))); cycles = 2; break;
    case 0xC4: op_cmp(c, c->y, RD(a_zp(n, c, &pen))); cycles = 3; break;
    case 0xCC: op_cmp(c, c->y, RD(a_abs(n, c, &pen))); cycles = 4; break;

    /* --- read-modify-write --- */
    case 0xE6: addr = a_zp(n, c, &pen); m = RD(addr); m++; WR(addr, m); set_zn(c, m); cycles = 5; break;
    case 0xF6: addr = a_zpx(n, c, &pen); m = RD(addr); m++; WR(addr, m); set_zn(c, m); cycles = 6; break;
    case 0xEE: addr = a_abs(n, c, &pen); m = RD(addr); m++; WR(addr, m); set_zn(c, m); cycles = 6; break;
    case 0xFE: addr = a_absx(n, c, &pen); m = RD(addr); m++; WR(addr, m); set_zn(c, m); cycles = 7; break;
    case 0xC6: addr = a_zp(n, c, &pen); m = RD(addr); m--; WR(addr, m); set_zn(c, m); cycles = 5; break;
    case 0xD6: addr = a_zpx(n, c, &pen); m = RD(addr); m--; WR(addr, m); set_zn(c, m); cycles = 6; break;
    case 0xCE: addr = a_abs(n, c, &pen); m = RD(addr); m--; WR(addr, m); set_zn(c, m); cycles = 6; break;
    case 0xDE: addr = a_absx(n, c, &pen); m = RD(addr); m--; WR(addr, m); set_zn(c, m); cycles = 7; break;

    case 0x0A: op_asl(c, &c->a); cycles = 2; break;
    case 0x06: addr = a_zp(n, c, &pen); m = RD(addr); op_asl(c, &m); WR(addr, m); cycles = 5; break;
    case 0x16: addr = a_zpx(n, c, &pen); m = RD(addr); op_asl(c, &m); WR(addr, m); cycles = 6; break;
    case 0x0E: addr = a_abs(n, c, &pen); m = RD(addr); op_asl(c, &m); WR(addr, m); cycles = 6; break;
    case 0x1E: addr = a_absx(n, c, &pen); m = RD(addr); op_asl(c, &m); WR(addr, m); cycles = 7; break;

    case 0x4A: op_lsr(c, &c->a); cycles = 2; break;
    case 0x46: addr = a_zp(n, c, &pen); m = RD(addr); op_lsr(c, &m); WR(addr, m); cycles = 5; break;
    case 0x56: addr = a_zpx(n, c, &pen); m = RD(addr); op_lsr(c, &m); WR(addr, m); cycles = 6; break;
    case 0x4E: addr = a_abs(n, c, &pen); m = RD(addr); op_lsr(c, &m); WR(addr, m); cycles = 6; break;
    case 0x5E: addr = a_absx(n, c, &pen); m = RD(addr); op_lsr(c, &m); WR(addr, m); cycles = 7; break;

    case 0x2A: op_rol(c, &c->a); cycles = 2; break;
    case 0x26: addr = a_zp(n, c, &pen); m = RD(addr); op_rol(c, &m); WR(addr, m); cycles = 5; break;
    case 0x36: addr = a_zpx(n, c, &pen); m = RD(addr); op_rol(c, &m); WR(addr, m); cycles = 6; break;
    case 0x2E: addr = a_abs(n, c, &pen); m = RD(addr); op_rol(c, &m); WR(addr, m); cycles = 6; break;
    case 0x3E: addr = a_absx(n, c, &pen); m = RD(addr); op_rol(c, &m); WR(addr, m); cycles = 7; break;

    case 0x6A: op_ror(c, &c->a); cycles = 2; break;
    case 0x66: addr = a_zp(n, c, &pen); m = RD(addr); op_ror(c, &m); WR(addr, m); cycles = 5; break;
    case 0x76: addr = a_zpx(n, c, &pen); m = RD(addr); op_ror(c, &m); WR(addr, m); cycles = 6; break;
    case 0x6E: addr = a_abs(n, c, &pen); m = RD(addr); op_ror(c, &m); WR(addr, m); cycles = 6; break;
    case 0x7E: addr = a_absx(n, c, &pen); m = RD(addr); op_ror(c, &m); WR(addr, m); cycles = 7; break;

    case 0xE8: c->x++; set_zn(c, c->x); cycles = 2; break;
    case 0xC8: c->y++; set_zn(c, c->y); cycles = 2; break;
    case 0xCA: c->x--; set_zn(c, c->x); cycles = 2; break;
    case 0x88: c->y--; set_zn(c, c->y); cycles = 2; break;

    case 0x24: m = RD(a_zp(n, c, &pen));
        c->p = (uint8_t)((c->p & ~(NES_FZ | NES_FV | NES_FN)) |
                         ((c->a & m) == 0 ? NES_FZ : 0u) |
                         (m & 0x40u) | (m & 0x80u)); cycles = 3; break;
    case 0x2C: m = RD(a_abs(n, c, &pen));
        c->p = (uint8_t)((c->p & ~(NES_FZ | NES_FV | NES_FN)) |
                         ((c->a & m) == 0 ? NES_FZ : 0u) |
                         (m & 0x40u) | (m & 0x80u)); cycles = 4; break;

    /* --- control flow --- */
    case 0x4C: c->pc = a_abs(n, c, &pen); cycles = 3; break;
    case 0x6C: /* JMP (ind) with page-wrap bug */
    {
        uint16_t ptr = a_abs(n, c, &pen);
        uint16_t lo = RD(ptr);
        uint16_t hi = RD((uint16_t)((ptr & 0xFF00u) | ((ptr + 1u) & 0x00FFu)));
        c->pc = (uint16_t)(lo | (hi << 8));
        cycles = 5;
        break;
    }
    case 0x20: /* JSR: pushes the address of the high operand byte */
    {
        uint16_t lo = RD(c->pc++);
        uint16_t ret = c->pc; /* pc now points at the high byte */
        push16(n, c, ret);
        uint16_t hi = RD(c->pc++);
        c->pc = (uint16_t)(lo | ((uint16_t)hi << 8));
        cycles = 6;
        break;
    }
    case 0x60: c->pc = (uint16_t)(pop16(n, c) + 1u); cycles = 6; break;
    case 0x40: /* RTI */
        c->p = (uint8_t)((pop8(n, c) & ~NES_FB) | NES_FU);
        c->pc = pop16(n, c);
        cycles = 6;
        break;
    case 0x00: /* BRK */
        c->pc++;
        push16(n, c, c->pc);
        push8(n, c, (uint8_t)(c->p | NES_FB | NES_FU));
        c->p |= NES_FI;
        {
            uint16_t lo = RD(0xFFFE);
            uint16_t hi = RD(0xFFFF);
            c->pc = (uint16_t)(lo | (hi << 8));
        }
        cycles = 7;
        break;

    case 0x10: cycles = 2; op_branch(n, c, !(c->p & NES_FN), &cycles); break;
    case 0x30: cycles = 2; op_branch(n, c, (c->p & NES_FN) != 0, &cycles); break;
    case 0x50: cycles = 2; op_branch(n, c, !(c->p & NES_FV), &cycles); break;
    case 0x70: cycles = 2; op_branch(n, c, (c->p & NES_FV) != 0, &cycles); break;
    case 0x90: cycles = 2; op_branch(n, c, !(c->p & NES_FC), &cycles); break;
    case 0xB0: cycles = 2; op_branch(n, c, (c->p & NES_FC) != 0, &cycles); break;
    case 0xD0: cycles = 2; op_branch(n, c, !(c->p & NES_FZ), &cycles); break;
    case 0xF0: cycles = 2; op_branch(n, c, (c->p & NES_FZ) != 0, &cycles); break;

    /* --- flags / transfers / stack --- */
    case 0x18: c->p &= (uint8_t)~NES_FC; cycles = 2; break;
    case 0x38: c->p |= NES_FC; cycles = 2; break;
    case 0x58: c->p &= (uint8_t)~NES_FI; cycles = 2; break;
    case 0x78: c->p |= NES_FI; cycles = 2; break;
    case 0xB8: c->p &= (uint8_t)~NES_FV; cycles = 2; break;
    case 0xD8: c->p &= (uint8_t)~NES_FD; cycles = 2; break;
    case 0xF8: c->p |= NES_FD; cycles = 2; break;
    case 0xAA: c->x = c->a; set_zn(c, c->x); cycles = 2; break;
    case 0xA8: c->y = c->a; set_zn(c, c->y); cycles = 2; break;
    case 0x8A: c->a = c->x; set_zn(c, c->a); cycles = 2; break;
    case 0x98: c->a = c->y; set_zn(c, c->a); cycles = 2; break;
    case 0xBA: c->x = c->s; set_zn(c, c->x); cycles = 2; break;
    case 0x9A: c->s = c->x; cycles = 2; break;
    case 0x48: push8(n, c, c->a); cycles = 3; break;
    case 0x68: c->a = pop8(n, c); set_zn(c, c->a); cycles = 4; break;
    case 0x08: push8(n, c, (uint8_t)(c->p | NES_FB | NES_FU)); cycles = 3; break;
    case 0x28: c->p = (uint8_t)((pop8(n, c) & ~NES_FB) | NES_FU); cycles = 4; break;

    case 0xEA: cycles = 2; break;
    default:
        /* unofficial/unimplemented: 2-cycle NOP (documented) */
        cycles = 2;
        break;
    }

    n->total_cycles += cycles;
    return cycles;
}

void nes_cpu_power_on(nes_cpu *c)
{
    /* documented 2A03 power-on approximations; exact values are not
     * architecture-defined */
    c->a = 0x00;
    c->x = 0x00;
    c->y = 0x00;
    c->s = 0xFD;
    c->p = NES_FI | NES_FU;
    c->pc = 0xFFFC;
    c->nmi_pending = 0;
    c->irq_line = 0;
    c->dma_stall = 0;
}

void nes_cpu_reset_regs(nes_cpu *c)
{
    c->s = (uint8_t)(c->s - 3u);
    c->p |= NES_FI;
    c->nmi_pending = 0;
    c->irq_line = 0;
    c->dma_stall = 0;
    /* pc = reset vector, fetched by the caller via the bus */
}
