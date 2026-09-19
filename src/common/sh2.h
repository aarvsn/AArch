/*
 * AArch shared SH-2 CPU interpreter (used by ms-32 / Sega 32X and
 * supersaturn / Sega Saturn).
 *
 * Implements the full SH7604 (SH-2) instruction set as documented in the
 * Hitachi SH-1/SH-2 Programming Manual: data transfer (incl. @-Rn, @Rn+,
 * @(R0,Rn), @(disp,GBR/Rn/PC) forms), arithmetic (incl. DIV0S/DIV0U/DIV1
 * hardware division steps, DMULS/DMULU 32x32=64), logic, shifts/rotates,
 * branches (incl. SH-2 delayed variants BFS/BTS/BRAF/BSRF), system control
 * (LDC/STS/LDS, TRAPA, RTE, SLEEP). Bus access goes through caller-supplied
 * callbacks so both systems can map the CPU into their memory maps.
 *
 * Cycle counts use a documented coarse model (base 1, memory/shift ops more)
 * rather than exact pipeline timing.
 */
#ifndef EMU_COMMON_SH2_H
#define EMU_COMMON_SH2_H

#include "emu/emu.h"

#define SH2_T 0x00000001u
#define SH2_S 0x00000002u
#define SH2_I 0x000000F0u /* interrupt mask bits 4-7 */
#define SH2_Q 0x00000100u
#define SH2_M 0x00000200u

typedef struct {
    void *user;
    uint8_t (*read8)(void *user, uint32_t addr);
    uint16_t (*read16)(void *user, uint32_t addr);
    uint32_t (*read32)(void *user, uint32_t addr);
    void (*write8)(void *user, uint32_t addr, uint8_t v);
    void (*write16)(void *user, uint32_t addr, uint16_t v);
    void (*write32)(void *user, uint32_t addr, uint32_t v);
} sh2_bus_t;

typedef struct {
    const sh2_bus_t *bus;
    uint32_t r[16];
    uint32_t pc, next_pc;
    uint32_t pr;
    uint32_t sr;     /* T/S/I/Q/M per bit definitions above */
    uint32_t gbr, vbr;
    uint32_t mach, macl;
    uint64_t cycles;
} sh2_t;

void sh2_init(sh2_t *c, const sh2_bus_t *bus);
void sh2_reset(sh2_t *c);
/* Execute one instruction; returns consumed cycles. */
uint32_t sh2_step(sh2_t *c);
/* Request an interrupt (level 0-15); auto-acknowledged on RTE.
 * Vectors through VBR + 0x600 + level*4. */
void sh2_irq(sh2_t *c, int level);
/* Interrupt with an explicit vector number (level 0-15, vector number as
 * supplied by the system's interrupt controller, e.g. Saturn SCU 0x40-0x5F).
 * Vectors through VBR + 0x600 + vector*4. */
void sh2_irq_vector(sh2_t *c, int level, uint32_t vector);

#endif /* EMU_COMMON_SH2_H */
