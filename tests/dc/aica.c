/*
 * supercastpro: AICA subset tests.
 *
 * Expected values derive from the documented AICA model in
 * src/supercastpro/dc.h (channel register layout, pitch formula,
 * linear gain math, doorbell semantics) and from the ARM architecture
 * for the hand-assembled ARM7 programs - never from emulator internals.
 */
#include "../tests.h"
#include "../../src/supercastpro/dc.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/common/util.h"

/* ---- shared harness --------------------------------------------------------------------- */

static void a32(uint8_t *wave, uint32_t off, uint32_t v)
{
    wave[off] = (uint8_t)v;
    wave[off + 1] = (uint8_t)(v >> 8);
    wave[off + 2] = (uint8_t)(v >> 16);
    wave[off + 3] = (uint8_t)(v >> 24);
}

static struct dc *aica_boot(void)
{
    static const uint16_t prog[] = { 0xAFFEu, 0x0009u }; /* SH-4 self loop */
    size_t boot_off = 0x10000u;
    size_t total = boot_off + sizeof prog;
    uint8_t *img = calloc(1, total);
    if (img == NULL)
        return NULL;
    memcpy(img, "SEGA SEGAKATANA", 15);
    emu_store_le32(img + 0x300u, (uint32_t)(boot_off / 2048u));
    emu_store_le32(img + 0x308u, (uint32_t)sizeof prog);
    for (size_t i = 0; i < sizeof prog / 2; i++) {
        img[boot_off + i * 2] = (uint8_t)prog[i];
        img[boot_off + i * 2 + 1] = (uint8_t)(prog[i] >> 8);
    }
    emu_core_t *c = NULL;
    if (emu_core_supercastpro()->create(&c) != EMU_OK ||
        emu_core_supercastpro()->load_rom(c, img, total) != EMU_OK) {
        free(img);
        if (c != NULL)
            emu_core_supercastpro()->destroy(c);
        return NULL;
    }
    free(img);
    return (struct dc *)c;
}

static void aica_free(struct dc *d)
{
    emu_core_supercastpro()->destroy(&d->base);
}

/* Channel register write: 32-bit access at DC_AICA_REG_BASE + ch*0x80 + off. */
static void chw(struct dc *d, uint32_t ch, uint32_t off, uint32_t v)
{
    dc_write32(d, DC_AICA_REG_BASE + ch * 0x80u + off, v);
}

static uint32_t chr(struct dc *d, uint32_t ch, uint32_t off)
{
    return dc_read32(d, DC_AICA_REG_BASE + ch * 0x80u + off);
}

/* Program a channel per the documented register model. */
static void ch_setup(struct dc *d, uint32_t ch, uint32_t sa, uint32_t lsa,
                     uint32_t lea, uint32_t fmt, int oct, uint32_t fsc,
                     int loop, uint32_t vol, uint32_t pan)
{
    uint32_t octf = (uint32_t)(oct & 0x1F) << 11; /* signed 5-bit field */
    chw(d, ch, 0x00u, (fmt << 11) | ((sa >> 16) & 0x07FFu));
    chw(d, ch, 0x04u, sa & 0xFFFFu);
    chw(d, ch, 0x08u, lsa & 0xFFFFu);
    chw(d, ch, 0x0Cu, lea & 0xFFFFu);
    chw(d, ch, 0x24u, octf | (fsc & 0x07FFu));
    chw(d, ch, 0x30u, (loop ? 0x4000u : 0u) | 0x0001u); /* loop | KYONB */
    chw(d, ch, 0x38u, vol & 0xFFu);
    chw(d, ch, 0x3Cu, pan & 0xFFu);
}

struct audio_cap {
    int16_t buf[735 * 2];
    size_t count;
    int calls;
};

static void audio_cb(void *user, const int16_t *samples, size_t count)
{
    struct audio_cap *cap = user;
    cap->calls++;
    if (count > sizeof cap->buf / sizeof cap->buf[0])
        count = sizeof cap->buf / sizeof cap->buf[0];
    memcpy(cap->buf, samples, count * sizeof(int16_t));
    cap->count = count;
}

/* ---- tests ----------------------------------------------------------------------------- */

static void aica_arm7_runs_from_wave_ram(void)
{
    struct dc *d = aica_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* ARM7 program at wave RAM 0 (SH-4 view 0x00400000):
     *   0x00 ldr r1,[pc,#12] -> 0x1000
     *   0x04 mov r2,#0x99
     *   0x08 str r2,[r1]
     *   0x0c b 0x0c
     *   0x14 .word 0x00001000
     */
    a32(d->aica_ram, 0x00u, 0xE59F100Cu);
    a32(d->aica_ram, 0x04u, 0xE3A02099u);
    a32(d->aica_ram, 0x08u, 0xE5812000u);
    a32(d->aica_ram, 0x0Cu, 0xEAFFFFFEu);
    a32(d->aica_ram, 0x14u, 0x00001000u);
    for (int i = 0; i < 16; i++)
        dc_step(d); /* SH-4 + ARM7 in lockstep (documented hook) */
    T_CHECK_EQ_U(chr(d, 0, 0x00u), 0u); /* sanity: reg 0 untouched */
    T_CHECK_EQ_U(dc_read32(d, 0x00401000u), 0x99u);
    aica_free(d);
}

static void aica_pcm16_first_samples(void)
{
    struct dc *d = aica_boot();
    struct audio_cap cap = { { 0 }, 0, 0 };
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    emu_core_supercastpro()->set_audio_callback(&d->base, audio_cb, &cap);
    /* wave data at 0x1000: 4 samples, LEA exclusive at 0x1008 */
    a32(d->aica_ram, 0x1000u, (uint16_t)1234u | ((uint16_t)(-432) << 16));
    a32(d->aica_ram, 0x1004u, (uint16_t)30000u | ((uint16_t)(-30000) << 16));
    ch_setup(d, 0, 0x1000u, 0x1000u, 0x1008u, DC_AICA_FMT_PCM16, 0, 0, 0, 0,
             128);
    chw(d, 0, DC_AICA_KYONEX, DC_AICA_KYONEX_BIT);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    T_CHECK(cap.calls == 1);
    T_CHECK_EQ_U(cap.count, 735u * 2u);
    /* full volume, center pan: L = R = sample (exact integer math) */
    T_CHECK_EQ(cap.buf[0], 1234);
    T_CHECK_EQ(cap.buf[1], 1234);
    T_CHECK_EQ(cap.buf[2], -432);
    T_CHECK_EQ(cap.buf[3], -432);
    T_CHECK_EQ(cap.buf[4], 30000);
    T_CHECK_EQ(cap.buf[5], 30000);
    T_CHECK_EQ(cap.buf[6], -30000);
    T_CHECK_EQ(cap.buf[7], -30000);
    /* one-shot: silence after the last sample */
    T_CHECK_EQ(cap.buf[8], 0);
    T_CHECK_EQ(cap.buf[9], 0);
    /* CA readback parks at LEA when the voice ends */
    T_CHECK_EQ_U(chr(d, 0, 0x10u), 0x1008u);
    aica_free(d);
}

static void aica_loop_wraps_to_lsa(void)
{
    struct dc *d = aica_boot();
    struct audio_cap cap = { { 0 }, 0, 0 };
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    emu_core_supercastpro()->set_audio_callback(&d->base, audio_cb, &cap);
    a32(d->aica_ram, 0x1000u, (uint16_t)100u | ((uint16_t)200u << 16));
    a32(d->aica_ram, 0x1004u, (uint16_t)300u | ((uint16_t)400u << 16));
    ch_setup(d, 0, 0x1000u, 0x1000u, 0x1008u, DC_AICA_FMT_PCM16, 0, 0, 1, 0,
             128);
    chw(d, 0, DC_AICA_KYONEX, DC_AICA_KYONEX_BIT);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    /* two frames = 1470 samples: the 4-sample loop repeats. Frame 1
     * consumed 735 = 183*4 + 3 samples, so frame 2 resumes at slot 3. */
    static const int expect[8] = { 400, 100, 200, 300, 400, 100, 200, 300 };
    for (int i = 0; i < 8; i++)
        T_CHECK_EQ(cap.buf[i * 2], expect[i]);
    /* CA stays inside [LSA, LEA) while looping */
    uint32_t ca = chr(d, 0, 0x10u);
    T_CHECK(ca >= 0x1000u && ca < 0x1008u);
    aica_free(d);
}

static void aica_kyonex_gates_the_voice(void)
{
    struct dc *d = aica_boot();
    struct audio_cap cap = { { 0 }, 0, 0 };
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    emu_core_supercastpro()->set_audio_callback(&d->base, audio_cb, &cap);
    a32(d->aica_ram, 0x1000u, (uint16_t)5000u | ((uint16_t)5000u << 16));
    a32(d->aica_ram, 0x1004u, (uint16_t)5000u | ((uint16_t)5000u << 16));
    ch_setup(d, 0, 0x1000u, 0x1000u, 0x1008u, DC_AICA_FMT_PCM16, 0, 0, 1, 0,
             128);
    /* frame 1: KYONB set but no KYONEX yet -> silent */
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ(cap.buf[0], 0);
    /* KYONEX starts the voice */
    chw(d, 0, DC_AICA_KYONEX, DC_AICA_KYONEX_BIT);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ(cap.buf[0], 5000);
    /* key-off via KYONB = 0 + KYONEX */
    ch_setup(d, 0, 0x1000u, 0x1000u, 0x1008u, DC_AICA_FMT_PCM16, 0, 0, 1, 0,
             128);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ(cap.buf[0], 5000);
    chw(d, 0, 0x30u, 0x0000u); /* KYONB cleared */
    chw(d, 0, DC_AICA_KYONEX, DC_AICA_KYONEX_BIT);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ(cap.buf[0], 0);
    aica_free(d);
}

static void aica_volume_and_pan_gains(void)
{
    struct dc *d = aica_boot();
    struct audio_cap cap = { { 0 }, 0, 0 };
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    emu_core_supercastpro()->set_audio_callback(&d->base, audio_cb, &cap);
    a32(d->aica_ram, 0x1000u, (uint16_t)30000u | ((uint16_t)30000u << 16));
    /* volume 128 of 255: out = s * (255-128) * 255 / 65025 */
    ch_setup(d, 0, 0x1000u, 0x1000u, 0x1008u, DC_AICA_FMT_PCM16, 0, 0, 0,
             128, 128);
    chw(d, 0, DC_AICA_KYONEX, DC_AICA_KYONEX_BIT);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    int32_t want = (int32_t)30000 * 127 * 255 / 65025;
    T_CHECK_EQ(cap.buf[0], (int16_t)want);
    T_CHECK_EQ(cap.buf[1], (int16_t)want);
    aica_free(d);

    /* pan hard left: right output silent */
    d = aica_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    memset(&cap, 0, sizeof cap);
    emu_core_supercastpro()->set_audio_callback(&d->base, audio_cb, &cap);
    a32(d->aica_ram, 0x1000u, (uint16_t)30000u | ((uint16_t)30000u << 16));
    ch_setup(d, 0, 0x1000u, 0x1000u, 0x1008u, DC_AICA_FMT_PCM16, 0, 0, 0, 0,
             0);
    chw(d, 0, DC_AICA_KYONEX, DC_AICA_KYONEX_BIT);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ(cap.buf[0], 30000);
    T_CHECK_EQ(cap.buf[1], 0);
    aica_free(d);
}

static void aica_pitch_octave_and_fsc(void)
{
    struct dc *d = aica_boot();
    struct audio_cap cap = { { 0 }, 0, 0 };
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    emu_core_supercastpro()->set_audio_callback(&d->base, audio_cb, &cap);
    /* samples 100,200,300,400... at octave +1: rate doubles, so the
     * mixer reads every other sample (step = 2.0 in 16.16 = 0x20000) */
    a32(d->aica_ram, 0x1000u, (uint16_t)100u | ((uint16_t)200u << 16));
    a32(d->aica_ram, 0x1004u, (uint16_t)300u | ((uint16_t)400u << 16));
    a32(d->aica_ram, 0x1008u, (uint16_t)500u | ((uint16_t)600u << 16));
    a32(d->aica_ram, 0x100Cu, (uint16_t)700u | ((uint16_t)800u << 16));
    ch_setup(d, 0, 0x1000u, 0x1000u, 0x1010u, DC_AICA_FMT_PCM16, 1, 0, 1, 0,
             128);
    chw(d, 0, DC_AICA_KYONEX, DC_AICA_KYONEX_BIT);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    T_CHECK_EQ(cap.buf[0], 100);
    T_CHECK_EQ(cap.buf[2], 300);
    T_CHECK_EQ(cap.buf[4], 500);
    T_CHECK_EQ(cap.buf[6], 700);
    T_CHECK_EQ(cap.buf[8], 100); /* looped: 8 samples consumed */
    aica_free(d);
}

static void aica_mcipd_interrupts_arm7(void)
{
    struct dc *d = aica_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* ARM7: enable IRQs (sys mode), loop; IRQ handler at 0x18 marks
     * wave RAM 0x1000 with 0x77 and returns. */
    a32(d->aica_ram, 0x00u, 0xE3A0001Fu); /* mov r0,#0x1f        */
    a32(d->aica_ram, 0x04u, 0xE129F000u); /* msr cpsr_c,r0       */
    a32(d->aica_ram, 0x08u, 0xEAFFFFFEu); /* b 0x08              */
    a32(d->aica_ram, 0x18u, 0xE59F1008u); /* ldr r1,[pc,#8]      */
    a32(d->aica_ram, 0x1Cu, 0xE3A02077u); /* mov r2,#0x77        */
    a32(d->aica_ram, 0x20u, 0xE5812000u); /* str r2,[r1]         */
    a32(d->aica_ram, 0x24u, 0xE25EF004u); /* subs pc,lr,#4       */
    a32(d->aica_ram, 0x28u, 0x00001000u); /* .word 0x00001000    */
    dc_write32(d, DC_AICA_REG_BASE + DC_AICA_MCIPD, DC_AICA_MCIPD_BIT);
    for (int i = 0; i < 64; i++)
        dc_step(d);
    T_CHECK_EQ_U(dc_read32(d, 0x00401000u), 0x77u);
    aica_free(d);
}

static void aica_arm7_mcire_sets_flag(void)
{
    struct dc *d = aica_boot();
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* ARM7 writes bit 9 of MCIRE (ARM7 view 0x0080289C). */
    a32(d->aica_ram, 0x00u, 0xE59F000Cu); /* ldr r0,[pc,#12]     */
    a32(d->aica_ram, 0x04u, 0xE59F100Cu); /* ldr r1,[pc,#12]     */
    a32(d->aica_ram, 0x08u, 0xE5801000u); /* str r1,[r0]         */
    a32(d->aica_ram, 0x0Cu, 0xEAFFFFFEu); /* b 0x0c              */
    a32(d->aica_ram, 0x14u, 0x0080289Cu); /* .word MCIRE         */
    a32(d->aica_ram, 0x18u, 0x00000200u); /* .word bit 9         */
    for (int i = 0; i < 16; i++)
        dc_step(d);
    T_CHECK_EQ_U(dc_read32(d, DC_AICA_REG_BASE + DC_AICA_MCIRE),
                 DC_AICA_MCIRE_BIT);
    /* SH-4 clears the flag by writing the same bit */
    dc_write32(d, DC_AICA_REG_BASE + DC_AICA_MCIRE, DC_AICA_MCIRE_BIT);
    T_CHECK_EQ_U(dc_read32(d, DC_AICA_REG_BASE + DC_AICA_MCIRE), 0u);
    aica_free(d);
}

static void aica_state_roundtrip_midplay(void)
{
    struct dc *ref = aica_boot();
    struct dc *cmp = aica_boot();
    struct audio_cap cap_ref = { { 0 }, 0, 0 };
    struct audio_cap cap_cmp = { { 0 }, 0, 0 };
    if (ref == NULL || cmp == NULL) {
        T_FAIL("boot failed");
        return;
    }
    emu_core_supercastpro()->set_audio_callback(&ref->base, audio_cb,
                                                &cap_ref);
    emu_core_supercastpro()->set_audio_callback(&cmp->base, audio_cb,
                                                &cap_cmp);
    a32(ref->aica_ram, 0x1000u, (uint16_t)100u | ((uint16_t)200u << 16));
    a32(ref->aica_ram, 0x1004u, (uint16_t)300u | ((uint16_t)400u << 16));
    ch_setup(ref, 0, 0x1000u, 0x1000u, 0x1008u, DC_AICA_FMT_PCM16, 0, 0, 1,
             0, 128);
    chw(ref, 0, DC_AICA_KYONEX, DC_AICA_KYONEX_BIT);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&ref->base), EMU_OK);

    size_t sz = emu_core_supercastpro()->state_size(&ref->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob != NULL) {
        T_CHECK_EQ(emu_core_supercastpro()->save_state(&ref->base, blob, sz),
                   EMU_OK);
        T_CHECK_EQ(emu_core_supercastpro()->load_state(&cmp->base, blob, sz),
                   EMU_OK);
        free(blob);
        /* both continue from the same mid-loop state: ref frame 2 vs
         * cmp frame 1 must be identical */
        T_CHECK_EQ(emu_core_supercastpro()->run_frame(&ref->base), EMU_OK);
        T_CHECK_EQ(emu_core_supercastpro()->run_frame(&cmp->base), EMU_OK);
        T_CHECK(cap_ref.count == cap_cmp.count);
        for (size_t i = 0; i < cap_ref.count && i < cap_cmp.count; i++)
            T_CHECK_EQ(cap_ref.buf[i], cap_cmp.buf[i]);
    }
    aica_free(ref);
    aica_free(cmp);
}

T_SUITE_BEGIN(dc_aica)
{ "aica_arm7_runs_from_wave_ram", aica_arm7_runs_from_wave_ram },
{ "aica_pcm16_first_samples", aica_pcm16_first_samples },
{ "aica_loop_wraps_to_lsa", aica_loop_wraps_to_lsa },
{ "aica_kyonex_gates_the_voice", aica_kyonex_gates_the_voice },
{ "aica_volume_and_pan_gains", aica_volume_and_pan_gains },
{ "aica_pitch_octave_and_fsc", aica_pitch_octave_and_fsc },
{ "aica_mcipd_interrupts_arm7", aica_mcipd_interrupts_arm7 },
{ "aica_arm7_mcire_sets_flag", aica_arm7_mcire_sets_flag },
{ "aica_state_roundtrip_midplay", aica_state_roundtrip_midplay },
T_SUITE_END
T_SUITE_REG(dc_aica)
