/*
 * AArch shared SH-2 interpreter. Encoding source of truth: Hitachi
 * SH-1/SH-2 Programming Manual (Sept 1996), tables 5.2-5.9 and the
 * instruction description section (DIV1 pseudocode verbatim).
 */
#include "sh2.h"

#include <string.h>

/* ---- helpers ------------------------------------------------------------------ */

static inline uint32_t sr_t(const sh2_t *c) { return c->sr & SH2_T; }
static inline void sr_set_t(sh2_t *c, uint32_t v)
{
    c->sr = (c->sr & ~SH2_T) | (v ? SH2_T : 0);
}
static inline uint32_t sr_q(const sh2_t *c) { return (c->sr >> 8) & 1u; }
static inline void sr_set_q(sh2_t *c, uint32_t v)
{
    c->sr = (c->sr & ~SH2_Q) | ((v ? 1u : 0u) << 8);
}
static inline uint32_t sr_m(const sh2_t *c) { return (c->sr >> 9) & 1u; }
static inline void sr_set_m(sh2_t *c, uint32_t v)
{
    c->sr = (c->sr & ~SH2_M) | ((v ? 1u : 0u) << 9);
}

static inline int32_t sext16(uint32_t v) { return (int32_t)(int16_t)v; }
static inline int32_t sext8(uint32_t v) { return (int32_t)(int8_t)v; }

void sh2_init(sh2_t *c, const sh2_bus_t *bus)
{
    memset(c, 0, sizeof *c);
    c->bus = bus;
    sh2_reset(c);
}

void sh2_reset(sh2_t *c)
{
    for (int i = 0; i < 16; i++)
        c->r[i] = 0;
    c->pc = 0;
    c->next_pc = 2; /* SH-2 instructions are 16-bit */
    c->pr = 0;
    c->sr = SH2_I; /* interrupts masked */
    c->gbr = 0;
    c->vbr = 0;
    c->mach = 0;
    c->macl = 0;
    c->cycles = 0;
}

/* ---- delayed-branch helpers ------------------------------------------------------ */

static inline void branch(sh2_t *c, uint32_t target)
{
    c->next_pc = target;
}

/* ---- interrupt handling ------------------------------------------------------------ */

void sh2_irq(sh2_t *c, int level)
{
    sh2_irq_vector(c, level, (uint32_t)level);
}

/*
 * Interrupt with an explicit vector number. Systems whose interrupt
 * controller supplies a vector (e.g. the Sega Saturn SCU, vectors 0x40-0x5F
 * per the SCU User's Manual interrupt table) vector through
 * VBR + 0x600 + vector*4; several factors may share one level. Systems
 * without a vector number (32X adapter) use sh2_irq(), which keeps the
 * level-derived vector address for compatibility.
 */
void sh2_irq_vector(sh2_t *c, int level, uint32_t vector)
{
    uint32_t lv = (uint32_t)level & 0xFu;
    if ((c->sr & SH2_I) >> 4 >= lv)
        return;
    /* stack SR and PC, vector through VBR */
    c->r[15] -= 4u;
    c->bus->write32(c->bus->user, c->r[15], c->sr);
    c->r[15] -= 4u;
    c->bus->write32(c->bus->user, c->r[15], c->next_pc);
    uint32_t vec = c->bus->read32(c->bus->user, c->vbr + 0x600u + vector * 4u);
    c->sr = (c->sr & ~SH2_I) | (lv << 4);
    c->pc = vec;
    c->next_pc = vec + 2u;
}

/* ---- main interpreter ----------------------------------------------------------------- */

uint32_t sh2_step(sh2_t *c)
{
    const sh2_bus_t *b = c->bus;

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
        else if (op == 0x0019u) { /* DIV0U */
            sr_set_m(c, 0);
            sr_set_q(c, 0);
            sr_set_t(c, 0);
        }
        else if (op == 0x0008u) sr_set_t(c, 0); /* CLRT */
        else if (op == 0x0018u) sr_set_t(c, 1); /* SETT */
        else if (op == 0x0028u) { c->mach = c->macl = 0; } /* CLRMAC */
        else if (op == 0x000Bu) { /* RTS */ branch(c, c->pr); cycles = 2; }
        else if (op == 0x001Bu) { /* SLEEP: documented model: 3 cycles */
            cycles = 3;
        } else if (op == 0x002Bu) { /* RTE */
            uint32_t pc2 = b->read32(b->user, c->r[15]);
            c->r[15] += 4u;
            c->sr = b->read32(b->user, c->r[15]) & 0x3F3u;
            c->r[15] += 4u;
            branch(c, pc2);
            cycles = 4;
        } else if (op == 0x0023u) { /* BRAF Rm */
            branch(c, pc + 4u + c->r[m]);
            cycles = 2;
        } else if (op == 0x0003u) { /* BSRF Rm */
            c->pr = pc + 4u;
            branch(c, pc + 4u + c->r[m]);
            cycles = 2;
        } else if ((op & 0x00FFu) == 0x0009u && n == 0) {
            /* NOP covered above; 0000nnnn00101001 MOVT */
        } else if ((op & 0x00FFu) == 0x0029u) { /* MOVT Rn */
            c->r[n] = sr_t(c);
        } else if ((op & 0x00FFu) == 0x000Au) { /* STS MACH,Rn */
            c->r[n] = c->mach;
        } else if ((op & 0x00FFu) == 0x001Au) { /* STS MACL,Rn */
            c->r[n] = c->macl;
        } else if ((op & 0x00FFu) == 0x002Au) { /* STS PR,Rn */
            c->r[n] = c->pr;
        } else if ((op & 0x000Fu) == 0x0007u) {
            /* 0000nnnnmmmm0111 MUL.L Rm,Rn */
            c->macl = c->r[n] * c->r[m];
            cycles = 2;
        } else if ((op & 0x000Fu) == 0x0004u) {
            /* MOV.B Rm,@(R0,Rn) */
            b->write8(b->user, c->r[0] + c->r[n], (uint8_t)c->r[m]);
            cycles = 2;
        } else if ((op & 0x000Fu) == 0x0005u) {
            /* MOV.W Rm,@(R0,Rn) */
            b->write16(b->user, c->r[0] + c->r[n], (uint16_t)c->r[m]);
            cycles = 2;
        } else if ((op & 0x000Fu) == 0x0006u) {
            /* MOV.L Rm,@(R0,Rn) */
            b->write32(b->user, c->r[0] + c->r[n], c->r[m]);
            cycles = 2;
        } else if ((op & 0x000Fu) == 0x000Cu) {
            /* MOV.B @(R0,Rm),Rn */
            c->r[n] = sext8(b->read8(b->user, c->r[0] + c->r[m]));
            cycles = 2;
        } else if ((op & 0x000Fu) == 0x000Du) {
            /* MOV.W @(R0,Rm),Rn */
            c->r[n] = sext16(b->read16(b->user, c->r[0] + c->r[m]));
            cycles = 2;
        } else if ((op & 0x000Fu) == 0x000Eu) {
            /* MOV.L @(R0,Rm),Rn */
            c->r[n] = b->read32(b->user, c->r[0] + c->r[m]);
            cycles = 2;
        } else if ((op & 0x000Fu) == 0x000Fu) {
            /* MAC.L @Rm+,@Rn+ */
            uint32_t a = b->read32(b->user, c->r[m]);
            c->r[m] += 4u;
            uint32_t d = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            int64_t prod = (int64_t)(int32_t)d * (int64_t)(int32_t)a;
            int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl) + prod;
            c->mach = (uint32_t)((uint64_t)acc >> 32);
            c->macl = (uint32_t)(uint64_t)acc;
            cycles = 3;
        } else {
            goto illegal;
        }
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
            sr_set_q(c, (c->r[n] >> 31) & 1u);
            sr_set_m(c, (c->r[m] >> 31) & 1u);
            sr_set_t(c, sr_q(c) ^ sr_m(c));
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
            uint32_t old_q = sr_q(c);
            uint32_t rm = c->r[m], rn = c->r[n];
            uint32_t q = (rn >> 31) & 1u;
            rn <<= 1;
            rn |= sr_t(c);
            uint32_t tmp0, tmp1;
            if (old_q == 0u) {
                if (sr_m(c) == 0u) {
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
                if (sr_m(c) == 0u) {
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
            sr_set_q(c, q);
            sr_set_t(c, q == sr_m(c));
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
         * bits 11-8. MAC.W is the exception: its Rm field occupies bits 7-4,
         * so it is matched on the low nibble first. */
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
        case 0x07u: /* LDC.L @Rm+,SR */
            c->sr = b->read32(b->user, c->r[n]) & 0x3F3u;
            c->r[n] += 4u;
            cycles = 3;
            break;
        case 0x08u: c->r[n] <<= 2; break;  /* SHLL2  */
        case 0x09u: c->r[n] >>= 2; break;  /* SHLR2  */
        case 0x0Au: c->mach = c->r[n]; break; /* LDS Rm,MACH */
        case 0x0Bu: /* JSR @Rm */
            c->pr = pc + 4u;
            branch(c, c->r[n]);
            cycles = 2;
            break;
        case 0x0Eu: /* LDC Rm,SR */
            c->sr = c->r[n] & 0x3F3u;
            break;
        case 0x0Fu: { /* MAC.W @Rm+,@Rn+ */
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
        case 0x10u: { /* DT (SH-2) */
            c->r[n] -= 1u;
            sr_set_t(c, c->r[n] == 0);
            break;
        }
        case 0x11u: /* CMP/PZ */
            sr_set_t(c, (int32_t)c->r[n] >= 0);
            break;
        case 0x12u: /* STS.L MACL,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->macl);
            break;
        case 0x15u: /* CMP/PL */
            sr_set_t(c, (int32_t)c->r[n] > 0);
            break;
        case 0x16u: /* LDS.L @Rm+,MACL */
            c->macl = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            break;
        case 0x17u: /* LDC.L @Rm+,GBR */
            c->gbr = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            cycles = 3;
            break;
        case 0x18u: c->r[n] <<= 8; break;  /* SHLL8  */
        case 0x19u: c->r[n] >>= 8; break;  /* SHLR8  */
        case 0x1Au: c->macl = c->r[n]; break; /* LDS Rm,MACL */
        case 0x1Eu: /* LDC Rm,GBR */
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
        case 0x22u: /* STS.L PR,@-Rn */
            c->r[n] -= 4u;
            b->write32(b->user, c->r[n], c->pr);
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
        case 0x26u: /* LDS.L @Rm+,PR */
            c->pr = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            break;
        case 0x27u: /* LDC.L @Rm+,VBR */
            c->vbr = b->read32(b->user, c->r[n]);
            c->r[n] += 4u;
            cycles = 3;
            break;
        case 0x28u: c->r[n] <<= 16; break; /* SHLL16 */
        case 0x29u: c->r[n] >>= 16; break; /* SHLR16 */
        case 0x2Au: c->pr = c->r[n]; break; /* LDS Rm,PR */
        case 0x2Bu: /* JMP @Rm */
            branch(c, c->r[n]);
            cycles = 2;
            break;
        case 0x2Eu: /* LDC Rm,VBR */
            c->vbr = c->r[n];
            break;
        default: goto illegal;
        }
        break;

    case 0x5: /* MOV.L @(disp,Rm),Rn */
        c->r[n] = b->read32(b->user, c->r[m] + ((op & 0xFu) << 2));
        cycles = 2;
        break;

    case 0x6:
        switch (op & 0xFu) {
        case 0x0: c->r[n] = sext8(b->read8(b->user, c->r[m])); cycles = 2; break;
        case 0x1: c->r[n] = sext16(b->read16(b->user, c->r[m])); cycles = 2; break;
        case 0x2: c->r[n] = b->read32(b->user, c->r[m]); cycles = 2; break;
        case 0x3: c->r[n] = c->r[m]; break;
        case 0x4: c->r[n] = sext8(b->read8(b->user, c->r[m])); c->r[m] += 1u;
            cycles = 2; break;
        case 0x5: c->r[n] = sext16(b->read16(b->user, c->r[m])); c->r[m] += 2u;
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
        case 0xE: c->r[n] = (uint32_t)(int32_t)(int8_t)c->r[m]; break;
        case 0xF: c->r[n] = (uint32_t)(int32_t)(int16_t)c->r[m]; break;
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
        case 0x4: /* MOV.B @(disp,Rm),R0 */
            c->r[0] = sext8(b->read8(b->user, c->r[m] + (op & 0xFu)));
            break;
        case 0x5: /* MOV.W @(disp,Rm),R0 */
            c->r[0] = sext16(b->read16(b->user, c->r[m] + ((op & 0xFu) << 1)));
            break;
        case 0x8: sr_set_t(c, (uint8_t)c->r[0] == (uint8_t)(op & 0xFFu)); break;
        case 0x9: /* AND #imm,R0 */
            c->r[0] &= op & 0xFFu;
            break;
        case 0xA: c->r[0] ^= op & 0xFFu; break; /* XOR #imm */
        case 0xB: c->r[0] |= op & 0xFFu; break; /* OR #imm */
        case 0xC: /* TST.B #imm,@(R0,GBR) */
            sr_set_t(c, (b->read8(b->user, c->r[0] + c->gbr) &
                         (op & 0xFFu)) == 0);
            cycles = 3;
            break;
        case 0xD: /* AND.B #imm,@(R0,GBR) */
            b->write8(b->user, c->r[0] + c->gbr,
                      b->read8(b->user, c->r[0] + c->gbr) & (op & 0xFFu));
            cycles = 3;
            break;
        case 0xE: /* XOR.B */
            b->write8(b->user, c->r[0] + c->gbr,
                      b->read8(b->user, c->r[0] + c->gbr) ^ (op & 0xFFu));
            cycles = 3;
            break;
        case 0xF: /* OR.B */
            b->write8(b->user, c->r[0] + c->gbr,
                      b->read8(b->user, c->r[0] + c->gbr) | (op & 0xFFu));
            cycles = 3;
            break;
        case 0x2: /* MOV.L R0,@(disp,Rm) */
            b->write32(b->user, c->r[m] + ((op & 0xFu) << 2), c->r[0]);
            break;
        case 0x6: /* MOV.L @(disp,Rm),R0 */
            c->r[0] = b->read32(b->user, c->r[m] + ((op & 0xFu) << 2));
            break;
        case 0x3:
        case 0x7: goto illegal;
        default: goto illegal;
        }
        break;

    case 0x9: /* MOV.W @(disp,PC),Rn (m nibble = disp high) */
        c->r[n] = sext16(b->read16(b->user, pc + 4u + ((op & 0xFFu) << 1)));
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
        case 0x3: { /* TRAPA #imm (immediate branch, no delay slot) */
            uint32_t vec = (op & 0xFFu) * 4u;
            c->r[15] -= 4u;
            b->write32(b->user, c->r[15], c->sr);
            c->r[15] -= 4u;
            b->write32(b->user, c->r[15], c->next_pc);
            c->pc = c->vbr + vec;
            c->next_pc = c->pc + 2u;
            cycles = 8;
            break;
        }
        case 0x4: c->r[0] = sext8(b->read8(b->user, c->gbr + (op & 0xFFu)));
            break;
        case 0x5: c->r[0] = sext16(b->read16(b->user, c->gbr +
                                             ((op & 0xFFu) << 1)));
            break;
        case 0x6: c->r[0] = b->read32(b->user, c->gbr + ((op & 0xFFu) << 2));
            break;
        case 0x7: /* MOVA @(disp,PC),R0 */
            c->r[0] = (pc + 4u) + ((op & 0xFFu) << 2);
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

    default: /* 0xF: not present on SH-2 */
        goto illegal;
    }

    c->cycles += cycles;
    return cycles;

illegal:
    /* Illegal opcode exception through VBR+0x100 (immediate, no slot). */
    c->r[15] -= 4u;
    b->write32(b->user, c->r[15], c->sr);
    c->r[15] -= 4u;
    b->write32(b->user, c->r[15], c->next_pc);
    c->pc = c->vbr + 0x100u;
    c->next_pc = c->pc + 2u;
    c->cycles += 8u;
    return 8u;
}
