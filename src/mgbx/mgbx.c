/*
 * mgbx core lifecycle: vtable implementation, frame loop, save states.
 *
 * Save-state format (version 1): explicit little-endian fields, no pointers.
 * The ROM image itself is NOT part of the state; a state is only meaningful
 * for a core with the same cartridge loaded.
 */
#include "mgbx.h"
#include "../common/util.h"

#include <stdlib.h>
#include <string.h>

static const emu_core_vtable_t mgbx_vtable;

/* ---- power-on defaults (no boot ROM; documented DMG post-boot values) ---- */

static void mgbx_power_on(struct mgbx *gb)
{
    memset(&gb->mem, 0, sizeof gb->mem);
    gb->mem.if_reg = 0xE1u;
    gb->mem.ie = 0x00;

    gb_cpu_reset_for_no_bootrom(&gb->cpu);

    gb_ppu_reset(&gb->ppu);
    gb_timer_reset(&gb->timer);
    gb_apu_reset(&gb->apu);
    gb_cart_reset(&gb->cart);
    gb->joypad.select = 0x30u;
    gb->joypad.buttons = 0;
    gb->total_cycles = 0;
}

/* ---- vtable ---- */

static emu_result_t mgbx_create(emu_core_t **out)
{
    if (out == NULL)
        return EMU_EINVAL;
    struct mgbx *gb = calloc(1, sizeof *gb);
    if (gb == NULL)
        return EMU_EINVAL;
    gb->base.vtable = &mgbx_vtable;
    gb_ppu_init(&gb->ppu, gb->fb);
    gb_apu_init(&gb->apu, gb->audio, sizeof gb->audio / sizeof gb->audio[0]);
    gb_cart_init(&gb->cart);
    mgbx_power_on(gb);
    *out = &gb->base;
    return EMU_OK;
}

static void mgbx_destroy(emu_core_t *core)
{
    struct mgbx *gb = (struct mgbx *)core;
    if (gb == NULL)
        return;
    gb_cart_free(&gb->cart);
    free(gb);
}

static emu_result_t mgbx_load_rom(emu_core_t *core, const uint8_t *data, size_t size)
{
    struct mgbx *gb = (struct mgbx *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    emu_result_t r = gb_cart_load(&gb->cart, data, size);
    if (r != EMU_OK)
        return r;
    mgbx_power_on(gb); /* inserting a cart powers the machine on */
    return EMU_OK;
}

static void mgbx_reset(emu_core_t *core)
{
    struct mgbx *gb = (struct mgbx *)core;
    mgbx_power_on(gb);
}

static emu_result_t mgbx_run_frame(emu_core_t *core)
{
    struct mgbx *gb = (struct mgbx *)core;
    if (gb->cart.rom == NULL)
        return EMU_ENOROM;

    uint32_t acc = 0;
    while (acc < GB_CYCLES_PER_FRAME) {
        uint32_t t = gb_cpu_step(gb);
        acc += t;
        gb_ppu_step(gb, t);
        gb_timer_step(gb, t);
        gb_apu_step(gb, t);
        gb_cart_tick_rtc(&gb->cart, t);
    }

    if (gb->audio_cb != NULL && gb->apu.out_pos > 0) {
        gb->audio_cb(gb->audio_user, gb->audio, gb->apu.out_pos);
        gb->apu.out_pos = 0;
    }
    return EMU_OK;
}

static const uint32_t *mgbx_framebuffer(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct mgbx *gb = (struct mgbx *)core;
    if (w)
        *w = GB_SCREEN_W;
    if (h)
        *h = GB_SCREEN_H;
    return gb->fb;
}

static void mgbx_set_input(emu_core_t *core, uint32_t buttons)
{
    struct mgbx *gb = (struct mgbx *)core;
    gb_joypad_set_buttons(&gb->joypad, buttons, gb);
}

static void mgbx_set_audio_callback(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    struct mgbx *gb = (struct mgbx *)core;
    gb->audio_cb = cb;
    gb->audio_user = user;
}

/* ---- save state ---- */

static void mgbx_serialize(struct mgbx *gb, emu_state_writer *w)
{
    sw_u32(w, EMU_STATE_MAGIC);
    sw_u32(w, EMU_STATE_VERSION);
    sw_u32(w, 1); /* core state format id: mgbx */

    gb_cpu *c = &gb->cpu;
    sw_u8(w, c->a); sw_u8(w, c->f); sw_u8(w, c->b); sw_u8(w, c->c);
    sw_u8(w, c->d); sw_u8(w, c->e); sw_u8(w, c->h); sw_u8(w, c->l);
    sw_u16(w, c->sp); sw_u16(w, c->pc);
    sw_u8(w, c->ime); sw_u8(w, c->ime_pending);
    sw_u8(w, c->halted); sw_u8(w, c->stopped); sw_u8(w, c->halt_bug);

    sw_mem(w, gb->mem.wram, sizeof gb->mem.wram);
    sw_mem(w, gb->mem.hram, sizeof gb->mem.hram);
    sw_u8(w, gb->mem.ie);
    sw_u8(w, gb->mem.if_reg);
    sw_u8(w, gb->mem.sb);
    sw_u8(w, gb->mem.sc);
    sw_u8(w, gb->mem.oam_dma_page);
    sw_u8(w, gb->mem.dma_active);
    sw_u8(w, gb->mem.dma_value);
    sw_u8(w, gb->mem.dma_index);

    sw_mem(w, gb->ppu.vram, sizeof gb->ppu.vram);
    sw_mem(w, gb->ppu.oam, sizeof gb->ppu.oam);
    sw_u8(w, gb->ppu.lcdc); sw_u8(w, gb->ppu.stat);
    sw_u8(w, gb->ppu.scy); sw_u8(w, gb->ppu.scx);
    sw_u8(w, gb->ppu.ly); sw_u8(w, gb->ppu.lyc);
    sw_u8(w, gb->ppu.bgp); sw_u8(w, gb->ppu.obp0); sw_u8(w, gb->ppu.obp1);
    sw_u8(w, gb->ppu.wy); sw_u8(w, gb->ppu.wx);
    sw_u16(w, gb->ppu.dot);
    sw_u8(w, gb->ppu.win_line);

    sw_u16(w, gb->timer.counter);
    sw_u8(w, gb->timer.tima);
    sw_u8(w, gb->timer.tma);
    sw_u8(w, gb->timer.tac);
    sw_u8(w, gb->timer.tima_reload);

    gb_apu *apu = &gb->apu;
    sw_u8(w, apu->enabled); sw_u8(w, apu->nr50); sw_u8(w, apu->nr51);
    for (int i = 0; i < 2; i++) {
        sw_u8(w, apu->sq[i].duty);
        sw_u8(w, apu->sq[i].duty_pos);
        sw_u32(w, apu->sq[i].freq_timer);
        sw_u16(w, apu->sq[i].freq);
        sw_u8(w, apu->sq[i].len_counter);
        sw_u8(w, apu->sq[i].len_enable);
        sw_u8(w, apu->sq[i].env_period);
        sw_u8(w, apu->sq[i].env_timer);
        sw_u8(w, apu->sq[i].env_direction);
        sw_u8(w, apu->sq[i].volume);
        sw_u8(w, apu->sq[i].dac_enable);
        sw_u8(w, apu->sq[i].sweep_period);
        sw_u8(w, apu->sq[i].sweep_timer);
        sw_u8(w, apu->sq[i].sweep_shift);
        sw_u8(w, apu->sq[i].sweep_negate);
        sw_u8(w, apu->sq[i].sweep_enabled);
        sw_u8(w, apu->sq[i].active);
    }
    sw_u8(w, apu->wave.dac_enable);
    sw_u8(w, apu->wave.volume_shift);
    sw_u32(w, apu->wave.freq_timer);
    sw_u16(w, apu->wave.freq);
    sw_u8(w, apu->wave.len_counter);
    sw_u8(w, apu->wave.len_enable);
    sw_u8(w, apu->wave.pos);
    sw_u8(w, apu->wave.sample);
    sw_u8(w, apu->wave.active);
    sw_u16(w, apu->noise.lfsr);
    sw_u8(w, apu->noise.width_mode);
    sw_u8(w, apu->noise.divisor_code);
    sw_u8(w, apu->noise.shift_clock);
    sw_u32(w, apu->noise.freq_timer);
    sw_u8(w, apu->noise.len_counter);
    sw_u8(w, apu->noise.len_enable);
    sw_u8(w, apu->noise.env_period);
    sw_u8(w, apu->noise.env_timer);
    sw_u8(w, apu->noise.env_direction);
    sw_u8(w, apu->noise.volume);
    sw_u8(w, apu->noise.dac_enable);
    sw_u8(w, apu->noise.active);
    sw_mem(w, apu->wave_ram, sizeof apu->wave_ram);
    sw_u16(w, apu->frame_seq_timer);
    sw_u8(w, apu->frame_seq_step);
    sw_u32(w, apu->sample_acc);
    sw_u32(w, (uint32_t)apu->out_pos);

    sw_u8(w, gb->joypad.select);
    sw_u32(w, gb->joypad.buttons);

    gb_cart *cart = &gb->cart;
    sw_u8(w, cart->type);
    sw_u8(w, cart->ram_enabled);
    sw_u8(w, cart->bank1);
    sw_u8(w, cart->bank2);
    sw_u8(w, cart->mode);
    sw_u16(w, cart->rom_bank);
    sw_u8(w, cart->rtc_halt);
    sw_u8(w, cart->rtc_latch_state);
    sw_u8(w, cart->rtc_latched_valid);
    sw_mem(w, cart->rtc, sizeof cart->rtc);
    sw_mem(w, cart->rtc_latched, sizeof cart->rtc_latched);
    sw_u64(w, cart->rtc_divider);
    sw_u32(w, (uint32_t)cart->ram_size);
    if (cart->ram != NULL && cart->ram_size > 0)
        sw_mem(w, cart->ram, cart->ram_size);

    sw_u64(w, gb->total_cycles);
}

static void mgbx_deserialize(struct mgbx *gb, emu_state_reader *r)
{
    (void)sr_u32(r); /* magic, checked by caller */
    (void)sr_u32(r); /* version, checked by caller */
    (void)sr_u32(r); /* format id */

    gb_cpu *c = &gb->cpu;
    c->a = sr_u8(r); c->f = sr_u8(r); c->b = sr_u8(r); c->c = sr_u8(r);
    c->d = sr_u8(r); c->e = sr_u8(r); c->h = sr_u8(r); c->l = sr_u8(r);
    c->sp = sr_u16(r); c->pc = sr_u16(r);
    c->ime = sr_u8(r); c->ime_pending = sr_u8(r);
    c->halted = sr_u8(r); c->stopped = sr_u8(r); c->halt_bug = sr_u8(r);

    sr_mem(r, gb->mem.wram, sizeof gb->mem.wram);
    sr_mem(r, gb->mem.hram, sizeof gb->mem.hram);
    gb->mem.ie = sr_u8(r);
    gb->mem.if_reg = sr_u8(r);
    gb->mem.sb = sr_u8(r);
    gb->mem.sc = sr_u8(r);
    gb->mem.oam_dma_page = sr_u8(r);
    gb->mem.dma_active = sr_u8(r);
    gb->mem.dma_value = sr_u8(r);
    gb->mem.dma_index = sr_u8(r);

    sr_mem(r, gb->ppu.vram, sizeof gb->ppu.vram);
    sr_mem(r, gb->ppu.oam, sizeof gb->ppu.oam);
    gb->ppu.lcdc = sr_u8(r); gb->ppu.stat = sr_u8(r);
    gb->ppu.scy = sr_u8(r); gb->ppu.scx = sr_u8(r);
    gb->ppu.ly = sr_u8(r); gb->ppu.lyc = sr_u8(r);
    gb->ppu.bgp = sr_u8(r); gb->ppu.obp0 = sr_u8(r); gb->ppu.obp1 = sr_u8(r);
    gb->ppu.wy = sr_u8(r); gb->ppu.wx = sr_u8(r);
    gb->ppu.dot = sr_u16(r);
    gb->ppu.win_line = sr_u8(r);

    gb->timer.counter = sr_u16(r);
    gb->timer.tima = sr_u8(r);
    gb->timer.tma = sr_u8(r);
    gb->timer.tac = sr_u8(r);
    gb->timer.tima_reload = sr_u8(r);

    gb_apu *apu = &gb->apu;
    apu->enabled = sr_u8(r); apu->nr50 = sr_u8(r); apu->nr51 = sr_u8(r);
    for (int i = 0; i < 2; i++) {
        apu->sq[i].duty = sr_u8(r);
        apu->sq[i].duty_pos = sr_u8(r);
        apu->sq[i].freq_timer = sr_u32(r);
        apu->sq[i].freq = sr_u16(r);
        apu->sq[i].len_counter = sr_u8(r);
        apu->sq[i].len_enable = sr_u8(r);
        apu->sq[i].env_period = sr_u8(r);
        apu->sq[i].env_timer = sr_u8(r);
        apu->sq[i].env_direction = sr_u8(r);
        apu->sq[i].volume = sr_u8(r);
        apu->sq[i].dac_enable = sr_u8(r);
        apu->sq[i].sweep_period = sr_u8(r);
        apu->sq[i].sweep_timer = sr_u8(r);
        apu->sq[i].sweep_shift = sr_u8(r);
        apu->sq[i].sweep_negate = sr_u8(r);
        apu->sq[i].sweep_enabled = sr_u8(r);
        apu->sq[i].active = sr_u8(r);
    }
    apu->wave.dac_enable = sr_u8(r);
    apu->wave.volume_shift = sr_u8(r);
    apu->wave.freq_timer = sr_u32(r);
    apu->wave.freq = sr_u16(r);
    apu->wave.len_counter = sr_u8(r);
    apu->wave.len_enable = sr_u8(r);
    apu->wave.pos = sr_u8(r);
    apu->wave.sample = sr_u8(r);
    apu->wave.active = sr_u8(r);
    apu->noise.lfsr = sr_u16(r);
    apu->noise.width_mode = sr_u8(r);
    apu->noise.divisor_code = sr_u8(r);
    apu->noise.shift_clock = sr_u8(r);
    apu->noise.freq_timer = sr_u32(r);
    apu->noise.len_counter = sr_u8(r);
    apu->noise.len_enable = sr_u8(r);
    apu->noise.env_period = sr_u8(r);
    apu->noise.env_timer = sr_u8(r);
    apu->noise.env_direction = sr_u8(r);
    apu->noise.volume = sr_u8(r);
    apu->noise.dac_enable = sr_u8(r);
    apu->noise.active = sr_u8(r);
    sr_mem(r, apu->wave_ram, sizeof apu->wave_ram);
    apu->frame_seq_timer = sr_u16(r);
    apu->frame_seq_step = sr_u8(r);
    apu->sample_acc = sr_u32(r);
    apu->out_pos = sr_u32(r);
    if (apu->out_pos > apu->out_cap)
        apu->out_pos = 0; /* defensive: never trust state blindly */

    gb->joypad.select = sr_u8(r);
    gb->joypad.buttons = sr_u32(r);

    gb_cart *cart = &gb->cart;
    cart->type = sr_u8(r);
    cart->ram_enabled = sr_u8(r);
    cart->bank1 = sr_u8(r);
    cart->bank2 = sr_u8(r);
    cart->mode = sr_u8(r);
    cart->rom_bank = sr_u16(r);
    cart->rtc_halt = sr_u8(r);
    cart->rtc_latch_state = sr_u8(r);
    cart->rtc_latched_valid = sr_u8(r);
    sr_mem(r, cart->rtc, sizeof cart->rtc);
    sr_mem(r, cart->rtc_latched, sizeof cart->rtc_latched);
    cart->rtc_divider = sr_u64(r);
    uint32_t ram_size = sr_u32(r);
    if (ram_size == (uint32_t)cart->ram_size && cart->ram != NULL && ram_size > 0)
        sr_mem(r, cart->ram, ram_size);
    else if (ram_size > 0)
        r->bad = 1; /* state belongs to a different cartridge RAM layout */

    gb->total_cycles = sr_u64(r);
}

static size_t mgbx_state_size(emu_core_t *core)
{
    struct mgbx *gb = (struct mgbx *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    mgbx_serialize(gb, &w);
    return w.pos;
}

static emu_result_t mgbx_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    struct mgbx *gb = (struct mgbx *)core;
    if (buf == NULL)
        return EMU_EINVAL;
    if (gb->cart.rom == NULL)
        return EMU_ENOROM;
    
    /* Contract: an undersized buffer must be rejected without writing. */
    if (cap < mgbx_state_size(core))
        return EMU_ENOSPACE;
    emu_state_writer w = { buf, cap, 0, 0 };
    mgbx_serialize(gb, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t mgbx_load_state(emu_core_t *core, const uint8_t *buf, size_t size)
{
    struct mgbx *gb = (struct mgbx *)core;
    if (buf == NULL || size == 0)
        return EMU_EINVAL;
    if (size < 12)
        return EMU_EBADSTATE;
    if (emu_le32(buf) != EMU_STATE_MAGIC ||
        emu_le32(buf + 4) != EMU_STATE_VERSION ||
        emu_le32(buf + 8) != 1)
        return EMU_EBADSTATE;
    if (gb->cart.rom == NULL)
        return EMU_ENOROM;

    emu_state_reader r = { buf, size, 0, 0 };
    mgbx_deserialize(gb, &r);
    if (r.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

static const emu_core_vtable_t mgbx_vtable = {
    "mgbx",
    "Game Boy (DMG)",
    GB_SCREEN_W,
    GB_SCREEN_H,
    32768,
    mgbx_create,
    mgbx_destroy,
    mgbx_load_rom,
    mgbx_reset,
    mgbx_run_frame,
    mgbx_framebuffer,
    mgbx_set_input,
    mgbx_set_audio_callback,
    mgbx_state_size,
    mgbx_save_state,
    mgbx_load_state
};

const emu_core_vtable_t *emu_core_mgbx(void)
{
    return &mgbx_vtable;
}
