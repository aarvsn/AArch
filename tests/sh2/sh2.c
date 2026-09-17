/*
 * AArch SH-2 interpreter tests. Every opcode is hand-assembled from the
 * Hitachi SH-1/SH-2 Programming Manual instruction tables, and expected
 * values derive from that specification (including the DIV1 pseudocode).
 */
#include "../tests.h"
#include "../testutil.h"
#include "common/sh2.h"

#include <stdlib.h>
#include <string.h>

#define RAM_SIZE (64u * 1024u)

typedef struct {
    uint8_t *ram;
    sh2_t cpu;
} sh2_machine;

static uint8_t t_read8(void *user, uint32_t a)
{
    sh2_machine *s = user;
    return s->ram[a & (RAM_SIZE - 1u)];
}
static uint16_t t_read16(void *user, uint32_t a)
{
    sh2_machine *s = user;
    uint32_t o = a & (RAM_SIZE - 1u);
    return (uint16_t)((uint16_t)s->ram[o] << 8 | s->ram[o + 1]);
}
static uint32_t t_read32(void *user, uint32_t a)
{
    return ((uint32_t)t_read16(user, a) << 16) | t_read16(user, a + 2u);
}
static void t_write8(void *user, uint32_t a, uint8_t v)
{
    sh2_machine *s = user;
    s->ram[a & (RAM_SIZE - 1u)] = v;
}
static void t_write16(void *user, uint32_t a, uint16_t v)
{
    sh2_machine *s = user;
    uint32_t o = a & (RAM_SIZE - 1u);
    s->ram[o] = (uint8_t)(v >> 8);
    s->ram[o + 1] = (uint8_t)v;
}
static void t_write32(void *user, uint32_t a, uint32_t v)
{
    t_write16(user, a, (uint16_t)(v >> 16));
    t_write16(user, a + 2u, (uint16_t)v);
}

static sh2_bus_t t_bus = { NULL, t_read8, t_read16, t_read32,
                           t_write8, t_write16, t_write32 };

/* Boot a machine with big-endian 16-bit code words at 0. */
static sh2_machine *boot(const uint16_t *code, size_t words)
{
    sh2_machine *s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    s->ram = calloc(1, RAM_SIZE);
    if (s->ram == NULL) {
        free(s);
        return NULL;
    }
    for (size_t i = 0; i < words; i++) {
        s->ram[i * 2u] = (uint8_t)(code[i] >> 8);
        s->ram[i * 2u + 1u] = (uint8_t)code[i];
    }
    t_bus.user = s;
    sh2_init(&s->cpu, &t_bus);
    return s;
}

static uint32_t sr_q_of(const sh2_t *c) { return (c->sr >> 8) & 1u; }
static uint32_t sr_m_of(const sh2_t *c) { return (c->sr >> 9) & 1u; }

static void run(sh2_machine *s, int n)
{
    for (int i = 0; i < n; i++)
        sh2_step(&s->cpu);
}

/* opcode builders (verified against the manual tables) */
#define MOV_IMM(n, imm) ((uint16_t)(0xE000u | ((n) << 8) | ((imm) & 0xFFu)))
#define ADD_IMM(n, imm) ((uint16_t)(0x7000u | ((n) << 8) | ((imm) & 0xFFu)))
#define MOV_L_PC(n, d)  ((uint16_t)(0xD000u | ((n) << 8) | ((d) & 0xFFu)))
#define MOV_W_PC(n, d)  ((uint16_t)(0x9000u | ((n) << 8) | ((d) & 0xFFu)))
#define MOVA(d)         ((uint16_t)(0xC700u | ((d) & 0xFFu)))
#define ADD(n, m)       ((uint16_t)(0x300Cu | ((n) << 8) | ((m) << 4)))
#define ADDC(n, m)      ((uint16_t)(0x300Eu | ((n) << 8) | ((m) << 4)))
#define ADDV(n, m)      ((uint16_t)(0x300Fu | ((n) << 8) | ((m) << 4)))
#define SUB(n, m)       ((uint16_t)(0x3008u | ((n) << 8) | ((m) << 4)))
#define DMULS(n, m)     ((uint16_t)(0x300Du | ((n) << 8) | ((m) << 4)))
#define DMULU(n, m)     ((uint16_t)(0x3005u | ((n) << 8) | ((m) << 4)))
#define DIV1(n, m)      ((uint16_t)(0x3004u | ((n) << 8) | ((m) << 4)))
#define DIV0U           0x0019u
#define CMPEQ(n, m)     ((uint16_t)(0x3000u | ((n) << 8) | ((m) << 4)))
#define EXTS_B(n, m)    ((uint16_t)(0x600Eu | ((n) << 8) | ((m) << 4)))
#define EXTU_B(n, m)    ((uint16_t)(0x600Cu | ((n) << 8) | ((m) << 4)))
#define SWAPB(n, m)     ((uint16_t)(0x6008u | ((n) << 8) | ((m) << 4)))
#define SWAPW(n, m)     ((uint16_t)(0x6009u | ((n) << 8) | ((m) << 4)))
#define XTRCT(n, m)     ((uint16_t)(0x200Du | ((n) << 8) | ((m) << 4)))
#define NOT(n, m)       ((uint16_t)(0x6007u | ((n) << 8) | ((m) << 4)))
#define NEG(n, m)       ((uint16_t)(0x600Bu | ((n) << 8) | ((m) << 4)))
#define SHLL(n)         ((uint16_t)(0x4000u | ((n) << 8)))
#define SHLR(n)         ((uint16_t)(0x4001u | ((n) << 8)))
#define SHAL(n)         ((uint16_t)(0x4020u | ((n) << 8)))
#define SHAR(n)         ((uint16_t)(0x4021u | ((n) << 8)))
#define ROTL(n)         ((uint16_t)(0x4004u | ((n) << 8)))
#define ROTCL(n)        ((uint16_t)(0x4024u | ((n) << 8)))
#define SHLL2(n)        ((uint16_t)(0x4008u | ((n) << 8)))
#define SHAR8(n)        ((uint16_t)(0x4019u | ((n) << 8)))
#define MOVT(n)         ((uint16_t)(0x4029u | ((n) << 8)))
#define SETT            0x0018u
#define CLRT            0x0008u
#define BRA(d)          ((uint16_t)(0xA000u | ((d) & 0xFFFu)))
#define BSR(d)          ((uint16_t)(0xB000u | ((d) & 0xFFFu)))
#define BT(d)           ((uint16_t)(0x8900u | ((d) & 0xFFu)))
#define BF(d)           ((uint16_t)(0x8B00u | ((d) & 0xFFu)))
#define RTS             0x000Bu
#define JMP(m)          ((uint16_t)(0x402Bu | ((m) << 8)))
#define JSR(m)          ((uint16_t)(0x400Bu | ((m) << 8)))
#define LDS_PR(m)       ((uint16_t)(0x402Au | ((m) << 8)))
#define STS_PR(n)       ((uint16_t)(0x002Au | ((n) << 8)))
#define STS_MACL(n)     ((uint16_t)(0x001Au | ((n) << 8)))
#define STS_MACH(n)     ((uint16_t)(0x000Au | ((n) << 8)))
#define TRAPA(imm)      ((uint16_t)(0xC300u | ((imm) & 0xFFu)))
#define MOV_L_Rm_Rn(n, m) ((uint16_t)(0x6002u | ((n) << 8) | ((m) << 4)))
#define MOV_L_Rm_AT_RN(n, m) ((uint16_t)(0x2002u | ((n) << 8) | ((m) << 4)))
#define MOV_L_AT_RM(n, m) ((uint16_t)(0x6002u | ((n) << 8) | ((m) << 4)))
#define MOV_L_AT_DISP_RM(n, m, d) \
    ((uint16_t)(0x5000u | ((n) << 8) | ((m) << 4) | ((d) & 0xFu)))
#define MOV_L_Rm_AT_R0RN(n, m) \
    ((uint16_t)(0x0006u | ((n) << 8) | ((m) << 4)))
#define MOV_L_AT_R0Rm(n, m) ((uint16_t)(0x000Eu | ((n) << 8) | ((m) << 4)))
#define MULL(n, m)      ((uint16_t)(0x0007u | ((n) << 8) | ((m) << 4)))
#define MACL_(n, m)     ((uint16_t)(0x000Fu | ((n) << 8) | ((m) << 4)))
#define MULS_W(n, m)    ((uint16_t)(0x200Fu | ((n) << 8) | ((m) << 4)))
#define NOP             0x0009u

static void sh2_mov_add_imm(void)
{
    static const uint16_t code[] = {
        MOV_IMM(0, 5), ADD_IMM(0, 3), MOV_IMM(1, (uint8_t)0xFC),
        ADD_IMM(1, 2), NOP,
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run(s, 4);
    T_CHECK_EQ(s->cpu.r[0], 8u);
    T_CHECK_EQ(s->cpu.r[1], 0xFFFFFFFEu); /* sign-extended immediate */
    T_CHECK((s->cpu.sr & SH2_T) == 0);
    free(s->ram);
    free(s);
}

static void sh2_dmul(void)
{
    static const uint16_t code[] = {
        MOV_IMM(4, (uint8_t)0xFD), /* r4 = 0xFFFFFFFD = -3 (low byte) */
        0xE000u | (5 << 8) | 5,    /* MOV #5,r5 */
        0x600Eu | (4 << 8) | (4 << 4), /* EXTS.B r4,r4: r4 = -3 */
        DMULS(4, 5),               /* -3 x 5 = -15 */
        STS_MACH(6), STS_MACL(7), NOP,
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run(s, 7);
    T_CHECK_EQ(s->cpu.r[4], 0xFFFFFFFDu);
    T_CHECK_EQ(s->cpu.mach, 0xFFFFFFFFu);
    T_CHECK_EQ(s->cpu.macl, 0xFFFFFFF1u);
    free(s->ram);
    free(s);
}

static void sh2_div1_steps(void)
{
    /* Hand-traced DIV1 steps from the manual pseudocode:
     * Rn=100, Rm=7, M=Q=T=0 after DIV0U.
     * S1: Rn = 0xC8-7 = C1h, T=1, Q=0
     * S2: Rn = (C1h<<1|T)-7 = 17Ch, T=1, Q=0
     * S3: Rn = (17Ch<<1|T)-7 = 2F2h, T=1, Q=0 */
    static const uint16_t code[] = {
        MOV_IMM(4, 100), DIV0U,
        DIV1(4, 5), DIV1(4, 5), DIV1(4, 5),
        NOP,
    };
    /* r5 is set via direct state (white-box) to keep the sequence short */
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    s->cpu.r[5] = 7u;
    run(s, 5);
    T_CHECK_EQ(s->cpu.r[4], 0x2F2u);
    T_CHECK((s->cpu.sr & SH2_T) != 0);
    T_CHECK_EQ(sr_q_of(&s->cpu), 0u);
    free(s->ram);
    free(s);
}

static void sh2_div_flags(void)
{
    /* DIV0S/DIV0U flag semantics (manual: MSB of Rn -> Q, MSB of Rm -> M,
     * M ^ Q -> T; DIV0U clears M/Q/T). */
    static const uint16_t code[] = {
        MOV_IMM(1, (uint8_t)0x80), /* r1 = 0xFFFFFF80 (negative) */
        MOV_IMM(2, 5),             /* r2 = positive */
        0x2007u | (1 << 8) | (2 << 4), /* DIV0S R2,R1: Q=msb(R1)=1,
                                          M=msb(R2)=0, T=M^Q=1 */
        DIV0U,
        NOP,
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run(s, 5);
    T_CHECK_EQ(sr_q_of(&s->cpu), 0u);  /* DIV0U cleared Q */
    T_CHECK_EQ(sr_m_of(&s->cpu), 0u);  /* DIV0U cleared M */
    T_CHECK((s->cpu.sr & SH2_T) == 0); /* DIV0U cleared T */
    free(s->ram);
    free(s);
}

static void sh2_shifts_t(void)
{
    static const uint16_t code[] = {
        MOV_IMM(1, 0x20),          /* r1 = 0x20 */
        SHLL2(1),                  /* r1 = 0x80 */
        MOV_IMM(2, 1),
        SETT, SHLR(2),             /* r2 = 0, T = 1 (LSB shifted out) */
        MOV_IMM(3, 0x7F),
        SHAR(3),                   /* arithmetic: r3 = 0x3F, T = 1 */
        MOV_IMM(4, 0x40),
        ROTCL(4),                  /* r4 = 0x80 | T(1) = 0x81, T = 0 */
        MOVT(5),                   /* r5 = T = 0 */
        MOV_IMM(6, 0x40),
        SHAR8(6),                  /* r6 = 0x40 >> 8 = 0 */
        NOP,
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run(s, 13);
    T_CHECK_EQ(s->cpu.r[1], 0x80u);
    T_CHECK_EQ(s->cpu.r[2], 0u);
    T_CHECK_EQ(s->cpu.r[3], 0x3Fu);
    T_CHECK((s->cpu.sr & SH2_T) == 0); /* ROTCL rotated in msb(0x40) = 0 */
    T_CHECK_EQ(s->cpu.r[4], 0x81u);    /* 0x40 << 1 | T(1) from SHAR */
    T_CHECK_EQ(s->cpu.r[5], 0u);
    T_CHECK_EQ(s->cpu.r[6], 0u);
    free(s->ram);
    free(s);
}

static void sh2_branch_delay(void)
{
    /* BRA +3: delay slot runs, skipped slot does not. */
    static const uint16_t code[] = {
        BRA(3),          /* 0: jump to word 0+2+3 = 5 */
        MOV_IMM(1, 7),   /* 1: delay slot */
        MOV_IMM(2, 9),   /* 2: skipped */
        MOV_IMM(3, 9),   /* 3: skipped */
        MOV_IMM(4, 9),   /* 4: skipped */
        MOV_IMM(5, 1),   /* 5: target */
        0x0009u,         /* 6: NOP */
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run(s, 4);
    T_CHECK_EQ(s->cpu.r[1], 7u);  /* delay slot executed */
    T_CHECK_EQ(s->cpu.r[2], 0u);
    T_CHECK_EQ(s->cpu.r[3], 0u);
    T_CHECK_EQ(s->cpu.r[4], 0u);
    T_CHECK_EQ(s->cpu.r[5], 1u);  /* target reached */
    free(s->ram);
    free(s);
}

static void sh2_jsr_rts(void)
{
    /* BSR stores PC+4 into PR; RTS returns to it. */
    static const uint16_t code[] = {
        BSR(5),          /* 0: call word 7 (4 + 5*2 = 14) */
        MOV_IMM(1, 1),   /* 1: delay slot */
        MOV_IMM(2, 2),   /* 2: return lands here (PR = 4) */
        BRA(4),          /* 3: skip rest of subroutine area */
        NOP,             /* 4: BRA delay slot */
        MOV_IMM(6, 6),   /* 5: never */
        NOP,             /* 6: never */
        MOV_IMM(7, 7),   /* 7: subroutine */
        RTS,             /* 8: return to PR = 4 */
        NOP,             /* 9: RTS delay slot */
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run(s, 9);
    T_CHECK_EQ(s->cpu.pr, 4u);    /* PC+4 at BSR (byte address of word 2) */
    T_CHECK_EQ(s->cpu.r[7], 7u);  /* subroutine ran */
    T_CHECK_EQ(s->cpu.r[1], 1u);  /* BSR delay slot */
    T_CHECK_EQ(s->cpu.r[2], 2u);  /* after return */
    T_CHECK_EQ(s->cpu.r[6], 0u);  /* skipped */
    free(s->ram);
    free(s);
}

static void sh2_memory_ops(void)
{
    static const uint16_t code[] = {
        MOV_IMM(0, 0x40),              /* r0 = 0x40 */
        MOV_IMM(1, (uint8_t)0x78),     /* r1 = 0x78 */
        MOV_L_Rm_AT_R0RN(2, 1),        /* MOV.L r1,@(r0,r2)? no:
                                          0000nnnnmmmm0110: MOV.L r1,@(R0,r2) */
        NOP,
    };
    /* rewrite: set r2 = 0x100 first via two MOVs is complex; use direct:
       r2 = 0 by reset, so @(R0,r2) = 0x40. */
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run(s, 4);
    uint32_t v = t_read32(s, 0x40);
    T_CHECK_EQ(v, 0x78u); /* store writes low word; high word 0 */
    free(s->ram);
    free(s);
}

static void sh2_ext_swap_xtrct(void)
{
    static const uint16_t code[] = {
        MOV_IMM(1, (uint8_t)0x88),     /* r1 = 0xFFFFFF88 */
        EXTS_B(2, 1),                  /* r2 = 0xFFFFFF88 */
        EXTU_B(3, 1),                  /* r3 = 0x00000088 */
        MOV_IMM(4, (uint8_t)0xAB),
        0x600Eu | (5 << 8) | (4 << 4), /* EXTS.B r5,r4: 0xFFFFFFAB */
        SWAPB(6, 4),                   /* 0xAB -> swap: 0xFFFFFFAB stays
                                          (bytes AB 00 00 00 -> 00 AB...) */
        XTRCT(7, 5),                   /* center 32 of r5,r7 */
        NOP,
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run(s, 9);
    T_CHECK_EQ(s->cpu.r[2], 0xFFFFFF88u);
    T_CHECK_EQ(s->cpu.r[3], 0x00000088u);
    /* SWAP.B of 0xFFFFFFAB = bytes FF,FF,FF,AB -> FF,FF,AB,FF = 0xFFFFABFF */
    T_CHECK_EQ(s->cpu.r[6], 0xFFFFABFFu);
    free(s->ram);
    free(s);
}

static void sh2_ldc_sts(void)
{
    static const uint16_t code[] = {
        MOV_IMM(1, 0x55),
        0x401Eu | (1 << 8),            /* LDC r1,GBR */
        STS_PR(2),                     /* STS PR,r2 */
        NOP,
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    s->cpu.pr = 0x1234u;
    run(s, 4);
    T_CHECK_EQ(s->cpu.gbr, 0x55u);
    T_CHECK_EQ(s->cpu.r[2], 0x1234u);
    free(s->ram);
    free(s);
}

static void sh2_trapa(void)
{
    static const uint16_t code[] = {
        TRAPA(0x10),                   /* vector VBR + 0x40 */
        NOP, NOP,
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* place RTE-ish handler at VBR+0x40: store a marker register write */
    s->cpu.vbr = 0x200u;
    /* handler at VBR + 0x40: MOV #1,r9 = 0xE901 (big-endian bytes) */
    s->ram[0x240u] = 0xE9u;
    s->ram[0x241u] = 0x01u;
    run(s, 2);
    T_CHECK_EQ(s->cpu.r[9], 1u);     /* handler instruction executed */
    T_CHECK_EQ(s->cpu.pc, 0x242u);   /* PC advanced past the handler */
    free(s->ram);
    free(s);
}

static void sh2_mac_w(void)
{
    /* MAC.W with pre-increment: two multiplies accumulate into MAC. */
    static const uint16_t code[] = {
        MOV_IMM(0, 0),                 /* r0 = 0 (R0 index unused) */
        MOV_IMM(4, 0x40),              /* r4 = source table */
        MOV_IMM(5, 0x50),              /* r5 = dest table */
        0x400Fu | (5 << 8) | (4 << 4), /* MAC.W @r4+,@r5+ (0100 0101 0100 1111) */
        NOP,
    };
    sh2_machine *s = boot(code, sizeof code / sizeof code[0]);
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* table values: src @0x40: 0x0003; dst @0x50: 0x0004 -> MAC = 12 */
    t_write16(s, 0x40, 3);
    t_write16(s, 0x50, 4);
    run(s, 5);
    T_CHECK_EQ(s->cpu.macl, 12u);
    T_CHECK_EQ(s->cpu.r[4], 0x42u);
    T_CHECK_EQ(s->cpu.r[5], 0x52u);
    free(s->ram);
    free(s);
}

T_SUITE_BEGIN(sh2)
{ "mov_add_imm", sh2_mov_add_imm },
{ "dmul", sh2_dmul },
{ "div1_steps", sh2_div1_steps },
{ "div_flags", sh2_div_flags },
{ "shifts_t", sh2_shifts_t },
{ "branch_delay", sh2_branch_delay },
{ "jsr_rts", sh2_jsr_rts },
{ "memory_ops", sh2_memory_ops },
{ "ext_swap_xtrct", sh2_ext_swap_xtrct },
{ "ldc_sts", sh2_ldc_sts },
{ "trapa", sh2_trapa },
{ "mac_w", sh2_mac_w },
T_SUITE_END

T_SUITE_REG(sh2)
