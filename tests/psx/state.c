/*
 * beatle-psx save-state contract tests: a state roundtrip must resume
 * bit-exactly (identical framebuffer CRCs and CPU state), and two cores
 * running the same program must produce identical results.
 */
#include "../tests.h"
#include "../testutil.h"
#include "psxh.h"
#include "beatle-psx/psx.h"

#include <stdlib.h>
#include <string.h>

/* Small deterministic program: fills r4..r7 with values, runs a multiply
 * loop, stores results, and issues a GTE command. */
static const uint32_t kDemoCode[] = {
    0x3C021234u, /* LUI  r2, 0x1234           */
    0x34425678u, /* ORI  r2, r2, 0x5678       */
    0x3C0300AAu, /* LUI  r3, 0x00AA           */
    0x346300BBu, /* ORI  r3, r3, 0x00BB       */
    0x00430018u, /* MULT r2, r3               */
    0x00001012u, /* MFLO r2                   */
    0x00001810u, /* MFHI r3                   */
    0x3C040100u, /* LUI  r4, 0x0100 (SP base) */
    0xAC820000u, /* SW   r2, 0(r4)            */
    0xAC830004u, /* SW   r3, 4(r4)            */
    0x3C0D0100u, /* LUI  r13, 0x0100          */
    0x34AD0010u, /* ORI  r13, r13, 0x10       */
    0x488D6800u, /* MTC2 r13, VXY0 (rgbc etc) */
    0x488D7001u, /* MTC2 r13, VZ0             */
    0x48A8001Fu, /* CTC2 r8, H                */
    0x4A180001u, /* RTPS                      */
    0x482F180Eu, /* MFC2 r15, SXY2            */
    0x08000012u, /* J 0x48 (self-loop region) */
    0x00000000u, /* NOP (delay slot)          */
};

static struct psx *boot_demo(void)
{
    size_t size;
    uint8_t *exe = psx_test_exe(0x80010000u, 0x80010000u, kDemoCode,
                                sizeof kDemoCode / 4u, 0, &size);
    struct psx *p = psx_boot(exe, size);
    free(exe);
    return p;
}

static void run_demo(struct psx *p, int steps)
{
    for (int i = 0; i < steps; i++)
        psx_cpu_step(&p->cpu);
}

static void psx_state_roundtrip(void)
{
    struct psx *a = boot_demo();
    T_CHECK(a != NULL);
    if (!a)
        return;
    run_demo(a, 40);

    size_t size = emu_core_beatle_psx()->state_size(&a->base);
    T_CHECK(size > 0);
    uint8_t *blob = malloc(size);
    T_CHECK(blob != NULL);
    if (!blob) {
        emu_core_beatle_psx()->destroy(&a->base);
        return;
    }
    emu_result_t r = emu_core_beatle_psx()->save_state(&a->base, blob, size);
    T_CHECK_EQ(r, EMU_OK);

    /* continue original */
    run_demo(a, 60);
    uint32_t fb_a[PSX_FB_W * PSX_FB_H];
    memcpy(fb_a, a->fb, sizeof fb_a);
    uint32_t regs_a[32];
    for (int i = 0; i < 32; i++)
        regs_a[i] = a->cpu.r[i];

    /* restore into a second core and run identically */
    struct psx *b = boot_demo();
    T_CHECK(b != NULL);
    if (!b) {
        free(blob);
        emu_core_beatle_psx()->destroy(&a->base);
        return;
    }
    r = emu_core_beatle_psx()->load_state(&b->base, blob, size);
    T_CHECK_EQ(r, EMU_OK);
    free(blob);
    run_demo(b, 60);
    uint32_t fb_b[PSX_FB_W * PSX_FB_H];
    memcpy(fb_b, b->fb, sizeof fb_b);

    T_CHECK(memcmp(fb_a, fb_b, sizeof fb_a) == 0);
    for (int i = 0; i < 32; i++)
        T_CHECK_EQ(b->cpu.r[i], regs_a[i]);
    T_CHECK_EQ(b->cpu.pc, a->cpu.pc);
    T_CHECK_EQ(b->cpu.next_pc, a->cpu.next_pc);
    T_CHECK_EQ(b->cpu.hi, a->cpu.hi);
    T_CHECK_EQ(b->cpu.lo, a->cpu.lo);

    emu_core_beatle_psx()->destroy(&a->base);
    emu_core_beatle_psx()->destroy(&b->base);
}

static void psx_state_two_cores_identical(void)
{
    struct psx *a = boot_demo();
    struct psx *b = boot_demo();
    T_CHECK(a != NULL && b != NULL);
    if (!a || !b) {
        if (a) emu_core_beatle_psx()->destroy(&a->base);
        if (b) emu_core_beatle_psx()->destroy(&b->base);
        return;
    }
    run_demo(a, 100);
    run_demo(b, 100);
    for (int i = 0; i < 32; i++)
        T_CHECK_EQ(b->cpu.r[i], a->cpu.r[i]);
    T_CHECK_EQ(b->cpu.pc, a->cpu.pc);
    emu_core_beatle_psx()->destroy(&a->base);
    emu_core_beatle_psx()->destroy(&b->base);
}

static void psx_state_undersized(void)
{
    struct psx *a = boot_demo();
    T_CHECK(a != NULL);
    if (!a)
        return;
    run_demo(a, 10);
    size_t size = emu_core_beatle_psx()->state_size(&a->base);
    uint8_t *blob = malloc(size);
    T_CHECK(blob != NULL);
    if (blob) {
        emu_result_t r =
            emu_core_beatle_psx()->save_state(&a->base, blob, size - 1u);
        T_CHECK_EQ(r, EMU_ENOSPACE);
        r = emu_core_beatle_psx()->save_state(&a->base, blob, size);
        T_CHECK_EQ(r, EMU_OK);
        /* corrupt magic */
        blob[0] ^= 0xFFu;
        r = emu_core_beatle_psx()->load_state(&a->base, blob, size);
        T_CHECK_EQ(r, EMU_EBADSTATE);
        free(blob);
    }
    emu_core_beatle_psx()->destroy(&a->base);
}

T_SUITE_BEGIN(beatle_psx_state)
{ "roundtrip_bit_exact", psx_state_roundtrip },
{ "two_cores_identical", psx_state_two_cores_identical },
{ "undersized_and_corrupt", psx_state_undersized },
T_SUITE_END

T_SUITE_REG(beatle_psx_state)
