/*
 * supersnes core lifecycle, frame loop, save states.
 *
 * The S-SMP/DSP are stubs (see apu.c) — audio output is silent, which is a
 * documented limitation of this milestone.
 */
#include "snes.h"
#include "../common/util.h"

#include <stdlib.h>
#include <string.h>

static const emu_core_vtable_t snes_vtable;

static void snes_power_on(snes_t *s)
{
    snes_cpu_power_on(&s->cpu);
    snes_mem_reset(&s->mem);
    snes_ppu_reset(&s->ppu);
    snes_dma_reset(&s->dma);
    snes_apu_reset(&s->apu);
    /* NOTE: the cartridge is initialized by load_rom/destroy, not here */
    /* reset vector */
    s->cpu.pc = (uint16_t)(snes_bus_read(s, 0x00FFFCu) |
                           ((uint16_t)snes_bus_read(s, 0x00FFFDu) << 8));
    s->total_cycles = 0;
}

static emu_result_t snes_create(emu_core_t **out)
{
    if (out == NULL)
        return EMU_EINVAL;
    snes_t *s = calloc(1, sizeof *s);
    if (s == NULL)
        return EMU_EINVAL;
    s->base.vtable = &snes_vtable;
    snes_ppu_init(&s->ppu, s->fb);
    *out = &s->base;
    return EMU_OK;
}

static void snes_destroy(emu_core_t *core)
{
    snes_t *s = (snes_t *)core;
    if (s == NULL)
        return;
    snes_cart_free(&s->cart);
    free(s);
}

static emu_result_t snes_load_rom(emu_core_t *core, const uint8_t *data, size_t size)
{
    snes_t *s = (snes_t *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    emu_result_t r = snes_cart_load(&s->cart, data, size);
    if (r != EMU_OK)
        return r;
    snes_power_on(s);
    return EMU_OK;
}

static void snes_reset(emu_core_t *core)
{
    snes_t *s = (snes_t *)core;
    snes_cpu_reset_regs(&s->cpu);
    snes_mem_reset(&s->mem);
    snes_ppu_reset(&s->ppu);
    snes_dma_reset(&s->dma);
    snes_apu_reset(&s->apu);
    s->cpu.pc = (uint16_t)(snes_bus_read(s, 0x00FFFCu) |
                           ((uint16_t)snes_bus_read(s, 0x00FFFDu) << 8));
}

static emu_result_t snes_run_frame(emu_core_t *core)
{
    snes_t *s = (snes_t *)core;
    if (s->cart.rom == NULL)
        return EMU_ENOROM;

    s->ppu.line = 0;
    s->ppu.dot = 0;
    /* run one video frame: 262 lines x 1364 dots x 4 master clocks; CPU
     * runs at 6 master clocks per cycle (slow-ROM; FastROM not modeled) */
    uint64_t target_master = 262ull * 1364ull * 4ull;
    uint64_t done_master = 0;
    while (done_master < target_master) {
        uint32_t cycles = snes_cpu_step(s);
        uint32_t master = cycles * 6u;
        snes_ppu_run(s, master);
        done_master += master;
    }
    /* audio: silent (S-SMP/DSP stub, documented) */
    return EMU_OK;
}

static const uint32_t *snes_framebuffer(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    snes_t *s = (snes_t *)core;
    if (w)
        *w = SNES_SCREEN_W;
    if (h)
        *h = SNES_SCREEN_H;
    return s->fb;
}

static void snes_set_input(emu_core_t *core, uint32_t buttons)
{
    snes_t *s = (snes_t *)core;
    s->buttons[0] = buttons & 0xFFFu; /* SNES layout, see emu.h */
    s->buttons[1] = 0;                /* controller 2 unused */
}

static void snes_set_audio_callback(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    snes_t *s = (snes_t *)core;
    s->audio_cb = cb;
    s->audio_user = user;
}

/* ---- save state ---- */

static void snes_serialize(snes_t *s, emu_state_writer *w)
{
    sw_u32(w, EMU_STATE_MAGIC);
    sw_u32(w, EMU_STATE_VERSION);
    sw_u32(w, 3); /* supersnes */

    snes_cpu *c = &s->cpu;
    sw_u16(w, c->a); sw_u16(w, c->x); sw_u16(w, c->y);
    sw_u16(w, c->sp); sw_u16(w, c->pc);
    sw_u8(w, c->dbr); sw_u8(w, c->k); sw_u16(w, c->dp); sw_u8(w, c->p);
    sw_u8(w, c->e); sw_u8(w, c->nmi_pending); sw_u8(w, c->irq_line);
    sw_u8(w, c->wai); sw_u8(w, c->stp);

    sw_mem(w, s->mem.wram, sizeof s->mem.wram);
    sw_u8(w, s->mem.nmitimen);
    sw_u8(w, s->mem.wrmpya); sw_u8(w, s->mem.wrmpyb);
    sw_u16(w, s->mem.wrdiv); sw_u8(w, s->mem.wrdivb);
    sw_u16(w, s->mem.htime); sw_u16(w, s->mem.vtime);
    sw_u16(w, s->mem.rddiv); sw_u16(w, s->mem.rdmpy);
    sw_mem(w, s->mem.joypad_auto, sizeof s->mem.joypad_auto);

    snes_ppu *p = &s->ppu;
    sw_mem(w, p->vram, sizeof p->vram);
    sw_mem(w, p->cgram, sizeof p->cgram);
    sw_mem(w, p->oam, sizeof p->oam);
    sw_u8(w, p->inidisp); sw_u8(w, p->bgmode);
    for (int i = 0; i < 8; i++)
        sw_u16(w, p->bg_scroll[i]);
    sw_u8(w, p->bg_map[0]); sw_u8(w, p->bg_map[1]);
    sw_u8(w, p->bg_map[2]); sw_u8(w, p->bg_map[3]);
    for (int i = 0; i < 5; i++)
        sw_u8(w, p->bg_char_base[i]);
    sw_u8(w, p->obj_size_reg); sw_u8(w, p->obj_8bpp);
    sw_u16(w, p->oamaddr);
    sw_u8(w, p->vmainc); sw_u16(w, p->vmadd);
    sw_u8(w, p->vm_lo_latch); sw_u8(w, p->vm_hi_latch);
    sw_u8(w, p->cgaddr); sw_u8(w, p->cgaddr_flip);
    sw_u8(w, p->tm); sw_u8(w, p->ts);
    sw_u8(w, p->m7sel);
    sw_u16(w, p->m7a); sw_u16(w, p->m7b); sw_u16(w, p->m7c); sw_u16(w, p->m7d);
    sw_u16(w, p->m7x); sw_u16(w, p->m7y);
    sw_u16(w, p->line); sw_u16(w, p->dot); sw_u32(w, p->dot_acc);

    sw_u8(w, s->dma.mdmaen); sw_u8(w, s->dma.hdmaen);
    for (int i = 0; i < 8; i++) {
        sw_u8(w, s->dma.ch[i].params);
        sw_u8(w, s->dma.ch[i].bbus);
        sw_u16(w, s->dma.ch[i].abus);
        sw_u8(w, s->dma.ch[i].abank);
        sw_u16(w, s->dma.ch[i].count);
        sw_u8(w, s->dma.ch[i].ibank);
    }
    sw_mem(w, s->apu.ports, sizeof s->apu.ports);
    sw_mem(w, s->cart.sram != NULL ? s->cart.sram : NULL, s->cart.sram_size);
    sw_u64(w, s->total_cycles);
}

static void snes_deserialize(snes_t *s, emu_state_reader *r)
{
    (void)sr_u32(r); (void)sr_u32(r); (void)sr_u32(r);

    snes_cpu *c = &s->cpu;
    c->a = sr_u16(r); c->x = sr_u16(r); c->y = sr_u16(r);
    c->sp = sr_u16(r); c->pc = sr_u16(r);
    c->dbr = sr_u8(r); c->k = sr_u8(r); c->dp = sr_u16(r); c->p = sr_u8(r);
    c->e = sr_u8(r); c->nmi_pending = sr_u8(r); c->irq_line = sr_u8(r);
    c->wai = sr_u8(r); c->stp = sr_u8(r);

    sr_mem(r, s->mem.wram, sizeof s->mem.wram);
    s->mem.nmitimen = sr_u8(r);
    s->mem.wrmpya = sr_u8(r); s->mem.wrmpyb = sr_u8(r);
    s->mem.wrdiv = sr_u16(r); s->mem.wrdivb = sr_u8(r);
    s->mem.htime = sr_u16(r); s->mem.vtime = sr_u16(r);
    s->mem.rddiv = sr_u16(r); s->mem.rdmpy = sr_u16(r);
    sr_mem(r, s->mem.joypad_auto, sizeof s->mem.joypad_auto);

    snes_ppu *p = &s->ppu;
    sr_mem(r, p->vram, sizeof p->vram);
    sr_mem(r, p->cgram, sizeof p->cgram);
    sr_mem(r, p->oam, sizeof p->oam);
    p->inidisp = sr_u8(r); p->bgmode = sr_u8(r);
    for (int i = 0; i < 8; i++)
        p->bg_scroll[i] = sr_u16(r);
    p->bg_map[0] = sr_u8(r); p->bg_map[1] = sr_u8(r);
    p->bg_map[2] = sr_u8(r); p->bg_map[3] = sr_u8(r);
    for (int i = 0; i < 5; i++)
        p->bg_char_base[i] = sr_u8(r);
    p->obj_size_reg = sr_u8(r); p->obj_8bpp = sr_u8(r);
    p->oamaddr = sr_u16(r);
    p->vmainc = sr_u8(r); p->vmadd = sr_u16(r);
    p->vm_lo_latch = sr_u8(r); p->vm_hi_latch = sr_u8(r);
    p->cgaddr = sr_u8(r); p->cgaddr_flip = sr_u8(r);
    p->tm = sr_u8(r); p->ts = sr_u8(r);
    p->m7sel = sr_u8(r);
    p->m7a = sr_u16(r); p->m7b = sr_u16(r); p->m7c = sr_u16(r); p->m7d = sr_u16(r);
    p->m7x = sr_u16(r); p->m7y = sr_u16(r);
    p->line = sr_u16(r); p->dot = sr_u16(r); p->dot_acc = sr_u32(r);

    s->dma.mdmaen = sr_u8(r); s->dma.hdmaen = sr_u8(r);
    for (int i = 0; i < 8; i++) {
        s->dma.ch[i].params = sr_u8(r);
        s->dma.ch[i].bbus = sr_u8(r);
        s->dma.ch[i].abus = sr_u16(r);
        s->dma.ch[i].abank = sr_u8(r);
        s->dma.ch[i].count = sr_u16(r);
        s->dma.ch[i].ibank = sr_u8(r);
    }
    sr_mem(r, s->apu.ports, sizeof s->apu.ports);
    if (s->cart.sram != NULL && s->cart.sram_size > 0u)
        sr_mem(r, s->cart.sram, s->cart.sram_size);
    s->total_cycles = sr_u64(r);
}

static size_t snes_state_size(emu_core_t *core)
{
    snes_t *s = (snes_t *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    snes_serialize(s, &w);
    return w.pos;
}

static emu_result_t snes_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    snes_t *s = (snes_t *)core;
    if (buf == NULL)
        return EMU_EINVAL;
    if (s->cart.rom == NULL)
        return EMU_ENOROM;
    
    /* Contract: an undersized buffer must be rejected without writing. */
    if (cap < snes_state_size(core))
        return EMU_ENOSPACE;
    emu_state_writer w = { buf, cap, 0, 0 };
    snes_serialize(s, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t snes_load_state(emu_core_t *core, const uint8_t *buf, size_t size)
{
    snes_t *s = (snes_t *)core;
    if (buf == NULL || size == 0)
        return EMU_EINVAL;
    if (size < 12)
        return EMU_EBADSTATE;
    if (emu_le32(buf) != EMU_STATE_MAGIC ||
        emu_le32(buf + 4) != EMU_STATE_VERSION ||
        emu_le32(buf + 8) != 3)
        return EMU_EBADSTATE;
    if (s->cart.rom == NULL)
        return EMU_ENOROM;

    emu_state_reader r = { buf, size, 0, 0 };
    snes_deserialize(s, &r);
    if (r.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

static const emu_core_vtable_t snes_vtable = {
    "supersnes",
    "Super Nintendo",
    SNES_SCREEN_W,
    SNES_SCREEN_H,
    SNES_OUT_RATE,
    snes_create,
    snes_destroy,
    snes_load_rom,
    snes_reset,
    snes_run_frame,
    snes_framebuffer,
    snes_set_input,
    snes_set_audio_callback,
    snes_state_size,
    snes_save_state,
    snes_load_state
};

const emu_core_vtable_t *emu_core_supersnes(void)
{
    return &snes_vtable;
}
