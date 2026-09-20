/*
 * supercastpro: Sega Dreamcast core (partial).
 *
 * Sources and scope (documented subset):
 *  - CPU: SH-4 (SH7750) via the shared interpreter (src/common/sh4.{c,h});
 *    200 MHz coarse model, P0/P1/P2/P3 address decoding with the
 *    documented physical windows, P4 on-chip module area for TMU/INTC/
 *    SCIF, URAM (on-chip RAM) at 0x7C000000 and its P4 mirror.
 *  - Boot model: no BIOS. The image is a raw 2048-byte/sector data track
 *    whose sector 0 is the IP.BIN System ID header ("SEGA SEGAKATANA").
 *    The boot program location follows the IP.BIN layout: LBA at 0x300
 *    and byte count at 0x308 (little-endian). The loader copies that
 *    region to main RAM at 0x8C010000 (the documented 1st_read.bin load
 *    address used by Sega's bootstrap and KOS), then enters the SH-4 at
 *    0x8C010000 with SR = BL|MD and r15 = 0x8CFF0000 (documented
 *    convention; homebrew usually replaces the stack).
 *  - Memory: 16 MiB main RAM (0x0C000000), 8 MiB video RAM
 *    (0x05000000), 2 MiB boot ROM (0x00000000, RAM-backed zeros - no
 *    BIOS), 128 KiB flash stub (0x00200000), 2 MiB AICA wave RAM
 *    (0x00400000), 16 KiB URAM (0x7C000000).
 *  - Timers: SH-4 TMU channels 0-2 at P4 0xFFD80000 (TSTR, TCOR/TCNT/
 *    TCR), input = 200 MHz / {4,16,64,256}, underflow flag + TUNI0-2
 *    interrupts (VBR + 0x400/0x420/0x440) when TCR.UNIE is set.
 *  - Video: PowerVR2 display-controller scanout only: FB_R_CTRL
 *    (0x5F8044), FB_R_SIZE (0x5F804C), FB_R_SOF1/2 (0x5F8060/64).
 *    Pixel format decode (documented): (FB_R_CTRL>>2)&3 with 0 = RGB565,
 *    2 = RGB888, 3 = ARGB8888; x = modules of 4 pixels, y = lines.
 *    The Tile Accelerator / rasterizer is a stub, so content must write
 *    the VRAM framebuffer directly (homebrew direct-VRAM convention).
 *  - Not implemented (honest gaps): Tile Accelerator 3D, AICA ARM7 +
 *    64-channel sound (registers stored, no audio), GD-ROM drive
 *    (registers stubbed; the boot copy is done by the loader), G2 DMA,
 *    Maple bus/input, modem, MIU.
 */
#ifndef EMU_SUPERCASTPRO_DC_H
#define EMU_SUPERCASTPRO_DC_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"
#include "../common/sh4.h"

#define DC_SCREEN_W 640u
#define DC_SCREEN_H 480u
#define DC_OUT_RATE 44100u

#define DC_SH4_HZ 200000000u
#define DC_CYCLES_PER_FRAME (DC_SH4_HZ / 60u)

/* P4 on-chip module bases */
#define DC_TMU_BASE 0xFFD80000u
#define DC_INTC_BASE 0xFFD00000u
#define DC_SCIF_BASE 0xFFE80000u

/* SH-4 TMU */
#define DC_TMU_CHANNELS 3u

/* Dreamcast physical bases */
#define DC_PHYS_BOOTROM 0x00000000u
#define DC_BOOTROM_SIZE (2u * 1024u * 1024u)
#define DC_PHYS_FLASH 0x00200000u
#define DC_FLASH_SIZE (128u * 1024u)
#define DC_PHYS_AICA 0x00400000u
#define DC_AICA_SIZE (2u * 1024u * 1024u)
#define DC_PVR_BASE 0x005F8000u
#define DC_PVR_SIZE 0x8000u
#define DC_PHYS_VRAM 0x05000000u
#define DC_VRAM_SIZE (8u * 1024u * 1024u)
#define DC_PHYS_RAM 0x0C000000u
#define DC_RAM_SIZE (16u * 1024u * 1024u)
#define DC_URAM_BASE 0x7C000000u
#define DC_URAM_SIZE (16u * 1024u)

/* PVR register offsets (from 0x005F8000) */
#define DC_PVR_FB_R_CTRL 0x044u
#define DC_PVR_FB_R_SIZE 0x04Cu
#define DC_PVR_FB_R_SOF1 0x060u
#define DC_PVR_FB_R_SOF2 0x064u

struct dc_tmu {
    uint32_t tcor, tcnt;
    uint16_t tcr;
    uint64_t acc; /* sub-divider cycle accumulator */
    int unf_pending;
};

struct dc {
    emu_core_t base;

    sh4_t cpu;
    sh4_bus_t bus; /* per-instance copy (user = this struct) */

    uint8_t bootrom[DC_BOOTROM_SIZE]; /* RAM-backed, zero (no BIOS) */
    uint8_t flash[DC_FLASH_SIZE];
    uint8_t aica[DC_AICA_SIZE];
    uint8_t vram[DC_VRAM_SIZE];
    uint8_t ram[DC_RAM_SIZE];
    uint8_t uram[DC_URAM_SIZE];

    uint8_t *rom;        /* loaded disc image (owned copy) */
    size_t rom_size;

    struct dc_tmu tmu[DC_TMU_CHANNELS];
    uint8_t tstr;
    uint32_t intc_regs[0x40 / 4];
    uint8_t pvr_regs[DC_PVR_SIZE];

    uint32_t buttons;
    uint32_t frame_count;
    uint32_t fb[DC_SCREEN_W * DC_SCREEN_H];
};

/* Test/inspection hooks (not part of the public API). */
uint32_t dc_read32(struct dc *d, uint32_t addr);
void dc_write32(struct dc *d, uint32_t addr, uint32_t v);
void dc_step(struct dc *d); /* one SH-4 instruction */
void dc_render(struct dc *d);

#endif /* EMU_SUPERCASTPRO_DC_H */
