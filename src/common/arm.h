/*
 * AArch shared ARM CPU interpreter (used by mds-a / Nintendo DS for the
 * ARM946E-S and the ARM7TDMI).
 *
 * Implements the ARMv4T instruction set (ARM + Thumb) plus the ARMv5TE
 * subset present on the ARM946E-S (CLZ, BLX, QADD/QSUB/QDADD/QDSUB,
 * SMULxy/SMLAxy/SMULWy/SMLAWy), selected with the v5te flag.
 *
 * Sources: ARM ARM (ARMv4/v5 architecture reference) and the ARM946E-S
 * technical reference. Signed/unsigned semantics, carry/borrow rules and
 * the shift-carry rules follow the architecture definitions.
 *
 * Pipeline model: PC-as-operand reads cur+8 (ARM) / cur+4 (Thumb) - the
 * documented architectural value. Cycle counts use a coarse model (1 per
 * instruction); memory wait states are the machine's business.
 *
 * Exceptions: Reset/SWI/UND/PABT/DABT/IRQ/FIQ vectoring with banked
 * registers, per-mode SPSRs and proper LR values (return with the usual
 * SUBS PC, LR, #4/#8 sequences). IRQ/FIQ lines are level inputs latched
 * by arm_irq()/arm_fiq().
 *
 * Coprocessor interface: MRC/MCR to CP15 route through caller-supplied
 * hooks (the DS core implements the ARM946E-S CP15 there); unhandled
 * coprocessor instructions raise UND.
 */
#ifndef EMU_COMMON_ARM_H
#define EMU_COMMON_ARM_H

#include "emu/emu.h"

/* Processor modes */
#define ARM_MODE_USR 0x10u
#define ARM_MODE_FIQ 0x11u
#define ARM_MODE_IRQ 0x12u
#define ARM_MODE_SVC 0x13u
#define ARM_MODE_ABT 0x17u
#define ARM_MODE_UND 0x1Bu
#define ARM_MODE_SYS 0x1Fu

/* CPSR bits */
#define ARM_F_N 0x80000000u
#define ARM_F_Z 0x40000000u
#define ARM_F_C 0x20000000u
#define ARM_F_V 0x10000000u
#define ARM_F_Q 0x08000000u
#define ARM_F_I 0x00000080u
#define ARM_F_F 0x00000040u
#define ARM_F_T 0x00000020u
#define ARM_F_MODE 0x0000001Fu

/* Exception vector offsets */
#define ARM_VEC_RESET 0x00u
#define ARM_VEC_UND   0x04u
#define ARM_VEC_SWI   0x08u
#define ARM_VEC_PABT  0x0Cu
#define ARM_VEC_DABT  0x10u
#define ARM_VEC_IRQ   0x18u
#define ARM_VEC_FIQ   0x1Cu

typedef struct {
    void *user;
    uint8_t (*read8)(void *user, uint32_t addr);
    uint16_t (*read16)(void *user, uint32_t addr);
    uint32_t (*read32)(void *user, uint32_t addr);
    void (*write8)(void *user, uint32_t addr, uint8_t v);
    void (*write16)(void *user, uint32_t addr, uint16_t v);
    void (*write32)(void *user, uint32_t addr, uint32_t v);
} arm_bus_t;

/*
 * CP15 hooks (optional; NULL = no coprocessor).
 * Return 1 when handled, 0 to raise UND. mrc writes *out on success.
 */
typedef struct {
    int (*mrc)(void *user, int cp, int opc1, int crn, int rd, int opc2,
               int crm, uint32_t *out);
    int (*mcr)(void *user, int cp, int opc1, int crn, int rd, int opc2,
               int crm, uint32_t val);
} arm_cp15_t;

typedef struct {
    const arm_bus_t *bus;
    const arm_cp15_t *cp15; /* may be NULL */

    uint32_t r[16];
    uint32_t pc;          /* address of the NEXT instruction to fetch */
    uint32_t cpsr;

    /* Banked registers (see mode_index() in arm.c). */
    uint32_t bank_r13[6], bank_r14[6];
    uint32_t bank_spsr[6]; /* [0] (usr/sys) unused, always 0 */
    uint32_t fiq_r8[5];    /* r8-r12 when in FIQ mode */

    int v5te;              /* ARM946E-S feature set when 1 */
    int irq_line, fiq_line;

    uint64_t cycles;
} arm_t;

void arm_init(arm_t *c, const arm_bus_t *bus, const arm_cp15_t *cp15);
void arm_reset(arm_t *c); /* vector 0, SVC mode, IRQ/FIQ masked, ARM state */

/* Execute one instruction; returns consumed cycles. */
uint32_t arm_step(arm_t *c);

/* Level-triggered interrupt inputs (latched until serviced). */
void arm_irq(arm_t *c);
void arm_fiq(arm_t *c);

/* Test/inspection hook: value the CPU would currently read from r15. */
uint32_t arm_r15_read(const arm_t *c, uint32_t cur);

#endif /* EMU_COMMON_ARM_H */
