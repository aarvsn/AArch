/*
 * mgbax APU tests: direct-sound FIFO channels A/B, DMA refill path, sound
 * control registers, PSG routing/volume and noise output.
 *
 * Expected values are derived from the GBA specification:
 *  - FIFO: 32-byte depth, one byte popped per selected-timer overflow,
 *    DMA request when <= 16 bytes queued (4-word fixed-destination refill),
 *    channels 1/2 service FIFO A, channels 2/3 FIFO B.
 *  - PSG: SOUNDCNT_L per-side volume (v+1)/8 and per-channel routing,
 *    SOUNDCNT_H 25/50/100% master scale, SOUNDCNT_X bit7 master gate.
 *  - Noise: LFSR shift clock = 524288 Hz/(R+1)/2^S -> 32*(R+1)<<S cycles;
 *    LFSR restarts at 0x7FFF, shift inserts ~(b0 xor b1) at bit 15.
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

/* Enables timer 0, prescale 1, reload 0xFFFF -> one overflow every tick. */
static void timer0_every_cycle(struct gba *g)
{
    gba_timers_write(g, 0x04000100u, 0xFFFFu);
    gba_timers_write(g, 0x04000102u, 0x0080u);
}

/* The frame loop order: timers first, then the APU consumes overflows. */
static void tick(struct gba *g, uint32_t cycles)
{
    gba_timers_step(g, cycles);
    gba_apu_step(g, cycles);
}

static void fifo_pop_order(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* FIFO A, timer 0 select, L+R enabled, 100% volume */
    gba_io_write16(g, 0x04000082u, 0x0004u | 0x0100u | 0x0200u);
    /* append bytes 00 01 02 03 (little-endian word store) */
    gba_mem_write32(g, 0x040000A0u, 0x03020100u);
    T_CHECK_EQ(g->apu.fifo_count[0], 4u);
    T_CHECK_EQ(g->apu.fifo[0][0], 0x00u);
    T_CHECK_EQ(g->apu.fifo[0][3], 0x03u);

    timer0_every_cycle(g);
    tick(g, 1); /* overflow: pop byte 0x00 */
    T_CHECK_EQ(g->apu.fifo_count[0], 3u);
    T_CHECK_EQ(g->apu.fifo_has[0], 1u);
    T_CHECK_EQ(g->apu.fifo_cur[0], 0);
    tick(g, 1); /* pop 0x01 */
    T_CHECK_EQ(g->apu.fifo_cur[0], 1);
    tick(g, 1); /* pop 0x02 */
    T_CHECK_EQ(g->apu.fifo_cur[0], 2);
    tick(g, 1); /* pop 0x03 */
    T_CHECK_EQ(g->apu.fifo_cur[0], 3);
    T_CHECK_EQ(g->apu.fifo_count[0], 0u);
    tick(g, 1); /* empty FIFO -> silence */
    T_CHECK_EQ(g->apu.fifo_has[0], 0u);
    T_CHECK_EQ(g->apu.fifo_cur[0], 0);
    emu_core_mgbax()->destroy(&g->base);
}

static void fifo_timer_select(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* FIFO A on timer 0 (bit10=0), FIFO B on timer 1 (bit14=1) */
    gba_io_write16(g, 0x04000082u, 0x4004u);
    gba_mem_write32(g, 0x040000A0u, 0x0A0A0A0Au);
    gba_mem_write32(g, 0x040000A4u, 0x0B0B0B0Bu);
    /* timer 1: prescale 1, reload 0xFFFF -> overflow on first tick */
    gba_timers_write(g, 0x04000104u, 0xFFFFu);
    gba_timers_write(g, 0x04000106u, 0x0080u);
    tick(g, 1); /* timer 1 overflow pops FIFO B only */
    T_CHECK_EQ(g->apu.fifo_count[1], 3u);
    T_CHECK_EQ(g->apu.fifo_cur[1], 0x0B);
    T_CHECK_EQ(g->apu.fifo_count[0], 4u); /* FIFO A untouched */
    T_CHECK_EQ(g->apu.fifo_has[0], 0u);
    emu_core_mgbax()->destroy(&g->base);
}

static void fifo_full_discard(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_io_write16(g, 0x04000082u, 0x0004u);
    /* 9 word writes = 36 bytes; the hardware FIFO holds 32 */
    for (uint32_t i = 0; i < 9; i++)
        gba_mem_write32(g, 0x040000A0u, i * 0x01010101u);
    T_CHECK_EQ(g->apu.fifo_count[0], 32u);
    /* drain fully: 32 pops, then a 33rd yields silence */
    for (int i = 0; i < 32; i++) {
        g->timers.ovf_bits = 1;
        gba_apu_step(g, 1);
    }
    T_CHECK_EQ(g->apu.fifo_count[0], 0u);
    g->timers.ovf_bits = 1;
    gba_apu_step(g, 1);
    T_CHECK_EQ(g->apu.fifo_has[0], 0u);
    T_CHECK_EQ(g->apu.fifo_cur[0], 0);
    emu_core_mgbax()->destroy(&g->base);
}

static void fifo_reset_bits(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_io_write16(g, 0x04000082u, 0x0004u);
    gba_mem_write32(g, 0x040000A0u, 0xDEADBEEFu);
    gba_mem_write32(g, 0x040000A4u, 0x11223344u);
    T_CHECK_EQ(g->apu.fifo_count[0], 4u);
    T_CHECK_EQ(g->apu.fifo_count[1], 4u);
    /* FIFO A reset bit */
    gba_io_write16(g, 0x04000082u, 0x0800u | 0x0004u);
    T_CHECK_EQ(g->apu.fifo_count[0], 0u);
    T_CHECK_EQ(g->apu.fifo_has[0], 0u);
    T_CHECK_EQ(g->apu.fifo_count[1], 4u); /* FIFO B untouched */
    /* FIFO B reset bit */
    gba_io_write16(g, 0x04000082u, 0x8000u | 0x0004u);
    T_CHECK_EQ(g->apu.fifo_count[1], 0u);
    emu_core_mgbax()->destroy(&g->base);
}

static void fifo_dma_refill(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* source pattern in WRAM: 0,1,...,31 */
    for (int i = 0; i < 32; i++)
        gba_mem_write8(g, 0x02000000u + (uint32_t)i, (uint8_t)i);

    /* DMA ch1 -> FIFO A: word, src increment, dst fixed, trigger 3, enable.
     * ctrl = 0x8000 | (3<<12) | (1<<10) | (2<<5) = 0xB440 */
    gba_dma_write(g, 0x040000BCu, 0x0000u); /* SAD1 low */
    gba_dma_write(g, 0x040000BEu, 0x0200u); /* SAD1 high */
    gba_dma_write(g, 0x040000C0u, 0x00A0u); /* DAD1 = 0x040000A0 */
    gba_dma_write(g, 0x040000C2u, 0x0400u);
    gba_dma_write(g, 0x040000C4u, 0x0000u); /* count ignored for FIFO */
    gba_dma_write(g, 0x040000C6u, 0xB440u);

    gba_io_write16(g, 0x04000082u, 0x0004u);
    /* one CPU word write -> count 4 <= 16 -> DMA fills 4 words */
    gba_mem_write32(g, 0x040000A0u, 0xFFFFFFFFu);
    T_CHECK_EQ(g->apu.fifo_count[0], 20u);
    /* first 4 bytes are the CPU-written word, then the DMA pattern */
    T_CHECK_EQ(g->apu.fifo[0][0], 0xFFu);
    T_CHECK_EQ(g->apu.fifo[0][4], 0x00u);
    T_CHECK_EQ(g->apu.fifo[0][5], 0x01u);
    T_CHECK_EQ(g->apu.fifo[0][19], 0x0Fu);
    /* source advanced by 16 bytes */
    T_CHECK_EQ(g->dma.sad[1], 0x02000010u);
    /* FIFO DMA transfers never touch the DAD register */
    T_CHECK_EQ(g->dma.dad[1], 0x040000A0u);

    /* FIFO B: ch1 is not allowed to service it -> no refill, no SAD change */
    gba_mem_write32(g, 0x040000A4u, 0xA5A5A5A5u);
    T_CHECK_EQ(g->apu.fifo_count[1], 4u);
    T_CHECK_EQ(g->dma.sad[1], 0x02000010u);
    emu_core_mgbax()->destroy(&g->base);
}

static void fifo_output_routing_volumes(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->apu.out = g->audio;
    g->apu.out_cap = 2048;
    g->apu.out_pos = 0;
    timer0_every_cycle(g);

    /* FIFO A on timer 0, L+R enabled, 100% volume */
    gba_io_write16(g, 0x04000082u, 0x0004u | 0x0100u | 0x0200u);
    gba_mem_write32(g, 0x040000A0u, 0x40404040u); /* sample byte 0x40 = 64 */
    tick(g, 1);                                    /* one overflow: pop */
    T_CHECK_EQ(g->apu.fifo_cur[0], 0x40);
    gba_apu_step(g, 512 - 1); /* complete the output interval */
    T_CHECK(g->apu.out_pos >= 2);
    T_CHECK_EQ(g->audio[0], 64 * 256); /* 0x40 << 8 = 16384 */
    T_CHECK_EQ(g->audio[1], 64 * 256);

    /* right-only routing */
    gba_io_write16(g, 0x04000082u, 0x0004u | 0x0100u);
    g->apu.out_pos = 0;
    g->apu.sample_acc = 0;
    gba_mem_write32(g, 0x040000A0u, 0x40404040u);
    tick(g, 1);
    gba_apu_step(g, 512 - 1);
    T_CHECK_EQ(g->audio[0], 0);
    T_CHECK_EQ(g->audio[1], 64 * 256);

    /* 50% volume (bit 2 = 0) */
    gba_io_write16(g, 0x04000082u, 0x0100u);
    g->apu.out_pos = 0;
    g->apu.sample_acc = 0;
    gba_mem_write32(g, 0x040000A0u, 0x40404040u);
    tick(g, 1);
    gba_apu_step(g, 512 - 1);
    T_CHECK_EQ(g->audio[1], 64 * 128);

    /* negative 8-bit sample sign-extends: reset FIFO first, then fill */
    gba_io_write16(g, 0x04000082u, 0x0800u | 0x0004u | 0x0100u | 0x0200u);
    g->apu.out_pos = 0;
    g->apu.sample_acc = 0;
    gba_mem_write32(g, 0x040000A0u, 0x80808080u); /* byte 0x80 = -128 */
    tick(g, 1);
    gba_apu_step(g, 512 - 1);
    T_CHECK_EQ(g->audio[0], -128 * 256); /* -32768 */
    emu_core_mgbax()->destroy(&g->base);
}

static void psg_master_gate_and_readback(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->apu.out = g->audio;
    g->apu.out_cap = 2048;
    g->apu.out_pos = 0;

    /* square 1: duty 1 (waveform high at position 0), env vol 8, restart.
     * Per spec: duty/length at 0x60, envelope volume (4-bit) at 0x62
     * bits 12-15. */
    gba_io_write16(g, 0x04000060u, 1u << 6);
    gba_io_write16(g, 0x04000062u, 8u << 12);
    gba_io_write16(g, 0x04000064u, 0x8400u);
    gba_io_write16(g, 0x04000080u, 0x77FFu); /* vol 7 L/R, ch1 L+R on */
    gba_io_write16(g, 0x04000082u, 0x0002u); /* 100% */
    T_CHECK_EQ(gba_io_read16(g, 0x04000084u) & 0x0001u, 0x0001u);

    /* master disabled -> silence */
    gba_io_write16(g, 0x04000084u, 0x0000u);
    gba_apu_step(g, 512);
    T_CHECK_EQ(g->audio[0], 0);
    T_CHECK_EQ(g->audio[1], 0);

    /* master enabled -> volume 8, gain (7+1)/8 = 1 -> 8*1024 */
    gba_io_write16(g, 0x04000084u, 0x0080u);
    g->apu.out_pos = 0;
    g->apu.sample_acc = 0;
    gba_apu_step(g, 512);
    T_CHECK_EQ(g->audio[0], 8 * 1024);
    T_CHECK_EQ(g->audio[1], 8 * 1024);

    /* register readback */
    T_CHECK_EQ(gba_io_read16(g, 0x04000080u), 0x77FFu);
    T_CHECK_EQ(gba_io_read16(g, 0x04000082u), 0x0002u);
    emu_core_mgbax()->destroy(&g->base);
}

static void psg_volume_25_50_100(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->apu.out = g->audio;
    g->apu.out_cap = 2048;
    g->apu.out_pos = 0;
    gba_io_write16(g, 0x04000060u, 1u << 6);
    gba_io_write16(g, 0x04000062u, 8u << 12);
    gba_io_write16(g, 0x04000064u, 0x8400u);
    gba_io_write16(g, 0x04000080u, 0x77FFu);
    gba_io_write16(g, 0x04000084u, 0x0080u); /* master enable */

    gba_io_write16(g, 0x04000082u, 0x0000u); /* 25% */
    gba_apu_step(g, 512);
    T_CHECK_EQ(g->audio[0], 8 * 1024 / 4);

    gba_io_write16(g, 0x04000082u, 0x0001u); /* 50% */
    g->apu.out_pos = 0;
    g->apu.sample_acc = 0;
    gba_apu_step(g, 512);
    T_CHECK_EQ(g->audio[0], 8 * 1024 / 2);
    emu_core_mgbax()->destroy(&g->base);
}

static void psg_noise_output(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    g->apu.out = g->audio;
    g->apu.out_cap = 2048;
    g->apu.out_pos = 0;

    /* env vol 15; LFSR: R=0, S=0 -> period 32 cycles, 15-bit mode */
    gba_io_write16(g, 0x04000078u, 15u << 12);
    gba_io_write16(g, 0x0400007Cu, 0x8000u);
    T_CHECK_EQ(g->apu.noise_active, 1u);
    T_CHECK_EQ(g->apu.noise_period, 32u);
    T_CHECK_EQ(g->apu.noise_volume, 15u);
    T_CHECK_EQ(g->apu.noise_lfsr, 0x7FFFu);
    /* spec-derived walk from 0x7FFF with zero inserted at bit 15:
     * 0x7FFF, 0x3FFF, 0x1FFF, ... one shift per 32 cycles */
    gba_apu_step(g, 32);
    T_CHECK_EQ(g->apu.noise_lfsr, 0x3FFFu);
    gba_apu_step(g, 32);
    T_CHECK_EQ(g->apu.noise_lfsr, 0x1FFFu);
    gba_apu_step(g, 32);
    T_CHECK_EQ(g->apu.noise_lfsr, 0x0FFFu);
    gba_apu_step(g, 32); /* 4 shifts total: 0x07FF */
    T_CHECK_EQ(g->apu.noise_lfsr, 0x07FFu);

    /* after 16 shifts the LFSR is 0x4000 (bit0 = 0) -> channel audible */
    for (int i = 0; i < 12; i++)
        gba_apu_step(g, 32);
    T_CHECK_EQ(g->apu.noise_lfsr, 0x4000u);
    gba_io_write16(g, 0x04000080u, (1u << 11) | (1u << 15) | 0x77u);
    gba_io_write16(g, 0x04000082u, 0x0002u);
    gba_io_write16(g, 0x04000084u, 0x0080u);
    g->apu.out_pos = 0;
    g->apu.sample_acc = 480; /* next sample after 32 more cycles */
    gba_apu_step(g, 32);
    /* noise vol 15, master gain 1 -> 15*1024 on both sides */
    T_CHECK_EQ(g->audio[0], 15 * 1024);
    T_CHECK_EQ(g->audio[1], 15 * 1024);

    /* 7-step width mode mirrors the inserted bit into bit 7; restart first */
    gba_io_write16(g, 0x0400007Cu, 0x8008u);
    T_CHECK_EQ(g->apu.noise_lfsr, 0x7FFFu);
    gba_apu_step(g, 32); /* one shift: feedback bit 0 -> bit15 and bit7 */
    T_CHECK_EQ(g->apu.noise_lfsr, 0x3F7Fu); /* 0x3FFF with bit7 cleared */
    T_CHECK((g->apu.noise_lfsr & 0x0080u) == 0u);
    emu_core_mgbax()->destroy(&g->base);
}

static void fifo_determinism_across_frames(void)
{
    /* Two state-identical cores running the same FIFO configuration must
     * deliver byte-identical audio streams. */
    struct gba *g1 = mk_core(), *g2 = mk_core();
    T_CHECK(g1 != NULL && g2 != NULL);
    if (!g1 || !g2) {
        if (g1)
            emu_core_mgbax()->destroy(&g1->base);
        if (g2)
            emu_core_mgbax()->destroy(&g2->base);
        return;
    }
    for (int f = 0; f < 2; f++) {
        struct gba *g = f ? g2 : g1;
        g->apu.out = g->audio;
        g->apu.out_cap = 2048;
        g->apu.out_pos = 0;
        gba_io_write16(g, 0x04000082u, 0x0004u | 0x0100u | 0x0200u);
        gba_mem_write32(g, 0x040000A0u, 0x55AA33CCu);
        /* overflow every 128 cycles: the 4 queued samples spread over
         * the first 512-cycle output interval (pops at 128/256/384/512) */
        gba_timers_write(g, 0x04000100u, 0xFF80u);
        gba_timers_write(g, 0x04000102u, 0x0080u);
    }
    for (int i = 0; i < 4096; i++) {
        gba_timers_step(g1, 1);
        gba_timers_step(g2, 1);
        gba_apu_step(g1, 1);
        gba_apu_step(g2, 1);
    }
    {
        int nonzero = 0;
        for (int i = 0; i < 2048; i++) {
            T_CHECK_EQ(g1->audio[i], g2->audio[i]);
            if (g1->audio[i] != 0)
                nonzero++;
        }
        T_CHECK(nonzero > 0); /* the FIFO path produced actual samples */
    }
    emu_core_mgbax()->destroy(&g1->base);
    emu_core_mgbax()->destroy(&g2->base);
}

T_SUITE_BEGIN(gba_apu)
{ "fifo_pop_order", fifo_pop_order },
{ "fifo_timer_select", fifo_timer_select },
{ "fifo_full_discard", fifo_full_discard },
{ "fifo_reset_bits", fifo_reset_bits },
{ "fifo_dma_refill", fifo_dma_refill },
{ "fifo_output_routing_volumes", fifo_output_routing_volumes },
{ "psg_master_gate_and_readback", psg_master_gate_and_readback },
{ "psg_volume_25_50_100", psg_volume_25_50_100 },
{ "psg_noise_output", psg_noise_output },
{ "fifo_determinism_across_frames", fifo_determinism_across_frames },
T_SUITE_END

T_SUITE_REG(gba_apu)
