/*
 * mds-a: Nintendo DS core tests.
 *
 * Expected values derive from the ARMv4T/v5TE architecture and the DS
 * hardware registers (hand-assembled opcodes; documented core model in
 * src/mds-a/ds.h) - never from emulator internals.
 */
#include "../tests.h"
#include "../../src/mds-a/ds.h"

#include <stdlib.h>
#include <string.h>

static void w16(uint8_t *p, uint32_t off, uint16_t v)
{
    p[off] = (uint8_t)v; /* NDS code/data is little-endian */
    p[off + 1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
}

/* Builds a minimal .nds image: valid logo at 0x160, ARM9 binary at 0x200
 * with header fields pointing at main RAM, ARM7 binary after it. */
static uint8_t *nds_make(const uint32_t *arm9, size_t arm9_words,
                         const uint32_t *arm7, size_t arm7_words,
                         size_t *size_out)
{
    size_t arm9_len = arm9_words * 4u;
    size_t arm7_len = arm7_words * 4u;
    size_t size = 0x400u + arm9_len + arm7_len;
    uint8_t *rom = calloc(1, size);
    if (rom == NULL)
        return NULL;
    static const uint8_t logo8[8] = {
        0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21
    };
    memcpy(rom + DS_HDR_LOGO, logo8, sizeof logo8);

    w32(rom, DS_HDR_ARM9_OFF, 0x200u);
    w32(rom, DS_HDR_ARM9_ENTRY, 0x02000000u);
    w32(rom, DS_HDR_ARM9_ADDR, 0x02000000u);
    w32(rom, DS_HDR_ARM9_SIZE, (uint32_t)arm9_len);
    w32(rom, DS_HDR_ARM7_OFF, (uint32_t)(0x200u + arm9_len));
    w32(rom, DS_HDR_ARM7_ENTRY, 0x02380000u);
    w32(rom, DS_HDR_ARM7_ADDR, 0x02380000u);
    w32(rom, DS_HDR_ARM7_SIZE, (uint32_t)arm7_len);

    for (size_t i = 0; i < arm9_words; i++)
        w32(rom, 0x200u + i * 4u, arm9[i]);
    for (size_t i = 0; i < arm7_words; i++)
        w32(rom, 0x200u + arm9_len + i * 4u, arm7[i]);
    *size_out = size;
    return rom;
}

static struct ds *ds_boot_prog(const uint32_t *arm9, size_t arm9_words,
                               const uint32_t *arm7, size_t arm7_words)
{
    size_t size = 0;
    uint8_t *rom = nds_make(arm9, arm9_words, arm7, arm7_words, &size);
    if (rom == NULL)
        return NULL;
    emu_core_t *c = NULL;
    if (emu_core_mds_a()->create(&c) != EMU_OK) {
        free(rom);
        return NULL;
    }
    if (emu_core_mds_a()->load_rom(c, rom, size) != EMU_OK) {
        free(rom);
        emu_core_mds_a()->destroy(c);
        return NULL;
    }
    free(rom);
    return (struct ds *)c;
}

static void ds_free(struct ds *d)
{
    emu_core_mds_a()->destroy(&d->base);
}

static void lifecycle_rejects_bad_images(void)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(emu_core_mds_a()->create(&c), EMU_OK);
    T_CHECK_EQ(emu_core_mds_a()->load_rom(c, NULL, 10), EMU_EINVAL);
    static const uint8_t bad[0x400] = { 0 };
    T_CHECK_EQ(emu_core_mds_a()->load_rom(c, bad, sizeof bad), EMU_EBADROM);
    T_CHECK_EQ(emu_core_mds_a()->run_frame(c), EMU_ENOROM);
    emu_core_mds_a()->destroy(c);
}

static void boot_runs_arm9_marker(void)
{
    /*  mov r2, #0x34
     *  add r2, r2, #0x02000000 (imm 2, rot 4: 2 ror 8 = 0x02000000)
     *  mov r1, #0x44
     *  str r1, [r2]
     *  b .
     */
    static const uint32_t arm9[] = {
        0xE3A02034u,
        0xE2822402u,
        0xE3A01044u,
        0xE5821000u,
        0xEAFFFFFEu,
    };
    static const uint32_t arm7[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 5, arm7, 1);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    T_CHECK_EQ(emu_core_mds_a()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ_U(d->a9.cpu.cpsr & ARM_F_MODE, ARM_MODE_SYS);
    T_CHECK((d->a9.cpu.cpsr & ARM_F_I) != 0);
    /* str landed at 0x02000034 */
    T_CHECK_EQ_U(ds9_read32(d, 0x02000034u), 0x44u);
    ds_free(d);
}

static void arm_alu_and_flags(void)
{
    /*  mov r0, #0xFF
     *  add r0, r0, #0x100
     *  mov r1, #0x200
     *  add r3, r0, r1
     *  subs r4, r1, r3
     *  subs r5, r3, r1
     *  mov r6, #1
     *  mov r6, r6, ror #2
     *  ldr r7, [pc, #12]
     *  b .
     *  .word 0x12345678
     */
    static const uint32_t arm9[] = {
        0xE3A000FFu,
        0xE2800C01u,
        0xE3A01C02u,
        0xE0803001u,
        0xE0514003u,
        0xE0535001u,
        0xE3A06001u,
        0xE1A06166u,
        0xE59F7000u,
        0xEAFFFFFEu,
        0x12345678u,
    };
    struct ds *d = ds_boot_prog(arm9, 11, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    for (int i = 0; i < 5; i++)
        ds9_step(d);
    T_CHECK_EQ_U(d->a9.cpu.r[0], 0x1FFu);
    T_CHECK_EQ_U(d->a9.cpu.r[1], 0x200u);
    T_CHECK_EQ_U(d->a9.cpu.r[3], 0x3FFu);
    /* subs r4, r1, r3: 0x200 - 0x3FF = -0x1FF, borrow: C=0, N=1 */
    T_CHECK_EQ_U(d->a9.cpu.r[4], 0xFFFFFE01u);
    T_CHECK((d->a9.cpu.cpsr & ARM_F_N) != 0);
    T_CHECK((d->a9.cpu.cpsr & ARM_F_C) == 0);
    ds9_step(d); /* subs r5, r3, r1: no borrow: C=1, N=0 */
    T_CHECK_EQ_U(d->a9.cpu.r[5], 0x1FFu);
    T_CHECK((d->a9.cpu.cpsr & ARM_F_C) != 0);
    T_CHECK((d->a9.cpu.cpsr & ARM_F_N) == 0);
    ds9_step(d); /* mov r6, #1 */
    ds9_step(d); /* mov r6, r6, ror #2 -> 0x40000000 (S=0: flags kept) */
    T_CHECK_EQ_U(d->a9.cpu.r[6], 0x40000000u);
    T_CHECK((d->a9.cpu.cpsr & ARM_F_C) != 0);
    ds9_step(d); /* ldr r7, [pc, #0] */
    T_CHECK_EQ_U(d->a9.cpu.r[7], 0x12345678u);
    ds_free(d);
}

static void arm_64bit_mul(void)
{
    /*  mov r0, #0x80000000
     *  mov r1, #4
     *  mov r2, #0
     *  mov r3, #0
     *  umull r2, r3, r1, r0
     *  b .
     *  0x80000000 * 4 = 0x2_00000000
     */
    static const uint32_t arm9[] = {
        0xE3A00102u,
        0xE3A01004u,
        0xE3A02000u,
        0xE3A03000u,
0xE0932190u,
        0xEAFFFFFEu,
    };
    struct ds *d = ds_boot_prog(arm9, 6, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    for (int i = 0; i < 5; i++)
        ds9_step(d);
    T_CHECK_EQ_U(d->a9.cpu.r[2], 0x00000000u);
    T_CHECK_EQ_U(d->a9.cpu.r[3], 0x00000002u);
    ds_free(d);
}

static void thumb_ops_and_bx(void)
{
    /* ARM prologue at 0x02000000:
     *   ldr r0, [pc, #4]   ; thumb entry | 1
     *   bx r0
     *   b .
     *   .word 0x02000101
     */
    static const uint32_t arm9[] = {
        0xE59F0004u,
        0xE12FFF10u,
        0xEAFFFFFEu,
        0x02000101u,
    };
    struct ds *d = ds_boot_prog(arm9, 4, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* Thumb program at 0x02000100:
     *   mov r0, #16
     *   mov r1, #4
     *   adds r0, r0, r1      ; 20
     *   lsl r0, r0, #18      ; 0x500000
     *   ldr r2, [pc, #8]     ; -> literal at 0x02000114
     *   str r0, [r2]
     *   b .
     *   (pad)
     *   .word 0x02000034 (literal at 0x02000114)
     */
    uint8_t *prog = d->main_ram + (0x02000100u - 0x02000000u);
    w16(prog, 0, 0x2010u);
    w16(prog, 2, 0x2104u);
    w16(prog, 4, 0x1840u);
    w16(prog, 6, 0x0480u);
    w16(prog, 8, 0x4A02u);
    w16(prog, 10, 0x6010u);
    w16(prog, 12, 0xE7FEu);
    w32(prog, 0x14u, 0x02000034u);

    ds9_step(d); /* ldr r0 */
    ds9_step(d); /* bx r0  */
    T_CHECK((d->a9.cpu.cpsr & ARM_F_T) != 0);
    T_CHECK_EQ_U(d->a9.cpu.pc, 0x02000100u);
    for (int i = 0; i < 6; i++)
        ds9_step(d);
    T_CHECK_EQ_U(d->a9.cpu.r[0], 20u * 262144u);
    T_CHECK_EQ_U(ds9_read32(d, 0x02000034u), 20u * 262144u);
    ds_free(d);
}

static void exception_entry_swi(void)
{
    /*  swi 0x123456            ; 0x02000000
     *  nop                     ; 0x02000004 (return target)
     *  b .                     ; 0x02000008
     */
    static const uint32_t arm9[] = {
        0xEF123456u,
        0xE1A00000u,
        0xEAFFFFFEu,
    };
    struct ds *d = ds_boot_prog(arm9, 3, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* SWI handler at the ITCM low-vector mirror: subs pc, lr, #0
     * (MOVS PC, LR - the SWI return convention) */
    w32(d->itcm, 0x08u, 0xE25EF000u);
    ds9_step(d);
    T_CHECK_EQ_U(d->a9.cpu.cpsr & ARM_F_MODE, ARM_MODE_SVC);
    T_CHECK((d->a9.cpu.cpsr & ARM_F_I) != 0);
    T_CHECK_EQ_U(d->a9.cpu.r[14], 0x02000004u);
    T_CHECK_EQ_U(d->a9.cpu.pc, 0x08u);
    ds9_step(d);
    T_CHECK_EQ_U(d->a9.cpu.pc, 0x02000004u);
    T_CHECK_EQ_U(d->a9.cpu.cpsr & ARM_F_MODE, ARM_MODE_SYS);
    ds_free(d);
}

static void timer_counts_and_irq(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* TM0: reload 0xFFFB (wrap every 5 counts), prescaler 1, IRQ on.
     * DS timers count UP from the reload value (GBA-compatible). */
    d->a9.tm[0].reload = 0xFFFBu;
    d->a9.tm[0].counter = 0xFFFBu;
    d->a9.tm[0].cnt = 0x00C0u;
    d->a9.tm[0].acc = 0;
    uint64_t irq = 0;
    ds_tick_timers(&d->a9, d, 3, &irq);
    T_CHECK_EQ_U(d->a9.tm[0].counter, 0xFFFEu);
    T_CHECK_EQ_U(irq, 0);
    /* 8 more cycles: 11 total > 5 -> overflowed twice */
    ds_tick_timers(&d->a9, d, 8, &irq);
    T_CHECK_EQ_U(irq & DS_IRQ_TM0, DS_IRQ_TM0);
    T_CHECK_EQ_U(d->a9.tm[0].counter, 0xFFFCu);
    /* cascade: TM1 counts TM0 overflows */
    d->a9.tm[0].reload = 0xFFFBu;
    d->a9.tm[0].counter = 0xFFFBu;
    d->a9.tm[0].acc = 0;
    d->a9.tm[1].reload = 0xFFF0u;
    d->a9.tm[1].counter = 0xFFF0u;
    d->a9.tm[1].cnt = 0x00C4u; /* cascade | IRQ | enable */
    d->a9.tm[1].acc = 0;
    irq = 0;
    ds_tick_timers(&d->a9, d, 6, &irq);
    T_CHECK_EQ_U(irq & DS_IRQ_TM0, DS_IRQ_TM0);
    T_CHECK_EQ_U(d->a9.tm[1].counter, 0xFFF1u);
    ds_free(d);
}

static void ipc_sync_cross(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    static const uint32_t arm7[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, arm7, 1);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* ARM9 sends 0x5 with the send-IRQ bit; ARM7 has its enable bit set */
    d->ipc7 = 0x4000u;
    ds9_write32(d, 0x04000180u, 0x4520u);
    T_CHECK_EQ_U(ds7_read32(d, 0x04000180u) & 0xFu, 0x5u);
    T_CHECK((d->a7.if_latch & DS_IRQ_IPC) != 0);
    ds_free(d);
}

static void engine_a_mode3_scanout(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* mode 3: BG3 = extended affine, format 3 (16bpp 256x256), base 0,
     * identity transform (center reference = 128 << 8) */
    w32(d->a9.io, 0x000u, 0x0803u); /* DISPCNT: mode 3, BG3 enable */
    w16(d->a9.io, 0x00Eu, 0x000Cu); /* BG3CNT: format 3, screen base 0 */
    w16(d->a9.io, 0x030u, 0x0100u); /* BG3PA = 1.0 */
    w16(d->a9.io, 0x036u, 0x0100u); /* BG3PD = 1.0 */
    w32(d->a9.io, 0x038u, 0x8000u); /* BG3X = 128 << 8 */
    w32(d->a9.io, 0x03Cu, 0x8000u); /* BG3Y = 128 << 8 */
    for (uint32_t y = 0; y < 192u; y++)
        for (uint32_t x = 0; x < 256u; x++)
            w16(d->vram_a, y * 512u + x * 2u, 0x7C1Fu);
    ds_render(d);
    /* BGR555 0x7C1F: r=31 g=0 b=31 -> (v<<3)|(v>>2) = 255 per channel */
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(255, 0, 255));
    T_CHECK_EQ_U(d->fb[192u * 256u], 0xFF000000u); /* engine B blank */

    /* ext format 2 (16bpp 128x128) on BG3 */
    w16(d->a9.io, 0x00Eu, 0x0008u); /* BG3CNT: format 2, base 0 */
    w16(d->vram_a, 2u, 0x03E0u);    /* tex (1,0) */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[1], EMU_PIXEL(0, 255, 0));

    /* mode 5: BG2 extended bitmap; the page bit shifts the base by the
     * documented 0xA000 core-convention stride */
    w32(d->a9.io, 0x000u, 0x0415u); /* mode 5, BG2 enable, page 1 */
    w16(d->a9.io, 0x00Cu, 0x000Cu); /* BG2CNT: format 3, base 0 */
    w16(d->a9.io, 0x020u, 0x0100u); /* BG2PA = 1.0 */
    w16(d->a9.io, 0x026u, 0x0100u); /* BG2PD = 1.0 */
    w32(d->a9.io, 0x028u, 0x8000u); /* BG2X = 128 << 8 */
    w32(d->a9.io, 0x02Cu, 0x8000u); /* BG2Y = 128 << 8 */
    for (uint32_t y = 0; y < 160u; y++) /* page 1 fits below 128 KiB */
        for (uint32_t x = 0; x < 256u; x++)
            w16(d->vram_a, 0xA000u + y * 512u + x * 2u, 0x03E0u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[1], EMU_PIXEL(0, 255, 0));
    ds_free(d);
}

static void run_frame_advances_vcount(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    static const uint32_t arm7[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, arm7, 1);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    T_CHECK_EQ(emu_core_mds_a()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ_U(d->vcount, 0u);
    T_CHECK_EQ_U(d->frame_count, 1u);
    T_CHECK((d->a9.if_latch & DS_IRQ_VBLANK) != 0);
    T_CHECK((d->a7.if_latch & DS_IRQ_VBLANK) != 0);
    ds_free(d);
}

static void keypad_active_low(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* input mask: A=0 B=1 Up=6 -> 0x043; KEYINPUT active-low */
    emu_core_mds_a()->set_input(&d->base, 0x043u);
    T_CHECK_EQ_U(ds9_read32(d, 0x04000130u) & 0xFFFu, (~0x043u) & 0xFFFu);
    emu_core_mds_a()->set_input(&d->base, 0u);
    T_CHECK_EQ_U(ds9_read32(d, 0x04000130u) & 0xFFFu, 0xFFFu);
    ds_free(d);
}

static void state_roundtrip_resumes(void)
{
    /*  mov r0, #5
     *  add r0, r0, #3
     *  b .
     */
    static const uint32_t arm9[] = {
        0xE3A00005u,
        0xE2800003u,
        0xEAFFFFFEu,
    };
    static const uint32_t arm7[] = { 0xEAFFFFFEu };
    struct ds *ref = ds_boot_prog(arm9, 3, arm7, 1);
    struct ds *cmp = ds_boot_prog(arm9, 3, arm7, 1);
    if (ref == NULL || cmp == NULL) {
        T_FAIL("boot failed");
        return;
    }
    ds9_step(ref);
    ds9_step(ref);
    ds9_step(cmp);
    size_t sz = emu_core_mds_a()->state_size(&ref->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob == NULL) {
        ds_free(ref);
        ds_free(cmp);
        return;
    }
    T_CHECK_EQ(emu_core_mds_a()->save_state(&ref->base, blob, sz), EMU_OK);
    T_CHECK_EQ(emu_core_mds_a()->load_state(&cmp->base, blob, sz), EMU_OK);
    free(blob);
    ds9_step(ref);
    ds9_step(cmp);
    T_CHECK_EQ_U(ref->a9.cpu.pc, cmp->a9.cpu.pc);
    T_CHECK_EQ_U(ref->a9.cpu.r[0], cmp->a9.cpu.r[0]);
    T_CHECK_EQ_U(ref->a9.cpu.cpsr, cmp->a9.cpu.cpsr);
    /* undersized buffer rejected, nothing written */
    uint8_t tiny[8];
    T_CHECK_EQ(emu_core_mds_a()->save_state(&ref->base, tiny, sizeof tiny),
               EMU_ENOSPACE);
    ds_free(ref);
    ds_free(cmp);
}

static void v5te_clz_and_qadd(void)
{
    /*  mov r0, #1
     *  clz r1, r0           -> 31
     *  mov r0, #0x40
     *  mov r1, #0x40
     *  qadd r0, r1, r0      -> 0x80 (no saturation)
     *  mov r0, #0x80000000  (imm 1, rot 2? 1 ror 2 = 0x40000000; use
     *                        mvn r0, #0 -> 0xFFFFFFFF then mov r0,r0,lsr#1?
     *                        simplest: mvn r0, #0 ; then qadd saturates)
     *  mov r0, #0x80000000
     *  qadd r0, r0, r0      -> INT_MIN + INT_MIN saturates to
     *                          0x80000000, Q set
     *  b .
     */
    static const uint32_t arm9[] = {
        0xE3A00001u,
        0xE161FF10u,
        0xE3A00040u,
        0xE3A01040u,
        0xE1000150u,
        0xE3A00102u,
        0xE1000050u,
        0xEAFFFFFEu,
    };
    struct ds *d = ds_boot_prog(arm9, 8, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    ds9_step(d);
    ds9_step(d);
    T_CHECK_EQ_U(d->a9.cpu.r[1], 31u);
    ds9_step(d);
    ds9_step(d);
    ds9_step(d); /* qadd r0, r1, r0 */
    T_CHECK_EQ_U(d->a9.cpu.r[0], 0x80u);
    ds9_step(d); /* mov r0, #0x80000000 */
    T_CHECK_EQ_U(d->a9.cpu.r[0], 0x80000000u);
    ds9_step(d); /* qadd r0, r0, r0 */
    T_CHECK_EQ_U(d->a9.cpu.r[0], 0x80000000u);
    T_CHECK((d->a9.cpu.cpsr & ARM_F_Q) != 0);
    ds_free(d);
}

/* ---- 2D compositing tests (expected values hand-computed from the
 * documented register/tile/map formats, never from emulator internals) ---- */

static void text_bg_tiles_and_scroll(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* mode 0, BG0 on: 16-color text BG, char base 0, screen base 1 */
    w32(d->a9.io, 0x000u, 0x0100u);
    w16(d->a9.io, 0x008u, 0x0100u);
    /* BG palette: entry 1 magenta, entry 33 (bank 2, idx 1) blue-ish */
    w16(d->pal, 2u, 0x7C1Fu);
    w16(d->pal, 66u, 0x0C00u); /* BGR555 0x0C00: b field (bits 10-14) = 3 */
    /* tile 1: pixel (0,0) = 1 (4bpp pixel 0 = low nibble), rest transparent */
    d->vram_a[32] = 0x01;
    /* map entries at screen base 1 (0x800) */
    w16(d->vram_a, 0x800u, 0x0001u); /* tile 1                    */
    w16(d->vram_a, 0x802u, 0x0401u); /* tile 1, hflip             */
    w16(d->vram_a, 0x804u, 0x0801u); /* tile 1, vflip             */
    w16(d->vram_a, 0x806u, 0x2001u); /* tile 1, palette bank 2 */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(255, 0, 255));
    T_CHECK_EQ_U(d->fb[1], 0xFF000000u);
    T_CHECK_EQ_U(d->fb[15], EMU_PIXEL(255, 0, 255)); /* hflip mirror */
    T_CHECK_EQ_U(d->fb[8], 0xFF000000u);
    T_CHECK_EQ_U(d->fb[7u * 256u + 16u], EMU_PIXEL(255, 0, 255));
    T_CHECK_EQ_U(d->fb[16], 0xFF000000u);
    T_CHECK_EQ_U(d->fb[24], EMU_PIXEL(0, 0, 24)); /* bank 2 */

    /* scroll: HOFS = 1 shifts sampling one pixel right */
    w16(d->a9.io, 0x010u, 1u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], 0xFF000000u);
    T_CHECK_EQ_U(d->fb[14], EMU_PIXEL(255, 0, 255));
    w16(d->a9.io, 0x010u, 0u);

    /* 8bpp: same map, tile 2, palette = raw index */
    w16(d->a9.io, 0x008u, 0x0180u);
    w16(d->vram_a, 0x800u, 0x0002u);
    d->vram_a[128] = 5u; /* 8bpp tile 2 pixel (0,0) */
    w16(d->pal, 10u, 0x03E0u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(0, 255, 0));
    T_CHECK_EQ_U(d->fb[1], 0xFF000000u);
    ds_free(d);
}

static void bg_priority_and_backdrop(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* backdrop = BG palette entry 0 = white */
    w16(d->pal, 0u, 0x7FFFu);
    /* BG0: prio 1, screen base 2; tile 1 filled with color 1 (magenta) */
    w16(d->pal, 2u, 0x7C1Fu);
    w32(d->a9.io, 0x000u, 0x0300u); /* BG0 + BG1 on */
    w16(d->a9.io, 0x008u, 0x0201u); /* BG0CNT prio 1, base 2 */
    w16(d->a9.io, 0x00Au, 0x0300u); /* BG1CNT prio 0, base 3 */
    for (uint32_t i = 0; i < 32u; i++)
        d->vram_a[32u + i] = 0x11u;   /* tile 1: all color 1 */
    w16(d->vram_a, 0x1000u, 0x0001u); /* BG0 map (0,0) */
    /* BG1: prio 0 wins the tie; tile 2 pixel 0 transparent, rest color 2 */
    w16(d->pal, 4u, 0x001Fu);
    d->vram_a[64] = 0x20u;
    w16(d->vram_a, 0x1800u, 0x0002u); /* BG1 map (0,0) */
    ds_render(d);
    /* BG1 pixel (0,0) is transparent by design: BG0 (prio 1) shows through */
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(255, 0, 255));
    T_CHECK_EQ_U(d->fb[1], EMU_PIXEL(255, 0, 0));    /* BG1 opaque, prio 0 */
    /* equal priority: BG0 (lower layer number) wins over BG1 */
    w16(d->a9.io, 0x00Au, 0x0201u); /* BG1CNT prio 1 too */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(255, 0, 255)); /* BG0 magenta    */
    T_CHECK_EQ_U(d->fb[1], EMU_PIXEL(255, 0, 255));
    /* both layers off: backdrop white everywhere */
    w32(d->a9.io, 0x000u, 0x0000u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(255, 255, 255));
    T_CHECK_EQ_U(d->fb[40000u], EMU_PIXEL(255, 255, 255));
    ds_free(d);
}

static void obj_sprites(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* OBJ palette entry 1 magenta; OBJ tile 0 pixel (0,0) = 1.
     * All-zero OAM decodes as 128 sprites at (0,0) sharing tile 0 -
     * disable the unused slots first, as real software does. */
    w16(d->pal, 0x202u, 0x7C1Fu);
    d->vram_c[0] = 0x01;
    for (unsigned i = 0; i < 128u; i++)
        ds9_write32(d, 0x07000000u + i * 8u, 0x0200u); /* attr0: disabled */
    /* sprite 0: 8x8 4bpp 2D, (10,5), prio 0, palette 0 */
    ds9_write32(d, 0x07000000u, 0x000A0005u); /* attr0=y5 attr1=x10 */
    ds9_write32(d, 0x07000004u, 0x00000000u); /* attr2: tile 0      */
    /* sprite 1: same tile, hflip, at (20,5) */
    ds9_write32(d, 0x07000008u, 0x10140005u);
    ds9_write32(d, 0x0700000Cu, 0x00000000u);
    /* sprite 2: disabled (attr0 bit 9, no rot/scale) */
    ds9_write32(d, 0x07000010u, 0x001E0205u);
    ds9_write32(d, 0x07000014u, 0x00000000u);

    w32(d->a9.io, 0x000u, 0x1000u); /* OBJ on, 2D mapping */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[5u * 256u + 10u], EMU_PIXEL(255, 0, 255));
    T_CHECK_EQ_U(d->fb[5u * 256u + 11u], 0xFF000000u);
    T_CHECK_EQ_U(d->fb[4u * 256u + 10u], 0xFF000000u);
    T_CHECK_EQ_U(d->fb[5u * 256u + 27u], EMU_PIXEL(255, 0, 255)); /* hflip */
    T_CHECK_EQ_U(d->fb[5u * 256u + 20u], 0xFF000000u);
    T_CHECK_EQ_U(d->fb[5u * 256u + 30u], 0xFF000000u); /* disabled */

    /* OBJ vs BG priority: sprites win ties at equal priority */
    w16(d->pal, 2u, 0x7C1Fu);        /* BG palette entry 1 magenta */
    w16(d->pal, 0x202u, 0x03E0u);    /* OBJ palette entry 1 green  */
    for (uint32_t i = 0; i < 32u; i++)
        d->vram_a[32u + i] = 0x11u;   /* BG tile 1: all magenta */
    w16(d->vram_a, 0x800u, 0x0001u);
    w16(d->a9.io, 0x008u, 0x0101u); /* BG0CNT prio 1, screen base 1 */
    w32(d->a9.io, 0x000u, 0x1100u); /* BG0 + OBJ */
    /* sprite 3: 8x8 at (0,0), tile 0, prio 1 -> tie with BG0 */
    ds9_write32(d, 0x07000018u, 0x00000000u);
    ds9_write32(d, 0x0700001Cu, 0x00000400u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(0, 255, 0));    /* OBJ wins tie */
    T_CHECK_EQ_U(d->fb[1], EMU_PIXEL(255, 0, 255));  /* BG behind it */
    /* drop the sprite to prio 2: BG (prio 1) takes over */
    ds9_write32(d, 0x0700001Cu, 0x00000800u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(255, 0, 255));
    /* backdrop (BG palette entry 0) shows where nothing covers */
    w16(d->pal, 0u, 0x7FFFu);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[8], EMU_PIXEL(255, 255, 255));

    /* 1D vs 2D tile mapping: 16x16 sprite, tile 4, row-1 pixel differs */
    d->vram_c[160] = 0x01;  /* 1D tile (1,0): 4*32 + 1*32 */
    d->vram_c[192] = 0x01;  /* 1D tile (0,1): 4*32 + 1*2*32 */
    w16(d->pal, 0x202u, 0x03E0u);
    ds9_write32(d, 0x07000020u, 0x40280028u); /* y=40 x=40 size=1 */
    ds9_write32(d, 0x07000024u, 0x00000004u); /* tile 4, prio 0   */
    w32(d->a9.io, 0x000u, 0x3000u); /* OBJ only, 1D mapping */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[40u * 256u + 48u], EMU_PIXEL(0, 255, 0));
    T_CHECK_EQ_U(d->fb[48u * 256u + 40u], EMU_PIXEL(0, 255, 0));
    /* 2D: tile (0,1) comes from n = 4 + 32 -> byte 1152, not 192 */
    d->vram_c[192] = 0;
    d->vram_c[1152] = 0x01;
    w32(d->a9.io, 0x000u, 0x1000u); /* OBJ only, 2D mapping */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[40u * 256u + 48u], EMU_PIXEL(0, 255, 0));
    T_CHECK_EQ_U(d->fb[48u * 256u + 40u], EMU_PIXEL(0, 255, 0));

    /* 8bpp OBJ: full palette, attr2 pal field unused */
    d->vram_c[256] = 3u; /* 2D 8bpp tile 8 pixel (0,0) = 3 */
    w16(d->pal, 0x206u, 0x001Fu);
    ds9_write32(d, 0x07000028u, 0x003C203Cu); /* attr0=y60+8bpp attr1=x60 */
    ds9_write32(d, 0x0700002Cu, 0x00000008u); /* tile 8 */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[60u * 256u + 60u], EMU_PIXEL(255, 0, 0));
    ds_free(d);
}

static void obj_rotation_scaling(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    w16(d->pal, 0x202u, 0x03E0u); /* OBJ palette entry 1 green */
    w32(d->a9.io, 0x000u, 0x1000u);
    for (unsigned i = 0; i < 128u; i++)
        ds9_write32(d, 0x07000000u + i * 8u, 0x0200u); /* disable all */
    /* sprite 0: 8x8, rot/scale set 0, at (10,10) */
    ds9_write32(d, 0x07000000u, 0x000A010Au);
    ds9_write32(d, 0x07000004u, 0x00000000u);
    /* param set 0: identity (PA = PD = 1.0) */
    ds9_write32(d, 0x07000300u, 0x00000100u); /* PA=0x100 PB=0 */
    ds9_write32(d, 0x07000304u, 0x01000000u); /* PC=0    PD=0x100 */
    d->vram_c[0] = 0x01; /* OBJ tile 0 pixel (0,0) */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[10u * 256u + 10u], EMU_PIXEL(0, 255, 0));
    T_CHECK_EQ_U(d->fb[10u * 256u + 11u], 0xFF000000u);

    /* 2x shrink (PA = PD = 0.5): screen (10,10) and (11,10) both sample
     * texture (2,2)  [rx=-4 -> 128*-4>>8 = -2 -> +4 = 2] */
    ds9_write32(d, 0x07000300u, 0x00800000u);
    ds9_write32(d, 0x07000304u, 0x00800000u);
    d->vram_c[0] = 0;
    d->vram_c[9] = 0x01; /* tile 0 pixel (2,2) */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[10u * 256u + 10u], EMU_PIXEL(0, 255, 0));
    T_CHECK_EQ_U(d->fb[10u * 256u + 11u], EMU_PIXEL(0, 255, 0));

    /* 90 degree rotation (PA=0 PB=-1.0 PC=1.0 PD=0): screen (10,17)
     * maps to texture (1,0) [rx=-4 ry=3: tx = -3+4, ty = -4+4] */
    ds9_write32(d, 0x07000300u, 0xFF000000u);
    ds9_write32(d, 0x07000304u, 0x00000100u);
    d->vram_c[9] = 0;
    d->vram_c[0] = 0x10; /* tile 0 pixel (1,0) = high nibble */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[17u * 256u + 10u], EMU_PIXEL(0, 255, 0));
    ds_free(d);
}

static void affine_bg_transform(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* mode 1, BG2 affine (8bpp tiles), char base 0, screen base 1 */
    w32(d->a9.io, 0x000u, 0x0401u);
    w16(d->a9.io, 0x00Cu, 0x0100u);
    w16(d->pal, 10u, 0x03E0u); /* BG pal 5 green */
    w16(d->pal, 12u, 0x001Fu); /* BG pal 6 red   */
    d->vram_a[0] = 5u;         /* tile 0 pixel (0,0) */
    d->vram_a[64] = 6u;        /* tile 1 pixel (0,0) */
    d->vram_a[0x800u] = 0u;    /* affine map (byte entries): tile 0 */
    /* identity: PA = PD = 1.0, center reference 128 << 8 */
    w16(d->a9.io, 0x020u, 0x0100u);
    w16(d->a9.io, 0x026u, 0x0100u);
    w32(d->a9.io, 0x028u, 0x8000u);
    w32(d->a9.io, 0x02Cu, 0x8000u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(0, 255, 0));

    /* 2x shrink: center reference for a 128px map = 64 << 8; the screen
     * center then samples tex (64,64) = tile (8,8) of the map */
    w16(d->a9.io, 0x020u, 0x0080u);
    w16(d->a9.io, 0x026u, 0x0080u);
    w32(d->a9.io, 0x028u, 0x4000u);
    w32(d->a9.io, 0x02Cu, 0x4000u);
    d->vram_a[0x800u + 8u * 16u + 8u] = 1u; /* map (8,8) = tile 1 */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[128u * 256u + 128u], EMU_PIXEL(255, 0, 0));

    /* reference 0, no wrap: sample falls outside -> backdrop (black) */
    w16(d->a9.io, 0x020u, 0x0100u);
    w16(d->a9.io, 0x026u, 0x0100u);
    w32(d->a9.io, 0x028u, 0u);
    w32(d->a9.io, 0x02Cu, 0u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], 0xFF000000u);
    /* wraparound bit: tex -128 wraps to 0 -> tile 0 again */
    w16(d->a9.io, 0x00Cu, 0x2100u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(0, 255, 0));
    ds_free(d);
}

static void engine_b_registers(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* engine B registers live at 0x04001000 (ARM9) */
    ds9_write32(d, 0x04001000u, 0x0100u); /* DISPCNT B: mode 0, BG0 */
    ds9_write16(d, 0x04001008u, 0x0000u); /* BG0CNT B: base 0 */
    T_CHECK_EQ_U(ds9_read32(d, 0x04001000u) & 0xFFFFu, 0x0100u);
    w16(d->vram_b, 0u, 0x0001u);  /* map (0,0) = tile 1 */
    d->vram_b[32] = 0x01;         /* tile 1 pixel (0,0) */
    w16(d->pal, 0x402u, 0x7C1Fu); /* engine B BG palette entry 1 */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[192u * 256u], EMU_PIXEL(255, 0, 255));
    T_CHECK_EQ_U(d->fb[0], 0xFF000000u); /* engine A untouched */
    ds_free(d);
}

static void timer_registers_via_bus(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* TM0: reload 0xFFFB, enable + IRQ, prescaler 1 -> counter reloads */
    ds9_write32(d, 0x04000100u, 0x00C0FFFBu);
    T_CHECK_EQ_U(ds9_read32(d, 0x04000100u), 0x00C0FFFBu);
    uint64_t irq = 0;
    ds_tick_timers(&d->a9, d, 3, &irq);
    T_CHECK_EQ_U(ds9_read16(d, 0x04000100u), 0xFFFEu);
    T_CHECK_EQ_U(irq, 0);
    /* reload-halfword write alone must not reload the counter */
    ds9_write16(d, 0x04000100u, 0xFFF0u);
    T_CHECK_EQ_U(ds9_read16(d, 0x04000100u), 0xFFFEu);
    /* control write with the enable bit set reloads immediately */
    ds9_write16(d, 0x04000102u, 0x00C0u);
    T_CHECK_EQ_U(ds9_read16(d, 0x04000100u), 0xFFF0u);
    ds_tick_timers(&d->a9, d, 16, &irq); /* wrap after 16 counts */
    T_CHECK_EQ_U(irq & DS_IRQ_TM0, DS_IRQ_TM0);
    T_CHECK_EQ_U(ds9_read16(d, 0x04000100u), 0xFFF0u);
    /* ARM7 timers are wired the same way */
    ds7_write32(d, 0x04000100u, 0x00800005u);
    T_CHECK_EQ_U(ds7_read32(d, 0x04000100u), 0x00800005u);
    ds_free(d);
}

static void master_brightness(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* BG0 pixel magenta at (0,0); backdrop white */
    w32(d->a9.io, 0x000u, 0x0100u);
    w16(d->a9.io, 0x008u, 0x0100u);
    w16(d->pal, 0u, 0x7FFFu);
    w16(d->pal, 2u, 0x7C1Fu);
    d->vram_a[32] = 0x01;
    w16(d->vram_a, 0x800u, 0x0001u);
    /* brightness down, factor 21/63: 255 -> 255*42/63 = 170 */
    w16(d->a9.io, 0x06Cu, 0x4015u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(170, 0, 170));
    T_CHECK_EQ_U(d->fb[8], EMU_PIXEL(170, 170, 170));
    /* brightness up on a black screen: 0 -> 255*21/63 = 85 */
    w32(d->a9.io, 0x000u, 0x0000u);
    w16(d->pal, 0u, 0x0000u);
    w16(d->a9.io, 0x06Cu, 0x8015u);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(85, 85, 85));
    ds_free(d);
}

static void forced_blank(void)
{
    static const uint32_t arm9[] = { 0xEAFFFFFEu };
    struct ds *d = ds_boot_prog(arm9, 1, NULL, 0);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* mode 3 BG3 bitmap with visible data, then force blank */
    w32(d->a9.io, 0x000u, 0x8803u); /* forced blank */
    w16(d->a9.io, 0x00Eu, 0x000Cu);
    w16(d->a9.io, 0x030u, 0x0100u);
    w16(d->a9.io, 0x036u, 0x0100u);
    w32(d->a9.io, 0x038u, 0x8000u);
    w32(d->a9.io, 0x03Cu, 0x8000u);
    w16(d->vram_a, 0u, 0x7C1Fu);
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], 0xFF000000u);
    w32(d->a9.io, 0x000u, 0x0803u); /* blank off */
    ds_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(255, 0, 255));
    ds_free(d);
}

T_SUITE_BEGIN(ds)
{ "lifecycle_rejects_bad_images", lifecycle_rejects_bad_images },
{ "boot_runs_arm9_marker", boot_runs_arm9_marker },
{ "arm_alu_and_flags", arm_alu_and_flags },
{ "arm_64bit_mul", arm_64bit_mul },
{ "thumb_ops_and_bx", thumb_ops_and_bx },
{ "exception_entry_swi", exception_entry_swi },
{ "timer_counts_and_irq", timer_counts_and_irq },
{ "ipc_sync_cross", ipc_sync_cross },
{ "engine_a_mode3_scanout", engine_a_mode3_scanout },
{ "text_bg_tiles_and_scroll", text_bg_tiles_and_scroll },
{ "bg_priority_and_backdrop", bg_priority_and_backdrop },
{ "obj_sprites", obj_sprites },
{ "obj_rotation_scaling", obj_rotation_scaling },
{ "affine_bg_transform", affine_bg_transform },
{ "engine_b_registers", engine_b_registers },
{ "timer_registers_via_bus", timer_registers_via_bus },
{ "master_brightness", master_brightness },
{ "forced_blank", forced_blank },
{ "run_frame_advances_vcount", run_frame_advances_vcount },
{ "keypad_active_low", keypad_active_low },
{ "state_roundtrip_resumes", state_roundtrip_resumes },
{ "v5te_clz_and_qadd", v5te_clz_and_qadd },
T_SUITE_END
T_SUITE_REG(ds)
