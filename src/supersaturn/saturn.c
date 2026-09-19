/*
 * supersaturn: Sega Saturn machine implementation. See saturn.h for the
 * verified scope, sources and documented simplifications.
 */
#include "saturn.h"

#include <stdlib.h>
#include <string.h>

#include "../common/util.h"

/* forward declarations (bus writes dispatch into SCU/SMPC below) */
static void scu_dma_start(struct saturn *s, int level);
static int scu_dma_load_indirect(struct saturn *s, int level);

/* ---- bus ------------------------------------------------------------------------ */

static uint8_t *sat_ram_ptr(struct saturn *s, uint32_t addr)
{
    if (addr >= 0x00200000u && addr < 0x00300000u)
        return &s->wraml[addr - 0x00200000u];
    if (addr >= 0x06000000u && addr < 0x06100000u)
        return &s->wramh[addr - 0x06000000u];
    if (addr >= 0x00180000u && addr < 0x00190000u)
        return &s->backup[addr - 0x00180000u];
    if (addr >= 0x05A00000u && addr < 0x05A80000u)
        return &s->soundram[addr - 0x05A00000u];
    if (addr >= 0x05C00000u && addr < 0x05C80000u)
        return &s->vdp1_vram[addr - 0x05C00000u];
    if (addr >= 0x05E00000u && addr < 0x05E80000u)
        return &s->vdp2_vram[addr - 0x05E00000u];
    if (addr >= 0x05F00000u && addr < 0x05F01000u)
        return &s->vdp2_cram[addr - 0x05F00000u];
    return NULL;
}

uint8_t sat_bus_read8(struct saturn *s, uint32_t addr)
{
    addr &= 0x07FFFFFFu;
    uint8_t *p = sat_ram_ptr(s, addr);
    if (p != NULL)
        return *p;
    if (addr >= 0x00100000u && addr < 0x00100080u) {
        /* SMPC: byte registers at odd offsets (ST-169 figure 1.3). */
        uint32_t o = addr - 0x00100000u;
        switch (o) {
        case 0x21: case 0x23: case 0x25: case 0x27:
        case 0x29: case 0x2B: case 0x2D: case 0x2F:
        case 0x31: case 0x33: case 0x35: case 0x37:
        case 0x39: case 0x3B: case 0x3D: case 0x3F:
        case 0x41: case 0x43: case 0x45: case 0x47:
        case 0x49: case 0x4B: case 0x4D: case 0x4F:
        case 0x51: case 0x53: case 0x55: case 0x57:
        case 0x59: case 0x5B: case 0x5D: case 0x5F:
            return s->smpc_oreg[(o - 0x21u) >> 1];
        case 0x63:
            return s->smpc_sf;
        case 0x61:
            return 0x80u; /* RESB = 1; other status bits not modeled   */
        default:
            return 0xFFu;
        }
    }
    if (addr >= 0x05C80000u && addr < 0x05CC0000u) {
        /* VDP1 frame buffer: only the drawing screen is CPU-accessible
         * (VDP1 manual figure 2.1 note *2). */
        uint32_t o = (addr - 0x05C80000u) >> 1;
        return (uint8_t)((addr & 1u) ? (s->vdp1_fb[s->vdp1_draw_sel][o] & 0xFFu)
                                     : (s->vdp1_fb[s->vdp1_draw_sel][o] >> 8));
    }
    if (addr >= 0x05D00000u && addr < 0x05D00018u) {
        uint16_t v = sat_vdp1_reg_read(s, (addr - 0x05D00000u) >> 1);
        return (uint8_t)((addr & 1u) ? v : (v >> 8));
    }
    if (addr >= 0x05F80000u && addr < 0x05F80120u)
        return s->vdp2_reg[addr - 0x05F80000u];
    if (addr >= 0x05FE0000u && addr < 0x05FE00D0u) {
        uint32_t o = addr - 0x05FE0000u;
        uint32_t v = s->scu[o >> 2];
        return (uint8_t)(v >> (8u * (3u - (o & 3u))));
    }
    if (addr >= 0x01000000u && addr < 0x01000008u)
        return 0; /* MINIT */
    if (addr >= 0x01800000u && addr < 0x01800008u)
        return 0; /* SINIT */
    /* IPL ROM region, A-bus (incl. CD block), SCSP registers, mirrors:
     * open bus (documented). */
    return 0xFFu;
}

uint16_t sat_bus_read16(struct saturn *s, uint32_t addr)
{
    addr &= 0x07FFFFFFu;
    uint16_t hi = (uint16_t)(sat_bus_read8(s, addr & ~1u) << 8);
    uint16_t lo = sat_bus_read8(s, addr | 1u);
    return (uint16_t)(hi | lo);
}

uint32_t sat_bus_read32(struct saturn *s, uint32_t addr)
{
    uint32_t v = (uint32_t)sat_bus_read16(s, addr & ~3u) << 16;
    v |= sat_bus_read16(s, (addr & ~3u) + 2u);
    return v;
}

void sat_bus_write8(struct saturn *s, uint32_t addr, uint8_t v)
{
    addr &= 0x07FFFFFFu;
    uint8_t *p = sat_ram_ptr(s, addr);
    if (p != NULL) {
        *p = v;
        return;
    }
    if (addr >= 0x00100000u && addr < 0x00100080u) {
        uint32_t o = addr - 0x00100000u;
        if (o == 0x1Fu) {
            /* COMREG: SMPC command dispatch (ST-169 section 2). */
            s->smpc_sf = 1u;
            smpc_command(s, v);
            return;
        }
        if (o >= 0x01u && o <= 0x0Du && (o & 1u) != 0u) {
            s->smpc_ireg[o >> 1] = v;
            return;
        }
        return;
    }
    if (addr >= 0x05C80000u && addr < 0x05CC0000u) {
        uint32_t o = (addr - 0x05C80000u) >> 1;
        if (addr & 1u)
            s->vdp1_fb[s->vdp1_draw_sel][o] =
                (uint16_t)((s->vdp1_fb[s->vdp1_draw_sel][o] & 0xFF00u) | v);
        else
            s->vdp1_fb[s->vdp1_draw_sel][o] =
                (uint16_t)((s->vdp1_fb[s->vdp1_draw_sel][o] & 0x00FFu) |
                           (uint16_t)(v << 8));
        return;
    }
    if (addr >= 0x05D00000u && addr < 0x05D00018u) {
        uint32_t i = (addr - 0x05D00000u) >> 1;
        uint16_t cur = s->vdp1_reg[i];
        uint16_t nv = (uint16_t)((addr & 1u) ? ((cur & 0xFF00u) | v)
                                             : ((cur & 0x00FFu) |
                                                (uint16_t)(v << 8)));
        sat_vdp1_reg_write(s, i, nv);
        return;
    }
    if (addr >= 0x05F80000u && addr < 0x05F80120u) {
        s->vdp2_reg[addr - 0x05F80000u] = v;
        return;
    }
    if (addr >= 0x05FE0000u && addr < 0x05FE00D0u) {
        uint32_t o = addr - 0x05FE0000u;
        uint32_t sh = 8u * (3u - (o & 3u));
        uint32_t mask = 0xFFu << sh;
        uint32_t oldv = s->scu[o >> 2];
        s->scu[o >> 2] = (oldv & ~mask) | ((uint32_t)v << sh);
        scu_reg_write(s, o & ~3u, oldv);
        return;
    }
    if (addr == 0x01800004u) {
        /* SINIT: release the slave SH-2 (SCU map region 0x01800004;
         * boot model documented in saturn.h). */
        if (s->disc != NULL && !s->slave_on) {
            s->slave_on = 1;
            sh2_reset(&s->ssh2);
            s->ssh2.r[15] = s->boot_sp_s;
            s->ssh2.pc = 0x06002000u;
            s->ssh2.next_pc = s->ssh2.pc + 2u;
            s->ssh2.sr = 0; /* interrupts enabled */
            s->ssh2.vbr = 0x06000000u;
        }
        return;
    }
    if (addr == 0x01000004u)
        return; /* MINIT: no distinct master boot action needed */
    /* writes to open-bus regions are dropped (documented) */
}

void sat_bus_write16(struct saturn *s, uint32_t addr, uint16_t v)
{
    addr &= 0x07FFFFFFu;
    if (addr >= 0x05D00000u && addr < 0x05D00018u) {
        sat_vdp1_reg_write(s, (addr - 0x05D00000u) >> 1, v);
        return;
    }
    if (addr >= 0x05F80000u && addr < 0x05F80120u) {
        uint32_t o = addr - 0x05F80000u;
        s->vdp2_reg[o] = (uint8_t)(v >> 8);
        if (o + 1u < sizeof s->vdp2_reg)
            s->vdp2_reg[o + 1u] = (uint8_t)v;
        return;
    }
    if (addr >= 0x05FE0000u && addr < 0x05FE00D0u) {
        uint32_t o = addr - 0x05FE0000u;
        uint32_t sh = 8u * (2u - (o & 2u));
        uint32_t mask = 0xFFFFu << sh;
        uint32_t oldv = s->scu[o >> 2];
        s->scu[o >> 2] = (oldv & ~mask) | ((uint32_t)v << sh);
        scu_reg_write(s, o & ~3u, oldv);
        return;
    }
    if (addr >= 0x00100000u && addr < 0x00100080u) {
        sat_bus_write8(s, addr | 1u, (uint8_t)v); /* SMPC: byte access */
        return;
    }
    sat_bus_write8(s, addr & ~1u, (uint8_t)(v >> 8));
    sat_bus_write8(s, addr | 1u, (uint8_t)v);
}

void sat_bus_write32(struct saturn *s, uint32_t addr, uint32_t v)
{
    sat_bus_write16(s, addr & ~3u, (uint16_t)(v >> 16));
    sat_bus_write16(s, (addr & ~3u) + 2u, (uint16_t)v);
}

/* ---- SCU ------------------------------------------------------------------------- */

/* Deliver one interrupt factor to both CPUs. IMS semantics: bit set =
 * masked (interrupt disabled). Vector/level pairs: SCU User's Manual
 * chapter 3 interrupt table. */
void sat_scu_send(struct saturn *s, uint32_t ist_bit, uint32_t vector,
                  uint32_t level)
{
    if (s->scu[SAT_SCU_IST >> 2] & ist_bit)
        return; /* already pending */
    s->scu[SAT_SCU_IST >> 2] |= ist_bit;
    if (s->scu[SAT_SCU_IMS >> 2] & ist_bit)
        return; /* masked */
    uint32_t lv = level & 0xFu;
    if (lv == 0)
        return;
    sh2_irq_vector(&s->msh2, (int)lv, vector);
    if (s->slave_on)
        sh2_irq_vector(&s->ssh2, (int)lv, vector);
}

/* SCU register write dispatch. Called AFTER the value is merged into the
 * register file, with the pre-write word snapshot for edge detection. */
void scu_reg_write(struct saturn *s, uint32_t off, uint32_t old)
{
    uint32_t idx = off >> 2;
    uint32_t newv = s->scu[idx];

    if (off == SAT_SCU_IST) {
        /* writing 0 to a status bit clears it; writing 1 keeps it */
        s->scu[idx] = old & newv;
        return;
    }
    if (off == SAT_SCU_T0C || off == SAT_SCU_T1S) {
        if ((old & 0xFFu) != (newv & 0xFFu))
            s->timer0 = 0, s->timer1 = 0;
        return;
    }

    /* DMA set registers: three levels sharing one layout, 0x20 apart. */
    if (off < SAT_SCU_D0R + 0x60u) {
        uint32_t level = (off - SAT_SCU_D0R) / 0x20u;
        uint32_t md = s->scu[(SAT_SCU_D0R + 0x20u * level + 0x14u) >> 2];
        if (off == SAT_SCU_D0R + 0x20u * level + 0x10u) {
            /* DlEN bit 0 (enable) rising edge: start when the programmed
             * start factor is 7, the "DMA starting bit" option (SCU
             * manual section 2). */
            if (!(old & 1u) && (newv & 1u) && (md & 7u) == 7u)
                scu_dma_start(s, (int)level);
        }
        if (off == SAT_SCU_D0R + 0x20u * level + 0x14u) {
            /* DlMD start factor set to 7 while enabled: start now. */
            if ((newv & 7u) == 7u && (old & 7u) != 7u &&
                (s->scu[(SAT_SCU_D0R + 0x20u * level + 0x10u) >> 2] & 1u))
                scu_dma_start(s, (int)level);
        }
    }
}

/* Start (or restart) one DMA level: latch the working set. */
static void scu_dma_start(struct saturn *s, int level)
{
    uint32_t base = SAT_SCU_D0MD - 0x14u + 0x20u * (uint32_t)level;
    uint32_t en = s->scu[(base + 0x10u) >> 2];
    if ((en & 1u) == 0u)
        return; /* DMA enable bit clear */

    struct saturn_dma_state *d = &s->dma[level];
    uint32_t md = s->scu[(base + 0x14u) >> 2];
    d->indirect = (int)((md >> 24) & 1u);
    d->active = 1;
    if (d->indirect) {
        d->itable = s->scu[(base + 0x04u) >> 2] & 0x07FFFFFFu;
        scu_dma_load_indirect(s, level);
    } else {
        d->raddr = s->scu[base >> 2] & 0x07FFFFFFu;
        d->waddr = s->scu[(base + 0x04u) >> 2] & 0x07FFFFFFu;
        d->count = s->scu[(base + 0x08u) >> 2];
        if (level == 0)
            d->count &= 0x000FFFFFu; /* D0C bits 19-0: up to 1 MB */
        else
            d->count &= 0x00000FFFu; /* D1C/D2C bits 11-0: up to 4 KB */
    }
}

/* Fetch one indirect-mode header entry (3 longwords: count, write
 * address, read address with bit 31 as the end-of-list flag - SCU manual
 * section 2 "end code"; the flagged entry still executes). */
static int scu_dma_load_indirect(struct saturn *s, int level)
{
    struct saturn_dma_state *d = &s->dma[level];
    uint32_t count = sat_bus_read32(s, d->itable);
    uint32_t waddr = sat_bus_read32(s, d->itable + 4u);
    uint32_t rword = sat_bus_read32(s, d->itable + 8u);
    d->last = (int)((rword >> 31) & 1u);
    d->raddr = rword & 0x07FFFFFFu;
    d->waddr = waddr & 0x07FFFFFFu;
    d->count = count & (level == 0 ? 0x000FFFFFu : 0x00000FFFu);
    d->itable += 0xCu;
    return 1;
}

/* Progress active DMA transfers (called once per scanline; a transfer
 * bounded by its byte count runs to completion - documented
 * simplification: transfers are not preempted mid-line).
 *
 * Transfer unit: one 32-bit read, written as two 16-bit words. The write
 * address add value applies per 16-bit write (manual figure 3.5: encoded
 * 0/2/4/8/16/32/64/128 bytes; the reset value DxAD = 101H = one word per
 * write). The read address always steps 4 bytes except inside the A-bus
 * CS2 window (CD block), where DxAD bit 8 selects 2- or 4-byte steps
 * (manual table 3.2: the read add bit is only effective for CS2). */
static void scu_dma_tick(struct saturn *s)
{
    static const uint32_t wtab[8] = { 0, 2, 4, 8, 16, 32, 64, 128 };
    static const uint32_t endbit[3] = { 0x00000800u, 0x00000400u,
                                        0x00000200u };
    for (int level = 0; level < 3; level++) {
        struct saturn_dma_state *d = &s->dma[level];
        uint32_t base = SAT_SCU_D0R + 0x20u * (uint32_t)level;
        for (int guard = 0; guard < 4096; guard++) {
            if (!d->active || d->count == 0u)
                break;
            uint32_t ad = s->scu[(base + 0x0Cu) >> 2];
            uint32_t md = s->scu[(base + 0x14u) >> 2];
            uint32_t ra = d->raddr & 0x07FFFFFFu;
            uint32_t rstep =
                ((ra & 0xFFF00000u) == 0x05800000u && !(ad & 0x100u)) ? 2u
                                                                      : 4u;
            uint32_t wstep = wtab[ad & 7u] * 2u; /* 2 words per unit */
            uint32_t rup = (md >> 16) & 1u;
            uint32_t wup = (md >> 8) & 1u;

            while (d->count > 0u) {
                uint32_t v = sat_bus_read32(s, d->raddr);
                sat_bus_write16(s, d->waddr, (uint16_t)(v >> 16));
                if (wstep != 0u)
                    sat_bus_write16(s, d->waddr + (wstep >> 1),
                                    (uint16_t)v);
                d->raddr += rstep;
                d->waddr += wstep;
                d->count = (d->count >= 4u) ? d->count - 4u : 0u;
            }

            /* address update bits: 0 = reload the register from the
             * working address, 1 = keep the advanced address (figure 2.7) */
            if (!rup)
                s->scu[base >> 2] = d->raddr & 0x07FFFFFFu;
            if (!wup)
                s->scu[(base + 0x04u) >> 2] = d->waddr & 0x07FFFFFFu;

            if (d->indirect && !d->last && scu_dma_load_indirect(s, level))
                continue; /* next list entry, same level */

            /* transfer list complete: drop enable, report end interrupt
             * (level 2 = bit 9/vector 0x49, level 1 = bit 10/vector 0x4A,
             * level 0 = bit 11/vector 0x4B - SCU manual chapter 3) */
            s->scu[(base + 0x10u) >> 2] &= ~1u;
            d->active = 0;
            sat_scu_send(s, endbit[level], 0x4Bu - (uint32_t)level,
                         level == 0 ? 5u : 6u);
            break;
        }
    }
}

/* SCU timers: timer 0 counts lines (compare = interrupt period),
 * timer 1 counts timer 0 matches (SCU manual figures 2.12/2.13). */
static void scu_timer_tick(struct saturn *s)
{
    uint16_t t0c = (uint16_t)(s->scu[SAT_SCU_T0C >> 2] & 0xFFu);
    uint16_t t1s = (uint16_t)(s->scu[SAT_SCU_T1S >> 2] & 0xFFu);
    uint16_t t1md = (uint16_t)(s->scu[SAT_SCU_T1MD >> 2] & 0xFFu);

    if (t0c != 0u) {
        s->timer0++;
        if (s->timer0 >= t0c) {
            s->timer0 = 0;
            sat_scu_send(s, SAT_IST_TIMER0, 0x43u, 0xCu);
            if ((t1md & 0x01u) != 0u) { /* timer 1 enabled, sync mode */
                s->timer1++;
                if (t1s != 0u && s->timer1 >= t1s) {
                    s->timer1 = 0;
                    sat_scu_send(s, SAT_IST_TIMER1, 0x44u, 0xBu);
                }
            }
        }
    }
}

/* ---- SMPC ------------------------------------------------------------------------ */

static void smpc_intback(struct saturn *s)
{
    /* INTBACK (COMREG 0x0D): peripheral data reports in OREG0.. (SMPC
     * manual section 2; layout of the standard digital-pad report). */
    s->smpc_oreg[0] = 0xF1u; /* 1 peripheral, standard data format       */
    s->smpc_oreg[1] = 0x02u; /* Saturn digital pad peripheral ID         */
    s->smpc_oreg[2] = 0x00u; /* no additional data                       */
    /* Button bytes are active-low on hardware (0 = pressed). Input mask
     * layout is documented in emu/emu.h and the README. */
    uint32_t b = s->buttons;
    uint8_t hi = 0xFFu;
    if (b & SAT_BTN_R)
        hi &= ~0x80u;
    if (b & SAT_BTN_L)
        hi &= ~0x40u;
    if (b & SAT_BTN_DOWN)
        hi &= ~0x20u;
    if (b & SAT_BTN_UP)
        hi &= ~0x10u;
    if (b & SAT_BTN_START)
        hi &= ~0x08u;
    if (b & SAT_BTN_A)
        hi &= ~0x04u;
    if (b & SAT_BTN_B)
        hi &= ~0x02u;
    if (b & SAT_BTN_C)
        hi &= ~0x01u;
    s->smpc_oreg[3] = hi;
    s->smpc_oreg[4] = 0x8Fu; /* second pad byte (released state)         */
    s->smpc_oreg[5] = 0x00u;
    s->smpc_oreg[6] = 0x00u;
    s->smpc_oreg[7] = 0x00u; /* RTC time bytes not modeled (documented)  */
    s->smpc_sf = 0u; /* command complete */
}

void smpc_command(struct saturn *s, uint8_t cmd)
{
    switch (cmd) {
    case 0x0Du: /* INTBACK */
        smpc_intback(s);
        break;
    case 0x0Fu: /* INTBACK continuation: re-deliver pad bytes           */
        smpc_pad_data(s);
        s->smpc_sf = 0u;
        break;
    default:
        /* Other system-management commands (RTC, standby, sound enable,
         * ...) are accepted and completed without effect (documented). */
        s->smpc_sf = 0u;
        break;
    }
    /* The System Manager interrupt (SCU bit 7, vector 0x47, level 8)
     * accompanies SMPC command completion. */
    sat_scu_send(s, 0x00000080u, 0x47u, 8u);
}

void smpc_pad_data(struct saturn *s)
{
    /* Continuation reports refresh OREG0-7 from the live button state. */
    uint32_t b = s->buttons;
    uint8_t hi = 0xFFu;
    if (b & SAT_BTN_R)
        hi &= ~0x80u;
    if (b & SAT_BTN_L)
        hi &= ~0x40u;
    if (b & SAT_BTN_DOWN)
        hi &= ~0x20u;
    if (b & SAT_BTN_UP)
        hi &= ~0x10u;
    if (b & SAT_BTN_START)
        hi &= ~0x08u;
    if (b & SAT_BTN_A)
        hi &= ~0x04u;
    if (b & SAT_BTN_B)
        hi &= ~0x02u;
    if (b & SAT_BTN_C)
        hi &= ~0x01u;
    s->smpc_oreg[3] = hi;
}

/* ---- SH-2 bus adapters ------------------------------------------------------------ */

static uint8_t sh2_r8(void *user, uint32_t a)
{
    return sat_bus_read8((struct saturn *)user, a);
}
static uint16_t sh2_r16(void *user, uint32_t a)
{
    return sat_bus_read16((struct saturn *)user, a);
}
static uint32_t sh2_r32(void *user, uint32_t a)
{
    return sat_bus_read32((struct saturn *)user, a);
}
static void sh2_w8(void *user, uint32_t a, uint8_t v)
{
    sat_bus_write8((struct saturn *)user, a, v);
}
static void sh2_w16(void *user, uint32_t a, uint16_t v)
{
    sat_bus_write16((struct saturn *)user, a, v);
}
static void sh2_w32(void *user, uint32_t a, uint32_t v)
{
    sat_bus_write32((struct saturn *)user, a, v);
}

/* ---- boot (documented model; see saturn.h) ---------------------------------------- */

static void saturn_boot(struct saturn *s)
{
    const uint8_t *d = s->disc;
    size_t n = s->disc_size;
    if (n < 0x100u)
        return;

    uint32_t ip_size = ((uint32_t)d[0xE0] << 24) | ((uint32_t)d[0xE1] << 16) |
                       ((uint32_t)d[0xE2] << 8) | d[0xE3];
    uint32_t sp_m = ((uint32_t)d[0xE8] << 24) | ((uint32_t)d[0xE9] << 16) |
                    ((uint32_t)d[0xEA] << 8) | d[0xEB];
    uint32_t sp_s = ((uint32_t)d[0xEC] << 24) | ((uint32_t)d[0xED] << 16) |
                    ((uint32_t)d[0xEE] << 8) | d[0xEF];

    /* AIP starts at file offset 0x800 (System ID + security + area codes
     * precede it; documented constant). Range: 0x1000-0x8000 per the
     * disc format spec section 4.3 (IP SIZE). */
    uint32_t aip = ip_size;
    if (aip < 0x1000u || aip > 0x8000u)
        aip = 0x7800u; /* conservative fallback: 30 KiB program area   */
    if (aip > n - 0x800u)
        aip = (uint32_t)(n - 0x800u);

    uint32_t dst = 0x06002000u;
    for (uint32_t i = 0; i < aip; i++) {
        uint32_t a = dst + i;
        if (a < 0x06100000u)
            s->wramh[a - 0x06000000u] = d[0x800u + i];
    }

    s->boot_sp_m = (sp_m != 0u) ? sp_m : 0x06002000u;
    s->boot_sp_s = (sp_s != 0u) ? sp_s : 0x06001000u;

    sh2_reset(&s->msh2);
    s->msh2.r[15] = s->boot_sp_m;
    s->msh2.pc = dst;
    s->msh2.next_pc = dst + 2u;
    s->msh2.sr = 0; /* interrupts enabled; code unmasks as needed        */
    /* Boot model: VBR = 0x06000000 so interrupt vectors (VBR+0x600+
     * vector*4, SCU vectors 0x40-0x5F) live in Work RAM-H; programs
     * install their handlers there. */
    s->msh2.vbr = 0x06000000u;
    s->slave_on = 0;
    /* No IPL/BIOS runs ahead of the program, so every SCU interrupt
     * starts masked (IMS bit = 1); programs unmask the factors they use
     * - mirrors the BIOS practice on hardware. */
    s->scu[SAT_SCU_IMS >> 2] = 0xFFFFFFFFu;
    s->scu[SAT_SCU_IST >> 2] = 0;
}

/* ---- core lifecycle ---------------------------------------------------------------- */

static emu_result_t saturn_validate(const uint8_t *d, size_t n)
{
    if (n < 0x800u)
        return EMU_EBADROM;
    if (memcmp(d, "SEGA SEGASATURN", 15) != 0)
        return EMU_EBADROM;
    return EMU_OK;
}

static emu_result_t saturn_load_rom(emu_core_t *core, const uint8_t *data,
                                    size_t size)
{
    struct saturn *s = (struct saturn *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    emu_result_t r = saturn_validate(data, size);
    if (r != EMU_OK)
        return r;
    uint8_t *copy = malloc(size);
    if (copy == NULL)
        return EMU_EINVAL;
    memcpy(copy, data, size);
    free(s->disc);
    s->disc = copy;
    s->disc_size = size;
    saturn_boot(s);
    return EMU_OK;
}

static void saturn_reset(emu_core_t *core)
{
    struct saturn *s = (struct saturn *)core;
    memset(s->wraml, 0, sizeof s->wraml);
    memset(s->wramh, 0, sizeof s->wramh);
    memset(s->backup, 0, sizeof s->backup);
    memset(s->soundram, 0, sizeof s->soundram);
    memset(s->vdp1_vram, 0, sizeof s->vdp1_vram);
    memset(s->vdp1_fb, 0, sizeof s->vdp1_fb);
    memset(s->vdp1_reg, 0, sizeof s->vdp1_reg);
    /* default system clipping = full frame buffer (registers are
     * undefined at power-on per the manual; full-screen keeps draws
     * visible before a program sets clipping) */
    s->sys_clip[0] = SAT_FB_W - 1u;
    s->sys_clip[1] = SAT_FB_H - 1u;
    memset(s->vdp2_vram, 0, sizeof s->vdp2_vram);
    memset(s->vdp2_cram, 0, sizeof s->vdp2_cram);
    memset(s->vdp2_reg, 0, sizeof s->vdp2_reg);
    memset(s->scu, 0, sizeof s->scu);
    memset(s->smpc_oreg, 0, sizeof s->smpc_oreg);
    s->vdp1_draw_sel = 0;
    s->vdp1_ptmr_run = 0;
    s->slave_on = 0;
    s->timer0 = 0;
    s->timer1 = 0;
    s->scu[SAT_SCU_IMS >> 2] = 0xFFFFFFFFu;
    s->scu[SAT_SCU_IST >> 2] = 0;
    sh2_reset(&s->msh2);
    sh2_reset(&s->ssh2);
    if (s->disc != NULL)
        saturn_boot(s);
}

void sat_line_tick(struct saturn *s)
{
    uint32_t budget = SAT_CYCLES_PER_LINE;
    while (budget > 0u) {
        uint32_t used = sh2_step(&s->msh2);
        budget -= (used > budget) ? budget : used;
    }
    if (s->slave_on) {
        budget = SAT_CYCLES_PER_LINE;
        while (budget > 0u) {
            uint32_t used = sh2_step(&s->ssh2);
            budget -= (used > budget) ? budget : used;
        }
    }
    scu_dma_tick(s);
}

static emu_result_t saturn_run_frame(emu_core_t *core)
{
    struct saturn *s = (struct saturn *)core;
    if (s->disc == NULL)
        return EMU_ENOROM;

    for (uint32_t line = 0; line < SAT_LINES; line++) {
        if (line == 0u)
            sat_scu_send(s, SAT_IST_VBLANKOUT, 0x41u, 0xEu);
        if (line == 224u)
            sat_scu_send(s, SAT_IST_VBLANKIN, 0x40u, 0xFu);
        sat_scu_send(s, SAT_IST_HBLANKIN, 0x42u, 0xDu);
        sat_line_tick(s);
        scu_timer_tick(s);
        if (line == 0u && (s->vdp1_reg[SAT_VDP1_PTMR] & 3u) == 2u)
            sat_vdp1_execute(s);
    }

    sat_vdp1_frame_change(s);
    sat_render_output(s);
    s->frame_count++;
    return EMU_OK;
}

static const uint32_t *saturn_fb(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct saturn *s = (struct saturn *)core;
    if (w != NULL)
        *w = SAT_SCREEN_W;
    if (h != NULL)
        *h = SAT_SCREEN_H;
    return s->fb;
}

static void saturn_set_input(emu_core_t *core, uint32_t buttons)
{
    ((struct saturn *)core)->buttons = buttons;
}

static void saturn_set_audio(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    (void)core;
    (void)cb;
    (void)user; /* SCSP not implemented: core produces no audio */
}

/* ---- save states ------------------------------------------------------------------ */

static void saturn_serialize(struct saturn *s, emu_state_writer *w)
{
    sw_u32(w, 0x53415455u); /* "SATU" */
    sw_u32(w, s->slave_on);
    sw_u32(w, s->vdp1_draw_sel);
    sw_u32(w, s->vdp1_ptmr_run);
    sw_u32(w, s->frame_count);
    sw_u16(w, (uint16_t)s->timer0);
    sw_u16(w, (uint16_t)s->timer1);
    for (int i = 0; i < 16; i++)
        sw_u16(w, s->vdp1_reg[i]);
    for (int i = 0; i < 4; i++)
        sw_u16(w, s->user_clip[i]);
    sw_u16(w, s->sys_clip[0]);
    sw_u16(w, s->sys_clip[1]);
    sw_u32(w, (uint32_t)s->local_x);
    sw_u32(w, (uint32_t)s->local_y);
    sw_mem(w, s->scu, sizeof s->scu);
    sw_mem(w, s->smpc_ireg, sizeof s->smpc_ireg);
    sw_mem(w, s->smpc_oreg, sizeof s->smpc_oreg);
    sw_u8(w, s->smpc_sf);
    for (int i = 0; i < 3; i++) {
        sw_u32(w, s->dma[i].raddr);
        sw_u32(w, s->dma[i].waddr);
        sw_u32(w, s->dma[i].count);
        sw_u32(w, s->dma[i].itable);
        sw_u32(w, (uint32_t)s->dma[i].active);
        sw_u32(w, (uint32_t)s->dma[i].indirect);
    }
    /* CPUs */
    sh2_t *cpus[2] = { &s->msh2, &s->ssh2 };
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 16; i++)
            sw_u32(w, cpus[c]->r[i]);
        sw_u32(w, cpus[c]->pc);
        sw_u32(w, cpus[c]->next_pc);
        sw_u32(w, cpus[c]->pr);
        sw_u32(w, cpus[c]->sr);
        sw_u32(w, cpus[c]->gbr);
        sw_u32(w, cpus[c]->vbr);
        sw_u32(w, cpus[c]->mach);
        sw_u32(w, cpus[c]->macl);
    }
    sw_mem(w, s->wraml, sizeof s->wraml);
    sw_mem(w, s->wramh, sizeof s->wramh);
    sw_mem(w, s->backup, sizeof s->backup);
    sw_mem(w, s->soundram, sizeof s->soundram);
    sw_mem(w, s->vdp1_vram, sizeof s->vdp1_vram);
    sw_mem(w, s->vdp1_fb, sizeof s->vdp1_fb);
    sw_mem(w, s->vdp2_vram, sizeof s->vdp2_vram);
    sw_mem(w, s->vdp2_cram, sizeof s->vdp2_cram);
    sw_mem(w, s->vdp2_reg, sizeof s->vdp2_reg);
}

static size_t saturn_state_size(emu_core_t *core)
{
    struct saturn *s = (struct saturn *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    saturn_serialize(s, &w);
    return w.pos;
}

static emu_result_t saturn_save_state(emu_core_t *core, uint8_t *buf,
                                      size_t cap)
{
    struct saturn *s = (struct saturn *)core;
    emu_state_writer w = { buf, cap, 0, 0 };
    saturn_serialize(s, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t saturn_load_state(emu_core_t *core, const uint8_t *buf,
                                      size_t size)
{
    struct saturn *s = (struct saturn *)core;
    emu_state_reader rd = { buf, size, 0, 0 };
    if (sr_u32(&rd) != 0x53415455u)
        return EMU_EBADSTATE;
    s->slave_on = (int)sr_u32(&rd);
    s->vdp1_draw_sel = (uint8_t)sr_u32(&rd);
    s->vdp1_ptmr_run = (uint8_t)sr_u32(&rd);
    s->frame_count = sr_u32(&rd);
    s->timer0 = sr_u16(&rd);
    s->timer1 = sr_u16(&rd);
    for (int i = 0; i < 16; i++)
        s->vdp1_reg[i] = sr_u16(&rd);
    for (int i = 0; i < 4; i++)
        s->user_clip[i] = sr_u16(&rd);
    s->sys_clip[0] = sr_u16(&rd);
    s->sys_clip[1] = sr_u16(&rd);
    s->local_x = (int32_t)sr_u32(&rd);
    s->local_y = (int32_t)sr_u32(&rd);
    sr_mem(&rd, s->scu, sizeof s->scu);
    sr_mem(&rd, s->smpc_ireg, sizeof s->smpc_ireg);
    sr_mem(&rd, s->smpc_oreg, sizeof s->smpc_oreg);
    s->smpc_sf = sr_u8(&rd);
    for (int i = 0; i < 3; i++) {
        s->dma[i].raddr = sr_u32(&rd);
        s->dma[i].waddr = sr_u32(&rd);
        s->dma[i].count = sr_u32(&rd);
        s->dma[i].itable = sr_u32(&rd);
        s->dma[i].active = (int)sr_u32(&rd);
        s->dma[i].indirect = (int)sr_u32(&rd);
    }
    sh2_t *cpus[2] = { &s->msh2, &s->ssh2 };
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 16; i++)
            cpus[c]->r[i] = sr_u32(&rd);
        cpus[c]->pc = sr_u32(&rd);
        cpus[c]->next_pc = sr_u32(&rd);
        cpus[c]->pr = sr_u32(&rd);
        cpus[c]->sr = sr_u32(&rd);
        cpus[c]->gbr = sr_u32(&rd);
        cpus[c]->vbr = sr_u32(&rd);
        cpus[c]->mach = sr_u32(&rd);
        cpus[c]->macl = sr_u32(&rd);
    }
    sr_mem(&rd, s->wraml, sizeof s->wraml);
    sr_mem(&rd, s->wramh, sizeof s->wramh);
    sr_mem(&rd, s->backup, sizeof s->backup);
    sr_mem(&rd, s->soundram, sizeof s->soundram);
    sr_mem(&rd, s->vdp1_vram, sizeof s->vdp1_vram);
    sr_mem(&rd, s->vdp1_fb, sizeof s->vdp1_fb);
    sr_mem(&rd, s->vdp2_vram, sizeof s->vdp2_vram);
    sr_mem(&rd, s->vdp2_cram, sizeof s->vdp2_cram);
    sr_mem(&rd, s->vdp2_reg, sizeof s->vdp2_reg);
    if (rd.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

/* ---- vtable ------------------------------------------------------------------------ */

static emu_result_t saturn_create(emu_core_t **out);
static void saturn_destroy(emu_core_t *core);

static const emu_core_vtable_t saturn_vtable = {
    "supersaturn", "Sega Saturn", SAT_SCREEN_W, SAT_SCREEN_H, SAT_OUT_RATE,
    saturn_create, saturn_destroy, saturn_load_rom, saturn_reset,
    saturn_run_frame, saturn_fb, saturn_set_input, saturn_set_audio,
    saturn_state_size, saturn_save_state, saturn_load_state,
};

static emu_result_t saturn_create(emu_core_t **out)
{
    struct saturn *s = calloc(1, sizeof *s);
    if (s == NULL)
        return EMU_EINVAL;
    s->base.vtable = &saturn_vtable;

    s->mbus.user = s;
    s->mbus.read8 = sh2_r8;
    s->mbus.read16 = sh2_r16;
    s->mbus.read32 = sh2_r32;
    s->mbus.write8 = sh2_w8;
    s->mbus.write16 = sh2_w16;
    s->mbus.write32 = sh2_w32;
    s->sbus = s->mbus;
    sh2_init(&s->msh2, &s->mbus);
    sh2_init(&s->ssh2, &s->sbus);
    s->sys_clip[0] = SAT_FB_W - 1u;
    s->sys_clip[1] = SAT_FB_H - 1u;

    *out = &s->base;
    return EMU_OK;
}

static void saturn_destroy(emu_core_t *core)
{
    struct saturn *s = (struct saturn *)core;
    if (s == NULL)
        return;
    free(s->disc);
    free(s);
}

const emu_core_vtable_t *emu_core_supersaturn(void)
{
    return &saturn_vtable;
}
