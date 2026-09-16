/*
 * mgbx — Game Boy (DMG) core, internal header.
 *
 * Layers: cpu (SM83/LR35902), mem (bus), cart (mappers), ppu, timer, joypad,
 * apu, and the mgbx_t top-level state that owns them all.
 *
 * Timing model: the CPU steps one instruction (or one M-cycle while halted/
 * stalled) and reports consumed T-cycles (4 per M-cycle). PPU advances one dot
 * per T-cycle, the timer ticks on M-cycle boundaries, the APU is clocked from
 * T-cycles. Interrupt dispatch happens at instruction boundaries.
 */
#ifndef EMU_MGBX_MGBX_H
#define EMU_MGBX_MGBX_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"

#define GB_SCREEN_W 160u
#define GB_SCREEN_H 144u
#define GB_CYCLES_PER_FRAME 70224u /* T-cycles */

/* cartridge type codes (header 0x147) */
enum {
    GB_CART_ROM_ONLY = 0x00,
    GB_CART_MBC1 = 0x01,
    GB_CART_MBC1_RAM = 0x02,
    GB_CART_MBC1_RAM_BAT = 0x03,
    GB_CART_MBC2 = 0x05,
    GB_CART_MBC2_BAT = 0x06,
    GB_CART_MBC3_TIMER = 0x0F,
    GB_CART_MBC3_TIMER_RAM = 0x10,
    GB_CART_MBC3 = 0x11,
    GB_CART_MBC3_RAM = 0x12,
    GB_CART_MBC3_RAM_BAT = 0x13,
    GB_CART_MBC5 = 0x19,
    GB_CART_MBC5_RAM = 0x1A,
    GB_CART_MBC5_RAM_BAT = 0x1B,
    GB_CART_MBC5_RUMBLE = 0x1C,
    GB_CART_MBC5_RUMBLE_RAM = 0x1D,
    GB_CART_MBC5_RUMBLE_RAM_BAT = 0x1E
};

struct mgbx;

/* ---- cartridge (src/mgbx/cart.c) ------------------------------------------ */

typedef struct {
    uint8_t *rom;
    size_t rom_size;
    uint32_t rom_banks;      /* 16 KiB banks */
    uint8_t *ram;            /* cart RAM (owned) */
    size_t ram_size;
    uint8_t type;            /* header cart type */

    /* MBC1 / MBC3 / MBC5 register state */
    uint8_t ram_enabled;
    uint8_t bank1;           /* 2000-3FFF */
    uint8_t bank2;           /* 4000-5FFF (MBC1) / ram+rtc select (MBC3) */
    uint8_t mode;            /* MBC1 banking mode */
    uint16_t rom_bank;       /* MBC5 9-bit bank */

    /* MBC3 RTC (deterministic: driven by emulation time, not wall clock) */
    uint8_t rtc_halt;
    uint8_t rtc_latch_state; /* latch protocol: write 0x00 then 0x01 */
    uint8_t rtc[5];          /* sec min hr dl dh */
    uint8_t rtc_latched[5];
    uint64_t rtc_divider;    /* T-cycles accumulated; 1 s = 4194304 */

    /* MBC2 512 x 4-bit RAM */
    uint8_t mbc2_ram[512];
} gb_cart;

void gb_cart_init(gb_cart *cart);
void gb_cart_free(gb_cart *cart);
/* Parses header, allocates ROM copy + RAM. Returns EMU_* result. */
emu_result_t gb_cart_load(gb_cart *cart, const uint8_t *data, size_t size);
void gb_cart_reset(gb_cart *cart);
uint8_t gb_cart_read(gb_cart *cart, uint16_t addr);
void gb_cart_write(gb_cart *cart, uint16_t addr, uint8_t v);
void gb_cart_tick_rtc(gb_cart *cart, uint32_t t_cycles);

/* ---- PPU (src/mgbx/ppu.c) -------------------------------------------------- */

typedef struct {
    uint8_t vram[0x2000];
    uint8_t oam[0xA0];
    uint8_t lcdc, stat, scy, scx, ly, lyc, bgp, obp0, obp1, wy, wx;
    uint16_t dot;        /* 0..455 within line */
    uint8_t win_line;    /* internal window line counter */
    uint32_t *fb;        /* XRGB8888, GB_SCREEN_W * GB_SCREEN_H */
    uint8_t line_pixels[GB_SCREEN_W]; /* shade indices 0..3 for current line */
    uint8_t sprite_drawn[GB_SCREEN_W];
} gb_ppu;

void gb_ppu_init(gb_ppu *ppu, uint32_t *fb);
void gb_ppu_reset(gb_ppu *ppu);
void gb_ppu_step(struct mgbx *gb, uint32_t t_cycles);
uint8_t gb_ppu_vram_read(gb_ppu *ppu, uint16_t addr);
void gb_ppu_vram_write(gb_ppu *ppu, uint16_t addr, uint8_t v);
uint8_t gb_ppu_oam_read(gb_ppu *ppu, uint16_t addr);
void gb_ppu_oam_write(gb_ppu *ppu, uint16_t addr, uint8_t v);
uint8_t gb_ppu_read_reg(gb_ppu *ppu, uint16_t addr);
void gb_ppu_write_reg(struct mgbx *gb, uint16_t addr, uint8_t v);

/* ---- timer (src/mgbx/timer.c) ---------------------------------------------- */

typedef struct {
    uint16_t counter;    /* internal 16-bit; DIV = counter >> 6 */
    uint8_t tima;
    uint8_t tma;
    uint8_t tac;         /* only bits 2..0 writable/readable */
    uint8_t tima_reload; /* pending reload after overflow (4 T-cycles) */
} gb_timer;

void gb_timer_reset(gb_timer *tm);
void gb_timer_step(struct mgbx *gb, uint32_t t_cycles);
uint8_t gb_timer_read(gb_timer *tm, uint16_t addr);
void gb_timer_write(struct mgbx *gb, uint16_t addr, uint8_t v);

/* ---- joypad (src/mgbx/joypad.c) --------------------------------------------- */

typedef struct {
    uint8_t select;      /* P1 bits 4/5 (0 = selected) */
    uint8_t p1;          /* last composed register value */
    uint32_t buttons;    /* public bitmask, see emu.h */
} gb_joypad;

enum {
    GB_BTN_A = 1u << 0,
    GB_BTN_B = 1u << 1,
    GB_BTN_SELECT = 1u << 2,
    GB_BTN_START = 1u << 3,
    GB_BTN_RIGHT = 1u << 4,
    GB_BTN_LEFT = 1u << 5,
    GB_BTN_UP = 1u << 6,
    GB_BTN_DOWN = 1u << 7
};

uint8_t gb_joypad_read(gb_joypad *jp);
void gb_joypad_write(gb_joypad *jp, uint8_t v, struct mgbx *gb);
void gb_joypad_set_buttons(gb_joypad *jp, uint32_t buttons, struct mgbx *gb);

/* ---- APU (src/mgbx/apu.c) ---------------------------------------------------- */

typedef struct {
    uint8_t enabled;      /* NR52 bit 7 */
    uint8_t nr50, nr51;
    struct {
        uint8_t duty;        /* NRx1 bits 6-7 */
        uint8_t duty_pos;
        uint32_t freq_timer; /* T-cycles until next duty step */
        uint16_t freq;       /* 11-bit */
        uint8_t len_counter;
        uint8_t len_enable;
        uint8_t env_volume, env_period, env_timer, env_direction;
        uint8_t volume;      /* current envelope output 0..15 */
        uint8_t dac_enable;  /* NRx2 bits 7-5 != 0 */
        uint8_t sweep_period, sweep_timer, sweep_shift, sweep_negate,
               sweep_enabled;
        uint8_t active;
    } sq[2];
    struct {
        uint8_t dac_enable;  /* NR30 bit 7 */
        uint8_t volume_shift;/* NR32 bits 5-6 */
        uint32_t freq_timer;
        uint16_t freq;
        uint16_t len_counter; /* 0..256 for wave */
        uint8_t len_enable;
        uint8_t pos;         /* 0..31 */
        uint8_t sample;      /* last 4-bit sample */
        uint8_t active;
    } wave;
    struct {
        uint16_t lfsr;
        uint8_t width_mode;  /* NR43 bit 3 */
        uint8_t divisor_code, shift_clock;
        uint32_t freq_timer;
        uint8_t len_counter, len_enable;
        uint8_t env_volume, env_period, env_timer, env_direction, volume;
        uint8_t dac_enable;
        uint8_t active;
    } noise;

    uint8_t wave_ram[16];

    uint16_t frame_seq_timer; /* T-cycles; 8192 per step */
    uint8_t frame_seq_step;   /* 0..7 */

    uint32_t sample_acc;      /* T-cycles toward next output sample */
    int16_t *out;             /* frame audio buffer (owned by mgbx_t) */
    size_t out_cap, out_pos;
} gb_apu;

void gb_apu_init(gb_apu *apu, int16_t *out, size_t cap);
void gb_apu_reset(gb_apu *apu);
void gb_apu_step(struct mgbx *gb, uint32_t t_cycles);
uint8_t gb_apu_read(gb_apu *apu, uint16_t addr);
void gb_apu_write(gb_apu *apu, uint16_t addr, uint8_t v);

/* ---- bus (src/mgbx/mem.c) ----------------------------------------------------- */

typedef struct {
    uint8_t wram[0x2000];
    uint8_t hram[0x7F];
    uint8_t ie;          /* FFFF */
    uint8_t if_reg;      /* FF0F, upper bits read as 1 */
    uint8_t sb, sc;      /* serial: stub, documented */
    uint8_t oam_dma_page;
    uint8_t dma_active;  /* 1 while the 160 M-cycle OAM DMA runs */
    uint8_t dma_value;   /* byte scheduled for copy on the current M-cycle */
    uint8_t dma_index;   /* bytes copied so far */
} gb_mem;

uint8_t gb_bus_read(struct mgbx *gb, uint16_t addr);
void gb_bus_write(struct mgbx *gb, uint16_t addr, uint8_t v);

/* ---- CPU (src/mgbx/cpu.c) ------------------------------------------------------- */

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    uint8_t ime;
    uint8_t ime_pending; /* EI takes effect after next instruction */
    uint8_t halted;
    uint8_t stopped;
    uint8_t halt_bug;
} gb_cpu;

/* Executes one instruction (or one M-cycle of DMA/halt); returns T-cycles. */
uint32_t gb_cpu_step(struct mgbx *gb);

/* Initializes documented DMG post-boot register state (no boot ROM). */
void gb_cpu_reset_for_no_bootrom(gb_cpu *c);

/* ---- top level (src/mgbx/mgbx.c) ------------------------------------------------- */

struct mgbx {
    emu_core_t base;
    gb_cpu cpu;
    gb_mem mem;
    gb_cart cart;
    gb_ppu ppu;
    gb_timer timer;
    gb_joypad joypad;
    gb_apu apu;
    uint32_t fb[GB_SCREEN_W * GB_SCREEN_H];
    int16_t audio[2048]; /* one frame of output at 32768 Hz + slack */
    emu_audio_cb_t audio_cb;
    void *audio_user;
    uint32_t buttons;
    uint64_t total_cycles;
};

/* interrupt request, bit 0..4 (VBlank, STAT, Timer, Serial, Joypad) */
void gb_request_interrupt(struct mgbx *gb, uint8_t bit);

#endif /* EMU_MGBX_MGBX_H */
