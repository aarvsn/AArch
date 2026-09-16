/* beatle-nes-redux bus: CPU address decoding. */
#include "nes.h"

uint8_t nes_bus_read(nes_t *n, uint16_t addr)
{
    if (addr < 0x2000u)
        return n->bus.ram[addr & 0x07FFu];
    if (addr < 0x4000u)
        return nes_ppu_read(n, (uint16_t)(0x2000u + (addr & 7u)));
    if (addr == 0x4016u) {
        /* controller 1 shift register; returns 1 once exhausted */
        uint8_t v = (uint8_t)(0x40u | (n->ctrl.shift & 1u));
        if (n->ctrl.strobe == 0)
            n->ctrl.shift = (uint8_t)((n->ctrl.shift >> 1) | 0x80u);
        return v;
    }
    if (addr == 0x4015u)
        return nes_apu_read_status(&n->apu);
    if (addr == 0x4017u)
        return 0x40u; /* controller 2 not implemented (documented) */
    if (addr < 0x4020u)
        return n->ppu.open_bus; /* unmapped APU/IO reads */
    if (addr < 0x6000u)
        return 0x00; /* expansion region: not implemented */
    if (addr < 0x8000u)
        return n->cart.prg_ram != NULL ? n->cart.prg_ram[addr - 0x6000u] : 0x00u;
    return nes_cart_cpu_read(&n->cart, addr);
}

void nes_bus_write(nes_t *n, uint16_t addr, uint8_t v)
{
    n->ppu.open_bus = v; /* simplified open-bus decay model */

    if (addr < 0x2000u) {
        n->bus.ram[addr & 0x07FFu] = v;
        return;
    }
    if (addr < 0x4000u) {
        nes_ppu_write(n, (uint16_t)(0x2000u + (addr & 7u)), v);
        return;
    }
    if (addr == 0x4014u) { /* OAM DMA */
        uint16_t page = (uint16_t)(v << 8);
        for (int i = 0; i < 256; i++)
            n->ppu.oam[(uint8_t)(n->ppu.oam_addr + (uint8_t)i)] =
                nes_bus_read(n, (uint16_t)(page + (uint8_t)i));
        n->cpu.dma_stall += 513;
        return;
    }
    if (addr == 0x4016u) {
        uint8_t prev = n->ctrl.strobe;
        n->ctrl.strobe = (uint8_t)(v & 1u);
        if (prev && !n->ctrl.strobe) {
            /* latch on 1->0; while strobe is high the register reloads */
        }
        if (n->ctrl.strobe)
            n->ctrl.shift = (uint8_t)(n->ctrl.buttons & 0xFFu);
        return;
    }
    if (addr < 0x4018u) {
        nes_apu_write(&n->apu, addr, v);
        return;
    }
    if (addr < 0x4020u)
        return;
    if (addr < 0x6000u)
        return; /* expansion: not implemented */
    if (addr < 0x8000u) {
        if (n->cart.prg_ram != NULL)
            n->cart.prg_ram[addr - 0x6000u] = v;
        return;
    }
    nes_cart_cpu_write(&n->cart, addr, v);
}
