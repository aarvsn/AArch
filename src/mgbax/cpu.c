/*
 * mgbax CPU: ARM7TDMI (ARMv4T) — ARM and Thumb instruction sets.
 *
 * Spec-driven details implemented:
 *  - full condition-code table; conditional execution of every instruction
 *  - barrel shifter with carry-out semantics (LSL/LSR/ASR/ROR/RRX, register
 *    and immediate shifts, shift-by-32+ values, ROR-by-0 = RRX)
 *  - ADC/SBC carry-in semantics (C = !borrow for SBC), CMN/CMP/TST/TEQ
 *  - PC pipeline reads: +8 ARM, +4 Thumb; PC as Rm with register-specified
 *    shift reads +12 (documented ARM7TDMI quirk)
 *  - MUL/MLA/UMULL/UMLAL/SMULL/SMLAL (S variants leave C unchanged: the
 *    ARMv4 "undefined" choice, documented)
 *  - SWP/SWPB, halfword transfers (LDRH/STRH/LDRSB/LDRSH), LDR/STR with
 *    pre/post-index + writeback, unaligned word loads rotate right
 *  - LDM/STM (IA/IB/DA/DB), writeback, PC write semantics
 *  - BX interworking, MRS/MSR with field masks and bank switching,
 *    banked registers incl. FIQ r8-r12
 *  - SWI routed to the HLE BIOS layer; exceptions via banked modes
 *
 * Cycle counts: flat deterministic model documented per class (not
 * per-bus-cycle accurate).
 */
#include "gba.h"

#include <string.h>

/* ---- banked registers ------------------------------------------------------- */

static int bank_index(uint8_t mode)
{
    switch (mode) {
    case GBA_MODE_FIQ: return 1;
    case GBA_MODE_IRQ: return 2;
    case GBA_MODE_SVC: return 3;
    case GBA_MODE_ABT: return 4;
    case GBA_MODE_UND: return 5;
    default: return 0; /* usr/sys */
    }
}

void gba_cpu_set_mode(gba_cpu *c, uint8_t new_mode)
{
    int old_b = bank_index(c->mode);
    int new_b = bank_index(new_mode);
    c->mode = new_mode;
    c->cpsr = (c->cpsr & ~0x1Fu) | (new_mode & 0x1Fu);
    if (old_b == new_b)
        return;
    c->bank_r13[old_b] = c->r[13];
    c->bank_r14[old_b] = c->r[14];
    c->spsr[old_b] = c->spsr[6]; /* current SPSR cache */
    if (old_b == 1)
        memcpy(c->bank_r8_fiq, &c->r[8], 5 * sizeof(uint32_t));
    c->r[13] = c->bank_r13[new_b];
    c->r[14] = c->bank_r14[new_b];
    c->spsr[6] = c->spsr[new_b];
    if (new_b == 1)
        memcpy(&c->r[8], c->bank_r8_fiq, 5 * sizeof(uint32_t));
}

uint32_t gba_cpu_read_spsr(const gba_cpu *c)
{
    return c->spsr[6]; /* user mode: SPSR unavailable; reads cached value */
}

void gba_cpu_write_spsr(gba_cpu *c, uint32_t v)
{
    c->spsr[6] = v;
}

void gba_cpu_exception(gba_t *g, uint32_t vector, uint8_t new_mode,
                       uint32_t lr_offset, int save_spsr)
{
    gba_cpu *c = &g->cpu;
    uint32_t old_cpsr = c->cpsr;
    uint32_t lr = c->r[15] + lr_offset;
    gba_cpu_set_mode(c, new_mode);
    if (save_spsr)
        c->spsr[6] = old_cpsr;
    c->r[14] = lr;
    c->cpsr |= GBA_I;
    c->cpsr &= (uint32_t)~GBA_T;
    c->r[15] = vector & ~3u;
}

void gba_cpu_power_on(gba_cpu *c)
{
    memset(c, 0, sizeof *c);
    c->mode = GBA_MODE_SVC;
    c->cpsr = GBA_I | GBA_F | GBA_MODE_SVC;
    /* BIOS-equivalent stack defaults (documented): SP_svc = $03007FE0,
     * SP_irq = $03007FA0, SP_sys/usr = $03007F00. Direct boot starts in
     * SVC mode, so r13 mirrors the SVC bank. */
    c->bank_r13[0] = 0x03007F00u;
    c->bank_r13[2] = 0x03007FA0u;
    c->bank_r13[3] = 0x03007FE0u;
    c->r[13] = 0x03007FE0u;
    c->r[15] = 0x08000000u;
}

/* ---- conditions ---------------------------------------------------------------- */

static int cond_ok(uint32_t cond, uint32_t cpsr)
{
    uint32_t n = cpsr & GBA_N, z = cpsr & GBA_Z, cc = cpsr & GBA_C, v = cpsr & GBA_V;
    switch (cond & 0xFu) {
    case 0x0: return z != 0;
    case 0x1: return z == 0;
    case 0x2: return cc != 0;
    case 0x3: return cc == 0;
    case 0x4: return n != 0;
    case 0x5: return n == 0;
    case 0x6: return v != 0;
    case 0x7: return v == 0;
    case 0x8: return cc != 0 && z == 0;
    case 0x9: return cc == 0 || z != 0;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return z == 0 && n == v;
    case 0xD: return z != 0 || n != v;
    default: return 1; /* AL (and NV treated as AL for milestone) */
    }
}

/* ---- barrel shifter -------------------------------------------------------------- */

typedef struct {
    uint32_t value;
    uint8_t carry_out;
} shift_result;

static shift_result shift_calc(uint32_t value, uint8_t type, uint32_t amount,
                               uint8_t carry_in)
{
    shift_result r;
    r.value = value;
    r.carry_out = carry_in;
    switch (type) {
    case 0: /* LSL */
        if (amount == 0u)
            break;
        if (amount < 32u) {
            r.carry_out = (uint8_t)((value >> (32u - amount)) & 1u);
            r.value = value << amount;
        } else if (amount == 32u) {
            r.carry_out = (uint8_t)(value & 1u);
            r.value = 0;
        } else {
            r.carry_out = 0;
            r.value = 0;
        }
        break;
    case 1: /* LSR */
        if (amount == 0u)
            amount = 32; /* register form LSR #0 */
        if (amount < 32u) {
            r.carry_out = (uint8_t)((value >> (amount - 1u)) & 1u);
            r.value = value >> amount;
        } else if (amount == 32u) {
            r.carry_out = (uint8_t)((value >> 31) & 1u);
            r.value = 0;
        } else {
            r.carry_out = 0;
            r.value = 0;
        }
        break;
    case 2: /* ASR */
        if (amount == 0u || amount >= 32u) {
            r.carry_out = (uint8_t)((value >> 31) & 1u);
            r.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
            break;
        }
        r.carry_out = (uint8_t)((value >> (amount - 1u)) & 1u);
        r.value = (uint32_t)((int32_t)value >> amount);
        break;
    default: /* ROR */
        if (amount == 0u) { /* RRX */
            r.carry_out = (uint8_t)(value & 1u);
            r.value = (value >> 1) | (carry_in ? 0x80000000u : 0u);
            break;
        }
        amount &= 31u;
        if (amount == 0u) {
            r.carry_out = (uint8_t)((value >> 31) & 1u);
            r.value = value;
        } else {
            r.carry_out = (uint8_t)((value >> (amount - 1u)) & 1u);
            r.value = (value >> amount) | (value << (32u - amount));
        }
        break;
    }
    return r;
}

static inline uint8_t cpsr_c(const gba_cpu *c)
{
    return (uint8_t)((c->cpsr & GBA_C) ? 1 : 0);
}

/* decode ARM operand2 (register form). PC reads: +8 for immediate shifts,
 * +12 for register-specified shifts (ARM7TDMI pipeline; c->r[15] currently
 * holds instruction address + 4, so +4/+8 on top of that). */
static shift_result arm_operand2(gba_cpu *c, uint32_t instr)
{
    uint8_t type = (uint8_t)((instr >> 5) & 3u);
    uint8_t rm = (uint8_t)(instr & 0xFu);
    if (instr & (1u << 4)) { /* register-specified shift */
        uint32_t amount = c->r[(instr >> 8) & 0xFu] & 0xFFu;
        uint32_t value = (rm == 15u) ? (c->r[15] + 8u) : c->r[rm];
        if (amount == 0u) { /* shift by 0: unchanged, C unaffected */
            shift_result r;
            r.value = value;
            r.carry_out = cpsr_c(c);
            return r;
        }
        return shift_calc(value, type, amount, cpsr_c(c));
    }
    uint32_t amount = (instr >> 7) & 0x1Fu;
    uint32_t value = (rm == 15u) ? (c->r[15] + 4u) : c->r[rm];
    if (amount != 0u)
        return shift_calc(value, type, amount, cpsr_c(c));
    /* shift-by-zero immediates */
    shift_result r;
    if (type == 0u) {
        r.value = value;
        r.carry_out = cpsr_c(c);
    } else if (type == 3u) { /* ROR #0 = RRX */
        r.carry_out = (uint8_t)(value & 1u);
        r.value = (value >> 1) | (cpsr_c(c) ? 0x80000000u : 0u);
    } else if (type == 1u) { /* LSR #32 */
        r.carry_out = (uint8_t)((value >> 31) & 1u);
        r.value = 0;
    } else { /* ASR #32 */
        r.carry_out = (uint8_t)((value >> 31) & 1u);
        r.value = (value & 0x80000000u) ? 0xFFFFFFFFu : 0u;
    }
    return r;
}

/* immediate operand2 with rotation; carry = bit rotated out */
static uint32_t arm_imm(gba_cpu *c, uint32_t instr, uint8_t *carry_out)
{
    uint32_t imm = instr & 0xFFu;
    uint32_t rot = ((instr >> 8) & 0xFu) * 2u;
    if (rot == 0u) {
        *carry_out = cpsr_c(c);
        return imm;
    }
    uint32_t v = (imm >> rot) | (imm << (32u - rot));
    *carry_out = (uint8_t)((v >> 31) & 1u);
    return v;
}

/* ---- ALU ------------------------------------------------------------------------ */

static inline void set_nz32(gba_cpu *c, uint32_t v)
{
    c->cpsr &= (uint32_t)~(GBA_N | GBA_Z);
    if (v == 0u)
        c->cpsr |= GBA_Z;
    if (v & 0x80000000u)
        c->cpsr |= GBA_N;
}

static inline uint32_t add_with_carry(gba_cpu *c, uint32_t a, uint32_t b,
                                      uint32_t carry_in, int set_flags)
{
    uint64_t sum = (uint64_t)a + b + carry_in;
    uint32_t r = (uint32_t)sum;
    if (set_flags) {
        c->cpsr &= (uint32_t)~(GBA_N | GBA_Z | GBA_C | GBA_V);
        if (sum > 0xFFFFFFFFu)
            c->cpsr |= GBA_C;
        if (~(a ^ b) & (a ^ r) & 0x80000000u)
            c->cpsr |= GBA_V;
        set_nz32(c, r);
    }
    return r;
}

static inline uint32_t sub_with_carry(gba_cpu *c, uint32_t a, uint32_t b,
                                      uint32_t borrow_in, int set_flags)
{
    uint32_t r = a - b - borrow_in;
    if (set_flags) {
        /* 64-bit compare: b + borrow_in can wrap when b == 0xFFFFFFFF */
        uint64_t rhs = (uint64_t)b + borrow_in;
        c->cpsr &= (uint32_t)~(GBA_N | GBA_Z | GBA_C | GBA_V);
        if ((uint64_t)a >= rhs)
            c->cpsr |= GBA_C;
        if ((a ^ b) & (a ^ r) & 0x80000000u)
            c->cpsr |= GBA_V;
        set_nz32(c, r);
    }
    return r;
}

static inline uint32_t get_reg(gba_cpu *c, uint8_t r, uint32_t extra_pc_offset)
{
    return (r == 15u) ? (c->r[15] + extra_pc_offset) : c->r[r];
}

static void do_msr(gba_cpu *c, uint32_t instr, uint32_t value)
{
    int use_spsr = (instr & (1u << 22)) != 0;
    uint32_t mask = 0;
    if (instr & (1u << 19)) mask |= 0xFF000000u;
    if (instr & (1u << 18)) mask |= 0x00FF0000u;
    if (instr & (1u << 17)) mask |= 0x0000FF00u;
    if (instr & (1u << 16)) mask |= 0x000000FFu;
    if (use_spsr) {
        uint32_t spsr = gba_cpu_read_spsr(c);
        gba_cpu_write_spsr(c, (spsr & ~mask) | (value & mask));
        return;
    }
    if (c->mode == GBA_MODE_USR)
        mask &= 0xFF000000u; /* user mode: flags only */
    uint32_t new_cpsr = (c->cpsr & ~mask) | (value & mask);
    uint8_t new_mode = (uint8_t)(new_cpsr & 0x1Fu);
    c->cpsr = new_cpsr;
    gba_cpu_set_mode(c, new_mode);
}

/* ---- ARM step ----------------------------------------------------------------------
 *
 * PC convention: gba_cpu_step has ALREADY advanced r15 past the fetched
 * instruction (r15 = instruction address + 4 here). Sequential handlers
 * must NOT advance r15 again; control-flow handlers assign r15 directly.
 * PC reads follow the ARM7 pipeline: r15+4 (instr+8) normally, r15+8
 * (instr+12) for register-specified shifts. */
static uint32_t arm_step(gba_t *g, gba_cpu *c, uint32_t instr)
{
    if (!cond_ok(instr >> 28, c->cpsr)) {
        return 1;
    }

    /* SWI: HLE */
    if ((instr & 0x0F000000u) == 0x0F000000u) {
        uint32_t comment = instr & 0xFFu;
        gba_swi_hle(g, comment);
        return 3;
    }

    /* BX */
    if ((instr & 0x0FFFFFF0u) == 0x012FFF10u) {
        uint32_t target = c->r[instr & 0xFu];
        c->cpsr = (c->cpsr & (uint32_t)~GBA_T) | (target & 1u ? GBA_T : 0u);
        c->r[15] = target & ~1u;
        return 3;
    }

    /* multiply family: MUL/MLA have bits[27:22] = 000000; the 64-bit
     * MULL family has bits[27:23] = 00001 with U=bit22, A=bit21 */
    if ((instr & 0x0FC000F0u) == 0x00000090u ||
        (instr & 0x0F8000F0u) == 0x00800090u) {
        uint8_t rd = (uint8_t)((instr >> 16) & 0xFu);
        uint8_t rn = (uint8_t)((instr >> 12) & 0xFu);
        uint8_t rs = (uint8_t)((instr >> 8) & 0xFu);
        uint8_t rm = (uint8_t)(instr & 0xFu);
        int set_flags = (instr & (1u << 20)) != 0;
        int accum = (instr & (1u << 21)) != 0;
        int is64 = (instr & (1u << 23)) != 0;
        int signed_mul = (instr & (1u << 22)) != 0;
        uint32_t m = c->r[rm], sv = c->r[rs];
        if (is64) {
            uint64_t result;
            if (signed_mul) {
                int64_t sm = (int64_t)(int32_t)m * (int64_t)(int32_t)sv;
                if (accum)
                    sm += (int64_t)((uint64_t)c->r[rn] | ((uint64_t)c->r[rd] << 32));
                result = (uint64_t)sm;
            } else {
                uint64_t um = (uint64_t)m * sv;
                if (accum)
                    um += (uint64_t)c->r[rn] | ((uint64_t)c->r[rd] << 32);
                result = um;
            }
            c->r[rn] = (uint32_t)result;
            c->r[rd] = (uint32_t)(result >> 32);
            if (set_flags) {
                c->cpsr &= (uint32_t)~(GBA_N | GBA_Z);
                if (result == 0u)
                    c->cpsr |= GBA_Z;
                if (result & 0x8000000000000000ull)
                    c->cpsr |= GBA_N;
            }
        } else {
            uint32_t result = accum ? m * sv + c->r[rn] : m * sv;
            c->r[rd] = result;
            if (set_flags)
                set_nz32(c, result);
        }
        return 4;
    }

    /* MRS */
    if ((instr & 0x0FBF0FFFu) == 0x010F0000u) {
        uint8_t rd = (uint8_t)((instr >> 12) & 0xFu);
        c->r[rd] = (instr & (1u << 22)) ? gba_cpu_read_spsr(c) : c->cpsr;
        return 2;
    }

    /* MSR (immediate and register) */
    if ((instr & 0x0DB0F000u) == 0x0120F000u) {
        uint32_t value;
        if (instr & (1u << 25)) {
            uint8_t carry;
            value = arm_imm(c, instr, &carry);
        } else {
            value = c->r[instr & 0xFu];
        }
        do_msr(c, instr, value);
        return 2;
    }

    /* SWP/SWPB */
    if ((instr & 0x0FB00FF0u) == 0x01000090u) {
        uint8_t rn = (uint8_t)((instr >> 16) & 0xFu);
        uint8_t rd = (uint8_t)((instr >> 12) & 0xFu);
        uint8_t rm = (uint8_t)(instr & 0xFu);
        uint32_t addr = c->r[rn];
        if (instr & (1u << 22)) {
            uint8_t tmp = gba_mem_read8(g, addr);
            gba_mem_write8(g, addr, (uint8_t)(c->r[rm] & 0xFFu));
            c->r[rd] = tmp;
        } else {
            uint32_t tmp = gba_bus_read32(g, addr);
            gba_mem_write32(g, addr, c->r[rm]);
            c->r[rd] = tmp;
        }
        return 4;
    }

    /* halfword / signed byte transfers (bits 4 and 7 set) */
    if ((instr & 0x0E000090u) == 0x00000090u && (instr & 0x60u) != 0u) {
        uint8_t rn = (uint8_t)((instr >> 16) & 0xFu);
        uint8_t rd = (uint8_t)((instr >> 12) & 0xFu);
        int up = (instr & (1u << 23)) != 0;
        int pre = (instr & (1u << 24)) != 0;
        int writeback = (instr & (1u << 21)) != 0;
        int load = (instr & (1u << 20)) != 0;
        uint32_t offset = (instr & (1u << 22))
                              ? ((uint32_t)instr & 0xFu) |
                                    ((uint32_t)(instr >> 4) & 0xF0u)
                              : c->r[instr & 0xFu];
        uint32_t base = get_reg(c, rn, 0);
        uint32_t addr = up ? base + offset : base - offset;
        uint8_t kind = (uint8_t)((instr >> 5) & 3u);
        if (pre) {
            if (!load) {
                if (kind == 1u)
                    gba_mem_write16(g, addr, (uint16_t)c->r[rd]);
            } else if (kind == 1u) {
                c->r[rd] = gba_bus_read16(g, addr);
            } else if (kind == 2u) {
                c->r[rd] = (uint32_t)(int32_t)(int8_t)gba_mem_read8(g, addr);
            } else {
                c->r[rd] = (uint32_t)(int32_t)(int16_t)gba_bus_read16(g, addr);
            }
            if (writeback && (!load || rd != rn))
                c->r[rn] = addr;
        } else {
            if (!load) {
                if (kind == 1u)
                    gba_mem_write16(g, base, (uint16_t)c->r[rd]);
            } else if (kind == 1u) {
                c->r[rd] = gba_bus_read16(g, base);
            } else if (kind == 2u) {
                c->r[rd] = (uint32_t)(int32_t)(int8_t)gba_mem_read8(g, base);
            } else {
                c->r[rd] = (uint32_t)(int32_t)(int16_t)gba_bus_read16(g, base);
            }
            c->r[rn] = addr;
        }
        return load ? 3u : 2u;
    }

    /* single data transfer */
    if ((instr & 0x0C000000u) == 0x04000000u) {
        uint8_t rn = (uint8_t)((instr >> 16) & 0xFu);
        uint8_t rd = (uint8_t)((instr >> 12) & 0xFu);
        int up = (instr & (1u << 23)) != 0;
        int pre = (instr & (1u << 24)) != 0;
        int writeback = (instr & (1u << 21)) != 0;
        int load = (instr & (1u << 20)) != 0;
        int byte = (instr & (1u << 22)) != 0;
        /* Rn == PC reads instruction address + 8 (r15 is instr + 4 here) */
        uint32_t base = get_reg(c, rn, 4);
        uint32_t offset;
        if (instr & (1u << 25)) {
            shift_result sr = arm_operand2(c, instr);
            offset = sr.value;
        } else {
            offset = instr & 0xFFFu;
        }
        uint32_t addr = up ? base + offset : base - offset;
        if (pre) {
            if (load) {
                if (byte) {
                    c->r[rd] = gba_mem_read8(g, addr);
                } else {
                    uint32_t v = gba_bus_read32(g, addr);
                    uint32_t un = addr & 3u;
                    if (un)
                        v = (v >> (un * 8u)) | (v << (32u - un * 8u));
                    if (rd == 15u)
                        c->r[15] = v & ~3u;
                    else
                        c->r[rd] = v;
                }
            } else if (byte) {
                gba_mem_write8(g, addr, (uint8_t)c->r[rd]);
            } else {
                gba_mem_write32(g, addr, c->r[rd]);
            }
            if (writeback && (!load || rd != rn))
                c->r[rn] = addr;
        } else {
            if (load) {
                if (byte) {
                    c->r[rd] = gba_mem_read8(g, base);
                } else {
                    uint32_t v = gba_bus_read32(g, base);
                    uint32_t un = base & 3u;
                    if (un)
                        v = (v >> (un * 8u)) | (v << (32u - un * 8u));
                    if (rd == 15u)
                        c->r[15] = v & ~3u;
                    else
                        c->r[rd] = v;
                }
            } else if (byte) {
                gba_mem_write8(g, base, (uint8_t)c->r[rd]);
            } else {
                gba_mem_write32(g, base, c->r[rd]);
            }
            c->r[rn] = addr;
        }
        if (load && rd == 15u)
            c->r[15] &= ~3u; /* word-aligned PC (also covers byte loads) */
        return load ? 3u : 2u;
    }

    /* block transfer */
    if ((instr & 0x0E000000u) == 0x08000000u) {
        uint8_t rn = (uint8_t)((instr >> 16) & 0xFu);
        int up = (instr & (1u << 23)) != 0;
        int pre = (instr & (1u << 24)) != 0;
        int writeback = (instr & (1u << 21)) != 0;
        int load = (instr & (1u << 20)) != 0;
        uint32_t list = instr & 0xFFFFu;
        int count = 0;
        for (int i = 0; i < 16; i++)
            if (list & (1u << i))
                count++;
        uint32_t base = c->r[rn];
        uint32_t addr;
        /* DB: first transfer at base - count*4 (ascending to base-4)
         * DA: first transfer at base - (count-1)*4 (ascending to base) */
        if (up)
            addr = pre ? base + 4u : base;
        else
            addr = base - (uint32_t)count * 4u + (pre ? 0u : 4u);
        uint32_t loaded_pc = 0;
        int has_pc = (list & (1u << 15)) != 0;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i)))
                continue;
            if (load) {
                uint32_t v = gba_bus_read32(g, addr);
                if (i == 15)
                    loaded_pc = v;
                else
                    c->r[i] = v;
            } else {
                uint32_t v = (i == 15) ? c->r[15] + 4u : c->r[i];
                gba_mem_write32(g, addr, v);
            }
            addr += 4u;
        }
        if (load && has_pc) {
            /* ARMv4T: LDM with PC does NOT interwork; bit0 ignored, T kept */
            c->r[15] = loaded_pc & ~3u;
            return (uint32_t)(4u + (uint32_t)count);
        }
        if (writeback && !(load && (list & (1u << rn)) != 0u))
            c->r[rn] = up ? base + (uint32_t)count * 4u
                          : base - (uint32_t)count * 4u;
        return (uint32_t)(2u + (uint32_t)count);
    }

    /* branch / branch with link */
    if ((instr & 0x0E000000u) == 0x0A000000u) {
        /* sign-extend the 24-bit offset: shift left as unsigned (no UB),
         * then arithmetic shift right */
        int32_t off = (int32_t)((instr & 0x00FFFFFFu) << 8) >> 8;
        int link = (instr & (1u << 24)) != 0;
        if (link)
            c->r[14] = c->r[15]; /* LR = instruction address + 4 */
        c->r[15] = c->r[15] + 4u + ((uint32_t)off << 2u);
        return 3;
    }

    /* coprocessor space: UND on GBA */
    if ((instr & 0x0C000000u) == 0x0C000000u) {
        gba_cpu_exception(g, 0x04u, GBA_MODE_UND, 4u, 1);
        return 4;
    }

    /* data processing */
    {
        uint8_t opcode = (uint8_t)((instr >> 21) & 0xFu);
        uint8_t rn = (uint8_t)((instr >> 16) & 0xFu);
        uint8_t rd = (uint8_t)((instr >> 12) & 0xFu);
        int set_flags = (instr & (1u << 20)) != 0;
        uint32_t a = get_reg(c, rn, 4);
        uint32_t b;
        uint8_t shifter_carry = cpsr_c(c);
        if (instr & (1u << 25))
            b = arm_imm(c, instr, &shifter_carry);
        else {
            shift_result sr = arm_operand2(c, instr);
            b = sr.value;
            shifter_carry = sr.carry_out;
        }
        uint32_t result = 0;
        int write = 1;
        switch (opcode) {
        case 0x0: result = a & b; if (set_flags) set_nz32(c, result); break;
        case 0x1: result = a ^ b; if (set_flags) set_nz32(c, result); break;
        case 0x2: result = sub_with_carry(c, a, b, 0, set_flags); break;
        case 0x3: result = sub_with_carry(c, b, a, 0, set_flags); break;
        case 0x4: result = add_with_carry(c, a, b, 0, set_flags); break;
        case 0x5: result = add_with_carry(c, a, b, cpsr_c(c), set_flags); break;
        case 0x6: result = sub_with_carry(c, a, b, 1u - cpsr_c(c), set_flags); break;
        case 0x7: result = sub_with_carry(c, b, a, 1u - cpsr_c(c), set_flags); break;
        case 0x8: result = a & b; write = 0;
                  if (set_flags) { set_nz32(c, result); } break;
        case 0x9: result = a ^ b; write = 0;
                  if (set_flags) { set_nz32(c, result); } break;
        case 0xA: result = sub_with_carry(c, a, b, 0, set_flags); write = 0; break;
        case 0xB: result = add_with_carry(c, a, b, 0, set_flags); write = 0; break;
        case 0xC: result = a | b; if (set_flags) set_nz32(c, result); break;
        case 0xD: result = b; if (set_flags) set_nz32(c, result); break;
        case 0xE: result = a & ~b; if (set_flags) set_nz32(c, result); break;
        default: result = ~b; if (set_flags) set_nz32(c, result); break;
        }
        if (set_flags && (opcode <= 1u || opcode == 8u || opcode == 9u ||
                          opcode >= 12u)) {
            /* logic operations: C from the shifter */
            c->cpsr = (c->cpsr & (uint32_t)~GBA_C) |
                      (shifter_carry ? GBA_C : 0u);
        }
        if (write) {
            if (rd == 15u) {
                if (set_flags) {
                    uint32_t spsr = gba_cpu_read_spsr(c);
                    c->cpsr = spsr;
                    gba_cpu_set_mode(c, (uint8_t)(spsr & 0x1Fu));
                    c->r[15] = result;
                } else {
                    c->r[15] = result & ~3u;
                }
                return 4;
            }
            c->r[rd] = result;
        }
        return 2;
    }
}

/* ---- Thumb ------------------------------------------------------------------
 *
 * Same PC convention: r15 already points past the fetched halfword.
 * Sequential handlers do not advance r15; branches assign it. */
static uint32_t thumb_step(gba_t *g, gba_cpu *c, uint16_t instr)
{
    uint32_t op = instr >> 11;

    /* shifted/added: 000 */
    if (op < 3u) {
        uint8_t rd = (uint8_t)(instr & 7u);
        uint8_t rm = (uint8_t)((instr >> 3) & 7u);
        uint32_t amount = (uint32_t)(instr >> 6) & 0x1Fu;
        uint32_t value = c->r[rm];
        shift_result r;
        if (op == 0u && amount == 0u) {
            r.value = value;
            r.carry_out = cpsr_c(c);
        } else {
            r = shift_calc(value, (uint8_t)op, amount, cpsr_c(c));
        }
        c->r[rd] = r.value;
        c->cpsr = (c->cpsr & (uint32_t)~GBA_C) | (r.carry_out ? GBA_C : 0u);
        set_nz32(c, r.value);
        return 2;
    }

    /* add/sub: 00011. Format 2: I=bit10, op=bit9, Rm bits[8:6],
     * Rn bits[5:3], Rd bits[2:0]. */
    if (op == 3u) {
        uint8_t rm = (uint8_t)((instr >> 6) & 7u);
        uint8_t rn = (uint8_t)((instr >> 3) & 7u);
        uint8_t rd = (uint8_t)(instr & 7u);
        int imm_form = (instr & (1u << 10)) != 0;
        int is_sub = (instr & (1u << 9)) != 0;
        uint32_t a = c->r[rn];
        uint32_t b = imm_form ? ((instr >> 6) & 7u) : c->r[rm];
        c->r[rd] = is_sub ? sub_with_carry(c, a, b, 0, 1)
                          : add_with_carry(c, a, b, 0, 1);
        return 2;
    }

    /* MOV/CMP/ADD/SUB immediate: 001 */
    if (op >= 4u && op <= 7u) {
        uint8_t rd = (uint8_t)((instr >> 8) & 7u);
        uint32_t imm = instr & 0xFFu;
        if (op == 4u) {
            c->r[rd] = imm;
            set_nz32(c, imm);
        } else if (op == 5u) {
            sub_with_carry(c, c->r[rd], imm, 0, 1);
        } else if (op == 6u) {
            c->r[rd] = add_with_carry(c, c->r[rd], imm, 0, 1);
        } else {
            c->r[rd] = sub_with_carry(c, c->r[rd], imm, 0, 1);
        }
        return 2;
    }

    /* ALU operations: 010000xxxx. Format 4: opcode bits[9:6], Rm bits[5:3],
     * Rd bits[2:0]. */
    if (instr >= 0x4000u && instr < 0x4400u) {
        uint8_t op2 = (uint8_t)((instr >> 6) & 0xFu);
        uint8_t rm = (uint8_t)((instr >> 3) & 7u);
        uint8_t rd = (uint8_t)(instr & 7u);
        uint32_t a = c->r[rd], b = c->r[rm];
        switch (op2) {
        case 0x0: c->r[rd] = a & b; set_nz32(c, c->r[rd]); break;
        case 0x1: c->r[rd] = a ^ b; set_nz32(c, c->r[rd]); break;
        case 0x2: case 0x3: case 0x4: case 0x7: {
            uint8_t type = (uint8_t)(op2 == 0x2u ? 0 : op2 == 0x3u ? 1
                                             : op2 == 0x4u ? 2 : 3);
            shift_result r = shift_calc(a, type, b & 0xFFu, cpsr_c(c));
            c->r[rd] = r.value;
            c->cpsr = (c->cpsr & (uint32_t)~GBA_C) | (r.carry_out ? GBA_C : 0u);
            set_nz32(c, r.value);
            break;
        }
        case 0x5: c->r[rd] = add_with_carry(c, a, b, cpsr_c(c), 1); break;
        case 0x6: c->r[rd] = sub_with_carry(c, a, b, 1u - cpsr_c(c), 1); break;
        case 0x8: set_nz32(c, a & b); break;
        case 0x9: c->r[rd] = sub_with_carry(c, 0, b, 0, 1); break;
        case 0xA: sub_with_carry(c, a, b, 0, 1); break;
        case 0xB: add_with_carry(c, a, b, 0, 1); break;
        case 0xC: c->r[rd] = a | b; set_nz32(c, c->r[rd]); break;
        case 0xD: c->r[rd] = a * b; set_nz32(c, c->r[rd]); break;
        case 0xE: c->r[rd] = a & ~b; set_nz32(c, c->r[rd]); break;
        default: c->r[rd] = ~b; set_nz32(c, c->r[rd]); break;
        }
        return 2;
    }

    /* PC-relative load: 01001. PC reads instruction address + 4 (r15 is
     * instr + 2 here), word-aligned. */
    if ((instr & 0xF800u) == 0x4800u) {
        uint8_t rd = (uint8_t)((instr >> 8) & 7u);
        uint32_t addr = ((c->r[15] + 2u) & ~3u) + (uint32_t)(instr & 0xFFu) * 4u;
        c->r[rd] = gba_bus_read32(g, addr);
        return 3;
    }

    /* Hi register ops / BX: 010001 (0x4400-0x47FF) */
    if ((instr & 0xFC00u) == 0x4400u) {
        uint8_t op2 = (uint8_t)((instr >> 8) & 3u);
        uint8_t rm = (uint8_t)((instr >> 3) & 0xFu);
        uint8_t rd = (uint8_t)((instr & 7u) | ((instr >> 4) & 8u));
        uint32_t b = c->r[rm];
        if (op2 == 3u) { /* BX */
            uint32_t target = c->r[rm];
            c->cpsr = (c->cpsr & (uint32_t)~GBA_T) | (target & 1u ? GBA_T : 0u);
            c->r[15] = target & ~1u;
            return 3;
        }
        if (op2 == 0u) { /* ADD */
            uint32_t r = c->r[rd] + b;
            if (rd == 15u)
                c->r[15] = r & ~1u;
            else
                c->r[rd] = r;
        } else if (op2 == 1u) { /* CMP */
            sub_with_carry(c, c->r[rd], b, 0, 1);
        } else { /* MOV */
            if (rd == 15u)
                c->r[15] = b & ~1u;
            else
                c->r[rd] = b;
        }
        if (rd == 15u && op2 != 1u)
            return 3;
        return 2;
    }

    /* load/store with register offset + sign-extended/halfword: 0101.
     * Format 6/7: Rm bits[8:6], Rn bits[5:3], Rd bits[2:0]; halfword form
     * has bit9 set and H=bit11, S=bit10. */
    if ((instr & 0xF000u) == 0x5000u) {
        uint8_t rm = (uint8_t)((instr >> 6) & 7u);
        uint8_t rn = (uint8_t)((instr >> 3) & 7u);
        uint8_t rd = (uint8_t)(instr & 7u);
        uint32_t addr = c->r[rn] + c->r[rm];
        uint8_t op2 = (uint8_t)((instr >> 10) & 3u);
        uint8_t kind = (uint8_t)((instr >> 10) & 3u);
        if ((instr & 0x0200u) == 0u) { /* 0101 0xx: STR/LDRB/LDR/STRB... */
            switch (op2) {
            case 0: gba_mem_write32(g, addr, c->r[rd]); break;
            case 1: gba_mem_write8(g, addr, (uint8_t)c->r[rd]); break;
            case 2: c->r[rd] = gba_bus_read32(g, addr); break;
            default: c->r[rd] = gba_mem_read8(g, addr); break;
            }
            return (op2 >= 2u) ? 3u : 2u;
        }
        /* 0101 1xx: STRH/LDSB/LDRH/LDSH */
        switch (kind) {
        case 0: gba_mem_write16(g, addr, (uint16_t)c->r[rd]); break;
        case 1: c->r[rd] = (uint32_t)(int32_t)(int8_t)gba_mem_read8(g, addr); break;
        case 2: c->r[rd] = gba_bus_read16(g, addr); break;
        default: c->r[rd] = (uint32_t)(int32_t)(int16_t)gba_bus_read16(g, addr); break;
        }
        return (kind & 2u) ? 3u : 2u;
    }

    /* STR/LDRB/LDR/LDRB immediate: 011. L=bit11, B=bit10; byte transfers
     * scale the offset by 1, word transfers by 4. */
    if ((instr & 0xE000u) == 0x6000u) {
        uint8_t op2 = (uint8_t)((instr >> 10) & 3u);
        uint32_t imm = (instr >> 6) & 0x1Fu;
        uint8_t rn = (uint8_t)((instr >> 3) & 7u);
        uint8_t rd = (uint8_t)(instr & 7u);
        uint32_t addr = c->r[rn] + ((op2 & 1u) ? imm : imm * 4u);
        if (op2 == 0u) {
            gba_mem_write32(g, addr, c->r[rd]);
        } else if (op2 == 1u) {
            gba_mem_write8(g, addr, (uint8_t)c->r[rd]);
        } else if (op2 == 2u) {
            c->r[rd] = gba_bus_read32(g, addr);
        } else {
            c->r[rd] = gba_mem_read8(g, addr);
        }
        return (op2 >= 2u) ? 3u : 2u;
    }

    /* STRH/LDRH immediate: 1000 */
    if ((instr & 0xF000u) == 0x8000u) {
        uint32_t imm = (uint32_t)((instr >> 6) & 0x1Fu) * 2u;
        uint8_t rn = (uint8_t)((instr >> 3) & 7u);
        uint8_t rd = (uint8_t)(instr & 7u);
        uint32_t addr = c->r[rn] + imm;
        if (instr & (1u << 11))
            c->r[rd] = gba_bus_read16(g, addr);
        else
            gba_mem_write16(g, addr, (uint16_t)c->r[rd]);
        return (instr & (1u << 11)) ? 3u : 2u;
    }

    /* STR/LDR SP-relative: 1001 */
    if ((instr & 0xF000u) == 0x9000u) {
        uint8_t rd = (uint8_t)((instr >> 8) & 7u);
        uint32_t addr = c->r[13] + (uint32_t)(instr & 0xFFu) * 4u;
        if (instr & (1u << 11))
            c->r[rd] = gba_bus_read32(g, addr);
        else
            gba_mem_write32(g, addr, c->r[rd]);
        return (instr & (1u << 11)) ? 3u : 2u;
    }

    /* load address: 1010. PC form: instruction address + 4, word-aligned */
    if ((instr & 0xF000u) == 0xA000u) {
        uint8_t rd = (uint8_t)((instr >> 8) & 7u);
        uint32_t imm = (uint32_t)(instr & 0xFFu) * 4u;
        c->r[rd] = (instr & (1u << 11)) ? c->r[13] + imm
                                        : ((c->r[15] + 2u) & ~3u) + imm;
        return 2;
    }

    /* push/pop: 1011 x10x */
    if ((instr & 0xF600u) == 0xB400u) {
        int load = (instr & (1u << 11)) != 0;
        int pc_lr = (instr & (1u << 8)) != 0;
        uint32_t count = 0;
        for (int i = 0; i < 8; i++)
            if (instr & (1u << i))
                count++;
        count += (uint32_t)pc_lr;
        uint32_t addr = c->r[13];
        if (!load)
            c->r[13] -= count * 4u;
        else
            addr = c->r[13];
        addr = c->r[13];
        if (!load)
            addr = c->r[13];
        for (int i = 0; i < 8; i++) {
            if (!(instr & (1u << i)))
                continue;
            if (!load) {
                addr += 4u;
                gba_mem_write32(g, addr - 4u, c->r[i]);
            } else {
                c->r[i] = gba_bus_read32(g, addr);
                addr += 4u;
            }
        }
        if (pc_lr) {
            if (load) {
                uint32_t v = gba_bus_read32(g, addr);
                c->r[13] = addr + 4u;
                /* ARMv4T: POP {pc} does not interwork; word-align, keep T */
                c->r[15] = v & ~3u;
                return 5;
            }
            gba_mem_write32(g, addr, c->r[14]);
            addr += 4u;
        }
        c->r[13] = load ? addr : c->r[13];
        return load ? 4u : 3u;
    }

    /* ADD/SUB SP immediate: 1011 0000x */
    if ((instr & 0xFF00u) == 0xB000u) {
        uint32_t imm = (uint32_t)(instr & 0x7Fu) * 4u;
        c->r[13] = (instr & (1u << 7)) ? c->r[13] + imm : c->r[13] - imm;
        return 2;
    }

    /* LDMIA/STMIA: 1100 */
    if ((instr & 0xF000u) == 0xC000u) {
        uint8_t rn = (uint8_t)((instr >> 8) & 7u);
        int load = (instr & (1u << 11)) != 0;
        uint32_t addr = c->r[rn];
        uint32_t base = addr;
        for (int i = 0; i < 8; i++) {
            if (!(instr & (1u << i)))
                continue;
            if (load)
                c->r[i] = gba_bus_read32(g, addr);
            else
                gba_mem_write32(g, addr, c->r[i]);
            addr += 4u;
        }
        if (!(instr & (1u << rn)) || !load)
            c->r[rn] = addr;
        else
            c->r[rn] = base;
        return load ? 4u : 3u;
    }

    /* conditional branch: 1101 */
    if ((instr & 0xF000u) == 0xD000u && (instr & 0x0F00u) != 0x0F00u) {
        uint32_t cond = (uint32_t)((instr >> 8) & 0xFu);
        int8_t off = (int8_t)(instr & 0xFFu);
        uint32_t next = c->r[15] + 2u;
        if (cond_ok(cond, c->cpsr)) {
            c->r[15] = next + ((uint32_t)(int32_t)off << 1u);
            return 3;
        }
        return 2;
    }

    /* SWI Thumb: 1101 1111 */
    if ((instr & 0xFF00u) == 0xDF00u) {
        gba_swi_hle(g, instr & 0xFFu);
        return 3;
    }

    /* unconditional branch: 11100. Target = instruction address + 4 + off*2
     * (r15 is instruction address + 2 inside thumb_step). */
    if ((instr & 0xF800u) == 0xE000u) {
        int32_t off = (int32_t)((instr & 0x7FFu) << 21) >> 21;
        c->r[15] = c->r[15] + 2u + ((uint32_t)off << 1u);
        return 3;
    }

    /* BL first halfword: 11110. LR = instruction address + 4 + off<<12
     * (LSB 1). r15 must stay on the SECOND halfword: it already points
     * there (pre-advanced fetch), so no advance here. */
    if ((instr & 0xF800u) == 0xF000u) {
        int32_t off = (int32_t)((instr & 0x7FFu) << 21) >> 21;
        c->r[14] = (c->r[15] + 2u + ((uint32_t)off << 12u)) | 1u;
        return 3;
    }

    /* BL second halfword: 11111. LR = instruction address of the pair + 4
     * (r15 already points there), LSB 1. */
    if ((instr & 0xF800u) == 0xF800u) {
        int32_t off = (int32_t)((instr & 0x7FFu) << 21) >> 21;
        uint32_t target = c->r[14] + ((uint32_t)off << 1u);
        c->r[14] = c->r[15] | 1u;
        c->r[15] = target & ~1u;
        return 4;
    }

    /* undefined (treated as NOP: documented) */
    return 2;
}

uint32_t gba_cpu_step(gba_t *g)
{
    gba_cpu *c = &g->cpu;

    /* returning from an HLE-dispatched IRQ handler: sentinel at BIOS 0 */
    if (c->bios_dispatch && c->r[15] == 0u) {
        gba_irq_dispatch_return(g);
        return 12;
    }

    if (c->halted) {
        uint32_t pending = (uint32_t)(g->mem.if_reg & g->mem.ie);
        /* IntrWait/VBlankIntrWait wake only on the requested flags;
         * plain HALT wakes on any enabled interrupt. */
        int wake = c->intr_wait_active
                       ? (pending & c->intr_wait_flags) != 0u
                       : pending != 0u;
        if (wake)
            c->halted = 0;
        return 4;
    }

    if ((g->mem.if_reg & g->mem.ie) != 0u && (c->cpsr & GBA_I) == 0u &&
        !c->bios_dispatch) {
        gba_irq_dispatch(g);
        return 12;
    }

    if (c->cpsr & GBA_T) {
        uint16_t instr = gba_bus_read16(g, c->r[15]);
        c->r[15] += 2u;
        return thumb_step(g, c, instr);
    }
    uint32_t instr = gba_bus_read32(g, c->r[15]);
    c->r[15] += 4u;
    return arm_step(g, c, instr);
}
