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
 *  - Video: PowerVR2 display-controller scanout: FB_R_CTRL (0x5F8044),
 *    FB_R_SIZE (0x5F804C), FB_R_SOF1/2 (0x5F8060/64). Pixel format decode
 *    (documented): (FB_R_CTRL>>2)&3 with 0 = RGB565, 2 = RGB888,
 *    3 = ARGB8888; x = modules of 4 pixels, y = lines.
 *  - Tile Accelerator (documented subset):
 *      * 32-bit writes to the window 0x10000000-0x11FFFFFF append one
 *        parameter word each to a 64 Ki-word FIFO (8/16-bit writes and
 *        overflow are ignored; the store-queue path is not modeled).
 *      * A STARTRENDER (PVR 0x034) write with bit 0 set parses the FIFO
 *        synchronously (documented simplification of the async TA) and
 *        rasterizes into the VRAM framebuffer described by FB_W_CTRL
 *        (0x5F8040, pack bits 2:0: 0/1 = RGB565, 3 = RGB888, 4 = ARGB8888),
 *        FB_W_LINESTRIDE (0x5F8042, 32-bit words, 0 = w*bpp/4) and
 *        FB_W_SOF1 (0x5F8044). The write target always covers the full
 *        640x480 tile space. The FIFO is cleared after the pass.
 *      * Parameter subset: control words 0x80/0x81/0x82/0x87 are skipped
 *        as one word; 0x83 (end) terminates the parse; polygon headers
 *        0x88-0x8C (opaque) / 0x90-0x94 (transparent) / 0x98-0x9C
 *        (punch-through) consume the documented word counts (3/4/4/5/5
 *        for types 0-4 opaque, +1 for transparent and punch-through);
 *        sprite headers 0x8D/0x95/0x9D consume 3/4/4.
 *      * Vertices: a tag word 0xE0-0xEF followed by 4 words (X, Y, Z as
 *        IEEE floats and ARGB8888 color bits); a strip ends at the next
 *        non-vertex word (real termination semantics). Sprites use the
 *        3 vertex groups A/B/C/D-implied.
 *      * Coordinates use the real TA convention (origin at screen center,
 *        Y up): sx = x + 320, sy = 240 - y, clipped to 640x480. Z is
 *        ignored. Primitives draw in stream order with the documented
 *        contract that lists are submitted contiguously opaque ->
 *        punch-through -> transparent (hardware buckets by list and
 *        sorts per tile; the subset is a painter's-order rasterizer).
 *        Strips are triangulated; sprites are axis-aligned rects built
 *        from A (base) plus the B (x) and C (y) edges. Fill color is
 *        the first vertex's (flat; Gouraud/alpha blending pending).
 *  - AICA (documented subset):
 *      * ARM7TDMI (shared ARMv4T interpreter, no v5te) at 33.8688 MHz
 *        running from the 2 MiB wave RAM (ARM7 map 0x00000000) with the
 *        AICA local register window at 0x00800000.
 *      * SH-4 side: wave RAM at 0x00400000 (above), AICA registers at
 *        0x00700000-0x00707FFF (G2 byte-lane swapping not modeled).
 *      * Channel registers: 64 channels x 32 x 16-bit at 32-bit stride,
 *        stored raw and decoded live: +0x00 bits 14:11 sample format
 *        (0 = PCM16 LE, 1 = PCM8 unsigned, 2 = ADPCM - decoded as
 *        silence, pending), bits 10:0 SA>>16; +0x04 SA&0xFFFF; +0x08
 *        LSA; +0x0C LEA (byte addresses, LEA exclusive); +0x10 CA
 *        readback = position >> 16; +0x24 bits 15:11 octave (signed)
 *        and bits 10:0 FSC, rate = 44100 * 2^oct * (1 + FSC/1024);
 *        +0x30 bit 14 loop, bit 0 KYONB; +0x38 bits 7:0 volume (0 =
 *        0 dB, 255 = silence; linear attenuation is a documented
 *        simplification of the hardware dB curve); +0x3C bits 7:0 pan
 *        (0 = left, 128 = center, 255 = right; linear gains).
 *      * Common registers: +0x2800 KYONEX (write bit 14 latches every
 *        channel's KYONB: key-on resets the phase, key-off stops);
 *        +0x2898 MCIPD (SH-4 write bit 14 pulses the ARM7 IRQ line);
 *        +0x289C MCIRE (ARM7 write bit 9 sets a flag the SH-4 reads
 *        and clears with bit 9; documented minimal doorbells).
 *      * Mixer: 44100 Hz stereo, straight saturated sum of active
 *        channels, nearest sample (no interpolation), no AEG/LFO/DSP
 *        (documented). The voice position is a byte address advancing
 *        bytes-per-sample * rate/44100 per output sample; one-shot
 *        channels stop at LEA and loops wrap to LSA (LEA exclusive).
 *        The ARM7 runs its full per-frame budget and the audio frame
 *        is generated at frame end (documented: not cycle-interleaved).
 *  - Not implemented (honest gaps): TA textures/UVs, modifier volumes,
 *    user tile clip, per-tile depth sort, Gouraud and alpha blending,
 *    store-queue FIFO path; AICA ADPCM, AEG/LFO/DSP; GD-ROM drive
 *    (registers stubbed; the boot copy is done by the loader), G2 DMA,
 *    Maple bus/input, modem, MIU, AICA->SH-4 interrupt delivery.
 */
#ifndef EMU_SUPERCASTPRO_DC_H
#define EMU_SUPERCASTPRO_DC_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"
#include "../common/arm.h"
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
#define DC_PVR_STARTRENDER 0x034u
#define DC_PVR_FB_R_CTRL 0x044u
#define DC_PVR_FB_R_SIZE 0x04Cu
#define DC_PVR_FB_R_SOF1 0x060u
#define DC_PVR_FB_R_SOF2 0x064u
#define DC_PVR_FB_W_CTRL 0x040u
#define DC_PVR_FB_W_LINESTRIDE 0x042u
#define DC_PVR_FB_W_SOF1 0x044u

/* Tile Accelerator FIFO window (32-bit writes append one word) */
#define DC_TA_BASE 0x10000000u
#define DC_TA_WINDOW (32u * 1024u * 1024u)
#define DC_TA_FIFO_WORDS (64u * 1024u)

/* AICA register space (SH-4 view at 0x00700000, ARM7 view at 0x00800000) */
#define DC_AICA_REG_BASE 0x00700000u
#define DC_AICA_REG_SIZE 0x8000u
#define DC_AICA_CH 64u
#define DC_AICA_HZ 33868800u /* ARM7 clock */
#define DC_AICA_ARM_WINDOW 0x00800000u

/* AICA common register offsets (from DC_AICA_REG_BASE) */
#define DC_AICA_KYONEX 0x2800u
#define DC_AICA_MCIPD 0x2898u
#define DC_AICA_MCIRE 0x289Cu
#define DC_AICA_KYONEX_BIT 0x4000u
#define DC_AICA_MCIPD_BIT 0x4000u
#define DC_AICA_MCIRE_BIT 0x0200u

/* AICA sample formats (channel reg +0x00 bits 14:11) */
#define DC_AICA_FMT_PCM16 0u
#define DC_AICA_FMT_PCM8 1u
#define DC_AICA_FMT_ADPCM 2u

struct dc_tmu {
    uint32_t tcor, tcnt;
    uint16_t tcr;
    uint64_t acc; /* sub-divider cycle accumulator */
    int unf_pending;
};

/* Tile Accelerator: parameter FIFO (filled by SH-4 writes, consumed by
 * the STARTRENDER pass). */
struct dc_ta {
    uint32_t fifo[DC_TA_FIFO_WORDS];
    uint32_t count;
    uint32_t overflow; /* sticky flag (test/inspection) */
};

/* AICA: ARM7 + raw channel registers + live voice state. */
struct dc_aica {
    arm_t cpu;
    uint16_t ch[DC_AICA_CH][32];       /* raw 16-bit channel registers */
    uint64_t pos[DC_AICA_CH];          /* sample position, 16.16 fixed */
    uint8_t active[DC_AICA_CH];
    uint8_t mci_flag;                  /* ARM7 -> SH-4 doorbell latch */
};

struct dc {
    emu_core_t base;

    sh4_t cpu;
    sh4_bus_t bus; /* per-instance copy (user = this struct) */

    uint8_t bootrom[DC_BOOTROM_SIZE]; /* RAM-backed, zero (no BIOS) */
    uint8_t flash[DC_FLASH_SIZE];
    uint8_t aica_ram[DC_AICA_SIZE]; /* AICA wave memory (2 MiB, shared) */
    uint8_t vram[DC_VRAM_SIZE];
    uint8_t ram[DC_RAM_SIZE];
    uint8_t uram[DC_URAM_SIZE];

    uint8_t *rom;        /* loaded disc image (owned copy) */
    size_t rom_size;

    struct dc_tmu tmu[DC_TMU_CHANNELS];
    uint8_t tstr;
    uint32_t intc_regs[0x40 / 4];
    uint8_t pvr_regs[DC_PVR_SIZE];

    struct dc_ta ta;
    struct dc_aica aica;

    emu_audio_cb_t audio_cb;
    void *audio_user;

    uint32_t buttons;
    uint32_t frame_count;
    uint32_t fb[DC_SCREEN_W * DC_SCREEN_H];
};

/* Test/inspection hooks (not part of the public API). */
uint32_t dc_read32(struct dc *d, uint32_t addr);
void dc_write32(struct dc *d, uint32_t addr, uint32_t v);
void dc_step(struct dc *d); /* one SH-4 instruction + one AICA ARM7 instruction */
void dc_render(struct dc *d);

/* Tile Accelerator (ta.c). */
void dc_ta_push(struct dc *d, uint32_t word);
void dc_ta_render(struct dc *d); /* parse FIFO, rasterize, clear FIFO */

/* AICA (aica.c). off is relative to DC_AICA_REG_BASE. */
void dc_aica_arm_init(struct dc *d);
uint32_t dc_aica_reg_read(struct dc *d, uint32_t off);
void dc_aica_reg_write(struct dc *d, uint32_t off, uint32_t v);
void dc_aica_arm_step(struct dc *d, uint32_t cycles);
void dc_aica_mix(struct dc *d, int16_t *out, size_t frames);

#endif /* EMU_SUPERCASTPRO_DC_H */
