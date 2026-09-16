/*
 * beatle-nes-redux CPU tests. Expected values hand-computed from the 6502
 * specification; opcodes are assembled by hand (raw bytes) independently of
 * the emulator's decoder.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "beatle-nes-redux/nes.h"

#include <stdlib.h>
#include <string.h>

static struct nes *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_beatle_nes_redux()->create(&c) != EMU_OK)
        return NULL;
    return (struct nes *)c;
}

/* NROM ROM with code at $C000; reset vector -> $C000 */
static struct nes *mk_cpu_core(const uint8_t *code, size_t len)
{
    struct nes *n = mk_core();
    if (n == NULL)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = nes_make_rom(2, 1, 0, 0x01 /* vertical mirroring */, &rom_size);
    memcpy(&rom[16 + 0x4000], code, len); /* second 16K bank at $C000 */
    rom[16 + 0x7FFC] = 0x00;
    rom[16 + 0x7FFD] = 0xC0; /* reset -> $C000 */
    emu_result_t r = emu_core_beatle_nes_redux()->load_rom(&n->base, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_beatle_nes_redux()->destroy(&n->base);
        return NULL;
    }
    return n;
}

static void run_n(struct nes *n, int count)
{
    for (int i = 0; i < count; i++)
        nes_cpu_step(n);
}

/* program fragments */
#define PROG_LDA_IMM(v) 0xA9, (v)
#define PROG_JMP_SELF 0x4C, 0x08, 0xC0 /* jmp $C008: self after 3-byte prog? */

static void cpu_lda_flags(void)
{
    static const uint8_t prog[] = { 0xA9, 0x00, 0x4C, 0x02, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 1);
    T_CHECK_EQ(n->cpu.a, 0x00);
    T_CHECK(n->cpu.p & NES_FZ);   /* zero */
    T_CHECK(!(n->cpu.p & NES_FN)); /* positive */
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_lda_negative(void)
{
    static const uint8_t prog[] = { 0xA9, 0x80, 0x4C, 0x02, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 1);
    T_CHECK(n->cpu.p & NES_FN);
    T_CHECK(!(n->cpu.p & NES_FZ));
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_adc_overflow(void)
{
    /* 0x80 + 0x80 = 0x00 with C=1, V=1, Z=1 */
    static const uint8_t prog[] = { 0xA9, 0x80, 0x69, 0x80, 0x4C, 0x04, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 2);
    T_CHECK_EQ(n->cpu.a, 0x00);
    T_CHECK(n->cpu.p & NES_FC);
    T_CHECK(n->cpu.p & NES_FV);
    T_CHECK(n->cpu.p & NES_FZ);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_sbc_borrow(void)
{
    /* SEC; LDA #5; SBC #10 -> 0xFB with C=0 (borrow), N=1.
     * SEC (0x38), LDA imm 5 (0xA9 0x05), SBC imm 10 (0xE9 0x0A) */
    static const uint8_t prog[] = { 0x38, 0xA9, 0x05, 0xE9, 0x0A, 0x4C, 0x05, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 3);
    T_CHECK_EQ(n->cpu.a, 0xFB);
    T_CHECK(!(n->cpu.p & NES_FC)); /* borrow occurred */
    T_CHECK(n->cpu.p & NES_FN);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_adc_decimal_disabled(void)
{
    /* NES 2A03 has no decimal mode: ADC with D set must act binary.
     * SED (0xF8); CLC (0x18); LDA #0x09; ADC #0x01 -> 0x0A (not BCD 0x10) */
    static const uint8_t prog[] = { 0xF8, 0x18, 0xA9, 0x09, 0x69, 0x01,
                                    0x4C, 0x06, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 4);
    T_CHECK_EQ(n->cpu.a, 0x0A); /* binary, not BCD */
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_asl_carry(void)
{
    /* LDA #0x81; ASL A -> 0x02, C=1, N=0, Z=0 */
    static const uint8_t prog[] = { 0xA9, 0x81, 0x0A, 0x4C, 0x03, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 2);
    T_CHECK_EQ(n->cpu.a, 0x02);
    T_CHECK(n->cpu.p & NES_FC);
    T_CHECK(!(n->cpu.p & NES_FN));
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_cmp_flags(void)
{
    /* LDA #0x50; CMP #0x50 -> equal: Z=1, C=1, N=0 */
    static const uint8_t prog[] = { 0xA9, 0x50, 0xC9, 0x50, 0x4C, 0x04, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 2);
    T_CHECK(n->cpu.p & NES_FZ);
    T_CHECK(n->cpu.p & NES_FC);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_zpx_wrap(void)
{
    /* LDX #0x05; LDA 0xFB,X -> zero-page wraps: address 0x00.
     * Preload RAM[0x00] = 0x99 via STA 0x00 after LDA #0x99.
     * LDA #0x99 (A9 99); STA 0x00 (85 00); LDX #5 (A2 05); LDA 0xFB,X (B5 FB) */
    static const uint8_t prog[] = { 0xA9, 0x99, 0x85, 0x00, 0xA2, 0x05,
                                    0xB5, 0xFB, 0x4C, 0x08, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 4);
    T_CHECK_EQ(n->cpu.a, 0x99); /* loaded from wrapped 0x00 */
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_jmp_indirect_bug(void)
{
    /* JMP ($02FF) reads low from $02FF and high from $0200 (page wrap bug).
     * Set RAM $02FF = 0x34, $0200 = 0x12 -> jump target $1234.
     * LDA #$34 (A9 34); STA $02FF (8D FF 02); LDA #$12 (A9 12); STA $0200
     * (8D 00 02); JMP ($02FF) (6C FF 02); then marker at $1234? $1234 is
     * RAM mirror... place target: we just check pc == 0x1234. */
    static const uint8_t prog[] = { 0xA9, 0x34, 0x8D, 0xFF, 0x02, 0xA9, 0x12,
                                    0x8D, 0x00, 0x02, 0x6C, 0xFF, 0x02 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 5);
    T_CHECK_EQ(n->cpu.pc, 0x1234); /* not 0x0234: wrap bug emulated */
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_jsr_rts(void)
{
    /* JSR $C009; subroutine: INC A (1A) at $C009, RTS (60) at $C00A.
     * RTS returns to $C003 (address after the JSR operand + 1). */
    static const uint8_t prog[] = { 0x20, 0x09, 0xC0, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x1A, 0x60 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 2); /* JSR + INC A */
    T_CHECK_EQ(n->cpu.pc, 0xC00A);
    run_n(n, 1); /* RTS */
    T_CHECK_EQ(n->cpu.pc, 0xC003);
    T_CHECK_EQ(n->cpu.s, 0xFD); /* restored initial stack */
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_php_plp_b_flag(void)
{
    /* PHP pushes B set; PLP strips it. LDA #$FF; PHP; PLA? verify pushed byte
     * at stack. LDA #$42 (A9 42); PHP (08); PLP? then PLA (68) -> A = pushed
     * value with B|U bits: 0x42 -> P has B=0 normally: pushed = P|0x30. */
    static const uint8_t prog[] = { 0xA9, 0x42, 0x08, 0x4C, 0x03, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 2);
    uint8_t pushed = nes_bus_read(n, 0x0100u + n->cpu.s + 1u);
    T_CHECK_EQ(pushed & 0x30u, 0x30u); /* B and U set on stack copy */
    T_CHECK_EQ(n->cpu.s, 0xFC);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_branch_cycles(void)
{
    /* BNE taken in same page: LDA #1; BNE +2: 3 cycles total (2+1 taken).
     * Compute via total_cycles delta. */
    static const uint8_t prog[] = { 0xA9, 0x01, 0xD0, 0x02, 0x4C, 0x06, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    uint64_t c0 = n->total_cycles;
    nes_cpu_step(n); /* LDA: 2 */
    T_CHECK_EQ((int)(n->total_cycles - c0), 2);
    nes_cpu_step(n); /* BNE taken: 3 */
    T_CHECK_EQ((int)(n->total_cycles - c0), 5);
    T_CHECK_EQ(n->cpu.pc, 0xC006);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_branch_page_cross(void)
{
    /* Case 1: BNE +2 at $C0FE. After-operand pc = $C100 (page $C1) and the
     * target $C102 is also page $C1 -> taken WITHOUT page cross: 3 cycles. */
    uint8_t prog[0x100];
    memset(prog, 0xEA, sizeof prog);
    prog[0x00] = 0xA9; prog[0x01] = 0x01; /* LDA #1 */
    prog[0xFE] = 0xD0; prog[0xFF] = 0x02;
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    uint64_t c0 = n->total_cycles;
    nes_cpu_step(n); /* LDA: 2 */
    for (int i = 0; i < 252; i++)
        nes_cpu_step(n); /* NOPs: 504 cycles */
    T_CHECK_EQ(n->cpu.pc, 0xC0FE);
    nes_cpu_step(n); /* BNE taken, same page: 3 */
    T_CHECK_EQ((int)(n->total_cycles - c0), 2 + 504 + 3);
    T_CHECK_EQ(n->cpu.pc, 0xC102);
    emu_core_beatle_nes_redux()->destroy(&n->base);

    /* Case 2: BNE +2 at $C0FD. After-operand pc = $C0FF (page $C0), target
     * $C101 (page $C1) -> taken WITH page cross: 4 cycles. */
    memset(prog, 0xEA, sizeof prog);
    prog[0x00] = 0xA9; prog[0x01] = 0x01;
    prog[0xFD] = 0xD0; prog[0xFE] = 0x02;
    n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    c0 = n->total_cycles;
    nes_cpu_step(n); /* LDA: 2 */
    for (int i = 0; i < 251; i++)
        nes_cpu_step(n); /* NOPs: 502 cycles */
    T_CHECK_EQ(n->cpu.pc, 0xC0FD);
    nes_cpu_step(n); /* BNE taken + cross: 4 */
    T_CHECK_EQ((int)(n->total_cycles - c0), 2 + 502 + 4);
    T_CHECK_EQ(n->cpu.pc, 0xC101);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_nmi_vector(void)
{
    static const uint8_t prog[] = { 0x4C, 0x00, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    n->cart.prg[0x7FFA] = 0x00; /* NMI vector -> $C000 (last bank) */
    n->cart.prg[0x7FFB] = 0xC0;
    run_n(n, 1); /* enter self-loop at $C000 */
    n->cpu.nmi_pending = 1;
    uint16_t sp = n->cpu.s;
    nes_cpu_step(n); /* services NMI: 7 cycles */
    T_CHECK_EQ(n->cpu.pc, 0xC000); /* NMI vector -> $C000 */
    T_CHECK_EQ(n->cpu.s, (uint8_t)(sp - 3));
    T_CHECK_EQ(n->cpu.nmi_pending, 0);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_irq_masked(void)
{
    static const uint8_t prog[] = { 0x4C, 0x00, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    run_n(n, 1);
    n->cpu.irq_line = 1; /* I flag is set after reset */
    uint16_t pc = n->cpu.pc;
    nes_cpu_step(n); /* IRQ must be ignored: executes JMP instead */
    T_CHECK_EQ(n->cpu.pc, pc);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cpu_brk(void)
{
    /* BRK at $C000: pushes pc+2, B set, jumps IRQ vector $C002 (loop) */
    static const uint8_t prog[] = { 0x00, 0x4C, 0x02, 0xC0 };
    struct nes *n = mk_cpu_core(prog, sizeof prog);
    T_CHECK(n != NULL);
    if (!n)
        return;
    /* set IRQ vector in the last PRG bank: $FFFE -> $C002 */
    n->cart.prg[0x7FFE] = 0x02;
    n->cart.prg[0x7FFF] = 0xC0;
    uint16_t sp = n->cpu.s;
    nes_cpu_step(n);
    T_CHECK_EQ(n->cpu.pc, 0xC002);
    T_CHECK(n->cpu.p & NES_FI); /* I set by BRK */
    uint8_t pushed_p = nes_bus_read(n, (uint16_t)(0x0100u + sp - 2u));
    T_CHECK(pushed_p & NES_FB);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

T_SUITE_BEGIN(nes_cpu)
{ "lda_flags", cpu_lda_flags },
{ "lda_negative", cpu_lda_negative },
{ "adc_overflow", cpu_adc_overflow },
{ "sbc_borrow", cpu_sbc_borrow },
{ "decimal_disabled", cpu_adc_decimal_disabled },
{ "asl_carry", cpu_asl_carry },
{ "cmp_flags", cpu_cmp_flags },
{ "zpx_wrap", cpu_zpx_wrap },
{ "jmp_indirect_page_bug", cpu_jmp_indirect_bug },
{ "jsr_rts", cpu_jsr_rts },
{ "php_b_flag", cpu_php_plp_b_flag },
{ "branch_cycles", cpu_branch_cycles },
{ "branch_page_cross", cpu_branch_page_cross },
{ "nmi_vector", cpu_nmi_vector },
{ "irq_masked", cpu_irq_masked },
{ "brk", cpu_brk },
T_SUITE_END

T_SUITE_REG(nes_cpu)
