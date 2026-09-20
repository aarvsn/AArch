/*
 * AArch shared ARM interpreter. See arm.h for scope and sources.
 *
 * Decoder layout note: the opcode space is decoded in the order mandated by
 * the architecture (halfword/signed transfers and the multiply/swap block
 * live inside the 000 opcode space, PSR transfers / BX / v5TE extras inside
 * both the 000 and 001 spaces, then single data transfer, block transfers,
 * branches, coprocessor, data processing last).
 */
#include "arm.h"

#include <string.h>

/* ---- helpers ------------------------------------------------------------------ */

static int mode_index(uint32_t mode)
{
    switch (mode) {
    case ARM_MODE_FIQ: return 1;
    case ARM_MODE_IRQ: return 2;
    case ARM_MODE_SVC: return 3;
    case ARM_MODE_ABT: return 4;
    case ARM_MODE_UND: return 5;
    default:           return 0; /* usr/sys share bank 0 */
    }
}

static void bank_save(arm_t *c)
{
    int i = mode_index(c->cpsr & ARM_F_MODE);
    c->bank_r13[i] = c->r[13];
    c->bank_r14[i] = c->r[14];
    if (i == 1) {
        for (int k = 0; k < 5; k++)
            c->fiq_r8[k] = c->r[8 + k];
    }
}

static void bank_load(arm_t *c)
{
    int i = mode_index(c->cpsr & ARM_F_MODE);
    c->r[13] = c->bank_r13[i];
    c->r[14] = c->bank_r14[i];
    if (i == 1) {
        for (int k = 0; k < 5; k++)
            c->r[8 + k] = c->fiq_r8[k];
    }
}

static void set_mode(arm_t *c, uint32_t mode)
{
    uint32_t m = mode & ARM_F_MODE;
    if ((c->cpsr & ARM_F_MODE) == m)
        return;
    bank_save(c);
    c->cpsr = (c->cpsr & ~ARM_F_MODE) | m;
    bank_load(c);
}

static uint32_t *spsr_ptr(arm_t *c)
{
    int i = mode_index(c->cpsr & ARM_F_MODE);
    if (i == 0)
        return NULL; /* no SPSR in usr/sys (unpredictable on hardware) */
    return &c->bank_spsr[i];
}

/* ---- condition field ------------------------------------------------------------ */

static int cond_pass(arm_t *c, uint32_t cond)
{
    uint32_t f = c->cpsr;
    int n = (f >> 31) & 1, z = (f >> 30) & 1;
    int cy = (f >> 29) & 1, v = (f >> 28) & 1;
    switch (cond >> 1) {
    case 0:  return z;             /* EQ/NE  */
    case 1:  return cy;            /* CS/CC  */
    case 2:  return n;             /* MI/PL  */
    case 3:  return v;             /* VS/VC  */
    case 4:  return cy & !z;       /* HI/LS  */
    case 5:  return n == v;        /* GE/LT  */
    case 6:  return (n == v) & !z; /* GT/LE  */
    default: return 1;             /* AL (and the reserved AL slot) */
    }
}

/* ---- shifter -------------------------------------------------------------------- */

/* Computes the shifter output and carry-out (0/1) for register operands.
 * type: 0=LSL 1=LSR 2=ASR 3=ROR. by_reg selects the register-amount forms
 * (LSR/ASR amount 0 == 32). For type 3 with immediate amount 0: RRX. */
static uint32_t shift_val(arm_t *c, int type, uint32_t val, uint32_t amount,
                          int by_reg, uint32_t *carry_out)
{
    uint32_t carry = (c->cpsr & ARM_F_C) ? 1u : 0u;
    *carry_out = carry;
    if (type == 0) { /* LSL */
        if (amount == 0)
            return val;
        if (amount < 32) {
            *carry_out = (val >> (32 - amount)) & 1u;
            return val << amount;
        }
        *carry_out = (amount == 32) ? (val & 1u) : 0u;
        return 0;
    }
    if (type == 1) { /* LSR */
        if (amount == 0 && !by_reg)
            return val;
        if (amount == 0)
            amount = 32;
        if (amount < 32) {
            *carry_out = (val >> (amount - 1)) & 1u;
            return val >> amount;
        }
        *carry_out = (val >> 31) & 1u;
        return 0;
    }
    if (type == 2) { /* ASR */
        int32_t s = (int32_t)val;
        if (amount == 0 && !by_reg)
            return val;
        if (amount == 0)
            amount = 32;
        if (amount < 32) {
            *carry_out = (val >> (amount - 1)) & 1u;
            return (uint32_t)(s >> amount);
        }
        *carry_out = (val >> 31) & 1u;
        return (uint32_t)(s >> 31);
    }
    /* ROR / RRX */
    if (amount == 0) {
        if (by_reg)
            return val; /* ROR by register with amount 0: unchanged */
        /* ROR #0 immediate == RRX */
        *carry_out = val & 1u;
        return (val >> 1) | ((c->cpsr & ARM_F_C) ? 0x80000000u : 0u);
    }
    amount &= 31u;
    if (amount == 0) {
        *carry_out = (val >> 31) & 1u;
        return val;
    }
    *carry_out = (val >> (amount - 1)) & 1u;
    return (val >> amount) | (val << (32 - amount));
}

/* ---- flags ---------------------------------------------------------------------- */

static void set_nzcv(arm_t *c, uint32_t n, uint32_t z, uint32_t cy, uint32_t v)
{
    c->cpsr &= ~(ARM_F_N | ARM_F_Z | ARM_F_C | ARM_F_V);
    if (n & 1u) c->cpsr |= ARM_F_N;
    if (z & 1u) c->cpsr |= ARM_F_Z;
    if (cy & 1u) c->cpsr |= ARM_F_C;
    if (v & 1u) c->cpsr |= ARM_F_V;
}

static void set_logic_nz(arm_t *c, uint32_t res)
{
    c->cpsr &= ~(ARM_F_N | ARM_F_Z);
    if (res & 0x80000000u) c->cpsr |= ARM_F_N;
    if (res == 0) c->cpsr |= ARM_F_Z;
}

/* ---- exceptions ----------------------------------------------------------------- */

/*
 * Vector through the fixed low-vector table (high vectors are a CP15
 * control feature; the DS core does not enable them - documented).
 * LR conventions (as stored; return with the usual SUBS PC, LR, #4/#8):
 *   SWI/UND/PABT/IRQ/FIQ: LR = interrupted + 4 ; DABT: LR = interrupted + 8
 */
static void take_exception(arm_t *c, uint32_t vec_addr, uint32_t lr,
                           uint32_t mode, uint32_t set_if)
{
    uint32_t *spsr;
    uint32_t old_cpsr = c->cpsr;
    set_mode(c, mode);
    spsr = spsr_ptr(c);
    if (spsr != NULL)
        *spsr = old_cpsr;
    c->r[14] = lr;
    c->cpsr &= ~(ARM_F_T | ARM_F_I | ARM_F_F);
    c->cpsr |= set_if;
    c->pc = vec_addr;
}

/* ---- init / reset --------------------------------------------------------------- */

void arm_init(arm_t *c, const arm_bus_t *bus, const arm_cp15_t *cp15)
{
    memset(c, 0, sizeof *c);
    c->bus = bus;
    c->cp15 = cp15;
}

void arm_reset(arm_t *c)
{
    memset(c->r, 0, sizeof c->r);
    memset(c->bank_r13, 0, sizeof c->bank_r13);
    memset(c->bank_r14, 0, sizeof c->bank_r14);
    memset(c->bank_spsr, 0, sizeof c->bank_spsr);
    memset(c->fiq_r8, 0, sizeof c->fiq_r8);
    c->pc = ARM_VEC_RESET;
    c->cpsr = ARM_MODE_SVC | ARM_F_I | ARM_F_F;
    c->irq_line = 0;
    c->fiq_line = 0;
}

void arm_irq(arm_t *c)
{
    c->irq_line = 1;
}

void arm_fiq(arm_t *c)
{
    c->fiq_line = 1;
}

uint32_t arm_r15_read(const arm_t *c, uint32_t cur)
{
    return (c->cpsr & ARM_F_T) ? cur + 4u : cur + 8u;
}

/* ---- bus helpers ---------------------------------------------------------------- */

static uint32_t rd32(arm_t *c, uint32_t a)
{
    return c->bus->read32(c->bus->user, a & ~3u);
}

static uint16_t rd16(arm_t *c, uint32_t a)
{
    return c->bus->read16(c->bus->user, a & ~1u);
}

static uint8_t rd8(arm_t *c, uint32_t a)
{
    return c->bus->read8(c->bus->user, a);
}

/* ---- MSR / MRS ------------------------------------------------------------------ */

static void msr_write(arm_t *c, uint32_t val, uint32_t mask, int spsr)
{
    uint32_t *target;
    if (spsr) {
        target = spsr_ptr(c);
        if (target == NULL)
            return; /* no SPSR in this mode: write ignored */
        *target = (*target & ~mask) | (val & mask);
        return;
    }
    /* CPSR */
    uint32_t ctrl = mask & (ARM_F_MODE | ARM_F_I | ARM_F_F | ARM_F_T);
    if (ctrl) {
        int privileged = (c->cpsr & ARM_F_MODE) != ARM_MODE_USR;
        if (privileged) {
            if (val & ARM_F_MODE)
                set_mode(c, val & ARM_F_MODE);
            c->cpsr &= ~(ARM_F_I | ARM_F_F | ARM_F_T);
            c->cpsr |= val & (ARM_F_I | ARM_F_F | ARM_F_T);
        }
    }
    uint32_t flags = mask & (ARM_F_N | ARM_F_Z | ARM_F_C | ARM_F_V);
    if (flags) {
        c->cpsr &= ~(ARM_F_N | ARM_F_Z | ARM_F_C | ARM_F_V);
        c->cpsr |= val & flags;
    }
}

/* ---- block transfer ------------------------------------------------------------- */

static void ldm_stm(arm_t *c, uint32_t insn)
{
    int p = (insn >> 24) & 1, u = (insn >> 23) & 1;
    int s = (insn >> 22) & 1, w = (insn >> 21) & 1;
    int l = (insn >> 20) & 1;
    uint32_t rn = (insn >> 16) & 0xFu;
    uint32_t list = insn & 0xFFFFu;
    uint32_t base = c->r[rn];
    uint32_t addr;
    int count = 0;
    uint32_t saved_mode = 0;

    for (int i = 0; i < 16; i++)
        if (list & (1u << i))
            count++;

    if (u)
        addr = p ? base + 4u : base;
    else
        addr = p ? base - 4u * (uint32_t)count
                 : base - 4u * ((uint32_t)count - 1u);

    /* user-bank transfer (^ without PC): access user-mode registers */
    if (s && !(list & 0x8000u) && (c->cpsr & ARM_F_MODE) != ARM_MODE_USR) {
        saved_mode = c->cpsr & ARM_F_MODE;
        set_mode(c, ARM_MODE_USR);
    }

    uint32_t wb = base + (u ? 4u * (uint32_t)count
                            : 0u - 4u * (uint32_t)count);
    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i)))
            continue;
        if (l) {
            if (i == 15) {
                c->pc = rd32(c, addr) & ~3u; /* v4: no interworking */
                if (s && saved_mode) {
                    /* ^ with PC: exception return, CPSR <- SPSR */
                    uint32_t *sp = spsr_ptr(c);
                    if (sp != NULL) {
                        uint32_t m = *sp;
                        set_mode(c, m & ARM_F_MODE);
                        c->cpsr = (c->cpsr & ARM_F_MODE) | (m & ~ARM_F_MODE);
                    }
                }
            } else {
                c->r[i] = rd32(c, addr);
            }
        } else {
            c->bus->write32(c->bus->user, addr & ~3u, c->r[i]);
        }
        addr += 4u;
    }

    if (saved_mode)
        set_mode(c, saved_mode);

    /* writeback (suppressed on LDM when base is in the list) */
    if (w && !(l && (list & (1u << rn))))
        c->r[rn] = wb;
}

/* ---- data processing ------------------------------------------------------------ */

static uint32_t dp_operand(arm_t *c, uint32_t insn, int is_imm, uint32_t *carry)
{
    *carry = (c->cpsr & ARM_F_C) ? 1u : 0u;
    if (is_imm) {
        uint32_t imm = insn & 0xFFu;
        uint32_t rot = ((insn >> 8) & 0xFu) * 2u;
        if (rot == 0)
            return imm; /* no RRX for immediates; C unaffected */
        uint32_t out = (imm >> rot) | (imm << (32 - rot));
        *carry = (imm >> (rot - 1)) & 1u;
        return out;
    }
    int type = (insn >> 5) & 3u;
    uint32_t rm = insn & 0xFu;
    if ((insn >> 4) & 1u) {
        uint32_t amount = c->r[(insn >> 8) & 0xFu] & 0xFFu;
        uint32_t co;
        uint32_t v = shift_val(c, type, c->r[rm], amount, 1, &co);
        *carry = co;
        return v;
    }
    uint32_t amount = (insn >> 7) & 0x1Fu;
    uint32_t co;
    uint32_t v = shift_val(c, type, c->r[rm], amount, 0, &co);
    *carry = co;
    return v;
}

static void dp_exec(arm_t *c, uint32_t insn, uint32_t a, uint32_t b,
                    uint32_t carry, uint32_t cur)
{
    int is_imm = (insn >> 25) & 1;
    uint32_t opcode = (insn >> 21) & 0xFu;
    int s = (insn >> 20) & 1;
    uint32_t rd = (insn >> 12) & 0xFu;
    uint32_t res = 0;
    uint32_t cy = carry, v = (c->cpsr & ARM_F_V) ? 1u : 0u;

    switch (opcode) {
    case 0x0: {
        res = a & b;
        c->r[rd] = res;
        if (s)
            set_nzcv(c, res >> 31, res == 0, cy, v);
        break;
    }
    case 0x1: {
        res = a ^ b;
        c->r[rd] = res;
        if (s)
            set_nzcv(c, res >> 31, res == 0, cy, v);
        break;
    }
    case 0x2: {
        uint32_t res2 = a - b;
        uint32_t cy2 = (a >= b) ? 1u : 0u;
        uint32_t v2 = ((a ^ b) & (a ^ res2)) >> 31;
        c->r[rd] = res2;
        if (s)
            set_nzcv(c, res2 >> 31, res2 == 0, cy2, v2);
        break;
    }
    case 0x3: {
        uint32_t res2 = b - a;
        uint32_t cy2 = (b >= a) ? 1u : 0u;
        uint32_t v2 = ((b ^ a) & (b ^ res2)) >> 31;
        c->r[rd] = res2;
        if (s)
            set_nzcv(c, res2 >> 31, res2 == 0, cy2, v2);
        break;
    }
    case 0x4: {
        uint32_t res2 = a + b;
        uint32_t cy2 = (res2 < a) ? 1u : 0u;
        uint32_t v2 = (~(a ^ b) & (a ^ res2)) >> 31;
        c->r[rd] = res2;
        if (s)
            set_nzcv(c, res2 >> 31, res2 == 0, cy2, v2);
        break;
    }
    case 0x5: { /* ADC */
        uint32_t ci = (c->cpsr & ARM_F_C) ? 1u : 0u;
        uint64_t r64 = (uint64_t)a + b + ci;
        res = (uint32_t)r64;
        cy = (uint32_t)(r64 >> 32);
        v = (~(a ^ b) & (a ^ res)) >> 31;
        c->r[rd] = res;
        if (s)
            set_nzcv(c, res >> 31, res == 0, cy, v);
        break;
    }
    case 0x6: { /* SBC */
        uint32_t ci = (c->cpsr & ARM_F_C) ? 0u : 1u;
        res = a - b - ci;
        cy = ci ? (a > b) : (a >= b);
        v = ((a ^ b) & (a ^ res)) >> 31;
        c->r[rd] = res;
        if (s)
            set_nzcv(c, res >> 31, res == 0, cy, v);
        break;
    }
    case 0x7: { /* RSC */
        uint32_t ci = (c->cpsr & ARM_F_C) ? 0u : 1u;
        res = b - a - ci;
        cy = ci ? (b > a) : (b >= a);
        v = ((b ^ a) & (b ^ res)) >> 31;
        c->r[rd] = res;
        if (s)
            set_nzcv(c, res >> 31, res == 0, cy, v);
        break;
    }
    case 0x8: {
        res = a & b;
        if (s)
            set_nzcv(c, res >> 31, res == 0, cy, v);
        break;
    }
    case 0x9: {
        res = a ^ b;
        if (s)
            set_nzcv(c, res >> 31, res == 0, cy, v);
        break;
    }
    case 0xA: {
        uint32_t res2 = a - b;
        uint32_t cy2 = (a >= b) ? 1u : 0u;
        uint32_t v2 = ((a ^ b) & (a ^ res2)) >> 31;
        if (s)
            set_nzcv(c, res2 >> 31, res2 == 0, cy2, v2);
        break;
    }
    case 0xB: {
        uint32_t res2 = a + b;
        uint32_t cy2 = (res2 < a) ? 1u : 0u;
        uint32_t v2 = (~(a ^ b) & (a ^ res2)) >> 31;
        if (s)
            set_nzcv(c, res2 >> 31, res2 == 0, cy2, v2);
        break;
    }
    case 0xC: res = a | b;  c->r[rd] = res; if (s) set_nzcv(c, res >> 31, res == 0, cy, v); break;
    case 0xD: res = b;      c->r[rd] = res; if (s) set_nzcv(c, res >> 31, res == 0, cy, v); break;
    case 0xE: res = a & ~b; c->r[rd] = res; if (s) set_nzcv(c, res >> 31, res == 0, cy, v); break;
    case 0xF: res = ~b;     c->r[rd] = res; if (s) set_nzcv(c, res >> 31, res == 0, cy, v); break;
    default: break;
    }

    /* write to PC: branch (+ exception return when S in privileged mode) */
    if (rd == 15) {
        int writes_pc = (opcode == 0xDu || opcode == 0xFu) ||
                        (opcode <= 0x3u || (opcode >= 0x5u && opcode <= 0x7u) ||
                         opcode == 0xCu || opcode == 0xEu);
        (void)writes_pc;
        if (s) {
            uint32_t *sp = spsr_ptr(c);
            if (sp != NULL) {
                uint32_t m = *sp;
                set_mode(c, m & ARM_F_MODE);
                c->cpsr = (c->cpsr & ARM_F_MODE) | (m & ~ARM_F_MODE);
            }
        }
        c->pc = c->r[15] & ~3u; /* v4 behavior; bit0 ignored */
        return;
    }
    (void)is_imm;
    (void)cur;
}

/* ---- one ARM instruction -------------------------------------------------------- */

static uint32_t arm_exec(arm_t *c, uint32_t insn, uint32_t cur)
{
    uint32_t cond = insn >> 28;

    /* unconditional space */
    if (cond == 0xFu) {
        /* BLX imm (v5): 1111 101H imm24 */
        if (c->v5te && ((insn >> 25) & 0x7u) == 0x5u) {
            int32_t off = (int32_t)(insn & 0x00FFFFFFu) << 2;
            if (insn & 0x01000000u)
                off += 2;
            if (off & 0x02000000u)
                off |= (int32_t)0xFC000000u;
            c->r[14] = cur + 4u;
            c->cpsr |= ARM_F_T;
            c->pc = cur + 8u + (uint32_t)off;
            return 1;
        }
        take_exception(c, ARM_VEC_UND, cur + 4u, ARM_MODE_UND, ARM_F_I);
        return 1;
    }

    if (!cond_pass(c, cond))
        return 1;

    uint32_t op = (insn >> 24) & 0xFu;

    /* SWI */
    if (op == 0xFu) {
        take_exception(c, ARM_VEC_SWI, cur + 4u, ARM_MODE_SVC, ARM_F_I);
        return 1;
    }

    /* ---- 000 opcode space ---------------------------------------------------- */
    if (op == 0x0u || op == 0x1u) {
        uint32_t b7 = (insn >> 7) & 1u;
        uint32_t b4 = insn & 0x10u; /* bit 4 as flag */

        /* halfword / signed transfer: bits [7:4] = 1SH1 with H (bit6) set.
         * The multiply block shares bit7/bit4 set (tail 1001), so H is
         * the discriminator. */
        if (b7 && b4 && ((insn >> 6) & 1u)) {
            int p = (insn >> 24) & 1, u = (insn >> 23) & 1;
            int w = (insn >> 21) & 1, l = (insn >> 20) & 1;
            int sh = (insn >> 5) & 3u;
            uint32_t rn = (insn >> 16) & 0xFu, rd = (insn >> 12) & 0xFu;
            uint32_t base = c->r[rn];
            uint32_t off;
            if ((insn >> 22) & 1u)
                off = c->r[insn & 0xFu]; /* register form (bit22 = I) */
            else
                off = ((insn >> 4) & 0xF0u) | (insn & 0xFu);
            uint32_t addr = p ? base + (u ? off : 0u - off) : base;
            if (l) {
                if (sh == 1)
                    c->r[rd] = rd16(c, addr);
                else if (sh == 2)
                    c->r[rd] = (uint32_t)(int32_t)(int8_t)rd8(c, addr);
                else
                    c->r[rd] = (uint32_t)(int32_t)(int16_t)rd16(c, addr);
            } else {
                c->bus->write16(c->bus->user, addr & ~1u, (uint16_t)c->r[rd]);
            }
            if (!p)
                c->r[rn] = base + (u ? off : 0u - off);
            else if (w)
                c->r[rn] = addr;
            return 1;
        }

        if ((insn & 0x0FB00FF0u) == 0x01000090u) { /* SWP/SWPB */
            uint32_t rn = (insn >> 16) & 0xFu, rd = (insn >> 12) & 0xFu;
            uint32_t rm = insn & 0xFu;
            if ((insn >> 22) & 1u) {
                uint8_t b = rd8(c, c->r[rn]);
                c->bus->write8(c->bus->user, c->r[rn], (uint8_t)c->r[rm]);
                c->r[rd] = b;
            } else {
                uint32_t w = rd32(c, c->r[rn]);
                c->bus->write32(c->bus->user, c->r[rn] & ~3u, c->r[rm]);
                c->r[rd] = w;
            }
            return 1;
        }

        if ((insn & 0x0FC000F0u) == 0x00000090u) { /* MUL/MLA */
            uint32_t rd = (insn >> 16) & 0xFu, rn = (insn >> 12) & 0xFu;
            uint32_t rs = (insn >> 8) & 0xFu, rm = insn & 0xFu;
            uint32_t res = c->r[rm] * c->r[rs];
            if ((insn >> 21) & 1u)
                res += c->r[rn];
            c->r[rd] = res;
            if ((insn >> 20) & 1u)
                set_logic_nz(c, res);
            return 1;
        }

        if ((insn & 0x0F8000F0u) == 0x00800090u) { /* UMULL/UMLAL/SMULL/SMLAL */
            uint32_t rdhi = (insn >> 16) & 0xFu, rdlo = (insn >> 12) & 0xFu;
            uint32_t rs = (insn >> 8) & 0xFu, rm = insn & 0xFu;
            int is_signed = (insn >> 22) & 1u; /* bit22: 1 = SMULL/UMLAL */
            int is_a = (insn >> 21) & 1u;
            uint64_t res;
            if (!is_signed)
                res = (uint64_t)c->r[rm] * (uint64_t)c->r[rs];
            else
                res = (uint64_t)((int64_t)(int32_t)c->r[rm] *
                                 (int64_t)(int32_t)c->r[rs]);
            if (is_a)
                res += ((uint64_t)c->r[rdhi] << 32) | c->r[rdlo];
            c->r[rdlo] = (uint32_t)res;
            c->r[rdhi] = (uint32_t)(res >> 32);
            if ((insn >> 20) & 1u) {
                c->cpsr &= ~(ARM_F_N | ARM_F_Z);
                if (res & 0x8000000000000000ull) c->cpsr |= ARM_F_N;
                if (res == 0) c->cpsr |= ARM_F_Z;
            }
            return 1;
        }

        if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) { /* BX */
            uint32_t target = c->r[insn & 0xFu];
            if (target & 1u) {
                c->cpsr |= ARM_F_T;
                c->pc = target & ~1u;
            } else {
                c->pc = target & ~3u;
            }
            return 1;
        }

        if ((insn & 0x0FFFFFF0u) == 0x012FFF30u) { /* BLX reg (v5) */
            uint32_t target = c->r[insn & 0xFu];
            if (!c->v5te) {
                take_exception(c, ARM_VEC_UND, cur + 4u, ARM_MODE_UND, ARM_F_I);
                return 1;
            }
            c->r[14] = cur + 4u;
            if (target & 1u) {
                c->cpsr |= ARM_F_T;
                c->pc = target & ~1u;
            } else {
                c->pc = target & ~3u;
            }
            return 1;
        }

        if ((insn & 0x0FBF0000u) == 0x010F0000u) { /* MRS */
            uint32_t rd = (insn >> 12) & 0xFu;
            uint32_t *sp = ((insn >> 22) & 1u) ? spsr_ptr(c) : NULL;
            c->r[rd] = sp ? *sp : c->cpsr;
            return 1;
        }

        if ((insn & 0x0FB0FFF0u) == 0x0120F000u) { /* MSR (register) */
            uint32_t mask = ((insn >> 16) & 0xFu) << 16;
            msr_write(c, c->r[insn & 0xFu], mask, (insn >> 22) & 1u);
            return 1;
        }

        if (c->v5te) {
            /* CLZ: cond 0001 0110 1111 Rd 1111 0001 Rm (Rd at [19:16]) */
            if ((insn & 0x0FF0FFF0u) == 0x0160FF10u) {
                uint32_t rd = (insn >> 16) & 0xFu, v = c->r[insn & 0xFu];
                uint32_t n = 0;
                while (n < 32 && !(v & 0x80000000u)) {
                    v <<= 1;
                    n++;
                }
                c->r[rd] = n;
                return 1;
            }
            /* QADD/QSUB/QDADD/QDSUB: [7:4]=0101 */
            if ((insn & 0x0F9000F0u) == 0x01000050u) {
                uint32_t rd = (insn >> 16) & 0xFu;
                uint32_t m = c->r[(insn >> 8) & 0xFu];
                uint32_t v = c->r[insn & 0xFu];
                int sub = (insn >> 21) & 1u;
                int dbl = (insn >> 22) & 1u;
                int32_t operand = (int32_t)m;
                if (dbl) { /* QDADD/QDSUB: saturating double of Rm */
                    int64_t d = (int64_t)operand * 2;
                    if (d > 0x7FFFFFFFll)
                        d = 0x7FFFFFFF;
                    else if (d < (int64_t)0xFFFFFFFF80000000ull)
                        d = (int64_t)0xFFFFFFFF80000000ull;
                    operand = (int32_t)d;
                }
                int64_t r64 = sub ? (int64_t)(int32_t)v - operand
                                  : (int64_t)(int32_t)v + operand;
                if (r64 > 0x7FFFFFFFll) {
                    c->r[rd] = 0x7FFFFFFFu;
                    c->cpsr |= ARM_F_Q;
                } else if (r64 < (int64_t)0xFFFFFFFF80000000ull) {
                    c->r[rd] = 0x80000000u;
                    c->cpsr |= ARM_F_Q;
                } else {
                    c->r[rd] = (uint32_t)r64;
                }
                return 1;
            }
            /* SMLAxy: 0001 0000 Rd Rn Rs 1yx0 Rm */
            if ((insn & 0x0FF00090u) == 0x01000080u) {
                uint32_t rd = (insn >> 16) & 0xFu, rn = (insn >> 12) & 0xFu;
                int x = (insn >> 5) & 1u, y = (insn >> 6) & 1u;
                int32_t m = (int32_t)(x ? (c->r[insn & 0xFu] >> 16)
                                        : (c->r[insn & 0xFu] & 0xFFFFu));
                int32_t s2 = (int32_t)(y ? (c->r[(insn >> 8) & 0xFu] >> 16)
                                         : (c->r[(insn >> 8) & 0xFu] & 0xFFFFu));
                int64_t acc = (int64_t)m * s2 + (int32_t)c->r[rn];
                if (acc > 0x7FFFFFFFll || acc < (int64_t)0xFFFFFFFF80000000ull)
                    c->cpsr |= ARM_F_Q;
                c->r[rd] = (uint32_t)acc;
                return 1;
            }
            /* SMLAWy/SMULWy: 0001 0010 Rd Rn Rs 1y00 Rm
             * W-variant semantics (documented): full 32-bit Rm x 16-bit
             * half of Rs (y selects the half), 48-bit product, take [47:16]. */
            if ((insn & 0x0FF000B0u) == 0x01200080u) {
                uint32_t rd = (insn >> 16) & 0xFu, rn = (insn >> 12) & 0xFu;
                int y = (insn >> 6) & 1u;
                int32_t full = (int32_t)c->r[insn & 0xFu];
                int32_t half = (int32_t)(y ? (c->r[(insn >> 8) & 0xFu] >> 16)
                                           : (c->r[(insn >> 8) & 0xFu] & 0xFFFFu));
                int64_t top = ((int64_t)full * half) >> 16;
                if (rn == 0u) { /* SMULWy */
                    c->r[rd] = (uint32_t)top;
                } else { /* SMLAWy: saturating add of Rn */
                    int64_t acc = top + (int32_t)c->r[rn];
                    if (acc > 0x7FFFFFFFll || acc < (int64_t)0xFFFFFFFF80000000ull)
                        c->cpsr |= ARM_F_Q;
                    c->r[rd] = (uint32_t)acc;
                }
                return 1;
            }
            /* SMULxy: 0001 0110 Rd 0000 Rs 1yx0 Rm */
            if ((insn & 0x0FF00090u) == 0x01600080u) {
                uint32_t rd = (insn >> 16) & 0xFu;
                int x = (insn >> 5) & 1u, y = (insn >> 6) & 1u;
                int32_t m = (int32_t)(x ? (c->r[insn & 0xFu] >> 16)
                                        : (c->r[insn & 0xFu] & 0xFFFFu));
                int32_t s2 = (int32_t)(y ? (c->r[(insn >> 8) & 0xFu] >> 16)
                                         : (c->r[(insn >> 8) & 0xFu] & 0xFFFFu));
                c->r[rd] = (uint32_t)(m * s2);
                return 1;
            }
        }
    }

    /* ---- MSR immediate (001 space) ------------------------------------------- */
    if ((insn & 0x0FB00000u) == 0x03200000u) {
        uint32_t mask = ((insn >> 16) & 0xFu) << 16;
        uint32_t imm = insn & 0xFFu;
        uint32_t rot = ((insn >> 8) & 0xFu) * 2u;
        uint32_t val = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
        msr_write(c, val, mask, (insn >> 22) & 1u);
        return 1;
    }

    /* ---- single data transfer (word/byte) ------------------------------------ */
    if (op >= 0x4u && op <= 0x7u) {
        int p = (insn >> 24) & 1, u = (insn >> 23) & 1;
        int b = (insn >> 22) & 1, w = (insn >> 21) & 1;
        int l = (insn >> 20) & 1;
        uint32_t rn = (insn >> 16) & 0xFu, rd = (insn >> 12) & 0xFu;
        uint32_t base = c->r[rn];
        uint32_t off;
        if ((insn >> 25) & 1u) {
            /* register offset (optionally shifted by immediate) */
            int type = (insn >> 5) & 3u;
            uint32_t amount = (insn >> 7) & 0x1Fu;
            uint32_t co;
            off = shift_val(c, type, c->r[insn & 0xFu], amount, 0, &co);
        } else {
            off = insn & 0xFFFu;
        }
        uint32_t addr = p ? base + (u ? off : 0u - off) : base;
        if (l) {
            if (b)
                c->r[rd] = rd8(c, addr);
            else
                c->r[rd] = rd32(c, addr);
        } else {
            if (b)
                c->bus->write8(c->bus->user, addr, (uint8_t)c->r[rd]);
            else
                c->bus->write32(c->bus->user, addr & ~3u, c->r[rd]);
        }
        if (!p)
            c->r[rn] = base + (u ? off : 0u - off);
        else if (w)
            c->r[rn] = addr;
        if (l && rd == 15)
            c->pc = c->r[15] & ~3u;
        return 1;
    }

    /* ---- block transfer ------------------------------------------------------ */
    if (op == 0x8u || op == 0x9u) {
        ldm_stm(c, insn);
        return 1;
    }

    /* ---- branch -------------------------------------------------------------- */
    if (op == 0xAu || op == 0xBu) {
        int32_t off = (int32_t)(insn & 0x00FFFFFFu) << 2;
        if (off & 0x02000000u)
            off |= (int32_t)0xFC000000u;
        if (op == 0xBu)
            c->r[14] = cur + 4u;
        c->pc = cur + 8u + (uint32_t)off;
        return 1;
    }

    /* ---- coprocessor --------------------------------------------------------- */
    if (op == 0xCu || op == 0xDu) { /* LDC/STC: not implemented */
        take_exception(c, ARM_VEC_UND, cur + 4u, ARM_MODE_UND, ARM_F_I);
        return 1;
    }
    if (op == 0xEu && (insn & 0x10u)) { /* MRC/MCR */
        int cp = (int)((insn >> 8) & 0xFu);
        int opc1 = (int)((insn >> 21) & 7u);
        int crn = (int)((insn >> 16) & 0xFu);
        int rd = (int)((insn >> 12) & 0xFu);
        int opc2 = (int)((insn >> 5) & 7u);
        int crm = (int)(insn & 0xFu);
        int handled = 0;
        if (cp == 15 && c->cp15 != NULL) {
            if ((insn >> 20) & 1u) { /* MRC */
                uint32_t out = 0;
                handled = c->cp15->mrc(c->bus->user, cp, opc1, crn, rd, opc2,
                                       crm, &out);
                if (handled) {
                    if (rd != 15)
                        c->r[rd] = out;
                }
            } else {
                handled = c->cp15->mcr(c->bus->user, cp, opc1, crn, rd, opc2,
                                       crm, c->r[rd]);
            }
        }
        if (!handled) {
            take_exception(c, ARM_VEC_UND, cur + 4u, ARM_MODE_UND, ARM_F_I);
        }
        return 1;
    }

    /* ---- data processing (also covers 000-space fallthrough) ----------------- */
    {
        int is_imm = (insn >> 25) & 1;
        uint32_t carry;
        uint32_t b = dp_operand(c, insn, is_imm, &carry);
        uint32_t a = c->r[(insn >> 16) & 0xFu];
        dp_exec(c, insn, a, b, carry, cur);
        return 1;
    }
}

/* ---- one Thumb instruction ------------------------------------------------------ */

static void thumb_set_nzc_add(arm_t *c, uint32_t a, uint32_t b, uint32_t res)
{
    c->cpsr &= ~(ARM_F_N | ARM_F_Z | ARM_F_C | ARM_F_V);
    if (res & 0x80000000u) c->cpsr |= ARM_F_N;
    if (res == 0) c->cpsr |= ARM_F_Z;
    if (res < a) c->cpsr |= ARM_F_C;
    if ((~(a ^ b) & (a ^ res)) >> 31) c->cpsr |= ARM_F_V;
}

static uint32_t thumb_exec(arm_t *c, uint16_t insn, uint32_t cur)
{
    uint32_t op5 = (insn >> 11) & 0x1Fu;

    /* format 1: shifted immediate (000xx); 00011 = format 2 */
    if (op5 < 0x3u) {
        uint32_t rd = insn & 7u, rm = (insn >> 3) & 7u;
        uint32_t imm = (insn >> 6) & 0x1Fu;
        uint32_t co;
        uint32_t v;
        if (op5 == 0x0u)
            v = shift_val(c, 0, c->r[rm], imm, 0, &co);
        else if (op5 == 0x1u)
            v = shift_val(c, 1, c->r[rm], imm, 0, &co);
        else
            v = shift_val(c, 2, c->r[rm], imm, 0, &co);
        c->r[rd] = v;
        set_nzcv(c, v >> 31, v == 0, co, (c->cpsr & ARM_F_V) ? 1u : 0u);
        return 1;
    }
    if (op5 == 0x3u) { /* ADD/SUB reg or 3-bit immediate */
        uint32_t rd = insn & 7u, rs = (insn >> 3) & 7u;
        uint32_t opb = (insn >> 9) & 1u;   /* 0 = ADD, 1 = SUB */
        uint32_t immf = (insn >> 10) & 1u; /* 0 = register, 1 = imm3 */
        uint32_t b = immf ? ((insn >> 6) & 7u) : c->r[(insn >> 6) & 7u];
        uint32_t a = c->r[rs];
        if (!opb) {
            uint32_t res = a + b;
            c->r[rd] = res;
            thumb_set_nzc_add(c, a, b, res);
        } else {
            uint32_t res = a - b;
            c->r[rd] = res;
            c->cpsr &= ~(ARM_F_N | ARM_F_Z | ARM_F_C | ARM_F_V);
            if (res & 0x80000000u) c->cpsr |= ARM_F_N;
            if (res == 0) c->cpsr |= ARM_F_Z;
            if (a >= b) c->cpsr |= ARM_F_C;
            if ((a ^ b) & (a ^ res) >> 31) c->cpsr |= ARM_F_V;
        }
        return 1;
    }

    /* format 3: MOV/CMP/ADD/SUB 8-bit immediate */
    if (op5 >= 0x4u && op5 <= 0x7u) {
        uint32_t rd = (insn >> 8) & 7u;
        uint32_t imm = insn & 0xFFu;
        uint32_t a = c->r[rd];
        switch (op5) {
        case 0x4: /* MOV */
            c->r[rd] = imm;
            set_nzcv(c, imm >> 31, imm == 0, (c->cpsr & ARM_F_C) ? 1u : 0u,
                     (c->cpsr & ARM_F_V) ? 1u : 0u);
            break;
        case 0x5: { /* CMP */
            uint32_t res = a - imm;
            set_nzcv(c, res >> 31, res == 0, a >= imm,
                     ((a ^ imm) & (a ^ res)) >> 31);
            break;
        }
        case 0x6: { /* ADD */
            uint32_t res = a + imm;
            c->r[rd] = res;
            thumb_set_nzc_add(c, a, imm, res);
            break;
        }
        case 0x7: { /* SUB */
            uint32_t res = a - imm;
            c->r[rd] = res;
            c->cpsr &= ~(ARM_F_N | ARM_F_Z | ARM_F_C | ARM_F_V);
            if (res & 0x80000000u) c->cpsr |= ARM_F_N;
            if (res == 0) c->cpsr |= ARM_F_Z;
            if (a >= imm) c->cpsr |= ARM_F_C;
            if ((a ^ imm) & (a ^ res) >> 31) c->cpsr |= ARM_F_V;
            break;
        }
        default:
            break;
        }
        return 1;
    }

    /* format 4 (ALU, bit10=0) and format 5 (Hi ops / BX, bit10=1) */
    if (op5 == 0x08u) {
        if (!((insn >> 10) & 1u)) {
            uint32_t rd = insn & 7u, rm = (insn >> 3) & 7u;
            uint32_t op = (insn >> 6) & 0xFu;
            uint32_t co;
            uint32_t a = c->r[rd], b = c->r[rm];
            switch (op) {
            case 0x0: c->r[rd] = a & b;  set_logic_nz(c, a & b); break;
            case 0x1: c->r[rd] = a ^ b;  set_logic_nz(c, a ^ b); break;
            case 0x2: { uint32_t v = shift_val(c, 0, a, b & 0xFFu, 0, &co);
                        c->r[rd] = v; set_nzcv(c, v >> 31, v == 0, co, (c->cpsr & ARM_F_V) ? 1u : 0u); break; }
            case 0x3: { uint32_t v = shift_val(c, 1, a, b & 0xFFu, 0, &co);
                        c->r[rd] = v; set_nzcv(c, v >> 31, v == 0, co, (c->cpsr & ARM_F_V) ? 1u : 0u); break; }
            case 0x4: { uint32_t v = shift_val(c, 2, a, b & 0xFFu, 0, &co);
                        c->r[rd] = v; set_nzcv(c, v >> 31, v == 0, co, (c->cpsr & ARM_F_V) ? 1u : 0u); break; }
            case 0x5: { /* ADC */
                uint32_t ci = (c->cpsr & ARM_F_C) ? 1u : 0u;
                uint64_t r64 = (uint64_t)a + b + ci;
                uint32_t res = (uint32_t)r64;
                c->r[rd] = res;
                set_nzcv(c, res >> 31, res == 0, (uint32_t)(r64 >> 32),
                         (~(a ^ b) & (a ^ res)) >> 31);
                break;
            }
            case 0x6: { /* SBC */
                uint32_t ci = (c->cpsr & ARM_F_C) ? 0u : 1u;
                uint32_t res = a - b - ci;
                c->r[rd] = res;
                set_nzcv(c, res >> 31, res == 0, ci ? (a > b) : (a >= b),
                         ((a ^ b) & (a ^ res)) >> 31);
                break;
            }
            case 0x7: { uint32_t v = shift_val(c, 3, a, b & 0xFFu, 0, &co);
                        c->r[rd] = v; set_nzcv(c, v >> 31, v == 0, co, (c->cpsr & ARM_F_V) ? 1u : 0u); break; }
            case 0x8: set_logic_nz(c, a & b); break;                      /* TST */
            case 0x9: { uint32_t res = 0u - b; c->r[rd] = res;            /* NEG */
                        set_nzcv(c, res >> 31, res == 0, b == 0,
                                 (b & res) >> 31);
                        break; }
            case 0xA: { uint32_t res = a - b;                             /* CMP */
                        set_nzcv(c, res >> 31, res == 0, a >= b,
                                 ((a ^ b) & (a ^ res)) >> 31);
                        break; }
            case 0xB: { uint32_t res = a + b;                             /* CMN */
                        thumb_set_nzc_add(c, a, b, res);
                        break; }
            case 0xC: c->r[rd] = a | b;  set_logic_nz(c, a | b); break;   /* ORR */
            case 0xD: { uint32_t res = a * b; c->r[rd] = res;             /* MUL */
                        set_logic_nz(c, res);
                        break; }
            case 0xE: c->r[rd] = a & ~b; set_logic_nz(c, a & ~b); break;  /* BIC */
            case 0xF: c->r[rd] = ~b;     set_logic_nz(c, ~b); break;      /* MVN */
            default: break;
            }
            return 1;
        }
        /* format 5: Hi register operations / BX */
        {
            uint32_t op = (insn >> 8) & 3u;
            int h1 = (insn >> 7) & 1u, h2 = (insn >> 6) & 1u;
            uint32_t rd = (insn & 7u) | ((uint32_t)h1 << 3);
            uint32_t rm = ((insn >> 3) & 7u) | ((uint32_t)h2 << 3);
            uint32_t a = c->r[rd], b = c->r[rm];
            switch (op) {
            case 0x0: /* ADD */
                c->r[rd] = a + b;
                if (rd == 15)
                    c->pc = c->r[15] & ~1u; /* v4T: no interworking */
                break;
            case 0x1: { /* CMP */
                uint32_t res = a - b;
                set_nzcv(c, res >> 31, res == 0, a >= b,
                         ((a ^ b) & (a ^ res)) >> 31);
                break;
            }
            case 0x2: /* MOV */
                c->r[rd] = b;
                if (rd == 15)
                    c->pc = c->r[15] & ~1u;
                break;
            case 0x3: /* BX */
                if (b & 1u) {
                    c->cpsr |= ARM_F_T;
                    c->pc = b & ~1u;
                } else {
                    c->cpsr &= ~ARM_F_T;
                    c->pc = b & ~3u;
                }
                break;
            default:
                break;
            }
            return 1;
        }
    }

    /* format 6: PC-relative load */
    if (op5 == 0x09u) {
        uint32_t rd = (insn >> 8) & 7u;
        uint32_t addr = ((cur + 4u) & ~3u) + ((insn & 0xFFu) << 2);
        c->r[rd] = rd32(c, addr);
        return 1;
    }

    /* formats 7/8: load/store with register offset (bits[15:9] = 0101 op) */
    if (op5 == 0x0Au) {
        uint32_t op = (insn >> 9) & 7u;
        uint32_t rd = insn & 7u, rb = (insn >> 3) & 7u, ro = (insn >> 6) & 7u;
        uint32_t addr = c->r[rb] + c->r[ro];
        switch (op) {
        case 0x0: c->bus->write32(c->bus->user, addr & ~3u, c->r[rd]); break; /* STR  */
        case 0x1: c->bus->write16(c->bus->user, addr & ~1u, (uint16_t)c->r[rd]); break;
        case 0x2: c->bus->write8(c->bus->user, addr, (uint8_t)c->r[rd]); break;
        case 0x3: c->r[rd] = (uint32_t)(int32_t)(int8_t)rd8(c, addr); break;
        case 0x4: c->r[rd] = rd32(c, addr); break;
        case 0x5: c->r[rd] = rd16(c, addr); break;
        case 0x6: c->r[rd] = rd8(c, addr); break;
        case 0x7: c->r[rd] = (uint32_t)(int32_t)(int16_t)rd16(c, addr); break;
        default: break;
        }
        return 1;
    }

    /* format 9: load/store with immediate offset (word/byte) */
    if (op5 >= 0x0Cu && op5 <= 0x0Fu) {
        uint32_t rd = insn & 7u, rn = (insn >> 3) & 7u;
        uint32_t imm = (insn >> 6) & 0x1Fu;
        int b = (insn >> 12) & 1u, l = (insn >> 11) & 1u;
        uint32_t off = b ? imm : imm << 2;
        uint32_t addr = c->r[rn] + off;
        if (l) {
            if (b)
                c->r[rd] = rd8(c, addr);
            else
                c->r[rd] = rd32(c, addr);
        } else {
            if (b)
                c->bus->write8(c->bus->user, addr, (uint8_t)c->r[rd]);
            else
                c->bus->write32(c->bus->user, addr & ~3u, c->r[rd]);
        }
        return 1;
    }

    /* format 10: load/store halfword immediate */
    if (op5 == 0x10u || op5 == 0x11u) {
        uint32_t rd = insn & 7u, rn = (insn >> 3) & 7u;
        uint32_t off = ((insn >> 6) & 0x1Fu) << 1;
        uint32_t addr = c->r[rn] + off;
        if ((insn >> 11) & 1u)
            c->r[rd] = rd16(c, addr);
        else
            c->bus->write16(c->bus->user, addr & ~1u, (uint16_t)c->r[rd]);
        return 1;
    }

    /* format 11: SP-relative load/store */
    if (op5 == 0x12u || op5 == 0x13u) {
        uint32_t rd = (insn >> 8) & 7u;
        uint32_t addr = c->r[13] + ((insn & 0xFFu) << 2);
        if ((insn >> 11) & 1u)
            c->r[rd] = rd32(c, addr);
        else
            c->bus->write32(c->bus->user, addr & ~3u, c->r[rd]);
        return 1;
    }

    /* format 12: load address (PC/SP relative) */
    if (op5 == 0x14u || op5 == 0x15u) {
        uint32_t rd = (insn >> 8) & 7u;
        uint32_t base = ((insn >> 11) & 1u) ? c->r[13]
                                            : ((cur + 4u) & ~3u);
        c->r[rd] = base + ((insn & 0xFFu) << 2);
        return 1;
    }

    /* format 13: add offset to stack pointer */
    if ((insn & 0xFF00u) == 0xB000u) {
        uint32_t off = (insn & 0x7Fu) << 2;
        c->r[13] += ((insn & 0x80u) ? 0u - off : off);
        return 1;
    }

    /* format 14: push/pop */
    if ((insn & 0xFE00u) == 0xB400u) { /* PUSH */
        uint32_t list = insn & 0xFFu;
        uint32_t addr = c->r[13];
        int count = 0;
        for (int i = 0; i < 8; i++)
            if (list & (1u << i))
                count++;
        if ((insn >> 8) & 1u)
            count++;
        addr -= 4u * (uint32_t)count;
        c->r[13] = addr;
        for (int i = 0; i < 8; i++) {
            if (!(list & (1u << i)))
                continue;
            c->bus->write32(c->bus->user, addr & ~3u, c->r[i]);
            addr += 4u;
        }
        if ((insn >> 8) & 1u) {
            c->bus->write32(c->bus->user, addr & ~3u, c->r[14]);
        }
        return 1;
    }
    if ((insn & 0xFE00u) == 0xBC00u) { /* POP */
        uint32_t list = insn & 0xFFu;
        uint32_t addr = c->r[13];
        for (int i = 0; i < 8; i++) {
            if (!(list & (1u << i)))
                continue;
            c->r[i] = rd32(c, addr);
            addr += 4u;
        }
        if ((insn >> 8) & 1u) {
            c->pc = rd32(c, addr) & ~1u; /* v4T: stay in Thumb */
            addr += 4u;
        }
        c->r[13] = addr;
        return 1;
    }

    /* format 15: multiple load/store */
    if (op5 == 0x18u || op5 == 0x19u) {
        uint32_t rb = (insn >> 8) & 7u;
        uint32_t list = insn & 0xFFu;
        uint32_t addr = c->r[rb];
        int l = (insn >> 11) & 1u;
        for (int i = 0; i < 8; i++) {
            if (!(list & (1u << i)))
                continue;
            if (l)
                c->r[i] = rd32(c, addr);
            else
                c->bus->write32(c->bus->user, addr & ~3u, c->r[i]);
            addr += 4u;
        }
        c->r[rb] = addr; /* writeback always */
        return 1;
    }

    /* format 16/17: conditional branch / SWI (1101 cond) */
    if (op5 == 0x1Au || op5 == 0x1Bu) {
        uint32_t cond = (insn >> 8) & 0xFu;
        if (cond == 0xFu) { /* SWI */
            take_exception(c, ARM_VEC_SWI, cur + 2u, ARM_MODE_SVC, ARM_F_I);
            return 1;
        }
        if (cond == 0xEu) { /* UDF: permanently undefined */
            take_exception(c, ARM_VEC_UND, cur + 2u, ARM_MODE_UND, ARM_F_I);
            return 1;
        }
        if (cond_pass(c, cond)) {
            int32_t off = (int32_t)(insn & 0xFFu);
            if (off & 0x80u)
                off |= (int32_t)0xFFFFFF00u;
            c->pc = cur + 4u + ((uint32_t)off << 1);
        }
        return 1;
    }

    /* format 18: unconditional branch */
    if (op5 == 0x1Cu) {
        int32_t off = (int32_t)(insn & 0x7FFu);
        if (off & 0x400u)
            off |= (int32_t)0xFFFFF800u;
        c->pc = cur + 4u + ((uint32_t)off << 1);
        return 1;
    }

    /* format 19: BL / BLX long forms: handled by arm_step lookahead */
    return 1;
}

uint32_t arm_step(arm_t *c)
{
    uint32_t cur = c->pc;

    /* Sample interrupt lines before executing the next instruction. */
    if (c->fiq_line && !(c->cpsr & ARM_F_F)) {
        c->fiq_line = 0;
        take_exception(c, ARM_VEC_FIQ, cur + 4u, ARM_MODE_FIQ,
                       ARM_F_F | ARM_F_I);
        c->cycles += 1;
        return 1;
    }
    if (c->irq_line && !(c->cpsr & ARM_F_I)) {
        c->irq_line = 0;
        take_exception(c, ARM_VEC_IRQ, cur + 4u, ARM_MODE_IRQ, ARM_F_I);
        c->cycles += 1;
        return 1;
    }

    if (c->cpsr & ARM_F_T) {
        uint16_t hw1 = rd16(c, cur);
        if ((hw1 & 0xF800u) == 0xF000u) {
            /* long BL / BLX */
            uint16_t hw2 = rd16(c, cur + 2u);
            int32_t off = (int32_t)(hw1 & 0x7FFu);
            int32_t off2 = (int32_t)(hw2 & 0x7FFu);
            int32_t total = (off << 12) | (off2 << 1);
            if (total & 0x00400000u)
                total |= (int32_t)0xFF800000u;
            if ((hw2 & 0xF800u) == 0xD800u) { /* BL (v4T/v5) */
                c->r[14] = cur + 4u;
                c->pc = cur + 4u + (uint32_t)total;
            } else {
                /* BLX from Thumb (v5T) and undefined pairs are not
                 * modeled; the pair is skipped (documented) */
                c->pc = cur + 4u;
            }
            c->cycles += 1;
            return 1;
        }
        c->pc = cur + 2u;
        c->r[15] = cur + 4u;
        uint32_t cyc = thumb_exec(c, hw1, cur);
        c->cycles += cyc;
        return cyc;
    }

    uint32_t insn = rd32(c, cur);
    c->pc = cur + 4u;
    c->r[15] = cur + 8u;
    uint32_t cyc = arm_exec(c, insn, cur);
    c->cycles += cyc;
    return cyc;
}
