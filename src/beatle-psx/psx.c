/*
 * beatle-psx: top-level core, memory map and PS-X EXE loader.
 *
 * Memory map (per spec; KUSEG 00000000h, KSEG0 80000000h and KSEG1
 * A0000000h all reach the same devices):
 *   00000000h  2 MiB main RAM (mirrored through the first 8 MiB)
 *   1F800000h  1 KiB scratchpad (not mirrored in KSEG1)
 *   1F801000h  hardware I/O (timers, IRQ, DMA, GPU, CD, SPU, pads, ...)
 *   1FC00000h  BIOS ROM - not bundled; reads return open-bus, writes ignored
 *   FFFE0000h  cache control (ignored)
 * Unsupported segments (KSEG2/TLB, expansion areas) raise bus-error style
 * address errors for the CPU as documented approximations.
 */
#include "psx.h"

#include <stdlib.h>
#include <string.h>

#include "../common/util.h"

/* ---- interrupt controller --------------------------------------------------- */

void psx_irq_raise(struct psx *p, uint16_t bits)
{
    p->irq_istat |= bits;
    /* CAUSE bit10 mirrors (I_STAT & I_MASK) != 0; CPU polls it each step. */
}

/* ---- address translation helpers --------------------------------------------- */

static uint8_t *ram_ptr(struct psx *p, uint32_t addr, uint32_t *mask)
{
    /* KUSEG 0-7FFFFFFFh, KSEG0 80000000h, KSEG1 A0000000h reach RAM
     * (2 MiB mirrored through the first 8 MiB of the physical space).
     * KSEG2 (C0000000h) is TLB-mapped and never aliased here. */
    if (addr < 0x00800000u ||
        (addr >= 0x80000000u && addr < 0x80800000u) ||
        (addr >= 0xA0000000u && addr < 0xA0800000u)) {
        *mask = addr & (PSX_RAM_SIZE - 1u);
        return p->ram;
    }
    *mask = 0;
    return NULL;
}

/* ---- bus: 32-bit -------------------------------------------------------------- */

uint32_t psx_bus_read32(struct psx *p, uint32_t addr)
{
    uint32_t off;
    uint32_t a = addr;

    if (ram_ptr(p, a, &off) != NULL)
        return emu_le32(p->ram + off);

    a &= 0x1FFFFFFFu;
    if (a >= 0x1F800000u && a < 0x1F800400u) {
        uint32_t o = a - 0x1F800000u;
        if (addr < 0xA0000000u) {
            /* scratchpad: KUSEG and KSEG0 only */
            return (uint32_t)p->scratch[o] |
                   ((uint32_t)p->scratch[o + 1] << 8) |
                   ((uint32_t)p->scratch[o + 2] << 16) |
                   ((uint32_t)p->scratch[o + 3] << 24);
        }
        return 0; /* KSEG1 has no scratchpad mirror */
    }
    if (a >= 0x1F801000u && a < 0x1F802000u)
        return psx_hw_read(p, a);
    if (a >= 0x1FC00000u && a < 0x1FC80000u)
        return 0xFFFFFFFFu; /* no BIOS: open bus */
    if (a == 0xFFFE0000u)
        return 0; /* cache control */
    return 0xFFFFFFFFu;
}

uint16_t psx_bus_read16(struct psx *p, uint32_t addr)
{
    uint32_t v = psx_bus_read32(p, addr & ~3u);
    return (uint16_t)(v >> ((addr & 2u) * 8u));
}

uint8_t psx_bus_read8(struct psx *p, uint32_t addr)
{
    uint32_t v = psx_bus_read32(p, addr & ~3u);
    return (uint8_t)(v >> ((addr & 3u) * 8u));
}

/* ---- bus: writes --------------------------------------------------------------- */

void psx_ram_write32(struct psx *p, uint32_t off, uint32_t v);

void psx_ram_write32(struct psx *p, uint32_t off, uint32_t v)
{
    p->ram[off] = (uint8_t)v;
    p->ram[off + 1] = (uint8_t)(v >> 8);
    p->ram[off + 2] = (uint8_t)(v >> 16);
    p->ram[off + 3] = (uint8_t)(v >> 24);
}

void psx_bus_write32(struct psx *p, uint32_t addr, uint32_t v)
{
    uint32_t off;
    uint32_t a = addr;

    if (ram_ptr(p, a, &off) != NULL) {
        psx_ram_write32(p, off, v);
        return;
    }

    a &= 0x1FFFFFFFu;
    if (a >= 0x1F800000u && a < 0x1F800400u && addr < 0xA0000000u) {
        uint32_t o = a - 0x1F800000u;
        p->scratch[o] = (uint8_t)v;
        p->scratch[o + 1] = (uint8_t)(v >> 8);
        p->scratch[o + 2] = (uint8_t)(v >> 16);
        p->scratch[o + 3] = (uint8_t)(v >> 24);
        return;
    }
    if (a >= 0x1F801000u && a < 0x1F802000u) {
        psx_hw_write(p, a, v);
        return;
    }
    /* BIOS ROM, cache control, expansion: writes ignored. */
}

void psx_bus_write16(struct psx *p, uint32_t addr, uint16_t v)
{
    uint32_t off;
    uint32_t shift = (addr & 2u) * 8u;

    if (ram_ptr(p, addr, &off) != NULL) {
        uint32_t o = off & ~3u;
        uint32_t cur = emu_le32(p->ram + o);
        cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
        psx_ram_write32(p, o, cur);
        return;
    }
    {
        uint32_t a = addr & ~3u;
        uint32_t old = psx_bus_read32(p, a);
        psx_bus_write32(p, a, (old & ~(0xFFFFu << shift)) |
                                  ((uint32_t)v << shift));
    }
}

void psx_bus_write8(struct psx *p, uint32_t addr, uint8_t v)
{
    uint32_t off;

    if (ram_ptr(p, addr, &off) != NULL) {
        p->ram[off] = v;
        return;
    }
    {
        uint32_t a = addr & ~3u;
        uint32_t shift = (addr & 3u) * 8u;
        uint32_t old = psx_bus_read32(p, a);
        psx_bus_write32(p, a, (old & ~(0xFFu << shift)) |
                                  ((uint32_t)v << shift));
    }
}

/* ---- CPU exceptions -------------------------------------------------------------- */

#define PSX_EXC_INT   0u
#define PSX_EXC_ADEL  4u
#define PSX_EXC_ADES  5u
#define PSX_EXC_SYS   8u
#define PSX_EXC_BP    9u
#define PSX_EXC_RI    10u
#define PSX_EXC_CPU   11u
#define PSX_EXC_OV    12u

void psx_cpu_exception(struct psx *p, uint32_t code, uint32_t badva)
{
    psx_cpu_t *c = &p->cpu;
    uint32_t in_delay = c->exc_in_delay;

    /* Push the current mode down one level (IEc/KUc -> IEp/KUp). */
    c->cop0_sr = ((c->cop0_sr & 0x3Fu) << 2) | (c->cop0_sr & 0xFFFFFFC0u);
    c->cop0_epc = in_delay ? c->exc_pc - 4u : c->exc_pc;
    c->cop0_cause = (c->cop0_cause & 0xFFFFFF40u) | (code << 2);
    if (in_delay)
        c->cop0_cause |= 0x80000000u;
    c->cop0_badva = badva;
    /* Vector: BEV=0 -> RAM 80000080h (the ROM-area BFC00200h vector is
     * unreachable without a BIOS image). The load-delay pipeline state is
     * intentionally preserved so interrupted streams re-execute exactly. */
    c->pc = 0x80000080u;
    c->next_pc = c->pc + 4u;
}

/* ---- frame / vblank timing --------------------------------------------------------- */

static void end_of_frame(struct psx *p)
{
    psx_irq_raise(p, PSX_IRQ_VBLANK);
    p->vblank_active = 1;
}

static void start_of_frame(struct psx *p)
{
    p->vblank_active = 0;
}

void psx_timers_tick(struct psx *p, uint32_t cycles, uint32_t line_cycle_pos,
                     uint32_t scanline, int in_hblank, int in_vblank)
{
    (void)line_cycle_pos;
    (void)scanline;
    for (int i = 0; i < 3; i++) {
        psx_timer_t *t = &p->timer[i];
        uint32_t mode = t->mode;
        uint32_t clock_sel = (mode >> 8) & 3u;
        uint32_t inc = 0;

        if (i == 0)
            inc = (clock_sel == 1u || clock_sel == 3u) ? 0u : cycles;
        else if (i == 1)
            inc = (clock_sel == 1u || clock_sel == 3u) ? 0u : cycles;
        else
            inc = (clock_sel >= 2u) ? cycles / 8u : cycles;

        if (mode & 1u) { /* synchronized */
            uint32_t smode = (mode >> 1) & 3u;
            if (i == 0) {
                if (smode == 2u && !in_hblank)
                    inc = 0;
                if (smode == 0u && in_hblank)
                    inc = 0;
            } else if (i == 1) {
                if (smode == 2u && !in_vblank)
                    inc = 0;
                if (smode == 0u && in_vblank)
                    inc = 0;
            } else {
                if (smode == 0u || smode == 3u)
                    inc = 0;
            }
        }

        while (inc-- > 0) {
            uint32_t next = (uint32_t)t->count + 1u;
            uint32_t target = t->target == 0 ? 0x10000u : t->target;
            int hit_target = ((mode & 8u) && next >= target);
            int hit_ffff = (!(mode & 8u) && next >= 0x10000u);

            if (hit_target || hit_ffff) {
                uint32_t flags = 0;
                if (hit_target) {
                    flags |= 0x1000u; /* bit12: reached target */
                    next = 0;
                }
                if (hit_ffff) {
                    flags |= 0x2000u; /* bit13: reached FFFF */
                    next = 0;
                }
                t->count = 0;
                t->mode_read_latch = (uint16_t)((t->mode_read_latch & ~0x3000u) |
                                                flags);
                if ((mode & 0x30u) != 0 &&          /* IRQ target/ffff enable */
                    !(t->mode_read_latch & 0x0004u)) { /* one-shot not done */
                    psx_irq_raise(p, i == 0 ? PSX_IRQ_TMR0
                                     : i == 1 ? PSX_IRQ_TMR1
                                              : PSX_IRQ_TMR2);
                    if (mode & 0x80u) /* toggle mode: invert bit10 */
                        t->mode_read_latch ^= 0x0400u;
                    if (!(mode & 0x40u)) /* one-shot: no further IRQs */
                        t->mode_read_latch |= 0x0004u;
                }
            } else {
                t->count = (uint16_t)next;
            }
        }
    }
}

/* ---- PS-X EXE loading ---------------------------------------------------------------- */

#define PSX_EXE_HEADER 0x800u

static emu_result_t psx_load_exe(struct psx *p, const uint8_t *data, size_t size)
{
    uint32_t pc0 = emu_le32(data + 0x10);
    uint32_t t_addr = emu_le32(data + 0x18);
    uint32_t t_size = emu_le32(data + 0x1C);
    uint32_t b_addr = emu_le32(data + 0x28);
    uint32_t b_size = emu_le32(data + 0x2C);
    uint32_t sp = emu_le32(data + 0x30);

    if (size < PSX_EXE_HEADER + 4u)
        return EMU_EBADROM;
    if (t_size > size - PSX_EXE_HEADER)
        return EMU_EBADROM;
    if ((t_addr & 0x1FFFFFFFu) + t_size > PSX_RAM_SIZE)
        return EMU_EBADROM;
    if ((b_addr & 0x1FFFFFFFu) + b_size > PSX_RAM_SIZE)
        return EMU_EBADROM;
    if ((pc0 & 0x1FFFFFFFu) >= PSX_RAM_SIZE)
        return EMU_EBADROM;

    memcpy(p->ram + (t_addr & 0x1FFFFFFFu), data + PSX_EXE_HEADER, t_size);
    if (b_size != 0)
        memset(p->ram + (b_addr & 0x1FFFFFFFu), 0, b_size);

    psx_cpu_reset(&p->cpu);
    p->cpu.pc = pc0;
    p->cpu.next_pc = pc0 + 4u;
    /* Boot environment (documented approximation of the BIOS hand-off):
     * kernel mode, interrupts disabled, sp at top of RAM. */
    p->cpu.cop0_sr = 0;
    p->cpu.r[28] = 0;            /* gp cleared; EXEs set their own      */
    p->cpu.r[29] = sp ? (sp & 0x1FFFFFFFu) | 0x80000000u : 0x801FFFF0u;
    p->cpu.r[30] = 0;            /* fp                                  */
    p->cpu.cop0_prid = 0x00000002u; /* R3000A, revision 2               */
    return EMU_OK;
}

static emu_result_t psx_validate(const uint8_t *d, size_t s)
{
    if (s >= 8 && memcmp(d, "PS-X EXE", 8) == 0)
        return EMU_OK;
    if (s >= 32800 && memcmp(d + 32769, "CD001", 5) == 0 &&
        memcmp(d + 32776, "PLAYSTATION", 11) == 0)
        return EMU_OK;
    return EMU_EBADROM;
}

/* ---- vtable implementation -------------------------------------------------------------- */

static const emu_core_vtable_t psx_vtable;

static emu_result_t psx_create(emu_core_t **out)
{
    struct psx *p = calloc(1, sizeof *p);
    if (p == NULL)
        return EMU_EINVAL;
    p->base.vtable = &psx_vtable;
    psx_cpu_init(&p->cpu, p);
    psx_gpu_init(&p->gpu);
    *out = &p->base;
    return EMU_OK;
}

static void psx_destroy(emu_core_t *core)
{
    struct psx *p = (struct psx *)core;
    if (p == NULL)
        return;
    free(p);
}

static emu_result_t psx_load_rom(emu_core_t *core, const uint8_t *data, size_t size)
{
    struct psx *p = (struct psx *)core;
    emu_result_t r = psx_validate(data, size);
    if (r != EMU_OK)
        return r;

    if (size >= 8 && memcmp(data, "PS-X EXE", 8) == 0) {
        r = psx_load_exe(p, data, size);
        if (r != EMU_OK)
            return r;
    } else {
        /* ISO9660 disc image: the core implements CPU/GPU/DMA/timers but no
         * CD-ROM drive, so disc-based titles cannot bootstrap. Accept the
         * image and reset CPU/GPU; the ROM is retained for identification. */
        psx_cpu_reset(&p->cpu);
    }
    psx_gpu_reset(&p->gpu);
    psx_hw_reset(&p->dma, p->timer, &p->pad);
    p->irq_istat = 0;
    p->irq_imask = 0;
    p->scanline = 0;
    p->line_cycles = 0;
    p->vblank_active = 0;
    p->rom_ok = 1;
    return EMU_OK;
}

static void psx_reset(emu_core_t *core)
{
    struct psx *p = (struct psx *)core;
    /* Reset re-runs the boot entry point (kept address in r[25] is not
     * tracked; documented limitation: reset reboots the loaded EXE). */
    psx_cpu_reset(&p->cpu);
    psx_gpu_reset(&p->gpu);
    psx_hw_reset(&p->dma, p->timer, &p->pad);
    p->irq_istat = 0;
    p->irq_imask = 0;
    p->scanline = 0;
    p->line_cycles = 0;
    p->vblank_active = 0;
}

static void psx_frame_tick(struct psx *p, uint32_t cycles)
{
    /* Advance scanline/vblank timing in one batch (documented model:
     * one H IRQ per scanline at the line boundary, V IRQ at line 240). */
    p->line_cycles += cycles;
    while (p->line_cycles >= (PSX_CYCLES_PER_FRAME / PSX_LINES_PER_FRAME)) {
        p->line_cycles -= PSX_CYCLES_PER_FRAME / PSX_LINES_PER_FRAME;
        p->scanline++;
        if (p->scanline == PSX_HBLANK_LINE) {
            end_of_frame(p);
        } else if (p->scanline >= PSX_LINES_PER_FRAME) {
            p->scanline = 0;
            start_of_frame(p);
        }
        psx_timers_tick(p, 0, 0, p->scanline, 0, p->vblank_active);
    }
    /* Timers tick with system-clock granularity. */
    psx_timers_tick(p, cycles, p->line_cycles, p->scanline, 0,
                    p->vblank_active);
}

static emu_result_t psx_run_frame(emu_core_t *core)
{
    struct psx *p = (struct psx *)core;
    if (!p->rom_ok)
        return EMU_ENOROM;

    uint32_t budget = PSX_CYCLES_PER_FRAME;
    while (budget > 0) {
        uint32_t used = psx_cpu_step(&p->cpu);
        psx_dma_run(p);
        psx_frame_tick(p, used);
        if (used > budget)
            budget = 0;
        else
            budget -= used;
    }

    psx_gpu_render(&p->gpu, p->fb);
    return EMU_OK;
}

static const uint32_t *psx_fb(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct psx *p = (struct psx *)core;
    if (w)
        *w = PSX_FB_W;
    if (h)
        *h = PSX_FB_H;
    return p->fb;
}

static void psx_set_input(emu_core_t *core, uint32_t buttons)
{
    struct psx *p = (struct psx *)core;
    /* Layout (documented): Select=bit0 L3=1 R3=2 Start=3 Up=4 Right=5
     * Down=6 Left=7 L2=8 R2=9 L1=10 R1=11 Triangle=12 Circle=13
     * Cross=14 Square=15 (standard PS pad order). */
    p->input = buttons;
    p->pad.buttons = (uint16_t)(~buttons & 0xFFFFu);
}

static void psx_set_audio(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    struct psx *p = (struct psx *)core;
    p->audio_cb = cb;
    p->audio_user = user; /* SPU not implemented: callback stays unused */
}

/* ---- save states ------------------------------------------------------------------------- */

static void psx_serialize(const struct psx *p, emu_state_writer *w)
{
    sw_u32(w, EMU_STATE_MAGIC);
    sw_u32(w, EMU_STATE_VERSION);
    sw_mem(w, p->ram, PSX_RAM_SIZE);
    sw_mem(w, p->scratch, PSX_SCRATCH_SIZE);
    sw_mem(w, p->gpu.vram, sizeof p->gpu.vram);
    for (int i = 0; i < 32; i++) {
        sw_u32(w, p->cpu.r[i]);
        sw_u32(w, p->gte.dr[i]);
        sw_u32(w, p->gte.cr[i]);
    }
    sw_u32(w, p->cpu.hi);
    sw_u32(w, p->cpu.lo);
    sw_u32(w, p->cpu.pc);
    sw_u32(w, p->cpu.next_pc);
    sw_u32(w, p->cpu.cop0_sr);
    sw_u32(w, p->cpu.cop0_cause);
    sw_u32(w, p->cpu.cop0_epc);
    sw_u32(w, p->cpu.cop0_prid);
    sw_u32(w, p->cpu.cop0_badva);
    sw_u32(w, p->cpu.ld_dreg);
    sw_u32(w, p->cpu.ld_dval);
    sw_u32(w, p->cpu.ld_new_dreg);
    sw_u32(w, p->cpu.ld_new_dval);
    sw_u32(w, p->gpu.gpustat);
    sw_u32(w, p->gpu.tpage);
    sw_u32(w, p->gpu.texwin);
    sw_u32(w, p->gpu.area_x1);
    sw_u32(w, p->gpu.area_y1);
    sw_u32(w, p->gpu.area_x2);
    sw_u32(w, p->gpu.area_y2);
    sw_u32(w, p->gpu.off_x);
    sw_u32(w, p->gpu.off_y);
    sw_u32(w, p->gpu.mask_set);
    sw_u32(w, p->gpu.mask_check);
    sw_u32(w, p->gpu.disp_x);
    sw_u32(w, p->gpu.disp_y);
    sw_u32(w, p->gpu.disp_w);
    sw_u32(w, p->gpu.disp_h);
    sw_u32(w, p->gpu.disp_depth24);
    sw_u32(w, p->gpu.disp_enabled);
    for (int i = 0; i < 7; i++) {
        sw_u32(w, p->dma.ch[i].madr);
        sw_u32(w, p->dma.ch[i].bcr);
        sw_u32(w, p->dma.ch[i].chcr);
    }
    sw_u32(w, p->dma.dpcr);
    sw_u32(w, p->dma.dicr);
    for (int i = 0; i < 3; i++) {
        sw_u16(w, p->timer[i].count);
        sw_u16(w, p->timer[i].mode);
        sw_u16(w, p->timer[i].target);
        sw_u16(w, p->timer[i].mode_read_latch);
    }
    sw_u16(w, p->irq_istat);
    sw_u16(w, p->irq_imask);
    sw_u16(w, p->pad.buttons);
    sw_u32(w, p->pad.ctrl);
    sw_u16(w, p->pad.stat);
    sw_u32(w, p->scanline);
    sw_u32(w, p->line_cycles);
    sw_u32(w, p->vblank_active);
}

static size_t psx_state_size(emu_core_t *core)
{
    (void)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    psx_serialize((const struct psx *)core, &w);
    return w.pos;
}

static emu_result_t psx_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    const struct psx *p = (const struct psx *)core;
    if (!p->rom_ok)
        return EMU_ENOROM;
    emu_state_writer w = { buf, cap, 0, 0 };
    psx_serialize(p, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t psx_load_state(emu_core_t *core, const uint8_t *buf, size_t size)
{
    struct psx *p = (struct psx *)core;
    if (!p->rom_ok)
        return EMU_ENOROM;
    emu_state_reader r = { buf, size, 0, 0 };
    if (size < 8)
        return EMU_EBADSTATE;
    if (sr_u32(&r) != EMU_STATE_MAGIC || sr_u32(&r) != EMU_STATE_VERSION)
        return EMU_EBADSTATE;

    sr_mem(&r, p->ram, PSX_RAM_SIZE);
    sr_mem(&r, p->scratch, PSX_SCRATCH_SIZE);
    sr_mem(&r, p->gpu.vram, sizeof p->gpu.vram);
    for (int i = 0; i < 32; i++) {
        p->cpu.r[i] = sr_u32(&r);
        p->gte.dr[i] = sr_u32(&r);
        p->gte.cr[i] = sr_u32(&r);
    }
    p->cpu.hi = sr_u32(&r);
    p->cpu.lo = sr_u32(&r);
    p->cpu.pc = sr_u32(&r);
    p->cpu.next_pc = sr_u32(&r);
    p->cpu.cop0_sr = sr_u32(&r);
    p->cpu.cop0_cause = sr_u32(&r);
    p->cpu.cop0_epc = sr_u32(&r);
    p->cpu.cop0_prid = sr_u32(&r);
    p->cpu.cop0_badva = sr_u32(&r);
    p->cpu.ld_dreg = sr_u32(&r);
    p->cpu.ld_dval = sr_u32(&r);
    p->cpu.ld_new_dreg = sr_u32(&r);
    p->cpu.ld_new_dval = sr_u32(&r);
    p->gpu.gpustat = sr_u32(&r);
    p->gpu.tpage = sr_u32(&r);
    p->gpu.texwin = sr_u32(&r);
    p->gpu.area_x1 = sr_u32(&r);
    p->gpu.area_y1 = sr_u32(&r);
    p->gpu.area_x2 = sr_u32(&r);
    p->gpu.area_y2 = sr_u32(&r);
    p->gpu.off_x = (int32_t)sr_u32(&r);
    p->gpu.off_y = (int32_t)sr_u32(&r);
    p->gpu.mask_set = sr_u32(&r);
    p->gpu.mask_check = sr_u32(&r);
    p->gpu.disp_x = sr_u32(&r);
    p->gpu.disp_y = sr_u32(&r);
    p->gpu.disp_w = sr_u32(&r);
    p->gpu.disp_h = sr_u32(&r);
    p->gpu.disp_depth24 = sr_u32(&r);
    p->gpu.disp_enabled = sr_u32(&r);
    for (int i = 0; i < 7; i++) {
        p->dma.ch[i].madr = sr_u32(&r);
        p->dma.ch[i].bcr = sr_u32(&r);
        p->dma.ch[i].chcr = sr_u32(&r);
    }
    p->dma.dpcr = sr_u32(&r);
    p->dma.dicr = sr_u32(&r);
    for (int i = 0; i < 3; i++) {
        p->timer[i].count = sr_u16(&r);
        p->timer[i].mode = sr_u16(&r);
        p->timer[i].target = sr_u16(&r);
        p->timer[i].mode_read_latch = sr_u16(&r);
    }
    p->irq_istat = sr_u16(&r);
    p->irq_imask = sr_u16(&r);
    p->pad.buttons = sr_u16(&r);
    p->pad.ctrl = sr_u32(&r);
    p->pad.stat = sr_u16(&r);
    p->scanline = sr_u32(&r);
    p->line_cycles = sr_u32(&r);
    p->vblank_active = (int)sr_u32(&r);

    if (r.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

/* ---- vtable --------------------------------------------------------------------------- */

static const emu_core_vtable_t psx_vtable = {
    "beatle-psx", "Sony PlayStation", PSX_FB_W, PSX_FB_H, 44100,
    psx_create, psx_destroy, psx_load_rom, psx_reset,
    psx_run_frame, psx_fb, psx_set_input, psx_set_audio,
    psx_state_size, psx_save_state, psx_load_state,
};

const emu_core_vtable_t *emu_core_beatle_psx(void)
{
    return &psx_vtable;
}
