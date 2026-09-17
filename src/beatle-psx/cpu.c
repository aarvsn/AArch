/*
 * beatle-psx: MIPS R3000A CPU interpreter (MIPS I).
 *
 * Implements the full official user-mode instruction set, COP0 (MFC0/MTC0/
 * RFE, exception entry), the load delay slot and the single branch delay
 * slot. COP1 is absent on the R3000A and raises CoProcessorUnusable; COP2
 * dispatches to the GTE. HI/LO arithmetic follows the R3000 datasheet; DIV
 * by zero yields an unpredictable result per spec - this implementation
 * defines it as LO=HI=0 without trapping (documented).
 *
 * LWL/LWR/SWL/SWR use the little-endian semantics of the console.
 */
#include "psx.h"

/* exception cause codes */
#define PSX_EXC_INT   0u
#define PSX_EXC_ADEL  4u
#define PSX_EXC_ADES  5u
#define PSX_EXC_SYS   8u
#define PSX_EXC_BP    9u
#define PSX_EXC_RI    10u
#define PSX_EXC_CPU   11u
#define PSX_EXC_OV    12u

void psx_cpu_init(psx_cpu_t *c, struct psx *p)
{
    c->p = p;
    psx_cpu_reset(c);
}

void psx_cpu_reset(psx_cpu_t *c)
{
    for (int i = 0; i < 32; i++)
        c->r[i] = 0;
    c->exc_pc = 0;
    c->exc_in_delay = 0;
    c->hi = c->lo = 0;
    c->pc = 0x80000000u;
    c->next_pc = 0x80000004u;
    c->cop0_sr = 0;
    c->cop0_cause = 0;
    c->cop0_epc = 0;
    c->cop0_prid = 0x00000002u;
    c->cop0_badva = 0;
    c->ld_dreg = 0;
    c->ld_dval = 0;
    c->ld_new_dreg = 0;
    c->ld_new_dval = 0;
    c->cycles = 0;
}

static inline void wr_reg(psx_cpu_t *c, uint32_t rd, uint32_t v)
{
    if (rd != 0)
        c->r[rd] = v;
}

static inline uint32_t sext16(uint32_t v)
{
    return (uint32_t)(int32_t)(int16_t)v;
}

static inline uint32_t sext8(uint32_t v)
{
    return (uint32_t)(int32_t)(int8_t)v;
}

static void mult_signed(psx_cpu_t *c, uint32_t a, uint32_t b)
{
    int64_t rr = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    c->lo = (uint32_t)(uint64_t)rr;
    c->hi = (uint32_t)((uint64_t)rr >> 32);
}

static void mult_unsigned(psx_cpu_t *c, uint32_t a, uint32_t b)
{
    uint64_t rr = (uint64_t)a * (uint64_t)b;
    c->lo = (uint32_t)rr;
    c->hi = (uint32_t)(rr >> 32);
}

static void div_signed(psx_cpu_t *c, uint32_t a, uint32_t b)
{
    if (b == 0) {
        c->lo = 0;
        c->hi = 0;
        return;
    }
    if (a == 0x80000000u && b == 0xFFFFFFFFu) {
        c->lo = 0x80000000u; /* documented: no trap, quotient = a */
        c->hi = 0;
        return;
    }
    c->lo = (uint32_t)((int32_t)a / (int32_t)b);
    c->hi = (uint32_t)((int32_t)a % (int32_t)b);
}

static void div_unsigned(psx_cpu_t *c, uint32_t a, uint32_t b)
{
    if (b == 0) {
        c->lo = 0;
        c->hi = 0;
        return;
    }
    c->lo = a / b;
    c->hi = a % b;
}

/* ---- COP0 ------------------------------------------------------------------------ */

static uint32_t cop0_read(psx_cpu_t *c, uint32_t reg)
{
    struct psx *p = c->p;
    switch (reg) {
    case 8:  return c->cop0_badva;
    case 12: return c->cop0_sr;
    case 13:
        /* CAUSE bit10 mirrors (I_STAT & I_MASK) != 0 (not a latch). */
        if ((p->irq_istat & p->irq_imask) != 0)
            return c->cop0_cause | 0x400u;
        return c->cop0_cause & ~0x400u;
    case 14: return c->cop0_epc;
    case 15: return c->cop0_prid;
    default: return 0;
    }
}

static void cop0_write(psx_cpu_t *c, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case 12: c->cop0_sr = v; break;
    case 13:
        /* software interrupt bits 8/9 are write-able latches */
        c->cop0_cause = (c->cop0_cause & 0xFFFFFCFFu) | (v & 0x300u);
        break;
    default: break; /* other COP0 registers: writes ignored (documented) */
    }
}

static void cop0_rfe(psx_cpu_t *c)
{
    /* Restore IEp/KUp into IEc/KUc, IEo/KUo into IEp/KUp. */
    uint32_t mode = c->cop0_sr & 0x3Fu;
    c->cop0_sr = (c->cop0_sr & 0xFFFFFFC0u) | ((mode >> 2) & 0x0Fu) |
                 ((mode & 0x0Fu) << 2);
}

/* ---- main interpreter -------------------------------------------------------------- */

uint32_t psx_cpu_step(psx_cpu_t *c)
{
    struct psx *p = c->p;

    /* Apply the load result scheduled one instruction ago. */
    if (c->ld_dreg != 0)
        c->r[c->ld_dreg] = c->ld_dval;
    c->ld_dreg = c->ld_new_dreg;
    c->ld_dval = c->ld_new_dval;
    c->ld_new_dreg = 0;

    /* Hardware interrupt: CAUSE bit10 set while (I_STAT & I_MASK) != 0.
     * Taken when IEc=1 (kernel and user mode), priority over fetch. */
    if ((c->cop0_sr & 1u) && ((p->irq_istat & p->irq_imask) != 0)) {
        psx_cpu_exception(p, PSX_EXC_INT, 0);
        return 60u; /* approximate interrupt entry cost */
    }

    uint32_t op = psx_bus_read32(p, c->pc);
    uint32_t pc = c->pc;
    uint32_t cycles = 2u; /* documented coarse model: base 2 cycles */

    /* Snapshot the fetch address for exception EPC/BD before advancing. */
    c->exc_pc = pc;
    c->exc_in_delay = (c->next_pc != pc + 4u);

    c->pc = c->next_pc;
    c->next_pc = c->pc + 4u;

    uint32_t rs = (op >> 21) & 31u;
    uint32_t rt = (op >> 16) & 31u;
    uint32_t rd = (op >> 11) & 31u;
    uint32_t sa = (op >> 6) & 31u;
    uint32_t fn = op & 63u;
    uint32_t imm = op & 0xFFFFu;
    uint32_t simm = sext16(imm);
    uint32_t a = c->r[rs];
    uint32_t b = c->r[rt];

    switch (op >> 26) {
    case 0x00: /* SPECIAL */
        switch (fn) {
        case 0x00: wr_reg(c, rd, b << sa); break;                 /* SLL  */
        case 0x02: wr_reg(c, rd, b >> sa); break;                 /* SRL  */
        case 0x03: wr_reg(c, rd, (uint32_t)((int32_t)b >> sa)); break;
        case 0x04: wr_reg(c, rd, b << (a & 31u)); break;          /* SLLV */
        case 0x06: wr_reg(c, rd, b >> (a & 31u)); break;          /* SRLV */
        case 0x07:
            wr_reg(c, rd, (uint32_t)((int32_t)b >> (a & 31u)));
            break;                                                /* SRAV */
        case 0x08: /* JR */
            c->next_pc = a;
            break;
        case 0x09: /* JALR */
            c->next_pc = a;
            wr_reg(c, rd, pc + 8u);
            break;
        case 0x0C: /* SYSCALL */
            psx_cpu_exception(p, PSX_EXC_SYS, 0);
            return 10u;
        case 0x0D: /* BREAK */
            psx_cpu_exception(p, PSX_EXC_BP, 0);
            return 10u;
        case 0x10: wr_reg(c, rd, c->hi); break;                   /* MFHI */
        case 0x11: c->hi = a; break;                              /* MTHI */
        case 0x12: wr_reg(c, rd, c->lo); break;                   /* MFLO */
        case 0x13: c->lo = a; break;                              /* MTLO */
        case 0x18: mult_signed(c, a, b); cycles = 10; break;      /* MULT */
        case 0x19: mult_unsigned(c, a, b); cycles = 10; break;    /* MULTU*/
        case 0x1A: div_signed(c, a, b); cycles = 30; break;       /* DIV  */
        case 0x1B: div_unsigned(c, a, b); cycles = 30; break;     /* DIVU */
        case 0x20: /* ADD */
        case 0x21: { /* ADDU */
            uint32_t rr = a + b;
            if (fn == 0x20u && ((~(a ^ b) & (a ^ rr)) >> 31) != 0u) {
                psx_cpu_exception(p, PSX_EXC_OV, 0);
                return 10u;
            }
            wr_reg(c, rd, rr);
            break;
        }
        case 0x22: /* SUB */
        case 0x23: { /* SUBU */
            uint32_t rr = a - b;
            if (fn == 0x22u && (((a ^ b) & (a ^ rr)) >> 31) != 0u) {
                psx_cpu_exception(p, PSX_EXC_OV, 0);
                return 10u;
            }
            wr_reg(c, rd, rr);
            break;
        }
        case 0x24: wr_reg(c, rd, a & b); break;                   /* AND  */
        case 0x25: wr_reg(c, rd, a | b); break;                   /* OR   */
        case 0x26: wr_reg(c, rd, a ^ b); break;                   /* XOR  */
        case 0x27: wr_reg(c, rd, ~(a | b)); break;                /* NOR  */
        case 0x2A: wr_reg(c, rd, (int32_t)a < (int32_t)b); break; /* SLT  */
        case 0x2B: wr_reg(c, rd, a < b); break;                   /* SLTU */
        default:
            psx_cpu_exception(p, PSX_EXC_RI, 0);
            return 10u;
        }
        break;

    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: /* BLTZ */
            if ((int32_t)a < 0)
                c->next_pc = pc + 4u + (simm << 2);
            break;
        case 0x01: /* BGEZ */
            if ((int32_t)a >= 0)
                c->next_pc = pc + 4u + (simm << 2);
            break;
        case 0x10: /* BLTZAL (r31 written regardless of taken) */
            wr_reg(c, 31, pc + 8u);
            if ((int32_t)a < 0)
                c->next_pc = pc + 4u + (simm << 2);
            break;
        case 0x11: /* BGEZAL */
            wr_reg(c, 31, pc + 8u);
            if ((int32_t)a >= 0)
                c->next_pc = pc + 4u + (simm << 2);
            break;
        default:
            psx_cpu_exception(p, PSX_EXC_RI, 0);
            return 10u;
        }
        break;

    case 0x02: /* J */
        c->next_pc = (pc & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
        break;
    case 0x03: /* JAL */
        wr_reg(c, 31, pc + 8u);
        c->next_pc = (pc & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
        break;
    case 0x04: /* BEQ */
        if (a == b)
            c->next_pc = pc + 4u + (simm << 2);
        break;
    case 0x05: /* BNE */
        if (a != b)
            c->next_pc = pc + 4u + (simm << 2);
        break;
    case 0x06: /* BLEZ */
        if ((int32_t)a <= 0)
            c->next_pc = pc + 4u + (simm << 2);
        break;
    case 0x07: /* BGTZ */
        if ((int32_t)a > 0)
            c->next_pc = pc + 4u + (simm << 2);
        break;
    case 0x08: { /* ADDI */
        uint32_t rr = a + simm;
        if ((~(a ^ simm) & (a ^ rr)) >> 31) {
            psx_cpu_exception(p, PSX_EXC_OV, 0);
            return 10u;
        }
        wr_reg(c, rt, rr);
        break;
    }
    case 0x09: wr_reg(c, rt, a + simm); break;                   /* ADDIU */
    case 0x0A: wr_reg(c, rt, (int32_t)a < (int32_t)simm); break; /* SLTI  */
    case 0x0B: wr_reg(c, rt, a < simm); break;                   /* SLTIU */
    case 0x0C: wr_reg(c, rt, a & imm); break;                    /* ANDI  */
    case 0x0D: wr_reg(c, rt, a | imm); break;                    /* ORI   */
    case 0x0E: wr_reg(c, rt, a ^ imm); break;                    /* XORI  */
    case 0x0F: wr_reg(c, rt, imm << 16); break;                  /* LUI   */

    case 0x10: /* COP0 */
        if ((op & 0x02000000u) == 0) { /* CO=0: MFC0 (sub 0) / MTC0 (4) */
            uint32_t sub = rs;
            if (sub == 0x00u) {
                c->ld_new_dreg = rt;
                c->ld_new_dval = cop0_read(c, rd);
            } else if (sub == 0x04u) {
                cop0_write(c, rd, b);
            } else {
                psx_cpu_exception(p, PSX_EXC_RI, 0);
                return 10u;
            }
        } else { /* CO=1 */
            if ((op & 0x03FFFFFFu) == 0x02000010u) { /* RFE */
                cop0_rfe(c);
            } else {
                psx_cpu_exception(p, PSX_EXC_RI, 0);
                return 10u;
            }
        }
        break;

    case 0x11: /* COP1: absent on R3000A (CE=1) */
        c->cop0_cause = (c->cop0_cause & 0xCFFFFF3Fu) | (11u << 2) | (1u << 28);
        psx_cpu_exception(p, PSX_EXC_CPU, 0);
        return 10u;
    case 0x13: /* COP3: unused (CE=3) */
        c->cop0_cause = (c->cop0_cause & 0xCFFFFF3Fu) | (11u << 2) | (3u << 28);
        psx_cpu_exception(p, PSX_EXC_CPU, 0);
        return 10u;

    case 0x12: /* COP2: GTE */
        if (!(c->cop0_sr & 2u) && !(c->cop0_sr & 0x40000000u)) {
            /* user mode without CU2: coprocessor unusable (CE=2) */
            c->cop0_cause =
                (c->cop0_cause & 0xCFFFFF3Fu) | (11u << 2) | (2u << 28);
            psx_cpu_exception(p, PSX_EXC_CPU, 0);
            return 10u;
        }
        /* R3000A CP2 format: register moves use funct=0 with rs selecting
         * MFC2(0)/CFC2(2)/MTC2(4)/CTC2(6); every other funct is a GTE
         * command (rs bits are part of the opcode suffix then). */
        if (fn == 0u) {
            switch (rs) {
            case 0x00: /* MFC2 */
                c->ld_new_dreg = rt;
                c->ld_new_dval = psx_gte_read(&p->gte, rd);
                break;
            case 0x02: /* CFC2 */
                c->ld_new_dreg = rt;
                c->ld_new_dval = psx_gte_read(&p->gte, rd + 32u);
                break;
            case 0x04: /* MTC2 */
                psx_gte_write(&p->gte, rd, b);
                break;
            case 0x06: /* CTC2 */
                psx_gte_write(&p->gte, rd + 32u, b);
                break;
            default:
                psx_cpu_exception(p, PSX_EXC_RI, 0);
                return 10u;
            }
        } else {
            psx_gte_execute(&p->gte, op);
            cycles = 15;
        }
        break;

    case 0x20: /* LB */
        c->ld_new_dreg = rt;
        c->ld_new_dval = sext8(psx_bus_read8(p, a + simm));
        cycles = 3;
        break;
    case 0x21: /* LH */
        if ((a + simm) & 1u) {
            psx_cpu_exception(p, PSX_EXC_ADEL, a + simm);
            return 10u;
        }
        c->ld_new_dreg = rt;
        c->ld_new_dval = sext16(psx_bus_read16(p, a + simm));
        cycles = 3;
        break;
    case 0x22: { /* LWL */
        uint32_t addr = a + simm;
        uint32_t word = psx_bus_read32(p, addr & ~3u);
        uint32_t n = addr & 3u;
        uint32_t sh = 8u * (3u - n);
        uint32_t v = (word << sh) | (b & ((sh == 0) ? 0u : ((1u << sh) - 1u)));
        c->ld_new_dreg = rt;
        c->ld_new_dval = v;
        cycles = 3;
        break;
    }
    case 0x23: /* LW */
        if ((a + simm) & 3u) {
            psx_cpu_exception(p, PSX_EXC_ADEL, a + simm);
            return 10u;
        }
        c->ld_new_dreg = rt;
        c->ld_new_dval = psx_bus_read32(p, a + simm);
        cycles = 3;
        break;
    case 0x24: /* LBU */
        c->ld_new_dreg = rt;
        c->ld_new_dval = psx_bus_read8(p, a + simm);
        cycles = 3;
        break;
    case 0x25: /* LHU */
        if ((a + simm) & 1u) {
            psx_cpu_exception(p, PSX_EXC_ADEL, a + simm);
            return 10u;
        }
        c->ld_new_dreg = rt;
        c->ld_new_dval = psx_bus_read16(p, a + simm);
        cycles = 3;
        break;
    case 0x26: { /* LWR */
        uint32_t addr = a + simm;
        uint32_t word = psx_bus_read32(p, addr & ~3u);
        uint32_t n = addr & 3u;
        uint32_t v;
        if (n == 0)
            v = word;
        else
            v = (word >> (8u * n)) | (b & (0xFFFFFFFFu << (32u - 8u * n)));
        c->ld_new_dreg = rt;
        c->ld_new_dval = v;
        cycles = 3;
        break;
    }
    case 0x28: /* SB */
        psx_bus_write8(p, a + simm, (uint8_t)b);
        cycles = 3;
        break;
    case 0x29: /* SH */
        if ((a + simm) & 1u) {
            psx_cpu_exception(p, PSX_EXC_ADES, a + simm);
            return 10u;
        }
        psx_bus_write16(p, a + simm, (uint16_t)b);
        cycles = 3;
        break;
    case 0x2A: { /* SWL */
        uint32_t addr = a + simm;
        uint32_t n = addr & 3u;
        uint32_t aligned = addr & ~3u;
        uint32_t cur = psx_bus_read32(p, aligned);
        uint32_t sh = 8u * (3u - n);
        /* RT's high (n+1) bytes replace memory bytes 0..n */
        uint32_t byte_mask =
            n == 3 ? 0xFFFFFFFFu : ((1u << (8u * (n + 1u))) - 1u);
        uint32_t v = (cur & ~byte_mask) | ((b >> sh) & byte_mask);
        psx_bus_write32(p, aligned, v);
        cycles = 3;
        break;
    }
    case 0x2B: /* SW */
        if ((a + simm) & 3u) {
            psx_cpu_exception(p, PSX_EXC_ADES, a + simm);
            return 10u;
        }
        psx_bus_write32(p, a + simm, b);
        cycles = 3;
        break;
    case 0x2E: { /* SWR */
        uint32_t addr = a + simm;
        uint32_t n = addr & 3u;
        uint32_t aligned = addr & ~3u;
        uint32_t cur = psx_bus_read32(p, aligned);
        uint32_t keep = (n == 0) ? 0u : (0xFFFFFFFFu << (32u - 8u * n));
        uint32_t v = (cur & keep) | (b << (8u * n));
        psx_bus_write32(p, aligned, v);
        cycles = 3;
        break;
    }
    default:
        psx_cpu_exception(p, PSX_EXC_RI, 0);
        return 10u;
    }

    c->cycles += cycles;
    return cycles;
}
