/*
 * m64-b: Nintendo 64 machine + R4300i interpreter. See n64.h for the
 * verified scope, boot model and documented simplifications.
 */
#include "n64.h"

#include <stdlib.h>
#include <string.h>

#include "../common/util.h"

/* ---- memory primitives (big-endian bus) --------------------------------------- */

static uint32_t phys(struct n64 *n, uint32_t va, int *mapped)
{
    uint32_t st = n->cp0[N64_CP0_STATUS];
    (void)n;
    *mapped = 1;
    if (va < 0x80000000u) {
        if (st & N64_ST_ERL)
            return va & 0x1FFFFFFFu; /* ERL: kuseg unmapped, uncached */
        *mapped = 0; /* TLB not implemented (documented) */
        return va & 0x1FFFFFFFu;
    }
    if (va < 0xA0000000u)
        return va - 0x80000000u; /* kseg0 */
    if (va < 0xC0000000u)
        return va - 0xA0000000u; /* kseg1 */
    *mapped = 0; /* kseg2/3: TLB not implemented */
    return va & 0x1FFFFFFFu;
}

static uint8_t *n64_mem_ptr(struct n64 *n, uint32_t pa)
{
    if (pa < 0x400000u)
        return &n->rdram[pa];
    if (pa >= 0x04000000u && pa < 0x04001000u)
        return &n->dmem[pa - 0x04000000u];
    if (pa >= 0x04001000u && pa < 0x04002000u)
        return &n->imem[pa - 0x04001000u];
    if (pa >= 0x10000000u && pa < 0x10000000u + n->rom_size)
        return &n->rom[pa - 0x10000000u];
    if (pa >= 0x1FC00000u && pa < 0x1FC00800u)
        return &n->pif_rom[pa - 0x1FC00000u];
    return NULL;
}

/* ---- bus (32-bit; 8/16-bit accessors route through it) ------------------------- */

static uint32_t reg_read(struct n64 *n, uint32_t pa)
{
    switch (pa & 0x0FFFFFFFu) {
    case 0x04300000u: return n->mi[0]; /* MI_INIT_MODE */
    case 0x04300004u: return 0x02020102u; /* MI_VERSION (storage) */
    case 0x04300008u: return n->mi[2]; /* MI_INTR */
    case 0x0430000Cu: return n->mi[3]; /* MI_INTR_MASK */
    case 0x04400000u: return n->vi[0];
    case 0x04400004u: return n->vi[1];
    case 0x04400008u: return n->vi[2];
    case 0x0440000Cu: return n->vi[3];
    case 0x04400010u: /* VI_V_CURRENT: current half-line (coarse model) */
        return n->vi_cur;
    case 0x04600000u: return n->pi[0];
    case 0x04600004u: return n->pi[1];
    case 0x04600008u: return n->pi[2];
    case 0x0460000Cu: return n->pi[3];
    case 0x04600010u: return 0; /* PI_STATUS: DMA idle */
    case 0x04800000u: return n->si[0];
    case 0x04800018u: return n->si[6];
    default:
        if ((pa & 0x0FF00000u) == 0x04000000u && (pa & 0x1FFFFu) >= 0x40000u)
            return n->sp_reg[(pa >> 2) & 0xFu];
        return 0;
    }
}

static void reg_write(struct n64 *n, uint32_t pa, uint32_t v);

static uint32_t n64_read32(struct n64 *n, uint32_t va, int *mapped_err)
{
    int mapped;
    uint32_t pa = phys(n, va, &mapped);
    if (!mapped) {
        if (mapped_err)
            *mapped_err = 1;
        return 0;
    }
    if (mapped_err)
        *mapped_err = 0;
    pa &= ~3u;
    uint8_t *p = n64_mem_ptr(n, pa);
    if (p != NULL)
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    return reg_read(n, pa);
}

static void n64_write32(struct n64 *n, uint32_t va, uint32_t v)
{
    int mapped;
    uint32_t pa = phys(n, va, &mapped);
    if (!mapped)
        return; /* TLB region: dropped (documented) */
    pa &= ~3u;
    uint8_t *p = n64_mem_ptr(n, pa);
    if (p != NULL) {
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
        return;
    }
    reg_write(n, pa, v);
}

uint32_t n64_bus_read32(struct n64 *n, uint32_t addr)
{
    return n64_read32(n, addr, NULL);
}

void n64_bus_write32(struct n64 *n, uint32_t addr, uint32_t v)
{
    n64_write32(n, addr, v);
}

/* ---- RCP registers ---------------------------------------------------------------- */

static void mi_raise(struct n64 *n, uint32_t bit)
{
    n->mi[2] |= bit; /* MI_INTR */
}

static int mi_cpu_interrupt(const struct n64 *n)
{
    return (n->mi[2] & n->mi[3]) != 0u; /* pending & masked */
}

static void pi_dma_start(struct n64 *n)
{
    uint32_t dram = n->pi[0] & 0x1FFFFFFEu; /* strip kseg bits, word align */
    uint32_t cart = n->pi[1] & 0x1FFFFFFEu;
    uint32_t len = (n->pi[2] & 0x00FFFFFFu) + 1u;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t d = dram + i, c = cart + i;
        uint8_t v = 0;
        if (c >= 0x10000000u && c - 0x10000000u < n->rom_size)
            v = n->rom[c - 0x10000000u];
        else if ((c & 0x1FC00000u) == 0x1FC00000u)
            v = (c - 0x1FC00000u) < 0x800u ? n->pif_rom[c - 0x1FC00000u] : 0;
        if (d < 0x400000u)
            n->rdram[d] = v;
    }
    mi_raise(n, N64_MI_PI);
}

static void si_dma_start(struct n64 *n, int to_pif)
{
    uint32_t dram = n->si[0] & 0x007FFFF0u;
    for (int i = 0; i < 64; i++) {
        if (to_pif) {
            if (dram + (uint32_t)i < 0x400000u)
                n->pif_ram[i] = n->rdram[dram + (uint32_t)i];
        } else if (dram + (uint32_t)i < 0x400000u) {
            n->rdram[dram + (uint32_t)i] = n->pif_ram[i];
        }
    }
    mi_raise(n, N64_MI_SI);
}

static void reg_write(struct n64 *n, uint32_t pa, uint32_t v)
{
    switch (pa & 0x0FFFFFFFu) {
    case 0x04300000u: n->mi[0] = v & 0x7Fu; break;
    case 0x04300008u: n->mi[2] &= ~v; break; /* write 1 to clear */
    case 0x0430000Cu:
        /* INTR_MASK: low byte sets mask bits, second byte clears them */
        n->mi[3] = (n->mi[3] | (v & 0xFFu)) & ~((v >> 8) & 0xFFu);
        break;
    case 0x04400000u: n->vi[0] = v; break;
    case 0x04400004u: n->vi[1] = v & 0x00FFFFF0u; break;
    case 0x04400008u: n->vi[2] = v & 0x00000FFFu; break;
    case 0x0440000Cu: n->vi[3] = v & 0x000003FFu; break;
    case 0x04400010u: n->vi_intr_pending = 0; break; /* clear VI intr */
    default:
        if (pa >= 0x04400000u && pa < 0x04400040u) {
            n->vi[(pa - 0x04400000u) >> 2] = v;
            break;
        }
        if (pa >= 0x04600000u && pa < 0x04600018u) {
            uint32_t idx = (pa - 0x04600000u) >> 2;
            n->pi[idx] = v;
            if (idx == 2)
                pi_dma_start(n); /* PI_RD_LEN write: cart -> RDRAM */
            break;
        }
        if (pa >= 0x04800000u && pa < 0x04800020u) {
            uint32_t idx = (pa - 0x04800000u) >> 2;
            n->si[idx] = v;
            if (idx == 0)
                break;
            if (idx == 4)
                si_dma_start(n, 1); /* SI_PIF_ADDR_WRITE64B */
            if (idx == 5)
                si_dma_start(n, 0); /* SI_PIF_ADDR_READ64B */
            break;
        }
        if ((pa & 0x0FF00000u) == 0x04000000u && (pa & 0x1FFFFu) >= 0x40000u)
            n->sp_reg[(pa >> 2) & 0xFu] = v;
        break;
    }
}

/* ---- exceptions ------------------------------------------------------------------ */

static void exception(struct n64 *n, uint32_t code)
{
    uint32_t st = n->cp0[N64_CP0_STATUS];
    uint32_t cause = n->cp0[N64_CP0_CAUSE] & ~0x7Fu;
    if (n->in_delay)
        cause |= 0x80000000u; /* BD */
    cause = (cause & ~0x1Fu) | code;
    n->cp0[N64_CP0_CAUSE] = cause;
    n->cp0[N64_CP0_EPC] = n->in_delay ? n->pc - 4u : n->pc;
    /* clear ERL on exception entry: with the TLB unimplemented the
     * bootstrap mapping must not mask EPC-based returns (documented) */
    st = (st & ~N64_ST_ERL) | N64_ST_EXL;
    n->cp0[N64_CP0_STATUS] = st;
    uint32_t vector = (st & N64_ST_BEV) ? 0xBFC00200u : 0x80000180u;
    n->pc = vector;
    n->next_pc = vector + 4u;
    n->in_delay = 0;
}

/* ---- CP0 ----------------------------------------------------------------------- */

static uint32_t cp0_read(struct n64 *n, uint32_t reg)
{
    switch (reg) {
    case N64_CP0_PRID: return 0x00000B22u; /* VR4300 */
    case N64_CP0_CONFIG: return n->cp0[reg] | 0x8000u; /* BE = 1 */
    case N64_CP0_COUNT: return (uint32_t)(n->cycles >> 1);
    default: return n->cp0[reg];
    }
}

static void cp0_write(struct n64 *n, uint32_t reg, uint32_t v)
{
    switch (reg) {
    case N64_CP0_COUNT: n->cycles = (uint64_t)(v << 1); break;
    case N64_CP0_COMPARE:
        n->cp0[N64_CP0_COMPARE] = v;
        n->cp0[N64_CP0_CAUSE] &= ~0x00008000u; /* IP7 clear */
        break;
    case N64_CP0_CAUSE:
        n->cp0[N64_CP0_CAUSE] =
            (n->cp0[N64_CP0_CAUSE] & ~0x00008000u) | (v & 0x00008000u);
        break;
    case N64_CP0_CONFIG:
        n->cp0[reg] = v & ~0x8000u; /* BE reads as 1 */
        break;
    default: n->cp0[reg] = v; break;
    }
}

/* ---- CPU: helpers --------------------------------------------------------------- */

#define S32(x) ((int64_t)(int32_t)(x))
#define U32(x) ((uint32_t)(x))
/* ---- CPU: one instruction --------------------------------------------------------- */

void n64_step(struct n64 *n)
{
    /* interrupt sampling (documented: IP3 = MI, IP7 = counter compare) */
    uint32_t st = n->cp0[N64_CP0_STATUS];
    uint32_t ip = 0;
    if (mi_cpu_interrupt(n))
        ip |= 0x00000800u; /* IP3 */
    if (n->cp0[N64_CP0_CAUSE] & 0x00008000u)
        ip |= 0x00008000u; /* IP7 */
    if (ip & (st & N64_ST_IMASK) && (st & N64_ST_IE) &&
        !(st & (N64_ST_EXL | N64_ST_ERL)))
        exception(n, N64_EXC_INT);

    uint32_t op = n64_read32(n, n->pc, NULL);
    uint32_t pc = n->pc;
    n->pc = n->next_pc;
    n->next_pc = n->pc + 4u;
    int was_delay = n->in_delay;
    n->in_delay = 0;

    uint32_t rs = (op >> 21) & 31u, rt = (op >> 16) & 31u, rd = (op >> 11) & 31u;
    uint32_t sa = (op >> 6) & 31u;
    uint32_t fn = op & 0x3Fu;
    uint32_t imm = op & 0xFFFFu;
    uint32_t simm = (uint32_t)(int32_t)(int16_t)imm;
    uint64_t target = (pc & 0xFFFFFFFFF0000000ull) |
                      ((uint64_t)((op >> 0) & 0x03FFFFFFu) << 2);

    /* Count: increments at half the pipeline rate (documented model) */
    n->cycles += 2;
    if ((uint32_t)(n->cycles >> 1) == n->cp0[N64_CP0_COMPARE]) {
        n->cp0[N64_CP0_CAUSE] |= 0x00008000u; /* IP7 */
    }

#define BR(c)                     \
    do {                          \
        n->next_pc = (c) ? (uint32_t)(pc + 4u + (simm << 2)) : n->next_pc; \
        n->in_delay = 1;          \
    } while (0)

    switch (op >> 26) {
    case 0x00: /* SPECIAL */
        switch (fn) {
        case 0x00: /* SLL */ if (op) n->r[rd] = (uint64_t)(uint32_t)(U32(n->r[rt]) << sa); break;
        case 0x02: /* SRL */ n->r[rd] = U32(n->r[rt]) >> sa; break;
        case 0x03: /* SRA */ n->r[rd] = (uint64_t)(int64_t)(S32(n->r[rt]) >> sa); break;
        case 0x04: /* SLLV */ n->r[rd] = (uint64_t)(uint32_t)(U32(n->r[rt]) << (U32(n->r[rs]) & 31u)); break;
        case 0x06: /* SRLV */ n->r[rd] = U32(n->r[rt]) >> (U32(n->r[rs]) & 31u); break;
        case 0x07: /* SRAV */ n->r[rd] = (uint64_t)(int64_t)(S32(n->r[rt]) >> (U32(n->r[rs]) & 31u)); break;
        case 0x08: /* JR */ n->next_pc = U32(n->r[rs]); n->in_delay = 1; break;
        case 0x09: /* JALR */
            n->r[rd] = pc + 8u;
            n->next_pc = U32(n->r[rs]);
            n->in_delay = 1;
            break;
        case 0x0C: /* SYSCALL */ exception(n, N64_EXC_SYS); return;
        case 0x0D: /* BREAK */ exception(n, N64_EXC_BP); return;
        case 0x0F: /* SYNC */ break;
        case 0x10: /* MFHI */ n->r[rd] = n->hi; break;
        case 0x11: /* MTHI */ n->hi = n->r[rs]; break;
        case 0x12: /* MFLO */ n->r[rd] = n->lo; break;
        case 0x13: /* MTLO */ n->lo = n->r[rs]; break;
        case 0x18: /* MULT */ {
            int64_t p = S32(n->r[rs]) * S32(n->r[rt]);
            n->lo = (uint64_t)(int64_t)(int32_t)(uint32_t)p;
            n->hi = (uint64_t)(int64_t)(int32_t)(uint32_t)(p >> 32);
            break;
        }
        case 0x19: /* MULTU */ {
            uint64_t p = U32(n->r[rs]) * (uint64_t)U32(n->r[rt]);
            n->lo = (uint32_t)p;
            n->hi = (uint32_t)(p >> 32);
            break;
        }
        case 0x1A: /* DIV */ {
            int32_t a = (int32_t)U32(n->r[rs]), b = (int32_t)U32(n->r[rt]);
            if (b != 0 && !(a == (-2147483647 - 1) && b == -1)) {
                n->lo = (uint32_t)(int32_t)(a / b);
                n->hi = (uint32_t)(int32_t)(a % b);
            } else if (a == (-2147483647 - 1) && b == -1) {
                n->lo = 0x80000000u;
                n->hi = 0;
            } else {
                n->lo = 0x80000000u;
                n->hi = 0;
            }
            break;
        }
        case 0x1B: /* DIVU */ {
            uint32_t a = U32(n->r[rs]), b = U32(n->r[rt]);
            if (b != 0) {
                n->lo = a / b;
                n->hi = a % b;
            }
            break;
        }
        case 0x20: /* ADD (overflow trap folded into ADDU; documented) */
        case 0x21: n->r[rd] = (uint64_t)(uint32_t)(U32(n->r[rs]) + U32(n->r[rt])); break;
        case 0x22: /* SUB */
        case 0x23: n->r[rd] = (uint64_t)(uint32_t)(U32(n->r[rs]) - U32(n->r[rt])); break;
        case 0x24: n->r[rd] = n->r[rs] & n->r[rt]; break;
        case 0x25: n->r[rd] = n->r[rs] | n->r[rt]; break;
        case 0x26: n->r[rd] = n->r[rs] ^ n->r[rt]; break;
        case 0x27: n->r[rd] = ~(n->r[rs] | n->r[rt]); break;
        case 0x2A: n->r[rd] = (S32(n->r[rs]) < S32(n->r[rt])) ? 1 : 0; break;
        case 0x2B: n->r[rd] = (n->r[rs] < n->r[rt]) ? 1 : 0; break;
        case 0x2C: /* TEQ folded into NOP (no trap semantics; documented) */ break;
        case 0x34: /* DSLLV */ n->r[rd] = n->r[rt] << (U32(n->r[rs]) & 63u); break;
        case 0x36: /* DSRLV */ n->r[rd] = n->r[rt] >> (U32(n->r[rs]) & 63u); break;
        case 0x37: /* DSRAV */ n->r[rd] = (uint64_t)((int64_t)n->r[rt] >> (U32(n->r[rs]) & 63u)); break;
        case 0x3C: /* DSLL32 */ n->r[rd] = n->r[rt] << (sa + 32u); break;
        case 0x3E: /* DSRL32 */ n->r[rd] = n->r[rt] >> (sa + 32u); break;
        case 0x3F: /* DSRA32 */ n->r[rd] = (uint64_t)((int64_t)n->r[rt] >> (sa + 32u)); break;
        default: exception(n, N64_EXC_RI); return;
        }
        break;

    case 0x01: /* REGIMM */
        switch (rt) {
        case 0x00: BR((int32_t)U32(n->r[rs]) < 0); break;              /* BLTZ */
        case 0x01: BR((int32_t)U32(n->r[rs]) >= 0); break;             /* BGEZ */
        case 0x10: { int32_t v = (int32_t)U32(n->r[rs]); n->r[31] = pc + 8u; BR(v < 0); break; }
        case 0x11: { int32_t v = (int32_t)U32(n->r[rs]); n->r[31] = pc + 8u; BR(v >= 0); break; }
        default: exception(n, N64_EXC_RI); return;
        }
        break;

    case 0x02: /* J */ n->next_pc = (uint32_t)target; n->in_delay = 1; break;
    case 0x03: /* JAL */ n->r[31] = pc + 8u; n->next_pc = (uint32_t)target; n->in_delay = 1; break;
    case 0x04: BR(n->r[rs] == n->r[rt]); break; /* BEQ */
    case 0x05: BR(n->r[rs] != n->r[rt]); break; /* BNE */
    case 0x06: BR((int32_t)U32(n->r[rs]) <= 0); break; /* BLEZ */
    case 0x07: BR((int32_t)U32(n->r[rs]) > 0); break;  /* BGTZ */
    case 0x08: /* ADDI (overflow folded; documented) */
    case 0x09: n->r[rt] = (uint64_t)(uint32_t)(U32(n->r[rs]) + simm); break;
    case 0x0A: n->r[rt] = (S32(n->r[rs]) < (int32_t)simm) ? 1 : 0; break;
    case 0x0B: n->r[rt] = (n->r[rs] < (uint64_t)simm) ? 1 : 0; break;
    case 0x0C: n->r[rt] = n->r[rs] & imm; break;
    case 0x0D: n->r[rt] = n->r[rs] | imm; break;
    case 0x0E: n->r[rt] = n->r[rs] ^ imm; break;
    case 0x0F: n->r[rt] = (uint64_t)(uint32_t)(imm << 16); break; /* LUI */
    case 0x10: /* COP0 */
        if (rs == 0x00) { /* MFC0 */ n->r[rt] = cp0_read(n, rd); }
        else if (rs == 0x04) { cp0_write(n, rd, U32(n->r[rt])); } /* MTC0 */
        else if (rs == 0x10 && fn == 0x18) { /* ERET */
            if (n->cp0[N64_CP0_STATUS] & N64_ST_ERL) {
                n->pc = n->cp0[N64_CP0_ERROREPC];
                n->cp0[N64_CP0_STATUS] &= ~N64_ST_ERL;
            } else {
                n->pc = n->cp0[N64_CP0_EPC];
                n->cp0[N64_CP0_STATUS] &= ~N64_ST_EXL;
            }
            n->next_pc = n->pc + 4u;
        } else { exception(n, N64_EXC_CPU); return; }
        break;
    case 0x14: /* BEQL (not-taken skips delay slot; implemented) */
        if (n->r[rs] == n->r[rt]) { BR(1); } else { n->pc = n->next_pc; n->next_pc = n->pc + 4u; }
        break;
    case 0x15: /* BNEL */
        if (n->r[rs] != n->r[rt]) { BR(1); } else { n->pc = n->next_pc; n->next_pc = n->pc + 4u; }
        break;
    case 0x1C: /* SPECIAL2: MUL (rd = lo32(rs*rt)) */
        if (fn == 0x02) {
            n->r[rd] = (uint64_t)(uint32_t)(S32(n->r[rs]) * S32(n->r[rt]));
        } else {
            exception(n, N64_EXC_RI);
            return;
        }
        break;
    case 0x20: /* LB */ {
        int err = 0;
        uint32_t pa_addr = n->pc; (void)pa_addr;
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t w = n64_read32(n, va & ~3u, &err);
        if (err) { exception(n, N64_EXC_ADEL); return; }
        n->r[rt] = (uint64_t)(int64_t)(int8_t)(w >> (24u - 8u * (va & 3u)));
        break;
    }
    case 0x21: /* LH */ {
        int err = 0;
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t w = n64_read32(n, va & ~3u, &err);
        if (err) { exception(n, N64_EXC_ADEL); return; }
        n->r[rt] = (uint64_t)(int64_t)(int16_t)(w >> (16u - 8u * (va & 2u)));
        break;
    }
    case 0x23: /* LW */ {
        int err = 0;
        uint32_t va = U32(n->r[rs]) + simm;
        if (va & 3u) { exception(n, N64_EXC_ADEL); return; }
        n->r[rt] = (uint64_t)(int32_t)n64_read32(n, va, &err);
        if (err) { exception(n, N64_EXC_ADEL); return; }
        break;
    }
    case 0x24: /* LBU */ {
        int err = 0;
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t w = n64_read32(n, va & ~3u, &err);
        if (err) { exception(n, N64_EXC_ADEL); return; }
        n->r[rt] = (w >> (24u - 8u * (va & 3u))) & 0xFFu;
        break;
    }
    case 0x25: /* LHU */ {
        int err = 0;
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t w = n64_read32(n, va & ~3u, &err);
        if (err) { exception(n, N64_EXC_ADEL); return; }
        n->r[rt] = (w >> (16u - 8u * (va & 2u))) & 0xFFFFu;
        break;
    }
    case 0x27: /* LWU */ {
        int err = 0;
        uint32_t va = U32(n->r[rs]) + simm;
        if (va & 3u) { exception(n, N64_EXC_ADEL); return; }
        n->r[rt] = n64_read32(n, va, &err);
        if (err) { exception(n, N64_EXC_ADEL); return; }
        break;
    }
    case 0x28: /* SB */ {
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t w = n64_read32(n, va & ~3u, NULL);
        uint32_t sh = 24u - 8u * (va & 3u);
        w = (w & ~(0xFFu << sh)) | ((U32(n->r[rt]) & 0xFFu) << sh);
        n64_write32(n, va & ~3u, w);
        break;
    }
    case 0x29: /* SH */ {
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t w = n64_read32(n, va & ~3u, NULL);
        uint32_t sh = 16u - 8u * (va & 2u);
        w = (w & ~(0xFFFFu << sh)) | ((U32(n->r[rt]) & 0xFFFFu) << sh);
        n64_write32(n, va & ~3u, w);
        break;
    }
    case 0x2B: /* SW */
        n64_write32(n, U32(n->r[rs]) + simm, U32(n->r[rt]));
        break;
    case 0x30: /* LL */ {
        int err = 0;
        uint32_t va = U32(n->r[rs]) + simm;
        n->r[rt] = (uint64_t)(int32_t)n64_read32(n, va, &err);
        if (err) { exception(n, N64_EXC_ADEL); return; }
        n->ll_bit = 1;
        break;
    }
    case 0x37: /* LD */ {
        int err = 0;
        uint32_t va = U32(n->r[rs]) + simm;
        if (va & 7u) { exception(n, N64_EXC_ADEL); return; }
        uint64_t hi = n64_read32(n, va, &err);
        uint64_t lo = err ? 0 : n64_read32(n, va + 4u, &err);
        if (err) { exception(n, N64_EXC_ADEL); return; }
        n->r[rt] = (hi << 32) | lo;
        break;
    }
    case 0x38: /* SC */ {
        uint32_t va = U32(n->r[rs]) + simm;
        if (n->ll_bit) n64_write32(n, va, U32(n->r[rt]));
        n->r[rt] = n->ll_bit;
        n->ll_bit = 0;
        break;
    }
    case 0x3D: /* SCD (64-bit store-conditional, documented) */ {
        uint32_t va = U32(n->r[rs]) + simm;
        if (n->ll_bit) {
            n64_write32(n, va, (uint32_t)(n->r[rt] >> 32));
            n64_write32(n, va + 4u, (uint32_t)n->r[rt]);
        }
        n->r[rt] = n->ll_bit;
        n->ll_bit = 0;
        break;
    }
    case 0x3F: /* SD */ {
        uint32_t va = U32(n->r[rs]) + simm;
        n64_write32(n, va, (uint32_t)(n->r[rt] >> 32));
        n64_write32(n, va + 4u, (uint32_t)n->r[rt]);
        break;
    }
    case 0x22: /* LWL (big-endian semantics) */ {
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t k = va & 3u;
        uint32_t w = n64_read32(n, va & ~3u, NULL);
        uint32_t keep = k ? ((1u << (8u * k)) - 1u) : 0u;
        n->r[rt] = (uint64_t)(int32_t)((w << (8u * k)) | (U32(n->r[rt]) & keep));
        break;
    }
    case 0x26: /* LWR (big-endian semantics) */ {
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t k = va & 3u;
        uint32_t w = n64_read32(n, va & ~3u, NULL);
        uint32_t loaded = w & (0xFFFFFFFFu >> (8u * k));
        uint32_t out = (U32(n->r[rt]) & ~(0xFFFFFFFFu >> (8u * k))) | loaded;
        n->r[rt] = (uint64_t)(int32_t)out;
        break;
    }
    case 0x2A: /* SWL (big-endian semantics) */ {
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t k = va & 3u;
        uint32_t w = n64_read32(n, va & ~3u, NULL);
        w = (w & (0xFFFFFFFFu << (32u - 8u * k))) | (U32(n->r[rt]) >> (8u * k));
        n64_write32(n, va & ~3u, w);
        break;
    }
    case 0x2E: /* SWR (big-endian semantics) */ {
        uint32_t va = U32(n->r[rs]) + simm;
        uint32_t k = va & 3u;
        uint32_t w = n64_read32(n, va & ~3u, NULL);
        w = (w & ~(0xFFFFFFFFu << (32u - 8u * (k + 1u)))) |
            (U32(n->r[rt]) << (8u * (3u - k)));
        n64_write32(n, va & ~3u, w);
        break;
    }
    case 0x33: /* PREF/NOP-like */ break;
    case 0x35: /* LDC1 */ case 0x39: /* SWC1 */ case 0x3B: /* SDC1 */
        exception(n, N64_EXC_CPU);
        return;
    case 0x31: /* LWC1 */ case 0x2D: /* SDC1 */
        exception(n, N64_EXC_CPU);
        return;
    default:
        exception(n, N64_EXC_RI);
        return;
    }
#undef BR
    (void)rd; (void)rt; (void)rs; (void)was_delay;
}

/* ---- frame --------------------------------------------------------------------- */

void n64_vi_render(struct n64 *n)
{
    uint32_t type = (n->vi[0] >> 1) & 3u;
    uint32_t origin = n->vi[1] & 0x00FFFFF0u;
    uint32_t width = n->vi[2] & 0x3FFu;
    if (width > N64_SCREEN_W)
        width = N64_SCREEN_W;
    for (uint32_t y = 0; y < N64_SCREEN_H; y++) {
        for (uint32_t x = 0; x < N64_SCREEN_W; x++) {
            uint32_t px = 0xFF000000u;
            if (type != 0u && width > 0u) {
                if (type == 1u) { /* 16 bpp RGBA5551 */
                    uint32_t off = origin + (y * width + x) * 2u;
                    if (off + 1u < 0x400000u) {
                        uint16_t c = (uint16_t)((n->rdram[off] << 8) |
                                                n->rdram[off + 1u]);
                        uint32_t r = ((c >> 10) & 0x1Fu) << 3;
                        uint32_t g = ((c >> 5) & 0x1Fu) << 3;
                        uint32_t b = (c & 0x1Fu) << 3;
                        r |= r >> 5;
                        g |= g >> 5;
                        b |= b >> 5;
                        px = EMU_PIXEL((uint8_t)r, (uint8_t)g, (uint8_t)b);
                    }
                } else if (type == 3u) { /* 32 bpp RGBA8 */
                    uint32_t off = origin + (y * width + x) * 4u;
                    if (off + 3u < 0x400000u) {
                        px = EMU_PIXEL(n->rdram[off], n->rdram[off + 1u],
                                       n->rdram[off + 2u]);
                    }
                }
            }
            n->fb[y * N64_SCREEN_W + x] = px;
        }
    }
}

static void n64_boot(struct n64 *n)
{
    /* documented boot model: the first 0x1000 ROM bytes (header + IPL3)
     * are copied to SP DMEM and the CPU starts at 0xA4000040, i.e. the
     * first IPL3 instruction (see n64.h) */
    for (uint32_t i = 0; i < 0x1000u && i < n->rom_size; i++)
        n->dmem[i] = n->rom[i];
    memset(n->r, 0, sizeof n->r);
    n->r[29] = 0xA4001FF0ull;
    n->hi = n->lo = 0;
    n->pc = 0xA4000040u;
    n->next_pc = n->pc + 4u;
    n->in_delay = 0;
    n->cp0[N64_CP0_STATUS] = N64_ST_BEV | N64_ST_ERL;
    n->cp0[N64_CP0_CONFIG] = 0x80000000u; /* little-endian bit cleared; BE */
    n->cycles = 0;
    n->ll_bit = 0;
}

static emu_result_t n64_load_rom(emu_core_t *core, const uint8_t *data,
                                 size_t size)
{
    struct n64 *n = (struct n64 *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    if (size < 0x1000u)
        return EMU_EBADROM;
    /* .z64 big-endian signature (registry probe agrees) */
    if (data[0] != 0x80u || data[1] != 0x37u || data[2] != 0x12u ||
        data[3] != 0x40u)
        return EMU_EBADROM;
    uint8_t *copy = malloc(size);
    if (copy == NULL)
        return EMU_EINVAL;
    memcpy(copy, data, size);
    free(n->rom);
    n->rom = copy;
    n->rom_size = size;
    n64_boot(n);
    return EMU_OK;
}

static void n64_reset(emu_core_t *core)
{
    struct n64 *n = (struct n64 *)core;
    memset(n->rdram, 0, sizeof n->rdram);
    memset(n->dmem, 0, sizeof n->dmem);
    memset(n->imem, 0, sizeof n->imem);
    memset(n->pif_rom, 0, sizeof n->pif_rom);
    memset(n->pif_ram, 0, sizeof n->pif_ram);
    memset(n->mi, 0, sizeof n->mi);
    memset(n->vi, 0, sizeof n->vi);
    memset(n->pi, 0, sizeof n->pi);
    memset(n->si, 0, sizeof n->si);
    memset(n->sp_reg, 0, sizeof n->sp_reg);
    n->vi_intr_pending = 0;
    if (n->rom != NULL)
        n64_boot(n);
}

static emu_result_t n64_run_frame(emu_core_t *core)
{
    struct n64 *n = (struct n64 *)core;
    if (n->rom == NULL)
        return EMU_ENOROM;

    uint64_t start = n->cycles;
    uint64_t target = n->cycles + (uint64_t)N64_CYCLES_PER_FRAME * 2u;
    while (n->cycles < target) {
        n64_step(n);
        /* VI: the half-line counter advances through the frame; when it
         * reaches VI_INTR the interrupt latches (cleared by a write to
         * VI_V_CURRENT). Documented model. */
        n->vi_cur = (uint32_t)(((n->cycles - start) >> 1) * 262u /
                               N64_CYCLES_PER_FRAME);
        if (!n->vi_intr_pending && (n->vi[3] & 0x3FFu) != 0u &&
            n->vi_cur >= (n->vi[3] & 0x3FFu)) {
            n->vi_intr_pending = 1;
            mi_raise(n, N64_MI_VI);
        }
    }
    n64_vi_render(n);
    n->frame_count++;
    return EMU_OK;
}

static const uint32_t *n64_fb(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct n64 *n = (struct n64 *)core;
    if (w != NULL)
        *w = N64_SCREEN_W;
    if (h != NULL)
        *h = N64_SCREEN_H;
    return n->fb;
}

static void n64_set_input(emu_core_t *core, uint32_t buttons)
{
    ((struct n64 *)core)->buttons = buttons; /* SI controller pending */
}

static void n64_set_audio(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    (void)core;
    (void)cb;
    (void)user; /* AI not implemented: core produces no audio */
}

/* ---- save states ----------------------------------------------------------------- */

static void n64_serialize(struct n64 *n, emu_state_writer *w)
{
    sw_u32(w, 0x4E363442u); /* "N64B" */
    for (int i = 0; i < 32; i++)
        sw_u64(w, n->r[i]);
    sw_u64(w, n->hi);
    sw_u64(w, n->lo);
    sw_u32(w, n->pc);
    sw_u32(w, n->next_pc);
    sw_u32(w, (uint32_t)n->in_delay);
    sw_u32(w, n->ll_bit);
    sw_u64(w, n->cycles);
    for (int i = 0; i < 32; i++)
        sw_u32(w, n->cp0[i]);
    for (int i = 0; i < 4; i++)
        sw_u32(w, n->mi[i]);
    for (int i = 0; i < 16; i++)
        sw_u32(w, n->vi[i]);
    for (int i = 0; i < 6; i++)
        sw_u32(w, n->pi[i]);
    for (int i = 0; i < 8; i++)
        sw_u32(w, n->si[i]);
    sw_mem(w, n->rdram, sizeof n->rdram);
    sw_mem(w, n->dmem, sizeof n->dmem);
    sw_mem(w, n->imem, sizeof n->imem);
    sw_mem(w, n->pif_ram, sizeof n->pif_ram);
}

static size_t n64_state_size(emu_core_t *core)
{
    struct n64 *n = (struct n64 *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    n64_serialize(n, &w);
    return w.pos;
}

static emu_result_t n64_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    struct n64 *n = (struct n64 *)core;
    emu_state_writer w = { buf, cap, 0, 0 };
    n64_serialize(n, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t n64_load_state(emu_core_t *core, const uint8_t *buf,
                                   size_t size)
{
    struct n64 *n = (struct n64 *)core;
    emu_state_reader rd = { buf, size, 0, 0 };
    if (sr_u32(&rd) != 0x4E363442u)
        return EMU_EBADSTATE;
    for (int i = 0; i < 32; i++)
        n->r[i] = sr_u64(&rd);
    n->hi = sr_u64(&rd);
    n->lo = sr_u64(&rd);
    n->pc = sr_u32(&rd);
    n->next_pc = sr_u32(&rd);
    n->in_delay = (int)sr_u32(&rd);
    n->ll_bit = (uint8_t)sr_u32(&rd);
    n->cycles = sr_u64(&rd);
    for (int i = 0; i < 32; i++)
        n->cp0[i] = sr_u32(&rd);
    for (int i = 0; i < 4; i++)
        n->mi[i] = sr_u32(&rd);
    for (int i = 0; i < 16; i++)
        n->vi[i] = sr_u32(&rd);
    for (int i = 0; i < 6; i++)
        n->pi[i] = sr_u32(&rd);
    for (int i = 0; i < 8; i++)
        n->si[i] = sr_u32(&rd);
    sr_mem(&rd, n->rdram, sizeof n->rdram);
    sr_mem(&rd, n->dmem, sizeof n->dmem);
    sr_mem(&rd, n->imem, sizeof n->imem);
    sr_mem(&rd, n->pif_ram, sizeof n->pif_ram);
    if (rd.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

/* ---- vtable ---------------------------------------------------------------------- */

static emu_result_t n64_create(emu_core_t **out);
static void n64_destroy(emu_core_t *core);

static const emu_core_vtable_t n64_vtable = {
    "m64-b", "Nintendo 64", N64_SCREEN_W, N64_SCREEN_H, N64_OUT_RATE,
    n64_create, n64_destroy, n64_load_rom, n64_reset,
    n64_run_frame, n64_fb, n64_set_input, n64_set_audio,
    n64_state_size, n64_save_state, n64_load_state,
};

static emu_result_t n64_create(emu_core_t **out)
{
    struct n64 *n = calloc(1, sizeof *n);
    if (n == NULL)
        return EMU_EINVAL;
    n->base.vtable = &n64_vtable;
    *out = &n->base;
    return EMU_OK;
}

static void n64_destroy(emu_core_t *core)
{
    struct n64 *n = (struct n64 *)core;
    if (n == NULL)
        return;
    free(n->rom);
    free(n);
}

const emu_core_vtable_t *emu_core_m64_b(void)
{
    return &n64_vtable;
}
