/*
 * supersaturn: Sega Saturn core, internal header.
 *
 * Status: partial. Scope and sources:
 *  - Memory map: SCU User's Manual (ST-097-R5) figures 1.3/1.5 (cache and
 *    cache-through maps; this core maps both identically because the SH-2
 *    core masks addresses to 27 bits - documented simplification).
 *  - SCU registers, DMA (direct + indirect mode), timers, interrupt
 *    controller: SCU User's Manual figures 1.6-1.9, 3.1-3.5 and the
 *    interrupt status bit table (chapter 3); DMA field decode cross-checked
 *    against the Yabause implementation (GPL) for the field positions the
 *    manual figures do not make unambiguous.
 *  - VDP1: VDP1 User's Manual (ST-013-R3) - address map (figure 2.1),
 *    system registers (table 2.1), command tables (chapter 6) and the
 *    erase/write, plot trigger and frame buffer change registers
 *    (sections 4.1-4.4).
 *  - SMPC: SMPC User's Manual (ST-169-R1) figure 1.3 register map; INTBACK
 *    peripheral reporting.
 *  - IP.BIN/System ID header and boot sequence: Disc Format Standards
 *    Specification Sheet (ST-040-R4) figures 4.1, sections 4.3/4.6/4.7.
 *
 * Not implemented (honest gaps, run_frame still executes the machine):
 *  - VDP2 rendering: VRAM/CRAM/registers are functional RAM and storage,
 *    but no scroll screens are generated. The VDP1 frame buffer is
 *    displayed directly. VDP2 compositing is future work.
 *  - SCSP sound: the sound region is plain RAM; no audio is produced.
 *  - CD block: the A-bus CS1 window (0x05800000) reads as open bus.
 *  - SH-2 cache, DMA device arbitration and exact bus cycle timing.
 *
 * Boot model (documented): no IPL/BIOS ROM is bundled. load_rom() treats
 * the image as a raw IP.BIN (or the first sectors of a raw disc dump,
 * 2048-byte sectors), copies the application initial program (AIP) to
 * Work RAM-H 0x06002000 and starts the master SH-2 there, honoring the
 * System ID stack fields (0xE8/0xEC). The slave SH-2 is held in reset
 * until software writes to SINIT (0x01800004).
 *
 * Timing model: per-line stepping. SH-2 clock 28.6364 MHz, 262 lines,
 * ~477273 cycles per frame -> 1821 cycles per line per CPU.
 */
#ifndef EMU_SUPERSATURN_SATURN_H
#define EMU_SUPERSATURN_SATURN_H

#include <stddef.h>
#include <stdint.h>

#include "common/sh2.h"
#include "emu/emu.h"

#define SAT_SCREEN_W 320u
#define SAT_SCREEN_H 224u
#define SAT_OUT_RATE 44100u

#define SAT_FB_W 512u /* physical VDP1 frame buffer (2 Mbit, 16 bpp)      */
#define SAT_FB_H 256u

#define SAT_LINES 262u
#define SAT_CYCLES_PER_FRAME (28636436u / 60u)
#define SAT_CYCLES_PER_LINE (SAT_CYCLES_PER_FRAME / SAT_LINES) /* 1821   */

/* SCU interrupt status bits (SCU User's Manual, chapter 3 table). */
#define SAT_IST_VBLANKIN 0x00000001u
#define SAT_IST_VBLANKOUT 0x00000002u
#define SAT_IST_HBLANKIN 0x00000004u
#define SAT_IST_TIMER0 0x00000008u
#define SAT_IST_TIMER1 0x00000010u
#define SAT_IST_DRAWEND 0x00002000u

/* SCU register block offsets (bytes from 0x05FE0000), figure 1.6. */
#define SAT_SCU_D0R 0x00u
#define SAT_SCU_D0W 0x04u
#define SAT_SCU_D0C 0x08u
#define SAT_SCU_D0AD 0x0Cu
#define SAT_SCU_D0EN 0x10u
#define SAT_SCU_D0MD 0x14u /* per level: +0x20 each                     */
#define SAT_SCU_DSTA 0x80u
#define SAT_SCU_T0C 0x90u
#define SAT_SCU_T1S 0x94u
#define SAT_SCU_T1MD 0x98u
#define SAT_SCU_IMS 0xA0u
#define SAT_SCU_IST 0xA4u

/* VDP1 system register indices (word slots at 0x05D00000), table 2.1. */
#define SAT_VDP1_TVMR 0
#define SAT_VDP1_FBCR 1
#define SAT_VDP1_PTMR 2
#define SAT_VDP1_EWDR 3
#define SAT_VDP1_EWLR 4
#define SAT_VDP1_EWRR 5
#define SAT_VDP1_ENDR 6
#define SAT_VDP1_EDSR 8
#define SAT_VDP1_LOPR 9
#define SAT_VDP1_COPR 10
#define SAT_VDP1_MODR 11

/* TVMR bits (VDP1 User's Manual, section 4.1): TVM bits 2-0 (only
 * TVM=0, 16 bpp normal, is implemented), VBE bit 3. */
#define SAT_TVMR_VBE 0x0008u

/* Input bitmask layout (documented contract for set_input): */
#define SAT_BTN_UP 0x00000010u
#define SAT_BTN_DOWN 0x00000020u
#define SAT_BTN_LEFT 0x00000040u
#define SAT_BTN_RIGHT 0x00000080u
#define SAT_BTN_START 0x00000008u
#define SAT_BTN_A 0x00000100u
#define SAT_BTN_B 0x00000200u
#define SAT_BTN_C 0x00000400u
#define SAT_BTN_X 0x00000800u
#define SAT_BTN_Y 0x00001000u
#define SAT_BTN_Z 0x00002000u
#define SAT_BTN_L 0x00004000u
#define SAT_BTN_R 0x00008000u

/* FBCR bits (section 4.2). */
#define SAT_FBCR_FCT 0x0001u
#define SAT_FBCR_FCM 0x0002u

struct saturn {
    emu_core_t base;

    /* CPUs */
    sh2_t msh2;
    sh2_t ssh2;
    sh2_bus_t mbus; /* per-CPU bus instances sharing callbacks          */
    sh2_bus_t sbus;
    int slave_on;

    /* memory */
    uint8_t wraml[1024 * 1024]; /* 0x00200000                           */
    uint8_t wramh[1024 * 1024]; /* 0x06000000                           */
    uint8_t backup[64 * 1024];  /* 0x00180000                           */
    uint8_t soundram[512 * 1024]; /* 0x05A00000                         */

    /* disc image (kept for boot + future CD block work) */
    uint8_t *disc;
    size_t disc_size;

    /* VDP1 */
    uint8_t vdp1_vram[512 * 1024];      /* 0x05C00000                   */
    uint16_t vdp1_fb[2][SAT_FB_W * SAT_FB_H]; /* 0x05C80000/0x05CA0000  */
    uint16_t vdp1_reg[16];              /* TVHR..MODR word slots        */
    uint8_t vdp1_draw_sel;              /* buffer index being drawn to  */
    uint16_t user_clip[4];              /* x1,y1,x2,y2                  */
    uint16_t sys_clip[2];               /* x2,y2 (x1,y1 = 0)            */
    int32_t local_x, local_y;
    uint8_t vdp1_ptmr_run;              /* auto-run armed (PTM=10B)     */

    /* VDP2: storage only (documented) */
    uint8_t vdp2_vram[512 * 1024]; /* 0x05E00000                        */
    uint8_t vdp2_cram[4 * 1024];   /* 0x05F00000                        */
    uint8_t vdp2_reg[0x120];       /* 0x05F80000, 288 bytes             */

    /* SCU */
    uint32_t scu[0xD0u / 4u]; /* register file, 208 bytes               */
    /* per-level DMA state derived from the register file at start      */
    struct saturn_dma_state {
        uint32_t raddr, waddr, count; /* latched working set             */
        int active;                   /* transfer in progress           */
        int indirect;                 /* indirect mode working set      */
        int last;                     /* current entry has end flag     */
        uint32_t itable;              /* indirect table cursor          */
    } dma[3];
    uint16_t timer0, timer1; /* live counters                            */

    /* SMPC */
    uint8_t smpc_ireg[7];
    uint8_t smpc_oreg[32];
    uint8_t smpc_sf; /* status flag: 1 = command in progress             */

    /* boot state derived from the System ID header */
    uint32_t boot_sp_m, boot_sp_s;

    /* video output */
    uint32_t fb[SAT_SCREEN_W * SAT_SCREEN_H];
    uint32_t buttons;
    uint32_t frame_count;
};

/* Internal helpers shared between saturn.c and vdp1.c. */
uint16_t sat_vdp1_vram16(const struct saturn *s, uint32_t byte_addr);
void sat_vdp1_set_vram16(struct saturn *s, uint32_t byte_addr, uint16_t v);
uint16_t sat_vdp1_reg_read(const struct saturn *s, uint32_t word_index);
void sat_vdp1_reg_write(struct saturn *s, uint32_t word_index, uint16_t v);
void sat_vdp1_execute(struct saturn *s);
void sat_vdp1_frame_change(struct saturn *s);
void sat_render_output(struct saturn *s);

/* SCU: raise an interrupt factor (sets IST, delivers to both CPUs). */
void sat_scu_send(struct saturn *s, uint32_t ist_bit, uint32_t vector,
                  uint32_t level);
/* SCU register write dispatch (called after the merge, with the old
 * 32-bit value for edge detection). */
void scu_reg_write(struct saturn *s, uint32_t off, uint32_t old);
/* SMPC command dispatch (COMREG). */
void smpc_command(struct saturn *s, uint8_t cmd);
void smpc_pad_data(struct saturn *s);

/* Bus primitives (used by the SH-2 cores, DMA and tests). */
uint8_t sat_bus_read8(struct saturn *s, uint32_t addr);
uint16_t sat_bus_read16(struct saturn *s, uint32_t addr);
uint32_t sat_bus_read32(struct saturn *s, uint32_t addr);
void sat_bus_write8(struct saturn *s, uint32_t addr, uint8_t v);
void sat_bus_write16(struct saturn *s, uint32_t addr, uint16_t v);
void sat_bus_write32(struct saturn *s, uint32_t addr, uint32_t v);

/* Run one scanline of both CPUs (exported for tests). */
void sat_line_tick(struct saturn *s);

#endif /* EMU_SUPERSATURN_SATURN_H */
