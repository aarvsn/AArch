/*
 * AArch shared SH-4 CPU interpreter (used by supercastpro / Dreamcast).
 *
 * Implements the SH7750 (SH-4) integer instruction set - the full SH-2
 * compatible 16-bit family (per the SH-4 Software Manual) plus DT and the
 * single-precision FPU subset:
 *   FADD/FSUB/FMUL/FDIV/FCMP.EQ/FCMP.GT/FMAC (0xF0nd-0xF5nd, 0xFEnd),
 *   FABS/FSQRT/FNEG/FLDI0/FLDI1, FLOAT/FTRC, FMOV.S register and
 *   @(R0,Rn)/@Rn/@Rn+/@-Rn forms, FIPR and FTRV (XMTRX).
 *
 * System register access follows the manual's STS/LDS encodings, with
 * FPUL = 5A/5D and FPSCR = 6A/6A codes (documented convention used
 * consistently here and by the test suite).
 *
 * Documented simplifications:
 *  - PR=1 (double-precision) FPU ops execute as single precision.
 *  - FPSCR.DN flushes denormal results to zero (implemented); exception
 *    cause/enable/mask bits are stored but never raised.
 *  - Banked FPU registers (XF) exist and switch with FPSCR.FR.
 *  - Unsupported opcodes raise a documented no-op (no reserved-instruction
 *    exception model).
 *  - Cycle counts use a coarse model (1 per instruction, FPU ops more).
 *
 * Delay slots and banked registers follow the SH-4 architecture: R0-R7
 * bank on the SR.RB bit, branch delay slots execute before the target.
 */
#ifndef EMU_COMMON_SH4_H
#define EMU_COMMON_SH4_H

#include "emu/emu.h"

/* SR bits */
#define SH4_SR_T  0x00000001u
#define SH4_SR_S  0x00000002u
#define SH4_SR_I  0x000000F0u /* interrupt mask bits 4-7 */
#define SH4_SR_Q  0x00000100u
#define SH4_SR_M  0x00000200u
#define SH4_SR_FD 0x00008000u
#define SH4_SR_BL 0x00001000u
#define SH4_SR_RB 0x00002000u
#define SH4_SR_MD 0x40000000u

/* FPSCR bits */
#define SH4_FPSCR_RM     0x00000003u
#define SH4_FPSCR_FLAG   0x0001F000u
#define SH4_FPSCR_FR     0x00200000u
#define SH4_FPSCR_SZ     0x00100000u
#define SH4_FPSCR_PR     0x00080000u
#define SH4_FPSCR_DN     0x00040000u

typedef struct {
    void *user;
    uint8_t (*read8)(void *user, uint32_t addr);
    uint16_t (*read16)(void *user, uint32_t addr);
    uint32_t (*read32)(void *user, uint32_t addr);
    void (*write8)(void *user, uint32_t addr, uint8_t v);
    void (*write16)(void *user, uint32_t addr, uint16_t v);
    void (*write32)(void *user, uint32_t addr, uint32_t v);
} sh4_bus_t;

typedef struct {
    const sh4_bus_t *bus;

    uint32_t r[16];      /* current bank view (r0-r7 map via SR.RB) */
    uint32_t rbank[2][8];
    uint32_t pc, next_pc;
    uint32_t pr;
    uint32_t sr;         /* T/S/I/Q/M/FD/BL/RB/MD per bits above */
    uint32_t gbr, vbr;
    uint32_t ssr, spc, sgr, dbr;
    uint32_t mach, macl;
    uint32_t fpul, fpscr;
    uint32_t fr[16];     /* raw bits of the 16 FR bank */
    uint32_t xf[16];     /* raw bits of the 16 XF bank */
    uint64_t cycles;
} sh4_t;

void sh4_init(sh4_t *c, const sh4_bus_t *bus);
void sh4_reset(sh4_t *c); /* PC = 0xA0000000 boot area base, BL|MD set */

/* Execute one instruction; returns consumed cycles. */
uint32_t sh4_step(sh4_t *c);

/* Request an interrupt; vectors through VBR + offset when taken (BL/I
 * gating per the architecture). level is the interrupt priority used to
 * update SR.I; offset is the VBR-relative vector, e.g. 0x400 for TUNI0,
 * 0x200 + 0x20*level for external IRL lines. */
void sh4_irq(sh4_t *c, int level, uint32_t offset);

/* Test/inspection hooks. */
uint32_t sh4_fr_bits(const sh4_t *c, int n); /* raw bits of FR[n] (banked) */
void sh4_set_fr_bits(sh4_t *c, int n, uint32_t bits);

#endif /* EMU_COMMON_SH4_H */
