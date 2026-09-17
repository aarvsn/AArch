/*
 * ms-32: Sega 32X core implementation. See x32.h for the model and the
 * documented subset. SH-2 bus here covers the adapter-visible windows;
 * the Genesis side runs inside the composed finalburn machine.
 */
#include "x32.h"

#include <stdlib.h>
#include <string.h>

#include "../common/util.h"

#define X32_FB_WORDS (128u * 1024u / 2u)
#define X32_SH2_HZ   23010470u /* 3x the 68K clock (23.01 MHz)             */
#define X32_SH2_PER_FRAME (X32_SH2_HZ / 60u)
#define X32_SH2_PER_LINE  (X32_SH2_HZ / 60u / 262u)

/* ---- SH-2 bus ------------------------------------------------------------------ */

static uint32_t x32_rom_words(const struct x32 *x)
{
    return x->md->cart.rom_size >> 1;
}

/* Cartridge ROM is seen byte-wide at SH-2 0x02000000 (adapter maps the
 * 16-bit ROM onto the 8-bit SH-2 bus; consecutive bytes come from the same
 * word per the adapter's data path). */
static uint8_t sh2_rom_read8(const struct x32 *x, uint32_t a)
{
    uint32_t off = (a - 0x02000000u) & 0x3FFFFFu;
    if ((off >> 1) >= x32_rom_words(x))
        return 0xFF;
    return x->md->cart.rom[off];
}

uint16_t x32_sh2_read16(struct x32 *x, uint32_t addr)
{
    uint32_t a = addr & 0x1FFFFFFFu;

    /* adapter registers (word) */
    if (a >= 0x4000u && a < 0x4100u)
        return x32_md_read(x, 0xA15100u + (a - 0x4000u));
    if (a >= 0x4100u && a < 0x4120u) {
        switch (a - 0x4100u) {
        case X32_VDP_MODE:  return x->vdp_mode;
        case X32_VDP_SHIFT: return x->vdp_shift;
        case X32_VDP_FILLA: return x->vdp_filla;
        case X32_VDP_FILLD: return x->vdp_filld;
        case X32_VDP_AUTO:  return 0;
        default:            return 0;
        }
    }
    /* work RAM (both CPUs share; documented simplification) */
    if (a >= 0x06000000u && a < 0x06020000u) {
        uint32_t o = a - 0x06000000u;
        return (uint16_t)((uint16_t)x->sh2ram[o] << 8 | x->sh2ram[o ^ 1u]);
    }
    /* framebuffers (treated/untreated windows) */
    if (a >= 0x04000000u && a < 0x04040000u) {
        uint32_t o = (a - 0x04000000u) >> 1;
        if (o >= X32_FB_WORDS)
            o -= X32_FB_WORDS;
        const uint16_t *fb = x->fb_selected ? x->fb_b : x->fb_a;
        return fb[o];
    }
    /* ROM through the adapter */
    if (a >= 0x02000000u && a < 0x02400000u) {
        uint32_t off = (a - 0x02000000u) & ~1u;
        if ((off >> 1) < x32_rom_words(x))
            return (uint16_t)((x->md->cart.rom[off] << 8) |
                              x->md->cart.rom[off + 1]);
        return 0xFFFF;
    }
    return 0xFFFF;
}

static uint8_t sh2_read8(void *user, uint32_t a)
{
    struct x32 *x = user;
    a &= 0x1FFFFFFFu;
    if (a >= 0x02000000u && a < 0x02400000u)
        return sh2_rom_read8(x, a);
    uint16_t w = x32_sh2_read16(user, a & ~1u);
    return (uint8_t)((a & 1u) ? w : (w >> 8));
}

static uint32_t sh2_read32(void *user, uint32_t a)
{
    uint16_t hi = x32_sh2_read16(user, a);
    uint16_t lo = x32_sh2_read16(user, a + 2u);
    return ((uint32_t)hi << 16) | lo;
}

void x32_sh2_write16(struct x32 *x, uint32_t addr, uint16_t v)
{
    uint32_t a = addr & 0x1FFFFFFFu;

    if (a >= 0x4000u && a < 0x4100u) {
        x32_md_write(x, 0xA15100u + (a - 0x4000u), v);
        return;
    }
    if (a >= 0x4100u && a < 0x4120u) {
        switch (a - 0x4100u) {
        case X32_VDP_MODE:  x->vdp_mode = v; return;
        case X32_VDP_SHIFT: x->vdp_shift = v; return;
        case X32_VDP_FILLA: x->vdp_filla = v; return;
        case X32_VDP_FILLD: {
            /* fill the selected framebuffer from FILLA to the end */
            x->vdp_filld = v;
            uint16_t *fb = x->fb_selected ? x->fb_b : x->fb_a;
            for (uint32_t o = x->vdp_filla; o < X32_FB_WORDS; o++)
                fb[o] = v;
            return;
        }
        default: return;
        }
    }
    if (a >= 0x06000000u && a < 0x06020000u) {
        uint32_t o = a - 0x06000000u;
        x->sh2ram[o] = (uint8_t)(v >> 8);
        x->sh2ram[o ^ 1u] = (uint8_t)v;
        return;
    }
    if (a >= 0x04000000u && a < 0x04040000u) {
        uint32_t o = (a - 0x04000000u) >> 1;
        if (o >= X32_FB_WORDS)
            o -= X32_FB_WORDS;
        uint16_t *fb = x->fb_selected ? x->fb_b : x->fb_a;
        fb[o] = v;
        return;
    }
}

static void sh2_write8(void *user, uint32_t a, uint8_t v)
{
    /* byte writes to the framebuffers mirror the word path per adapter */
    struct x32 *x = user;
    a &= 0x1FFFFFFFu;
    if (a >= 0x04000000u && a < 0x04040000u) {
        /* byte write packs into the addressed byte of the word */
        uint32_t o = (a - 0x04000000u) >> 1;
        if (o >= X32_FB_WORDS)
            o -= X32_FB_WORDS;
        uint16_t *fb = x->fb_selected ? x->fb_b : x->fb_a;
        if (a & 1u)
            fb[o] = (uint16_t)((fb[o] & 0xFF00u) | v);
        else
            fb[o] = (uint16_t)((fb[o] & 0x00FFu) | (v << 8));
        return;
    }
    uint16_t w = x32_sh2_read16(x, a & ~1u);
    if (a & 1u)
        x32_sh2_write16(x, a & ~1u, (uint16_t)((w & 0xFF00u) | v));
    else
        x32_sh2_write16(x, a & ~1u, (uint16_t)((w & 0x00FFu) | (v << 8)));
}

static void sh2_write32(void *user, uint32_t a, uint32_t v)
{
    x32_sh2_write16(user, a, (uint16_t)(v >> 16));
    x32_sh2_write16(user, a + 2u, (uint16_t)v);
}

static uint16_t bus_read16(void *user, uint32_t a)
{
    return x32_sh2_read16(user, a);
}
static void bus_write16(void *user, uint32_t a, uint16_t v)
{
    x32_sh2_write16(user, a, v);
}

/* ---- 68K-side adapter window ------------------------------------------------------ */

uint16_t x32_md_read(void *ext, uint32_t addr)
{
    struct x32 *x = ext;
    switch (addr - 0xA15100u) {
    case X32_REG_RES:    return x->reg_res;
    case X32_REG_INTS:   return x->reg_ints;
    case X32_REG_HVEC:   return x->reg_hvec;
    case X32_REG_HCOUNT: return x->reg_hcount;
    case X32_REG_VVEC:   return x->reg_vvec;
    default: break;
    }
    if (addr - 0xA15100u >= X32_REG_COMM &&
        addr - 0xA15100u < X32_REG_COMM + 16u)
        return x->comm[(addr - 0xA15100u - X32_REG_COMM) >> 1];
    return 0;
}

void x32_md_write(void *ext, uint32_t addr, uint16_t v)
{
    struct x32 *x = ext;
    switch (addr - 0xA15100u) {
    case X32_REG_RES:
        x->reg_res = v & 0x0Fu;
        if ((v & 2u) == 0u && (v & 1u) != 0u && x->rom_ok) {
            /* RES release with ADEN: both SH-2s fetch SP/PC from the cart
             * vector area (documented model of the adapter boot). */
            for (int i = 0; i < 2; i++) {
                sh2_reset(&x->sh2[i]);
                x->sh2[i].r[15] = sh2_read32(x, 0x02000000u);
                x->sh2[i].pc = sh2_read32(x, 0x02000004u);
                x->sh2[i].next_pc = x->sh2[i].pc + 2u;
            }
        }
        return;
    case X32_REG_INTS:   x->reg_ints = v & 0x000Fu; return;
    case X32_REG_HVEC:   x->reg_hvec = v; return;
    case X32_REG_HCOUNT: x->reg_hcount = v & 0xFFu; return;
    case X32_REG_VVEC:   x->reg_vvec = v; return;
    default: break;
    }
    if (addr - 0xA15100u >= X32_REG_COMM &&
        addr - 0xA15100u < X32_REG_COMM + 16u) {
        x->comm[(addr - 0xA15100u - X32_REG_COMM) >> 1] = v;
        return;
    }
}

/* ---- scanline / frame timing -------------------------------------------------------- */

void x32_line_tick(struct x32 *x, uint32_t line, int in_vblank)
{
    (void)line;
    (void)in_vblank;
    /* run a scanline's worth of both SH-2s */
    for (int i = 0; i < 2; i++) {
        uint32_t budget = X32_SH2_PER_LINE;
        while (budget > 0) {
            uint32_t used = sh2_step(&x->sh2[i]);
            budget -= used > budget ? budget : used;
        }
    }
}

/* ---- VDP output ---------------------------------------------------------------------- */

static void x32_render(struct x32 *x)
{
    /* packed-pixel mode: each byte of a framebuffer word is a palette
     * index into the 256-color CRAM of the MD VDP; documented
     * simplification: RLE mode renders as packed pixel. */
        uint16_t *disp = x->fb_selected ? x->fb_b : x->fb_a;
    for (uint32_t y = 0; y < FB_SCREEN_H; y++) {
        uint32_t row = (y * 224u) / FB_SCREEN_H;
        for (uint32_t px = 0; px < FB_SCREEN_W; px++) {
            uint32_t col = (px * 320u) / FB_SCREEN_W;
            uint32_t off = row * 160u + (col >> 1);
            uint8_t idx = (uint8_t)((col & 1u) ? (disp[off] & 0xFFu)
                                               : (disp[off] >> 8));
            uint32_t r = 0, g = 0, b = 0;
            if (idx != 0) { /* palette entry 0 = transparent: show backdrop */
                uint16_t cram = 0;
                uint32_t ci = (uint32_t)idx * 2u;
                if (ci < FB_CRAM_SIZE)
                    cram = (uint16_t)(x->md->vdp.cram[ci] << 8 |
                                      x->md->vdp.cram[ci + 1]);
                r = ((cram >> 1) & 7u) << 5; r |= r >> 5;
                g = ((cram >> 5) & 7u) << 5; g |= g >> 5;
                b = ((cram >> 9) & 7u) << 5; b |= b >> 5;
            }
            x->fb[y * FB_SCREEN_W + px] =
                EMU_PIXEL((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
}

/* ---- core vtable ----------------------------------------------------------------------- */

static emu_result_t x32_load_rom(emu_core_t *core, const uint8_t *data,
                                 size_t size)
{
    struct x32 *x = (struct x32 *)core;
    emu_result_t r = emu_core_finalburn()->load_rom(&x->md->base, data, size);
    if (r != EMU_OK)
        return r;
    x->rom_ok = 1;
    return EMU_OK;
}

static void x32_reset(emu_core_t *core)
{
    struct x32 *x = (struct x32 *)core;
    emu_core_finalburn()->reset(&x->md->base);
    sh2_reset(&x->sh2[0]);
    sh2_reset(&x->sh2[1]);
    x->reg_res = 0;
    x->reg_ints = 0;
    x->reg_hvec = 0;
    x->reg_hcount = 0;
    x->reg_vvec = 0;
    memset(x->comm, 0, sizeof x->comm);
    x->vdp_mode = 0;
    x->vdp_shift = 0;
    x->vdp_filla = 0;
    x->vdp_filld = 0;
    memset(x->fb_a, 0, sizeof x->fb_a);
    memset(x->fb_b, 0, sizeof x->fb_b);
    x->fb_selected = 0;
}

static emu_result_t x32_run_frame(emu_core_t *core)
{
    struct x32 *x = (struct x32 *)core;
    if (!x->rom_ok)
        return EMU_ENOROM;

    /* run the Genesis frame (68K/Z80/VDP) with the adapter window live */
    emu_result_t r = emu_core_finalburn()->run_frame(&x->md->base);

    /* run the SH-2s for one frame */
    for (int i = 0; i < 2; i++) {
        uint32_t budget = X32_SH2_PER_FRAME;
        while (budget > 0) {
            uint32_t used = sh2_step(&x->sh2[i]);
            budget -= used > budget ? budget : used;
        }
    }

    x32_render(x);
    return r;
}

static const uint32_t *x32_fb(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct x32 *x = (struct x32 *)core;
    if (w)
        *w = FB_SCREEN_W;
    if (h)
        *h = FB_SCREEN_H;
    return x->fb;
}

static void x32_set_input(emu_core_t *core, uint32_t buttons)
{
    struct x32 *x = (struct x32 *)core;
    emu_core_finalburn()->set_input(&x->md->base, buttons);
}

static void x32_set_audio(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    struct x32 *x = (struct x32 *)core;
    emu_core_finalburn()->set_audio_callback(&x->md->base, cb, user);
}

static size_t x32_state_size(emu_core_t *core)
{
    struct x32 *x = (struct x32 *)core;
    size_t s = emu_core_finalburn()->state_size(&x->md->base);
    /* adapter state: regs + comm + SH-2 cores + VDP + framebuffers */
    s += 64u + sizeof x->comm + 2u * sizeof(sh2_t) + 64u + sizeof x->fb_a +
         sizeof x->fb_b;
    return s;
}

static emu_result_t x32_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    struct x32 *x = (struct x32 *)core;
    emu_result_t r = emu_core_finalburn()->save_state(&x->md->base, buf, cap);
    if (r != EMU_OK)
        return r;
    size_t md_size = emu_core_finalburn()->state_size(&x->md->base);
    emu_state_writer w = { buf, cap, md_size, 0 };
    sw_u32(&w, 0x58333253u); /* "X32S" */
    sw_u16(&w, x->reg_res);
    sw_u16(&w, x->reg_ints);
    sw_u16(&w, x->reg_hvec);
    sw_u16(&w, x->reg_hcount);
    sw_u16(&w, x->reg_vvec);
    for (int i = 0; i < 8; i++)
        sw_u16(&w, x->comm[i]);
    sw_u16(&w, x->vdp_mode);
    sw_u16(&w, x->vdp_shift);
    sw_u16(&w, x->vdp_filla);
    sw_u16(&w, x->vdp_filld);
    sw_u16(&w, x->fb_selected);
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 16; k++)
            sw_u32(&w, x->sh2[i].r[k]);
        sw_u32(&w, x->sh2[i].pc);
        sw_u32(&w, x->sh2[i].next_pc);
        sw_u32(&w, x->sh2[i].pr);
        sw_u32(&w, x->sh2[i].sr);
        sw_u32(&w, x->sh2[i].gbr);
        sw_u32(&w, x->sh2[i].vbr);
        sw_u32(&w, x->sh2[i].mach);
        sw_u32(&w, x->sh2[i].macl);
    }
    sw_mem(&w, x->fb_a, sizeof x->fb_a);
    sw_mem(&w, x->fb_b, sizeof x->fb_b);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t x32_load_state(emu_core_t *core, const uint8_t *buf,
                                   size_t size)
{
    struct x32 *x = (struct x32 *)core;
    emu_result_t r = emu_core_finalburn()->load_state(&x->md->base, buf, size);
    if (r != EMU_OK)
        return r;
    size_t md_size = emu_core_finalburn()->state_size(&x->md->base);
    if (size < md_size + 4u)
        return EMU_EBADSTATE;
    emu_state_reader rd = { buf, size, md_size, 0 };
    if (sr_u32(&rd) != 0x58333253u)
        return EMU_EBADSTATE;
    x->reg_res = sr_u16(&rd);
    x->reg_ints = sr_u16(&rd);
    x->reg_hvec = sr_u16(&rd);
    x->reg_hcount = sr_u16(&rd);
    x->reg_vvec = sr_u16(&rd);
    for (int i = 0; i < 8; i++)
        x->comm[i] = sr_u16(&rd);
    x->vdp_mode = sr_u16(&rd);
    x->vdp_shift = sr_u16(&rd);
    x->vdp_filla = sr_u16(&rd);
    x->vdp_filld = sr_u16(&rd);
    x->fb_selected = (uint8_t)sr_u16(&rd);
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 16; k++)
            x->sh2[i].r[k] = sr_u32(&rd);
        x->sh2[i].pc = sr_u32(&rd);
        x->sh2[i].next_pc = sr_u32(&rd);
        x->sh2[i].pr = sr_u32(&rd);
        x->sh2[i].sr = sr_u32(&rd);
        x->sh2[i].gbr = sr_u32(&rd);
        x->sh2[i].vbr = sr_u32(&rd);
        x->sh2[i].mach = sr_u32(&rd);
        x->sh2[i].macl = sr_u32(&rd);
    }
    sr_mem(&rd, x->fb_a, sizeof x->fb_a);
    sr_mem(&rd, x->fb_b, sizeof x->fb_b);
    if (rd.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

static emu_result_t x32_create(emu_core_t **out);
static void x32_destroy(emu_core_t *core);

static const emu_core_vtable_t x32_vtable = {
    "ms-32", "Sega 32X", FB_SCREEN_W, FB_SCREEN_H, FB_OUT_RATE,
    x32_create, x32_destroy, x32_load_rom, x32_reset,
    x32_run_frame, x32_fb, x32_set_input, x32_set_audio,
    x32_state_size, x32_save_state, x32_load_state,
};

static emu_result_t x32_create(emu_core_t **out)
{
    struct x32 *x = calloc(1, sizeof *x);
    if (x == NULL)
        return EMU_EINVAL;
    emu_core_t *md = NULL;
    if (emu_core_finalburn()->create(&md) != EMU_OK) {
        free(x);
        return EMU_EINVAL;
    }
    /* emu_core_t is the first member of struct fb_md, so the finalburn
     * handle doubles as the composed machine pointer. */
    x->md = (struct fb_md *)md;
    x->base.vtable = &x32_vtable;

    /* wire the adapter window into the 68K bus */
    x->md->ext32x = x;
    x->md->ext32x_read = x32_md_read;
    x->md->ext32x_write = x32_md_write;

    /* SH-2 buses (per-instance bindings; callbacks are shared) */
    for (int i = 0; i < 2; i++) {
        x->sh2_bus[i].user = x;
        x->sh2_bus[i].read8 = sh2_read8;
        x->sh2_bus[i].read16 = bus_read16;
        x->sh2_bus[i].read32 = sh2_read32;
        x->sh2_bus[i].write8 = sh2_write8;
        x->sh2_bus[i].write16 = bus_write16;
        x->sh2_bus[i].write32 = sh2_write32;
        sh2_init(&x->sh2[i], &x->sh2_bus[i]);
    }

    *out = &x->base;
    return EMU_OK;
}

static void x32_destroy(emu_core_t *core)
{
    struct x32 *x = (struct x32 *)core;
    if (x == NULL)
        return;
    if (x->md != NULL)
        emu_core_finalburn()->destroy(&x->md->base);
    free(x);
}

const emu_core_vtable_t *emu_core_ms_32(void)
{
    return &x32_vtable;
}
