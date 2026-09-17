/*
 * beatle-psx: Sony PlayStation core (internal interface).
 *
 * Status: PARTIAL. Implemented: MIPS R3000A (full MIPS I user set, load and
 * branch delay slots, COP0 exceptions/RFE), GTE geometry coprocessor (all
 * documented commands incl. the hardware division algorithm and the MVMVA
 * cv=2 bug), GPU with 1 MiB VRAM (fills, polygons/lines/rectangles incl.
 * textured and semi-transparent forms, VRAM transfers, mask/dither), DMA
 * (burst, sync-block and linked-list modes, OTC), root counters, interrupt
 * controller, digital joypad, PS-X EXE loader (no BIOS image required).
 * NOT implemented (documented gaps): CD-ROM drive (commands answer "no
 * disc" style responses; disc streaming impossible), SPU (silent), MDEC
 * (register stubs), memory control timing, cache isolation effects.
 */
#ifndef EMU_PSX_PSX_H
#define EMU_PSX_PSX_H

#include "emu/emu.h"

#define PSX_RAM_SIZE     (2u * 1024u * 1024u)
#define PSX_SCRATCH_SIZE 1024u
#define PSX_VRAM_W       1024u
#define PSX_VRAM_H       512u
#define PSX_FB_W         320u
#define PSX_FB_H         240u
#define PSX_CYCLES_PER_FRAME 563680u /* 33.8688 MHz / 60.0988 Hz (approx.)   */
#define PSX_LINES_PER_FRAME  263u    /* NTSC; 563680/263 cycles per line      */

/* Root counters (approximate but deterministic timing model). */
#define PSX_DOTS_PER_SYSCLK 10u  /* dotclock = sysclk/10 (5.37 MHz approx.) */
#define PSX_HBLANK_LINE     240u /* first vblank line (approx.)             */

struct psx;

/* ---- interrupts ----------------------------------------------------------- */
#define PSX_IRQ_VBLANK 0x0001u
#define PSX_IRQ_GPU    0x0002u
#define PSX_IRQ_CDROM  0x0004u
#define PSX_IRQ_DMA    0x0008u
#define PSX_IRQ_TMR0   0x0010u
#define PSX_IRQ_TMR1   0x0020u
#define PSX_IRQ_TMR2   0x0040u
#define PSX_IRQ_PAD    0x0080u
#define PSX_IRQ_SIO    0x0100u
#define PSX_IRQ_SPU    0x0200u
#define PSX_IRQ_PIO    0x0400u

void psx_irq_raise(struct psx *p, uint16_t bits);

/* ---- GTE (implemented in gte.c) -------------------------------------------- */
typedef struct {
    uint32_t dr[32]; /* data registers cop2r0..31 */
    uint32_t cr[32]; /* control registers cop2r32..63 */
} psx_gte_t;

/* Register access (MFC2/CFC2/MTC2/CTC2); reg 0..63 per the GTE numbering. */
uint32_t psx_gte_read(psx_gte_t *g, uint32_t reg);
void psx_gte_write(psx_gte_t *g, uint32_t reg, uint32_t v);
/* Execute a COP2 command word (funct bits 0-5; sf bit19, lm bit25). */
void psx_gte_execute(psx_gte_t *g, uint32_t op);

/* ---- CPU (implemented in cpu.c) -------------------------------------------- */
typedef struct {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t pc, next_pc;
    uint32_t cop0_sr, cop0_cause, cop0_epc, cop0_prid, cop0_badva;
    /* Load delay slot: the load becomes visible after one instruction. */
    uint32_t ld_dreg, ld_dval;
    uint32_t ld_new_dreg, ld_new_dval;
    /* Exception context (fetched-instruction address and delay-slot flag). */
    uint32_t exc_pc;
    uint32_t exc_in_delay;
    struct psx *p; /* back pointer for bus access and exceptions */
    uint64_t cycles; /* total executed (diagnostics) */
} psx_cpu_t;

void psx_cpu_init(psx_cpu_t *c, struct psx *p);
void psx_cpu_reset(psx_cpu_t *c);
/* Execute one instruction; returns the number of cycles it consumed. */
uint32_t psx_cpu_step(psx_cpu_t *c);

/* ---- GPU (implemented in gpu.c) --------------------------------------------- */
typedef struct {
    uint16_t vram[PSX_VRAM_W * PSX_VRAM_H];

    /* Command FIFO state (single pending command + parameters). */
    uint32_t fifo[64];
    uint8_t fifo_len;
    uint8_t fifo_need;   /* expected parameter words for current command */
    uint8_t fifo_active;

    /* Rendering state. */
    uint32_t tpage;      /* E1 draw mode bits (GPUSTAT 0-11,15 mirror)   */
    uint32_t texwin;     /* E2 texture window                            */
    uint32_t area_x1, area_y1, area_x2, area_y2; /* drawing area (E3/E4) */
    int32_t off_x, off_y;                        /* drawing offset (E5)  */
    uint32_t mask_set;   /* E6 bit0: set mask bit when drawing           */
    uint32_t mask_check; /* E6 bit1: do not draw over masked pixels      */

    /* VRAM->CPU transfer state. */
    int32_t read_x, read_y, read_w, read_h, read_px, read_py;
    int read_pending;

    /* CPU->VRAM streaming transfer (GP0 A0h). */
    int xfer_active;
    uint32_t xfer_left;  /* halfwords remaining                        */
    int32_t xfer_x, xfer_y, xfer_x0, xfer_w, xfer_h;

    uint32_t gpustat;    /* bits 13-31 (status; 0-12 derived from state) */
    uint32_t gread;      /* GPUREAD response latch                       */

    /* Display configuration (GP1). */
    uint32_t disp_x, disp_y;         /* display area start in VRAM       */
    uint32_t disp_w, disp_h;         /* display area size in VRAM        */
    uint32_t disp_depth24;           /* GP1(08).4                        */
    uint32_t disp_enabled;           /* GP1(03)                          */
} psx_gpu_t;

void psx_gpu_init(psx_gpu_t *g);
void psx_gpu_reset(psx_gpu_t *g);
void psx_gpu_write_gp0(psx_gpu_t *g, struct psx *p, uint32_t v);
void psx_gpu_write_gp1(psx_gpu_t *g, struct psx *p, uint32_t v);
uint32_t psx_gpu_status(const psx_gpu_t *g);
uint32_t psx_gpu_read(psx_gpu_t *g);
/* Render one frame into the core framebuffer (XRGB8888 320x240). */
void psx_gpu_render(const psx_gpu_t *g, uint32_t *fb);

/* ---- hardware block (implemented in hw.c) ----------------------------------- */
typedef struct {
    uint32_t madr, bcr, chcr;
} psx_dma_ch_t;

typedef struct {
    psx_dma_ch_t ch[7];
    uint32_t dpcr, dicr;
} psx_dma_t;

typedef struct {
    uint16_t count, mode, target;
    uint16_t mode_read_latch; /* bits 11/12 clear after read */
} psx_timer_t;

typedef struct {
    /* Simplified digital pad: replies after host sends 01h,42h,00h,00h. */
    uint8_t reply[4];    /* bytes to return for the current access        */
    uint8_t reply_len, reply_pos;
    uint8_t expect;      /* handshake stage counter                      */
    uint16_t buttons;    /* active-low mask of pressed buttons           */
    uint32_t ctrl;       /* JOY_CTRL register                            */
    uint16_t stat;       /* JOY_STAT register                            */
} psx_pad_t;

/* Write a word to main RAM (DMA/OTC use); addr is the KSEG-masked offset. */
void psx_ram_write32(struct psx *p, uint32_t off, uint32_t v);

/* Hardware I/O space dispatch (timers, IRQ, DMA, GPU, CD, SPU, pads). */
uint32_t psx_hw_read(struct psx *p, uint32_t addr);
void psx_hw_write(struct psx *p, uint32_t addr, uint32_t v);

void psx_hw_reset(psx_dma_t *d, psx_timer_t *t, psx_pad_t *pad);
/* Run pending DMA transfers (called after CHCR writes and each tick batch). */
void psx_dma_run(struct psx *p);
/* Advance timers by `cycles` system clocks. */
void psx_timers_tick(struct psx *p, uint32_t cycles, uint32_t line_cycle_pos,
                     uint32_t scanline, int in_hblank, int in_vblank);

/* ---- top-level bus + core (implemented in psx.c) ---------------------------- */
struct psx {
    emu_core_t base;

    psx_cpu_t cpu;
    psx_gte_t gte;
    psx_gpu_t gpu;
    psx_dma_t dma;
    psx_timer_t timer[3];
    psx_pad_t pad;

    uint8_t ram[PSX_RAM_SIZE];
    uint8_t scratch[PSX_SCRATCH_SIZE];
    uint16_t irq_istat, irq_imask;

    uint32_t scanline;       /* current line 0..262                       */
    uint32_t line_cycles;    /* cycles consumed in current scanline       */
    int vblank_active;

    uint32_t fb[PSX_FB_W * PSX_FB_H];

    emu_audio_cb_t audio_cb;
    void *audio_user;

    uint8_t rom_ok;   /* ROM loaded                                       */
    uint32_t input;
};

/* Bus access used by the CPU; full memory map incl. hardware registers. */
uint32_t psx_bus_read32(struct psx *p, uint32_t addr);
uint16_t psx_bus_read16(struct psx *p, uint32_t addr);
uint8_t psx_bus_read8(struct psx *p, uint32_t addr);
void psx_bus_write32(struct psx *p, uint32_t addr, uint32_t v);
void psx_bus_write16(struct psx *p, uint32_t addr, uint16_t v);
void psx_bus_write8(struct psx *p, uint32_t addr, uint8_t v);

/* CPU raises an exception (vector, cause code, badvaddr). */
void psx_cpu_exception(struct psx *p, uint32_t code, uint32_t badva);

#endif /* EMU_PSX_PSX_H */
