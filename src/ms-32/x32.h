/*
 * ms-32: Sega 32X core (PARTIAL).
 *
 * Composes the finalburn Genesis machine with a real 32X adapter: two SH-2
 * CPUs (through the shared interpreter), the adapter register file (adapter
 * control with ADEN/RES/SOMAP/RV handshake, interrupt control with CMD/
 * HINT/VINT enables and vectors, H counter), the eight COMM ports shared
 * between the 68000 and both SH-2s, the DREQ DMA engine (68K -> SH-2), the
 * 32X VDP in 256-color packed-pixel mode with framebuffer select and fill,
 * and the dual 128 KiB framebuffers.
 *
 * Documented gaps: RLE pixel mode and autosprites are not implemented (the
 * mode register is honored and packed-pixel output is rendered); the PWM
 * audio stage is not implemented (silent output); cartridge SRAM/bank
 * switching windows and the 32X-to-68000 external-interrupt details are
 * simplified. Genesis-only titles still run through the composed finalburn
 * machine unchanged.
 */
#ifndef EMU_MS32_X32_H
#define EMU_MS32_X32_H

#include "emu/emu.h"
#include "../finalburn/fb_md.h"
#include "../common/sh2.h"

/* 32X adapter registers (MD window A15100h; SH-2 window 0x4000+). */
#define X32_REG_RES     0x00u /* adapter control                        */
#define X32_REG_INTS    0x02u /* interrupt control                      */
#define X32_REG_HVEC    0x04u /* H interrupt vector                     */
#define X32_REG_HCOUNT  0x06u /* H count compare                        */
#define X32_REG_VVEC    0x08u /* V interrupt vector                     */
#define X32_REG_COMM    0x20u /* COMM0..7 (16-bit each)                 */
#define X32_REG_CPUR    0x18u /* SH-2 interrupt status (CPU2 bus)        */

/* SH-2-side windows */
#define X32_SH2_REG     0x4000u /* adapter register window base          */
#define X32_SH2_COMM    0x4020u
#define X32_SH2_PWM     0x4040u
#define X32_SH2_VDP     0x4100u

/* VDP registers (SH-2 window 0x4100+; mirrored at A15180 on the MD side) */
#define X32_VDP_MODE    0x00u
#define X32_VDP_SHIFT   0x02u
#define X32_VDP_FILLA   0x04u
#define X32_VDP_FILLD   0x06u
#define X32_VDP_AUTO    0x08u

struct x32_dreq {
    uint32_t src;         /* 68K source address                        */
    uint32_t dst;         /* SH-2 destination address                  */
    uint16_t len;         /* words remaining                          */
    uint8_t  active;      /* DREQV start flag                          */
    uint8_t  fifo_full;
};

struct x32 {
    emu_core_t base;

    struct fb_md *md;     /* composed Genesis machine (owned)          */
    sh2_t sh2[2];         /* 0 = master, 1 = slave                     */
    sh2_bus_t sh2_bus[2]; /* per-instance bus bindings (user = this)   */
    uint8_t sh2ram[0x20000]; /* shared 128 KiB SH-2 work RAM           */

    uint16_t reg_res;     /* adapter control                           */
    uint16_t reg_ints;    /* interrupt control                         */
    uint16_t reg_hvec;
    uint16_t reg_hcount;
    uint16_t reg_vvec;
    uint16_t comm[8];

    uint16_t vdp_mode;
    uint16_t vdp_shift;
    uint16_t vdp_filla;
    uint16_t vdp_filld;

    struct x32_dreq dreq;

    uint16_t fb_a[128 * 1024 / 2]; /* 128 KiB framebuffer A (words)     */
    uint16_t fb_b[128 * 1024 / 2]; /* framebuffer B                     */
    uint8_t fb_selected;  /* 0 = A displayed, 1 = B (FEN bit)          */

    uint32_t fb[FB_SCREEN_W * FB_SCREEN_H];
    uint8_t rom_ok;
};

/* 68K-side adapter register window dispatch (registered as ext32x hook). */
uint16_t x32_md_read(void *ext, uint32_t addr);
void x32_md_write(void *ext, uint32_t addr, uint16_t v);

/* Advance both SH-2s by one scanline worth of cycles and raise HINTs. */
void x32_line_tick(struct x32 *x, uint32_t line, int in_vblank);

/* SH-2 bus access (exposed for tests and the DREQ engine). */
uint16_t x32_sh2_read16(struct x32 *x, uint32_t addr);
void x32_sh2_write16(struct x32 *x, uint32_t addr, uint16_t v);

#endif /* EMU_MS32_X32_H */
