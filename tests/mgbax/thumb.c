/*
 * mgbax Thumb instruction tests. Every opcode is a raw 16-bit halfword
 * hand-assembled from the ARM ARM (ARMv4T); expected values derive from
 * the specification. Thumb PC rules: PC reads instruction address + 4
 * (bit 1 forced to 0 for literal loads / ADR); branch targets are
 * instruction address + 4 + offset.
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

static struct gba *mk_thumb_core(const uint16_t *code, size_t halfwords)
{
    struct gba *g = mk_core();
    if (g == NULL)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    memcpy(rom, code, halfwords * 2u);
    emu_result_t r = emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_mgbax()->destroy(&g->base);
        return NULL;
    }
    /* enter Thumb mode at 0x08000000 */
    g->cpu.cpsr |= GBA_T;
    return g;
}

static void run_n(struct gba *g, int count)
{
    for (int i = 0; i < count; i++)
        gba_cpu_step(g);
}

/* --- format 1/2/3: shifts, add/sub, immediates -------------------------------- */

static void thumb_mov_imm(void)
{
    static const uint16_t prog[] = {
        0x2042u, /* MOV R0, #0x42 */
        0xE7FEu  /* B self */
    };
    struct gba *g = mk_thumb_core(prog, 2);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[0], 0x42u);
    T_CHECK(!(g->cpu.cpsr & GBA_N));
    T_CHECK(!(g->cpu.cpsr & GBA_Z));
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_lsl_lsr_asr_imm(void)
{
    static const uint16_t prog[] = {
        0x2080u, /* 0: MOV R0, #0x80 */
        0x0600u, /* 1: LSL R0, R0, #24 -> 0x80000000 */
        0x08C1u, /* 2: LSR R1, R0, #3  -> 0x10000000 */
        0x10C2u, /* 3: ASR R2, R0, #3  -> 0xF0000000 (sign fill) */
        0xE7FEu
    };
    struct gba *g = mk_thumb_core(prog, 5);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[0], 0x80000000u);
    T_CHECK(g->cpu.cpsr & GBA_N);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[1], 0x10000000u);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[2], 0xF0000000u);
    T_CHECK(g->cpu.cpsr & GBA_N);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_lsl_carry(void)
{
    /* LSL by 31 of 3: result 0x80000000, carry = bit(32-31) of source = 1 */
    static const uint16_t prog[] = {
        0x2003u, /* MOV R0, #3 */
        0x07C0u, /* LSL R0, R0, #31 */
        0xE7FEu
    };
    struct gba *g = mk_thumb_core(prog, 3);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[0], 0x80000000u);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(g->cpu.cpsr & GBA_N);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_add_sub_reg(void)
{
    static const uint16_t prog[] = {
        0x2080u, /* 0: MOV R0, #0x80 */
        0x0600u, /* 1: LSL R0, R0, #24 -> 0x80000000 */
        0x2180u, /* 2: MOV R1, #0x80 */
        0x0609u, /* 3: LSL R1, R1, #24 -> 0x80000000 */
        0x1842u, /* 4: ADD R2, R0, R1  -> 0, C=1, Z=1, V=1 */
        0x1A43u, /* 5: SUB R3, R0, R1  -> 0, C=1 (no borrow), Z=1, V=0 */
        0xE7FEu
    };
    struct gba *g = mk_thumb_core(prog, 7);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 5);
    T_CHECK_EQ(g->cpu.r[2], 0u);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(g->cpu.cpsr & GBA_Z);
    T_CHECK(g->cpu.cpsr & GBA_V);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[3], 0u);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(g->cpu.cpsr & GBA_Z);
    T_CHECK(!(g->cpu.cpsr & GBA_V));
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_sub_imm_carry(void)
{
    static const uint16_t prog[] = {
        0x2001u, /* MOV R0, #1 */
        0x3802u, /* SUB R0, R0, #2 -> 0xFFFFFFFF, C=0 (borrow), N=1 */
        0xE7FEu
    };
    struct gba *g = mk_thumb_core(prog, 3);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[0], 0xFFFFFFFFu);
    T_CHECK(!(g->cpu.cpsr & GBA_C));
    T_CHECK(g->cpu.cpsr & GBA_N);
    emu_core_mgbax()->destroy(&g->base);
}

/* --- ALU operations (format 4) ------------------------------------------------- */

static void thumb_alu_ops(void)
{
    static const uint16_t prog[] = {
        0x200Fu, /* 0: MOV R0, #0x0F */
        0x2136u, /* 1: MOV R1, #0x36 */
        0x4008u, /* 2: AND R0, R1    -> 0x06 */
        0x200Fu, /* 3: MOV R0, #0x0F */
        0x4388u, /* 4: BIC R0, R1    -> 0x09 */
        0x2000u, /* 5: MOV R0, #0    */
        0x4248u, /* 6: NEG R0, R1    -> 0xFFFFFFCA, C=0, N=1 */
        0x2007u, /* 7: MOV R0, #7    */
        0x4348u, /* 8: MUL R0, R1    -> 0x17A */
        0xE7FEu
    };
    struct gba *g = mk_thumb_core(prog, 10);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 3);
    T_CHECK_EQ(g->cpu.r[0], 0x06u);
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[0], 0x09u);
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[0], 0xFFFFFFCAu);
    T_CHECK(g->cpu.cpsr & GBA_N);
    T_CHECK(!(g->cpu.cpsr & GBA_C)); /* 0 - 0x36 borrows */
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[0], 0x17Au);
    emu_core_mgbax()->destroy(&g->base);
}

/* --- format 5: hi-register ops + BX --------------------------------------------- */

static void thumb_hi_reg_ops(void)
{
    static const uint16_t prog[] = {
        0x2020u, /* 0: MOV R0, #0x20 */
        0x4680u, /* 1: MOV R8, R0    -> 0x20 */
        0x4480u, /* 2: ADD R8, R0    -> 0x40 */
        0x2101u, /* 3: MOV R1, #1    */
        0x4588u, /* 4: CMP R8, R1    -> 0x40 > 1: C=1, Z=0 */
        0xE7FEu
    };
    struct gba *g = mk_thumb_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[8], 0x20u);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[8], 0x40u);
    run_n(g, 2);
    T_CHECK(g->cpu.cpsr & GBA_C);
    T_CHECK(!(g->cpu.cpsr & GBA_Z));
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_bx_roundtrip(void)
{
    /* Thumb -> ARM -> Thumb interworking via BX.
     * 0x08000000 (thumb): MOV R5,#0x11 ; BX LR
     * 0x08000010 (arm):   LDR R0,[PC,#4] ; BX R0 ; B self ; literal
     * literal at 0x0800001C = 0x08000001 (thumb | 1) */
    static const uint16_t thumb2[] = {
        0x2511u, /* 0x08000000: MOV R5, #0x11 */
        0x4770u  /* 0x08000002: BX LR */
    };
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    memcpy(rom, thumb2, sizeof thumb2);
    memcpy(&rom[0x10], (const uint32_t[]){ 0xE59F0004u, 0xE12FFF10u,
                                           0xEAFFFFFEu, 0x08000001u },
           4 * 4u);
    emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    g->cpu.cpsr |= GBA_T;
    g->cpu.r[14] = 0x08000010u; /* LR: ARM return address */
    run_n(g, 1); /* thumb MOV R5 */
    run_n(g, 1); /* thumb BX LR -> ARM, T=0 */
    T_CHECK(!(g->cpu.cpsr & GBA_T));
    T_CHECK_EQ(g->cpu.r[15], 0x08000010u);
    T_CHECK_EQ(g->cpu.r[5], 0x11u);
    run_n(g, 2); /* ARM LDR + BX R0 -> Thumb, T=1 */
    T_CHECK(g->cpu.cpsr & GBA_T);
    T_CHECK_EQ(g->cpu.r[15], 0x08000000u);
    T_CHECK_EQ(g->cpu.r[0], 0x08000001u);
    emu_core_mgbax()->destroy(&g->base);
}

/* --- loads / stores -------------------------------------------------------------- */

static void thumb_ldr_literal(void)
{
    /* LDR R1, [PC, #4] at 0x08000000: base = align4(0x08000004) + 4 =
     * 0x08000008 (halfwords 4 and 5) */
    static const uint16_t prog[] = {
        0x4901u, /* 0: LDR R1, [PC, #4] */
        0xE7FEu, /* 1: B self */
        0x0000u, /* 2: pad */
        0x0000u, /* 3: pad */
        0xBEEFu, /* 4: literal low */
        0xDEADu  /* 5: literal high */
    };
    struct gba *g = mk_thumb_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[1], 0xDEADBEEFu);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_load_store_offsets(void)
{
    /* Pointer via literal; word/byte stores and loads through offsets.
     * literal at 0x08000024 (PC+32 from index 0). */
    static const uint16_t prog[] = {
        0x4A08u, /* 0: LDR R2, [PC, #32] */
        0x2078u, /* 1: MOV R0, #0x78 */
        0x2134u, /* 2: MOV R1, #0x34 */
        0x6010u, /* 3: STR R0, [R2] */
        0x61D1u, /* 4: STR R1, [R2, #28] */
        0x6853u, /* 5: LDR R3, [R2, #4] */
        0x7C93u, /* 6: LDRB R3, [R2, #18] */
        0x6994u, /* 7: LDR R4, [R2, #24] */
        0x69D5u, /* 8: LDR R5, [R2, #28] */
        0x6816u, /* 9: LDR R6, [R2] */
        0xE7FEu, /* 10: B self */
        [18] = 0x0000u, /* 0x24: literal low half */
        [19] = 0x0200u  /* 0x26: literal high half */
    };
    struct gba *g = mk_thumb_core(prog, 20);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[2], 0x02000000u);
    run_n(g, 9);
    T_CHECK_EQ(gba_mem_read32(g, 0x02000000u), 0x78u);
    T_CHECK_EQ(gba_mem_read32(g, 0x0200001Cu), 0x34u);
    T_CHECK_EQ(g->cpu.r[3], 0u); /* untouched memory reads 0 */
    T_CHECK_EQ(g->cpu.r[4], 0u);
    T_CHECK_EQ(g->cpu.r[5], 0x34u);
    T_CHECK_EQ(g->cpu.r[6], 0x78u);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_strh_ldrh_ldrsh(void)
{
    /* Halfword store/load + LDRSB/LDRSH through register offsets. */
    static const uint16_t prog[] = {
        0x4A08u, /* 0: LDR R2, [PC, #32] */
        0x2089u, /* 1: MOV R0, #0x89 */
        0x8090u, /* 2: STRH R0, [R2, #4] */
        0x88D3u, /* 3: LDRH R3, [R2, #6] -> 0 */
        0x8894u, /* 4: LDRH R4, [R2, #4] -> 0x0089 */
        0x56D5u, /* 5: LDRSB R5, [R2, R3] -> byte at +0 = 0x78 */
        0x5ED6u, /* 6: LDRSH R6, [R2, R3] -> halfword at +0 = 0x78 */
        0xE7FEu,
        [18] = 0x0000u,
        [19] = 0x0200u
    };
    struct gba *g = mk_thumb_core(prog, 20);
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->mem.ewram[0] = 0x78; /* preload halfword 0x0078 at EWRAM +0 */
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[2], 0x02000000u);
    run_n(g, 6);
    T_CHECK_EQ(g->cpu.r[3], 0u);
    T_CHECK_EQ(g->cpu.r[4], 0x0089u);
    T_CHECK_EQ(g->cpu.r[5], 0x78u); /* positive byte: no sign extension */
    T_CHECK_EQ(g->cpu.r[6], 0x78u);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_ldrsh_sign_extend(void)
{
    /* LDRSH of a negative halfword 0x8977 -> 0xFFFF8977 */
    static const uint16_t prog[] = {
        0x4A05u, /* 0: LDR R2, [PC, #20] -> literal at 0x08000018 */
        0x2189u, /* 1: MOV R1, #0x89 */
        0x0209u, /* 2: LSL R1, R1, #8 -> 0x8900 */
        0x3177u, /* 3: ADD R1, #0x77 -> 0x8977 */
        0x8091u, /* 4: STRH R1, [R2, #4] */
        0x8894u, /* 5: LDRH R4, [R2, #4] -> 0x8977 (zero-extended) */
        0x2000u, /* 6: MOV R0, #0 */
        0x5E15u, /* 7: LDRSH R5, [R2, R0] -> preloaded 0x8977 -> 0xFFFF8977 */
        0xE7FEu,
        [12] = 0x0000u, /* 0x18: literal low */
        [13] = 0x0200u  /* 0x1A: literal high */
    };
    struct gba *g = mk_thumb_core(prog, 14);
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->mem.ewram[0] = 0x77;
    g->mem.ewram[1] = 0x89;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[2], 0x02000000u);
    run_n(g, 7);
    T_CHECK_EQ(g->cpu.r[4], 0x8977u);
    T_CHECK_EQ(g->cpu.r[5], 0xFFFF8977u);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_push_pop(void)
{
    /* PUSH {R0, LR}; clobber; POP {R1, PC} -> PC restored, T kept (v4T) */
    static const uint16_t prog[] = {
        0x203Bu, /* 0: MOV R0, #0x3B */
        0xB501u, /* 1: PUSH {R0, LR} */
        0x2000u, /* 2: MOV R0, #0 */
        0xBD02u, /* 3: POP {R1, PC} */
        0x2700u, /* 4: MOV R7, #0 (PC lands here) */
        0xE7FEu  /* 5: B self */
    };
    struct gba *g = mk_thumb_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->cpu.r[14] = 0x08000008u; /* LR: return target (word 4) */
    run_n(g, 2); /* MOV R0, PUSH {R0,LR} */
    T_CHECK_EQ(g->cpu.r[13], 0x03007FD8u); /* svc sp - 8 */
    run_n(g, 2); /* clobber, POP {R1, PC} */
    T_CHECK_EQ(g->cpu.r[1], 0x3Bu);
    T_CHECK_EQ(g->cpu.r[15], 0x08000008u);
    T_CHECK(g->cpu.cpsr & GBA_T); /* ARMv4T: POP {pc} keeps T */
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_sp_adjust_ldmia(void)
{
    static const uint16_t prog[] = {
        0xB008u, /* 0: ADD SP, #8 */
        0xB088u, /* 1: SUB SP, #8 */
        0x2101u, /* 2: MOV R1, #1 */
        0x2202u, /* 3: MOV R2, #2 */
        0x2303u, /* 4: MOV R3, #3 */
        0xA80Au, /* 5: ADD R0, SP, #40 -> 0x03008008 (IWRAM alias) */
        0xC00Eu, /* 6: STMIA R0!, {R1-R3} */
        0xA80Au, /* 7: ADD R0, SP, #40 (reload base) */
        0x2100u, /* 8: MOV R1, #0 */
        0x2200u, /* 9: MOV R2, #0 */
        0x2300u, /* 10: MOV R3, #0 */
        0xC80Eu, /* 11: LDMIA R0!, {R1-R3} */
        0xE7FEu  /* 12: B self */
    };
    struct gba *g = mk_thumb_core(prog, 13);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK_EQ(g->cpu.r[13], 0x03007FE0u); /* +8 then -8 */
    run_n(g, 5);
    T_CHECK_EQ(g->cpu.r[0], 0x03008014u); /* advanced by 12 */
    T_CHECK_EQ(gba_mem_read32(g, 0x03008008u), 1u);
    T_CHECK_EQ(gba_mem_read32(g, 0x0300800Cu), 2u);
    T_CHECK_EQ(gba_mem_read32(g, 0x03008010u), 3u);
    run_n(g, 6);
    T_CHECK_EQ(g->cpu.r[1], 1u);
    T_CHECK_EQ(g->cpu.r[2], 2u);
    T_CHECK_EQ(g->cpu.r[3], 3u);
    T_CHECK_EQ(g->cpu.r[0], 0x03008014u); /* base reloaded + 3 words */
    emu_core_mgbax()->destroy(&g->base);
}

/* --- branches --------------------------------------------------------------------- */

static void thumb_conditional_branch(void)
{
    /* Z=1: BEQ (offset 0) jumps to PC(=instr+4), skipping one halfword */
    static const uint16_t prog[] = {
        0x2005u, /* 0: MOV R0, #5 */
        0x2805u, /* 1: CMP R0, #5 -> Z=1 */
        0xD000u, /* 2: BEQ -> 0x08000008 (skips word 3) */
        0x2101u, /* 3: MOV R1, #1 (skipped) */
        0x2202u, /* 4: MOV R2, #2 */
        0xE7FEu  /* 5: B self */
    };
    struct gba *g = mk_thumb_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2);
    T_CHECK(g->cpu.cpsr & GBA_Z);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[15], 0x08000008u);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[2], 2u);
    T_CHECK_EQ(g->cpu.r[1], 0u);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_unconditional_branch(void)
{
    static const uint16_t prog[] = {
        0xE000u, /* 0: B -> 0x08000004 (skips word 1) */
        0x2001u, /* 1: MOV R0, #1 (skipped) */
        0x2102u, /* 2: MOV R1, #2 */
        0xE7FEu  /* 3: B self */
    };
    struct gba *g = mk_thumb_core(prog, 4);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[15], 0x08000004u);
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[1], 2u);
    T_CHECK_EQ(g->cpu.r[0], 0u);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_bl(void)
{
    /* BL at 0x08000000: LR = 0x08000005, target 0x0800000A */
    static const uint16_t prog[] = {
        0xF000u, /* 0: BL first half (high offset 0) */
        0xF802u, /* 1: BL second half (low offset 2) -> 0x08000008 */
        0x2001u, /* 2: MOV R0, #1 (return lands here) */
        0xE7FEu, /* 3: B self */
        0x2599u, /* 4: MOV R5, #0x99 (call target) */
        0x4770u  /* 5: BX LR -> 0x08000004, stays Thumb */
    };
    struct gba *g = mk_thumb_core(prog, 6);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 2); /* BL pair */
    T_CHECK_EQ(g->cpu.r[15], 0x08000008u);
    T_CHECK_EQ(g->cpu.r[14], 0x08000005u);
    run_n(g, 2); /* MOV R5, BX LR */
    T_CHECK_EQ(g->cpu.r[5], 0x99u);
    T_CHECK(g->cpu.cpsr & GBA_T); /* LR bit0 = 1: stays Thumb */
    T_CHECK_EQ(g->cpu.r[15], 0x08000004u);
    run_n(g, 1); /* MOV R0, #1 (index 2) */
    T_CHECK_EQ(g->cpu.r[0], 1u);
    emu_core_mgbax()->destroy(&g->base);
}

static void thumb_adr_sp_rel(void)
{
    static const uint16_t prog[] = {
        0xA002u, /* 0: ADR R0, +8 -> align4(0x08000004)+8 = 0x0800000C */
        0x9102u, /* 1: STR R1, [SP, #8] */
        0x2115u, /* 2: MOV R1, #0x15 */
        0x9101u, /* 3: STR R1, [SP, #4] */
        0x9802u, /* 4: LDR R0, [SP, #8] -> 0 (SP area untouched) */
        0x9901u, /* 5: LDR R1, [SP, #4] -> 0x15 */
        0xE7FEu
    };
    struct gba *g = mk_thumb_core(prog, 7);
    T_CHECK(g != NULL);
    if (!g)
        return;
    run_n(g, 1);
    T_CHECK_EQ(g->cpu.r[0], 0x0800000Cu);
    run_n(g, 5);
    T_CHECK_EQ(g->cpu.r[0], 0u);
    T_CHECK_EQ(g->cpu.r[1], 0x15u);
    emu_core_mgbax()->destroy(&g->base);
}

T_SUITE_BEGIN(gba_thumb)
{ "mov_imm", thumb_mov_imm },
{ "lsl_lsr_asr_imm", thumb_lsl_lsr_asr_imm },
{ "lsl_carry", thumb_lsl_carry },
{ "add_sub_reg", thumb_add_sub_reg },
{ "sub_imm_carry", thumb_sub_imm_carry },
{ "alu_ops", thumb_alu_ops },
{ "hi_reg_ops", thumb_hi_reg_ops },
{ "bx_roundtrip", thumb_bx_roundtrip },
{ "ldr_literal", thumb_ldr_literal },
{ "load_store_offsets", thumb_load_store_offsets },
{ "strh_ldrh_ldrsh", thumb_strh_ldrh_ldrsh },
{ "ldrsh_sign_extend", thumb_ldrsh_sign_extend },
{ "push_pop", thumb_push_pop },
{ "sp_adjust_ldmia", thumb_sp_adjust_ldmia },
{ "conditional_branch", thumb_conditional_branch },
{ "unconditional_branch", thumb_unconditional_branch },
{ "bl", thumb_bl },
{ "adr_sp_rel", thumb_adr_sp_rel },
T_SUITE_END

T_SUITE_REG(gba_thumb)
