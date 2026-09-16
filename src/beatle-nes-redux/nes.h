/*
 * beatle-nes-redux — NES core, internal header.
 *
 * Layers: cpu (6502/2A03), bus, cart (iNES + mappers 0/1/2/3/4), ppu, apu,
 * controller input, and the nes_t top-level state.
 *
 * Timing model: the CPU steps one instruction; the PPU then advances
 * 3 dots per CPU cycle used. Key PPU events (VBlank flag, NMI edge, sprite
 * evaluation, MMC3 A12 edges from BG fetch pattern) are modeled at line/dot
 * checkpoint granularity — documented approximations are noted per file.
 */
#ifndef EMU_NES_NES_H
#define EMU_NES_NES_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"

#define NES_SCREEN_W 256u
#define NES_SCREEN_H 240u
#define NES_CPU_HZ 1789773u
#define NES_MASTER_HZ 5369318u
#define NES_OUT_RATE 48000u

struct nes;
typedef struct nes nes_t;

/* ---- cartridge (cart.c) ---------------------------------------------------- */

typedef struct {
    uint8_t *prg;            /* PRG ROM, 16384 * banks */
    size_t prg_size;
    uint32_t prg_banks;      /* 16 KiB units */
    uint8_t *chr;            /* CHR ROM or CHR RAM */
    size_t chr_size;         /* 0 -> 8 KiB CHR RAM allocated */
    uint8_t chr_ram;
    uint8_t *prg_ram;        /* 8 KiB at 6000-7FFF */
    uint8_t mapper;
    uint8_t mirroring;       /* 0 = horizontal, 1 = vertical (header) */

    /* MMC1 (mapper 1) */
    uint8_t mmc1_shift;
    uint8_t mmc1_shift_count;
    uint8_t mmc1_regs[4];

    /* UNROM (2) / CNROM (3) */
    uint8_t prg_bank;
    uint8_t chr_bank;

    /* MMC3 (4) */
    uint8_t mmc3_reg_select;
    uint8_t mmc3_regs[8];    /* R0..R7 */
    uint8_t mmc3_irq_latch;
    uint8_t mmc3_irq_counter;
    uint8_t mmc3_irq_reload_pending;
    uint8_t mmc3_irq_enable;
    uint8_t mmc3_irq_asserted;
    uint8_t mmc3_a12_prev;
} nes_cart;

emu_result_t nes_cart_load(nes_cart *c, const uint8_t *data, size_t size);
void nes_cart_init(nes_cart *c);
void nes_cart_free(nes_cart *c);
void nes_cart_reset(nes_cart *c);
uint8_t nes_cart_cpu_read(nes_cart *c, uint16_t addr);
void nes_cart_cpu_write(nes_cart *c, uint16_t addr, uint8_t v);
/* CHR access through the mapper (drives MMC3 A12 edge detection) */
uint8_t nes_cart_chr_read(nes_cart *c, uint16_t addr);
void nes_cart_chr_write(nes_cart *c, uint16_t addr, uint8_t v);

/* ---- PPU (ppu.c) ------------------------------------------------------------ */

typedef struct {
    uint8_t vram[0x1000];    /* 2 KiB nametable RAM (mirroring via cart) */
    uint8_t palette[32];
    uint8_t oam[256];
    /* registers */
    uint8_t ctrl, mask, status;
    uint8_t oam_addr;
    uint8_t open_bus;
    /* loopy registers */
    uint16_t v, t;
    uint8_t fine_x;
    uint8_t w;
    uint8_t read_buffer;
    /* timing */
    uint16_t scanline;       /* 0..261 (261 = pre-render) */
    uint16_t dot;            /* 0..340 */
    uint8_t odd_frame;
    uint8_t vblank_event;    /* set at (241,1) each frame; frame boundary */
    uint8_t nmi_line;        /* level: vblank flag & ctrl.7 */
    uint8_t nmi_out_prev;
    /* sprite state for the line being rendered (evaluated at dot 257) */
    struct {
        uint8_t count;
        uint8_t spr_x[8];
        uint8_t attr[8];
        uint8_t pat_lo[8];   /* fetched pattern bytes */
        uint8_t pat_hi[8];
    } spr;
    uint8_t spr0_slot;       /* slot of sprite 0 this line, 0xFF = none */
    /* rasterized line layers: palette RAM indices */
    uint8_t line_bg[256];    /* 0 = transparent, else palette index */
    uint8_t line_spr[256];   /* bit7 present, bit6 behind-bg, low 5 = index */
    uint8_t line_spr0[256];  /* bit7 = sprite-0 pixel */
    uint8_t nmi_enabled_prev;
    uint32_t *fb;
} nes_ppu;

void nes_ppu_init(nes_ppu *p, uint32_t *fb);
void nes_ppu_reset(nes_ppu *p);
uint8_t nes_ppu_read(nes_t *n, uint16_t addr);
void nes_ppu_write(nes_t *n, uint16_t addr, uint8_t v);
/* advance the PPU by cpu_cycles*3 dots, servicing line events */
void nes_ppu_run(nes_t *n, uint32_t cpu_cycles);

/* ---- APU (apu.c) -------------------------------------------------------------- */

typedef struct {
    /* frame sequencer */
    uint32_t frame_cycles;   /* CPU cycles since sequencer step */
    uint32_t frame_step;     /* 0..3 (4-step) or 0..4 (5-step) */
    uint8_t mode5;
    uint8_t irq_inhibit;
    uint8_t frame_irq;

    struct {
        uint8_t enabled, duty, duty_pos;
        uint16_t timer, period; /* period = (t+1)*2 CPU cycles */
        uint8_t len_counter, len_enable;
        uint8_t env_volume, env_period, env_timer, env_direction, volume;
        uint8_t constant_volume;
        uint8_t sweep_enable, sweep_period, sweep_timer, sweep_negate,
               sweep_shift, sweep_reload;
        uint16_t freq;
        uint8_t dac;         /* gates output */
    } pulse[2];

    struct {
        uint8_t enabled;
        uint16_t timer, period;
        uint8_t seq_pos;
        uint8_t len_counter, len_enable;
        uint8_t linear_counter, linear_reload, linear_ctrl, linear_reload_flag;
    } tri;

    struct {
        uint8_t enabled;
        uint16_t timer, period;
        uint16_t lfsr;
        uint8_t mode;
        uint8_t len_counter, len_enable;
        uint8_t env_volume, env_period, env_timer, env_direction, volume;
        uint8_t constant_volume;
        uint8_t dac;
    } noise;

    struct {
        uint8_t enabled;
        uint8_t irq_enable, loop;
        uint16_t rate, timer;
        uint8_t output_level;
        uint16_t sample_addr, sample_len;
        uint16_t bytes_remaining;
        uint16_t addr_counter;
        uint8_t sample_buffer, buffer_empty;
        uint8_t shift, bits_remaining;
        uint8_t silence;
        uint8_t irq_flag;
    } dmc;

    /* output */
    uint32_t sample_acc;     /* 16.16 fixed point step per CPU cycle */
    uint32_t sample_step;
    int16_t *out;
    size_t out_cap, out_pos;
} nes_apu;

void nes_apu_init(nes_apu *a, int16_t *out, size_t cap);
void nes_apu_reset(nes_apu *a);
void nes_apu_run(nes_t *n, uint32_t cpu_cycles);
uint8_t nes_apu_read_status(nes_apu *a);
void nes_apu_write(nes_apu *a, uint16_t addr, uint8_t v);

/* ---- controller ------------------------------------------------------------------ */

enum {
    NES_BTN_A = 1u << 0,
    NES_BTN_B = 1u << 1,
    NES_BTN_SELECT = 1u << 2,
    NES_BTN_START = 1u << 3,
    NES_BTN_UP = 1u << 4,
    NES_BTN_DOWN = 1u << 5,
    NES_BTN_LEFT = 1u << 6,
    NES_BTN_RIGHT = 1u << 7
};

typedef struct {
    uint32_t buttons;
    uint8_t shift;
    uint8_t strobe;
} nes_controller;

/* ---- bus (bus.c) ---------------------------------------------------------------- */

typedef struct {
    uint8_t ram[0x800];
} nes_bus;

uint8_t nes_bus_read(nes_t *n, uint16_t addr);
void nes_bus_write(nes_t *n, uint16_t addr, uint8_t v);

/* ---- CPU (cpu.c) ------------------------------------------------------------------ */

/* P flag bits */
#define NES_FC 0x01u
#define NES_FZ 0x02u
#define NES_FI 0x04u
#define NES_FD 0x08u
#define NES_FB 0x10u
#define NES_FU 0x20u
#define NES_FV 0x40u
#define NES_FN 0x80u

typedef struct {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint8_t nmi_pending;
    uint8_t irq_line;
    uint32_t dma_stall;      /* OAM DMA cycles the CPU is suspended */
} nes_cpu;

uint32_t nes_cpu_step(nes_t *n); /* returns CPU cycles consumed */

/* Initializes documented 2A03 power-on state; reset vector is fetched by
 * the caller through the bus after power/reset. */
void nes_cpu_power_on(nes_cpu *c);
void nes_cpu_reset_regs(nes_cpu *c);

/* ---- top level (nes.c) ------------------------------------------------------------- */

struct nes {
    emu_core_t base;
    nes_cpu cpu;
    nes_bus bus;
    nes_cart cart;
    nes_ppu ppu;
    nes_apu apu;
    nes_controller ctrl;
    uint32_t fb[NES_SCREEN_W * NES_SCREEN_H];
    int16_t audio[4096];
    emu_audio_cb_t audio_cb;
    void *audio_user;
    uint64_t total_cycles;
};

/* CPU helpers shared with bus/apu live in cpu.c; nothing else exported. */

#endif /* EMU_NES_NES_H */
