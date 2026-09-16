/*
 * beatle-nes-redux core lifecycle, frame loop, save states.
 *
 * Save-state format: explicit little-endian fields. The PRG/CHR ROM data is
 * NOT part of the state; loading a state assumes the same cartridge image.
 */
#include "nes.h"
#include "../common/util.h"

#include <stdlib.h>
#include <string.h>

static const emu_core_vtable_t nes_vtable;

static void nes_power_on(nes_t *n)
{
    nes_cpu_power_on(&n->cpu);
    nes_ppu_reset(&n->ppu);
    nes_apu_reset(&n->apu);
    nes_cart_reset(&n->cart);
    memset(&n->bus, 0, sizeof n->bus);
    memset(&n->ctrl, 0, sizeof n->ctrl);
    /* reset vector fetch */
    uint16_t lo = nes_bus_read(n, 0xFFFC);
    uint16_t hi = nes_bus_read(n, 0xFFFD);
    n->cpu.pc = (uint16_t)(lo | (hi << 8));
    n->total_cycles = 0;
}

static emu_result_t nes_create(emu_core_t **out)
{
    if (out == NULL)
        return EMU_EINVAL;
    nes_t *n = calloc(1, sizeof *n);
    if (n == NULL)
        return EMU_EINVAL;
    n->base.vtable = &nes_vtable;
    nes_ppu_init(&n->ppu, n->fb);
    nes_apu_init(&n->apu, n->audio, sizeof n->audio / sizeof n->audio[0]);
    nes_cart_init(&n->cart);
    nes_power_on(n);
    *out = &n->base;
    return EMU_OK;
}

static void nes_destroy(emu_core_t *core)
{
    nes_t *n = (nes_t *)core;
    if (n == NULL)
        return;
    nes_cart_free(&n->cart);
    free(n);
}

static emu_result_t nes_load_rom(emu_core_t *core, const uint8_t *data, size_t size)
{
    nes_t *n = (nes_t *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    emu_result_t r = nes_cart_load(&n->cart, data, size);
    if (r != EMU_OK)
        return r;
    nes_power_on(n);
    return EMU_OK;
}

static void nes_reset(emu_core_t *core)
{
    nes_t *n = (nes_t *)core;
    nes_cpu_reset_regs(&n->cpu);
    nes_apu_reset(&n->apu);
    memset(&n->bus, 0, sizeof n->bus);
    nes_cart_reset(&n->cart);
    uint16_t lo = nes_bus_read(n, 0xFFFC);
    uint16_t hi = nes_bus_read(n, 0xFFFD);
    n->cpu.pc = (uint16_t)(lo | (hi << 8));
}

static emu_result_t nes_run_frame(emu_core_t *core)
{
    nes_t *n = (nes_t *)core;
    if (n->cart.prg == NULL)
        return EMU_ENOROM;

    n->ppu.vblank_event = 0;
    while (!n->ppu.vblank_event) {
        uint32_t cycles = nes_cpu_step(n);
        nes_ppu_run(n, cycles);
        nes_apu_run(n, cycles);
        n->total_cycles += 0;
        /* IRQ level: frame IRQ, DMC IRQ, MMC3 IRQ */
        n->cpu.irq_line = (uint8_t)((n->apu.frame_irq ||
                                     n->apu.dmc.irq_flag ||
                                     n->cart.mmc3_irq_asserted)
                                        ? 1
                                        : 0);
        /* safety: if the ROM disables rendering, vblank still fires at
         * scanline 241 (independent of rendering), so the loop terminates */
        if (n->cpu.pc == 0xFFFF && n->cart.prg == NULL)
            return EMU_ENOROM;
    }

    if (n->audio_cb != NULL && n->apu.out_pos > 0) {
        n->audio_cb(n->audio_user, n->audio, n->apu.out_pos);
        n->apu.out_pos = 0;
    }
    return EMU_OK;
}

static const uint32_t *nes_framebuffer(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    nes_t *n = (nes_t *)core;
    if (w)
        *w = NES_SCREEN_W;
    if (h)
        *h = NES_SCREEN_H;
    return n->fb;
}

static void nes_set_input(emu_core_t *core, uint32_t buttons)
{
    nes_t *n = (nes_t *)core;
    n->ctrl.buttons = buttons & 0xFFu;
    if (n->ctrl.strobe)
        n->ctrl.shift = (uint8_t)(n->ctrl.buttons & 0xFFu);
}

static void nes_set_audio_callback(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    nes_t *n = (nes_t *)core;
    n->audio_cb = cb;
    n->audio_user = user;
}

/* ---- save state ---- */

static void nes_serialize(nes_t *n, emu_state_writer *w)
{
    sw_u32(w, EMU_STATE_MAGIC);
    sw_u32(w, EMU_STATE_VERSION);
    sw_u32(w, 2); /* nes */

    nes_cpu *c = &n->cpu;
    sw_u8(w, c->a); sw_u8(w, c->x); sw_u8(w, c->y); sw_u8(w, c->s); sw_u8(w, c->p);
    sw_u16(w, c->pc);
    sw_u8(w, c->nmi_pending);
    sw_u8(w, c->irq_line);
    sw_u32(w, c->dma_stall);

    sw_mem(w, n->bus.ram, sizeof n->bus.ram);

    nes_ppu *p = &n->ppu;
    sw_mem(w, p->vram, sizeof p->vram);
    sw_mem(w, p->palette, sizeof p->palette);
    sw_mem(w, p->oam, sizeof p->oam);
    sw_u8(w, p->ctrl); sw_u8(w, p->mask); sw_u8(w, p->status);
    sw_u8(w, p->oam_addr); sw_u8(w, p->open_bus);
    sw_u16(w, p->v); sw_u16(w, p->t);
    sw_u8(w, p->fine_x); sw_u8(w, p->w); sw_u8(w, p->read_buffer);
    sw_u16(w, p->scanline); sw_u16(w, p->dot);
    sw_u8(w, p->odd_frame); sw_u8(w, p->vblank_event);
    sw_u8(w, p->nmi_line); sw_u8(w, p->nmi_out_prev);

    nes_apu *a = &n->apu;
    sw_u32(w, a->frame_cycles); sw_u32(w, a->frame_step);
    sw_u8(w, a->mode5); sw_u8(w, a->irq_inhibit); sw_u8(w, a->frame_irq);
    for (int i = 0; i < 2; i++) {
        sw_u8(w, a->pulse[i].enabled); sw_u8(w, a->pulse[i].duty);
        sw_u8(w, a->pulse[i].duty_pos);
        sw_u16(w, a->pulse[i].timer); sw_u16(w, a->pulse[i].period);
        sw_u8(w, a->pulse[i].len_counter); sw_u8(w, a->pulse[i].len_enable);
        sw_u8(w, a->pulse[i].env_volume); sw_u8(w, a->pulse[i].env_period);
        sw_u8(w, a->pulse[i].env_timer); sw_u8(w, a->pulse[i].env_direction);
        sw_u8(w, a->pulse[i].volume); sw_u8(w, a->pulse[i].constant_volume);
        sw_u8(w, a->pulse[i].sweep_enable); sw_u8(w, a->pulse[i].sweep_period);
        sw_u8(w, a->pulse[i].sweep_timer); sw_u8(w, a->pulse[i].sweep_negate);
        sw_u8(w, a->pulse[i].sweep_shift); sw_u8(w, a->pulse[i].sweep_reload);
        sw_u16(w, a->pulse[i].freq); sw_u8(w, a->pulse[i].dac);
    }
    sw_u8(w, a->tri.enabled); sw_u16(w, a->tri.timer); sw_u16(w, a->tri.period);
    sw_u8(w, a->tri.seq_pos); sw_u8(w, a->tri.len_counter);
    sw_u8(w, a->tri.len_enable); sw_u8(w, a->tri.linear_counter);
    sw_u8(w, a->tri.linear_reload); sw_u8(w, a->tri.linear_ctrl);
    sw_u8(w, a->tri.linear_reload_flag);
    sw_u8(w, a->noise.enabled); sw_u16(w, a->noise.timer);
    sw_u16(w, a->noise.period); sw_u16(w, a->noise.lfsr);
    sw_u8(w, a->noise.mode); sw_u8(w, a->noise.len_counter);
    sw_u8(w, a->noise.len_enable); sw_u8(w, a->noise.env_volume);
    sw_u8(w, a->noise.env_period); sw_u8(w, a->noise.env_timer);
    sw_u8(w, a->noise.env_direction); sw_u8(w, a->noise.volume);
    sw_u8(w, a->noise.constant_volume); sw_u8(w, a->noise.dac);
    sw_u8(w, a->dmc.enabled); sw_u8(w, a->dmc.irq_enable); sw_u8(w, a->dmc.loop);
    sw_u16(w, a->dmc.rate); sw_u16(w, a->dmc.timer);
    sw_u8(w, a->dmc.output_level);
    sw_u16(w, a->dmc.sample_addr); sw_u16(w, a->dmc.sample_len);
    sw_u16(w, a->dmc.bytes_remaining); sw_u16(w, a->dmc.addr_counter);
    sw_u8(w, a->dmc.sample_buffer); sw_u8(w, a->dmc.buffer_empty);
    sw_u8(w, a->dmc.shift); sw_u8(w, a->dmc.bits_remaining);
    sw_u8(w, a->dmc.silence); sw_u8(w, a->dmc.irq_flag);
    sw_u32(w, a->sample_acc);
    sw_u32(w, (uint32_t)a->out_pos);

    sw_u8(w, n->ctrl.buttons); sw_u8(w, n->ctrl.shift); sw_u8(w, n->ctrl.strobe);

    nes_cart *ct = &n->cart;
    sw_u8(w, ct->mapper); sw_u8(w, ct->mirroring);
    sw_u8(w, ct->mmc1_shift); sw_u8(w, ct->mmc1_shift_count);
    sw_mem(w, ct->mmc1_regs, sizeof ct->mmc1_regs);
    sw_u8(w, ct->prg_bank); sw_u8(w, ct->chr_bank);
    sw_u8(w, ct->mmc3_reg_select);
    sw_mem(w, ct->mmc3_regs, sizeof ct->mmc3_regs);
    sw_u8(w, ct->mmc3_irq_latch); sw_u8(w, ct->mmc3_irq_counter);
    sw_u8(w, ct->mmc3_irq_reload_pending); sw_u8(w, ct->mmc3_irq_enable);
    sw_u8(w, ct->mmc3_irq_asserted); sw_u8(w, ct->mmc3_a12_prev);
    sw_u8(w, ct->chr_ram);
    sw_u32(w, 0x2000u); /* prg_ram size sanity */
    if (ct->prg_ram != NULL)
        sw_mem(w, ct->prg_ram, 0x2000u);
    sw_u64(w, n->total_cycles);
}

static void nes_deserialize(nes_t *n, emu_state_reader *r)
{
    (void)sr_u32(r); (void)sr_u32(r); (void)sr_u32(r);

    nes_cpu *c = &n->cpu;
    c->a = sr_u8(r); c->x = sr_u8(r); c->y = sr_u8(r); c->s = sr_u8(r);
    c->p = sr_u8(r);
    c->pc = sr_u16(r);
    c->nmi_pending = sr_u8(r);
    c->irq_line = sr_u8(r);
    c->dma_stall = sr_u32(r);

    sr_mem(r, n->bus.ram, sizeof n->bus.ram);

    nes_ppu *p = &n->ppu;
    sr_mem(r, p->vram, sizeof p->vram);
    sr_mem(r, p->palette, sizeof p->palette);
    sr_mem(r, p->oam, sizeof p->oam);
    p->ctrl = sr_u8(r); p->mask = sr_u8(r); p->status = sr_u8(r);
    p->oam_addr = sr_u8(r); p->open_bus = sr_u8(r);
    p->v = sr_u16(r); p->t = sr_u16(r);
    p->fine_x = sr_u8(r); p->w = sr_u8(r); p->read_buffer = sr_u8(r);
    p->scanline = sr_u16(r); p->dot = sr_u16(r);
    p->odd_frame = sr_u8(r); p->vblank_event = sr_u8(r);
    p->nmi_line = sr_u8(r); p->nmi_out_prev = sr_u8(r);

    nes_apu *a = &n->apu;
    a->frame_cycles = sr_u32(r); a->frame_step = sr_u32(r);
    a->mode5 = sr_u8(r); a->irq_inhibit = sr_u8(r); a->frame_irq = sr_u8(r);
    for (int i = 0; i < 2; i++) {
        a->pulse[i].enabled = sr_u8(r); a->pulse[i].duty = sr_u8(r);
        a->pulse[i].duty_pos = sr_u8(r);
        a->pulse[i].timer = sr_u16(r); a->pulse[i].period = sr_u16(r);
        a->pulse[i].len_counter = sr_u8(r); a->pulse[i].len_enable = sr_u8(r);
        a->pulse[i].env_volume = sr_u8(r); a->pulse[i].env_period = sr_u8(r);
        a->pulse[i].env_timer = sr_u8(r); a->pulse[i].env_direction = sr_u8(r);
        a->pulse[i].volume = sr_u8(r); a->pulse[i].constant_volume = sr_u8(r);
        a->pulse[i].sweep_enable = sr_u8(r); a->pulse[i].sweep_period = sr_u8(r);
        a->pulse[i].sweep_timer = sr_u8(r); a->pulse[i].sweep_negate = sr_u8(r);
        a->pulse[i].sweep_shift = sr_u8(r); a->pulse[i].sweep_reload = sr_u8(r);
        a->pulse[i].freq = sr_u16(r); a->pulse[i].dac = sr_u8(r);
    }
    a->tri.enabled = sr_u8(r); a->tri.timer = sr_u16(r); a->tri.period = sr_u16(r);
    a->tri.seq_pos = sr_u8(r); a->tri.len_counter = sr_u8(r);
    a->tri.len_enable = sr_u8(r); a->tri.linear_counter = sr_u8(r);
    a->tri.linear_reload = sr_u8(r); a->tri.linear_ctrl = sr_u8(r);
    a->tri.linear_reload_flag = sr_u8(r);
    a->noise.enabled = sr_u8(r); a->noise.timer = sr_u16(r);
    a->noise.period = sr_u16(r); a->noise.lfsr = sr_u16(r);
    a->noise.mode = sr_u8(r); a->noise.len_counter = sr_u8(r);
    a->noise.len_enable = sr_u8(r); a->noise.env_volume = sr_u8(r);
    a->noise.env_period = sr_u8(r); a->noise.env_timer = sr_u8(r);
    a->noise.env_direction = sr_u8(r); a->noise.volume = sr_u8(r);
    a->noise.constant_volume = sr_u8(r); a->noise.dac = sr_u8(r);
    a->dmc.enabled = sr_u8(r); a->dmc.irq_enable = sr_u8(r); a->dmc.loop = sr_u8(r);
    a->dmc.rate = sr_u16(r); a->dmc.timer = sr_u16(r);
    a->dmc.output_level = sr_u8(r);
    a->dmc.sample_addr = sr_u16(r); a->dmc.sample_len = sr_u16(r);
    a->dmc.bytes_remaining = sr_u16(r); a->dmc.addr_counter = sr_u16(r);
    a->dmc.sample_buffer = sr_u8(r); a->dmc.buffer_empty = sr_u8(r);
    a->dmc.shift = sr_u8(r); a->dmc.bits_remaining = sr_u8(r);
    a->dmc.silence = sr_u8(r); a->dmc.irq_flag = sr_u8(r);
    a->sample_acc = sr_u32(r);
    a->out_pos = sr_u32(r);
    if (a->out_pos > a->out_cap)
        a->out_pos = 0;

    n->ctrl.buttons = sr_u8(r); n->ctrl.shift = sr_u8(r); n->ctrl.strobe = sr_u8(r);

    nes_cart *ct = &n->cart;
    ct->mapper = sr_u8(r); ct->mirroring = sr_u8(r);
    ct->mmc1_shift = sr_u8(r); ct->mmc1_shift_count = sr_u8(r);
    sr_mem(r, ct->mmc1_regs, sizeof ct->mmc1_regs);
    ct->prg_bank = sr_u8(r); ct->chr_bank = sr_u8(r);
    ct->mmc3_reg_select = sr_u8(r);
    sr_mem(r, ct->mmc3_regs, sizeof ct->mmc3_regs);
    ct->mmc3_irq_latch = sr_u8(r); ct->mmc3_irq_counter = sr_u8(r);
    ct->mmc3_irq_reload_pending = sr_u8(r); ct->mmc3_irq_enable = sr_u8(r);
    ct->mmc3_irq_asserted = sr_u8(r); ct->mmc3_a12_prev = sr_u8(r);
    ct->chr_ram = sr_u8(r);
    (void)sr_u32(r); /* prg_ram size sanity field (always 0x2000) */
    if (ct->prg_ram != NULL)
        sr_mem(r, ct->prg_ram, 0x2000u);
    n->total_cycles = sr_u64(r);
}

static size_t nes_state_size(emu_core_t *core)
{
    nes_t *n = (nes_t *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    nes_serialize(n, &w);
    return w.pos;
}

static emu_result_t nes_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    nes_t *n = (nes_t *)core;
    if (buf == NULL)
        return EMU_EINVAL;
    if (n->cart.prg == NULL)
        return EMU_ENOROM;
    emu_state_writer w = { buf, cap, 0, 0 };
    nes_serialize(n, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t nes_load_state(emu_core_t *core, const uint8_t *buf, size_t size)
{
    nes_t *n = (nes_t *)core;
    if (buf == NULL || size == 0)
        return EMU_EINVAL;
    if (size < 12)
        return EMU_EBADSTATE;
    if (emu_le32(buf) != EMU_STATE_MAGIC ||
        emu_le32(buf + 4) != EMU_STATE_VERSION ||
        emu_le32(buf + 8) != 2)
        return EMU_EBADSTATE;
    if (n->cart.prg == NULL)
        return EMU_ENOROM;

    emu_state_reader r = { buf, size, 0, 0 };
    nes_deserialize(n, &r);
    if (r.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

static const emu_core_vtable_t nes_vtable = {
    "beatle-nes-redux",
    "Nintendo Entertainment System",
    NES_SCREEN_W,
    NES_SCREEN_H,
    NES_OUT_RATE,
    nes_create,
    nes_destroy,
    nes_load_rom,
    nes_reset,
    nes_run_frame,
    nes_framebuffer,
    nes_set_input,
    nes_set_audio_callback,
    nes_state_size,
    nes_save_state,
    nes_load_state
};

const emu_core_vtable_t *emu_core_beatle_nes_redux(void)
{
    return &nes_vtable;
}
