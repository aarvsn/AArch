/*
 * supersnes CPU tests. Expected values hand-computed from the 65C816
 * specification (Western Digital / WDC manuals); opcodes are raw bytes.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "supersnes/snes.h"

#include <stdlib.h>
#include <string.h>

static struct snes *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_supersnes()->create(&c) != EMU_OK)
        return NULL;
    return (struct snes *)c;
}

static struct snes *mk_cpu_core(const uint8_t *code, size_t len)
{
    struct snes *s = mk_core();
    if (s == NULL)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = snes_make_lorom(16, &rom_size);
    memcpy(&rom[0x0000], code, len); /* LoROM bank 0 maps at $8000 */
    emu_result_t r = emu_core_supersnes()->load_rom(&s->base, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_supersnes()->destroy(&s->base);
        return NULL;
    }
    /* point the CPU at $8000 (bank 0), native mode for most tests */
    s->cpu.pc = 0x8000;
    s->cpu.k = 0;
    s->cpu.dbr = 0;
    return s;
}

static void run_n(struct snes *s, int count)
{
    for (int i = 0; i < count; i++)
        snes_cpu_step(s);
}

/* SEP #$20 (M=1, 8-bit) helper prefix */
#define SEP_M8 0xE2, 0x20
#define REP_M16 0xC2, 0x20

static void cpu_8bit_adc(void)
{
    /* SEP #$20; CLC; A9 80 (LDA #0x80); 69 80 (ADC #0x80) -> A=0x00 C=1 V=1 */
    static const uint8_t prog[] = { SEP_M8, 0x18, 0xA9, 0x80, 0x69, 0x80 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 4);
    T_CHECK_EQ(s->cpu.a & 0xFFu, 0x00);
    T_CHECK(s->cpu.p & SNES_FC);
    T_CHECK(s->cpu.p & SNES_FV);
    T_CHECK(s->cpu.p & SNES_FZ);
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_16bit_adc(void)
{
    /* REP #$20; CLC; A9 FF FF (LDA #$FFFF); 69 01 00 (ADC #1) -> A=0x0000 C=1 */
    static const uint8_t prog[] = { 0x18, 0xFB, 0xC2, 0x20, 0xA9, 0xFF, 0xFF,
                                    0x18, 0x69, 0x01, 0x00 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 6);
    T_CHECK_EQ(s->cpu.a, 0x0000);
    T_CHECK(s->cpu.p & SNES_FC);
    T_CHECK(!(s->cpu.p & SNES_FV));
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_16bit_index(void)
{
    /* REP #$10; A2 FF 00 (LDX #$00FF); E8 (INX) -> X=0x0100 */
    static const uint8_t prog[] = { 0x18, 0xFB, 0xC2, 0x10, 0xA2, 0xFF, 0x00, 0xE8 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 5);
    T_CHECK_EQ(s->cpu.x, 0x0100);
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_xce_emulation(void)
{
    /* CLC; XCE (to native); SEC; XCE -> enter emulation: E=1, M/X forced 1 */
    static const uint8_t prog[] = { 0x18, 0xFB, 0x38, 0xFB };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 4);
    T_CHECK_EQ(s->cpu.e, 1);
    T_CHECK(s->cpu.p & SNES_FM);
    T_CHECK(s->cpu.p & SNES_FX);
    T_CHECK(!(s->cpu.p & SNES_FC)); /* carry = old E (0) */
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_emulation_dp_high_forced(void)
{
    /* In emulation mode the direct page high byte is forced to zero:
     * TCS sets S; use XCE into emulation, then LDA #2; TCD would set DP...
     * Simpler: verify via direct struct manipulation + (dp) access. */
    static const uint8_t prog[] = { 0x38, 0xFB, SEP_M8, 0xA9, 0x34, 0x85, 0x10 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    s->cpu.dp = 0x1000; /* high byte nonzero: emulation must ignore it */
    s->cpu.e = 1;
    run_n(s, 4); /* SEC, XCE, SEP, LDA #0x34 */
    snes_cpu_step(s); /* STA $10 -> writes to $00:0010 (DP high forced 0) */
    T_CHECK_EQ(s->mem.wram[0x10], 0x34);
    T_CHECK_EQ(s->mem.wram[0x1010], 0x00);
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_decimal_adc(void)
{
    /* SED; CLC; LDA #0x45; ADC #0x38 -> BCD 83: A=0x83, C=0 */
    static const uint8_t prog[] = { SEP_M8, 0xF8, 0x18, 0xA9, 0x45, 0x69, 0x38 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 5);
    T_CHECK_EQ(s->cpu.a & 0xFFu, 0x83);
    T_CHECK(!(s->cpu.p & SNES_FC));
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_decimal_adc_carry(void)
{
    /* SED; CLC; LDA #0x99; ADC #0x01 -> BCD 100: A=0x00, C=1 (Z from the
     * binary result 0x9A: not zero) */
    static const uint8_t prog[] = { SEP_M8, 0xF8, 0x18, 0xA9, 0x99, 0x69, 0x01 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 5);
    T_CHECK_EQ(s->cpu.a & 0xFFu, 0x00);
    T_CHECK(s->cpu.p & SNES_FC);
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_xba(void)
{
    /* LDA #$12AB (16-bit); XBA swaps -> B=0x12, A=0xAB12; N set from 0x12 */
    static const uint8_t prog[] = { 0x18, 0xFB, REP_M16, 0xA9, 0xAB, 0x12, 0xEB };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 5);
    T_CHECK_EQ(s->cpu.a, 0xAB12); /* bytes swapped */
    T_CHECK_EQ(s->cpu.a & 0xFFu, 0x12);
    T_CHECK(!(s->cpu.p & SNES_FN)); /* N from new low byte 0x12 */
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_long_jump(void)
{
    /* JML $028000 (5C 00 80 02) -> K=2, PC=0x8000 */
    static const uint8_t prog[] = { 0x5C, 0x00, 0x80, 0x02 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 1);
    T_CHECK_EQ(s->cpu.k, 2);
    T_CHECK_EQ(s->cpu.pc, 0x8000);
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_per_pea_stack(void)
{
    /* PEA #$1234 pushes 0x1234 at S -> read back from stack memory */
    static const uint8_t prog[] = { 0xF4, 0x34, 0x12 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    s->cpu.sp = 0x01FF;
    run_n(s, 1);
    T_CHECK_EQ(s->cpu.sp, 0x01FD);
    T_CHECK_EQ(s->mem.wram[0x01FF], 0x12); /* high byte pushed first */
    T_CHECK_EQ(s->mem.wram[0x01FE], 0x34);
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_mvn_block_move(void)
{
    /* MVN moves A bytes from X (src bank) to Y (dst bank):
     * set X=$8000 (src bank 0 file), Y=WRAM $7E2000, A=4; MVN 0,7E */
    /* CLC; XCE (native); REP #$30 (M and X 16-bit); LDX #$8000; LDY #$2000;
     * LDA #4; MVN 0,$7E copies 4 bytes from $00:8000 to $7E:2000 */
    static const uint8_t prog[] = { 0x18, 0xFB, 0xC2, 0x30, 0xA2, 0x00, 0x80,
                                    0xA0, 0x00, 0x20, 0xA9, 0x04, 0x00,
                                    0x54, 0x00, 0x7E };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 7); /* XCE, REP, LDX, LDY, LDA, MVN */
    /* source data at $00:9000 (file offset 0x1000) */
    for (int i = 0; i < 4; i++)
        s->cart.rom[0x1000u + (uint32_t)i] = (uint8_t)i;
    s->cpu.x = 0x9000;
    s->cpu.y = 0x2000; /* first MVN advanced Y; reset it */
    s->cpu.a = 4;
    s->cpu.pc = 0x800D; /* MVN operand bytes within the program */
    static const uint8_t mvn[] = { 0x54, 0x00, 0x7E };
    memcpy(&s->cart.rom[0x000D], mvn, 3);
    snes_cpu_step(s);
    T_CHECK_EQ(s->mem.wram[0x2000], 0);
    T_CHECK_EQ(s->mem.wram[0x2001], 1);
    T_CHECK_EQ(s->mem.wram[0x2002], 2);
    T_CHECK_EQ(s->mem.wram[0x2003], 3);
    T_CHECK_EQ(s->cpu.dbr, 0x7E);
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_wai_wakes_on_nmi(void)
{
    static const uint8_t prog[] = { 0xCB, 0xEA, 0xEA };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    run_n(s, 1);
    T_CHECK_EQ(s->cpu.wai, 1);
    uint64_t c0 = s->total_cycles;
    snes_cpu_step(s); /* WAI keeps sleeping: 3 cycles? step returns 6 */
    T_CHECK_EQ(s->cpu.wai, 1);
    (void)c0;
    s->cpu.nmi_pending = 1;
    snes_cpu_step(s); /* wakes and services NMI */
    T_CHECK_EQ(s->cpu.wai, 0);
    T_CHECK_EQ(s->cpu.nmi_pending, 0);
    emu_core_supersnes()->destroy(&s->base);
}

static void cpu_brk_native_vector(void)
{
    /* native-mode BRK uses $FFE6 */
    static const uint8_t prog[] = { 0x00 };
    struct snes *s = mk_cpu_core(prog, sizeof prog);
    T_CHECK(s != NULL);
    if (!s)
        return;
    /* vector bytes in WRAM mirrors? No: vectors are in bank 0 ROM at FFF6-7.
     * LoROM: $00:FFE6 -> file offset (0<<15)|(0x7FE6) = 0x7FE6 */
    s->cart.rom[0x7FE6] = 0x00;
    s->cart.rom[0x7FE7] = 0x80;
    s->cpu.e = 0;
    s->cpu.p &= (uint8_t)~SNES_FM; /* not needed; BRK forces M on dispatch */
    run_n(s, 1);
    T_CHECK_EQ(s->cpu.pc, 0x8000);
    T_CHECK(s->cpu.p & SNES_FI); /* I set by BRK */
    emu_core_supersnes()->destroy(&s->base);
}

T_SUITE_BEGIN(snes_cpu)
{ "adc_8bit", cpu_8bit_adc },
{ "adc_16bit", cpu_16bit_adc },
{ "index_16bit", cpu_16bit_index },
{ "xce_emulation", cpu_xce_emulation },
{ "emulation_dp_high", cpu_emulation_dp_high_forced },
{ "decimal_adc", cpu_decimal_adc },
{ "decimal_adc_carry", cpu_decimal_adc_carry },
{ "xba", cpu_xba },
{ "long_jump", cpu_long_jump },
{ "pea_stack", cpu_per_pea_stack },
{ "mvn_block_move", cpu_mvn_block_move },
{ "wai_nmi", cpu_wai_wakes_on_nmi },
{ "brk_native_vector", cpu_brk_native_vector },
T_SUITE_END

T_SUITE_REG(snes_cpu)
