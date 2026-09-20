/*
 * supercastpro: Sega Dreamcast machine. See dc.h for the verified scope,
 * boot model and documented simplifications.
 */
#include "dc.h"

#include <stdlib.h>
#include <string.h>

#include "../common/util.h"



/* ---- address decode ---------------------------------------------------------------- */

/* Physical address for P0/P1/P2/P3 windows (U0 cached/uncached). */
static uint32_t dc_phys(struct dc *d, uint32_t a, int *mapped)
{
    (void)d;
    *mapped = 1;
    if (a < 0x80000000u)
        return a; /* P0/U0: MMU not modeled (documented) */
    if (a < 0xC0000000u)
        return a & 0x1FFFFFFFu; /* P1 cached */
    if (a < 0xE0000000u)
        return a & 0x1FFFFFFFu; /* P2 uncached */
    /* P3 (0xC0000000-0xDFFFFFFF handled above): remaining < 0xE0000000
     * is unreachable on 32-bit; P4 checks follow. */
    /* P4 */
    if (a >= 0xF4000000u && a < 0xF4100000u)
        return DC_URAM_BASE + (a - 0xF4000000u); /* URAM mirror */
    if (a >= 0xFC000000u && a < 0xFE000000u)
        return DC_PHYS_BOOTROM + (a - 0xFC000000u); /* boot ROM mirror */
    if (a >= 0xFFD00000u && a < 0xFFE00000u)
        return a; /* SH-4 on-chip modules (TMU/INTC): P4-only */
    if (a >= 0xFFE80000u && a < 0xFFE90000u)
        return a; /* SCIF: P4-only */
    if (a >= 0xFF000000u)
        return a - 0xFF000000u; /* DC peripheral window */
    *mapped = 0;
    return a;
}

/* test hooks declared non-static in dc.h */
uint32_t dc_read32(struct dc *d, uint32_t addr);
void dc_write32(struct dc *d, uint32_t addr, uint32_t v);

/* ---- memory access ------------------------------------------------------------------ */

static uint8_t *dc_ptr(struct dc *d, uint32_t pa)
{
    if (pa < DC_BOOTROM_SIZE)
        return &d->bootrom[pa];
    if (pa >= DC_PHYS_FLASH && pa < DC_PHYS_FLASH + DC_FLASH_SIZE)
        return &d->flash[pa - DC_PHYS_FLASH];
    if (pa >= DC_PHYS_AICA && pa < DC_PHYS_AICA + DC_AICA_SIZE)
        return &d->aica[pa - DC_PHYS_AICA];
    if (pa >= DC_PHYS_VRAM && pa < DC_PHYS_VRAM + DC_VRAM_SIZE)
        return &d->vram[pa - DC_PHYS_VRAM];
    if (pa >= DC_PHYS_RAM && pa < DC_PHYS_RAM + DC_RAM_SIZE)
        return &d->ram[pa - DC_PHYS_RAM];
    if (pa >= DC_URAM_BASE && pa < DC_URAM_BASE + DC_URAM_SIZE)
        return &d->uram[pa - DC_URAM_BASE];
    return NULL;
}

static uint32_t dc_tmu_read(struct dc *d, uint32_t a)
{
    uint32_t off = a - DC_TMU_BASE;
    if (off == 0x004u)
        return d->tstr; /* TSTR */
    uint32_t ch = (off - 0x008u) / 0xCu;
    uint32_t r = (off - 0x008u) % 0xCu;
    if (ch >= DC_TMU_CHANNELS)
        return 0;
    switch (r) {
    case 0x0u: return d->tmu[ch].tcor;
    case 0x4u: return d->tmu[ch].tcnt;
    case 0x8u: return d->tmu[ch].tcr;
    default: return 0;
    }
}

static void dc_tmu_write(struct dc *d, uint32_t a, uint32_t v)
{
    uint32_t off = a - DC_TMU_BASE;
    if (off == 0x004u) {
        d->tstr = v & 7u;
        return;
    }
    uint32_t ch = (off - 0x008u) / 0xCu;
    uint32_t r = (off - 0x008u) % 0xCu;
    if (ch >= DC_TMU_CHANNELS)
        return;
    switch (r) {
    case 0x0u: d->tmu[ch].tcor = v; return;
    case 0x4u: d->tmu[ch].tcnt = v; return;
    case 0x8u:
        d->tmu[ch].tcr = v & 0x075u; /* clock | UNIE | UNF(w1c) */
        return;
    default: return;
    }
}

static uint32_t dc_pvr_read(struct dc *d, uint32_t off)
{
    /* documented best-effort ID/revision responses */
    if (off == 0x008u)
        return 0x1Du; /* PVR ID */
    if (off == 0x014u)
        return 0x11u; /* revision */
    return emu_le32(&d->pvr_regs[off]);
}

static uint32_t dc_bus_read32(struct dc *d, uint32_t a)
{
    int mapped;
    uint32_t pa = dc_phys(d, a, &mapped);
#ifdef DBG_DC_PRINT
    fprintf(stderr, "read32 a=%08X pa=%08X mapped=%d\n", a, pa, mapped);
#endif
    if (!mapped)
        return 0;
    if (pa >= DC_TMU_BASE && pa < DC_TMU_BASE + 0x30u)
        return dc_tmu_read(d, pa);
    if (pa >= DC_INTC_BASE && pa < DC_INTC_BASE + 0x40u)
        return d->intc_regs[(pa - DC_INTC_BASE) / 4u];
    if (pa >= DC_SCIF_BASE && pa < DC_SCIF_BASE + 0x100u)
        return 0; /* SCIF stub: no data */
    if (pa >= DC_PVR_BASE && pa < DC_PVR_BASE + DC_PVR_SIZE)
        return dc_pvr_read(d, pa - DC_PVR_BASE);
    uint8_t *p = dc_ptr(d, pa);
    if (p != NULL)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return 0;
}

static void dc_bus_write32(struct dc *d, uint32_t a, uint32_t v)
{
    int mapped;
    uint32_t pa = dc_phys(d, a, &mapped);
    if (!mapped)
        return;
    if (pa >= DC_TMU_BASE && pa < DC_TMU_BASE + 0x30u) {
        dc_tmu_write(d, pa, v);
        return;
    }
    if (pa >= DC_INTC_BASE && pa < DC_INTC_BASE + 0x40u) {
        d->intc_regs[(pa - DC_INTC_BASE) / 4u] = v;
        return;
    }
    if (pa >= DC_SCIF_BASE && pa < DC_SCIF_BASE + 0x100u)
        return; /* SCIF stub */
    if (pa >= DC_PVR_BASE && pa < DC_PVR_BASE + DC_PVR_SIZE) {
        emu_store_le32(&d->pvr_regs[pa - DC_PVR_BASE], v);
        return;
    }
    uint8_t *p = dc_ptr(d, pa);
    if (p != NULL) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    }
}

static uint16_t dc_bus_read16(struct dc *d, uint32_t a)
{
    int mapped;
    uint32_t pa = dc_phys(d, a, &mapped);
    if (!mapped)
        return 0;
    if (pa >= DC_PVR_BASE && pa < DC_PVR_BASE + DC_PVR_SIZE)
        return (uint16_t)dc_pvr_read(d, pa - DC_PVR_BASE);
    uint8_t *p = dc_ptr(d, pa & ~1u);
    if (p != NULL)
        return (uint16_t)(p[0] | (p[1] << 8));
    return 0;
}

static void dc_bus_write16(struct dc *d, uint32_t a, uint16_t v)
{
    int mapped;
    uint32_t pa = dc_phys(d, a, &mapped);
    if (!mapped)
        return;
    uint8_t *p = dc_ptr(d, pa & ~1u);
    if (p != NULL) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

static uint8_t dc_bus_read8(struct dc *d, uint32_t a)
{
    int mapped;
    uint32_t pa = dc_phys(d, a, &mapped);
    if (!mapped)
        return 0;
    uint8_t *p = dc_ptr(d, pa);
    return p != NULL ? *p : 0;
}

static void dc_bus_write8(struct dc *d, uint32_t a, uint8_t v)
{
    int mapped;
    uint32_t pa = dc_phys(d, a, &mapped);
    if (!mapped)
        return;
    uint8_t *p = dc_ptr(d, pa);
    if (p != NULL)
        *p = v;
}

/* ---- sh4 bus adapters --------------------------------------------------------------- */

static uint8_t sh4_rb(void *u, uint32_t a) { return dc_bus_read8(u, a); }
static uint16_t sh4_rh(void *u, uint32_t a) { return dc_bus_read16(u, a); }
static uint32_t sh4_rw(void *u, uint32_t a) { return dc_bus_read32(u, a); }
static void sh4_wb(void *u, uint32_t a, uint8_t v) { dc_bus_write8(u, a, v); }
static void sh4_wh(void *u, uint32_t a, uint16_t v) { dc_bus_write16(u, a, v); }
static void sh4_ww(void *u, uint32_t a, uint32_t v) { dc_bus_write32(u, a, v); }

static const sh4_bus_t dc_bus_tmpl = {
    NULL, sh4_rb, sh4_rh, sh4_rw, sh4_wb, sh4_wh, sh4_ww
};

/* ---- timers -------------------------------------------------------------------------- */

static const uint32_t tmu_div[4] = { 4u, 16u, 64u, 256u };

static void dc_tick_tmu(struct dc *d, uint32_t cycles)
{
    for (int ch = 0; ch < 3; ch++) {
        if (!(d->tstr & (1u << ch)))
            continue;
        uint32_t div = tmu_div[d->tmu[ch].tcr & 3u];
        uint64_t total = d->tmu[ch].acc + cycles;
        uint64_t counts = total / div;
        d->tmu[ch].acc = total % div;
        if (counts == 0)
            continue;
        uint64_t wrap = (uint64_t)d->tmu[ch].tcor + 1u;
        uint64_t v = d->tmu[ch].tcnt;
        uint64_t under = 0;
        if (counts >= v) {
            counts -= v;
            under = 1 + counts / wrap;
            uint64_t rem = counts % wrap;
            v = wrap - 1u - rem; /* back at TCOR, counting down */
        } else {
            v -= counts;
        }
        d->tmu[ch].tcnt = (uint32_t)v;
        if (under > 0) {
            d->tmu[ch].tcr |= 0x0020u; /* UNF (w1c by software) */
            d->tmu[ch].unf_pending = 1;
        }
    }
}

/* ---- video scanout (PVR display controller) ------------------------------------------- */

static uint32_t rgb565_px(uint16_t c)
{
    uint32_t r = (uint32_t)((c >> 11) & 31u), g = (uint32_t)((c >> 5) & 63u),
             b = (uint32_t)(c & 31u);
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return EMU_PIXEL(r, g, b);
}

void dc_render(struct dc *d)
{
    for (size_t i = 0; i < sizeof d->fb / sizeof d->fb[0]; i++)
        d->fb[i] = 0xFF000000u; /* opaque black */

    uint32_t ctrl = emu_le32(&d->pvr_regs[DC_PVR_FB_R_CTRL]);
    uint32_t size = emu_le32(&d->pvr_regs[DC_PVR_FB_R_SIZE]);
    uint32_t sof = emu_le32(&d->pvr_regs[DC_PVR_FB_R_SOF1]) & 0x03FFFFFFu;
    uint32_t fmt = (ctrl >> 2) & 3u;
    uint32_t bpp = (fmt == 0u) ? 2u : (fmt == 2u) ? 3u : 4u;
    uint32_t w = ((size & 0x3FFu) + 1u) * 4u;
    uint32_t h = ((size >> 10) & 0x3FFu) + 1u;
    if (w > DC_SCREEN_W)
        w = DC_SCREEN_W;
    if (h > DC_SCREEN_H)
        h = DC_SCREEN_H;
    if (sof >= DC_VRAM_SIZE)
        return;
    const uint8_t *src = d->vram + sof;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *out = &d->fb[y * DC_SCREEN_W];
        const uint8_t *row = src + (size_t)y * w * bpp;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *px = row + x * bpp;
            if (fmt == 0u) { /* RGB565 packed */
                uint16_t c = (uint16_t)(px[0] | (px[1] << 8));
                out[x] = rgb565_px(c);
            } else if (fmt == 2u) { /* RGB888 (3 bytes) */
                out[x] = EMU_PIXEL(px[0], px[1], px[2]);
            } else if (fmt == 3u) { /* ARGB0888 */
                out[x] = EMU_PIXEL(px[2], px[1], px[0]);
            } else {
                out[x] = 0xFF000000u;
            }
        }
    }
}

/* ---- frame loop ----------------------------------------------------------------------- */

static void dc_deliver_tuni(struct dc *d)
{
    for (int ch = 0; ch < 3; ch++) {
        if (d->tmu[ch].unf_pending) {
            d->tmu[ch].unf_pending = 0;
            if (d->tmu[ch].tcr & 0x0040u) { /* UNIE */
                static const uint32_t tuni[3] = { 0x400u, 0x420u, 0x440u };
                sh4_irq(&d->cpu, 8 + ch, tuni[ch]);
            }
        }
    }
}

void dc_step(struct dc *d)
{
    uint32_t cyc = sh4_step(&d->cpu);
    dc_tick_tmu(d, cyc);
    dc_deliver_tuni(d);
}

static emu_result_t dc_run_frame(emu_core_t *core)
{
    struct dc *d = (struct dc *)core;
    if (d->rom_size == 0)
        return EMU_ENOROM;

    uint64_t remaining = DC_CYCLES_PER_FRAME;
    while (remaining > 0) {
        uint32_t cyc = sh4_step(&d->cpu);
        dc_tick_tmu(d, cyc);
        dc_deliver_tuni(d);
        if (remaining < cyc)
            remaining = 0;
        else
            remaining -= cyc;
    }
    d->frame_count++;
    dc_render(d);
    return EMU_OK;
}

/* ---- load / reset ------------------------------------------------------------------ */

static void dc_boot(struct dc *d)
{
    if (d->rom == NULL)
        return;
    /* documented direct-boot model: IP.BIN 0x300 = boot file LBA,
     * 0x308 = byte count; load to 0x8C010000, enter there. */
    uint32_t lba = emu_le32(&d->rom[0x300u]);
    uint32_t size = emu_le32(&d->rom[0x308u]);
    if (size == 0 || lba * 2048u + (uint64_t)size > d->rom_size)
        return; /* malformed: stay halted (validation catches this) */
    size_t off = (size_t)lba * 2048u;
    if (off + size > DC_RAM_SIZE)
        return;
    memcpy(&d->ram[0x00010000u], &d->rom[off], size); /* 0x8C010000-0x8C000000 */

    sh4_reset(&d->cpu);
    d->cpu.pc = 0x8C010000u;
    d->cpu.next_pc = d->cpu.pc + 2u;
    d->cpu.r[15] = 0x8CFF0000u; /* documented stack convention */
    d->frame_count = 0;
}

static emu_result_t dc_load_rom(emu_core_t *core, const uint8_t *data,
                                size_t size)
{
    struct dc *d = (struct dc *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    if (size < 0x9300u)
        return EMU_EBADROM;
    if (memcmp(data, "SEGA SEGAKATANA", 15) != 0)
        return EMU_EBADROM;
    uint32_t lba = emu_le32(&data[0x300u]);
    uint32_t blen = emu_le32(&data[0x308u]);
    if (lba == 0 || blen == 0 || (uint64_t)lba * 2048u + blen > size)
        return EMU_EBADROM;

    uint8_t *copy = malloc(size);
    if (copy == NULL)
        return EMU_EINVAL;
    memcpy(copy, data, size);
    free(d->rom);
    d->rom = copy;
    d->rom_size = size;

    memset(d->ram, 0, sizeof d->ram);
    memset(d->vram, 0, sizeof d->vram);
    memset(d->aica, 0, sizeof d->aica);
    memset(d->uram, 0, sizeof d->uram);
    memset(d->flash, 0xFF, sizeof d->flash); /* erased flash convention */
    memset(d->pvr_regs, 0, sizeof d->pvr_regs);
    memset(d->tmu, 0, sizeof d->tmu);
    d->tstr = 0;

    dc_boot(d);
    dc_render(d);
    return EMU_OK;
}

static void dc_reset(emu_core_t *core)
{
    struct dc *d = (struct dc *)core;
    if (d->rom == NULL)
        return;
    dc_boot(d);
    dc_render(d);
}

static const uint32_t *dc_fb(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct dc *d = (struct dc *)core;
    if (w != NULL)
        *w = DC_SCREEN_W;
    if (h != NULL)
        *h = DC_SCREEN_H;
    return d->fb;
}

static void dc_set_input(emu_core_t *core, uint32_t buttons)
{
    struct dc *d = (struct dc *)core;
    d->buttons = buttons; /* stored; Maple bus not modeled (documented) */
}

static void dc_set_audio(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    (void)core; (void)cb; (void)user; /* AICA not implemented: no audio */
}

/* ---- save states --------------------------------------------------------------------- */

static void dc_serialize(struct dc *d, emu_state_writer *w)
{
    sw_u32(w, 0x44434231u); /* "1BCD" */
    for (int i = 0; i < 16; i++)
        sw_u32(w, d->cpu.r[i]);
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 8; i++)
            sw_u32(w, d->cpu.rbank[b][i]);
    sw_u32(w, d->cpu.pc);
    sw_u32(w, d->cpu.next_pc);
    sw_u32(w, d->cpu.pr);
    sw_u32(w, d->cpu.sr);
    sw_u32(w, d->cpu.gbr);
    sw_u32(w, d->cpu.vbr);
    sw_u32(w, d->cpu.ssr);
    sw_u32(w, d->cpu.spc);
    sw_u32(w, d->cpu.sgr);
    sw_u32(w, d->cpu.dbr);
    sw_u32(w, d->cpu.mach);
    sw_u32(w, d->cpu.macl);
    sw_u32(w, d->cpu.fpul);
    sw_u32(w, d->cpu.fpscr);
    for (int i = 0; i < 16; i++)
        sw_u32(w, d->cpu.fr[i]);
    for (int i = 0; i < 16; i++)
        sw_u32(w, d->cpu.xf[i]);
    for (int ch = 0; ch < 3; ch++) {
        sw_u32(w, d->tmu[ch].tcor);
        sw_u32(w, d->tmu[ch].tcnt);
        sw_u16(w, d->tmu[ch].tcr);
        sw_u64(w, d->tmu[ch].acc);
    }
    sw_u8(w, d->tstr);
    for (int i = 0; i < 16; i++)
        sw_u32(w, d->intc_regs[i]);
    sw_u32(w, d->frame_count);
    sw_u32(w, d->buttons);
    sw_mem(w, d->pvr_regs, sizeof d->pvr_regs);
    sw_mem(w, d->flash, sizeof d->flash);
    sw_mem(w, d->aica, sizeof d->aica);
    sw_mem(w, d->vram, sizeof d->vram);
    sw_mem(w, d->ram, sizeof d->ram);
    sw_mem(w, d->uram, sizeof d->uram);
}

static size_t dc_state_size(emu_core_t *core)
{
    struct dc *d = (struct dc *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    dc_serialize(d, &w);
    return w.pos;
}

static emu_result_t dc_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    struct dc *d = (struct dc *)core;
    emu_state_writer w = { buf, cap, 0, 0 };
    dc_serialize(d, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t dc_load_state(emu_core_t *core, const uint8_t *buf,
                                  size_t size)
{
    struct dc *d = (struct dc *)core;
    emu_state_reader rd = { buf, size, 0, 0 };
    if (sr_u32(&rd) != 0x44434231u)
        return EMU_EBADSTATE;
    for (int i = 0; i < 16; i++)
        d->cpu.r[i] = sr_u32(&rd);
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 8; i++)
            d->cpu.rbank[b][i] = sr_u32(&rd);
    d->cpu.pc = sr_u32(&rd);
    d->cpu.next_pc = sr_u32(&rd);
    d->cpu.pr = sr_u32(&rd);
    d->cpu.sr = sr_u32(&rd);
    d->cpu.gbr = sr_u32(&rd);
    d->cpu.vbr = sr_u32(&rd);
    d->cpu.ssr = sr_u32(&rd);
    d->cpu.spc = sr_u32(&rd);
    d->cpu.sgr = sr_u32(&rd);
    d->cpu.dbr = sr_u32(&rd);
    d->cpu.mach = sr_u32(&rd);
    d->cpu.macl = sr_u32(&rd);
    d->cpu.fpul = sr_u32(&rd);
    d->cpu.fpscr = sr_u32(&rd);
    for (int i = 0; i < 16; i++)
        d->cpu.fr[i] = sr_u32(&rd);
    for (int i = 0; i < 16; i++)
        d->cpu.xf[i] = sr_u32(&rd);
    for (int ch = 0; ch < 3; ch++) {
        d->tmu[ch].tcor = sr_u32(&rd);
        d->tmu[ch].tcnt = sr_u32(&rd);
        d->tmu[ch].tcr = sr_u16(&rd);
        d->tmu[ch].acc = sr_u64(&rd);
    }
    d->tstr = sr_u8(&rd);
    for (int i = 0; i < 16; i++)
        d->intc_regs[i] = sr_u32(&rd);
    d->frame_count = sr_u32(&rd);
    d->buttons = sr_u32(&rd);
    sr_mem(&rd, d->pvr_regs, sizeof d->pvr_regs);
    sr_mem(&rd, d->flash, sizeof d->flash);
    sr_mem(&rd, d->aica, sizeof d->aica);
    sr_mem(&rd, d->vram, sizeof d->vram);
    sr_mem(&rd, d->ram, sizeof d->ram);
    sr_mem(&rd, d->uram, sizeof d->uram);
    if (rd.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

/* ---- vtable --------------------------------------------------------------------------- */

static emu_result_t dc_create(emu_core_t **out);
static void dc_destroy(emu_core_t *core);

static const emu_core_vtable_t dc_vtable = {
    "supercastpro", "Sega Dreamcast", DC_SCREEN_W, DC_SCREEN_H, DC_OUT_RATE,
    dc_create, dc_destroy, dc_load_rom, dc_reset,
    dc_run_frame, dc_fb, dc_set_input, dc_set_audio,
    dc_state_size, dc_save_state, dc_load_state,
};

static emu_result_t dc_create(emu_core_t **out)
{
    struct dc *d = calloc(1, sizeof *d);
    if (d == NULL)
        return EMU_EINVAL;
    d->base.vtable = &dc_vtable;
    sh4_init(&d->cpu, &d->bus);
    d->bus = dc_bus_tmpl;
    d->bus.user = d;
    *out = &d->base;
    return EMU_OK;
}

static void dc_destroy(emu_core_t *core)
{
    struct dc *d = (struct dc *)core;
    if (d == NULL)
        return;
    free(d->rom);
    free(d);
}

const emu_core_vtable_t *emu_core_supercastpro(void)
{
    return &dc_vtable;
}

/* ---- test/inspection hooks ------------------------------------------------------- */

uint32_t dc_read32(struct dc *d, uint32_t addr)
{
    return dc_bus_read32(d, addr);
}

void dc_write32(struct dc *d, uint32_t addr, uint32_t v)
{
    dc_bus_write32(d, addr, v);
}
