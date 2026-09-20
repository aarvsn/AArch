/*
 * mds-a: Nintendo DS core (partial).
 *
 * Sources and scope (documented subset):
 *  - CPUs: ARM946E-S (v5TE features enabled) + ARM7TDMI via the shared
 *    interpreter (src/common/arm.{c,h}), both running full ARM and Thumb.
 *  - Boot model: direct boot from the cartridge header (the documented
 *    flashcart/DSBooter convention, no BIOS): the ARM9 and ARM7 code
 *    regions are copied from the ROM image to their header addresses
 *    (NDS header 0x20/0x24/0x28/0x2C and 0x30/0x34/0x38/0x3C) and each
 *    CPU enters at its entry point in System mode with IRQ/FIQ masked
 *    (CPSR = 0x5F | mode). ARM9 sp = 0x023FC000, ARM7 sp = 0x0380FC00
 *    (documented convention; homebrew usually replaces these).
 *  - Memory: 4 MiB main RAM (0x02000000), ITCM 32 KiB @ 0x01000000
 *    (ARM9, CP15-configurable), DTCM 16 KiB @ 0x08000000 (ARM9,
 *    CP15-configurable), shared WRAM 32 KiB (ARM9 @ 0x03000000, ARM7 @
 *    0x037F8000), ARM7 WRAM 64 KiB @ 0x03800000, VRAM banks A-G at their
 *    default linear addresses (0x06000000.., 128+128+128+128+64+16+16
 *    KiB; VRAMCNT banking writes are ignored - documented), BIOS regions
 *    are RAM-backed zeros (no BIOS), GBA slot reads 0.
 *  - Interrupts: per-CPU IME/IE/IF (0x04000208/10/14) with VBlank(0),
 *    VCount(2), Timer0-3(3-6), IPC sync(16) sources; latched IF, simple
 *    priority, ARM7 and ARM9 fully independent.
 *  - Timers: 4 per CPU (0x04000100..), 16-bit with reload, prescalers
 *    1/64/256/1024, cascade, IRQ enable.
 *  - IPC: IPCSYNC (0x04000180) send/recv nibbles cross between CPUs and
 *    can raise the remote IRQ when enabled.
 *  - Video: both 2D engines, BG extended-bitmap modes 3 and 5 (16-bit
 *    BGR555) scanned out of VRAM A (engine A, 0x06000000) / B (engine B,
 *    0x06200000); mode 5 uses the DISPCNT page bit with a 0xA000 stride.
 *    Engine A renders to the top screen (rows 0-191 of the 256x384
 *    framebuffer), engine B to the bottom (rows 192-383). Text/affine
 *    modes render black (2D compositing is a documented stub).
 *  - Input: KEYINPUT at 0x04000130 (active-low): A=0 B=1 Select=2
 *    Start=3 Right=4 Left=5 Up=6 Down=7 R=8 L=9 X=10 Y=11; touch is not
 *    modeled.
 *  - Not implemented (honest gaps): 3D engine, sprite compositing, sound
 *    (0x04000400 stub), card bus (registers stub; the boot copies are
 *    done by the loader), Wi-Fi, touch, firmware settings, cache
 *    modeling (CP15 c1/c3/c7/c10 accepted and stored).
 *
 * Frame model: 263 scanlines per 60 Hz frame; ARM9 runs 67.028 MHz / 60
 * cycles per frame, ARM7 33.514 MHz / 60 (coarse, documented).
 */
#ifndef EMU_MDS_A_DS_H
#define EMU_MDS_A_DS_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"
#include "../common/arm.h"

#define DS_SCREEN_W 256u
#define DS_SCREEN_H 192u
#define DS_FB_H     384u /* top screen (engine A) + bottom screen (engine B) */
#define DS_OUT_RATE 32768u

#define DS_ARM9_HZ 67027968u
#define DS_ARM7_HZ 33513982u
#define DS_LINES_PER_FRAME 263u

/* NDS header offsets */
#define DS_HDR_ARM9_OFF   0x20u
#define DS_HDR_ARM9_ENTRY 0x24u
#define DS_HDR_ARM9_ADDR  0x28u
#define DS_HDR_ARM9_SIZE  0x2Cu
#define DS_HDR_ARM7_OFF   0x30u
#define DS_HDR_ARM7_ENTRY 0x34u
#define DS_HDR_ARM7_ADDR  0x38u
#define DS_HDR_ARM7_SIZE  0x3Cu
#define DS_HDR_LOGO       0x160u

/* DS memory sizes */
#define DS_MAIN_RAM_SIZE (4u * 1024u * 1024u)
#define DS_ITCM_SIZE     (32u * 1024u)
#define DS_DTCM_SIZE     (16u * 1024u)
#define DS_WRAM_SH_SIZE  (32u * 1024u)
#define DS_WRAM7_SIZE    (64u * 1024u)
#define DS_VRAM_A_SIZE   (128u * 1024u)
#define DS_VRAM_B_SIZE   (128u * 1024u)
#define DS_VRAM_C_SIZE   (128u * 1024u)
#define DS_VRAM_D_SIZE   (128u * 1024u)
#define DS_VRAM_E_SIZE   (64u * 1024u)
#define DS_VRAM_F_SIZE   (16u * 1024u)
#define DS_VRAM_G_SIZE   (16u * 1024u)

/* IRQ bits (IF/IE) */
#define DS_IRQ_VBLANK 0x0001u
#define DS_IRQ_HBLANK 0x0002u
#define DS_IRQ_VCOUNT 0x0004u
#define DS_IRQ_TM0    0x0008u
#define DS_IRQ_TM1    0x0010u
#define DS_IRQ_TM2    0x0020u
#define DS_IRQ_TM3    0x0040u
#define DS_IRQ_IPC    0x00010000u

/* DISPCNT bits used */
#define DS_DISP_MODE      0x0007u
#define DS_DISP_PAGE      0x0010u
#define DS_DISP_BG2_ENABLE 0x0400u

struct ds_timer {
    uint16_t reload;
    uint16_t cnt;
    uint32_t counter;  /* current 16-bit counter value, expanded */
    uint64_t acc;      /* sub-prescale cycle accumulator */
    int irq_pending;
};

struct ds_cpu {
    arm_t cpu;
    uint8_t io[0x1000];      /* own 0x04000000 page */
    struct ds_timer tm[4];
    uint32_t ie, if_latch, ime;
    uint64_t cycle_accum;    /* sub-frame remainder */
};

struct ds {
    emu_core_t base;

    struct ds_cpu a9, a7;
    arm_bus_t bus9, bus7; /* per-instance copies (user = this struct) */

    uint8_t main_ram[DS_MAIN_RAM_SIZE];
    uint8_t itcm[DS_ITCM_SIZE];
    uint8_t dtcm[DS_DTCM_SIZE];
    uint8_t wram_sh[DS_WRAM_SH_SIZE];
    uint8_t wram7[DS_WRAM7_SIZE];
    uint8_t vram_a[DS_VRAM_A_SIZE];
    uint8_t vram_b[DS_VRAM_B_SIZE];
    uint8_t vram_c[DS_VRAM_C_SIZE];
    uint8_t vram_d[DS_VRAM_D_SIZE];
    uint8_t vram_e[DS_VRAM_E_SIZE];
    uint8_t vram_f[DS_VRAM_F_SIZE];
    uint8_t vram_g[DS_VRAM_G_SIZE];

    uint8_t *rom;
    size_t rom_size;

    /* CP15 (ARM9): c9 TCM region registers + c1 control (stored) */
    uint32_t cp15_c1;
    uint32_t cp15_dtcm; /* (base & ~0x1FFF) | (size encoded) */
    uint32_t cp15_itcm;

    /* IPC sync cross-latch (engine for tests) */
    uint32_t ipc9, ipc7;

    uint32_t buttons;
    uint32_t vcount; /* current scanline */
    uint32_t frame_count;
    uint32_t fb[DS_SCREEN_W * DS_FB_H];
};

/* Test/inspection hooks (not part of the public API). */
uint32_t ds9_read32(struct ds *d, uint32_t addr);
void ds9_write32(struct ds *d, uint32_t addr, uint32_t v);
uint32_t ds7_read32(struct ds *d, uint32_t addr);
void ds7_write32(struct ds *d, uint32_t addr, uint32_t v);
void ds9_step(struct ds *d); /* one ARM9 instruction */
void ds7_step(struct ds *d); /* one ARM7 instruction */
void ds_render(struct ds *d);
void ds_tick_timers(struct ds_cpu *c, struct ds *d, uint32_t cycles,
                    uint64_t *irq_out);

#endif /* EMU_MDS_A_DS_H */
