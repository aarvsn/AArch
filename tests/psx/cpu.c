/*
 * beatle-psx R3000A CPU tests. Every opcode is a raw 32-bit encoding
 * hand-assembled from the MIPS I specification, and every expected value
 * derives from that specification - never from emulator helpers.
 */
#include "../tests.h"
#include "../testutil.h"
#include "psxh.h"
#include "beatle-psx/psx.h"

#include <stdlib.h>
#include <string.h>

/* --- opcode encoders (MIPS I, per the architecture manual) ---------------- */

#define LUI(rt, imm)       (0x3C000000u | ((rt) << 16) | ((imm) & 0xFFFFu))
#define ORI(rt, rs, imm)   (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define ADDIU(rt, rs, imm) (0x24000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define ADDI(rt, rs, imm)  (0x20000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define ADDU(rd, rs, rt)   (0x00000021u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SUBU(rd, rs, rt)   (0x00000023u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define AND(rd, rs, rt)    (0x00000024u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define OR(rd, rs, rt)     (0x00000025u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define XOR(rd, rs, rt)    (0x00000026u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define NOR(rd, rs, rt)    (0x00000027u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SLT(rd, rs, rt)    (0x0000002Au | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SLTU(rd, rs, rt)   (0x0000002Bu | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SLTI(rt, rs, imm)  (0x28000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SLTIU(rt, rs, imm) (0x2C000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SLL(rd, rt, sa)    (0x00000000u | ((rt) << 16) | ((rd) << 11) | ((sa) << 6))
#define SLLV(rd, rt, rs)   (0x00000004u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SRLV(rd, rt, rs)   (0x00000006u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SRAV(rd, rt, rs)   (0x00000007u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define LWL(rt, off, rs)   (0x88000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LWR(rt, off, rs)   (0x98000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SRL(rd, rt, sa)    (0x00000002u | ((rt) << 16) | ((rd) << 11) | ((sa) << 6))
#define SRA(rd, rt, sa)    (0x00000003u | ((rt) << 16) | ((rd) << 11) | ((sa) << 6))
#define MULT(rs, rt)       (0x00000018u | ((rs) << 21) | ((rt) << 16))
#define MULTU(rs, rt)      (0x00000019u | ((rs) << 21) | ((rt) << 16))
#define DIV(rs, rt)        (0x0000001Au | ((rs) << 21) | ((rt) << 16))
#define DIVU(rs, rt)       (0x0000001Bu | ((rs) << 21) | ((rt) << 16))
#define MFHI(rd)           (0x00000010u | ((rd) << 11))
#define MFLO(rd)           (0x00000012u | ((rd) << 11))
#define MTLO(rs)           (0x00000013u | ((rs) << 21))
#define LW(rt, off, rs)    (0x8C000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SW(rt, off, rs)    (0xAC000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LB(rt, off, rs)    (0x80000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LBU(rt, off, rs)   (0x90000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LH(rt, off, rs)    (0x84000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define LHU(rt, off, rs)   (0x94000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SB(rt, off, rs)    (0xA0000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SH(rt, off, rs)    (0xA4000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define BEQ(rs, rt, off)   (0x10000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define BNE(rs, rt, off)   (0x14000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define JR(rs)             (0x00000008u | ((rs) << 21))
#define JAL(target)        (0x0C000000u | (((target) >> 2) & 0x03FFFFFFu))
#define SYSCALL            0x0000000Cu
#define MFC0(rt, rd)       (0x40000000u | ((rt) << 16) | ((rd) << 11))
#define MTC0(rt, rd)       (0x40800000u | ((rt) << 16) | ((rd) << 11))
#define RFE                0x42000010u
#define MFC1(rt, rd)       (0x44000000u | ((rt) << 16) | ((rd) << 11))

static const uint32_t kBase = 0x80010000u; /* test EXE load address */

/* --- ALU ---------------------------------------------------------------------- */

static void psx_lui_ori(void)
{
    static const uint32_t code[] = {
        LUI(8, 0x1234), ORI(8, 8, 0x5678),
        LUI(9, 0xFFFF), ORI(9, 9, 0xFFFF),
        LUI(10, 0), ORI(10, 0, 0x7FFF),
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 6);
    T_CHECK_EQ(p->cpu.r[8], 0x12345678u);
    T_CHECK_EQ(p->cpu.r[9], 0xFFFFFFFFu);
    T_CHECK_EQ(p->cpu.r[10], 0x7FFFu);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_addu_subu_r0(void)
{
    static const uint32_t code[] = {
        LUI(8, 0x0000), ORI(8, 8, 0x1234),
        LUI(9, 0x0000), ORI(9, 9, 0x2345),
        ADDU(10, 8, 9),            /* 0x3579 */
        SUBU(11, 9, 8),            /* 0x1111 */
        ADDU(0, 8, 9),             /* r0 must stay 0 */
        ADDIU(12, 8, -1),          /* 0x1233 */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 8);
    T_CHECK_EQ(p->cpu.r[10], 0x3579u);
    T_CHECK_EQ(p->cpu.r[11], 0x1111u);
    T_CHECK_EQ(p->cpu.r[0], 0u);
    T_CHECK_EQ(p->cpu.r[12], 0x1233u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_slt_family(void)
{
    static const uint32_t code[] = {
        LUI(8, 0xFFFF), ORI(8, 8, 0xFFFF), /* -1 */
        LUI(9, 0), ORI(9, 9, 1),           /* 1 */
        SLT(10, 8, 9),                     /* signed: -1 < 1 => 1 */
        SLTU(11, 8, 9),                    /* unsigned: 0xFFFFFFFF < 1 => 0 */
        SLTI(12, 9, 0),                    /* 1 < 0 => 0 */
        SLTIU(13, 9, 0xFFFF),              /* 1 < 0xFFFF(=-1 sext) => 1 */
        NOR(14, 9, 9),                     /* ~1 = 0xFFFFFFFE */
        XOR(15, 8, 9),                     /* 0xFFFFFFFE */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 12);
    T_CHECK_EQ(p->cpu.r[10], 1u);
    T_CHECK_EQ(p->cpu.r[11], 0u);
    T_CHECK_EQ(p->cpu.r[12], 0u);
    /* SLTIU sign-extends the immediate then compares unsigned: 1 < 0xFFFFFFFF */
    T_CHECK_EQ(p->cpu.r[13], 1u);
    T_CHECK_EQ(p->cpu.r[14], 0xFFFFFFFEu);
    T_CHECK_EQ(p->cpu.r[15], 0xFFFFFFFEu);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_shifts(void)
{
    static const uint32_t code[] = {
        LUI(8, 0x8000),            /* 0x80000000 */
        SRA(9, 8, 4),              /* 0xF8000000 */
        SRL(10, 8, 4),             /* 0x08000000 */
        SLL(11, 8, 4),             /* 0x00000000 */
        LUI(12, 0), ORI(12, 12, 24),
        SRAV(13, 8, 12),           /* arithmetic >> 24: 0xFFFFFF80 */
        SRLV(14, 8, 12),           /* 0x00000080 */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 9);
    T_CHECK_EQ(p->cpu.r[9], 0xF8000000u);
    T_CHECK_EQ(p->cpu.r[10], 0x08000000u);
    T_CHECK_EQ(p->cpu.r[11], 0x00000000u);
    T_CHECK_EQ(p->cpu.r[13], 0xFFFFFF80u);
    T_CHECK_EQ(p->cpu.r[14], 0x00000080u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_mult_div(void)
{
    static const uint32_t code[] = {
        LUI(8, 0xFFFF), ORI(8, 8, 0xFFF8), /* -8 */
        LUI(9, 0), ORI(9, 9, 3),           /* 3 */
        MULT(8, 9), MFLO(10), MFHI(11),    /* -24: HI=FFFFFFFF LO=FFFFFFE8 */
        LUI(12, 0xFFFF), ORI(12, 12, 0xFFF8),
        MULTU(12, 9), MFLO(13), MFHI(14),  /* 0xFFFFFFF8*3: LO=FFFFFFE8 HI=2 */
        DIV(9, 8), MFLO(15), MFHI(16),     /* 3 / -8 = 0 rem 3: LO=0 HI=3 */
        DIVU(12, 9), MFLO(17), MFHI(18),   /* 0xFFFFFFF8 / 3: LO=0x55555552
                                              HI=2 (C99 trunc semantics) */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 20);
    T_CHECK_EQ(p->cpu.r[10], 0xFFFFFFE8u); /* -24 */
    T_CHECK_EQ(p->cpu.r[11], 0xFFFFFFFFu);
    T_CHECK_EQ(p->cpu.r[13], 0xFFFFFFE8u); /* low 32 of 0x2FFFFFFE8 */
    T_CHECK_EQ(p->cpu.r[14], 0x00000002u);
    /* MIPS DIV truncates toward zero: 3/-8 = 0 remainder 3 */
    T_CHECK_EQ(p->cpu.r[15], 0u);
    T_CHECK_EQ(p->cpu.r[16], 3u);
    /* 0xFFFFFFF8 / 3 = 0x55555552 remainder 2 */
    T_CHECK_EQ(p->cpu.r[17], 0x55555552u);
    T_CHECK_EQ(p->cpu.r[18], 2u);
    emu_core_beatle_psx()->destroy(&p->base);
}

/* --- load delay slot ------------------------------------------------------------- */

static void psx_load_delay(void)
{
    /* LW r8 <- [base]; ADDIU r9, r8, 1 executes BEFORE the load lands, so
     * r9 uses the OLD r8 (0); the instruction after sees the new r8. */
    static const uint32_t code[] = {
        SW(0, 0x100, 0),           /* [base+0x100] = 0x2A (r0=0? no: stores 0) */
        LUI(1, 0), ORI(1, 1, 0x2A),
        SW(1, 0x100, 0),           /* [base+0x100] = 0x2A */
        LW(8, 0x100, 0),           /* load 0x2A into r8 (delayed) */
        ADDIU(9, 8, 1),            /* old r8 = 0 -> r9 = 1 */
        ADDIU(10, 8, 2),           /* new r8 = 0x2A -> r10 = 0x2C */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 8);
    T_CHECK_EQ(p->cpu.r[8], 0x2Au);
    /* r9 took the pre-load value of r8 (0) + 1 */
    T_CHECK_EQ(p->cpu.r[9], 1u);
    /* r10 sees the loaded value */
    T_CHECK_EQ(p->cpu.r[10], 0x2Cu);
    emu_core_beatle_psx()->destroy(&p->base);
}

/* --- branch delay slot ------------------------------------------------------------- */

static void psx_branch_delay(void)
{
    /* BEQ taken: the delay-slot ADDIU executes before the target. */
    static const uint32_t code[] = {
        LUI(1, 0), ORI(1, 1, 1),   /* r1 = 1 */
        BEQ(0, 0, 2),              /* always taken: skip 2 after delay slot */
        ADDIU(2, 2, 5),            /* delay slot: r2 += 5 */
        ADDIU(3, 3, 7),            /* skipped */
        ADDIU(4, 4, 9),            /* target: r4 += 9 */
        ADDIU(5, 5, 1),            /* sentinel path */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 6);
    T_CHECK_EQ(p->cpu.r[2], 5u);   /* delay slot ran */
    T_CHECK_EQ(p->cpu.r[3], 0u);   /* skipped */
    T_CHECK_EQ(p->cpu.r[4], 9u);   /* target ran */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_jal_link(void)
{
    /* JAL at address A: r31 = A + 8 (instruction after the delay slot). */
    static const uint32_t code[] = {
        JAL(0),                    /* patched below: call subroutine */
        ORI(2, 2, 1),              /* delay slot: r2 = 1 */
        ADDIU(6, 6, 1),            /* return lands here: r6 = 1 */
        BEQ(0, 0, 4),              /* skip to end (offset 4 from delay) */
        ADDIU(7, 7, 1),            /* BEQ delay slot: runs (r7 = 1) */
        ORI(8, 8, 1),              /* subroutine: r8 = 1 */
        JR(31),                    /* return to kBase+8 */
        ADDIU(5, 5, 1),            /* JR delay slot: r5 = 1 */
        ADDIU(9, 9, 1),            /* landing pad of BEQ: never r9? runs */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    psx_exe_patch(exe, 0, JAL(kBase + 16u)); /* word 4 = subroutine */
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* trace: JAL -> delay(ORI r2) -> sub(ORI r8) -> JR -> delay(ADDIU r5)
     * -> kBase+8 (ADDIU r6) -> BEQ taken -> delay(ADDIU r7) -> end */
    psx_run_n(p, 8);
    T_CHECK_EQ(p->cpu.r[31], kBase + 8u); /* return address = A + 8 */
    T_CHECK_EQ(p->cpu.r[2], 1u);          /* JAL delay slot ran */
    T_CHECK_EQ(p->cpu.r[8], 1u);          /* subroutine ran */
    T_CHECK_EQ(p->cpu.r[5], 1u);          /* JR delay slot ran */
    T_CHECK_EQ(p->cpu.r[6], 1u);          /* post-return ran */
    T_CHECK_EQ(p->cpu.r[7], 1u);          /* BEQ delay slot ran */
    emu_core_beatle_psx()->destroy(&p->base);
}

/* --- memory: widths and unaligned forms -------------------------------------------- */

static void psx_memory_widths(void)
{
    static const uint32_t code[] = {
        LUI(1, 0x1234), ORI(1, 1, 0x5678),
        SW(1, 0x200, 0),
        LB(2, 0x200, 0),           /* 0x78 sign-extended */
        LBU(3, 0x200, 0),          /* 0x78 */
        LH(4, 0x200, 0),           /* 0x5678 */
        LHU(5, 0x200, 0),          /* 0x5678 */
        LH(6, 0x202, 0),           /* 0x1234 */
        SB(1, 0x210, 0),           /* store low byte 0x78 */
        SH(1, 0x214, 0),           /* store low half 0x5678 */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 12);
    T_CHECK_EQ(p->cpu.r[2], 0x78u);
    T_CHECK_EQ(p->cpu.r[3], 0x78u);
    T_CHECK_EQ(p->cpu.r[4], 0x5678u);
    T_CHECK_EQ(p->cpu.r[5], 0x5678u);
    T_CHECK_EQ(p->cpu.r[6], 0x1234u);
    uint32_t at210 = (uint32_t)p->ram[0x210];
    uint32_t at214 = (uint32_t)p->ram[0x214] | ((uint32_t)p->ram[0x215] << 8);
    T_CHECK_EQ(at210, 0x78u);
    T_CHECK_EQ(at214, 0x5678u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_lwl_lwr(void)
{
    /* Word 0x12345678 at base+0x220; LWL/LWR compose unaligned loads.
     * Little-endian semantics per the MIPS manual:
     *   LWL rt, 1(base): high 3 bytes <- mem bytes 0..2 => 0x345678xx
     *   LWR rt, 4(base): low 3 bytes <- mem bytes 5..7 (of next word) */
    static const uint32_t code[] = {
        LUI(1, 0x1234), ORI(1, 1, 0x5678),
        SW(1, 0x220, 0),
        SW(1, 0x224, 0),
        LUI(2, 0), ORI(2, 2, 0xABCD),  /* old value: 0x0000ABCD */
        LWL(2, 0x221, 0),              /* r2 = 0x5678ABCD */
        LUI(3, 0xAB00),                /* old r3 = 0xAB000000 */
        LWR(3, 0x225, 0),              /* r3 = 0xAB123456 */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 12);
    /* LWL n=1: v = (word << 16) | (old & 0xFFFF) = 0x56780000 | 0xABCD */
    T_CHECK_EQ(p->cpu.r[2], 0x5678ABCDu);
    /* LWR at offset 1 of word 0x12345678: v = (word >> 8) | old&0xFF000000
     * = 0x00123456 | 0xAB000000 = 0xAB123456. */
    T_CHECK_EQ(p->cpu.r[3], 0xAB123456u);
    emu_core_beatle_psx()->destroy(&p->base);
}

/* --- exceptions ---------------------------------------------------------------------- */

static void psx_syscall_exception(void)
{
    static const uint32_t code[] = {
        ADDIU(1, 1, 1),            /* 0x80010000 */
        SYSCALL,                   /* 0x80010004 */
        ADDIU(2, 2, 2),            /* must not run */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 2);
    T_CHECK_EQ(p->cpu.cop0_cause & 0x7Cu, 8u << 2); /* ExcCode = 8 */
    T_CHECK_EQ(p->cpu.cop0_epc, kBase + 4u);        /* EPC = syscall addr */
    T_CHECK_EQ(p->cpu.pc, 0x80000080u);             /* BEV=0 vector */
    T_CHECK_EQ(p->cpu.r[2], 0u);                    /* skipped */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_cop0_roundtrip_rfe(void)
{
    static const uint32_t code[] = {
        LUI(8, 0x1234), ORI(8, 8, 0x5678),
        MTC0(8, 12),               /* SR = 0x12345678 */
        MFC0(9, 12),               /* read back (delayed) */
        LUI(10, 0), ORI(10, 10, 1),
        MTC0(10, 12),              /* SR = 1 */
        RFE,                       /* SR = ((1&0x3F)<<2 stuff) */
        MFC0(11, 12),
        MFC0(12, 15),              /* PRID read */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 12);
    T_CHECK_EQ(p->cpu.r[9], 0x12345678u);
    /* RFE shifts the mode stack: SR = ((SR>>2)&0xF) | (SR&0x3F)<<2 | rest */
    {
        uint32_t sr1 = 1u;
        uint32_t expect = (0xFFFFFFC0u & sr1) | ((sr1 >> 2) & 0x0Fu) |
                          ((sr1 & 0x0Fu) << 2);
        T_CHECK_EQ(p->cpu.r[11], expect);
    }
    T_CHECK(p->cpu.r[12] != 0u); /* PRID = 2 */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_cop1_unusable(void)
{
    static const uint32_t code[] = {
        MFC1(8, 0),                /* COP1 unusable on R3000A */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 1);
    T_CHECK_EQ(p->cpu.cop0_cause & 0x7Cu, 11u << 2); /* CPU exception */
    T_CHECK_EQ((p->cpu.cop0_cause >> 28) & 3u, 1u);  /* CE = 1 */
    T_CHECK_EQ(p->cpu.pc, 0x80000080u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_address_error(void)
{
    static const uint32_t code[] = {
        LUI(5, 0x8001), ORI(5, 5, 0x0201),
        LH(8, 0, 5),               /* unaligned halfword load -> AdEL */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 3);
    T_CHECK_EQ(p->cpu.cop0_cause & 0x7Cu, 4u << 2); /* AdEL = 4 */
    T_CHECK_EQ(p->cpu.cop0_badva, kBase + 0x201u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_addi_overflow(void)
{
    static const uint32_t code[] = {
        LUI(8, 0x7FFF), ORI(8, 8, 0xFFFF), /* 0x7FFFFFFF */
        ADDI(9, 8, 1),                     /* overflow */
        ADDIU(10, 10, 1),                  /* must not run */
    };
    size_t size;
    uint8_t *exe = psx_test_exe(kBase, kBase, code, sizeof code / sizeof code[0], 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    T_CHECK(p != NULL);
    if (!p)
        return;
    psx_run_n(p, 3);
    T_CHECK_EQ(p->cpu.cop0_cause & 0x7Cu, 12u << 2); /* Ov = 12 */
    T_CHECK_EQ(p->cpu.r[10], 0u);
    emu_core_beatle_psx()->destroy(&p->base);
}

/* --- register suite -------------------------------------------------------------------- */

T_SUITE_BEGIN(beatle_psx_cpu)
{ "lui_ori", psx_lui_ori },
{ "addu_subu_r0", psx_addu_subu_r0 },
{ "slt_family", psx_slt_family },
{ "shifts", psx_shifts },
{ "mult_div", psx_mult_div },
{ "load_delay", psx_load_delay },
{ "branch_delay", psx_branch_delay },
{ "jal_link", psx_jal_link },
{ "memory_widths", psx_memory_widths },
{ "lwl_lwr", psx_lwl_lwr },
{ "syscall_exception", psx_syscall_exception },
{ "cop0_roundtrip_rfe", psx_cop0_roundtrip_rfe },
{ "cop1_unusable", psx_cop1_unusable },
{ "address_error", psx_address_error },
{ "addi_overflow", psx_addi_overflow },
T_SUITE_END

T_SUITE_REG(beatle_psx_cpu)
