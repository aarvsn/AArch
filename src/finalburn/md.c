/*
 * finalburn: machine glue - 68K/Z80 buses, I/O ports, frame loop,
 * core vtable and explicit save states.
 *
 * 68K memory map:
 *   $000000-$3FFFFF ROM (cart, mirrored inside its own size)
 *   $A00000-$A07FFF Z80 RAM (when the Z80 bus is released)
 *   $A10000-$A1001F I/O ports (version, pads, ctrl regs)
 *   $A11100 busreq, $A11200 reset, $A14000 Z80 bank
 *   $A04000-$A04003 YM2612 (byte access)
 *   $C00000-$C0001F VDP
 *   $C00011         PSG
 *   $E00000-$FFFFFF 64 KiB work RAM (mirrored)
 *
 * Approximations: unaligned word/long accesses are aligned silently, word
 * reads on the byte-wide I/O devices are assembled from two byte reads,
 * TMSS/open-bus regions return 0xFF.
 */
#include "fb_md.h"

#include <stdlib.h>
#include <string.h>

/* ---- cartridge helpers ----------------------------------------------------- */

static uint32_t bus_mask24(uint32_t addr)
{
    return addr & 0xFFFFFFu;
}

/* ---- 68K bus ---------------------------------------------------------------- */

static void ym_68k_write(struct fb_md *md, uint32_t a, uint8_t v);

uint8_t fb_md_68k_read8(struct fb_md *md, uint32_t a)
{
    a = bus_mask24(a);
    if (a < 0x400000u)
        return fb_cart_read8(&md->cart, a);
    if (a >= 0xA00000u && a < 0xA10000u) {
        if (md->z80_busreq)
            return 0xFF;
        a &= 0x7FFFu;
        if (a < 0x2000u)
            return md->zram[a & 0x1FFFu];
        if (a < 0x4000u)
            return md->zram[a & 0x1FFFu];
        if (a >= 0x8000u)
            return fb_z80_bus_read(md, (uint16_t)(a - 0x8000u));
        return 0xFF;
    }
    if (a >= 0xA10000u && a < 0xA10020u) {
        switch (a & 0x1Fu) {
        case 0x00: return 0xA0;            /* version low byte */
        case 0x03: {                       /* pad 1 data (active low) */
            if (md->pad_th)
                return (uint8_t)(0x30u | (md->pad1 & 0x0Fu));
            return (uint8_t)(0x70u | ((md->pad1 >> 4) & 0x0Fu));
        }
        case 0x05: return 0x7F;            /* pad 2: not connected */
        case 0x07: return 0x7F;            /* pad 3: not connected */
        default:   return 0x00;
        }
    }
    if (a >= 0xA04000u && a < 0xA04004u)
        return 0; /* YM2612 reads return 0 (busy never set) */
    if (a >= 0xC00000u && a < 0xC00020u)
        return fb_vdp_read8(md, a);
    if (a >= 0xE00000u)
        return md->ram[a & 0xFFFFu];
    return 0xFF; /* open bus */
}

uint16_t fb_md_68k_read16(struct fb_md *md, uint32_t a)
{
    uint16_t hi = fb_md_68k_read8(md, a);
    uint16_t lo = fb_md_68k_read8(md, a + 1);
    return (uint16_t)((hi << 8) | lo);
}

uint32_t fb_md_68k_read32(struct fb_md *md, uint32_t a)
{
    uint32_t hi = fb_md_68k_read16(md, a);
    uint32_t lo = fb_md_68k_read16(md, a + 2);
    return (hi << 16) | lo;
}

void fb_md_68k_write8(struct fb_md *md, uint32_t a, uint8_t v)
{
    a = bus_mask24(a);
    if (a >= 0x200000u && a < 0x210000u) {
        fb_cart_sram_write8(&md->cart, a, v);
        return;
    }
    if (a < 0x400000u)
        return; /* ROM */
    if (a >= 0xA00000u && a < 0xA10000u) {
        if (md->z80_busreq)
            return;
        a &= 0x7FFFu;
        if (a < 0x4000u) {
            md->zram[a & 0x1FFFu] = v;
            return;
        }
        if (a >= 0x8000u) {
            fb_z80_bus_write(md, (uint16_t)(a - 0x8000u), v);
            return;
        }
        return;
    }
    if (a >= 0xA10000u && a < 0xA10020u) {
        switch (a & 0x1Fu) {
        case 0x09: md->pad_th = (uint8_t)((v >> 6) & 1u); break; /* pad1 ctrl */
        default: break;
        }
        return;
    }
    if (a >= 0xA04000u && a < 0xA04004u) {
        ym_68k_write(md, a, v);
        return;
    }
    if (a == 0xA11000u || a == 0xA11100u) {
        md->z80_busreq = (uint8_t)(v & 1u);
        return;
    }
    if (a == 0xA11200u) {
        md->z80_reset = (uint8_t)(v & 1u);
        if (md->z80_reset)
            fb_z80_reset(&md->z80);
        return;
    }
    if (a == 0xA14000u) {
        md->z80_bank = v;
        return;
    }
    if (a >= 0xC00000u && a < 0xC00020u) {
        fb_vdp_write8(md, a, v);
        return;
    }
    if (a >= 0xE00000u) {
        md->ram[a & 0xFFFFu] = v;
        return;
    }
}

void fb_md_68k_write16(struct fb_md *md, uint32_t a, uint16_t v)
{
    /* byte-wide devices: 68K word writes drive both halves */
    fb_md_68k_write8(md, a, (uint8_t)(v >> 8));
    fb_md_68k_write8(md, a + 1, (uint8_t)v);
}

void fb_md_68k_write32(struct fb_md *md, uint32_t a, uint32_t v)
{
    fb_md_68k_write16(md, a, (uint16_t)(v >> 16));
    fb_md_68k_write16(md, a + 2, (uint16_t)v);
}

/* ---- YM2612 68K access (register selection via $A04000/$A04002) ------------- */

static void ym_68k_write(struct fb_md *md, uint32_t a, uint8_t v)
{
    uint32_t r = (a - 0xA04000u) & 3u;
    uint8_t bank = (uint8_t)((r >> 1) & 1u);
    if ((r & 1u) == 0)
        fb_ym_write_addr(&md->ym, bank, v);
    else
        fb_ym_write_data(&md->ym, bank, v);
}

/* ---- Z80 bus bridge ---------------------------------------------------------- */

uint8_t fb_z80_bus_read(struct fb_md *md, uint16_t a)
{
    if (a < 0x4000u)
        return md->zram[a & 0x1FFFu];
    if (a < 0x6000u)
        return 0; /* YM2612 reads */
    if (a < 0x8000u) {
        if (a == 0x7F00u) { /* H counter (approximation) */
            return (uint8_t)((md->vdp.line_cycle >> 1) & 0xFFu);
        }
        return 0;
    }
    /* banked window into 68K space */
    uint32_t map = (uint32_t)md->z80_bank << 15;
    return fb_md_68k_read8(md, map + (uint32_t)(a - 0x8000u));
}

void fb_z80_bus_write(struct fb_md *md, uint16_t a, uint8_t v)
{
    if (a < 0x4000u) {
        md->zram[a & 0x1FFFu] = v;
        return;
    }
    if (a < 0x6000u) { /* YM2612: 4000/4001 bank0, 4002/4003 bank1 */
        uint8_t slot = (uint8_t)(a & 3u);
        uint8_t bank = (uint8_t)((slot >> 1) & 1u);
        if ((slot & 1u) == 0)
            fb_ym_write_addr(&md->ym, bank, v);
        else
            fb_ym_write_data(&md->ym, bank, v);
        return;
    }
    if (a < 0x8000u) {
        if (a < 0x6100u) { /* bank register */
            md->z80_bank = (uint8_t)(v & 0xFFu);
            return;
        }
        return; /* mirrors / pause: ignored */
    }
    uint32_t map = (uint32_t)md->z80_bank << 15;
    fb_md_68k_write8(md, map + (uint32_t)(a - 0x8000u), v);
}

uint8_t fb_z80_io_read(struct fb_md *md, uint16_t port)
{
    (void)md;
    (void)port;
    return 0xFF; /* Genesis Z80 I/O is unconnected (memory-mapped instead) */
}

void fb_z80_io_write(struct fb_md *md, uint16_t port, uint8_t v)
{
    (void)md;
    (void)port;
    (void)v;
}

/* ---- machine frame ------------------------------------------------------------ */

static void recompute_irq(struct fb_md *md)
{
    uint8_t lvl = 0;
    if (md->vdp.vint_68k)
        lvl = 6;
    if (md->vdp.hint_68k && lvl < 4)
        lvl = 4;
    if (md->ym.irq && lvl < 2)
        lvl = 2;
    fb_m68k_set_irq(md, lvl);
}

static void periph_chunk(struct fb_md *md, int32_t chunk)
{
    struct fb_md *m = md;
    /* Z80: 3579545/7670453 t-states per 68K cycle, with credit accounting */
    m->z80_acc += (uint64_t)(uint32_t)chunk * FB_Z80_HZ;
    while (m->z80_acc >= FB_MASTER_HZ) {
        m->z80_acc -= FB_MASTER_HZ;
        m->z80_credit += 1;
    }
    if (m->z80_credit > 0 && !m->z80_busreq && !m->z80_reset) {
        int32_t used = fb_z80_run(m, m->z80_credit);
        m->z80_credit -= used;
    }
    /* YM2612 internal sample = master/144 */
    m->ym_acc += (uint64_t)(uint32_t)chunk;
    while (m->ym_acc >= FB_YM_DIV) {
        m->ym_acc -= FB_YM_DIV;
        fb_ym_tick(&m->ym);
    }
    /* PSG tone tick = z80 clock/16 */
    m->psg_acc += (uint64_t)(uint32_t)chunk * FB_Z80_HZ;
    while (m->psg_acc >= FB_MASTER_HZ * FB_PSG_DIV) {
        m->psg_acc -= FB_MASTER_HZ * FB_PSG_DIV;
        fb_psg_tick(&m->psg);
    }
    /* output samples at 48 kHz */
    m->out_acc += (uint64_t)(uint32_t)chunk * FB_OUT_RATE;
    while (m->out_acc >= FB_MASTER_HZ) {
        m->out_acc -= FB_MASTER_HZ;
        if (m->audio_count / 2 < FB_MAX_SAMPLES) {
            int32_t psg = fb_psg_sample(&m->psg) / 2;
            int32_t l = psg + m->ym.last_l;
            int32_t r = psg + m->ym.last_r;
            if (l > 32767)
                l = 32767;
            if (l < -32768)
                l = -32768;
            if (r > 32767)
                r = 32767;
            if (r < -32768)
                r = -32768;
            m->audio[m->audio_count++] = (int16_t)l;
            m->audio[m->audio_count++] = (int16_t)r;
        }
    }
    m->vdp.line_cycle += (uint32_t)chunk;
    fb_vdp_run_dma(m, chunk);
    recompute_irq(m);
}

static void fb_power_on(struct fb_md *md)
{
    memset(md->ram, 0, sizeof md->ram);
    memset(md->zram, 0, sizeof md->zram);
    fb_vdp_reset(&md->vdp);
    fb_psg_reset(&md->psg);
    fb_ym_reset(&md->ym);
    fb_z80_reset(&md->z80);
    md->z80_busreq = 0;
    md->z80_reset = 1; /* Z80 held in reset until the 68K releases it */
    md->z80_bank = 0;
    md->pad_th = 0;
    md->pad1 = 0xFF; /* active low: nothing pressed */
    md->z80_acc = 0;
    md->z80_credit = 0;
    md->ym_acc = 0;
    md->psg_acc = 0;
    md->out_acc = 0;
    md->audio_count = 0;
    md->frame_done = 0;
    md->m68k.halted = 0;
    md->m68k.stopped = 0;
    fb_m68k_reset_machine(md);
}

static emu_result_t fb_run_frame(emu_core_t *core)
{
    struct fb_md *md = (struct fb_md *)core;
    if (md->cart.rom == NULL)
        return EMU_ENOROM;

    md->audio_count = 0;
    int32_t target = 0;
    uint32_t total = 0;
    for (int line = 0; line < FB_LINES; line++) {
        target += FB_68K_PER_LINE;
        while (total < (uint32_t)target) {
            int32_t chunk = 16;
            int32_t used = fb_m68k_run(md, chunk);
            if (md->m68k.halted)
                used = chunk;
            total += (uint32_t)used;
            periph_chunk(md, used > chunk ? chunk : used);
        }
        fb_vdp_end_of_line(md);
    }
    if (md->audio_cb && md->audio_count)
        md->audio_cb(md->audio_user, md->audio, md->audio_count);
    return EMU_OK;
}

/* ---- vtable ------------------------------------------------------------------ */

static const emu_core_vtable_t fb_vtable;

static emu_result_t fb_create(emu_core_t **out)
{
    if (out == NULL)
        return EMU_EINVAL;
    struct fb_md *md = calloc(1, sizeof *md);
    if (md == NULL)
        return EMU_EINVAL;
    md->base.vtable = &fb_vtable;
    fb_cart_init(&md->cart);
    fb_vdp_init(&md->vdp, md->fb);
    fb_psg_init(&md->psg);
    fb_ym_init(&md->ym);
    fb_power_on(md);
    *out = &md->base;
    return EMU_OK;
}

static void fb_destroy(emu_core_t *core)
{
    struct fb_md *md = (struct fb_md *)core;
    if (md == NULL)
        return;
    fb_cart_free(&md->cart);
    free(md);
}

static emu_result_t fb_load_rom(emu_core_t *core, const uint8_t *data, size_t size)
{
    struct fb_md *md = (struct fb_md *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    emu_result_t r = fb_cart_load(&md->cart, data, size);
    if (r != EMU_OK)
        return r;
    fb_power_on(md);
    return EMU_OK;
}

static void fb_reset(emu_core_t *core)
{
    struct fb_md *md = (struct fb_md *)core;
    fb_power_on(md);
}

static const uint32_t *fb_framebuffer(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct fb_md *md = (struct fb_md *)core;
    if (w)
        *w = FB_SCREEN_W;
    if (h)
        *h = FB_SCREEN_H;
    return md->fb;
}

static void fb_set_input(emu_core_t *core, uint32_t buttons)
{
    struct fb_md *md = (struct fb_md *)core;
    md->input = buttons;
    /* active-low pad state; bits 0-3 U/D/L/R, 4-6 A/B/C, 7 Start */
    md->pad1 = (uint8_t)(~buttons & 0xFFu);
}

static void fb_set_audio(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    struct fb_md *md = (struct fb_md *)core;
    md->audio_cb = cb;
    md->audio_user = user;
}

/* ---- save states --------------------------------------------------------------- */

static void fb_serialize(const struct fb_md *md, emu_state_writer *w)
{
    sw_u32(w, EMU_STATE_MAGIC);
    sw_u32(w, 0x46423130u); /* "FB10" */
    sw_mem(w, md->ram, sizeof md->ram);
    sw_mem(w, md->zram, sizeof md->zram);
    sw_mem(w, md->vdp.vram, sizeof md->vdp.vram);
    sw_mem(w, md->vdp.cram, sizeof md->vdp.cram);
    sw_mem(w, md->vdp.vsram, sizeof md->vdp.vsram);
    for (int i = 0; i < 24; i++)
        sw_u16(w, md->vdp.regs[i]);
    sw_u32(w, md->vdp.addr);
    sw_u8(w, md->vdp.mode);
    sw_u8(w, md->vdp.cmd_pending);
    sw_u16(w, md->vdp.cmd_w1);
    sw_u16(w, md->vdp.read_buf);
    sw_u8(w, md->vdp.dma_active);
    sw_u8(w, md->vdp.dma_type);
    sw_u32(w, md->vdp.dma_left);
    sw_u32(w, md->vdp.dma_src);
    sw_u8(w, md->vdp.dma_fill_val);
    sw_u8(w, md->vdp.vint_68k);
    sw_u8(w, md->vdp.hint_68k);
    sw_u8(w, md->vdp.vint_z80);
    sw_u8(w, md->vdp.sprite_collision);
    sw_u8(w, md->vdp.sprite_overflow);
    sw_u16(w, md->vdp.hint_counter);
    sw_u32(w, md->vdp.line_cycle);
    sw_u16(w, md->vdp.hscroll_a);
    sw_u16(w, md->vdp.hscroll_b);
    sw_u32(w, (uint32_t)md->vdp.line);

    /* 68K */
    for (int i = 0; i < 8; i++)
        sw_u32(w, md->m68k.d[i]);
    for (int i = 0; i < 8; i++)
        sw_u32(w, md->m68k.a[i]);
    sw_u32(w, md->m68k.usp);
    sw_u32(w, md->m68k.ssp);
    sw_u32(w, md->m68k.pc);
    sw_u32(w, md->m68k.ppc);
    sw_u16(w, md->m68k.sr);
    sw_u8(w, md->m68k.stopped);
    sw_u8(w, md->m68k.halted);
    sw_u8(w, md->m68k.int_line);

    /* Z80 */
    sw_u16(w, md->z80.af);
    sw_u16(w, md->z80.bc);
    sw_u16(w, md->z80.de);
    sw_u16(w, md->z80.hl);
    sw_u16(w, md->z80.sp);
    sw_u16(w, md->z80.pc);
    sw_u16(w, md->z80.ix);
    sw_u16(w, md->z80.iy);
    sw_u16(w, md->z80.af2);
    sw_u16(w, md->z80.bc2);
    sw_u16(w, md->z80.de2);
    sw_u16(w, md->z80.hl2);
    sw_u8(w, md->z80.i);
    sw_u8(w, md->z80.r);
    sw_u8(w, md->z80.im);
    sw_u8(w, md->z80.iff1);
    sw_u8(w, md->z80.iff2);
    sw_u8(w, md->z80.halted);
    sw_u8(w, md->z80.int_pending);

    /* PSG */
    for (int i = 0; i < 3; i++)
        sw_u16(w, md->psg.tone[i]);
    for (int i = 0; i < 4; i++)
        sw_u8(w, md->psg.vol[i]);
    for (int i = 0; i < 3; i++)
        sw_u16(w, md->psg.tone_ctr[i]);
    sw_mem(w, md->psg.tone_out, 3);
    sw_u16(w, md->psg.noise_shift);
    sw_u8(w, md->psg.noise_mode);
    sw_u16(w, md->psg.noise_ctr);
    sw_u16(w, md->psg.noise_per);
    sw_u8(w, md->psg.noise_out);
    sw_u8(w, md->psg.latch_type);
    sw_u8(w, md->psg.latch_chan);

    /* YM2612 */
    for (int ch = 0; ch < 6; ch++) {
        for (int op = 0; op < 4; op++) {
            const struct fb_ym_op *o = &md->ym.chan[ch].op[op];
            sw_u16(w, o->fnum);
            sw_u8(w, o->block);
            sw_u8(w, o->mult);
            sw_u32(w, o->phase);
            sw_u32(w, o->phase_inc);
            sw_u8(w, o->tl);
            sw_u8(w, o->dt);
            sw_u8(w, o->ks);
            sw_u8(w, o->ar);
            sw_u8(w, o->dr);
            sw_u8(w, o->sr);
            sw_u8(w, o->rr);
            sw_u8(w, o->sl);
            sw_u16(w, (uint16_t)o->att_q8);
            sw_u8(w, o->state);
        }
        const struct fb_ym_chan *c = &md->ym.chan[ch];
        sw_u8(w, c->algorithm);
        sw_u8(w, c->feedback);
        sw_u8(w, c->pan_l);
        sw_u8(w, c->pan_r);
        sw_i16(w, c->fb_hist[0]);
        sw_i16(w, c->fb_hist[1]);
    }
    sw_u8(w, md->ym.dac_data);
    sw_u8(w, md->ym.dac_en);
    sw_u8(w, md->ym.addr_bank);
    sw_mem(w, md->ym.reg_latched, 2);
    sw_u16(w, md->ym.timer_a_load);
    sw_u16(w, md->ym.timer_b_load);
    sw_u16(w, md->ym.timer_a_ctr);
    sw_u16(w, md->ym.timer_b_ctr);
    sw_u8(w, md->ym.timer_a_en);
    sw_u8(w, md->ym.timer_b_en);
    sw_u8(w, md->ym.timer_a_over);
    sw_u8(w, md->ym.timer_b_over);
    sw_u8(w, md->ym.tb_div);
    sw_u8(w, md->ym.irq);

    /* machine */
    sw_u8(w, md->z80_busreq);
    sw_u8(w, md->z80_reset);
    sw_u8(w, md->z80_bank);
    sw_u8(w, md->pad1);
    sw_u8(w, md->pad_th);
    sw_u32(w, md->input);
    if (md->cart.sram != NULL)
        sw_mem(w, md->cart.sram, 0x10000);
}

static void fb_deserialize(struct fb_md *md, emu_state_reader *r)
{
    (void)sr_u32(r); /* magic */
    (void)sr_u32(r); /* version */
    sr_mem(r, md->ram, sizeof md->ram);
    sr_mem(r, md->zram, sizeof md->zram);
    sr_mem(r, md->vdp.vram, sizeof md->vdp.vram);
    sr_mem(r, md->vdp.cram, sizeof md->vdp.cram);
    sr_mem(r, md->vdp.vsram, sizeof md->vdp.vsram);
    for (int i = 0; i < 24; i++)
        md->vdp.regs[i] = sr_u16(r);
    md->vdp.addr = sr_u32(r);
    md->vdp.mode = sr_u8(r);
    md->vdp.cmd_pending = sr_u8(r);
    md->vdp.cmd_w1 = sr_u16(r);
    md->vdp.read_buf = sr_u16(r);
    md->vdp.dma_active = sr_u8(r);
    md->vdp.dma_type = sr_u8(r);
    md->vdp.dma_left = sr_u32(r);
    md->vdp.dma_src = sr_u32(r);
    md->vdp.dma_fill_val = sr_u8(r);
    md->vdp.vint_68k = sr_u8(r);
    md->vdp.hint_68k = sr_u8(r);
    md->vdp.vint_z80 = sr_u8(r);
    md->vdp.sprite_collision = sr_u8(r);
    md->vdp.sprite_overflow = sr_u8(r);
    md->vdp.hint_counter = sr_u16(r);
    md->vdp.line_cycle = sr_u32(r);
    md->vdp.hscroll_a = sr_u16(r);
    md->vdp.hscroll_b = sr_u16(r);
    md->vdp.line = (int)sr_u32(r);

    for (int i = 0; i < 8; i++)
        md->m68k.d[i] = sr_u32(r);
    for (int i = 0; i < 8; i++)
        md->m68k.a[i] = sr_u32(r);
    md->m68k.usp = sr_u32(r);
    md->m68k.ssp = sr_u32(r);
    md->m68k.pc = sr_u32(r);
    md->m68k.ppc = sr_u32(r);
    md->m68k.sr = sr_u16(r);
    md->m68k.stopped = sr_u8(r);
    md->m68k.halted = sr_u8(r);
    md->m68k.int_line = sr_u8(r);

    md->z80.af = sr_u16(r);
    md->z80.bc = sr_u16(r);
    md->z80.de = sr_u16(r);
    md->z80.hl = sr_u16(r);
    md->z80.sp = sr_u16(r);
    md->z80.pc = sr_u16(r);
    md->z80.ix = sr_u16(r);
    md->z80.iy = sr_u16(r);
    md->z80.af2 = sr_u16(r);
    md->z80.bc2 = sr_u16(r);
    md->z80.de2 = sr_u16(r);
    md->z80.hl2 = sr_u16(r);
    md->z80.i = sr_u8(r);
    md->z80.r = sr_u8(r);
    md->z80.im = sr_u8(r);
    md->z80.iff1 = sr_u8(r);
    md->z80.iff2 = sr_u8(r);
    md->z80.halted = sr_u8(r);
    md->z80.int_pending = sr_u8(r);

    for (int i = 0; i < 3; i++)
        md->psg.tone[i] = sr_u16(r);
    for (int i = 0; i < 4; i++)
        md->psg.vol[i] = sr_u8(r);
    for (int i = 0; i < 3; i++)
        md->psg.tone_ctr[i] = sr_u16(r);
    sr_mem(r, md->psg.tone_out, 3);
    md->psg.noise_shift = sr_u16(r);
    md->psg.noise_mode = sr_u8(r);
    md->psg.noise_ctr = sr_u16(r);
    md->psg.noise_per = sr_u16(r);
    md->psg.noise_out = sr_u8(r);
    md->psg.latch_type = sr_u8(r);
    md->psg.latch_chan = sr_u8(r);

    for (int ch = 0; ch < 6; ch++) {
        for (int op = 0; op < 4; op++) {
            struct fb_ym_op *o = &md->ym.chan[ch].op[op];
            o->fnum = sr_u16(r);
            o->block = sr_u8(r);
            o->mult = sr_u8(r);
            o->phase = sr_u32(r);
            o->phase_inc = sr_u32(r);
            o->tl = sr_u8(r);
            o->dt = sr_u8(r);
            o->ks = sr_u8(r);
            o->ar = sr_u8(r);
            o->dr = sr_u8(r);
            o->sr = sr_u8(r);
            o->rr = sr_u8(r);
            o->sl = sr_u8(r);
            o->att_q8 = (int32_t)(int16_t)sr_u16(r);
            o->state = sr_u8(r);
        }
        struct fb_ym_chan *c = &md->ym.chan[ch];
        c->algorithm = sr_u8(r);
        c->feedback = sr_u8(r);
        c->pan_l = sr_u8(r);
        c->pan_r = sr_u8(r);
        c->fb_hist[0] = sr_i16(r);
        c->fb_hist[1] = sr_i16(r);
    }
    md->ym.dac_data = sr_u8(r);
    md->ym.dac_en = sr_u8(r);
    md->ym.addr_bank = sr_u8(r);
    sr_mem(r, md->ym.reg_latched, 2);
    md->ym.timer_a_load = sr_u16(r);
    md->ym.timer_b_load = sr_u16(r);
    md->ym.timer_a_ctr = sr_u16(r);
    md->ym.timer_b_ctr = sr_u16(r);
    md->ym.timer_a_en = sr_u8(r);
    md->ym.timer_b_en = sr_u8(r);
    md->ym.timer_a_over = sr_u8(r);
    md->ym.timer_b_over = sr_u8(r);
    md->ym.tb_div = sr_u8(r);
    md->ym.irq = sr_u8(r);

    md->z80_busreq = sr_u8(r);
    md->z80_reset = sr_u8(r);
    md->z80_bank = sr_u8(r);
    md->pad1 = sr_u8(r);
    md->pad_th = sr_u8(r);
    md->input = sr_u32(r);
    if (md->cart.sram != NULL)
        sr_mem(r, md->cart.sram, 0x10000);
}

static size_t fb_state_size(emu_core_t *core)
{
    struct fb_md *md = (struct fb_md *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    fb_serialize(md, &w);
    return w.pos;
}

static emu_result_t fb_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    struct fb_md *md = (struct fb_md *)core;
    if (buf == NULL || cap == 0)
        return EMU_EINVAL;
    if (cap < fb_state_size(core))
        return EMU_ENOSPACE;
    emu_state_writer w = { buf, cap, 0, 0 };
    fb_serialize(md, &w);
    return w.overflow ? EMU_ENOSPACE : EMU_OK;
}

static emu_result_t fb_load_state(emu_core_t *core, const uint8_t *buf, size_t size)
{
    struct fb_md *md = (struct fb_md *)core;
    if (buf == NULL || size < 8)
        return EMU_EBADSTATE;
    emu_state_reader r = { buf, size, 0, 0 };
    uint32_t magic = sr_u32(&r);
    if (magic != EMU_STATE_MAGIC)
        return EMU_EBADSTATE;
    uint32_t ver = sr_u32(&r);
    if (ver != 0x46423130u)
        return EMU_EBADSTATE;
    r.pos = 0;
    fb_deserialize(md, &r);
    if (r.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

static const emu_core_vtable_t fb_vtable = {
    "finalburn",
    "Sega Genesis / Mega Drive",
    FB_SCREEN_W,
    FB_SCREEN_H,
    FB_OUT_RATE,
    fb_create,
    fb_destroy,
    fb_load_rom,
    fb_reset,
    fb_run_frame,
    fb_framebuffer,
    fb_set_input,
    fb_set_audio,
    fb_state_size,
    fb_save_state,
    fb_load_state,
};

const emu_core_vtable_t *emu_core_finalburn(void)
{
    return &fb_vtable;
}
