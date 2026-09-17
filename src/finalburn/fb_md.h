/*
 * finalburn: Sega Genesis / Mega Drive core.
 *
 * Machine model (v1, see README for the documented subset):
 *   - 68000: broad instruction subset, cycle-count approximations
 *   - Z80: full documented instruction set (base/CB/ED/DD/FD), IM0/1/2
 *   - VDP 315-5313: mode-4 graphics (planes A/B/window, sprites, scrolling,
 *     DMA), V-int/H-int, status/HV counters
 *   - PSG SN76489 + YM2612 (4-op FM, 8 algorithms, DAC) integer-only
 *   - 3-button pad on port 1
 *
 * NTSC frame: 262 scanlines, 488 68K cycles per line, 127840 cycles/frame
 * (7670453 Hz / 60). Z80 at 3579545 Hz. Output: 320x224 XRGB8888,
 * 48000 Hz stereo PCM.
 */
#ifndef FB_MD_H
#define FB_MD_H

#include "emu/emu.h"
#include "../common/util.h"

#define FB_SCREEN_W   320u
#define FB_SCREEN_H   224u
#define FB_OUT_RATE   48000u

#define FB_LINES          262
#define FB_VBLANK_LINE    224
#define FB_68K_PER_LINE   488
#define FB_68K_PER_FRAME  127840   /* 262*488 = 127856; frame loop folds the drift */

#define FB_MASTER_HZ   7670453u   /* 68K clock */
#define FB_Z80_HZ      3579545u   /* Z80 clock */
#define FB_YM_DIV      144        /* YM2612 internal sample = master/144 */
#define FB_PSG_DIV     16         /* PSG tone tick = z80 clock/16        */

#define FB_RAM_SIZE    0x10000u
#define FB_ZRAM_SIZE   0x2000u
#define FB_VRAM_SIZE   0x10000u
#define FB_CRAM_SIZE   0x80u      /* 64 words, byte-addressed */
#define FB_VSRAM_SIZE  0x50u      /* 40 words, byte-addressed */

#define FB_MAX_SAMPLES 832u       /* 48000/60 = 800; slack for Bresenham drift */

/* ---- 68000 ------------------------------------------------------------- */

struct fb_m68k {
    uint32_t d[8];
    uint32_t a[8];        /* a[7] is the ACTIVE stack pointer          */
    uint32_t usp, ssp;
    uint32_t pc;
    uint32_t ppc;         /* PC of the currently executing instruction */
    uint16_t sr;          /* T S 0 0 I2I1I0 0 0 0 XNZVC               */
    uint8_t stopped;      /* STOP executed, waiting for interrupt      */
    uint8_t halted;       /* double bus fault / catastrophic halt      */
    int32_t cycles;       /* consumed cycles for the current slice     */
    uint8_t int_line;     /* highest pending IPL level (0 = none)      */
};

#define FB_SR_T     0x8000u
#define FB_SR_S     0x2000u
#define FB_SR_I     0x0700u
#define FB_SR_IPL(s) (((s) >> 8) & 7u)
#define FB_SR_X     0x0010u
#define FB_SR_N     0x0008u
#define FB_SR_Z     0x0004u
#define FB_SR_V     0x0002u
#define FB_SR_C     0x0001u

/* ---- Z80 --------------------------------------------------------------- */

struct fb_z80 {
    uint16_t af, bc, de, hl, sp, pc, ix, iy, af2, bc2, de2, hl2;
    uint8_t  i, r, im;    /* im: 0, 1, 2 */
    uint8_t  iff1, iff2;
    uint8_t  halted;
    uint8_t  int_pending; /* INT line sampled */
    int32_t  cyc;         /* t-states consumed in the current slice */
};

/* ---- VDP --------------------------------------------------------------- */

struct fb_vdp {
    uint8_t  vram[FB_VRAM_SIZE];
    uint8_t  cram[FB_CRAM_SIZE];
    uint8_t  vsram[FB_VSRAM_SIZE];

    uint16_t regs[24];

    uint8_t  cmd_pending;   /* first control word written                */
    uint16_t cmd_w1;        /* latched first command word                */
    uint32_t addr;          /* current address counter                   */
    uint8_t  mode;          /* access mode bits 2:0 from command word    */
    uint16_t read_buf;      /* read-ahead buffer                         */
    uint8_t  read_buf_valid;

    /* DMA engine */
    uint8_t  dma_active;
    uint8_t  dma_type;      /* 0=68K->VDP 1=fill 2=copy                  */
    uint32_t dma_left;      /* words for 0/1, bytes for 2                */
    uint32_t dma_src;
    uint8_t  dma_fill_val;

    /* interrupt state */
    uint8_t  vint_68k;      /* level 6                                   */
    uint8_t  hint_68k;      /* level 4                                   */
    uint8_t  vint_z80;
    uint8_t  irq_line_ym;   /* YM2612 timer IRQ -> 68K level 2           */

    uint8_t  sprite_collision, sprite_overflow;
    uint16_t hint_counter;

    int      line;          /* current scanline 0..261                   */
    uint32_t line_cycle;    /* 68K cycles elapsed in the current line    */

    /* per-line latches */
    uint16_t hscroll_a, hscroll_b;

    uint32_t *fb;
};

/* ---- PSG (SN76489) ----------------------------------------------------- */

struct fb_psg {
    uint16_t tone[3];       /* 10-bit tone period N */
    uint8_t  vol[4];        /* 4-bit attenuation    */
    uint16_t tone_ctr[3];
    uint8_t  tone_out[3];
    uint16_t noise_shift;   /* 15-bit LFSR */
    uint8_t  noise_mode;    /* bit0: white, bits 2-1: rate select */
    uint16_t noise_ctr;
    uint16_t noise_per;     /* ticks per LFSR shift */
    uint8_t  noise_out;
    uint8_t  latch_type;    /* 0 = tone period, 1 = volume */
    uint8_t  latch_chan;    /* 0..3 */
};

/* ---- YM2612 ------------------------------------------------------------ */

struct fb_ym_op {
    uint16_t fnum;
    uint8_t  block;
    uint8_t  mult;          /* 0 = x0.5 */
    uint32_t phase;         /* 20-bit */
    uint32_t phase_inc;     /* per internal sample */
    uint8_t  tl;            /* total level (0.75 dB units) */
    uint8_t  dt;
    uint8_t  ks;
    uint8_t  ar, dr, sr, rr;
    uint8_t  sl;            /* sustain level 0-15 */
    int32_t  att_q8;        /* envelope attenuation, Q8, 0..128*256 */
    uint8_t  state;         /* 0=release 1=attack 2=decay 3=sustain */
};

struct fb_ym_chan {
    struct fb_ym_op op[4];
    uint8_t  algorithm;     /* 0-7 */
    uint8_t  feedback;
    uint8_t  pan_l, pan_r;
    uint8_t  ams, fms;      /* registered, unused (LFO off) */
    uint8_t  key_on;        /* bit per operator */
    int16_t  out;           /* last mixed output (pre-volume) */
    int16_t  fb_hist[2];    /* operator-1 feedback history */
};

struct fb_ym {
    struct fb_ym_chan chan[6];
    uint8_t  dac_data;
    uint8_t  dac_en;
    uint8_t  addr_bank;     /* 0/1 selected via second address port */
    uint8_t  reg_latched[2];
    uint16_t timer_a_load, timer_b_load;
    uint16_t timer_a_ctr, timer_b_ctr;
    uint8_t  timer_a_en, timer_b_en;
    uint8_t  timer_a_over, timer_b_over;
    uint8_t  tb_div;        /* timer B 1-of-3 prescaler */
    uint8_t  irq;
    int16_t  last_l, last_r;
};

/* ---- cartridge --------------------------------------------------------- */

struct fb_cart {
    uint8_t *rom;
    size_t   rom_size;
    uint32_t rom_mask;      /* power-of-two mask, or 0 when using modulo */
    uint8_t *sram;          /* optional 64 KiB at $200000 when header says so */
    int      sram_en;
};

/* ---- machine ----------------------------------------------------------- */

/* 32X adapter extension (used by the ms-32 core): when ext32x is set, the
 * 68K bus forwards the A15100h-A1517Fh word window to the adapter. */
struct fb_32x;
typedef uint16_t (*fb_32x_read_fn)(void *ext, uint32_t addr);
typedef void (*fb_32x_write_fn)(void *ext, uint32_t addr, uint16_t v);

struct fb_md {
    emu_core_t base;

    /* 32X adapter extension hook (NULL in the plain Genesis core). */
    void *ext32x;
    fb_32x_read_fn ext32x_read;
    fb_32x_write_fn ext32x_write;

    struct fb_m68k m68k;
    struct fb_z80  z80;
    struct fb_vdp  vdp;
    struct fb_psg  psg;
    struct fb_ym   ym;
    struct fb_cart cart;

    uint32_t fb[FB_SCREEN_W * FB_SCREEN_H];
    uint8_t  ram[FB_RAM_SIZE];
    uint8_t  zram[FB_ZRAM_SIZE];

    /* 68K -> Z80 control */
    uint8_t  z80_busreq;
    uint8_t  z80_reset;
    uint8_t  z80_bank;
    int32_t  z80_credit;    /* pending Z80 t-states */

    /* pad */
    uint8_t  pad1;          /* active-low button state */
    uint8_t  pad_th;        /* TH select latch */
    uint32_t input;         /* public input bitmask */

    /* audio */
    int16_t  audio[FB_MAX_SAMPLES * 2];
    size_t   audio_count;
    emu_audio_cb_t audio_cb;
    void    *audio_user;

    /* clock accumulators (fixed-point bresenham) */
    uint64_t z80_acc;
    uint64_t ym_acc;
    uint64_t psg_acc;
    uint64_t out_acc;

    int      frame_done;
};

/* ---- subsystem entry points --------------------------------------------- */

/* cart */
void fb_cart_init(struct fb_cart *c);
void fb_cart_free(struct fb_cart *c);
emu_result_t fb_cart_load(struct fb_cart *c, const uint8_t *data, size_t size);
uint8_t fb_cart_read8(struct fb_cart *c, uint32_t addr);
void fb_cart_sram_write8(struct fb_cart *c, uint32_t addr, uint8_t v);
uint8_t fb_cart_sram_read8(struct fb_cart *c, uint32_t addr);

/* 68K */
void fb_m68k_reset_machine(struct fb_md *md);
int32_t fb_m68k_run(struct fb_md *md, int32_t budget);
void fb_m68k_set_irq(struct fb_md *md, uint8_t level);

/* Z80 */
void fb_z80_reset(struct fb_z80 *z);
int32_t fb_z80_run(struct fb_md *md, int32_t budget);

/* VDP */
void fb_vdp_init(struct fb_vdp *v, uint32_t *fb);
void fb_vdp_reset(struct fb_vdp *v);
uint8_t fb_vdp_read8(struct fb_md *md, uint32_t addr);
uint16_t fb_vdp_read16(struct fb_md *md, uint32_t addr);
void fb_vdp_write8(struct fb_md *md, uint32_t addr, uint8_t v);
void fb_vdp_write16(struct fb_md *md, uint32_t addr, uint16_t v);
void fb_vdp_end_of_line(struct fb_md *md);
void fb_vdp_run_dma(struct fb_md *md, int32_t cycles);

/* PSG */
void fb_psg_init(struct fb_psg *p);
void fb_psg_reset(struct fb_psg *p);
void fb_psg_write(struct fb_psg *p, uint8_t v);
void fb_psg_tick(struct fb_psg *p);
int16_t fb_psg_sample(const struct fb_psg *p);

/* YM2612 */
void fb_ym_init(struct fb_ym *y);
void fb_ym_reset(struct fb_ym *y);
void fb_ym_tick(struct fb_ym *y);
void fb_ym_write_addr(struct fb_ym *y, uint8_t bank, uint8_t reg);
void fb_ym_write_data(struct fb_ym *y, uint8_t bank, uint8_t data);
void fb_ym_mix(struct fb_ym *y);

/* Z80 bus bridge */
uint8_t fb_z80_bus_read(struct fb_md *md, uint16_t addr);
void fb_z80_bus_write(struct fb_md *md, uint16_t addr, uint8_t v);
uint8_t fb_z80_io_read(struct fb_md *md, uint16_t port);
void fb_z80_io_write(struct fb_md *md, uint16_t port, uint8_t v);

/* 68K bus */
uint8_t  fb_md_68k_read8(struct fb_md *md, uint32_t addr);
uint16_t fb_md_68k_read16(struct fb_md *md, uint32_t addr);
uint32_t fb_md_68k_read32(struct fb_md *md, uint32_t addr);
void fb_md_68k_write8(struct fb_md *md, uint32_t addr, uint8_t v);
void fb_md_68k_write16(struct fb_md *md, uint32_t addr, uint16_t v);
void fb_md_68k_write32(struct fb_md *md, uint32_t addr, uint32_t v);

#endif /* FB_MD_H */
