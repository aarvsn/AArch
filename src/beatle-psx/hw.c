/*
 * beatle-psx: hardware I/O dispatch - interrupt controller, DMA controller,
 * root counters, joypad, and stub regions (CD-ROM, SPU, MDEC, memory control).
 *
 * All register offsets follow the specification; approximations are marked.
 */
#include "psx.h"

#include <string.h>

#include "../common/util.h"

/* ---- register offsets (within 1F801000h..1F801FFFh) -------------------------- */
#define R_MEMCTL   0x000u /* 1F801000h */
#define R_RAMSIZE  0x060u /* 1F801060h */
#define R_IRQ      0x070u /* 1F801070h */
#define R_DMA      0x080u /* 1F801080h */
#define R_TMR      0x100u /* 1F801100h */
#define R_PAD      0x040u /* 1F801040h */
#define R_GPU2     0x810u /* 1F801810h */
#define R_CD2      0x800u /* 1F801800h */
#define R_SPU      0xC00u /* 1F801C00h (stubs) */

void psx_hw_reset(psx_dma_t *d, psx_timer_t *t, psx_pad_t *pad)
{
    memset(d, 0, sizeof *d);
    d->dpcr = 0x07654321u; /* spec: priorities by channel, master disabled */
    d->dicr = 0;
    memset(t, 0, 3 * sizeof *t);
    memset(pad, 0, sizeof *pad);
    pad->buttons = 0xFFFFu; /* nothing pressed */
}

/* ---- DMA ------------------------------------------------------------------------ */

static void dma_complete(struct psx *p, int ch)
{
    uint32_t dicr = p->dma.dicr;
    p->dma.ch[ch].chcr &= ~0x01000000u; /* clear Start/Busy */
    if ((dicr >> (16u + (uint32_t)ch)) & 1u)
        p->dma.dicr |= 1u << (24u + (uint32_t)ch);
    /* Recompute bit31 and raise IRQ3 on 0-to-1 transition. */
    uint32_t new31 = (p->dma.dicr & 0x00800000u) &&
                             (p->dma.dicr & 0x7F000000u)
                         ? 0x80000000u
                         : 0;
    if (!(p->dma.dicr & 0x80000000u) && new31)
        psx_irq_raise(p, PSX_IRQ_DMA);
    p->dma.dicr = (p->dma.dicr & 0x00FFFFFFu) | new31;
}

static void dma_transfer(struct psx *p, int ch)
{
    psx_dma_ch_t *c = &p->dma.ch[ch];
    uint32_t chcr = c->chcr;
    int to_ram = (chcr & 1u) ? 1 : 0;      /* 0=to RAM? spec: 0=To Main RAM */
    uint32_t step = (chcr & 2u) ? 0xFFFFFFFCu : 4u;
    uint32_t sync = (chcr >> 9) & 3u;
    uint32_t addr = c->madr & 0x00FFFFFFu;

    if (ch == 6) { /* OTC: build reverse clear chain */
        uint32_t bcr = (c->bcr & 0xFFFFu) == 0 ? 0x10000u : c->bcr & 0xFFFFu;
        uint32_t cur = addr;
        for (uint32_t i = 0; i < bcr; i++) {
            uint32_t next = (i == bcr - 1u) ? 0xFFFFFFu : (cur - 4u) & 0xFFFFFFu;
            psx_ram_write32(p, cur & (PSX_RAM_SIZE - 1u), next);
            cur -= 4u;
        }
        dma_complete(p, ch);
        return;
    }

    if (sync == 2u && ch == 2) { /* linked-list mode (GPU) */
        uint32_t next = addr;
        for (int guard = 0; guard < 65536; guard++) {
            if (next & 0x800000u)
                break;
            uint32_t hdr = psx_bus_read32(p, 0x80000000u | (next & 0x1FFFFCu));
            uint32_t count = hdr >> 24;
            uint32_t p0 = (next + 4u) & 0x1FFFFCu;
            for (uint32_t i = 0; i < count; i++)
                psx_gpu_write_gp0(&p->gpu, p,
                                  psx_bus_read32(p, 0x80000000u |
                                                        ((p0 + i * 4u) &
                                                         0x1FFFFCu)));
            next = hdr & 0xFFFFFFu;
            if (next == 0xFFFFFFu)
                break;
        }
        dma_complete(p, ch);
        return;
    }

    /* Burst / sync-block transfer: BS words x BA blocks (0 => 10000). */
    uint32_t bs = c->bcr & 0xFFFFu;
    uint32_t ba = c->bcr >> 16;
    if (sync == 1u) {
        if (bs == 0)
            bs = 0x10000u;
        if (ba == 0)
            ba = 0x10000u;
    } else {
        ba = 1;
        if (bs == 0)
            bs = 0x10000u;
    }

    uint32_t total = bs * ba;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t a = 0x80000000u | (addr & 0x1FFFFCu);
        if (to_ram) { /* direction bit set: device -> RAM */
            uint32_t v = 0;
            if (ch == 4)
                v = 0; /* SPU: silent, no data */
            psx_bus_write32(p, a, v);
        } else { /* RAM -> device */
            uint32_t v = psx_bus_read32(p, a);
            (void)v; /* GPU/CD/MDEC consume silently (stubs) */
        }
        addr += step;
    }
    dma_complete(p, ch);
}

void psx_dma_run(struct psx *p)
{
    psx_dma_t *d = &p->dma;
    for (int ch = 0; ch < 7; ch++) {
        uint32_t chcr = d->ch[ch].chcr;
        if (!(chcr & 0x01000000u)) /* Start/Busy */
            continue;
        uint32_t prio_shift = (uint32_t)ch * 4u;
        if (!((d->dpcr >> (prio_shift + 3u)) & 1u))
            continue; /* channel master disabled */
        uint32_t sync = (chcr >> 9) & 3u;
        /* SyncMode 0 requires the Trigger bit (bit24 of CHCR). */
        if (sync == 0u && !(chcr & 0x10000000u))
            continue;
        if (sync == 0u)
            d->ch[ch].chcr &= ~0x10000000u; /* trigger clears at begin */
        dma_transfer(p, ch);
    }
}

/* ---- joypad (simplified digital pad) ---------------------------------------------- */

static void pad_write_data(psx_pad_t *pad, uint8_t v)
{
    /* Documented simplified handshake: the host writes 01h,42h,00h,00h;
     * the pad answers 5Ah (ID) plus the 16 button bits and an idle byte. */
    if ((pad->ctrl & 2u) == 0)
        return; /* TXE not set */

    if (v == 0x01u) {
        pad->expect = 1;
        pad->reply_pos = 0;
        pad->reply_len = 0;
        pad->stat |= 0x0002u; /* RX FIFO not empty (ready to reply) */
        pad->reply[0] = 0xFFu;
        pad->reply_len = 1;
    } else if (pad->expect == 1u && v == 0x42u) {
        pad->expect = 2;
    } else if (pad->expect == 2u) {
        pad->expect = 3;
    } else if (pad->expect == 3u) {
        /* access acknowledged: queue pad ID + buttons */
        pad->reply[0] = 0x5Au;
        pad->reply[1] = (uint8_t)(pad->buttons & 0xFFu);
        pad->reply[2] = (uint8_t)(pad->buttons >> 8);
        pad->reply[3] = 0xFFu;
        pad->reply_len = 4;
        pad->reply_pos = 0;
        pad->stat |= 0x0002u;
        pad->expect = 0;
    } else {
        pad->reply[0] = 0xFFu;
        pad->reply_len = 1;
        pad->reply_pos = 0;
        pad->stat |= 0x0002u;
    }
}

static uint8_t pad_read_data(psx_pad_t *pad)
{
    if (!(pad->stat & 0x0002u))
        return 0xFFu;
    if (pad->reply_pos >= pad->reply_len) {
        pad->stat &= 0xFFFDu;
        return 0xFFu;
    }
    uint8_t v = pad->reply[pad->reply_pos++];
    if (pad->reply_pos >= pad->reply_len) {
        pad->stat &= 0xFFFDu; /* FIFO drained */
        pad->stat |= 0x0004u; /* acknowledge */
    }
    return v;
}

/* ---- hardware read dispatch --------------------------------------------------------- */

uint32_t psx_hw_read(struct psx *p, uint32_t addr)
{
    /* Memory control registers return their spec defaults. */
    if (addr < 0x1F801020u) {
        static const uint32_t defaults[8] = { 0x1F000000u, 0x1F802000u,
                                              0x0013243Fu, 0x00003022u,
                                              0x0013243Fu, 0x200931E1u,
                                              0x00020843u, 0x000543C7u };
        return defaults[(addr - 0x1F801000u) >> 2];
    }
    if (addr == 0x1F801060u)
        return 0x00000B88u; /* RAM_SIZE */
    if (addr == 0x1F801070u)
        return p->irq_istat;
    if (addr == 0x1F801074u)
        return p->irq_imask;

    if (addr >= 0x1F801080u && addr <= 0x1F8010F4u) {
        if (addr >= 0x1F8010F0u) {
            if (addr == 0x1F8010F0u)
                return p->dma.dpcr;
            if (addr == 0x1F8010F4u) {
                /* bit31 follows enable/flags per spec */
                uint32_t d = p->dma.dicr;
                uint32_t b31 = (d & 0x00800000u) && (d & 0x7F000000u)
                                   ? 0x80000000u
                                   : 0u;
                return (d & 0x00FFFFFFu) | b31;
            }
            return 0;
        }
        uint32_t ch = (addr - 0x1F801080u) >> 4;
        switch (addr & 0xFu) {
        case 0x0: return p->dma.ch[ch].madr;
        case 0x4: return p->dma.ch[ch].bcr;
        case 0x8:
        case 0xC: return p->dma.ch[ch].chcr;
        default:  return 0;
        }
    }

    if (addr >= 0x1F801100u && addr < 0x1F801130u) {
        uint32_t t = (addr - 0x1F801100u) >> 4;
        switch (addr & 0xFu) {
        case 0x0: {
            /* reading the counter clears the reached flags */
            uint32_t v = p->timer[t].count;
            p->timer[t].mode_read_latch &= (uint16_t)~0x3000u;
            return v;
        }
        case 0x4: {
            /* bit10 = IRQ line state (1 = idle); reached flags read-clear */
            uint32_t ret = (p->timer[t].mode & ~0x3000u) |
                           (p->timer[t].mode_read_latch & 0x3400u);
            p->timer[t].mode_read_latch &= 0x0400u;
            return ret;
        }
        case 0x8: return p->timer[t].target;
        default:  return 0;
        }
    }

    if (addr >= 0x1F801040u && addr <= 0x1F80104Fu) {
        psx_pad_t *pad = &p->pad;
        switch (addr & 0xFu) {
        case 0x0: return pad_read_data(pad);
        case 0x4: return pad->stat | 0x0005u; /* TX ready bits set */
        case 0x8: return pad->ctrl;
        case 0xE: return 0; /* baud */
        default:  return 0;
        }
    }

    if (addr >= 0x1F801810u && addr <= 0x1F801823u) {
        if (addr == 0x1F801810u)
            return psx_gpu_read(&p->gpu);
        if (addr == 0x1F801814u)
            return psx_gpu_status(&p->gpu);
        return 0; /* MDEC data/control: stub */
    }

    if (addr >= 0x1F801800u && addr <= 0x1F801803u)
        return 0xFFu; /* CD-ROM: no drive */

    /* SPU, expansion regions: open bus. */
    return 0xFFFFFFFFu;
}

/* ---- hardware write dispatch ---------------------------------------------------------- */

void psx_hw_write(struct psx *p, uint32_t addr, uint32_t v)
{
    if (addr < 0x1F801020u)
        return; /* memory control: accepted, no timing model */
    if (addr == 0x1F801060u)
        return;
    if (addr == 0x1F801070u) {
        p->irq_istat &= (uint16_t)~(v & 0x7FFu); /* write 1 clears */
        return;
    }
    if (addr == 0x1F801074u) {
        p->irq_imask = (uint16_t)(v & 0x7FFu);
        return;
    }

    if (addr >= 0x1F801080u && addr <= 0x1F8010F4u) {
        if (addr >= 0x1F8010F0u) {
            if (addr == 0x1F8010F0u) {
                p->dma.dpcr = v;
                return;
            }
            if (addr == 0x1F8010F4u) {
                uint32_t ack = v & 0x7F000000u;
                p->dma.dicr = (p->dma.dicr & ~ack & 0x00FFFFFFu) |
                              (v & 0x00FFFFFFu);
                return;
            }
            return;
        }
        uint32_t ch = (addr - 0x1F801080u) >> 4;
        switch (addr & 0xFu) {
        case 0x0: p->dma.ch[ch].madr = v; return;
        case 0x4: p->dma.ch[ch].bcr = v; return;
        case 0x8:
        case 0xC:
            p->dma.ch[ch].chcr = v;
            psx_dma_run(p);
            return;
        default: return;
        }
    }

    if (addr >= 0x1F801100u && addr < 0x1F801130u) {
        uint32_t t = (addr - 0x1F801100u) >> 4;
        switch (addr & 0xFu) {
        case 0x0: p->timer[t].count = (uint16_t)v; return;
        case 0x4:
            p->timer[t].mode = (uint16_t)v;
            p->timer[t].count = 0; /* mode write resets counter */
            p->timer[t].mode_read_latch = 0x0400u; /* bit10 set after write */
            return;
        case 0x8: p->timer[t].target = (uint16_t)v; return;
        default:  return;
        }
    }

    if (addr >= 0x1F801040u && addr <= 0x1F80104Fu) {
        psx_pad_t *pad = &p->pad;
        switch (addr & 0xFu) {
        case 0x0: pad_write_data(pad, (uint8_t)v); return;
        case 0x4: return;
        case 0x8:
            pad->ctrl = v & 0x3FFFu;
            if (!(pad->ctrl & 1u)) { /* SELECT released: reset handshake */
                pad->expect = 0;
                pad->reply_pos = 0;
                pad->reply_len = 0;
                pad->stat &= 0xFFFBu;
            }
            return;
        case 0xA: return; /* baud */
        default:  return;
        }
    }

    if (addr >= 0x1F801810u && addr <= 0x1F801823u) {
        if (addr == 0x1F801810u) {
            psx_gpu_write_gp0(&p->gpu, p, v);
            return;
        }
        if (addr == 0x1F801814u) {
            psx_gpu_write_gp1(&p->gpu, p, v);
            return;
        }
        return; /* MDEC: stub */
    }

    /* CD-ROM, SPU, expansion: writes accepted and ignored (documented). */
}
