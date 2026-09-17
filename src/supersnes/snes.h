/*
 * supersnes — SNES core, internal header.
 *
 * Layers: cpu (65C816), mem (memory map), cart (LoROM/HiROM), ppu
 * (BG modes 0/1/7 + sprites), dma (general purpose DMA), apu (stub),
 * and the snes_t top-level state.
 *
 * Timing model: the CPU steps one instruction and reports CPU cycles
 * (1 cycle = 6 master clocks; FastROM speed is not modeled). PPU advances
 * one dot per 4 master clocks with a fractional accumulator (deterministic).
 * DMA steals CPU cycles per byte.
 *
 * Documented limitations (this milestone): S-SMP/DSP audio is a stub
 * (ports answer with fixed handshake values, no audio output), HDMA is not
 * implemented, PPU modes 2-6 and color math / windowing / mosaic are not
 * implemented, PPU cycle timing is line-granular for events.
 */
#ifndef EMU_SUPERSNES_SNES_H
#define EMU_SUPERSNES_SNES_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"

#define SNES_SCREEN_W 256u
#define SNES_SCREEN_H 224u
#define SNES_MASTER_HZ 21477272u
#define SNES_OUT_RATE 32000u

struct snes;
typedef struct snes snes_t;

/* ---- cartridge (cart.c) --------------------------------------------------- */

typedef struct {
    uint8_t *rom;
    size_t rom_size;
    uint8_t *sram;
    size_t sram_size;
    uint8_t lorom;           /* 1 = LoROM, 0 = HiROM */
    uint8_t fastrom;
    char title[22];
} snes_cart;

emu_result_t snes_cart_load(snes_cart *c, const uint8_t *data, size_t size);
void snes_cart_init(snes_cart *c);
void snes_cart_free(snes_cart *c);
/* 24-bit SNES address (bank<<16 | offset) mapped through the cartridge */
uint8_t snes_cart_read(snes_cart *c, uint32_t addr);
void snes_cart_write(snes_cart *c, uint32_t addr, uint8_t v);

/* ---- PPU (ppu.c) ------------------------------------------------------------ */

typedef struct {
    uint8_t vram[0x8000];    /* 32 KiB */
    uint8_t cgram[512];
    uint8_t oam[544];        /* 512 + 32 priority table bytes */
    /* registers (subset implemented; see ppu.c) */
    uint8_t inidisp;         /* 2100 */
    uint8_t bgmode;          /* 2105 */
    uint16_t bg_scroll[8];   /* BG1H, BG1V, BG2H, BG2V, BG3H, BG3V, BG4H, BG4V */
    uint8_t scroll_latch;    /* lo/hi toggle for scroll + mode7 registers */
    uint8_t bg_map[4];       /* 210B-210C lo/hi pairs */
    uint8_t bg_char_base[5]; /* BG1-4 char bases + OBJ char base */
    uint8_t obj_size_reg;    /* 2101: bits 0-2 OBJ name base, 5-6 size table */
    uint8_t obj_8bpp;        /* SETINI bit 0: 8bpp OBJ mode */
    uint16_t oamaddr;        /* 2102/2103 */
    uint8_t vmainc;          /* 2115 */
    uint16_t vmadd;          /* 2116/2117 */
    uint8_t vm_lo_latch;     /* pending low byte of the VRAM word */
    uint8_t vm_hi_latch;     /* pending high byte of the VRAM word */
    uint8_t cgaddr;          /* 2121 */
    uint8_t cgaddr_flip;
    uint8_t tm, ts;          /* 212C/212D (sub screen unused) */
    /* mode 7 */
    uint8_t m7sel;           /* 211A */
    uint16_t m7a, m7b, m7c, m7d; /* 211B-211E */
    uint16_t m7x, m7y;       /* 211F/2120 */
    uint8_t oam_latch, oam_addr2;
    /* reads */
    uint8_t vmadd_read_latch;
    uint8_t cg_read_latch;
    uint8_t openbus;
    /* timing */
    uint16_t line;           /* 0..261 */
    uint16_t dot;            /* 0..1363 */
    uint32_t dot_acc;        /* fractional dot accumulator (master/4) */
    uint8_t nmi_line, nmi_prev;
    uint32_t *fb;
} snes_ppu;

void snes_ppu_init(snes_ppu *p, uint32_t *fb);
void snes_ppu_reset(snes_ppu *p);
uint8_t snes_ppu_read(snes_t *s, uint16_t addr);
void snes_ppu_write(snes_t *s, uint16_t addr, uint8_t v);
void snes_ppu_run(snes_t *s, uint32_t master_cycles);

/* ---- APU stub (apu.c) ---------------------------------------------------------- */

typedef struct {
    uint8_t ports[8];        /* 2140-2147 mirror region (stub) */
} snes_apu;

void snes_apu_reset(snes_apu *a);
uint8_t snes_apu_read(snes_apu *a, uint16_t addr);
void snes_apu_write(snes_apu *a, uint16_t addr, uint8_t v);

/* ---- DMA (dma.c) ------------------------------------------------------------------ */

typedef struct {
    uint8_t params;          /* 43x0: bit7 dir, bit6 indirect, bit4 fixed,
                              * bit3 decrement, bits0-2 transfer mode */
    uint8_t bbus;            /* 43x1 */
    uint16_t abus;           /* 43x2/3 (A2: table pointer for HDMA) */
    uint8_t abank;           /* 43x4 */
    uint16_t count;          /* 43x5/6 */
    uint8_t ibank;           /* 43x7 (indirect bank) */
    /* HDMA per-channel runtime state */
    uint8_t hdma_finished;   /* table exhausted for this frame */
    uint16_t hdma_lines_left;/* lines remaining in the current block */
    uint8_t hdma_repeat;     /* repeat flag of the current block */
    uint8_t hdma_xfer;       /* transfer on this line? */
    uint8_t hdma_ind_loaded; /* indirect address captured for this block */
    uint16_t hdma_ind_addr;
    uint8_t hdma_ind_bank;
    /* A2 backup: reloaded into abus/abank at V=0 (captured on writes) */
    uint16_t hdma_reload_abus;
    uint8_t hdma_reload_abank;
} snes_dma_channel;

typedef struct {
    snes_dma_channel ch[8];
    uint8_t mdmaen;          /* 420B */
    uint8_t hdmaen;          /* 420C */
} snes_dma;

void snes_dma_reset(snes_dma *d);
/* returns master cycles consumed by the transfer */
uint32_t snes_dma_run(snes_t *s);
/* HDMA: init_frame resets channel tables at V=0; hdma_line runs one
 * per-scanline pass (called during the HBlank that precedes a line) */
void snes_hdma_init_frame(snes_t *s);
void snes_hdma_line(snes_t *s);

/* ---- bus / memory map (mem.c) -------------------------------------------------------- */

typedef struct {
    uint8_t wram[0x20000];   /* 128 KiB */
    /* CPU registers 4200-421F */
    uint8_t nmitimen;        /* 4200 */
    uint8_t wrmpya, wrmpyb;
    uint16_t wrdiv;          /* 4204/5 */
    uint8_t wrdivb;          /* 4206 */
    uint16_t htime;          /* 4207/8 */
    uint16_t vtime;          /* 4209/A */
    uint8_t fastrom;         /* 420D */
    /* results */
    uint16_t rddiv, rdmpy;   /* 4216/7, 4214/5 */
    uint8_t timeup;          /* 4211 */
    uint8_t joypad_auto[4];  /* latched auto-read output (2 pads) */
    /* joypad serial read state */
    uint8_t joypad_shift[2];
    uint8_t joypad_latch;
} snes_mem;

uint8_t snes_bus_read(snes_t *s, uint32_t addr); /* 24-bit address */
void snes_bus_write(snes_t *s, uint32_t addr, uint8_t v);
void snes_mem_reset(snes_mem *m);
/* latches controller states into 4218-421B (called at VBlank start) */
void snes_mem_latch_joypads(snes_t *s);

/* ---- CPU (cpu.c) ------------------------------------------------------------------------ */

/* P flag bits */
#define SNES_FC 0x01u
#define SNES_FZ 0x02u
#define SNES_FI 0x04u
#define SNES_FD 0x08u
#define SNES_FX 0x10u /* index register width: 1 = 8-bit */
#define SNES_FM 0x20u /* accumulator/memory width: 1 = 8-bit */
#define SNES_FV 0x40u
#define SNES_FN 0x80u

typedef struct {
    uint16_t a;              /* C: a_lo = low byte */
    uint16_t x;
    uint16_t y;
    uint16_t sp;
    uint16_t pc;
    uint8_t dbr;
    uint8_t k;
    uint16_t dp;
    uint8_t p;
    uint8_t e;
    uint8_t nmi_pending;
    uint8_t irq_line;
    uint8_t wai;             /* waiting for interrupt */
    uint8_t stp;             /* stopped */
} snes_cpu;

void snes_cpu_power_on(snes_cpu *c);
void snes_cpu_reset_regs(snes_cpu *c);
uint32_t snes_cpu_step(snes_t *s); /* returns CPU cycles consumed */

/* ---- top level (snes.c) -------------------------------------------------------------------- */

struct snes {
    emu_core_t base;
    snes_cpu cpu;
    snes_mem mem;
    snes_cart cart;
    snes_ppu ppu;
    snes_dma dma;
    snes_apu apu;
    uint32_t buttons[2];
    uint32_t fb[SNES_SCREEN_W * SNES_SCREEN_H];
    int16_t audio[2048];
    emu_audio_cb_t audio_cb;
    void *audio_user;
    uint64_t total_cycles;
};

#endif /* EMU_SUPERSNES_SNES_H */
