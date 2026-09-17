/*
 * mgbax — Game Boy Advance core, internal header.
 *
 * Layers: cpu (ARM7TDMI: ARMv4T ARM + Thumb), mem, cart, ppu, timer, dma,
 * apu (PSG stub for FIFO channels), swi (HLE BIOS services), gba (lifecycle).
 *
 * BIOS: no BIOS ROM is bundled. The core implements an HLE BIOS layer:
 *  - SWI instructions are serviced in C (see swi.c)
 *  - IRQ dispatch is performed by an emulated BIOS dispatcher that calls
 *    the handler pointer at $03007FFC and returns (see cpu.c)
 *
 * Timing model: instruction-level with flat per-access cycle costs
 * (1 cycle per 8/16-bit bus access to ROM/WRAM approximated; documented).
 */
#ifndef EMU_MGBAX_GBA_H
#define EMU_MGBAX_GBA_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"

#define GBA_SCREEN_W 240u
#define GBA_SCREEN_H 160u
#define GBA_OUT_RATE 32768u

struct gba;
typedef struct gba gba_t;

/* CPU modes (CPSR bits 0-4) */
enum {
    GBA_MODE_USR = 0x10,
    GBA_MODE_FIQ = 0x11,
    GBA_MODE_IRQ = 0x12,
    GBA_MODE_SVC = 0x13,
    GBA_MODE_ABT = 0x17,
    GBA_MODE_UND = 0x1B,
    GBA_MODE_SYS = 0x1F
};

/* CPSR bits */
#define GBA_N 0x80000000u
#define GBA_Z 0x40000000u
#define GBA_C 0x20000000u
#define GBA_V 0x10000000u
#define GBA_I 0x00000080u
#define GBA_F 0x00000040u
#define GBA_T 0x00000020u

/* ---- CPU (cpu.c) ----------------------------------------------------------- */

typedef struct {
    uint32_t r[16];
    uint32_t cpsr;
    /* banked: indices 0=usr/sys 1=fiq 2=irq 3=svc 4=abt 5=und */
    uint32_t bank_r13[6];
    uint32_t bank_r14[6];
    uint32_t bank_r8_fiq[5]; /* FIQ-private r8-r12 */
    uint32_t spsr[7]; /* 0..5 per-mode, 6 = current-mode view */
    uint8_t mode;
    uint8_t halted;          /* HALT until (IF & IE) */
    uint8_t intr_wait_active;
    uint32_t intr_wait_flags;
    uint8_t bios_dispatch;   /* currently inside the HLE IRQ dispatcher */
    uint8_t prev_cpsr_t;
} gba_cpu;

void gba_cpu_power_on(gba_cpu *c);
uint32_t gba_cpu_step(gba_t *g); /* returns CPU cycles consumed */
/* banked-register switching (used by PSR transfer + exceptions) */
void gba_cpu_set_mode(gba_cpu *c, uint8_t new_mode);
uint32_t gba_cpu_read_spsr(const gba_cpu *c);
void gba_cpu_write_spsr(gba_cpu *c, uint32_t v);
void gba_cpu_exception(gba_t *g, uint32_t vector, uint8_t new_mode,
                       uint32_t lr_offset, int copy_spsr);

/* ---- memory (mem.c) ----------------------------------------------------------- */

typedef struct {
    uint8_t ewram[0x40000];  /* 256 KiB */
    uint8_t iwram[0x8000];   /* 32 KiB */
    uint8_t pal[0x400];
    uint8_t vram[0x18000];
    uint8_t oam[0x400];
    uint8_t io[0x400];
    uint16_t ie, if_reg;     /* IRQ controller */
    uint16_t waitcnt;
    uint8_t imiu;
} gba_mem;

uint8_t gba_mem_read8(gba_t *g, uint32_t addr);
uint16_t gba_mem_read16(gba_t *g, uint32_t addr);
uint32_t gba_mem_read32(gba_t *g, uint32_t addr);
void gba_mem_write8(gba_t *g, uint32_t addr, uint8_t v);
void gba_mem_write16(gba_t *g, uint32_t addr, uint16_t v);
void gba_mem_write32(gba_t *g, uint32_t addr, uint32_t v);
/* bus access used by the CPU (applies open-bus/rotate behavior) */
uint32_t gba_bus_read32(gba_t *g, uint32_t addr);
uint16_t gba_bus_read16(gba_t *g, uint32_t addr);
void gba_request_irq(gba_t *g, uint16_t bits);
/* IO dispatch (region $04) */
uint16_t gba_io_read16(gba_t *g, uint32_t addr);
void gba_io_write16(gba_t *g, uint32_t addr, uint16_t v);
/* called once per frame from the core loop */
void gba_update_input_reg(gba_t *g);

/* ---- cartridge (cart.c) ------------------------------------------------------------ */

typedef struct {
    uint8_t *rom;
    size_t rom_size;
    uint8_t sram[0x10000];
    uint8_t sram_flash;      /* 0 = SRAM, 1 = flash-style 8-bit RAM (same model) */
} gba_cart;

emu_result_t gba_cart_load(gba_cart *c, const uint8_t *data, size_t size);
void gba_cart_init(gba_cart *c);
void gba_cart_free(gba_cart *c);
uint8_t gba_cart_read8(gba_cart *c, uint32_t addr);
void gba_cart_write8(gba_cart *c, uint32_t addr, uint8_t v);

/* ---- PPU (ppu.c) ---------------------------------------------------------------------- */

typedef struct {
    uint8_t dispcnt_latch;
    uint16_t dispstat, vcount;
    /* BG control registers (BG0CNT-BG3CNT packed) */
    uint16_t bgcnt[4];
    uint16_t bg_hofs[4], bg_vofs[4];
    /* affine params */
    int16_t bgpa, bgpb, bgpc, bgpd;
    int32_t bgx[2], bgy[2];
    uint16_t winh[2], winv[2], winin, winout;
    uint16_t bldcnt, bldalpha, bldy;
    uint32_t frame;          /* frame counter (mode 4 page flip base) */
    uint32_t *fb;
} gba_ppu;

void gba_ppu_init(gba_ppu *p, uint32_t *fb);
void gba_ppu_reset(gba_ppu *p);
void gba_ppu_render_line(gba_t *g, uint16_t line);
/* IO access from the CPU side */
uint16_t gba_ppu_io_read16(gba_t *g, uint32_t addr);
void gba_ppu_io_write16(gba_t *g, uint32_t addr, uint16_t v);

/* ---- timers (timer.c) ---------------------------------------------------------------- */

typedef struct {
    uint16_t reload[4];
    uint16_t counter[4];
    uint16_t ctrl[4];
    uint32_t prescaler[4];   /* sub-tick accumulators */
    uint32_t tick_div[4];    /* cycles per tick: 1,64,256,1024 */
    uint8_t ovf_bits;        /* overflow notifications (consumed by APU) */
} gba_timers;

void gba_timers_reset(gba_timers *t);
void gba_timers_step(gba_t *g, uint32_t cycles);
uint16_t gba_timers_read(gba_timers *t, uint32_t addr);
void gba_timers_write(gba_t *g, uint32_t addr, uint16_t v);

/* ---- DMA (dma.c) ------------------------------------------------------------------------- */

typedef struct {
    uint32_t sad[4], dad[4];
    uint32_t dad_latch[4];   /* DAD captured at enable (DST_RELOAD source) */
    uint16_t count_latch[4];
    uint16_t ctrl[4];
    uint16_t count[4];
    uint8_t enabled[4];
} gba_dma;

void gba_dma_reset(gba_dma *d);
/* services immediate and triggered transfers; returns cycles consumed */
uint32_t gba_dma_run(gba_t *g, uint16_t trigger_flags);
uint16_t gba_dma_read_ctrl(gba_dma *d, uint32_t addr);
void gba_dma_write(gba_t *g, uint32_t addr, uint16_t v);
/* services a sound-FIFO DMA request (trigger=3 channels, 4-word transfer) */
void gba_dma_fifo_request(gba_t *g, int fifo);

/* ---- APU (apu.c) --------------------------------------------------------------------------- */

#define GBA_FIFO_DEPTH 32u /* bytes per hardware FIFO (8 x 32-bit entries) */

typedef struct {
    uint16_t soundbias;
    uint16_t soundcnt_l;     /* PSG routing + per-side master volume */
    uint16_t soundcnt_h;     /* PSG volume bits + FIFO A/B control */
    uint8_t soundcnt_x;      /* bit7 = PSG master enable */
    /* Direct-sound FIFO channels A/B */
    uint8_t fifo[2][GBA_FIFO_DEPTH];
    uint8_t fifo_count[2];   /* bytes queued */
    int8_t fifo_cur[2];      /* sample popped on selected-timer overflow */
    uint8_t fifo_has[2];     /* fifo_cur holds a real sample */
    /* PSG channel state (squares 1/2 + noise) */
    uint8_t sq_duty[2], sq_env_vol[2], sq_env_timer[2], sq_volume[2];
    uint16_t sq_freq_timer[2], sq_period[2];
    uint8_t sq_duty_pos[2], sq_len[2], sq_len_en[2], sq_active[2];
    uint8_t noise_active, noise_len, noise_len_en, noise_env_vol;
    uint8_t noise_volume;    /* current output volume (loaded on restart) */
    uint8_t noise_width7;
    uint32_t noise_timer, noise_period; /* CPU cycles per LFSR shift */
    uint16_t noise_lfsr;
    uint32_t sample_acc;
    int16_t *out;
    size_t out_cap, out_pos;
} gba_apu;

void gba_apu_init(gba_apu *a, int16_t *out, size_t cap);
void gba_apu_reset(gba_apu *a);
void gba_apu_step(gba_t *g, uint32_t cycles);
uint16_t gba_apu_io_read16(gba_apu *a, uint32_t addr);
void gba_apu_io_write16(gba_apu *a, uint32_t addr, uint16_t v);
/* 8-bit FIFO writes (SOUNDFIFO A/B accept byte writes on hardware) */
void gba_apu_io_write8(gba_apu *a, uint32_t addr, uint8_t v);
/* raw 4-byte append used by the FIFO DMA path (no request generation) */
void gba_apu_fifo_write32(gba_t *g, int fifo, uint32_t v);

/* ---- HLE BIOS (swi.c) ------------------------------------------------------------------------ */

void gba_swi_hle(gba_t *g, uint32_t comment);
/* dispatches a pending hardware IRQ through the HLE BIOS dispatcher */
void gba_irq_dispatch(gba_t *g);
/* returns from the dispatched game handler to the interrupted context */
void gba_irq_dispatch_return(gba_t *g);

/* ---- top level (gba.c) -------------------------------------------------------------------------- */

struct gba {
    emu_core_t base;
    gba_cpu cpu;
    gba_mem mem;
    gba_cart cart;
    gba_ppu ppu;
    gba_timers timers;
    gba_dma dma;
    gba_apu apu;
    uint32_t buttons;
    uint32_t fb[GBA_SCREEN_W * GBA_SCREEN_H];
    int16_t audio[2048];
    emu_audio_cb_t audio_cb;
    void *audio_user;
    uint64_t total_cycles;
};

#endif /* EMU_MGBAX_GBA_H */
