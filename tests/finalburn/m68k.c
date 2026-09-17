/*
 * finalburn 68000 instruction tests. Encodings are hand-assembled from the
 * 68000 Programmer's Reference Manual; expected values are derived from the
 * architecture spec, never from emulator helpers.
 */
#include "tests.h"
#include "fbh.h"
#include "finalburn/fb_md.h"
#include "emu/emu.h"

#include <stdlib.h>
#include <string.h>

/* Boot helper: rom with code at 0x400, PC=0x400, run `cycles` 68K cycles. */
static struct fb_md *run_code(const uint16_t *code, size_t words, int32_t cycles)
{
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    if (rom == NULL)
        return NULL;
    for (size_t i = 0; i < words; i++)
        fb_rom_w16(rom, 0x400 + i * 2, code[i]);
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (md == NULL)
        return NULL;
    fb_m68k_run(md, cycles);
    return md;
}

/* End marker: BRA.S to self (0x60FE). */
#define BRA_SELF 0x60FEu

static void test_moveq(void)
{
    uint16_t code[] = { 0x7012, BRA_SELF }; /* MOVEQ #$12,D0 */
    struct fb_md *md = run_code(code, 2, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0], 0x12);
    T_CHECK_EQ_U(md->m68k.sr & (FB_SR_N | FB_SR_Z | FB_SR_V | FB_SR_C), 0);
    emu_core_destroy(&md->base);
}

static void test_moveq_zero(void)
{
    uint16_t code[] = { 0x7000, BRA_SELF }; /* MOVEQ #0,D0 */
    struct fb_md *md = run_code(code, 2, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0], 0);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_Z, FB_SR_Z);
    emu_core_destroy(&md->base);
}

static void test_addi_carry(void)
{
    /* MOVEQ #1,D0 ; ADDI.B #$FF,D0 -> 0x100: byte result 0, C=1 X=1 */
    uint16_t code[] = { 0x7001, 0x0600, 0x00FF, BRA_SELF };
    struct fb_md *md = run_code(code, 4, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0] & 0xFFu, 0x00);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_C, FB_SR_C);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_X, FB_SR_X);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_Z, FB_SR_Z);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_V, 0);
    emu_core_destroy(&md->base);
}

static void test_move_w_reg(void)
{
    /* MOVE.L #$1234ABCD,D1 ; MOVE.W D1,D0 -> D0 = 0xABCD (zero-extended) */
    uint16_t code[] = { 0x223C, 0x1234, 0xABCD, 0x3001, BRA_SELF };
    struct fb_md *md = run_code(code, 5, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0], 0xABCD);
    emu_core_destroy(&md->base);
}

static void test_lea_abs_w(void)
{
    /* LEA $1234.W,A0 */
    uint16_t code[] = { 0x41F8, 0x1234, BRA_SELF };
    struct fb_md *md = run_code(code, 3, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.a[0], 0x1234);
    emu_core_destroy(&md->base);
}

static void test_movea_sign_extend(void)
{
    /* MOVE.L #$FFFF8000,D2 ; MOVEA.W D2,A0 -> A0 = 0xFFFF8000 */
    uint16_t code[] = { 0x243C, 0xFFFF, 0x8000, 0x3042, BRA_SELF };
    struct fb_md *md = run_code(code, 5, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.a[0], 0xFFFF8000u);
    emu_core_destroy(&md->base);
}

static void test_jsr_rts(void)
{
    /* JSR $500 ; at $500: RTS ; then BRA.S self */
    uint16_t code[] = { 0x4EB8, 0x0500, BRA_SELF };
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    fb_rom_w16(rom, 0x400, code[0]);
    fb_rom_w16(rom, 0x402, code[1]);
    fb_rom_w16(rom, 0x404, code[2]);
    fb_rom_w16(rom, 0x500, 0x4E75); /* RTS */
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    uint32_t sp0 = md->m68k.a[7];
    fb_m68k_run(md, 200);
    T_CHECK_EQ_U(md->m68k.pc & 0xFFFFFFu, 0x404); /* back after JSR */
    T_CHECK_EQ_U(md->m68k.a[7], sp0);             /* stack balanced */
    emu_core_destroy(&md->base);
}

static void test_dbra_loop(void)
{
    /*
     * $400: MOVE.W #3,D0        303C 0003
     * $404: DBRA D0,$404        51C8 FFFC  (loop while D0 != -1)
     * $408: BRA.S self          60FE
     * DBRA decrements 3->2->1->0->-1, branching four times.
     */
    uint16_t code[] = { 0x303C, 0x0003, 0x51C8, 0xFFFC, BRA_SELF };
    struct fb_md *md = run_code(code, 5, 400);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0] & 0xFFFFu, 0xFFFFu);
    emu_core_destroy(&md->base);
}

static void test_addq_subq(void)
{
    /* MOVEQ #8,D0 ; ADDQ.W #1,D0 ; SUBQ.B #2,D0 */
    uint16_t code[] = { 0x7008, 0x5240, 0x5500, BRA_SELF };
    struct fb_md *md = run_code(code, 4, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0] & 0xFFFFu, 0x0007);
    emu_core_destroy(&md->base);
}

static void test_mulu_divu(void)
{
    /* MOVEQ #100,D0 ; MULU.W #10,D0 ; DIVU.W #3,D0
     * 100*10 = 1000; 1000/3 = 333 rem 1 -> D0 = (1<<16)|333 = 0x0001_014D */
    uint16_t code[] = { 0x7064, 0xC0FC, 0x000A, 0x80FC, 0x0003, BRA_SELF };
    struct fb_md *md = run_code(code, 6, 300);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0], (1u << 16) | 333u);
    emu_core_destroy(&md->base);
}

static void test_divu_overflow(void)
{
    /* MOVE.L #$000A0000,D0 ; DIVU.W #3,D0 -> quotient 0x55555 > 0xFFFF: V=1,
     * operand unchanged. */
    uint16_t code[] = { 0x203C, 0x000A, 0x0000, 0x80FC, 0x0003, BRA_SELF };
    struct fb_md *md = run_code(code, 6, 300);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0], 0x000A0000u);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_V, FB_SR_V);
    emu_core_destroy(&md->base);
}

static void test_lsl_flags(void)
{
    /* MOVE.W #$8000,D0 ; LSL.W #1,D0 -> result 0, X=C=1, Z=1 */
    uint16_t code[] = { 0x303C, 0x8000, 0xE348, BRA_SELF };
    struct fb_md *md = run_code(code, 4, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0] & 0xFFFFu, 0);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_C, FB_SR_C);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_X, FB_SR_X);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_Z, FB_SR_Z);
    emu_core_destroy(&md->base);
}

static void test_neg(void)
{
    /* MOVE.B #5,D0 ; NEG.B D0 -> 0xFB, C=1 X=1, N=1 */
    uint16_t code[] = { 0x7005, 0x4400, BRA_SELF };
    struct fb_md *md = run_code(code, 3, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0] & 0xFFu, 0xFB);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_C, FB_SR_C);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_N, FB_SR_N);
    emu_core_destroy(&md->base);
}

static void test_abcd(void)
{
    /* MOVEQ #$54,D0 ; MOVEQ #$27,D1 ; ABCD.B D1,D0 -> 0x81, C=0 */
    uint16_t code[] = { 0x7054, 0x7227, 0xC101, BRA_SELF };
    struct fb_md *md = run_code(code, 4, 100);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0] & 0xFFu, 0x81);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_C, 0);
    emu_core_destroy(&md->base);
}

static void test_movem(void)
{
    /* LEA $FF0000,A0 ; MOVEM.L D0-D1,(A0) ; CLR.L D0 ; MOVEM.L (A0),D0-D2
     * -> D0 = 0xAAAAAAA7 again */
    uint16_t code[] = {
        0x203C, 0xAAAA, 0xAAA7,          /* MOVE.L #$AAAAAAA7,D0 */
        0x41F9, 0x00FF, 0x0000,          /* LEA $FF0000.L,A0 */
        0x48D0, 0x0003,                  /* MOVEM.L D0-D1,(A0) */
        0x4280,                          /* CLR.L D0 */
        0x4CD0, 0x0003,                  /* MOVEM.L (A0),D0-D1 */
        BRA_SELF
    };
    struct fb_md *md = run_code(code, 11, 300);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[0], 0xAAAAAAA7u);
    emu_core_destroy(&md->base);
}

static void test_cmp_bcc(void)
{
    /* MOVEQ #5,D0 ; CMP.W #$10,D0 ; BLE taken? 5 <= 16 -> BCS? Use BLT: N!=V
     * after CMP: 5-16 = -11 <0: N=1 V=0 -> BLT taken.
     * $406: BLT $40A (0x6D02) ; NOP; BRA.S err ; $40A: BRA.S self
     * Simpler: branch to self directly. */
    uint16_t code[] = {
        0x7005,        /* MOVEQ #5,D0 */
        0xB07C, 0x0010,/* CMP.W #$10,D0 */
        0x6D02,        /* BLT +2 -> skip the BRA */
        0x60FE,        /* (not taken path) BRA.S self */
        0x60FE,        /* taken path: BRA.S self */
    };
    struct fb_md *md = run_code(code, 6, 200);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.pc & 0xFFFFFFu, 0x40A);
    emu_core_destroy(&md->base);
}

static void test_predecrement_postincrement(void)
{
    /* LEA $FF0100,A0 ; MOVE.L #$11223344,(A0)+ ; MOVE.L -(A0),D1 */
    uint16_t code[] = {
        0x41F9, 0x00FF, 0x0100, /* LEA $FF0100.L,A0 */
        0x20FC, 0x1122, 0x3344, /* MOVE.L #$11223344,(A0)+ */
        0x2220,                 /* MOVE.L -(A0),D1 */
        BRA_SELF
    };
    struct fb_md *md = run_code(code, 8, 200);
    if (!md) { T_FAIL("boot failed"); return; }
    T_CHECK_EQ_U(md->m68k.d[1], 0x11223344u);
    T_CHECK_EQ_U(md->m68k.a[0], 0xFF0100u);
    emu_core_destroy(&md->base);
}

static void test_trap_exception(void)
{
    /* TRAP #1 (0x4E41): vector 33 at $84. Put an RTE at the handler. */
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    fb_rom_w16(rom, 0x400, 0x4E41); /* TRAP #1 */
    fb_rom_w16(rom, 0x402, 0x60FE); /* BRA.S self */
    fb_rom_w16(rom, 0x404, 0x60FE); /* handler: BRA.S self */
    fb_rom_w32(rom, 0x84, 0x00000404); /* vector 33 -> $404 */
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    fb_m68k_run(md, 200);
    T_CHECK_EQ_U(md->m68k.pc & 0xFFFFFFu, 0x404);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_S, FB_SR_S); /* supervisor */
    emu_core_destroy(&md->base);
}

static void test_line_a_exception(void)
{
    /* 0xA000 triggers the line-A exception (vector 10 at $28). */
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    fb_rom_w16(rom, 0x400, 0xA000);
    fb_rom_w16(rom, 0x402, 0x60FE);
    fb_rom_w16(rom, 0x404, 0x60FE); /* handler: BRA.S self */
    fb_rom_w32(rom, 0x28, 0x00000404);
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    fb_m68k_run(md, 200);
    T_CHECK_EQ_U(md->m68k.pc & 0xFFFFFFu, 0x404);
    emu_core_destroy(&md->base);
}

static void test_privilege_violation(void)
{
    /* RTE in user mode -> privilege violation (vector 8 at $20).
     * Start in user mode: set SR = 0 via MOVE #$0000,SR from a supervisor
     * boot: after boot (SR=0x2700), do ANDI.W #$CFFF? Simplest: MOVE.W
     * #$0000,SR (0x46C0? MOVE to SR is privileged; from supervisor ok).
     */
    size_t size = 0;
    uint8_t *rom = fb_test_rom(&size, 0x400);
    fb_rom_w16(rom, 0x400, 0x46FC); /* MOVE.W #$0000,SR */
    fb_rom_w16(rom, 0x402, 0x0000); /*   -> user mode, IPL 0 */
    fb_rom_w16(rom, 0x404, 0x4E73); /* RTE: privileged -> violation */
    fb_rom_w16(rom, 0x406, 0x60FE);
    fb_rom_w16(rom, 0x408, 0x60FE); /* handler loops */
    fb_rom_w32(rom, 0x20, 0x00000408);
    struct fb_md *md = fb_boot(rom, size);
    free(rom);
    if (!md) { T_FAIL("boot failed"); return; }
    fb_m68k_run(md, 300);
    T_CHECK_EQ_U(md->m68k.pc & 0xFFFFFFu, 0x408);
    T_CHECK_EQ_U(md->m68k.sr & FB_SR_S, FB_SR_S); /* back to supervisor */
    emu_core_destroy(&md->base);
}

T_SUITE_BEGIN(finalburn_m68k)
{ "moveq_imm", test_moveq },
{ "moveq_zero_sets_z", test_moveq_zero },
{ "addi_byte_carry", test_addi_carry },
{ "move_w_reg_to_reg", test_move_w_reg },
{ "lea_abs_w", test_lea_abs_w },
{ "movea_sign_extend", test_movea_sign_extend },
{ "jsr_rts_stack", test_jsr_rts },
{ "dbra_loop", test_dbra_loop },
{ "addq_subq", test_addq_subq },
{ "mulu_divu", test_mulu_divu },
{ "divu_overflow_v", test_divu_overflow },
{ "lsl_flags", test_lsl_flags },
{ "neg", test_neg },
{ "abcd_decimal", test_abcd },
{ "movem_roundtrip", test_movem },
{ "cmp_blt", test_cmp_bcc },
{ "predecrement_postincrement", test_predecrement_postincrement },
{ "trap_exception", test_trap_exception },
{ "line_a_exception", test_line_a_exception },
{ "privilege_violation", test_privilege_violation },
T_SUITE_END
T_SUITE_REG(finalburn_m68k)
