/*
 * AArch shared SH-4 interpreter. See sh4.h for scope, sources and the
 * documented simplifications. Group decode mirrors the shared SH-2 core
 * (the 16-bit ISA is the same family); SH-4 deltas: banked R0-R7 (RB),
 * extended system-register set, TRAPA vector = VBR + 0x100 + imm*4, the
 * FPUsingle-precision subset, and controller-supplied interrupt vectors.
 */
#include "sh4.h"

#include <math.h>
#include <string.h>

/* ---- register-bank helpers ------------------------------------------------------- */

static uint32_t sr_t(const sh4_t *c) { return c->sr & SH4_SR_T; }
static void sr_set_t(sh4_t *c, uint32_t v)
{
    c->sr = (c->sr & ~SH4_SR_T) | (v ? SH4_SR_T : 0u);
}

/* Writable SR bits (T,S,I,Q,M,FD,BL,RB,MD) */
#define SH4_SR_MASK 0x4000B3F3u

static void bank_sync_out(sh4_t *c)
{
    int bank = (c->sr & SH4_SR_RB) ? 1 : 0;
    for (int i = 0; i < 8; i++)
        c->rbank[bank][i] = c->r[i];
}

static void bank_sync_in(sh4_t *c)
{
    int bank = (c->sr & SH4_SR_RB) ? 1 : 0;
    for (int i = 0; i < 8; i++)
        c->r[i] = c->rbank[bank][i];
}

static void sr_write(sh4_t *c, uint32_t v)
{
    bank_sync_out(c);
    c->sr = v & SH4_SR_MASK;
    bank_sync_in(c);
}

/* Active FPU bank (FPSCR.FR selects FR vs XF). */
static uint32_t *freg(sh4_t *c, int n)
{
    return (c->fpscr & SH4_FPSCR_FR) ? &c->xf[n] : &c->fr[n];
}

static float fget(sh4_t *c, int n)
{
    float f;
    uint32_t bits = *freg(c, n);
    memcpy(&f, &bits, 4);
    return f;
}

static void fset(sh4_t *c, int n, float v)
{
    if ((c->fpscr & SH4_FPSCR_DN) && v != 0.0f && -1.17549435e-38f < v &&
        v < 1.17549435e-38f)
        v = 0.0f; /* FPSCR.DN: flush denormals to zero */
    uint32_t bits;
    memcpy(&bits, &v, 4);
    *freg(c, n) = bits;
}

static int16_t sext16(uint32_t v) { return (int16_t)(uint16_t)v; }
static int8_t sext8(uint32_t v) { return (int8_t)(uint8_t)v; }

/* ---- init / reset ----------------------------------------------------------------- */

void sh4_init(sh4_t *c, const sh4_bus_t *bus)
{
    memset(c, 0, sizeof *c);
    c->bus = bus;
}

void sh4_reset(sh4_t *c)
{
    for (int i = 0; i < 16; i++)
        c->r[i] = 0;
    memset(c->rbank, 0, sizeof c->rbank);
    c->pc = 0xA0000000u; /* H'AOOO'O000 boot area (documented reset PC) */
    c->next_pc = c->pc + 2u;
    c->pr = 0;
    c->sr = SH4_SR_BL | SH4_SR_MD | SH4_SR_I; /* interrupts masked */
    c->gbr = 0;
    c->vbr = 0;
    c->ssr = 0;
    c->spc = 0;
    c->sgr = 0;
    c->dbr = 0;
    c->mach = 0;
    c->macl = 0;
    c->fpul = 0;
    c->fpscr = 0;
    c->cycles = 0;
}

/* ---- branch / interrupt ----------------------------------------------------------- */

static void branch(sh4_t *c, uint32_t target)
{
    c->next_pc = target;
}

void sh4_irq(sh4_t *c, int level, uint32_t offset)
{
    uint32_t lv = (uint32_t)level & 0xFu;
    if (c->sr & SH4_SR_BL)
        return; /* blocked while BL=1 (documented coarse model) */
    if ((c->sr & SH4_SR_I) >> 4 >= lv)
        return;
    c->r[15] -= 4u;
    c->bus->write32(c->bus->user, c->r[15], c->sr);
    c->r[15] -= 4u;
    c->bus->write32(c->bus->user, c->r[15], c->next_pc);
    uint32_t vec = c->bus->read32(c->bus->user, c->vbr + offset);
    c->sr = (c->sr & ~SH4_SR_I) | (lv << 4) | SH4_SR_BL;
    c->pc = vec;
    c->next_pc = vec + 2u;
}

/* ---- main interpreter ------------------------------------------------------------- */

uint32_t sh4_step(sh4_t *c)
{
    const sh4_bus_t *b = c->bus;

    uint32_t op = b->read16(b->user, c->pc);
    uint32_t pc = c->pc;
    c->pc = c->next_pc;
    c->next_pc = c->pc + 2u;

    uint32_t n = (op >> 8) & 0xFu;
    uint32_t m = (op >> 4) & 0xFu;
    uint32_t cycles = 1u;

    switch (op >> 12) {
    case 0x0:
        if (op == 0x0009u) { /* NOP */ }
        else if (op == 0x0008u) sr_set_t(c, 0); /* CLRT */
        else if (op == 0x0018u) sr_set_t(c, 1); /* SETT */
        else if (op == 0x0019u) { /* DIV0U */
            c->sr &= ~(SH4_SR_M | SH4_SR_Q | SH4_SR_T);
        }
        else if (op == 0x0028u) { c->mach = c->macl = 0; } /* CLRMAC */
        else if (op == 0x0029u) c->r[n] = sr_t(c); /* MOVT Rn */
        else if (op == 0x0038u) { /* LDTLB: no MMU - documented no-op */ }
        else if (op == 0x0048u) c->sr &= ~SH4_SR_S; /* CLRS */
        else if (op == 0x0058u) c->sr |= SH4_SR_S;  /* SETS */
        else if (op == 0x000Bu) { branch(c, c->pr); cycles = 2; } /* RTS */
        else if (op == 0x001Bu) cycles = 3; /* SLEEP (coarse) */
        else if (op == 0x002Bu) { /* RTE */
            uint32_t pc2 = b->read32(b->user, c->r[15]);
            c->r[15] += 4u;
            sr_write(c, b->read32(b->user, c->r[15]));
            c->r[15] += 4u;
            branch(c, pc2);
            cycles = 4;
        }
        else if ((op & 0x00FFu) == 0x0003u) { /* BSRF Rn */
            c->pr = pc + 4u;
            branch(c, pc + 4u + c->r[n]);
            cycles = 2;
        }
        else if ((op & 0x00FFu) == 0x0023u) { /* BRAF Rn */
            branch(c, pc + 4u + c->r[n]);
            cycles = 2;
        }
        else if ((op & 0x00FFu) == 0x0002u) sr_write(c, c->r[n]); /* STC SR */
        else if ((op & 0x00FFu) == 0x0012u) c->r[n] = c->gbr; /* STC GBR */
        else if ((op & 0x00FFu) == 0x0022u) c->r[n] = c->vbr; /* STC VBR */
        else if ((op & 0x00FFu) == 0x0032u) c->r[n] = c->ssr; /* STC SSR */
        else if ((op & 0x00FFu) == 0x0042u) c->r[n] = c->spc; /* STC SPC */
        else if ((op & 0x00FFu) == 0x003Au) c->r[n] = c->sgr; /* STC SGR */
        else if ((op & 0x00FFu) == 0x00FAu) c->r[n] = c->dbr; /* STC DBR */
        else if ((op & 0x00FFu) == 0x005Au) c->r[n] = c->fpul; /* STS FPUL */
        else if ((op & 0x00FFu) == 0x006Au) c->r[n] = c->fpscr; /* STS FPSCR */
        else if ((op & 0x00FFu) == 0x000Au) c->r[n] = c->mach; /* STS MACH */
        else if ((op & 0x00FFu) == 0x001Au) c->r[n] = c->macl; /* STS MACL */
        else if ((op & 0x00FFu) == 0x002Au) c->r[n] = c->pr; /* STS PR */
        else if ((op & 0x000Fu) == 0x0007u) { /* MUL.L Rm,Rn */
            c->macl = c->r[n] * c->r[m];
            cycles = 2;
        }
        else if ((op & 0x000Fu) == 0x0004u) { /* MOV.B Rm,@(R0,Rn) */
            b->write8(b->user, c->r[0] + c->r[n], (uint8_t)c->r[m]);
        }
        else if ((op & 0x000Fu) == 0x0005u) { /* MOV.W Rm,@(R0,Rn) */
            b->write16(b->user, c->r[0] + c->r[n], (uint16_t)c->r[m]);
        }
        else if ((op & 0x000Fu) == 0x0006u) { /* MOV.L Rm,@(R0,Rn) */
            b->write32(b->user, c->r[0] + c->r[n], c->r[m]);
            cycles = 2;
        }
        else if ((op & 0x000Fu) == 0x000Cu) { /* MOV.B @(R0,Rm),Rn */
            c->r[n] = (uint32_t)(int32_t)sext8(b->read8(b->user, c->r[0] + c->r[m]));
        }
        else if ((op & 0x000Fu) == 0x000Du) { /* MOV.W @(R0,Rm),Rn */
            c->r[n] = (uint32_t)(int32_t)sext16(b->read16(b->user, c->r[0] + c->r[m]));
        }
        else if ((op & 0x000Fu) == 0x000Eu) { /* MOV.L @(R0,Rm),Rn */
            c->r[n] = b->read32(b->user, c->r[0] + c->r[m]);
            cycles = 2;
        }
        else if ((op & 0x000Fu) == 0x000Fu) { /* MAC.L @Rm+,@Rn+ */
            uint32_t a = b->read32(b->user, c->r[m]);
            c->r[m] += 4u;
            uint32_t d = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            int64_t prod = (int64_t)(int32_t)d * (int64_t)(int32_t)a;
            int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl) + prod;
            c->mach = (uint32_t)((uint64_t)acc >> 32);
            c->macl = (uint32_t)(uint64_t)acc;
            cycles = 3;
        }
        else goto illegal;
        break;

    case 0x1: /* MOV.L Rm,@(disp,Rn) */
        b->write32(b->user, c->r[n] + ((op & 0xFu) << 2), c->r[m]);
        cycles = 2;
        break;

    case 0x2:
        switch (op & 0xFu) {
        case 0x0: b->write8(b->user, c->r[n], (uint8_t)c->r[m]); break;
        case 0x1: b->write16(b->user, c->r[n], (uint16_t)c->r[m]); break;
        case 0x2: b->write32(b->user, c->r[n], c->r[m]); cycles = 2; break;
        case 0x4: {
            uint32_t a = c->r[n] - 1u;
            b->write8(b->user, a, (uint8_t)c->r[m]);
            c->r[n] = a;
            cycles = 2;
            break;
        }
        case 0x5: {
            uint32_t a = c->r[n] - 2u;
            b->write16(b->user, a, (uint16_t)c->r[m]);
            c->r[n] = a;
            cycles = 2;
            break;
        }
        case 0x6: {
            uint32_t a = c->r[n] - 4u;
            b->write32(b->user, a, c->r[m]);
            c->r[n] = a;
            cycles = 2;
            break;
        }
        case 0x7: { /* DIV0S Rm,Rn */
            uint32_t q = (c->r[n] >> 31) & 1u;
            uint32_t mb = (c->r[m] >> 31) & 1u;
            c->sr = (c->sr & ~(SH4_SR_Q | SH4_SR_M | SH4_SR_T)) |
                    (q ? SH4_SR_Q : 0u) | (mb ? SH4_SR_M : 0u) |
                    ((q ^ mb) ? SH4_SR_T : 0u);
            break;
        }
        case 0x8: sr_set_t(c, (c->r[n] & c->r[m]) == 0); break; /* TST */
        case 0x9: c->r[n] &= c->r[m]; break;                    /* AND */
        case 0xA: c->r[n] ^= c->r[m]; break;                    /* XOR */
        case 0xB: c->r[n] |= c->r[m]; break;                    /* OR  */
        case 0xC: { /* CMP/STR */
            uint32_t x = c->r[n] ^ c->r[m];
            uint32_t hit = ((x & 0xFFu) == 0) |
                           (((x >> 8) & 0xFFu) == 0 ? 2u : 0u) |
                           (((x >> 16) & 0xFFu) == 0 ? 4u : 0u) |
                           ((x >> 24) == 0 ? 8u : 0u);
            sr_set_t(c, hit != 0);
            break;
        }
        case 0xD: /* XTRCT */
            c->r[n] = ((c->r[n] << 16) & 0xFFFF0000u) | (c->r[m] >> 16);
            break;
        case 0xE: c->macl = c->r[n] * c->r[m]; cycles = 2; break; /* MULU.W */
        case 0xF:
            c->macl = (uint32_t)((int64_t)(int16_t)c->r[n] *
                                 (int64_t)(int16_t)c->r[m]);
            cycles = 2;
            break; /* MULS.W */
        default: goto illegal;
        }
        break;

    case 0x3:
        switch (op & 0xFu) {
        case 0x0: sr_set_t(c, c->r[n] == c->r[m]); break;           /* CMP/EQ */
        case 0x2: sr_set_t(c, c->r[n] >= c->r[m]); break;           /* CMP/HS */
        case 0x3: sr_set_t(c, (int32_t)c->r[n] >= (int32_t)c->r[m]); break;
        case 0x4: { /* DIV1 Rm,Rn (manual pseudocode) */
            uint32_t old_q = (c->sr & SH4_SR_Q) ? 1u : 0u;
            uint32_t mb = (c->sr & SH4_SR_M) ? 1u : 0u;
            uint32_t rm = c->r[m], rn = c->r[n];
            uint32_t q = (rn >> 31) & 1u;
            rn <<= 1;
            rn |= sr_t(c);
            uint32_t tmp0, tmp1;
            if (old_q == 0u) {
                if (mb == 0u) {
                    tmp0 = rn;
                    rn -= rm;
                    tmp1 = (rn > tmp0) ? 1u : 0u;
                    q = (q == 0u) ? tmp1 : (tmp1 == 0u ? 1u : 0u);
                } else {
                    tmp0 = rn;
                    rn += rm;
                    tmp1 = (rn < tmp0) ? 1u : 0u;
                    q = (q == 0u) ? (tmp1 == 0u ? 1u : 0u) : tmp1;
                }
            } else {
                if (mb == 0u) {
                    tmp0 = rn;
                    rn += rm;
                    tmp1 = (rn < tmp0) ? 1u : 0u;
                    q = (q == 0u) ? (tmp1 == 0u ? 1u : 0u) : tmp1;
                } else {
                    tmp0 = rn;
                    rn -= rm;
                    tmp1 = (rn > tmp0) ? 1u : 0u;
                    q = (q == 0u) ? tmp1 : (tmp1 == 0u ? 1u : 0u);
                }
            }
            c->sr = (c->sr & ~(SH4_SR_Q | SH4_SR_T)) |
                    (q ? SH4_SR_Q : 0u) | ((q == mb) ? SH4_SR_T : 0u);
            c->r[n] = rn;
            break;
        }
        case 0x5: { /* DMULU.L */
            uint64_t p = (uint64_t)c->r[n] * (uint64_t)c->r[m];
            c->mach = (uint32_t)(p >> 32);
            c->macl = (uint32_t)p;
            cycles = 2;
            break;
        }
        case 0x6: sr_set_t(c, c->r[n] > c->r[m]); break;            /* CMP/HI */
        case 0x7: sr_set_t(c, (int32_t)c->r[n] > (int32_t)c->r[m]); break;
        case 0x8: c->r[n] -= c->r[m]; break;                        /* SUB */
        case 0xA: { /* SUBC */
            uint32_t d0 = c->r[n] - c->r[m];
            uint32_t d1 = d0 - sr_t(c);
            sr_set_t(c, (c->r[n] < c->r[m]) || (d0 < (uint32_t)sr_t(c)));
            c->r[n] = d1;
            break;
        }
        case 0xB: { /* SUBV */
            uint32_t rr = c->r[n] - c->r[m];
            sr_set_t(c, ((c->r[n] ^ c->r[m]) & (c->r[n] ^ rr)) >> 31);
            c->r[n] = rr;
            break;
        }
        case 0xC: c->r[n] += c->r[m]; break;                        /* ADD */
        case 0xD: { /* DMULS.L */
            int64_t p = (int64_t)(int32_t)c->r[n] * (int64_t)(int32_t)c->r[m];
            c->mach = (uint32_t)((uint64_t)p >> 32);
            c->macl = (uint32_t)(uint64_t)p;
            cycles = 2;
            break;
        }
        case 0xE: { /* ADDC */
            uint32_t s0 = c->r[n] + c->r[m];
            uint32_t s1 = s0 + sr_t(c);
            sr_set_t(c, (s0 < c->r[n]) || (s1 < s0));
            c->r[n] = s1;
            break;
        }
        case 0xF: { /* ADDV */
            uint32_t rr = c->r[n] + c->r[m];
            sr_set_t(c, (~(c->r[n] ^ c->r[m]) & (c->r[n] ^ rr)) >> 31);
            c->r[n] = rr;
            break;
        }
        default: goto illegal;
        }
        break;

    case 0x4:
        /* Group 4 dispatches on the LOW BYTE; the register operand sits in
         * bits 11-8. MAC.W/MAC.L put Rm in bits 7-4 and match on the low
         * nibble first. */
        if ((op & 0xFu) == 0xFu) { /* MAC.W @Rm+,@Rn+ */
            uint16_t a = b->read16(b->user, c->r[m]);
            c->r[m] += 2u;
            uint16_t d = b->read16(b->user, c->r[n]);
            c->r[n] += 2u;
            int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl) +
                          (int64_t)(int16_t)d * (int64_t)(int16_t)a;
            c->mach = (uint32_t)((uint64_t)acc >> 32);
            c->macl = (uint32_t)(uint64_t)acc;
            cycles = 3;
            break;
        }
        if ((op & 0xFu) == 0x0Cu) { /* MAC.L @Rm+,@Rn+ */
            uint32_t a = b->read32(b->user, c->r[m]);
            c->r[m] += 4u;
            uint32_t d = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl) +
                          (int64_t)(int32_t)d * (int64_t)(int32_t)a;
            c->mach = (uint32_t)((uint64_t)acc >> 32);
            c->macl = (uint32_t)(uint64_t)acc;
            cycles = 3;
            break;
        }
        switch (op & 0xFFu) {
        case 0x00u: { /* SHLL */
            sr_set_t(c, (c->r[n] >> 31) & 1u);
            c->r[n] <<= 1;
            break;
        }
        case 0x01u: { /* SHLR */
            sr_set_t(c, c->r[n] & 1u);
            c->r[n] >>= 1;
            break;
        }
        case 0x02u: /* STS.L MACH,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->mach);
            break;
        case 0x03u: /* STS.L MACL,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->macl);
            break;
        case 0x04u: { /* ROTL */
            uint32_t msb = (c->r[n] >> 31) & 1u;
            c->r[n] = (c->r[n] << 1) | msb;
            sr_set_t(c, msb);
            break;
        }
        case 0x05u: { /* ROTR */
            uint32_t lsb = c->r[n] & 1u;
            c->r[n] = (c->r[n] >> 1) | (lsb << 31);
            sr_set_t(c, lsb);
            break;
        }
        case 0x06u: /* LDS.L @Rm+,MACH */
            c->mach = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            break;
        case 0x07u: /* LDS.L @Rm+,MACL */
            c->macl = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            break;
        case 0x08u: c->r[n] <<= 2; break;  /* SHLL2  */
        case 0x09u: c->r[n] >>= 2; break;  /* SHLR2  */
        case 0x0Au: c->mach = c->r[n]; break; /* LDS Rm,MACH */
        case 0x0Bu: /* JSR @Rn */
            c->pr = pc + 4u;
            branch(c, c->r[n]);
            cycles = 2;
            break;
        case 0x0Eu: /* LDS Rn,SR (privileged; MD gate documented coarse) */
            sr_write(c, c->r[n]);
            cycles = 2;
            break;
        case 0x10u: { /* DT Rn */
            c->r[n] -= 1u;
            sr_set_t(c, c->r[n] == 0);
            break;
        }
        case 0x11u: /* CMP/PZ */
            sr_set_t(c, (int32_t)c->r[n] >= 0);
            break;
        case 0x12u: /* STS.L PR,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->pr);
            break;
        case 0x13u: /* STC.L SR,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->sr);
            cycles = 2;
            break;
        case 0x15u: /* CMP/PL */
            sr_set_t(c, (int32_t)c->r[n] > 0);
            break;
        case 0x16u: /* LDS.L @Rm+,PR */
            c->pr = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            break;
        case 0x17u: /* LDS.L @Rm+,SR */
            sr_write(c, b->read32(b->user, c->r[n]));
            c->r[n] += 4u;
            cycles = 3;
            break;
        case 0x18u: c->r[n] <<= 8; break;  /* SHLL8  */
        case 0x19u: c->r[n] >>= 8; break;  /* SHLR8  */
        case 0x1Au: c->pr = c->r[n]; break; /* LDS Rn,PR */
        case 0x1Eu: /* LDS Rn,GBR */
            c->gbr = c->r[n];
            break;
        case 0x20u: { /* SHAL */
            sr_set_t(c, (c->r[n] >> 31) & 1u);
            c->r[n] <<= 1;
            break;
        }
        case 0x21u: { /* SHAR */
            sr_set_t(c, c->r[n] & 1u);
            c->r[n] = (uint32_t)((int32_t)c->r[n] >> 1);
            break;
        }
        case 0x22u: /* STC.L GBR,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->gbr);
            cycles = 2;
            break;
        case 0x23u: /* STC.L VBR,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->vbr);
            cycles = 2;
            break;
        case 0x24u: { /* ROTCL */
            uint32_t msb = (c->r[n] >> 31) & 1u;
            c->r[n] = (c->r[n] << 1) | sr_t(c);
            sr_set_t(c, msb);
            break;
        }
        case 0x25u: { /* ROTCR */
            uint32_t lsb = c->r[n] & 1u;
            c->r[n] = (c->r[n] >> 1) | (sr_t(c) << 31);
            sr_set_t(c, lsb);
            break;
        }
        case 0x26u: /* LDS.L @Rm+,FPUL */
            c->fpul = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            break;
        case 0x27u: /* LDS.L @Rm+,FPSCR */
            c->fpscr = b->read32(b->user, c->r[n]) & 0x00FFFFFFu;
            c->r[n] += 4u;
            cycles = 2;
            break;
        case 0x28u: c->r[n] <<= 16; break; /* SHLL16 */
        case 0x29u: c->r[n] >>= 16; break; /* SHLR16 */
        case 0x2Au: c->fpul = c->r[n]; break; /* LDS Rn,FPUL */
        case 0x2Bu: /* JMP @Rn */
            branch(c, c->r[n]);
            cycles = 2;
            break;
        case 0x2Eu: /* LDS Rn,VBR */
            c->vbr = c->r[n];
            break;
        case 0x33u: /* STC.L SGR,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->sgr);
            cycles = 2;
            break;
        case 0x36u: /* LDS.L @Rm+,SGR */
            c->sgr = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            cycles = 2;
            break;
        case 0x37u: /* LDS.L @Rm+,DBR */
            c->dbr = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            cycles = 2;
            break;
        case 0x3Au: /* STC.L DBR,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->dbr);
            cycles = 2;
            break;
        case 0x3Eu: /* LDS Rn,SSR */
            c->ssr = c->r[n];
            break;
        case 0x4Eu: /* LDS Rn,SPC */
            c->spc = c->r[n];
            break;
        case 0x5Au: /* LDS Rn,FPUL (direct, code 5) */
            c->fpul = c->r[n];
            break;
        case 0x6Au: /* LDS Rn,FPSCR */
            c->fpscr = c->r[n] & 0x00FFFFFFu;
            cycles = 2;
            break;
        case 0xF6u: /* reserved slot in this decode table */
        default:
            goto illegal;
        }
        break;

    case 0x5: /* MOV.L @(disp,Rm),Rn */
        c->r[n] = b->read32(b->user, c->r[m] + ((op & 0xFu) << 2));
        cycles = 2;
        break;

    case 0x6:
        switch (op & 0xFu) {
        case 0x0: c->r[n] = (uint32_t)(int32_t)sext8(b->read8(b->user, c->r[m])); cycles = 2; break;
        case 0x1: c->r[n] = (uint32_t)(int32_t)sext16(b->read16(b->user, c->r[m])); cycles = 2; break;
        case 0x2: c->r[n] = b->read32(b->user, c->r[m]); cycles = 2; break;
        case 0x3: c->r[n] = c->r[m]; break;
        case 0x4: c->r[n] = (uint32_t)(int32_t)sext8(b->read8(b->user, c->r[m])); c->r[m] += 1u;
            cycles = 2; break;
        case 0x5: c->r[n] = (uint32_t)(int32_t)sext16(b->read16(b->user, c->r[m])); c->r[m] += 2u;
            cycles = 2; break;
        case 0x6: c->r[n] = b->read32(b->user, c->r[m]); c->r[m] += 4u;
            cycles = 2; break;
        case 0x7: c->r[n] = ~c->r[m]; break;                     /* NOT */
        case 0x8: /* SWAP.B: exchange the two low bytes */
            c->r[n] = (c->r[m] & 0xFFFF0000u) | ((c->r[m] & 0xFFu) << 8) |
                      ((c->r[m] >> 8) & 0xFFu);
            break;
        case 0x9: c->r[n] = (c->r[m] << 16) | (c->r[m] >> 16); break; /* SWAP.W */
        case 0xA: { /* NEGC */
            uint32_t t = 0u - c->r[m] - sr_t(c);
            sr_set_t(c, (c->r[m] != 0u) || sr_t(c));
            c->r[n] = t;
            break;
        }
        case 0xB: c->r[n] = 0u - c->r[m]; break;                 /* NEG */
        case 0xC: c->r[n] = c->r[m] & 0xFFu; break;              /* EXTU.B */
        case 0xD: c->r[n] = c->r[m] & 0xFFFFu; break;            /* EXTU.W */
        case 0xE: c->r[n] = (uint32_t)(int32_t)(int8_t)c->r[m]; break; /* EXTS.B */
        case 0xF: c->r[n] = (uint32_t)(int32_t)(int16_t)c->r[m]; break; /* EXTS.W */
        default: goto illegal;
        }
        break;

    case 0x7: /* ADD #imm,Rn */
        c->r[n] += (uint32_t)sext8(op & 0xFFu);
        break;

    case 0x8:
        switch ((op >> 8) & 0xFu) {
        case 0x0: /* MOV.B R0,@(disp,Rm) */
            b->write8(b->user, c->r[m] + (op & 0xFu), (uint8_t)c->r[0]);
            break;
        case 0x1: /* MOV.W R0,@(disp,Rm) */
            b->write16(b->user, c->r[m] + ((op & 0xFu) << 1),
                       (uint16_t)c->r[0]);
            break;
        case 0x2: /* MOV.L R0,@(disp,Rm) */
            b->write32(b->user, c->r[m] + ((op & 0xFu) << 2), c->r[0]);
            break;
        case 0x4: /* MOV.B @(disp,Rm),R0 */
            c->r[0] = (uint32_t)(int32_t)sext8(b->read8(b->user, c->r[m] + (op & 0xFu)));
            break;
        case 0x5: /* MOV.W @(disp,Rm),R0 */
            c->r[0] = (uint32_t)(int32_t)sext16(b->read16(b->user, c->r[m] + ((op & 0xFu) << 1)));
            break;
        case 0x6: /* MOV.L @(disp,Rm),R0 */
            c->r[0] = b->read32(b->user, c->r[m] + ((op & 0xFu) << 2));
            break;
        case 0x8: /* CMP/EQ #imm,R0 */
            sr_set_t(c, (uint8_t)c->r[0] == (uint8_t)(op & 0xFFu));
            break;
        case 0x9: { /* BT label */
            int32_t d = (int32_t)(int8_t)(op & 0xFFu);
            if (sr_t(c)) {
                branch(c, pc + 4u + ((uint32_t)d << 1));
                cycles = 3;
            }
            break;
        }
        case 0xB: { /* BF label */
            int32_t d = (int32_t)(int8_t)(op & 0xFFu);
            if (!sr_t(c)) {
                branch(c, pc + 4u + ((uint32_t)d << 1));
                cycles = 3;
            }
            break;
        }
        case 0xD: { /* BT.S label (delayed) */
            int32_t d = (int32_t)(int8_t)(op & 0xFFu);
            if (sr_t(c)) {
                branch(c, pc + 4u + ((uint32_t)d << 1));
                cycles = 2;
            }
            break;
        }
        case 0xF: { /* BF.S label (delayed) */
            int32_t d = (int32_t)(int8_t)(op & 0xFFu);
            if (!sr_t(c)) {
                branch(c, pc + 4u + ((uint32_t)d << 1));
                cycles = 2;
            }
            break;
        }
        default: goto illegal;
        }
        break;

    case 0x9: /* MOV.W @(disp,PC),Rn */
        c->r[n] = (uint32_t)(int32_t)sext16(b->read16(b->user, pc + 4u + ((op & 0xFFu) << 1)));
        cycles = 2;
        break;

    case 0xA:   /* BRA  */
    case 0xB: { /* BSR  */
        uint32_t disp = op & 0xFFFu;
        int32_t sext = (disp & 0x800u) ? (int32_t)(disp | 0xFFFFF000u)
                                       : (int32_t)disp;
        if (op >> 12 == 0xBu)
            c->pr = pc + 4u;
        branch(c, pc + 4u + (uint32_t)(sext * 2));
        cycles = 2;
        break;
    }

    case 0xC:
        switch ((op >> 8) & 0xFu) {
        case 0x0: b->write8(b->user, c->gbr + (op & 0xFFu), (uint8_t)c->r[0]);
            break;
        case 0x1: b->write16(b->user, c->gbr + ((op & 0xFFu) << 1),
                             (uint16_t)c->r[0]);
            break;
        case 0x2: b->write32(b->user, c->gbr + ((op & 0xFFu) << 2), c->r[0]);
            break;
        case 0x3: { /* TRAPA #imm: vector = VBR + 0x100 + imm*4 (SH-4) */
            uint32_t vec = 0x100u + (op & 0xFFu) * 4u;
            c->r[15] -= 4u;
            b->write32(b->user, c->r[15], c->sr);
            c->r[15] -= 4u;
            b->write32(b->user, c->r[15], c->next_pc);
            sr_write(c, c->sr | SH4_SR_BL);
            c->pc = c->vbr + vec;
            c->next_pc = c->pc + 2u;
            cycles = 8;
            break;
        }
        case 0x4: c->r[0] = (uint32_t)(int32_t)sext8(b->read8(b->user, c->gbr + (op & 0xFFu)));
            break;
        case 0x5: c->r[0] = (uint32_t)(int32_t)sext16(b->read16(b->user, c->gbr +
                                             ((op & 0xFFu) << 1)));
            break;
        case 0x6: c->r[0] = b->read32(b->user, c->gbr + ((op & 0xFFu) << 2));
            break;
        case 0x7: /* MOVA @(disp,PC),R0 */
            c->r[0] = ((pc + 4u) & ~3u) + ((op & 0xFFu) << 2);
            break;
        case 0x8: sr_set_t(c, (c->r[0] & (op & 0xFFu)) == 0); break;
        case 0x9: c->r[0] &= op & 0xFFu; break;
        case 0xA: c->r[0] ^= op & 0xFFu; break;
        case 0xB: c->r[0] |= op & 0xFFu; break;
        default: goto illegal;
        }
        break;

    case 0xD: /* MOV.L @(disp,PC),Rn */
        c->r[n] = b->read32(b->user, ((pc + 4u) & ~3u) + ((op & 0xFFu) << 2));
        cycles = 2;
        break;

    case 0xE: /* MOV #imm,Rn */
        c->r[n] = (uint32_t)sext8(op & 0xFFu);
        break;

    case 0xF: /* FPU subset */
        switch (op & 0xFu) {
        case 0x0u: fset(c, (int)n, fget(c, (int)n) + fget(c, (int)m)); cycles = 4; break; /* FADD */
        case 0x1u: fset(c, (int)n, fget(c, (int)n) - fget(c, (int)m)); cycles = 4; break; /* FSUB */
        case 0x2u: fset(c, (int)n, fget(c, (int)n) * fget(c, (int)m)); cycles = 4; break; /* FMUL */
        case 0x3u: { /* FDIV */
            float d2 = fget(c, (int)m);
            fset(c, (int)n, d2 == 0.0f ? 0.0f : fget(c, (int)n) / d2);
            cycles = 12;
            break;
        }
        case 0x4u: sr_set_t(c, fget(c, (int)n) == fget(c, (int)m)); cycles = 4; break; /* FCMP/EQ */
        case 0x5u: sr_set_t(c, fget(c, (int)n) > fget(c, (int)m)); cycles = 4; break; /* FCMP/GT */
        case 0x6u: /* FMOV.S @(R0,Rm),FRn */
            *freg(c, (int)n) = b->read32(b->user, c->r[0] + c->r[m]);
            cycles = 2;
            break;
        case 0x7u: /* FMOV.S FRm,@(R0,Rn) */
            b->write32(b->user, c->r[0] + c->r[n], *freg(c, (int)m));
            cycles = 2;
            break;
        case 0x8u: /* FMOV.S @Rm,FRn */
            *freg(c, (int)n) = b->read32(b->user, c->r[m]);
            cycles = 2;
            break;
        case 0x9u: /* FMOV.S @Rm+,FRn */
            *freg(c, (int)n) = b->read32(b->user, c->r[m]);
            c->r[m] += 4u;
            cycles = 2;
            break;
        case 0xAu: /* FMOV.S FRm,@Rn */
            b->write32(b->user, c->r[n], *freg(c, (int)m));
            cycles = 2;
            break;
        case 0xBu: /* FMOV.S FRm,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], *freg(c, (int)m));
            cycles = 2;
            break;
        case 0xCu: /* FMOV.S FRm,FRn */
            *freg(c, (int)n) = *freg(c, (int)m);
            break;
        case 0xDu: { /* single-operand family (1111nnnn0ooo1101) */
            uint32_t sub = m;
            float v;
            switch (sub) {
            case 0x1u: /* FSTS FPUL,FRn */
                *freg(c, (int)n) = c->fpul;
                break;
            case 0x2u: { /* FLOAT FPUL,FRn */
                float fv = (float)(int32_t)c->fpul;
                fset(c, (int)n, fv);
                cycles = 2;
                break;
            }
            case 0x3u: { /* FTRC FRm,FPUL (source register at [11:8]) */
                float fv = fget(c, (int)n);
                if (fv >= 2147483648.0f)
                    c->fpul = 0x7FFFFFFFu;
                else if (fv <= -2147483648.0f)
                    c->fpul = 0x80000000u;
                else
                    c->fpul = (uint32_t)(int32_t)fv;
                cycles = 2;
                break;
            }
            case 0x4u: /* FNEG */
                v = fget(c, (int)n);
                fset(c, (int)n, -v);
                break;
            case 0x5u: /* FABS */
                v = fget(c, (int)n);
                fset(c, (int)n, v < 0.0f ? -v : v);
                break;
            case 0x6u: { /* FSQRT */
                v = fget(c, (int)n);
                fset(c, (int)n, v < 0.0f ? 0.0f : sqrtf(v));
                cycles = 10;
                break;
            }
            case 0x8u: *freg(c, (int)n) = 0; break; /* FLDI0 */
            case 0x9u: *freg(c, (int)n) = 0x3F800000u; break; /* FLDI1 */
            case 0xEu: { /* FIPR FVm,FVn */
                int vn = (int)((op >> 10) & 3u);
                int vm = (int)((op >> 8) & 1u);
                float dot = fget(c, 4 * vm + 0) * fget(c, 4 * vn + 0) +
                            fget(c, 4 * vm + 1) * fget(c, 4 * vn + 1) +
                            fget(c, 4 * vm + 2) * fget(c, 4 * vn + 2) +
                            fget(c, 4 * vm + 3) * fget(c, 4 * vn + 3);
                fset(c, 4 * vn + 3, dot);
                cycles = 4;
                break;
            }
            case 0xFu: { /* FTRV XMTRX,FVn */
                int vn = (int)((op >> 9) & 7u);
                if (vn > 3)
                    vn &= 3;
                float vec0 = fget(c, 4 * vn + 0);
                float vec1 = fget(c, 4 * vn + 1);
                float vec2 = fget(c, 4 * vn + 2);
                float res[4];
                uint32_t *x = c->xf; /* XMTRX lives in the XF bank */
                for (int i = 0; i < 4; i++) {
                    float row[4];
                    for (int j = 0; j < 4; j++) {
                        uint32_t bits = x[4 * i + j];
                        memcpy(&row[j], &bits, 4);
                    }
                    res[i] = row[0] * vec0 + row[1] * vec1 + row[2] * vec2 +
                             row[3];
                }
                for (int i = 0; i < 3; i++)
                    fset(c, 4 * vn + i, res[i]);
                cycles = 6;
                break;
            }
            default:
                goto illegal; /* documented: unsupported FPU op */
            }
            break;
        }
        case 0xEu: { /* FMAC FR0,FRm,FRn */
            fset(c, (int)n, fget(c, (int)n) + fget(c, 0) * fget(c, (int)m));
            cycles = 4;
            break;
        }
        default:
            goto illegal;
        }
        break;

    default: goto illegal;
    }

    c->cycles += cycles;
    return cycles;

illegal:
    /* Illegal opcode: general exception through VBR+0x100 (immediate). */
    c->r[15] -= 4u;
    b->write32(b->user, c->r[15], c->sr);
    c->r[15] -= 4u;
    b->write32(b->user, c->r[15], c->next_pc);
    sr_write(c, c->sr | SH4_SR_BL);
    c->pc = c->vbr + 0x100u;
    c->next_pc = c->pc + 2u;
    c->cycles += 8u;
    return 8u;
}

/* ---- test/inspection hooks --------------------------------------------------------- */

uint32_t sh4_fr_bits(const sh4_t *c, int n)
{
    return *freg((sh4_t *)c, n);
}

void sh4_set_fr_bits(sh4_t *c, int n, uint32_t bits)
{
    *freg(c, n) = bits;
}
