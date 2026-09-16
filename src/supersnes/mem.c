/*
 * supersnes memory map (bus), CPU I/O registers, hardware multiply/divide,
 * controller auto-read (active-low, latched at the start of VBlank).
 */
#include "snes.h"

#include <string.h>

uint8_t snes_bus_read(snes_t *s, uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t offset = (uint16_t)(addr & 0xFFFFu);
    snes_mem *m = &s->mem;

    if (bank == 0x7Eu || bank == 0x7Fu)
        return m->wram[((uint32_t)(bank - 0x7Eu) << 16) | offset];

    if (bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu)) {
        if (offset < 0x2000u)
            return m->wram[offset];
        if (offset >= 0x2100u && offset <= 0x213Fu)
            return snes_ppu_read(s, offset);
        if (offset >= 0x2140u && offset <= 0x217Fu)
            return snes_apu_read(&s->apu, offset);
        if (offset >= 0x4200u && offset <= 0x421Fu) {
            switch (offset) {
            case 0x4210: {
                uint8_t v = (uint8_t)(m->nmitimen & 0x80u);
                m->nmitimen &= (uint8_t)~0x80u; /* NMI flag cleared on read */
                return (uint8_t)(v | 0x42u);
            }
            case 0x4211: {
                uint8_t v = (uint8_t)(m->timeup & 0x80u);
                m->timeup &= (uint8_t)~0x80u;
                return v;
            }
            case 0x4212: {
                uint8_t v = 0;
                if (s->ppu.line >= 225u)
                    v |= 0x80u; /* vblank flag */
                if (s->ppu.dot >= 1280u || s->ppu.dot < 4u)
                    v |= 0x40u; /* approximate hblank window */
                return v;
            }
            case 0x4213: return 0xFFu; /* RDIO: parallel I/O stub */
            case 0x4214: return (uint8_t)(m->rddiv & 0xFFu);
            case 0x4215: return (uint8_t)(m->rddiv >> 8);
            case 0x4216: return (uint8_t)(m->rdmpy & 0xFFu);
            case 0x4217: return (uint8_t)(m->rdmpy >> 8);
            case 0x4218: return m->joypad_auto[0];
            case 0x4219: return m->joypad_auto[1];
            case 0x421A: return m->joypad_auto[2];
            case 0x421B: return m->joypad_auto[3];
            default: return s->ppu.openbus;
            }
        }
        if (offset >= 0x4300u && offset <= 0x437Fu) {
            uint8_t ch = (uint8_t)((offset >> 4) & 7u);
            uint8_t reg = (uint8_t)(offset & 0x0Fu);
            const snes_dma_channel *d = &s->dma.ch[ch];
            switch (reg) {
            case 0x0: return d->params;
            case 0x1: return d->bbus;
            case 0x2: return (uint8_t)(d->abus & 0xFFu);
            case 0x3: return (uint8_t)(d->abus >> 8);
            case 0x4: return d->abank;
            case 0x5: return (uint8_t)(d->count & 0xFFu);
            case 0x6: return (uint8_t)(d->count >> 8);
            case 0x7: return d->ibank;
            default: return s->ppu.openbus;
            }
        }
        if (offset < 0x6000u)
            return s->ppu.openbus;
        if (offset < 0x8000u)
            return m->wram[offset]; /* 6000-7FFF mirrors 7E:0000-1FFF */
        return snes_cart_read(&s->cart, addr);
    }

    /* banks 40-7D and C0-FF: cart regions */
    if ((bank >= 0x40u && bank <= 0x7Du) || bank >= 0xC0u) {
        if (offset >= 0x8000u || (bank >= 0x40u && bank <= 0x7Du && offset >= 0x6000u))
            return snes_cart_read(&s->cart, addr);
        return s->ppu.openbus;
    }
    return s->ppu.openbus;
}

void snes_bus_write(snes_t *s, uint32_t addr, uint8_t v)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t offset = (uint16_t)(addr & 0xFFFFu);
    snes_mem *m = &s->mem;

    if (bank == 0x7Eu || bank == 0x7Fu) {
        m->wram[((uint32_t)(bank - 0x7Eu) << 16) | offset] = v;
        return;
    }

    if (bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu)) {
        if (offset < 0x2000u) {
            m->wram[offset] = v;
            return;
        }
        if (offset >= 0x2100u && offset <= 0x213Fu) {
            snes_ppu_write(s, offset, v);
            return;
        }
        if (offset >= 0x2140u && offset <= 0x217Fu) {
            snes_apu_write(&s->apu, offset, v);
            return;
        }
        if (offset >= 0x4200u && offset <= 0x421Fu) {
            switch (offset) {
            case 0x4200:
                m->nmitimen = v;
                break;
            case 0x4201:
                /* joypad strobe: bit6 falling edge latches controllers */
                if (!(v & 0x40u))
                    m->joypad_latch = 1;
                else
                    m->joypad_latch = 0;
                break;
            case 0x4202:
                m->wrmpya = v;
                break;
            case 0x4203:
                m->wrmpyb = v;
                m->rdmpy = (uint16_t)((uint16_t)m->wrmpya * v);
                break;
            case 0x4204:
                m->wrdiv = (uint16_t)((m->wrdiv & 0xFF00u) | v);
                break;
            case 0x4205:
                m->wrdiv = (uint16_t)((m->wrdiv & 0x00FFu) | ((uint16_t)v << 8));
                break;
            case 0x4206:
                m->wrdivb = v;
                if (v == 0u) {
                    m->rddiv = 0xFFFFu;
                    m->rdmpy = (uint16_t)(m->wrdiv & 0xFFFFu);
                } else {
                    m->rddiv = (uint16_t)(m->wrdiv / v);
                    m->rdmpy = (uint16_t)(m->wrdiv % v);
                }
                break;
            case 0x4207: m->htime = (uint16_t)((m->htime & 0xFF00u) | v); break;
            case 0x4208: m->htime = (uint16_t)((m->htime & 0x00FFu) | ((uint16_t)(v & 1u) << 8)); break;
            case 0x4209: m->vtime = (uint16_t)((m->vtime & 0xFF00u) | v); break;
            case 0x420A: m->vtime = (uint16_t)((m->vtime & 0x00FFu) | ((uint16_t)(v & 1u) << 8)); break;
            case 0x420B:
                m->nmitimen = (uint8_t)((m->nmitimen & 0xF0u) | (v & 0x0Fu));
                if (v & 0x0Fu) {
                    uint32_t stolen = snes_dma_run(s);
                    s->total_cycles += (uint64_t)stolen;
                }
                break;
            case 0x420C: m->nmitimen = (uint8_t)((m->nmitimen & 0x0Fu) | (v & 0xF0u)); break;
            case 0x420D: m->fastrom = (uint8_t)(v & 1u); break;
            default: break;
            }
            return;
        }
        if (offset >= 0x4300u && offset <= 0x437Fu) {
            uint8_t ch = (uint8_t)((offset >> 4) & 7u);
            uint8_t reg = (uint8_t)(offset & 0x0Fu);
            snes_dma_channel *d = &s->dma.ch[ch];
            switch (reg) {
            case 0x0: d->params = v; break;
            case 0x1: d->bbus = v; break;
            case 0x2: d->abus = (uint16_t)((d->abus & 0xFF00u) | v); break;
            case 0x3: d->abus = (uint16_t)((d->abus & 0x00FFu) | ((uint16_t)v << 8)); break;
            case 0x4: d->abank = v; break;
            case 0x5: d->count = (uint16_t)((d->count & 0xFF00u) | v); break;
            case 0x6: d->count = (uint16_t)((d->count & 0x00FFu) | ((uint16_t)v << 8)); break;
            case 0x7: d->ibank = v; break;
            default: break;
            }
            return;
        }
        if (offset < 0x6000u)
            return;
        if (offset < 0x8000u) {
            m->wram[offset] = v; /* mirrors 7E:0000-1FFF */
            return;
        }
        snes_cart_write(&s->cart, addr, v);
        return;
    }

    if ((bank >= 0x40u && bank <= 0x7Du) || bank >= 0xC0u) {
        snes_cart_write(&s->cart, addr, v);
        return;
    }
}

void snes_mem_reset(snes_mem *m)
{
    memset(m, 0, sizeof *m);
}

void snes_mem_latch_joypads(snes_t *s)
{
    /* 4218/4219 per pad: bit0 Right, 1 Left, 2 Up, 3 Down, 4 Start,
     * 5 Select, 6 Y, 7 B; 4219: bit4 R, 5 L, 6 X, 7 A; active low. */
    static const uint32_t lo_bits[8] = { 7u, 6u, 5u, 4u, 5u, 4u, 6u, 7u };
    (void)lo_bits;
    for (int p = 0; p < 2; p++) {
        uint32_t b = s->buttons[p];
        uint8_t lo = 0xFFu, hi = 0x0Fu;
        if (b & (1u << 0)) lo &= (uint8_t)~0x80u; /* B */
        if (b & (1u << 1)) lo &= (uint8_t)~0x40u; /* Y */
        if (b & (1u << 2)) lo &= (uint8_t)~0x20u; /* Select */
        if (b & (1u << 3)) lo &= (uint8_t)~0x10u; /* Start */
        if (b & (1u << 4)) lo &= (uint8_t)~0x04u; /* Up */
        if (b & (1u << 5)) lo &= (uint8_t)~0x08u; /* Down */
        if (b & (1u << 6)) lo &= (uint8_t)~0x02u; /* Left */
        if (b & (1u << 7)) lo &= (uint8_t)~0x01u; /* Right */
        if (b & (1u << 8)) hi &= (uint8_t)~0x80u; /* A */
        if (b & (1u << 9)) hi &= (uint8_t)~0x40u; /* X */
        if (b & (1u << 10)) hi &= (uint8_t)~0x20u; /* L */
        if (b & (1u << 11)) hi &= (uint8_t)~0x10u; /* R */
        s->mem.joypad_auto[p * 2] = lo;
        s->mem.joypad_auto[p * 2 + 1] = hi;
    }
}
