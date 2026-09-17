/*
 * finalburn audio tests: PSG (SN76489) and YM2612 subsets.
 * Expectations derived from the SN76489 and YM2612 datasheets and the
 * documented 440 Hz tuning anchor (FNUM 1024, OCT 4, MULT 1).
 */
#include "tests.h"
#include "fbh.h"
#include "finalburn/fb_md.h"
#include "emu/emu.h"

#include <stdlib.h>
#include <string.h>

/* ---- PSG ---------------------------------------------------------------- */

static void test_psg_tone_toggle(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_psg *p = &md->psg;
    fb_psg_write(p, 0x82);  /* latch ch0 tone data, low nibble = 2 */
    fb_psg_write(p, 0x00);  /* tone high bits 0 -> N = 2 */
    fb_psg_write(p, 0x90);  /* latch ch0 volume, attenuation 0 (loudest) */
    /* N=2 -> the counter limit is N-1=1: output toggles every 2 ticks */
    int flips = 0;
    uint8_t last = p->tone_out[0];
    for (int i = 0; i < 40; i++) {
        fb_psg_tick(p);
        if (p->tone_out[0] != last) {
            flips++;
            last = p->tone_out[0];
        }
    }
    T_CHECK_EQ_U(flips, 20);
    T_CHECK(p->vol[0] == 0);
    emu_core_destroy(&md->base);
}

static void test_psg_volume_attenuation(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_psg *p = &md->psg;
    fb_psg_write(p, 0x9F); /* ch0 volume = 15 -> silent */
    fb_psg_write(p, 0x92);
    for (int i = 0; i < 100; i++)
        fb_psg_tick(p);
    T_CHECK_EQ_U(fb_psg_sample(p), 0);
    emu_core_destroy(&md->base);
}

static void test_psg_noise_periodic(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_psg *p = &md->psg;
    /* noise channel: rate 3 (fixed), periodic (white bit = 0): 0b1110 = 0xE6?
     * noise register write: latch byte 0xE0 | mode: mode bits [2:1]=rate=11,
     * [0]=white=0 -> 0b110 = 6 -> byte = 0xE6 */
    fb_psg_write(p, 0xE6);
    fb_psg_write(p, 0xF0); /* ch3 volume 0 (loudest) */
    int nonzero = 0;
    for (int i = 0; i < 512; i++) { /* > 2 full LFSR cycles at rate 16 */
        fb_psg_tick(p);
        if (fb_psg_sample(p) != 0)
            nonzero++;
    }
    /* periodic 1/16-duty LFSR must be non-silent sometimes */
    T_CHECK(nonzero > 0);
    T_CHECK(nonzero < 200); /* not a constant output */
    emu_core_destroy(&md->base);
}

/* ---- YM2612 --------------------------------------------------------------- */

static void ym_write(struct fb_ym *y, uint8_t reg, uint8_t val)
{
    fb_ym_write_addr(y, 0, reg);
    fb_ym_write_data(y, 0, val);
}

static void test_ym_timer_irq(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_ym *y = &md->ym;
    ym_write(y, 0x24, 0x00); /* timer A load = 0 -> overflow after 1024 */
    ym_write(y, 0x27, 0x05); /* enable timer A + its IRQ (bit2), reset over */
    T_CHECK_EQ_U(y->timer_a_en, 1);
    for (int i = 0; i < 1100; i++)
        fb_ym_tick(y);
    T_CHECK_EQ_U(y->timer_a_over, 1);
    T_CHECK_EQ_U(y->irq, 1);
    emu_core_destroy(&md->base);
}

static void test_ym_dac_passthrough(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_ym *y = &md->ym;
    ym_write(y, 0x2B, 0x80); /* DAC enable */
    ym_write(y, 0x2A, 0xFF); /* DAC = max */
    fb_ym_mix(y);
    T_CHECK(y->last_l > 3000);
    T_CHECK_EQ_U(y->last_l, y->last_r); /* both channels enabled by default */
    ym_write(y, 0x2A, 0x00);
    fb_ym_mix(y);
    T_CHECK(y->last_l < -3000);
    emu_core_destroy(&md->base);
}

static void test_ym_keyon_envelope(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_ym *y = &md->ym;
    /* channel 0, algorithm 7 (all carriers), moderate TL, fast attack */
    ym_write(y, 0xB0, 0x07);          /* alg 7, fb 0 */
    ym_write(y, 0x30, 0x01);          /* op1 DT 0, MULT 1 */
    ym_write(y, 0x4C, 0x08);          /* op1 TL = 8 */
    ym_write(y, 0x50, 0x1F);          /* op1 AR = 31, KS 0 */
    ym_write(y, 0xA4, 0x44);          /* ch0 block 4, fnum hi 4 */
    ym_write(y, 0xA0, 0x00);          /* fnum lo */
    ym_write(y, 0x28, 0xF0);          /* key on all 4 ops of ch0 */
    T_CHECK_EQ_U(y->chan[0].op[0].state, 1);
    int32_t att0 = y->chan[0].op[0].att_q8;
    for (int i = 0; i < 50; i++)
        fb_ym_tick(y);
    /* attack must reduce attenuation (louder) */
    T_CHECK(y->chan[0].op[0].att_q8 < att0);
    T_CHECK(y->chan[0].out != 0);
    /* key off -> release */
    ym_write(y, 0x28, 0x00);
    T_CHECK_EQ_U(y->chan[0].op[0].state, 0);
    emu_core_destroy(&md->base);
}

static void test_ym_frequency_anchor(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_ym *y = &md->ym;
    /* FNUM=1024, OCT=4, MULT=1 -> 440 Hz (documented anchor).
     * phase_inc must be 8659 per internal sample (8659*53267/2^20 = 439.98). */
    ym_write(y, 0x30, 0x01); /* MULT 1 */
    ym_write(y, 0xA4, (uint8_t)(4u | ((1024u >> 8) << 4))); /* block 4, hi bits */
    ym_write(y, 0xA0, 1024 & 0xFF);
    T_CHECK_EQ_U(y->chan[0].op[0].fnum, 1024);
    T_CHECK_EQ_U(y->chan[0].op[0].block, 4);
    T_CHECK_EQ_U(y->chan[0].op[0].phase_inc, 8659);
    /* one octave up: block 5 doubles the phase increment */
    ym_write(y, 0xA4, (uint8_t)(5u | ((1024u >> 8) << 4)));
    T_CHECK_EQ_U(y->chan[0].op[0].phase_inc, 8659 * 2);
    emu_core_destroy(&md->base);
}

static void test_ym_algorithm7_sum(void)
{
    struct fb_md *md = fb_create_bare();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_ym *y = &md->ym;
    /* alg 7: all four operators are independent carriers. With only op1
     * keyed on, output == op1; keying op2 must add a second component. */
    ym_write(y, 0xB0, 0x07);
    ym_write(y, 0x30, 0x01); /* op1 MULT 1 */
    ym_write(y, 0x50, 0x1F); /* op1 AR 31 */
    ym_write(y, 0x31, 0x01); /* op2 MULT 1 */
    ym_write(y, 0x51, 0x1F); /* op2 AR 31 */
    ym_write(y, 0xA4, 0x44);
    ym_write(y, 0xA0, 0x00);
    ym_write(y, 0x28, 0x10); /* key op1 only */
    for (int i = 0; i < 40; i++)
        fb_ym_tick(y);
    int32_t one = y->chan[0].out;
    T_CHECK(one != 0);
    ym_write(y, 0x28, 0x20); /* key op2 as well */
    for (int i = 0; i < 40; i++)
        fb_ym_tick(y);
    /* alg 7 with two carriers: magnitude differs from one carrier */
    T_CHECK(y->chan[0].out != one);
    (void)one;
    emu_core_destroy(&md->base);
}

/* ---- frame-level determinism ------------------------------------------------ */

uint8_t *psg_rom_shared(size_t *size)
{
    uint8_t *rom = fb_test_rom(size, 0x400);
    /* move.b #$92,$C00011 ; move.b #$00,$C00011 ; move.b #$90,$C00011 ;
     * bra.s self  -- programs PSG ch0 tone N=2 at full volume */
    static const uint16_t code[] = {
        0x1E7C, 0x00C0, 0x0011, 0x0082, /* PSG: latch ch0 tone, N low = 2 */
        0x1E7C, 0x00C0, 0x0011, 0x0000, /* tone high bits = 0 */
        0x1E7C, 0x00C0, 0x0011, 0x0090, /* latch ch0 volume, att 0 */
        0x60FE,
    };
    for (size_t i = 0; i < sizeof code / sizeof code[0]; i++)
        fb_rom_w16(rom, 0x400 + i * 2, code[i]);
    return rom;
}

static void test_frame_determinism_audio(void)
{
    size_t size = 0;
    uint8_t *rom = psg_rom_shared(&size);
    emu_core_t *a = NULL, *b = NULL;
    emu_core_create(emu_core_finalburn(), &a);
    emu_core_create(emu_core_finalburn(), &b);
    emu_core_finalburn()->load_rom(a, rom, size);
    emu_core_finalburn()->load_rom(b, rom, size);
    free(rom);
    if (!a || !b) { T_FAIL("create failed"); return; }

    size_t an = 0, bn = 0;
    for (int f = 0; f < 4; f++) {
        an = 0;
        bn = 0;
        a->vtable->run_frame(a);
        b->vtable->run_frame(b);
        struct fb_md *ma = (struct fb_md *)a;
        struct fb_md *mb = (struct fb_md *)b;
        an = ma->audio_count;
        bn = mb->audio_count;
        T_CHECK(an > 0);
        T_CHECK_EQ_U(an, bn);
        int same = 1;
        for (size_t i = 0; i < an && i < bn; i++)
            if (ma->audio[i] != mb->audio[i])
                same = 0;
        T_CHECK(same);
        T_CHECK_EQ_U(a->vtable->framebuffer(a, NULL, NULL) != NULL, 1);
    }
    emu_core_destroy(a);
    emu_core_destroy(b);
}

T_SUITE_BEGIN(finalburn_audio)
{ "psg_tone_toggle", test_psg_tone_toggle },
{ "psg_silent_at_max_attenuation", test_psg_volume_attenuation },
{ "psg_periodic_noise", test_psg_noise_periodic },
{ "ym_timer_irq", test_ym_timer_irq },
{ "ym_dac_passthrough", test_ym_dac_passthrough },
{ "ym_keyon_envelope", test_ym_keyon_envelope },
{ "ym_frequency_anchor_440hz", test_ym_frequency_anchor },
{ "ym_algorithm_carriers", test_ym_algorithm7_sum },
{ "frame_determinism_audio", test_frame_determinism_audio },
T_SUITE_END
T_SUITE_REG(finalburn_audio)
