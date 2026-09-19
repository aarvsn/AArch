/*
 * m64-b: Nintendo 64 core (partial).
 *
 * Sources and scope (documented subset):
 *  - CPU: MIPS R4300i (MIPS III) integer interpreter running in big-endian
 *    mode: full MIPS I integer set, MIPS III shifts/loads/stores (LD/SD/
 *    LDL/LDR/SDL/SDR/LL/SC/LLD/SCD), 64-bit GPRs, CP0 (Status, Cause, EPC,
 *    ErrorEPC, Count/Compare interrupt, PRId, Config) and the exception
 *    model (AdEL/AdES/Sys/Bp/RI/Int, BEV vectors, ERET). Address
 *    translation: kseg0/kseg1 fixed mappings; kuseg is unmapped-cached
 *    while Status.ERL=1 (bootstrap state) and raises Address Error once
 *    ERL clears - the on-chip TLB is NOT implemented (documented).
 *  - Boot model: no PIF/CIC ROM. The core copies ROM[0x040..0x1000) (the
 *    cartridge's IPL3) into SP IMEM at 0x04001000 and starts the CPU at
 *    0xA4000040 with r29 = 0xA4001FF0, Status = ERL|BEV. This matches the
 *    observable PIF behavior without bundling firmware.
 *  - Memory: 4 MiB RDRAM, SP DMEM/IMEM, PI/SI/MI/VI registers, cartridge
 *    ROM at 0x10000000, PIF ROM (2 KiB, RAM-backed in place of the
 *    unmodeled boot ROM) + 64-byte PIF RAM.
 *  - PI: cart->RDRAM DMA (PI_RD_LEN write); SI: 64-byte RDRAM<->PIF DMA;
 *    MI: interrupt latch and mask (VI/PI/SI/SP/AI/DP bits).
 *  - VI: framebuffer output from RDRAM at VI_ORIGIN/VI_WIDTH with pixel
 *    type 1 = 16 bpp RGBA5551 and 3 = 32 bpp RGBA8; VI interrupt at the
 *    programmed half-line. Video timing is a fixed 60 Hz frame.
 *  - Not implemented (honest gaps): RSP (incl. DP/rasterizer), audio
 *    (AI), CIC/PIF security, RDRAM expansion, TLB, controllerpak.
 *
 * Bus model: uniformly big-endian (the CPU view). The VI reads through
 * the same bus, so framebuffer data appears consistent (documented
 * simplification of the hardware's byte-lane wiring).
 */
#ifndef EMU_M64_B_N64_H
#define EMU_M64_B_N64_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"

#define N64_SCREEN_W 320u
#define N64_SCREEN_H 240u
#define N64_OUT_RATE 32000u

#define N64_CYCLES_PER_FRAME (93750000u / 60u / 2u) /* coarse, documented */

/* CP0 registers */
#define N64_CP0_INDEX 0u
#define N64_CP0_COUNT 9u
#define N64_CP0_COMPARE 11u
#define N64_CP0_STATUS 12u
#define N64_CP0_CAUSE 13u
#define N64_CP0_EPC 14u
#define N64_CP0_PRID 15u
#define N64_CP0_CONFIG 16u
#define N64_CP0_ERROREPC 30u

/* Status bits */
#define N64_ST_BEV 0x00400000u
#define N64_ST_IMASK 0x0000FF00u
#define N64_ST_ERL 0x00000004u
#define N64_ST_EXL 0x00000002u
#define N64_ST_IE 0x00000001u

/* Cause ExcCode values */
enum {
    N64_EXC_INT = 0,
    N64_EXC_ADEL = 4,
    N64_EXC_ADES = 5,
    N64_EXC_SYS = 8,
    N64_EXC_BP = 9,
    N64_EXC_RI = 10,
    N64_EXC_CPU = 11,
    N64_EXC_OV = 12
};

/* MI interrupt bits */
#define N64_MI_SP 0x01u
#define N64_MI_SI 0x02u
#define N64_MI_AI 0x04u
#define N64_MI_VI 0x08u
#define N64_MI_PI 0x10u
#define N64_MI_DP 0x20u

/* RCP register word offsets (physical base) */
#define N64_MI_BASE 0x04300000u
#define N64_VI_BASE 0x04400000u
#define N64_PI_BASE 0x04600000u
#define N64_SI_BASE 0x04800000u

struct n64 {
    emu_core_t base;

    /* CPU */
    uint64_t r[32];
    uint64_t hi, lo;
    uint32_t pc, next_pc;
    int in_delay; /* current instruction sits in a branch delay slot */
    uint32_t cp0[32];
    uint64_t cycles;
    uint8_t ll_bit;

    /* memory */
    uint8_t rdram[4 * 1024 * 1024];
    uint8_t dmem[4 * 1024];
    uint8_t imem[4 * 1024];
    uint8_t pif_rom[2048]; /* RAM-backed boot ROM substitute (documented) */
    uint8_t pif_ram[64];
    uint8_t *rom;
    size_t rom_size;

    /* RCP register files (32-bit words) */
    uint32_t mi[4], vi[16], pi[6], si[8];
    uint32_t sp_reg[10]; /* SP status etc.; storage only (RSP absent) */

    /* video */
    uint32_t fb[N64_SCREEN_W * N64_SCREEN_H];
    uint32_t buttons;
    uint32_t frame_count;
    uint32_t vi_cur; /* current half-line within the frame */
    int vi_intr_pending;
};

/* Test/inspection hooks (used by the suite; not part of the public API). */
uint32_t n64_bus_read32(struct n64 *n, uint32_t addr);
void n64_bus_write32(struct n64 *n, uint32_t addr, uint32_t v);
void n64_step(struct n64 *n); /* one CPU instruction */
void n64_vi_render(struct n64 *n);

#endif /* EMU_M64_B_N64_H */
