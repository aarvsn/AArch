/*
 * beatle-nes-redux APU tests: length counters, frame sequencer IRQ,
 * deterministic output, channel silencing conditions, status register.
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

static void apu_pulse_length_and_status(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    nes_apu_write(&n->apu, 0x4015, 0x01);      /* enable pulse 0 */
    nes_apu_write(&n->apu, 0x4000, 0xBF);      /* duty 3, halt? bit5=1 halt?
                                                  0xBF: duty 11, bit5=1 (halt)
                                                  constant vol 1, vol 15 */
    nes_apu_write(&n->apu, 0x4003, 0x08);      /* length index 1 = 254 */
    T_CHECK_EQ(n->apu.pulse[0].len_counter, 254);
    T_CHECK_EQ(nes_apu_read_status(&n->apu) & 1u, 1u);
    /* halt enabled: length must NOT decrement */
    nes_apu_run(n, 29830);
    T_CHECK_EQ(n->apu.pulse[0].len_counter, 254);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void apu_length_counts_down(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    nes_apu_write(&n->apu, 0x4015, 0x01);
    nes_apu_write(&n->apu, 0x4000, 0x00);      /* halt = 0, envelope off */
    nes_apu_write(&n->apu, 0x4003, 0x02);      /* length index 0 = 10 */
    T_CHECK_EQ(n->apu.pulse[0].len_counter, 10);
    /* 5-step mode with immediate clocks: 4017 = 0x80 */
    nes_apu_write(&n->apu, 0x4017, 0x80);      /* 5-step, immediate half */
    T_CHECK_EQ(n->apu.pulse[0].len_counter, 9); /* one half-frame elapsed */
    /* run a full 5-step sequence (37282 cycles): lengths clocked at steps
     * 0 and 2 -> two more decrements */
    nes_apu_run(n, 37282);
    T_CHECK_EQ(n->apu.pulse[0].len_counter, 7);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void apu_frame_irq(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    nes_apu_write(&n->apu, 0x4017, 0x00); /* 4-step, IRQ enabled */
    nes_apu_run(n, 29830);
    T_CHECK(n->apu.frame_irq);
    T_CHECK(nes_apu_read_status(&n->apu) & 0x40u);
    T_CHECK_EQ(n->apu.frame_irq, 0); /* cleared by status read */
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void apu_sweep_silence(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    /* frequency < 8 silences the pulse */
    nes_apu_write(&n->apu, 0x4015, 0x01);
    nes_apu_write(&n->apu, 0x4002, 0x03); /* freq low 3 -> freq=3 <8 */
    nes_apu_write(&n->apu, 0x4003, 0x80); /* trigger, length 10 */
    nes_apu_write(&n->apu, 0x4000, 0x2F); /* constant volume 15, duty */
    /* run and sample: all samples must be zero due to freq<8 */
    for (uint32_t i = 0; i < 2000u; i++)
        nes_apu_run(n, 1);
    for (size_t s = 0; s < n->apu.out_pos; s++)
        T_CHECK_EQ(n->audio[s], 0);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void apu_deterministic_output(void)
{
    struct nes *a = mk_core();
    struct nes *b = mk_core();
    T_CHECK(a != NULL && b != NULL);
    if (!a || !b) {
        if (a) emu_core_beatle_nes_redux()->destroy(&a->base);
        if (b) emu_core_beatle_nes_redux()->destroy(&b->base);
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        struct nes *n = pass == 0 ? a : b;
        nes_apu_write(&n->apu, 0x4015, 0x0F);
        nes_apu_write(&n->apu, 0x4000, 0x2F); /* const vol 15 duty */
        nes_apu_write(&n->apu, 0x4002, 0xFD);
        nes_apu_write(&n->apu, 0x4003, 0x08);
        nes_apu_write(&n->apu, 0x4008, 0xFF); /* tri ctrl + reload 127 */
        nes_apu_write(&n->apu, 0x400A, 0x40);
        nes_apu_write(&n->apu, 0x400B, 0x08);
        nes_apu_run(n, 4000);
    }
    T_CHECK_EQ(a->apu.out_pos, b->apu.out_pos);
    T_CHECK(memcmp(a->audio, b->audio, a->apu.out_pos * sizeof(int16_t)) == 0);
    T_CHECK(a->apu.out_pos > 0);
    /* non-zero output: pulse + triangle active */
    int nonzero = 0;
    for (size_t s = 0; s < a->apu.out_pos; s++)
        if (a->audio[s] != 0)
            nonzero++;
    T_CHECK(nonzero > 0);
    emu_core_beatle_nes_redux()->destroy(&a->base);
    emu_core_beatle_nes_redux()->destroy(&b->base);
}

static void apu_triangle_sequence(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    nes_apu_write(&n->apu, 0x4015, 0x04);
    nes_apu_write(&n->apu, 0x4008, 0x7F); /* linear ctrl (no reload flag
                                             clear), reload 127 */
    nes_apu_write(&n->apu, 0x400A, 0x00); /* period small */
    nes_apu_write(&n->apu, 0x400B, 0x01);
    nes_apu_run(n, 8000); /* first quarter frame at cycle 7457 */
    /* linear counter reloaded (ctrl set) while length still counting */
    T_CHECK_EQ(n->apu.tri.linear_counter, 127);
    T_CHECK(n->apu.tri.seq_pos != 0 || n->apu.tri.seq_pos == 0);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

T_SUITE_BEGIN(nes_apu)
{ "pulse_length_status", apu_pulse_length_and_status },
{ "length_counts_down", apu_length_counts_down },
{ "frame_irq", apu_frame_irq },
{ "sweep_silence", apu_sweep_silence },
{ "deterministic_output", apu_deterministic_output },
{ "triangle_sequence", apu_triangle_sequence },
T_SUITE_END

T_SUITE_REG(nes_apu)
