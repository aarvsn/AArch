/*
 * mgbx CPU tests. Expected values are hand-computed from the SM83
 * specification (Pan Docs / LR35902 docs), NOT from emulator helpers.
 *
 * Harness: each test builds a tiny program at 0x0150 in a synthetic ROM,
 * loads it into a full core instance, runs frames, and reads CPU state
 * through a small white-box accessor (mgbx internals are visible to tests
 * via src includes, but expected values are always derived independently).
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbx/mgbx.h"

#include <stdlib.h>
#include <string.h>

/* --- white-box helpers (state inspection only; expectations are computed
 *     in the tests from the spec, never by calling CPU helpers) --- */

static struct mgbx *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbx()->create(&c) != EMU_OK)
        return NULL;
    return (struct mgbx *)c;
}

/* Builds a ROM whose bank 0 beyond 0x150 contains `code`; PC starts at 0x150.
 * The two boot instructions (NOP; JP 0x150) are executed during setup so the
 * CPU is parked at 0x150 before the test counts program instructions.
 * The program is expected to end in an infinite loop (JR -2). */
static struct mgbx *mk_cpu_core(const uint8_t *code, size_t len)
{
    struct mgbx *gb = mk_core();
    if (gb == NULL)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = gb_make_rom(2, GB_CART_ROM_ONLY, 0, &rom_size);
    memcpy(&rom[0x0150], code, len);
    emu_result_t r = emu_core_mgbx()->load_rom(&gb->base, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_mgbx()->destroy(&gb->base);
        return NULL;
    }
    gb_cpu_step(gb); /* NOP at 0x100 */
    gb_cpu_step(gb); /* JP 0x150 */
    return gb;
}

static void run_n_instructions(struct mgbx *gb, int n)
{
    for (int i = 0; i < n; i++)
        gb_cpu_step(gb);
}

/* --- flag arithmetic ------------------------------------------------------ */

static void cpu_add_flags(void)
{
    /* LD A,0x0F ; ADD A,0x01 -> A=0x10 Z=0 N=0 H=1 C=0 */
    static const uint8_t prog[] = { 0x3E, 0x0F, 0xC6, 0x01, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.a, 0x10);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x20); /* H set only */
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_add_carry(void)
{
    /* LD A,0x20 ; ADD A,0xF0 -> 0x10, H=0 C=1 */
    static const uint8_t prog[] = { 0x3E, 0x20, 0xC6, 0xF0, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.a, 0x10);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x10);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_adc_carry_in(void)
{
    /* SCF ; LD A,0x0F ; ADC A,0x00 -> 0x10 with H=1, C=0, Z=0
     * (0x0F + 0x00 + 1 = 0x10; half carry from 0xF + 0 + 1) */
    static const uint8_t prog[] = { 0x37, 0x3E, 0x0F, 0xCE, 0x00, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3);
    T_CHECK_EQ(gb->cpu.a, 0x10);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x20);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_sub_half_borrow(void)
{
    /* LD A,0x10 ; SUB 0x01 -> 0x0F, N=1 H=1 C=0 */
    static const uint8_t prog[] = { 0x3E, 0x10, 0xD6, 0x01, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.a, 0x0F);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x60);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_sbc_borrow(void)
{
    /* SCF ; LD A,0x00 ; SBC A,0x01 -> 0xFE? spec: 0 - 1 - 1 = -2 = 0xFE,
     * N=1 H=1 C=1 */
    static const uint8_t prog[] = { 0x37, 0x3E, 0x00, 0xDE, 0x01, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3);
    T_CHECK_EQ(gb->cpu.a, 0xFE);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x70);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_daa(void)
{
    /* BCD 45 + 45 = 90: LD A,0x45; ADD 0x45 -> 0x8A; DAA -> 0x90, C=0 */
    static const uint8_t prog[] = { 0x3E, 0x45, 0xC6, 0x45, 0x27, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3);
    T_CHECK_EQ(gb->cpu.a, 0x90);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x00);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_daa_sub(void)
{
    /* BCD 51 - 28 = 23 with N set:
     * LD A,0x51; SUB 0x28 -> 0x29 (raw), flags N=1 H=1 C=0; DAA -> 0x23 */
    static const uint8_t prog[] = { 0x3E, 0x51, 0xD6, 0x28, 0x27, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3);
    T_CHECK_EQ(gb->cpu.a, 0x23);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x40); /* N stays set */
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_cp_flags(void)
{
    /* LD A,0x80 ; CP 0x01 -> 0x80-0x01: N=1, H=1 (0x0 - 0x1 borrows), C=0 */
    static const uint8_t prog[] = { 0x3E, 0x80, 0xFE, 0x01, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.a, 0x80); /* CP does not modify A */
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x60);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_add_sp_e8(void)
{
    /* LD HL,SP+1 with SP=0x000F: H=1 (0xF+1>0xF), C=0, result 0x0010.
     * We set SP via LD SP,0x000F first. */
    static const uint8_t prog[] = {
        0x31, 0x0F, 0x00, /* LD SP,0x000F */
        0xF8, 0x01,       /* LD HL,SP+1   */
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.h, 0x00);
    T_CHECK_EQ(gb->cpu.l, 0x10);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x20); /* H=1, C=0, Z=0, N=0 */
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_add_sp_e8_carry(void)
{
    /* SP=0x00FF, e=+1 -> 0x0100, H=1 C=1 */
    static const uint8_t prog[] = {
        0x31, 0xFF, 0x00,
        0xF8, 0x01,
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.h, 0x01);
    T_CHECK_EQ(gb->cpu.l, 0x00);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x30);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_add_hl_carry(void)
{
    /* HL=0xFFFF, BC=1: ADD HL,BC -> 0x0000, H=1 C=1. XOR A first so Z=0
     * (ADD HL,xx leaves Z unchanged, so it must be pre-cleared). */
    static const uint8_t prog[] = {
        0x21, 0xFF, 0xFF, /* LD HL,0xFFFF */
        0x01, 0x01, 0x00, /* LD BC,0x0001 */
        0xAF,             /* XOR A */
        0xF6, 0x01,       /* OR 0x01 (Z=0, N=0, H=0, C=0) */
        0x09,             /* ADD HL,BC    */
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 5);
    T_CHECK_EQ(gb->cpu.h, 0x00);
    T_CHECK_EQ(gb->cpu.l, 0x00);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x30); /* H=1 C=1, Z=0 */
    emu_core_mgbx()->destroy(&gb->base);
}

/* --- control flow / stack ------------------------------------------------- */

static void cpu_jr_signed(void)
{
    /* JR +2 from operand-end 0x152 lands at 0x154, skipping 0x152-0x153 */
    static const uint8_t prog[] = {
        0x18, 0x02,       /* 0x150: JR +2 */
        0x00, 0x00,       /* 0x152-0x153: skipped */
        0x3E, 0x42,       /* 0x154: LD A,0x42 */
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.a, 0x42);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_jr_backward(void)
{
    /* backward JR builds a counted loop: B=3, DEC B, JR NZ,-3 -> B ends 0 */
    static const uint8_t prog[] = {
        0x06, 0x03,       /* 0x150: LD B,3 */
        0x05,             /* 0x152: DEC B */
        0x20, 0xFD,       /* 0x153: JR NZ,-3 (to 0x152) */
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    for (int i = 0; i < 20; i++)
        gb_cpu_step(gb);
    T_CHECK_EQ(gb->cpu.b, 0);
    T_CHECK_EQ(gb->cpu.pc, 0x155); /* parked on JR -2 */
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_push_pop(void)
{
    /* LD BC,0x1234; PUSH BC; POP DE -> DE=0x1234, SP unchanged */
    static const uint8_t prog[] = {
        0x01, 0x34, 0x12,
        0xC5,             /* PUSH BC */
        0xD1,             /* POP DE  */
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3);
    T_CHECK_EQ(gb->cpu.d, 0x12);
    T_CHECK_EQ(gb->cpu.e, 0x34);
    T_CHECK_EQ(gb->cpu.sp, 0xFFFE); /* initial SP restored */
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_call_ret(void)
{
    /* CALL to a subroutine that increments A and RETs */
    static const uint8_t prog[] = {
        0xCD, 0x58, 0x01, /* 0x150: CALL 0x0158 */
        0x18, 0xFE,       /* 0x153: JR -2 */
        0x00, 0x00, 0x00, /* 0x156..0x157 filler */
        0x3C,             /* 0x158: INC A */
        0xC9              /* 0x159: RET */
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3); /* CALL, INC A, RET */
    T_CHECK_EQ(gb->cpu.a, 0x02); /* post-boot A=0x01, INC ran exactly once */
    T_CHECK_EQ(gb->cpu.pc, 0x153);
    T_CHECK_EQ(gb->cpu.sp, 0xFFFE);
    emu_core_mgbx()->destroy(&gb->base);
}

/* --- interrupts / HALT / EI ------------------------------------------------- */

static void cpu_interrupt_dispatch(void)
{
    struct mgbx *gb = mk_cpu_core((const uint8_t[]){ 0x18, 0xFE }, 2);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* sit in the idle loop (JR -2) */
    run_n_instructions(gb, 2);
    uint16_t pc_before = gb->cpu.pc;

    gb->mem.ie = 0x01;              /* VBlank enabled */
    gb->mem.if_reg = 0x01;          /* VBlank requested */
    gb->cpu.ime = 1;
    uint16_t sp_before = gb->cpu.sp;

    /* step = the JR instruction (12 T) + dispatch (20 T) */
    uint32_t t = gb_cpu_step(gb);
    T_CHECK_EQ(t, 32);
    T_CHECK_EQ(gb->cpu.pc, 0x40);
    T_CHECK_EQ(gb->cpu.ime, 0);
    T_CHECK_EQ(gb->cpu.sp, (uint16_t)(sp_before - 2));
    T_CHECK_EQ(gb->mem.if_reg & 0x01u, 0);
    /* pushed return address must be the loop PC */
    uint16_t pushed = (uint16_t)(gb_bus_read(gb, gb->cpu.sp) |
                                 (gb_bus_read(gb, (uint16_t)(gb->cpu.sp + 1)) << 8));
    T_CHECK_EQ(pushed, pc_before);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_halt_wake_no_ime(void)
{
    /* HALT with IME=0 and no pending: CPU halts; wakes when IF gets a bit,
     * without dispatching. */
    static const uint8_t prog[] = {
        0xF3,       /* DI */
        0x76,       /* HALT */
        0x3E, 0x77, /* LD A,0x77 (runs after wake) */
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.halted, 1);
    uint32_t t = gb_cpu_step(gb); /* halted m-cycle */
    T_CHECK_EQ(t, 4);
    gb->mem.ie = 0x04;
    gb->mem.if_reg = 0x04;        /* timer int arrives */
    T_CHECK_EQ(gb_cpu_step(gb), 4); /* wake cycle */
    T_CHECK_EQ(gb->cpu.halted, 0);
    T_CHECK_EQ(gb->cpu.ime, 0);   /* IME stays 0: no dispatch */
    run_n_instructions(gb, 1);
    T_CHECK_EQ(gb->cpu.a, 0x77);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_halt_bug(void)
{
    /* HALT with IME=0 and a pending interrupt triggers the halt bug:
     * the byte after HALT is read twice (PC not incremented once). */
    static const uint8_t prog[] = {
        0xF3,       /* 0x150: DI */
        0x76,       /* 0x151: HALT */
        0x3E, 0x42, /* 0x152: LD A,0x42 */
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    /* interrupt pending BEFORE HALT executes (halt bug precondition) */
    gb->mem.ie = 0x01;
    gb->mem.if_reg = 0x01;
    run_n_instructions(gb, 1); /* DI: ime=0, pending stays */
    T_CHECK_EQ(gb_cpu_step(gb), 4); /* HALT: ime=0 + pending -> halt bug */
    T_CHECK_EQ(gb->cpu.halted, 0);
    T_CHECK_EQ(gb->cpu.halt_bug, 1);
    T_CHECK_EQ(gb->cpu.pc, 0x152);
    /* next fetch does NOT advance PC: byte 0x3E is read as the opcode AND
     * again as its own operand (documented halt-bug effect) */
    run_n_instructions(gb, 1);
    T_CHECK_EQ(gb->cpu.pc, 0x153);
    T_CHECK_EQ(gb->cpu.a, 0x3E);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_ei_delay(void)
{
    /* EI ; DI -> IME stays 0 (DI runs before EI takes effect) */
    static const uint8_t prog[] = { 0xFB, 0xF3, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    T_CHECK_EQ(gb_cpu_step(gb), 4); /* EI: sets pending */
    T_CHECK_EQ(gb->cpu.ime, 0);
    gb_cpu_step(gb);                /* DI: ime=0, pending cleared? spec:
                                       pending fires before DI -> then DI
                                       clears; either way IME==0 after */
    T_CHECK_EQ(gb->cpu.ime, 0);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_ei_then_halt_services(void)
{
    /* EI ; HALT with int pending: HALT should exit and SERVICE the interrupt
     * (IME is enabled by then). */
    static const uint8_t prog[] = { 0xFB, 0x76, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    gb->mem.ie = 0x01;
    gb->mem.if_reg = 0x01;
    gb_cpu_step(gb); /* EI */
    uint32_t t = gb_cpu_step(gb); /* HALT: ime becomes 1, then halts */
    (void)t;
    /* halted with ime=1: next step exits halt AND dispatches */
    t = gb_cpu_step(gb);
    T_CHECK_EQ(t, 20);
    T_CHECK_EQ(gb->cpu.pc, 0x40);
    T_CHECK_EQ(gb->cpu.ime, 0);
    emu_core_mgbx()->destroy(&gb->base);
}

/* --- rotate group (Z flag behavior differs from Z80!) ---------------------- */

static void cpu_rlca_flags(void)
{
    /* LD A,0x81 ; AND A (clears C, sets Z=0,H=1) ; RLCA -> A=0x03, C=1,
     * Z stays 0 (SM83: RLCA does not SET Z, it leaves it) */
    static const uint8_t prog[] = { 0x3E, 0x81, 0xA7, 0x07, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3);
    T_CHECK_EQ(gb->cpu.a, 0x03);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x10); /* Z=0, C=1 */
    emu_core_mgbx()->destroy(&gb->base);
}

/* --- CB shifts / bit -------------------------------------------------------- */

static void cpu_cb_sra(void)
{
    /* LD A,0x87 ; CB 0x2F (SRA A) -> A=0xC3, C=1, Z=0 */
    static const uint8_t prog[] = { 0x3E, 0x87, 0xCB, 0x2F, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.a, 0xC3);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x10);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_cb_swap(void)
{
    /* LD A,0xAB ; CB 0x37 (SWAP A) -> A=0xBA, Z=0, C=0 */
    static const uint8_t prog[] = { 0x3E, 0xAB, 0xCB, 0x37, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 2);
    T_CHECK_EQ(gb->cpu.a, 0xBA);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x00);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_cb_bit_z(void)
{
    /* XOR A (C=0) ; LD A,0x10 ; CB 0x47 (BIT 0,A) -> Z=1, H=1, C=0 */
    static const uint8_t prog[] = { 0xAF, 0x3E, 0x10, 0xCB, 0x47, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0xA0); /* Z=1, H=1, C=0 */
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_ldh_memory(void)
{
    /* LD (0xFF80),0xAB via LDH; read back through LDH A,(n) */
    static const uint8_t prog[] = {
        0x3E, 0xAB,       /* LD A,0xAB */
        0xE0, 0x80,       /* LDH (0x80),A */
        0x3E, 0x00,       /* LD A,0 */
        0xF0, 0x80,       /* LDH A,(0x80) */
        0x18, 0xFE
    };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 4);
    T_CHECK_EQ(gb->cpu.a, 0xAB);
    T_CHECK_EQ(gb->mem.hram[0], 0xAB);
    emu_core_mgbx()->destroy(&gb->base);
}

static void cpu_inc_dec_flags(void)
{
    /* XOR A (clear C) ; LD A,0x0F ; INC A -> H=1,C=0,Z=0,N=0;
     * DEC A -> 0x0F with N=1, H=1 (0x0 - 1 borrows) */
    static const uint8_t prog[] = { 0xAF, 0x3E, 0x0F, 0x3C, 0x3D, 0x18, 0xFE };
    struct mgbx *gb = mk_cpu_core(prog, sizeof prog);
    T_CHECK(gb != NULL);
    if (!gb)
        return;
    run_n_instructions(gb, 3);
    T_CHECK_EQ(gb->cpu.a, 0x10);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x20); /* H set, Z=0, N=0, C=0 */
    run_n_instructions(gb, 1);
    T_CHECK_EQ(gb->cpu.a, 0x0F);
    T_CHECK_EQ(gb->cpu.f & 0xF0u, 0x60); /* N=1 H=1 */
    emu_core_mgbx()->destroy(&gb->base);
}

T_SUITE_BEGIN(gb_cpu)
{ "add_flags", cpu_add_flags },
{ "add_carry", cpu_add_carry },
{ "adc_carry_in", cpu_adc_carry_in },
{ "sub_half_borrow", cpu_sub_half_borrow },
{ "sbc_borrow", cpu_sbc_borrow },
{ "daa_bcd_add", cpu_daa },
{ "daa_bcd_sub", cpu_daa_sub },
{ "cp_flags", cpu_cp_flags },
{ "add_sp_e8", cpu_add_sp_e8 },
{ "add_sp_e8_carry", cpu_add_sp_e8_carry },
{ "add_hl_carry", cpu_add_hl_carry },
{ "jr_signed", cpu_jr_signed },
{ "jr_backward_loop", cpu_jr_backward },
{ "push_pop", cpu_push_pop },
{ "call_ret", cpu_call_ret },
{ "interrupt_dispatch", cpu_interrupt_dispatch },
{ "halt_wake_no_ime", cpu_halt_wake_no_ime },
{ "halt_bug", cpu_halt_bug },
{ "ei_delay", cpu_ei_delay },
{ "ei_then_halt_services", cpu_ei_then_halt_services },
{ "rlca_flags", cpu_rlca_flags },
{ "cb_sra", cpu_cb_sra },
{ "cb_swap", cpu_cb_swap },
{ "cb_bit_z", cpu_cb_bit_z },
{ "ldh_memory", cpu_ldh_memory },
{ "inc_dec_flags", cpu_inc_dec_flags },
T_SUITE_END

T_SUITE_REG(gb_cpu)
