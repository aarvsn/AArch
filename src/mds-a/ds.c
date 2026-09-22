/*
 * mds-a: Nintendo DS machine. See ds.h for the verified scope, boot model
 * and documented simplifications.
 */
#include "ds.h"

#include <stdlib.h>
#include <string.h>

#include "../common/util.h"

/* forward declarations (I/O model defined below the bus primitives) */
static uint32_t a9_io_read32(struct ds *d, uint32_t a);
static void a9_io_write32(struct ds *d, uint32_t a, uint32_t v);
static uint16_t a9_io_read16(struct ds *d, uint32_t a);
static uint8_t a9_io_read8(struct ds *d, uint32_t a);
static void a9_io_write16(struct ds *d, uint32_t a, uint16_t v);
static uint32_t a7_io_read32(struct ds *d, uint32_t a);
static void a7_io_write32(struct ds *d, uint32_t a, uint32_t v);

/* ---- bus: ARM9 ----------------------------------------------------------------- */

static uint8_t *a9_mem_ptr(struct ds *d, uint32_t a)
{
    if (a < DS_ITCM_SIZE)
        return &d->itcm[a]; /* ITCM mirror at 0x00000000 */
    if (a >= 0x01000000u && a < 0x01000000u + DS_ITCM_SIZE)
        return &d->itcm[a - 0x01000000u];
    if (a >= 0x02000000u && a < 0x02000000u + DS_MAIN_RAM_SIZE)
        return &d->main_ram[a - 0x02000000u];
    if (a >= 0x03000000u && a < 0x03000000u + DS_WRAM_SH_SIZE)
        return &d->wram_sh[a - 0x03000000u];
    if (a >= 0x06000000u && a < 0x06000000u + DS_VRAM_A_SIZE)
        return &d->vram_a[a - 0x06000000u];
    if (a >= 0x06200000u && a < 0x06200000u + DS_VRAM_B_SIZE)
        return &d->vram_b[a - 0x06200000u];
    if (a >= 0x06400000u && a < 0x06400000u + DS_VRAM_C_SIZE)
        return &d->vram_c[a - 0x06400000u];
    if (a >= 0x06600000u && a < 0x06600000u + DS_VRAM_D_SIZE)
        return &d->vram_d[a - 0x06600000u];
    if (a >= 0x06800000u && a < 0x06800000u + DS_VRAM_E_SIZE)
        return &d->vram_e[a - 0x06800000u];
    if (a >= 0x06890000u && a < 0x06890000u + DS_VRAM_F_SIZE)
        return &d->vram_f[a - 0x06890000u];
    if (a >= 0x068A0000u && a < 0x068A0000u + DS_VRAM_G_SIZE)
        return &d->vram_g[a - 0x068A0000u];
    if (a >= 0x06880000u && a < 0x06888000u)
        return &d->pal[a - 0x06880000u]; /* BG/OBJ palettes (fixed RAM) */
    if (a >= 0x07000000u && a < 0x07000800u)
        return (a - 0x07000000u) < 0x400u ? &d->oam_a[a - 0x07000000u]
                                          : &d->oam_b[a - 0x07000400u];
    if (a >= 0xFFFF0000u)
        return NULL; /* BIOS region: RAM-backed zeros */
    return NULL;
}

/* DTCM overlays the ARM9 map where CP15 places it (default 0x08000000). */
static uint32_t a9_dtcm_base(struct ds *d)
{
    return d->cp15_dtcm & ~0x1FFFu;
}

static uint32_t a9_dtcm_size(struct ds *d)
{
    return (((d->cp15_dtcm >> 1) & 0x1Fu) + 1u) * 4096u;
}

static uint32_t a9_itcm_base(struct ds *d)
{
    return d->cp15_itcm & ~0x1FFFu;
}

static uint32_t a9_itcm_size(struct ds *d)
{
    return (((d->cp15_itcm >> 1) & 0x1Fu) + 1u) * 4096u;
}

static uint8_t *a9_data_ptr(struct ds *d, uint32_t a)
{
    uint32_t db = a9_dtcm_base(d);
    if (a >= db && a < db + a9_dtcm_size(d) && db != 0u)
        return &d->dtcm[a - db];
    uint32_t ib = a9_itcm_base(d);
    if (a >= ib && a < ib + a9_itcm_size(d) && ib != 0u)
        return &d->itcm[a - ib];
    return a9_mem_ptr(d, a);
}

static uint8_t a9_read8(struct ds *d, uint32_t a)
{
    uint8_t *p = a9_data_ptr(d, a);
    if (p != NULL)
        return *p;
    if (a >= 0x04000000u && a < 0x04002000u)
        return a9_io_read8(d, a);
    return 0; /* BIOS, GBA slot, unmapped: RAM-backed zeros */
}

static uint16_t a9_read16(struct ds *d, uint32_t a)
{
    uint8_t *p = a9_data_ptr(d, a);
    if (p != NULL)
        return (uint16_t)(p[0] | (p[1] << 8));
    if (a >= 0x04000000u && a < 0x04002000u)
        return a9_io_read16(d, a);
    return 0;
}

static uint32_t a9_read32(struct ds *d, uint32_t a)
{
    uint8_t *p = a9_data_ptr(d, a);
    if (p != NULL)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (a >= 0x04000000u && a < 0x04002000u)
        return a9_io_read32(d, a);
    return 0;
}

static void a9_write8(struct ds *d, uint32_t a, uint8_t v)
{
    uint8_t *p = a9_data_ptr(d, a);
    if (p != NULL) {
        *p = v;
        return;
    }
}

static void a9_write16(struct ds *d, uint32_t a, uint16_t v)
{
    uint8_t *p = a9_data_ptr(d, a);
    if (p != NULL) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        return;
    }
    if (a >= 0x04000000u && a < 0x04002000u)
        a9_io_write16(d, a, v);
}

static void a9_write32(struct ds *d, uint32_t a, uint32_t v)
{
    uint8_t *p = a9_data_ptr(d, a);
    if (p != NULL) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
        return;
    }
    if (a >= 0x04000000u && a < 0x04002000u)
        a9_io_write32(d, a, v);
}

/* ---- bus: ARM7 ----------------------------------------------------------------- */

static uint8_t *a7_mem_ptr(struct ds *d, uint32_t a)
{
    if (a < 0x00004000u)
        return NULL; /* ARM7 BIOS: RAM-backed zeros */
    if (a >= 0x02000000u && a < 0x02000000u + DS_MAIN_RAM_SIZE)
        return &d->main_ram[a - 0x02000000u];
    if (a >= 0x037F8000u && a < 0x037F8000u + DS_WRAM_SH_SIZE)
        return &d->wram_sh[a - 0x037F8000u];
    if (a >= 0x03800000u && a < 0x03800000u + DS_WRAM7_SIZE)
        return &d->wram7[a - 0x03800000u];
    if (a >= 0x06000000u && a < 0x06000000u + DS_VRAM_A_SIZE)
        return &d->vram_a[a - 0x06000000u];
    if (a >= 0x06200000u && a < 0x06200000u + DS_VRAM_B_SIZE)
        return &d->vram_b[a - 0x06200000u];
    if (a >= 0x06400000u && a < 0x06400000u + DS_VRAM_C_SIZE)
        return &d->vram_c[a - 0x06400000u];
    if (a >= 0x06600000u && a < 0x06600000u + DS_VRAM_D_SIZE)
        return &d->vram_d[a - 0x06600000u];
    if (a >= 0x06800000u && a < 0x06800000u + DS_VRAM_E_SIZE)
        return &d->vram_e[a - 0x06800000u];
    if (a >= 0x06890000u && a < 0x06890000u + DS_VRAM_F_SIZE)
        return &d->vram_f[a - 0x06890000u];
    if (a >= 0x068A0000u && a < 0x068A0000u + DS_VRAM_G_SIZE)
        return &d->vram_g[a - 0x068A0000u];
    return NULL;
}

static uint8_t a7_read8(struct ds *d, uint32_t a)
{
    uint8_t *p = a7_mem_ptr(d, a);
    if (p != NULL)
        return *p;
    if (a >= 0x04000000u && a < 0x04001000u)
        return (uint8_t)a7_io_read32(d, a & ~3u);
    return 0;
}

static uint16_t a7_read16(struct ds *d, uint32_t a)
{
    uint8_t *p = a7_mem_ptr(d, a);
    if (p != NULL)
        return (uint16_t)(p[0] | (p[1] << 8));
    if (a >= 0x04000000u && a < 0x04001000u)
        return (uint16_t)a7_io_read32(d, a & ~3u);
    return 0;
}

static uint32_t a7_read32(struct ds *d, uint32_t a)
{
    uint8_t *p = a7_mem_ptr(d, a);
    if (p != NULL)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (a >= 0x04000000u && a < 0x04001000u)
        return a7_io_read32(d, a);
    return 0;
}

static void a7_write8(struct ds *d, uint32_t a, uint8_t v)
{
    uint8_t *p = a7_mem_ptr(d, a);
    if (p != NULL)
        *p = v;
}

static void a7_write16(struct ds *d, uint32_t a, uint16_t v)
{
    uint8_t *p = a7_mem_ptr(d, a);
    if (p != NULL) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

static void a7_write32(struct ds *d, uint32_t a, uint32_t v)
{
    uint8_t *p = a7_mem_ptr(d, a);
    if (p != NULL) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
        return;
    }
    if (a >= 0x04000000u && a < 0x04001000u)
        a7_io_write32(d, a, v);
}

/* ---- arm bus adapters ----------------------------------------------------------- */

static uint8_t arm9_rb(void *u, uint32_t a) { return a9_read8(u, a); }
static uint16_t arm9_rh(void *u, uint32_t a) { return a9_read16(u, a); }
static uint32_t arm9_rw(void *u, uint32_t a) { return a9_read32(u, a); }
static void arm9_wb(void *u, uint32_t a, uint8_t v) { a9_write8(u, a, v); }
static void arm9_wh(void *u, uint32_t a, uint16_t v) { a9_write16(u, a, v); }
static void arm9_ww(void *u, uint32_t a, uint32_t v) { a9_write32(u, a, v); }

static uint8_t arm7_rb(void *u, uint32_t a) { return a7_read8(u, a); }
static uint16_t arm7_rh(void *u, uint32_t a) { return a7_read16(u, a); }
static uint32_t arm7_rw(void *u, uint32_t a) { return a7_read32(u, a); }
static void arm7_wb(void *u, uint32_t a, uint8_t v) { a7_write8(u, a, v); }
static void arm7_wh(void *u, uint32_t a, uint16_t v) { a7_write16(u, a, v); }
static void arm7_ww(void *u, uint32_t a, uint32_t v) { a7_write32(u, a, v); }

static const arm_bus_t arm9_bus_tmpl = {
    NULL, arm9_rb, arm9_rh, arm9_rw, arm9_wb, arm9_wh, arm9_ww
};
static const arm_bus_t arm7_bus_tmpl = {
    NULL, arm7_rb, arm7_rh, arm7_rw, arm7_wb, arm7_wh, arm7_ww
};

/* ---- I/O register model --------------------------------------------------------- */

/* Registers handled dynamically; everything else uses the io[] array.
 * Engine A lives at 0x04000000, engine B at 0x04001000 (ARM9 only, storage).
 * Timer registers (0x04000100 + 4n, both CPUs) are wired to the timer units. */
#define IO_VCOUNT   0x004u
#define IO_TM_BASE  0x100u
#define IO_KEYINPUT 0x130u
#define IO_IPCSYNC  0x180u
#define IO_IME      0x208u
#define IO_IE       0x210u
#define IO_IF       0x214u

static uint16_t keyinput(struct ds *d)
{
    return (uint16_t)(~d->buttons & 0xFFFu); /* active-low */
}

/* timer register slot for a page offset, or NULL outside 0x100..0x11F */
static struct ds_timer *io_timer(struct ds_cpu *c, uint32_t off)
{
    if (off < IO_TM_BASE || off >= IO_TM_BASE + 0x20u || (off & 3u) != 0u)
        return NULL;
    return &c->tm[(off - IO_TM_BASE) / 4u];
}

static uint32_t io_timer_read32(struct ds_cpu *c, uint32_t off)
{
    const struct ds_timer *t = io_timer(c, off);
    return (t->counter & 0xFFFFu) | ((uint32_t)t->cnt << 16);
}

/* Documented write model: writing the control halfword with the enable bit
 * set reloads the counter immediately; writing the reload halfword alone
 * does not. */
static void io_timer_write32(struct ds_cpu *c, uint32_t off, uint32_t v)
{
    struct ds_timer *t = io_timer(c, off);
    t->reload = (uint16_t)v;
    t->cnt = (uint8_t)(v >> 16);
    if (t->cnt & 0x80u) {
        t->counter = t->reload;
        t->acc = 0;
    }
}

static uint16_t io_read16(struct ds *d, struct ds_cpu *c, uint32_t off)
{
    if (off < 0x1000u) {
        switch (off) {
        case IO_VCOUNT:
            return (uint16_t)d->vcount;
        case IO_KEYINPUT:
            return keyinput(d);
        case IO_IPCSYNC:
            return (uint16_t)(c == &d->a9 ? d->ipc9 : d->ipc7);
        case IO_IE:
            return (uint16_t)c->ie;
        case IO_IF:
            return (uint16_t)c->if_latch;
        case IO_IME:
            return (uint16_t)c->ime;
        default:
            if (off >= IO_TM_BASE && off < IO_TM_BASE + 0x20u) {
                uint32_t tidx = (off - IO_TM_BASE) / 4u;
                if ((off & 3u) == 0u) {
                    return (uint16_t)(c->tm[tidx].counter & 0xFFFFu);
                }
                if ((off & 3u) == 2u)
                    return c->tm[tidx].cnt;
            }
            break;
        }
    }
    return emu_le16(&c->io[off]);
}

static uint8_t io_read8(struct ds *d, struct ds_cpu *c, uint32_t off)
{
    uint16_t v = io_read16(d, c, off & ~1u);
    return (uint8_t)((off & 1u) ? (v >> 8) : v);
}

static void io_write16(struct ds *d, struct ds_cpu *c, uint32_t off,
                       uint16_t v)
{
    if (off < 0x1000u) {
        switch (off) {
        case IO_IPCSYNC: {
            /* documented model: bits 0-3 recv, 8-11 send, bit14 = IRQ
             * enable. Writing with bit14 set raises the remote IRQ when
             * the remote side has its own bit14 enable set. */
            uint32_t cur = c == &d->a9 ? d->ipc9 : d->ipc7;
            uint32_t *rem = c == &d->a9 ? &d->ipc7 : &d->ipc9;
            uint32_t send = (v >> 8) & 0xFu;
            /* own latch: keep the recv nibble (remote-driven), take the
             * send nibble + IRQ-enable from the write */
            uint32_t val = (cur & 0xFu) | (v & 0x4F00u);
            if (c == &d->a9)
                d->ipc9 = val;
            else
                d->ipc7 = val;
            *rem = (*rem & 0x4F00u) | send;
            if ((v & 0x4000u) && (*rem & 0x4000u)) {
                if (c == &d->a9)
                    d->a7.if_latch |= DS_IRQ_IPC;
                else
                    d->a9.if_latch |= DS_IRQ_IPC;
            }
            return;
        }
        case IO_IE:
            c->ie = (c->ie & 0xFFFF0000u) | v;
            return;
        case IO_IF:
            c->if_latch &= ~(uint32_t)v;
            return;
        case IO_IME:
            c->ime = v & 1u;
            return;
        default:
            if (off >= IO_TM_BASE && off < IO_TM_BASE + 0x20u) {
                uint32_t tidx = (off - IO_TM_BASE) / 4u;
                if ((off & 3u) == 0u) {
                    c->tm[tidx].reload = v; /* reload halfword: no reload */
                } else if ((off & 3u) == 2u) {
                    c->tm[tidx].cnt = (uint8_t)v;
                    if (v & 0x80u) {
                        c->tm[tidx].counter = c->tm[tidx].reload;
                        c->tm[tidx].acc = 0;
                    }
                }
                return;
            }
            break;
        }
    }
    emu_store_le16(&c->io[off], v);
}

static uint32_t a9_io_read32(struct ds *d, uint32_t a)
{
    uint32_t off = a & 0x1FFFu;
    if (off < 0x1000u) {
        switch (off) {
        case IO_VCOUNT:
            return d->vcount;
        case IO_KEYINPUT:
            return keyinput(d); /* bits 0-11, active-low */
        case IO_IPCSYNC:
            return d->ipc9;
        case IO_IE:
            return d->a9.ie;
        case IO_IF:
            return d->a9.if_latch;
        case IO_IME:
            return d->a9.ime;
        default:
            if (io_timer(&d->a9, off) != NULL)
                return io_timer_read32(&d->a9, off);
            break;
        }
    }
    return emu_le32(&d->a9.io[off]);
}

static uint32_t a7_io_read32(struct ds *d, uint32_t a)
{
    uint32_t off = a & 0xFFFu;
    switch (off) {
    case IO_VCOUNT:
        return d->vcount;
    case IO_KEYINPUT:
        return keyinput(d); /* bits 0-11, active-low */
    case IO_IPCSYNC:
        return d->ipc7;
    case IO_IE:
        return d->a7.ie;
    case IO_IF:
        return d->a7.if_latch;
    case IO_IME:
        return d->a7.ime;
    default:
        if (io_timer(&d->a7, off) != NULL)
            return io_timer_read32(&d->a7, off);
        return emu_le32(&d->a7.io[off]);
    }
}

static uint16_t a9_io_read16(struct ds *d, uint32_t a)
{
    return io_read16(d, &d->a9, a & 0x1FFFu);
}

static uint8_t a9_io_read8(struct ds *d, uint32_t a)
{
    return io_read8(d, &d->a9, a & 0x1FFFu);
}

static void a9_io_write16(struct ds *d, uint32_t a, uint16_t v)
{
    io_write16(d, &d->a9, a & 0x1FFFu, v);
}

static void a9_io_write32(struct ds *d, uint32_t a, uint32_t v)
{
    uint32_t off = a & 0x1FFFu;
    if (off < 0x1000u) {
        switch (off) {
        case IO_IPCSYNC:
            io_write16(d, &d->a9, IO_IPCSYNC, (uint16_t)v);
            return;
        case IO_IE:
            d->a9.ie = v & 0x0007FFFFu;
            return;
        case IO_IF:
            d->a9.if_latch &= ~v;
            return;
        case IO_IME:
            d->a9.ime = v & 1u;
            return;
        case IO_VCOUNT:
            emu_store_le16(&d->a9.io[IO_VCOUNT], (uint16_t)v);
            return;
        default:
            if (io_timer(&d->a9, off) != NULL) {
                io_timer_write32(&d->a9, off, v);
                return;
            }
            break;
        }
    }
    emu_store_le32(&d->a9.io[off], v);
}

static void a7_io_write32(struct ds *d, uint32_t a, uint32_t v)
{
    uint32_t off = a & 0xFFFu;
    switch (off) {
    case IO_IPCSYNC:
        io_write16(d, &d->a7, IO_IPCSYNC, (uint16_t)v);
        return;
    case IO_IE:
        d->a7.ie = v & 0x0007FFFFu;
        return;
    case IO_IF:
        d->a7.if_latch &= ~v;
        return;
    case IO_IME:
        d->a7.ime = v & 1u;
        return;
    case IO_VCOUNT:
        emu_store_le16(&d->a7.io[IO_VCOUNT], (uint16_t)v);
        return;
    default:
        if (io_timer(&d->a7, off) != NULL) {
            io_timer_write32(&d->a7, off, v);
            return;
        }
        emu_store_le32(&d->a7.io[off], v);
        return;
    }
}

/* ---- timers ---------------------------------------------------------------------- */

static const uint32_t tm_prescale[4] = { 1u, 64u, 256u, 1024u };

static void timer_feed(struct ds *d, struct ds_cpu *c, int idx,
                       uint64_t counts, uint64_t *irq_out)
{
    struct ds_timer *t = &c->tm[idx];
    if (!(t->cnt & 0x80u))
        return;
    if (idx > 0 && (t->cnt & 0x04u)) {
        /* cascade: counts arrive as overflows of the previous timer */
        uint64_t wrap = 0x10000ull - (uint64_t)t->reload;
        uint64_t all = (uint64_t)(t->counter - t->reload) + counts;
        uint64_t over = all / wrap;
        t->counter = (uint32_t)((uint64_t)t->reload + all % wrap);
        if (over > 0) {
            if (t->cnt & 0x40u)
                *irq_out |= DS_IRQ_TM0 << idx;
            if (idx < 3)
                timer_feed(d, c, idx + 1, over, irq_out);
        }
        return;
    }
    uint64_t total = t->acc + counts;
    uint64_t presc = tm_prescale[t->cnt & 3u];
    uint64_t n = total / presc;
    t->acc = total % presc;
    if (n == 0)
        return;
    uint64_t wrap = 0x10000ull - (uint64_t)t->reload;
    uint64_t all = (uint64_t)(t->counter - t->reload) + n;
    uint64_t over = all / wrap;
    t->counter = (uint32_t)((uint64_t)t->reload + all % wrap);
    if (over > 0) {
        if (t->cnt & 0x40u)
            *irq_out |= DS_IRQ_TM0 << idx;
        if (idx < 3)
            timer_feed(d, c, idx + 1, over, irq_out);
    }
}

void ds_tick_timers(struct ds_cpu *c, struct ds *d, uint32_t cycles,
                    uint64_t *irq_out)
{
    for (int i = 0; i < 4; i++) {
        struct ds_timer *t = &c->tm[i];
        if (!(t->cnt & 0x80u))
            continue;
        if (i > 0 && (t->cnt & 0x04u))
            continue; /* cascaded: fed by the previous timer */
        uint64_t total = (uint64_t)cycles;
        uint64_t presc = tm_prescale[t->cnt & 3u];
        uint64_t n = total / presc;
        uint64_t frac = total % presc;
        t->acc = (t->acc + frac) % presc;
        if (n > 0)
            timer_feed(d, c, i, n, irq_out);
    }
    (void)d;
}

/* ---- CP15 hooks (ARM946E-S subset) ----------------------------------------------- */

static int ds9_mrc(void *user, int cp, int opc1, int crn, int rd, int opc2,
                   int crm, uint32_t *out)
{
    struct ds *d = (struct ds *)user;
    (void)opc1; (void)rd;
    if (cp != 15)
        return 0;
    if (crn == 0 && crm == 0 && opc2 == 0) {
        *out = 0x41059461u; /* documented ARM946E-S ID placeholder */
        return 1;
    }
    if (crn == 1 && crm == 0 && opc2 == 0) {
        *out = d->cp15_c1;
        return 1;
    }
    if (crn == 9 && crm == 0 && opc2 == 0) {
        *out = d->cp15_dtcm;
        return 1;
    }
    if (crn == 9 && crm == 1 && opc2 == 0) {
        *out = d->cp15_itcm;
        return 1;
    }
    *out = 0; /* cache/protect registers read as 0 (not modeled) */
    return 1;
}

static int ds9_mcr(void *user, int cp, int opc1, int crn, int rd, int opc2,
                   int crm, uint32_t val)
{
    struct ds *d = (struct ds *)user;
    (void)opc1; (void)rd; (void)opc2;
    if (cp != 15)
        return 0;
    switch (crn) {
    case 1:
        if (crm == 0)
            d->cp15_c1 = val; /* control bits stored; caches not modeled */
        return 1;
    case 7: /* cache invalidate/clean: no-op (no cache model) */
        return 1;
    case 8: /* TLB ops: no-op (no MMU model) */
        return 1;
    case 3: /* write buffer control: stored, not modeled */
        return 1;
    case 9:
        if (crm == 0)
            d->cp15_dtcm = val;
        else if (crm == 1)
            d->cp15_itcm = val;
        return 1;
    case 10: /* TCM/clean ops: accepted */
        return 1;
    default:
        return 1; /* accept-and-ignore (documented) */
    }
}

static const arm_cp15_t ds9_cp15 = { ds9_mrc, ds9_mcr };

/* ---- interrupt model -------------------------------------------------------------- */

static void ds_update_irq(struct ds *d)
{
    /* ARM9 */
    if (d->a9.ime && (d->a9.if_latch & d->a9.ie))
        arm_irq(&d->a9.cpu);
    /* ARM7 */
    if (d->a7.ime && (d->a7.if_latch & d->a7.ie))
        arm_irq(&d->a7.cpu);
}

/* ---- video (full 2D composition, see ds2d.c) --------------------------------------- */

void ds_render(struct ds *d)
{
    for (size_t i = 0; i < sizeof d->fb / sizeof d->fb[0]; i++)
        d->fb[i] = 0xFF000000u; /* opaque black (blank/disabled engines) */
    ds2d_render_engine(d, 0, &d->fb[0]);              /* A: top screen    */
    ds2d_render_engine(d, 1, &d->fb[192u * DS_SCREEN_W]); /* B: bottom    */
}

/* ---- per-frame run ---------------------------------------------------------------- */

void ds9_step(struct ds *d)
{
    uint32_t cyc = arm_step(&d->a9.cpu);
    uint64_t irq = 0;
    ds_tick_timers(&d->a9, d, cyc, &irq);
    if (irq)
        d->a9.if_latch |= (uint32_t)irq;
    ds_update_irq(d);
}

void ds7_step(struct ds *d)
{
    uint32_t cyc = arm_step(&d->a7.cpu);
    uint64_t irq = 0;
    ds_tick_timers(&d->a7, d, cyc, &irq);
    if (irq)
        d->a7.if_latch |= (uint32_t)irq;
    ds_update_irq(d);
}

static void run_cpu(struct ds *d, struct ds_cpu *c, uint32_t cycles)
{
    uint64_t remaining = (uint64_t)cycles;
    while (remaining > 0) {
        uint32_t cyc = arm_step(&c->cpu);
        uint64_t irq = 0;
        ds_tick_timers(c, d, cyc, &irq);
        if (irq)
            c->if_latch |= (uint32_t)irq;
        ds_update_irq(d);
        if (remaining < cyc)
            remaining = 0;
        else
            remaining -= cyc;
    }
}

static emu_result_t ds_run_frame(emu_core_t *core)
{
    struct ds *d = (struct ds *)core;
    if (d->rom == NULL)
        return EMU_ENOROM;

    const uint32_t a9_total = DS_ARM9_HZ / 60u;
    const uint32_t a7_total = DS_ARM7_HZ / 60u;
    const uint32_t per_line9 = a9_total / DS_LINES_PER_FRAME;
    const uint32_t per_line7 = a7_total / DS_LINES_PER_FRAME;
    uint32_t match = emu_le16(&d->a9.io[IO_VCOUNT]);

    for (uint32_t line = 0; line < DS_LINES_PER_FRAME; line++) {
        d->vcount = line;
        run_cpu(d, &d->a9, per_line9);
        run_cpu(d, &d->a7, per_line7);
        if (line == 192u) {
            /* start of VBlank (documented approximation) */
            d->a9.if_latch |= DS_IRQ_VBLANK;
            d->a7.if_latch |= DS_IRQ_VBLANK;
            ds_update_irq(d);
        }
        if (line == match) {
            d->a9.if_latch |= DS_IRQ_VCOUNT;
            d->a7.if_latch |= DS_IRQ_VCOUNT;
            ds_update_irq(d);
        }
    }
    d->vcount = 0; /* wrapped at the frame boundary */
    d->frame_count++;
    ds_render(d);
    return EMU_OK;
}

/* ---- load / reset ------------------------------------------------------------------ */

static void ds_boot(struct ds *d)
{
    /* Direct-boot model (documented): copy ARM9/ARM7 code per the header,
     * enter both CPUs in System mode with IRQ/FIQ masked. */
    if (d->rom == NULL)
        return;
    uint32_t a9_off = emu_le32(&d->rom[DS_HDR_ARM9_OFF]);
    uint32_t a9_adr = emu_le32(&d->rom[DS_HDR_ARM9_ENTRY]);
    uint32_t a9_dst = emu_le32(&d->rom[DS_HDR_ARM9_ADDR]);
    uint32_t a9_len = emu_le32(&d->rom[DS_HDR_ARM9_SIZE]);
    uint32_t a7_off = emu_le32(&d->rom[DS_HDR_ARM7_OFF]);
    uint32_t a7_adr = emu_le32(&d->rom[DS_HDR_ARM7_ENTRY]);
    uint32_t a7_dst = emu_le32(&d->rom[DS_HDR_ARM7_ADDR]);
    uint32_t a7_len = emu_le32(&d->rom[DS_HDR_ARM7_SIZE]);

    if (a9_len > 0 && a9_dst <= 0x02400000u &&
        a9_dst + a9_len <= 0x02400000u && a9_off + a9_len <= d->rom_size) {
        memcpy(&d->main_ram[a9_dst - 0x02000000u], &d->rom[a9_off], a9_len);
    }
    if (a7_len > 0 && a7_dst <= 0x02400000u &&
        a7_dst + a7_len <= 0x02400000u && a7_off + a7_len <= d->rom_size) {
        memcpy(&d->main_ram[a7_dst - 0x02000000u], &d->rom[a7_off], a7_len);
    }

    arm_reset(&d->a9.cpu);
    arm_reset(&d->a7.cpu);
    d->a9.cpu.cpsr = ARM_MODE_SYS | ARM_F_I | ARM_F_F;
    d->a7.cpu.cpsr = ARM_MODE_SYS | ARM_F_I | ARM_F_F;
    d->a9.cpu.pc = a9_adr;
    d->a7.cpu.pc = a7_adr;
    d->a9.cpu.r[13] = 0x023FC000u; /* documented stack convention */
    d->a7.cpu.r[13] = 0x0380FC00u;
    d->a9.cpu.r[15] = a9_adr + 8u;
    d->a7.cpu.r[15] = a7_adr + 8u;
    d->ipc9 = 0;
    d->ipc7 = 0;
    d->vcount = 0;
    d->frame_count = 0;
}

static emu_result_t ds_load_rom(emu_core_t *core, const uint8_t *data,
                                size_t size)
{
    struct ds *d = (struct ds *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    if (size < 0x200u)
        return EMU_EBADROM;
    static const uint8_t logo8[8] = {
        0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21
    };
    if (memcmp(data + DS_HDR_LOGO, logo8, sizeof logo8) != 0)
        return EMU_EBADROM;

    uint8_t *copy = malloc(size);
    if (copy == NULL)
        return EMU_EINVAL;
    memcpy(copy, data, size);
    free(d->rom);
    d->rom = copy;
    d->rom_size = size;

    /* clear writable machine memory (fresh boot) */
    memset(d->main_ram, 0, sizeof d->main_ram);
    memset(d->itcm, 0, sizeof d->itcm);
    memset(d->dtcm, 0, sizeof d->dtcm);
    memset(d->wram_sh, 0, sizeof d->wram_sh);
    memset(d->wram7, 0, sizeof d->wram7);
    memset(d->vram_a, 0, sizeof d->vram_a);
    memset(d->vram_b, 0, sizeof d->vram_b);
    memset(d->vram_c, 0, sizeof d->vram_c);
    memset(d->vram_d, 0, sizeof d->vram_d);
    memset(d->vram_e, 0, sizeof d->vram_e);
    memset(d->vram_f, 0, sizeof d->vram_f);
    memset(d->vram_g, 0, sizeof d->vram_g);
    memset(d->a9.io, 0, sizeof d->a9.io);
    memset(d->a7.io, 0, sizeof d->a7.io);
    for (int i = 0; i < 4; i++) {
        memset(&d->a9.tm[i], 0, sizeof(struct ds_timer));
        memset(&d->a7.tm[i], 0, sizeof(struct ds_timer));
    }
    d->a9.ie = 0;
    d->a9.if_latch = 0;
    d->a9.ime = 0;
    d->a7.ie = 0;
    d->a7.if_latch = 0;
    d->a7.ime = 0;
    d->cp15_c1 = 0x00050078u; /* documented reset value (stored only) */
    d->cp15_dtcm = 0x08000006u; /* 0x08000000, 16 KiB */
    d->cp15_itcm = 0x0100000Eu; /* 0x01000000, 32 KiB */

    ds_boot(d);
    ds_render(d);
    return EMU_OK;
}

static void ds_reset(emu_core_t *core)
{
    struct ds *d = (struct ds *)core;
    if (d->rom == NULL)
        return;
    ds_boot(d);
    ds_render(d);
}

static const uint32_t *ds_fb(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct ds *d = (struct ds *)core;
    if (w != NULL)
        *w = DS_SCREEN_W;
    if (h != NULL)
        *h = DS_FB_H;
    return d->fb;
}

static void ds_set_input(emu_core_t *core, uint32_t buttons)
{
    struct ds *d = (struct ds *)core;
    d->buttons = buttons & 0xFFFu;
}

static void ds_set_audio(emu_core_t *core, emu_audio_cb_t cb, void *user)
{
    (void)core; (void)cb; (void)user; /* sound not implemented: no audio */
}

/* ---- save states --------------------------------------------------------------------- */

static void ds_serialize(struct ds *d, emu_state_writer *w)
{
    sw_u32(w, 0x4D533241u); /* "A2SM" - state format rev 2 (palette/OAM,
                             * 2K io pages, engine-B registers) */
    /* ARM9 CPU */
    for (int i = 0; i < 16; i++)
        sw_u32(w, d->a9.cpu.r[i]);
    sw_u32(w, d->a9.cpu.pc);
    sw_u32(w, d->a9.cpu.cpsr);
    for (int i = 0; i < 6; i++) {
        sw_u32(w, d->a9.cpu.bank_r13[i]);
        sw_u32(w, d->a9.cpu.bank_r14[i]);
        sw_u32(w, d->a9.cpu.bank_spsr[i]);
    }
    for (int i = 0; i < 5; i++)
        sw_u32(w, d->a9.cpu.fiq_r8[i]);
    /* ARM7 CPU */
    for (int i = 0; i < 16; i++)
        sw_u32(w, d->a7.cpu.r[i]);
    sw_u32(w, d->a7.cpu.pc);
    sw_u32(w, d->a7.cpu.cpsr);
    for (int i = 0; i < 6; i++) {
        sw_u32(w, d->a7.cpu.bank_r13[i]);
        sw_u32(w, d->a7.cpu.bank_r14[i]);
        sw_u32(w, d->a7.cpu.bank_spsr[i]);
    }
    for (int i = 0; i < 5; i++)
        sw_u32(w, d->a7.cpu.fiq_r8[i]);
    /* machine */
    for (int i = 0; i < 4; i++) {
        sw_u16(w, d->a9.tm[i].reload);
        sw_u16(w, d->a9.tm[i].cnt);
        sw_u32(w, d->a9.tm[i].counter);
    }
    for (int i = 0; i < 4; i++) {
        sw_u16(w, d->a7.tm[i].reload);
        sw_u16(w, d->a7.tm[i].cnt);
        sw_u32(w, d->a7.tm[i].counter);
    }
    sw_u32(w, d->a9.ie);
    sw_u32(w, d->a9.if_latch);
    sw_u32(w, d->a9.ime);
    sw_u32(w, d->a7.ie);
    sw_u32(w, d->a7.if_latch);
    sw_u32(w, d->a7.ime);
    sw_u32(w, d->ipc9);
    sw_u32(w, d->ipc7);
    sw_u32(w, d->cp15_c1);
    sw_u32(w, d->cp15_dtcm);
    sw_u32(w, d->cp15_itcm);
    sw_u32(w, d->vcount);
    sw_u32(w, d->frame_count);
    sw_u32(w, d->buttons);
    sw_mem(w, d->a9.io, sizeof d->a9.io);
    sw_mem(w, d->a7.io, sizeof d->a7.io);
    sw_mem(w, d->main_ram, sizeof d->main_ram);
    sw_mem(w, d->itcm, sizeof d->itcm);
    sw_mem(w, d->dtcm, sizeof d->dtcm);
    sw_mem(w, d->wram_sh, sizeof d->wram_sh);
    sw_mem(w, d->wram7, sizeof d->wram7);
    sw_mem(w, d->vram_a, sizeof d->vram_a);
    sw_mem(w, d->vram_b, sizeof d->vram_b);
    sw_mem(w, d->vram_c, sizeof d->vram_c);
    sw_mem(w, d->vram_d, sizeof d->vram_d);
    sw_mem(w, d->vram_e, sizeof d->vram_e);
    sw_mem(w, d->vram_f, sizeof d->vram_f);
    sw_mem(w, d->vram_g, sizeof d->vram_g);
    sw_mem(w, d->pal, sizeof d->pal);
    sw_mem(w, d->oam_a, sizeof d->oam_a);
    sw_mem(w, d->oam_b, sizeof d->oam_b);
}

static size_t ds_state_size(emu_core_t *core)
{
    struct ds *d = (struct ds *)core;
    emu_state_writer w = { NULL, 0, 0, 0 };
    ds_serialize(d, &w);
    return w.pos;
}

static emu_result_t ds_save_state(emu_core_t *core, uint8_t *buf, size_t cap)
{
    struct ds *d = (struct ds *)core;
    emu_state_writer w = { buf, cap, 0, 0 };
    ds_serialize(d, &w);
    if (w.overflow)
        return EMU_ENOSPACE;
    return EMU_OK;
}

static emu_result_t ds_load_state(emu_core_t *core, const uint8_t *buf,
                                  size_t size)
{
    struct ds *d = (struct ds *)core;
    emu_state_reader rd = { buf, size, 0, 0 };
    if (sr_u32(&rd) != 0x4D533241u)
        return EMU_EBADSTATE;
    for (int i = 0; i < 16; i++)
        d->a9.cpu.r[i] = sr_u32(&rd);
    d->a9.cpu.pc = sr_u32(&rd);
    d->a9.cpu.cpsr = sr_u32(&rd);
    for (int i = 0; i < 6; i++) {
        d->a9.cpu.bank_r13[i] = sr_u32(&rd);
        d->a9.cpu.bank_r14[i] = sr_u32(&rd);
        d->a9.cpu.bank_spsr[i] = sr_u32(&rd);
    }
    for (int i = 0; i < 5; i++)
        d->a9.cpu.fiq_r8[i] = sr_u32(&rd);
    for (int i = 0; i < 16; i++)
        d->a7.cpu.r[i] = sr_u32(&rd);
    d->a7.cpu.pc = sr_u32(&rd);
    d->a7.cpu.cpsr = sr_u32(&rd);
    for (int i = 0; i < 6; i++) {
        d->a7.cpu.bank_r13[i] = sr_u32(&rd);
        d->a7.cpu.bank_r14[i] = sr_u32(&rd);
        d->a7.cpu.bank_spsr[i] = sr_u32(&rd);
    }
    for (int i = 0; i < 5; i++)
        d->a7.cpu.fiq_r8[i] = sr_u32(&rd);
    for (int i = 0; i < 4; i++) {
        d->a9.tm[i].reload = sr_u16(&rd);
        d->a9.tm[i].cnt = sr_u16(&rd);
        d->a9.tm[i].counter = sr_u32(&rd);
    }
    for (int i = 0; i < 4; i++) {
        d->a7.tm[i].reload = sr_u16(&rd);
        d->a7.tm[i].cnt = sr_u16(&rd);
        d->a7.tm[i].counter = sr_u32(&rd);
    }
    d->a9.ie = sr_u32(&rd);
    d->a9.if_latch = sr_u32(&rd);
    d->a9.ime = sr_u32(&rd);
    d->a7.ie = sr_u32(&rd);
    d->a7.if_latch = sr_u32(&rd);
    d->a7.ime = sr_u32(&rd);
    d->ipc9 = sr_u32(&rd);
    d->ipc7 = sr_u32(&rd);
    d->cp15_c1 = sr_u32(&rd);
    d->cp15_dtcm = sr_u32(&rd);
    d->cp15_itcm = sr_u32(&rd);
    d->vcount = sr_u32(&rd);
    d->frame_count = sr_u32(&rd);
    d->buttons = sr_u32(&rd);
    sr_mem(&rd, d->a9.io, sizeof d->a9.io);
    sr_mem(&rd, d->a7.io, sizeof d->a7.io);
    sr_mem(&rd, d->main_ram, sizeof d->main_ram);
    sr_mem(&rd, d->itcm, sizeof d->itcm);
    sr_mem(&rd, d->dtcm, sizeof d->dtcm);
    sr_mem(&rd, d->wram_sh, sizeof d->wram_sh);
    sr_mem(&rd, d->wram7, sizeof d->wram7);
    sr_mem(&rd, d->vram_a, sizeof d->vram_a);
    sr_mem(&rd, d->vram_b, sizeof d->vram_b);
    sr_mem(&rd, d->vram_c, sizeof d->vram_c);
    sr_mem(&rd, d->vram_d, sizeof d->vram_d);
    sr_mem(&rd, d->vram_e, sizeof d->vram_e);
    sr_mem(&rd, d->vram_f, sizeof d->vram_f);
    sr_mem(&rd, d->vram_g, sizeof d->vram_g);
    sr_mem(&rd, d->pal, sizeof d->pal);
    sr_mem(&rd, d->oam_a, sizeof d->oam_a);
    sr_mem(&rd, d->oam_b, sizeof d->oam_b);
    if (rd.bad)
        return EMU_EBADSTATE;
    return EMU_OK;
}

/* ---- vtable --------------------------------------------------------------------------- */

static emu_result_t ds_create(emu_core_t **out);
static void ds_destroy(emu_core_t *core);

static const emu_core_vtable_t ds_vtable = {
    "mds-a", "Nintendo DS", DS_SCREEN_W, DS_FB_H, DS_OUT_RATE,
    ds_create, ds_destroy, ds_load_rom, ds_reset,
    ds_run_frame, ds_fb, ds_set_input, ds_set_audio,
    ds_state_size, ds_save_state, ds_load_state,
};

static emu_result_t ds_create(emu_core_t **out)
{
    struct ds *d = calloc(1, sizeof *d);
    if (d == NULL)
        return EMU_EINVAL;
    d->base.vtable = &ds_vtable;
    arm_init(&d->a9.cpu, &d->bus9, &ds9_cp15);
    d->a9.cpu.v5te = 1; /* ARM946E-S feature set */
    arm_init(&d->a7.cpu, &d->bus7, NULL);
    d->bus9 = arm9_bus_tmpl;
    d->bus7 = arm7_bus_tmpl;
    d->bus9.user = d;
    d->bus7.user = d;
    d->cp15_dtcm = 0x08000006u;
    d->cp15_itcm = 0x0100000Eu;
    *out = &d->base;
    return EMU_OK;
}

static void ds_destroy(emu_core_t *core)
{
    struct ds *d = (struct ds *)core;
    if (d == NULL)
        return;
    free(d->rom);
    free(d);
}

const emu_core_vtable_t *emu_core_mds_a(void)
{
    return &ds_vtable;
}

/* ---- test/inspection hooks ------------------------------------------------------- */

uint32_t ds9_read32(struct ds *d, uint32_t addr)
{
    return a9_read32(d, addr);
}

void ds9_write32(struct ds *d, uint32_t addr, uint32_t v)
{
    a9_write32(d, addr, v);
}

uint16_t ds9_read16(struct ds *d, uint32_t addr)
{
    return a9_read16(d, addr);
}

void ds9_write16(struct ds *d, uint32_t addr, uint16_t v)
{
    a9_write16(d, addr, v);
}

uint32_t ds7_read32(struct ds *d, uint32_t addr)
{
    return a7_read32(d, addr);
}

void ds7_write32(struct ds *d, uint32_t addr, uint32_t v)
{
    a7_write32(d, addr, v);
}
