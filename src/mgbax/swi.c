/*
 * mgbax HLE BIOS layer.
 *
 * No BIOS ROM is bundled. SWI calls are serviced in C; the IRQ dispatcher
 * is emulated: it saves {r0-r3,r12,lr}, calls the handler at $03007FFC and
 * restores state (matching the documented BIOS contract). IntrWait/
 * VBlankIntrWait are implemented by halting the CPU until the requested
 * interrupt bits arrive.
 *
 * HLE SWI services implemented: SoftReset (partial), RegisterRamReset,
 * Halt, IntrWait, VBlankIntrWait, Div, DivArm, Sqrt, ArcTan, ArcTan2,
 * CpuSet, CpuFastSet, GetBiosChecksum (fixed value). Others return 0
 * (documented).
 */
#include "gba.h"

#include <string.h>

#define SWI_SOFTRESET 0x00u
#define SWI_REGISTERRAMRESET 0x01u
#define SWI_HALT 0x02u
#define SWI_INTRWAIT 0x04u
#define SWI_VBLANKINTRWAIT 0x05u
#define SWI_DIV 0x06u
#define SWI_DIVARM 0x07u
#define SWI_SQRT 0x08u
#define SWI_ARCTAN 0x09u
#define SWI_ARCTAN2 0x0Au
#define SWI_CPUSET 0x0Bu
#define SWI_CPUFASTSET 0x0Cu
#define SWI_GETBIOSCHECKSUM 0x0Du

static uint32_t irq_handler_addr(gba_t *g)
{
    return (uint32_t)gba_mem_read32(g, 0x03007FFCu);
}

void gba_irq_dispatch(gba_t *g)
{
    gba_cpu *c = &g->cpu;
    uint32_t handler = irq_handler_addr(g);
    if (handler == 0u)
        return; /* game has not installed a handler: keep IRQ pending */

    c->bios_dispatch = 1;
    /* emulate the BIOS prologue: push {r0-r3,r12,lr} onto the IRQ stack */
    uint32_t sp = c->r[13];
    uint32_t lr = c->r[14];
    sp -= 4u;
    gba_mem_write32(g, sp, lr);
    sp -= 4u;
    gba_mem_write32(g, sp, c->r[12]);
    sp -= 4u;
    gba_mem_write32(g, sp, c->r[3]);
    sp -= 4u;
    gba_mem_write32(g, sp, c->r[2]);
    sp -= 4u;
    gba_mem_write32(g, sp, c->r[1]);
    sp -= 4u;
    gba_mem_write32(g, sp, c->r[0]);
    c->r[13] = sp;

    /* BIOS return sentinel: the game handler returns here */
    c->r[14] = 0x00000000u;

    /* enter the game handler in IRQ mode via the CPU (thumb bit honored) */
    c->cpsr = (c->cpsr & (uint32_t)~GBA_T) | (handler & 1u ? GBA_T : 0u);
    c->r[15] = handler & ~1u;
}

/* returns from the game handler to the interrupted context (called when the
 * CPU returns to the sentinel BIOS return address 0) */
void gba_irq_dispatch_return(gba_t *g)
{
    gba_cpu *c = &g->cpu;
    uint32_t sp = c->r[13];
    c->r[0] = gba_mem_read32(g, sp);
    c->r[1] = gba_mem_read32(g, sp + 4u);
    c->r[2] = gba_mem_read32(g, sp + 8u);
    c->r[3] = gba_mem_read32(g, sp + 12u);
    c->r[12] = gba_mem_read32(g, sp + 16u);
    c->r[14] = gba_mem_read32(g, sp + 20u);
    c->r[13] = sp + 24u;
    c->bios_dispatch = 0;
    /* return: SUBS pc, lr, #4 (CPSR restored from SPSR) */
    uint32_t spsr = gba_cpu_read_spsr(c);
    c->r[15] = c->r[14] - 4u;
    c->cpsr = spsr;
    gba_cpu_set_mode(c, (uint8_t)(spsr & 0x1Fu));
}

void gba_swi_hle(gba_t *g, uint32_t comment)
{
    gba_cpu *c = &g->cpu;

    /* returning from the emulated IRQ dispatcher: sentinel address 0 */
    if (c->bios_dispatch && (c->r[15] & 0x07FFFFFFu) == 0u) {
        irq_dispatch_return(g);
        return;
    }

    switch (comment) {
    case SWI_HALT:
        c->halted = 1;
        break;
    case SWI_VBLANKINTRWAIT:
        /* r1 = 1 (vblank flag); same as IntrWait(1, 1) */
        c->r[0] = 1;
        c->r[1] = 1;
        /* fallthrough */
    case SWI_INTRWAIT: {
        uint32_t flags = comment == SWI_VBLANKINTRWAIT ? 1u : c->r[1];
        if (c->r[0] != 0u)
            g->mem.if_reg &= (uint16_t)~flags; /* discard pending */
        if ((g->mem.if_reg & flags) == 0u) {
            c->intr_wait_active = 1;
            c->intr_wait_flags = flags;
            c->halted = 1;
        } else {
            g->mem.if_reg &= (uint16_t)~flags;
        }
        break;
    }
    case SWI_DIV: {
        int32_t num = (int32_t)c->r[0], den = (int32_t)c->r[1];
        if (den != 0) {
            c->r[0] = (uint32_t)(num / den);
            c->r[1] = (uint32_t)(num % den);
            c->r[3] = (uint32_t)(abs(den));
        }
        break;
    }
    case SWI_DIVARM: {
        int32_t num = (int32_t)c->r[1], den = (int32_t)c->r[0];
        if (den != 0) {
            c->r[0] = (uint32_t)(num / den);
            c->r[1] = (uint32_t)(num % den);
            c->r[3] = (uint32_t)(num);
        }
        break;
    }
    case SWI_SQRT: {
        uint32_t v = c->r[0];
        uint32_t r = 0, bit = 1u << 30;
        while (bit > v)
            bit >>= 2;
        while (bit != 0u) {
            if (v >= r + bit) {
                v -= r + bit;
                r = (r >> 1) + bit;
            } else {
                r >>= 1;
            }
            bit >>= 2;
        }
        c->r[0] = r;
        break;
    }
    case SWI_ARCTAN: {
        int32_t x = (int32_t)c->r[0] >> 16;
        int64_t a = (int64_t)x * x >> 14;
        int64_t a2 = a * (8192 - (a * (3632 - (a * 118)) >> 14)) >> 14;
        c->r[0] = (uint32_t)(int32_t)(((8192 * x) - (int32_t)(x * a2 >> 14)) >> 16);
        break;
    }
    case SWI_ARCTAN2: {
        int32_t x = (int32_t)(int16_t)(c->r[0] & 0xFFFFu);
        int32_t y = (int32_t)(int16_t)(c->r[1] & 0xFFFFu);
        if (y == 0) {
            c->r[0] = (x >= 0) ? 0x0000u : 0x8000u;
        } else if (x == 0) {
            c->r[0] = (y > 0) ? 0x4000u : 0xC000u;
        } else {
            int32_t phi;
            if (abs(y) > abs(x)) {
                int32_t t = x;
                x = y;
                y = t;
                phi = 0x4000u;
            } else {
                phi = 0;
            }
            int32_t sign = 1;
            if (x < 0) {
                x = -x;
                sign = -1;
            }
            if (y < 0)
                y = -y;
            int32_t mask = 0x4000u >> 0;
            (void)mask;
            int32_t arctan = (int32_t)(((int64_t)y * 0x4000u) / x);
            arctan = (int32_t)(((int64_t)arctan * 128) >> 14);
            c->r[0] = (uint32_t)((phi + sign * arctan) & 0xFFFFu);
        }
        break;
    }
    case SWI_CPUSET: {
        uint32_t src = c->r[0], dst = c->r[1];
        uint32_t cnt = c->r[2] & 0x1FFFFFu;
        uint32_t mode = c->r[2] >> 24;
        int fixed = (mode & 4u) != 0;
        int word = (mode & 3u) != 0;
        uint32_t step = word ? 4u : 2u;
        uint32_t v32 = 0;
        uint16_t v16 = 0;
        if (fixed) {
            if (word)
                v32 = c->r[3];
            else
                v16 = (uint16_t)(c->r[3] & 0xFFFFu);
        }
        for (uint32_t i = 0; i < cnt; i++) {
            if (!fixed) {
                if (word)
                    v32 = gba_mem_read32(g, src);
                else
                    v16 = gba_mem_read16(g, src);
            }
            if (word) {
                gba_mem_write32(g, dst, v32);
                if (!fixed)
                    src += 4u;
                dst += 4u;
            } else {
                gba_mem_write16(g, dst, v16);
                if (!fixed)
                    src += 2u;
                dst += 2u;
            }
            step = step; /* cycle accounting approximate */
        }
        break;
    }
    case SWI_CPUFASTSET: {
        uint32_t src = c->r[0], dst = c->r[1];
        uint32_t cnt = c->r[2] & 0x1FFFFFu;
        uint32_t mode = c->r[2] >> 24;
        int fixed = (mode & 4u) != 0;
        for (uint32_t i = 0; i < cnt; i += 8u) {
            uint32_t w0 = fixed ? c->r[3] : gba_mem_read32(g, src);
            uint32_t w1 = fixed ? c->r[3] : gba_mem_read32(g, src + 4u);
            gba_mem_write32(g, dst, w0);
            gba_mem_write32(g, dst + 4u, w1);
            if (!fixed)
                src += 8u;
            dst += 8u;
        }
        break;
    }
    case SWI_REGISTERRAMRESET: {
        uint32_t flags = c->r[0];
        if (flags & 0x01u)
            memset(g->mem.ewram, 0, sizeof g->mem.ewram);
        if (flags & 0x02u)
            memset(g->mem.iwram, 0, sizeof g->mem.iwram);
        if (flags & 0x04u)
            memset(g->mem.pal, 0, sizeof g->mem.pal);
        if (flags & 0x08u)
            memset(g->mem.vram, 0, sizeof g->mem.vram);
        if (flags & 0x10u)
            memset(g->mem.oam, 0, sizeof g->mem.oam);
        break;
    }
    case SWI_SOFTRESET:
        gba_cpu_power_on(c);
        break;
    case SWI_GETBIOSCHECKSUM:
        c->r[0] = 0xBAAE187Fu; /* real BIOS checksum; games may poll it */
        break;
    default:
        c->r[0] = 0; /* unimplemented services return 0 (documented) */
        break;
    }
}
