/*
 * mgbax core lifecycle, frame loop, save states.
 *
 * Direct boot (no BIOS ROM): the CPU starts at $08000000 in Supervisor mode
 * with BIOS-equivalent default stacks (SP_svc=$03007FE0, SP_irq=$03007FA0,
 * SP_sys=$03007F00). IRQs are dispatched through the HLE BIOS dispatcher
 * (see swi.c): the game handler pointer at $03007FFC must be installed by
 * the ROM for IRQs to be delivered, and the handler must acknowledge IF.
 */
#include "gba.h"
#include "../common/util.h"

#include <stdlib.h>
#include <string.h>

static const emu_core_vtable_t gba_vtable;

#define GBA_CPU_HZ 16777216u
#define GBA_LINES_PER_FRAME 228u
#define GBA_CYCLES_PER_LINE 1232u

static emu_result_t gba_create(emu_core_t **out)
{
    if (out == NULL)
        return EMU_EINVAL;
    gba_t *g = calloc(1, sizeof *g);
    if (g == NULL)
        return EMU_EINVAL;
    g->base.vtable = &gba_vtable;
    gba_ppu_init(&g->ppu, g->fb);
    gba_apu_init(&g->apu, g->audio, sizeof g->audio / sizeof g->audio[0]);
    gba_cart_init(&g->cart);
    gba_timers_reset(&g->timers);
    gba_dma_reset(&g->dma);
    gba_cpu_power_on(&g->cpu);
    *out = &g->base;
    return EMU_OK;
}

static void gba_destroy(emu_core_t *core)
{
    gba_t *g = (gba_t *)core;
    if (g == NULL)
        return;
    gba_cart_free(&g->cart);
    free(g);
}

static emu_result_t gba_load_rom(emu_core_t *core, const uint8_t *data, size_t size)
{
    gba_t *g = (gba_t *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    emu_result_t r = gba_cart_load(&g->cart, data, size);
    if (r != EMU_OK)
        return r;
    gba_cpu_power_on(&g->cpu);
    gba_timers_reset(&g->timers);
    gba_dma_reset(&g->dma);
    gba_apu_reset(&g->apu);
    return EMU_OK;
}

static void gba_reset(emu_core_t *core)
{
    gba_t *g = (gba_t *)core;
    gba_cpu_power_on(&g->cpu);
    gba_timers_reset(&g->timers);
    gba_dma_reset(&g->dma);
    gba_apu_reset(&g->apu);
    memset(&g->mem, 0, sizeof g->mem);
}

static emu_result_t gba_run_frame(emu_core_t *core)
{
    gba_t *g = (gba_t *)core;
    if (g->cart.rom == NULL)
        return EMU_ENOROM;

    for (uint16_t line = 0; line < GBA_LINES_PER_FRAME; line++) {
        uint32_t line_cycles = 0;
        g->ppu.vcount = line;

        while (line_cycles < GBA_CYCLES_PER_LINE) {
            uint32_t cycles = gba_cpu_step(g);
            line_cycles += cycles;
            g->total_cycles += cycles;
            gba_timers_step(g, cycles);
            gba_apu_step(g, cycles);
        }

        /* end of visible line: render, then HBlank DMA trigger */
        if (line < 160u) {
            gba_ppu_render_line(g, line);
            gba_dma_run(g, 2u); /* HBlank DMA */
        }

        if (line == 159u) {
            /* VBlank start: trigger VBlank DMA + IRQ */
            gba_dma_run(g, 1u);
            if (g->mem.ie & 0x0001u)
                gba_request_irq(g, 0x0001u);
        }
    }
    g->ppu.frame++;

    if (g->audio_cb != NULL && g->apu.out_pos > 0) {
        g->audio_cb(g->audio_user, g->audio, g->apu.out_pos);
        g->apu.out_pos = 0;
    }
    return EMU_OK;
}

static const uint32_t *gba_framebuffer(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    gba_t *g = (gba_t *)core;
    if (w)
        *w = GBA_SCREEN_W;
    if (h)
        *h = GBA_SCREEN_H;
    return g->fb;
}

static void gba_set_input(emu_core_t *core, uint32_t buttons)
{
    gba_t *g = (gba_t *)core;
    g->buttons = buttons; /* A B Select Start Right Left Up Down R L */
}

static void gba_set_audio_callback(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    gba_t *g = (gba_t *)core;
    g->audio_cb = cb;
    g->audio_user = user;
}

/* ---- save state ---- */

static void gba_serialize(gba_t *g, emu_state_writer *w)
{
    sw_u32(w, EMU_STATE_MAGIC);
    sw_u32(w, EMU_STATE_VERSION);
    sw_u32(w, 4); /* mgbax */

    gba_cpu *c = &g->cpu;
    for (int i = 0; i < 16; i++)
        sw_u32(w, c->r[i]);
    sw_u32(w, c->cpsr);
    for (int i = 0; i < 6; i++) {
        sw_u32(w, c->bank_r13[i]);
        sw_u32(w, c->bank_r14[i]);
        sw_u32(w, c->spsr[i]);
    }
    for (int i = 0; i < 5; i++)
        sw_u32(w, c->bank_r8_fiq[i]);
    sw_u8(w, c->mode);
    sw_u8(w, c->halted);
    sw_u8(w, c->intr_wait_active);
    sw_u32(w, c->intr_wait_flags);
    sw_u8(w, c->bios_dispatch);
    sw_u32(w, c->spsr[6]);

    sw_mem(w, g->mem.ewram, sizeof g->mem.ewram);
    sw_mem(w, g->mem.iwram, sizeof g->mem.iwram);
    sw_mem(w, g->mem.pal, sizeof g->mem.pal);
    sw_mem(w, g->mem.vram, sizeof g->mem.vram);
    sw_mem(w, g->mem.oam, sizeof g->mem.oam);
    sw_mem(w, g->mem.io, sizeof g->mem.io);
    sw_u16(w, g->mem.ie); sw_u16(w, g->mem.if_reg);
    sw_u16(w, g->mem.waitcnt);
    sw_u8(w, g->mem.imiu);

    for (int i = 0; i < 4; i++) {
        sw_u32(w, g->dma.sad[i]);
        sw_u32(w, g->dma.dad[i]);
        sw_u32(w, g->dma.dad_latch[i]);
        sw_u16(w, g->dma.count_latch[i]);
        sw_u16(w, g->dma.ctrl[i]);
        sw_u16(w, g->dma.count[i]);
        sw_u8(w, g->dma.enabled[i]);
    }
    for (int i = 0; i < 4; i++) {
        sw_u16(w, g->timers.reload[i]);
        sw_u16(w, g->timers.counter[i]);
        sw_u16(w, g->timers.ctrl[i]);
        sw_u32(w, g->timers.prescaler[i]);
    }
    sw_u32(w, g->ppu.frame);
    sw_u16(w, g->ppu.bgpa); sw_u16(w, (uint16_t)g->ppu.bgpb);
    sw_u16(w, (uint16_t)g->ppu.bgpc); sw_u16(w, (uint16_t)g->ppu.bgpd);
    sw_u32(w, (uint32_t)g->ppu.bgx[0]); sw_u32(w, (uint32_t)g->ppu.bgx[1]);
    sw_u32(w, (uint32_t)g->ppu.bgy[0]); sw_u32(w, (uint32_t)g->ppu.bgy[1]);
    sw_mem(w, g->cart.sram, sizeof g->cart.sram);
    sw_u64(w, g->total_cycles);
}

static void gba_deserialize(gba_t *g, emu_state_reader *r)
{
    (void)sr_u32(r); (void)sr_u32(r); (void)sr_u32(r);

    gba_cpu *c = &g->cpu;
    for (int i = 0; i < 16; i++)
        c->r[i] = sr_u32(r);
    c->cpsr = sr_u32(r);
    for (int i = 0; i < 6; i++) {
        c->bank_r13[i] = sr_u32(r);
        c->bank_r14[i] = sr_u32(r);
        c->spsr[i] = sr_u32(r);
    }
    for (int i = 0; i < 5; i++)
        c->bank_r8_fiq[i] = sr_u32(r);
    c->mode = sr_u8(r);
    c->halted = sr_u8(r);
    c->intr_wait_active = sr_u8(r);
    c->intr_wait_flags = sr_u32(r);
    c->bios_dispatch = sr_u8(r);
    c->spsr[6] = sr_u32(r);

    sr_mem(r, g->mem.ewram, sizeof g->mem.ewram);
    sr_mem(r, g->mem.iwram, sizeof g->mem.iwram);
    sr_mem(r, g->mem.pal, sizeof g->mem.pal);
    sr_mem(r, g->mem.vram, sizeof g->mem.vram);
    sr_mem(r, g->mem.oam, sizeof g->mem.oam);
    sr_mem(r, g->mem.io, sizeof g->mem.io);
    g->mem.ie = sr_u16(r);
    g->mem.if_reg = sr_u16(r);
    g->mem.waitcnt = sr_u16(r);
    g->mem.imiu = sr_u8(r);

    for (int i = 0; i < 4; i++) {
        g->dma.sad[i] = sr_u32(r);
        g->dma.dad[i] = sr_u32(r);
        g->dma.dad_latch[i] = sr_u32(r);
        g->dma.count_latch[i] = sr_u16(r);
        g->dma.ctrl[i] = sr_u16(r);
        g->dma.count[i] = sr_u16(r);
        g->dma.enabled[i] = sr_u8(r);
    }
    for (int i = 0; i < 4; i++) {
        g->timers.reload[i] = sr_u16(r);
        g->timers.counter[i] = sr_u16(r);
        g->timers.ctrl[i] = sr_u16(r);
        g->timers.prescaler[i] = sr_u32(r);
    }
    g->ppu.frame = sr_u32(r);
    g->ppu.bgpa = (int16_t)sr_u16(r);
    g->ppu.bgpb = (int16_t)sr_u16(r);
    g->ppu.bgpc = (int16_t)sr_u16(r);
    g->ppu.bgpd = (int16_t)sr_u16(r);
    g->ppu.bgx[0] = (int32_t)sr_u32(r);
    g->ppu.bgx[1] = (int32_t)sr_u32(r);
    g->ppu.bgy[0] = (int32_t)sr_u32(r);
    g->ppu.bgy[1] = (int32_t)sr_u32(r);
    sr_mem(r, g->cart.sram, sizeof g->cart.sram);
    g->total_cycles = sr_u64(r);
}

static size_t gba_state_size(emu_core_t *core)
{
    gba_t *g = (gba_t *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    gba_serialize(g, &w);
    return w.pos;
}

static emu_result_t gba_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    gba_t *g = (gba_t *)core;
    if (buf == NULL)
        return EMU_EINVAL;
    if (g->cart.rom == NULL)
        return EMU_ENOROM;
    
    /* Contract: an undersized buffer must be rejected without writing. */
    if (cap < gba_state_size(core))
        return EMU_ENOSPACE;
    emu_state_writer w = { buf, cap, 0, 0 };
    gba_serialize(g, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t gba_load_state(emu_core_t *core, const uint8_t *buf, size_t size)
{
    gba_t *g = (gba_t *)core;
    if (buf == NULL || size == 0)
        return EMU_EINVAL;
    if (size < 12)
        return EMU_EBADSTATE;
    if (emu_le32(buf) != EMU_STATE_MAGIC ||
        emu_le32(buf + 4) != EMU_STATE_VERSION ||
        emu_le32(buf + 8) != 4)
        return EMU_EBADSTATE;
    if (g->cart.rom == NULL)
        return EMU_ENOROM;

    emu_state_reader r = { buf, size, 0, 0 };
    gba_deserialize(g, &r);
    if (r.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

static const emu_core_vtable_t gba_vtable = {
    "mgbax",
    "Game Boy Advance",
    GBA_SCREEN_W,
    GBA_SCREEN_H,
    GBA_OUT_RATE,
    gba_create,
    gba_destroy,
    gba_load_rom,
    gba_reset,
    gba_run_frame,
    gba_framebuffer,
    gba_set_input,
    gba_set_audio_callback,
    gba_state_size,
    gba_save_state,
    gba_load_state
};

const emu_core_vtable_t *emu_core_mgbax(void)
{
    return &gba_vtable;
}
