/*
 * mgbax ARM instruction tests. Every opcode is a raw 32-bit encoding
 * hand-assembled from the ARM ARM (ARMv4), and every expected value is
 * derived from the architectural specification, never from emulator
 * helper functions. PC-relative expectations use the ARM7 pipeline rule
 * (PC reads instruction address + 8; +12 for register-specified shifts).
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbax/gba.h"

#include <stdlib.h>
#include <string.h>

static struct gba *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbax()->create(&c) != EMU_OK)
        return NULL;
    return (struct gba *)c;
}

/* Loads a program into ROM (mapped at $08000000), PC at the start. */
static struct gba *mk_cpu_core(const uint32_t *code, size_t words)
{
    struct gba *g = mk_core();
    if (g == NULL)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    memcpy(rom, code, words * 4u);
    emu_result_t r = emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_mgbax()->destroy(&g->base);
        return NULL;
    }
    return g;
}

static void run_n(struct gba *g, int count)
{
    for (int i = 0; i < count; i++)
        gba_cpu_step(g);
}

/* --- data processing: immediates and flags ---------------------------------- */

static void arm_mov_imm_rotated(void)
{
    /* MOV R0, #0xF0000000 : imm8=0x0F ror 4 (rot field 2) */
    static const uint32_t prog[] = { 0xE3A0020Fu, 0xEAFFFFFEu };
    struct gba *g = mk_cpu_core(prog, 2);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[0], 0xF0000000u);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_movs_imm_carry(void)
{
    /* MOVS R1, #0xF0000000: shifter carry = bit31 of the rotated result = 1 */
    static const uint32_t prog[] = { 0xE3B0120Fu, 0xEAFFFFFEu };
    struct gba *g = mk_cpu_core(prog, 2);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[1], 0xF0000000u);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(g->cpu.cpsr & GBA_N);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_adds_overflow(void)
{
    /* 0x40000000 + 0x40000000 = 0x80000000: C=0, V=1, N=1 */
    static const uint32_t prog[] = {
        0xE3A00101u, /* MOV R0, #0x40000000 (imm 1 ror 2) */
        0xE0900000u, /* ADDS R0, R0, R0 */
        0xEAFFFFFEu  /* B self */
    };
    struct gba *g = mk_cpu_core(prog, 3);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[0], 0x80000000u);
    T_CHECK(g->cpu.cpsr & GBA_V);
    T_CHECK(g->cpu.cpsr & GBA_N);
    T_CHECK(!(g->cpu.cpsr & GBA_C));
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_adds_carry_out(void)
{
    /* MVN R0, #0 -> 0xFFFFFFFF; ADDS R0, R0, R0 -> 0xFFFFFFFE, C=1, V=0 */
    static const uint32_t prog[] = {
        0xE3E00000u, /* MVN R0, #0 */
        0xE0900000u, /* ADDS R0, R0, R0 */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 3);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[0], 0xFFFFFFFEu);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(g->cpu.cpsr & GBA_N);
    T_CHECK(!(g->cpu.cpsr & GBA_V));
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_adc_carry_in(void)
{
    /* CMP R1,R1 sets C=1; ADCS R0,R0,#0: 0xFFFFFFFF+0+1 = 0, C=1, Z=1 */
    static const uint32_t prog[] = {
        0xE3E00000u, /* MVN R0, #0       -> 0xFFFFFFFF */
        0xE3A0100Bu, /* MOV R1, #0x0B    */
        0xE1510001u, /* CMP R1, R1       -> C = 1 */
        0xE2B00000u, /* ADCS R0, R0, #0  -> 0, C=1 (carry out), Z=1 */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 5);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 4);
    T_CHECK_EQ(g->cpu.r[0], 0x00000000u);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(g->cpu.cpsr & GBA_Z);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_sbc_borrow(void)
{
    /* SBCS R0, R0, #0 with C=0: 5 - 0 - 1 = 4, C=1 (no borrow) */
    static const uint32_t prog[] = {
        0xE3A00005u, /* MOV R0, #5 */
        0xE3A01000u, /* MOV R1, #0 */
        0xE1500001u, /* CMP R0, R1 -> C=1 */
        0xE1510000u, /* CMP R1, R0 -> C=0 */
        0xE2D00000u, /* SBCS R0, R0, #0 -> 4, C=1 */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 5);
    T_CHECK_EQ(g->cpu.r[0], 4u);
    T_CHECK(g->cpu.cpsr & GBA_C);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_sbc_borrow_edge(void)
{
    /* Spec edge: SBCS R0, R0, R1 with R0=0, R1=0xFFFFFFFF, C=0:
     * 0 - 0xFFFFFFFF - 1 = -0x100000000 -> R0 = 0 (mod 2^32), borrow out
     * -> C=0, Z=1. A 32-bit (b + borrow_in) comparison would wrap to 0
     * and wrongly set C=1. */
    static const uint32_t prog[] = {
        0xE3A00000u, /* MOV R0, #0 */
        0xE3E01000u, /* MVN R1, #0 -> 0xFFFFFFFF */
        0xE3A03005u, /* MOV R3, #5 */
        0xE1530001u, /* CMP R3, R1 -> 5 < 0xFFFFFFFF: C=0, N=1 */
        0xE0D00001u, /* SBCS R0, R0, R1 -> 0, C=0 (borrow), Z=1 */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 5);
    T_CHECK_EQ(g->cpu.r[0], 0u);
    T_CHECK(!(g->cpu.cpsr & GBA_C)); /* borrow occurred */
    T_CHECK(g->cpu.cpsr & GBA_Z);
    T_CHECK(!(g->cpu.cpsr & GBA_N));
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_cmp_vs_cmn(void)
{
    /* CMP 0x80000000, 1: C=1 (no borrow), V=1, N=0.
     * CMN 0xFFFFFFFF, 1: C=1, Z=1, V=0. */
    static const uint32_t prog[] = {
        0xE3A00101u, /* MOV R0, #0x40000000 */
        0xE0800000u, /* ADD R0, R0, R0      -> 0x80000000 (no S) */
        0xE3500001u, /* CMP R0, #1          -> C=1, V=1, N=0 */
        0xE3E01000u, /* MVN R1, #0          -> 0xFFFFFFFF */
        0xE3710001u, /* CMN R1, #1          -> Z=1, C=1, V=0 */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 3);
    T_CHECK(g->cpu.cpsr & GBA_V);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(!(g->cpu.cpsr & GBA_N));
    run_n(g, 2);
    T_CHECK(g->cpu.cpsr & GBA_Z);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(!(g->cpu.cpsr & GBA_V));
    emu_core_mgbax()->destroy(&g->base);
}

/* --- PC pipeline semantics --------------------------------------------------- */

static void arm_pc_read_operand2(void)
{
    /* MOV R0, PC at 0x08000000: R0 = instruction + 8 = 0x08000008 */
    static const uint32_t prog[] = {
        0xE1A0000Fu, /* MOV R0, PC (no shift) */
        0xE3A02008u, /* MOV R2, #8 */
        0xE1A0121Fu, /* MOV R1, PC, LSL R2 : PC reads +12, << 8 */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 4);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[0], 0x08000008u);
    run_n(g, 2);
    /* instruction at 0x08000008: PC reads 0x08000008 + 12 = 0x08000014,
     * LSL by 8 -> 0x00001400 */
    T_CHECK_EQ(g->cpu.r[1], 0x00001400u);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_ldr_pc_base(void)
{
    /* LDR R0, [PC, #4] at 0x08000000: base = instr+8 = 0x08000008,
     * +4 -> 0x0800000C (word 3) */
    static const uint32_t prog[] = {
        0xE59F0004u, /* LDR R0, [PC, #4] */
        0xEAFFFFFEu, /* B self */
        0x11111111u, /* word 2: not the target */
        0xDEADBEEFu  /* word 3: literal */
    };
    struct gba *g = mk_cpu_core(prog, 4);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[0], 0xDEADBEEFu);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_branch_link(void)
{
    /* BL +8: LR = 0x08000004, target = 0x08000008 */
    static const uint32_t prog[] = {
        0xEB000001u, /* 0: BL +12 -> 0x0800000C */
        0xE3A03077u, /* 1: MOV R3, #0x77 (return lands here) */
        0xEAFFFFFEu, /* 2: B self */
        0xE3A02055u, /* 3: MOV R2, #0x55 (call target) */
        0xE12FFF1Eu  /* 4: BX LR -> 0x08000004 */
    };
    struct gba *g = mk_cpu_core(prog, 5);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1); /* BL */
    T_CHECK_EQ(g->cpu.r[14], 0x08000004u);
    T_CHECK_EQ(g->cpu.r[15], 0x0800000Cu);
    run_n(g, 2); /* MOV R2, BX LR */
    T_CHECK_EQ(g->cpu.r[2], 0x55u);
    T_CHECK_EQ(g->cpu.r[15], 0x08000004u);
    run_n(g, 1); /* MOV R3 */
    T_CHECK_EQ(g->cpu.r[3], 0x77u);
    emu_core_mgbax()->destroy(&g->base);
}

/* --- shifts ------------------------------------------------------------------ */

static void arm_shifts_carry(void)
{
    /* Register-specified shift amounts: LSL/LSR #32 produce 0 with carry
     * = bit0/bit31; ASR #32 = sign fill; ROR #32 = identity. */
    static const uint32_t prog[] = {
        0xE3E02000u, /* 0: MVN R2, #0        : 0xFFFFFFFF */
        0xE3A04020u, /* 1: MOV R4, #32       */
        0xE1B00412u, /* 2: MOVS R0, R2, LSL R4 -> 0, C = bit0 = 1 */
        0xE1B05432u, /* 3: MOVS R5, R2, LSR R4 -> 0, C = bit31 = 1 */
        0xE1B06452u, /* 4: MOVS R6, R2, ASR R4 -> 0xFFFFFFFF, C = 1 */
        0xE1A07472u, /* 5: MOV R7, R2, ROR R4  -> 0xFFFFFFFF (no S) */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 7);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 3);
    T_CHECK_EQ(g->cpu.r[0], 0u);
    T_CHECK(g->cpu.cpsr & GBA_C);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[5], 0u);
    T_CHECK(g->cpu.cpsr & GBA_C);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[6], 0xFFFFFFFFu);
    T_CHECK(g->cpu.cpsr & GBA_C);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[7], 0xFFFFFFFFu);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_rrx(void)
{
    /* RRX (ROR #0 immediate) with C=1 and 0x80000000:
     * result = (v >> 1) | C<<31 = 0xC0000000, carry = old bit0 = 0 */
    static const uint32_t prog[] = {
        0xE3A03101u, /* 0: MOV R3, #0x40000000 (imm 1 ror 2) */
        0xE0833003u, /* 1: ADD R3, R3, R3 -> 0x80000000 */
        0xE1500000u, /* 2: CMP R0, R0 -> C = 1 */
        0xE1B00063u, /* 3: MOVS R0, R3, ROR #0 -> RRX */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 5);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 3);
    T_CHECK(g->cpu.cpsr & GBA_C);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[0], 0xC0000000u);
    T_CHECK(!(g->cpu.cpsr & GBA_C)); /* carry = old bit0 = 0 */
    T_CHECK(g->cpu.cpsr & GBA_N);
    emu_core_mgbax()->destroy(&g->base);
}

/* --- loads / stores / swap ----------------------------------------------------- */

static void arm_ldr_str_byte_half(void)
{
    /* Literal-load the EWRAM pointer, store a word, then byte/half/word
     * reads with sign extension. LDR R1, [PC, #0x14] at 0x08000008 reads
     * the literal at 0x08000008 + 8 + 0x14 = 0x08000024 (word 9). */
    static const uint32_t prog[] = {
        0xE3A00078u, /* 0: MOV R0, #0x78 */
        0xE3800B01u, /* 1: ADD R0, R0, #0x400 -> 0x478 */
        0xE59F1018u, /* 2: LDR R1, [PC, #0x18] -> 0x08000008+8+0x18 = word 10 */
        0xE5810000u, /* 3: STR R0, [R1] */
        0xE5D13000u, /* 4: LDRB R3, [R1] */
        0xE5D14001u, /* 5: LDRB R4, [R1,#1] */
        0xE1D150B0u, /* 6: LDRH R5, [R1] */
        0xE1D160F0u, /* 7: LDRSH R6, [R1] */
        0xE5912002u, /* 8: LDR R2, [R1, #2] : unaligned word -> ror 16 */
        0xEAFFFFFEu, /* 9: B self */
        0x02000000u  /* 10: literal (EWRAM) */
    };
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    memcpy(rom, prog, sizeof prog);
    emu_result_t r = emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    T_CHECK_EQ(r, EMU_OK);
    run_n(g, 9);
    T_CHECK_EQ(g->cpu.r[1], 0x02000000u);
    T_CHECK_EQ(g->cpu.r[3], 0x78u);   /* LDRB low byte */
    T_CHECK_EQ(g->cpu.r[4], 0x04u);   /* LDRB byte 1 */
    T_CHECK_EQ(g->cpu.r[5], 0x0478u); /* LDRH */
    T_CHECK_EQ(g->cpu.r[6], 0x0478u); /* LDRSH (positive) */
    T_CHECK_EQ(g->cpu.r[2], 0x04780000u); /* unaligned LDR rotates right 16 */
    T_CHECK_EQ(gba_mem_read32(g, 0x02000000u), 0x00000478u);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_ldrsb_negative(void)
{
    /* LDRSB/LDRSH sign extension of negative values. EWRAM preloaded with
     * halfword 0x8989: byte0 = 0x89 (negative byte), halfword 0x8989
     * (negative halfword). */
    static const uint32_t prog[] = {
        0xE59F100Cu, /* 0: LDR R1, [PC, #12] -> 0x08000008+12 = word 5 */
        0xE1D140D0u, /* 1: LDRSB R4, [R1] -> 0xFFFFFF89 */
        0xE1D150F0u, /* 2: LDRSH R5, [R1] -> 0xFFFF8989 */
        0xE5D13001u, /* 3: LDRB R3, [R1,#1] -> 0x89 */
        0xEAFFFFFEu, /* 4: B self */
        0x02000000u  /* 5: literal */
    };
    struct gba *g = mk_cpu_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->mem.ewram[0] = 0x89;
    g->mem.ewram[1] = 0x89;
    run_n(g, 4);
    T_CHECK_EQ(g->cpu.r[4], 0xFFFFFF89u);
    T_CHECK_EQ(g->cpu.r[5], 0xFFFF8989u);
    T_CHECK_EQ(g->cpu.r[3], 0x89u);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_mul_mla(void)
{
    /* MUL R0, R1, R2 = 15; MLA R4, R1, R2, R3 = 22 */
    static const uint32_t prog[] = {
        0xE3A01003u, /* MOV R1, #3 */
        0xE3A02005u, /* MOV R2, #5 */
        0xE3A03007u, /* MOV R3, #7 */
        0xE0000291u, /* MUL R0, R1, R2   -> 15 */
        0xE0243291u, /* MLA R4, R1, R2, R3 -> 3*5+7 = 22 */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 5);
    T_CHECK_EQ(g->cpu.r[0], 15u);
    T_CHECK_EQ(g->cpu.r[4], 22u);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_mull_64bit(void)
{
    /* UMULL R4, R5, R1, R0: 0xFFFFFFFF^2 = 0xFFFFFFFE_00000001 */
    static const uint32_t prog[] = {
        0xE3E00000u, /* MVN R0, #0 */
        0xE1A01000u, /* MOV R1, R0 */
        0xE0854190u, /* UMULL R4, R5, R1, R0 */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 4);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 3);
    T_CHECK_EQ(g->cpu.r[4], 0x00000001u); /* RdLo */
    T_CHECK_EQ(g->cpu.r[5], 0xFFFFFFFEu); /* RdHi */
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_swp(void)
{
    /* SWP R2, R3, [R0] with R0 = 0x02000000, R3 = 0x77, memory preloaded
     * with 0xDEADBEEF */
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    static const uint32_t prog[] = {
        0xE3A00402u, /* MOV R0, #0x02 ror 8 -> 0x02000000 */
        0xE3A03077u, /* MOV R3, #0x77 */
        0xE1002093u, /* SWP R2, R3, [R0] */
        0xEAFFFFFEu
    };
    memcpy(rom, prog, sizeof prog);
    emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    g->mem.ewram[0] = 0xEF;
    g->mem.ewram[1] = 0xBE;
    g->mem.ewram[2] = 0xAD;
    g->mem.ewram[3] = 0xDE;
    run_n(g, 3);
    T_CHECK_EQ(g->cpu.r[2], 0xDEADBEEFu);
    T_CHECK_EQ(gba_mem_read32(g, 0x02000000u), 0x77u);
    emu_core_mgbax()->destroy(&g->base);
}

/* --- block transfers ------------------------------------------------------------ */

static void arm_ldm_stm_ia(void)
{
    /* STMIA R0!, {R4-R6} then LDMIA R0!, {R4-R6} through EWRAM */
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    static const uint32_t prog[] = {
        0xE3A040AAu, /* 0: MOV R4, #0xAA */
        0xE3A0500Bu, /* 1: MOV R5, #0x0B */
        0xE3A0600Cu, /* 2: MOV R6, #0x0C */
        0xE3A00402u, /* 3: MOV R0, #0x02000000 (imm 0x02 ror 8) */
        0xE8800070u, /* 4: STMIA R0, {R4-R6} (no writeback) */
        0xE3A050FFu, /* 5: MOV R5, #0xFF */
        0xE8B00070u, /* 6: LDMIA R0!, {R4-R6} (writeback) */
        0xEAFFFFFEu  /* 7: B self */
    };
    memcpy(rom, prog, sizeof prog);
    emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    run_n(g, 7);
    T_CHECK_EQ(g->cpu.r[4], 0xAAu);
    T_CHECK_EQ(g->cpu.r[5], 0x0Bu);
    T_CHECK_EQ(g->cpu.r[6], 0x0Cu);
    T_CHECK_EQ(g->cpu.r[0], 0x0200000Cu); /* LDM writeback only */
    T_CHECK_EQ(gba_mem_read32(g, 0x02000000u), 0xAAu);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_ldm_stm_db_da(void)
{
    /* STMDA R0, {R4-R6} with base 0x02000010: transfers at base-8, base-4,
     * base (ascending from base-(count-1)*4). LDMDB R0, {R4-R6} reloads
     * from the same addresses. 0x02000000 stays untouched. */
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    static const uint32_t prog[] = {
        0xE3A040AAu, /* 0: MOV R4, #0xAA */
        0xE3A0500Bu, /* 1: MOV R5, #0x0B */
        0xE3A0600Cu, /* 2: MOV R6, #0x0C */
        0xE3A00402u, /* 3: MOV R0, #0x02000000 */
        0xE2800010u, /* 4: ADD R0, R0, #0x10 -> 0x02000010 */
        0xE8000070u, /* 5: STMDA R0, {R4-R6} */
        0xE3A04000u, /* 6: MOV R4, #0 */
        0xE3A05000u, /* 7: MOV R5, #0 */
        0xE3A06000u, /* 8: MOV R6, #0 */
        0xE8100070u, /* 9: LDMDA R0, {R4-R6} (same addresses as STMDA) */
        0xEAFFFFFEu  /* 10: B self */
    };
    memcpy(rom, prog, sizeof prog);
    emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    run_n(g, 6);
    T_CHECK_EQ(gba_mem_read32(g, 0x02000008u), 0xAAu);
    T_CHECK_EQ(gba_mem_read32(g, 0x0200000Cu), 0x0Bu);
    T_CHECK_EQ(gba_mem_read32(g, 0x02000010u), 0x0Cu);
    T_CHECK_EQ(gba_mem_read32(g, 0x02000000u), 0x00000000u);
    run_n(g, 4);
    T_CHECK_EQ(g->cpu.r[4], 0xAAu);
    T_CHECK_EQ(g->cpu.r[5], 0x0Bu);
    T_CHECK_EQ(g->cpu.r[6], 0x0Cu);
    T_CHECK_EQ(g->cpu.r[0], 0x02000010u); /* no writeback */
    emu_core_mgbax()->destroy(&g->base);
}

/* --- misc ------------------------------------------------------------------------- */

static void arm_condition_codes(void)
{
    /* CMP 5 vs 3: C=1,N=0,Z=0,V=0 -> CS true, CC false, GT true, LT false */
    static const uint32_t prog[] = {
        0xE3A00005u, /* MOV R0, #5 */
        0xE3A01003u, /* MOV R1, #3 */
        0xE1500001u, /* CMP R0, R1 */
        0x33A02001u, /* MOVCC R2, #1 (skipped) */
        0x23A03001u, /* MOVCS R3, #1 */
        0xC3A04001u, /* MOVGT R4, #1 */
        0xB3A05001u, /* MOVLT R5, #1 (skipped) */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 8);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 7);
    T_CHECK_EQ(g->cpu.r[2], 0u);
    T_CHECK_EQ(g->cpu.r[3], 1u);
    T_CHECK_EQ(g->cpu.r[4], 1u);
    T_CHECK_EQ(g->cpu.r[5], 0u);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_bx_thumb_switch(void)
{
    /* LDR R0, [PC, #0] at 0x08000000 -> literal at 0x08000008 (word 2) */
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    static const uint16_t thumb[] = {
        0x2542u, /* MOV R5, #0x42 */
        0x4770u  /* BX LR */
    };
    memcpy(&rom[0x10], thumb, sizeof thumb);
    static const uint32_t prog[] = {
        0xE59F0000u, /* 0: LDR R0, [PC, #0] -> word 2 */
        0xE12FFF10u, /* 1: BX R0 -> thumb at 0x08000010 */
        0x08000011u  /* 2: literal: thumb entry | 1 */
    };
    memcpy(rom, prog, sizeof prog);
    emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    g->cpu.r[14] = 0x08000004u; /* LR for BX back */
    run_n(g, 1); /* LDR */
    T_CHECK_EQ(g->cpu.r[0], 0x08000011u);
    run_n(g, 1); /* BX R0 */
    T_CHECK(g->cpu.cpsr & GBA_T);
    T_CHECK_EQ(g->cpu.r[15], 0x08000010u);
    run_n(g, 2); /* thumb MOV + BX LR */
    T_CHECK_EQ(g->cpu.r[5], 0x42u);
    T_CHECK(!(g->cpu.cpsr & GBA_T));
    T_CHECK_EQ(g->cpu.r[15], 0x08000004u);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_mrs_msr(void)
{
    /* MSR CPSR_c, #0x13 (mode bits only; MRS R1, CPSR reads it back) */
    static const uint32_t prog[] = {
        0xE321F013u, /* MSR CPSR_c, #0x13 */
        0xE10F1000u, /* MRS R1, CPSR */
        0xEAFFFFFEu
    };
    struct gba *g = mk_cpu_core(prog, 3);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.mode, GBA_MODE_SVC);
    T_CHECK_EQ(g->cpu.r[1] & 0x1Fu, GBA_MODE_SVC);
    emu_core_mgbax()->destroy(&g->base);
}

static void arm_interrupt_dispatch(void)
{
    /* Full HLE IRQ round trip:
     * main: ADD R0, PC, #0 at 0x08000000; B self at 0x08000004.
     * handler (IWRAM 0x03000100): count [0x03000100]++, acknowledge IF
     * (write 1 to 0x04000202), BX LR (sentinel) -> BIOS return.
     * Spec contract: LR_irq = interrupted next_pc + 4; return restores
     * CPSR from SPSR_irq and resumes at the interrupted next_pc. */
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    static const uint32_t prog[] = {
        0xE28F0000u, /* 0: ADD R0, PC, #0 */
        0xEAFFFFFEu  /* 1: B self */
    };
    memcpy(rom, prog, sizeof prog);
    emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    static const uint32_t handler[] = {
        0xE3A04404u, /* 0x03000100: MOV R4, #0x04000000 (imm 4 ror 8) */
        0xE3844C02u, /* 0x03000104: ORR R4, R4, #0x200 */
        0xE3844002u, /* 0x03000108: ORR R4, R4, #2 -> 0x04000202 */
        0xE3A03001u, /* 0x0300010C: MOV R3, #1 */
        0xE1C430B0u, /* 0x03000110: STRH R3, [R4]  ; ack IF bit 0 */
        0xE59F100Cu, /* 0x03000114: LDR R1, [PC, #12] -> 0x03000128 */
        0xE5912000u, /* 0x03000118: LDR R2, [R1] */
        0xE2822001u, /* 0x0300011C: ADD R2, R2, #1 */
        0xE5812000u, /* 0x03000120: STR R2, [R1] */
        0xE12FFF1Eu, /* 0x03000124: BX LR (BIOS sentinel) */
        0x03000140u  /* 0x03000128: counter literal */
    };
    memcpy(&g->mem.iwram[0x100], handler, sizeof handler);
    gba_mem_write32(g, 0x03007FFCu, 0x03000100u); /* handler pointer */
    g->cpu.r[12] = 0x1234u; /* must survive the round trip */
    g->mem.ie = 1;          /* vblank IRQ enabled */
    g->mem.if_reg = 1;      /* pending */
    g->cpu.cpsr &= (uint32_t)~GBA_I; /* IRQs unmasked */
    run_n(g, 1); /* dispatch: enter IRQ mode, push frame, jump to handler */
    T_CHECK_EQ(g->cpu.mode, GBA_MODE_IRQ);
    T_CHECK(g->cpu.cpsr & GBA_I);
    T_CHECK_EQ(gba_mem_read32(g, 0x03007F88u), 0u);       /* pushed r0 */
    T_CHECK_EQ(gba_mem_read32(g, 0x03007F98u), 0x1234u);  /* pushed r12 */
    T_CHECK_EQ(gba_mem_read32(g, 0x03007F9Cu), 0x08000004u); /* LR = next+4 */
    run_n(g, 10); /* handler body (10 instructions, ends with BX LR) */
    T_CHECK(g->cpu.bios_dispatch);
    T_CHECK_EQ(g->cpu.r[15], 0u); /* at the BIOS return sentinel */
    run_n(g, 1); /* BIOS return: pop frame, SUBS pc, lr, #4 equivalent */
    T_CHECK_EQ(gba_mem_read32(g, 0x03000140u), 1u); /* counter incremented */
    T_CHECK_EQ(g->mem.if_reg & 1u, 0u);             /* acknowledged */
    T_CHECK_EQ(g->cpu.r[12], 0x1234u);              /* r12 preserved */
    T_CHECK_EQ(g->cpu.r[15], 0x08000000u);          /* resumed at next_pc */
    T_CHECK_EQ(g->cpu.mode, GBA_MODE_SVC);          /* mode restored */
    T_CHECK(!(g->cpu.cpsr & GBA_I));                /* CPSR from SPSR */
    T_CHECK_EQ(g->cpu.bank_r13[2], 0x03007FA0u);    /* IRQ stack unwound */
    run_n(g, 2); /* ADD R0 executes, then B self */
    T_CHECK_EQ(g->cpu.r[0], 0x08000008u);
    T_CHECK_EQ(g->cpu.r[15], 0x08000004u);
    emu_core_mgbax()->destroy(&g->base);
}

T_SUITE_BEGIN(gba_arm)
{ "mov_imm_rotated", arm_mov_imm_rotated },
{ "movs_imm_carry", arm_movs_imm_carry },
{ "adds_overflow", arm_adds_overflow },
{ "adds_carry_out", arm_adds_carry_out },
{ "adc_carry_in", arm_adc_carry_in },
{ "sbc_borrow", arm_sbc_borrow },
{ "sbc_borrow_edge", arm_sbc_borrow_edge },
{ "cmp_vs_cmn", arm_cmp_vs_cmn },
{ "pc_read_operand2", arm_pc_read_operand2 },
{ "ldr_pc_base", arm_ldr_pc_base },
{ "branch_link", arm_branch_link },
{ "shifts_carry_32", arm_shifts_carry },
{ "rrx", arm_rrx },
{ "ldr_str_byte_half", arm_ldr_str_byte_half },
{ "ldrsb_negative", arm_ldrsb_negative },
{ "mul_mla", arm_mul_mla },
{ "umull_64bit", arm_mull_64bit },
{ "swp", arm_swp },
{ "ldm_stm_ia", arm_ldm_stm_ia },
{ "ldm_stm_db_da", arm_ldm_stm_db_da },
{ "condition_codes", arm_condition_codes },
{ "bx_thumb_switch", arm_bx_thumb_switch },
{ "mrs_msr", arm_mrs_msr },
{ "interrupt_dispatch", arm_interrupt_dispatch },
T_SUITE_END

T_SUITE_REG(gba_arm)
