/* mgbx bus: address decoding for the DMG memory map. */
#include "mgbx.h"

void gb_request_interrupt(struct mgbx *gb, uint8_t bit)
{
    gb->mem.if_reg |= (uint8_t)(1u << bit);
}

uint8_t gb_bus_read(struct mgbx *gb, uint16_t addr)
{
    if (addr < 0x8000u)
        return gb_cart_read(&gb->cart, addr);
    if (addr < 0xA000u)
        return gb_ppu_vram_read(&gb->ppu, addr);
    if (addr < 0xC000u)
        return gb_cart_read(&gb->cart, addr);
    if (addr < 0xFE00u) { /* WRAM (C000-DFFF) + echo (E000-FDFF) */
        return gb->mem.wram[addr & 0x1FFFu];
    }
    if (addr < 0xFEA0u)
        return gb_ppu_oam_read(&gb->ppu, addr);
    if (addr < 0xFF00u)
        return 0x00; /* prohibited region: reads 0x00 (documented choice) */

    switch (addr) {
    case 0xFF00: return gb_joypad_read(&gb->joypad);
    case 0xFF01: return gb->mem.sb;
    case 0xFF02: return (uint8_t)(gb->mem.sc | 0x7Eu);
    case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
        return gb_timer_read(&gb->timer, addr);
    case 0xFF0F: return (uint8_t)(gb->mem.if_reg | 0xE0u);
    case 0xFF46: return gb->mem.oam_dma_page;
    case 0xFF4D: return 0xFFu; /* KEY1: CGB only */
    case 0xFF4F: return 0xFFu; /* VBK: CGB only */
    case 0xFF50: return 0xFFu;
    default:
        break;
    }
    if (addr >= 0xFF10u && addr <= 0xFF3Fu)
        return gb_apu_read(&gb->apu, addr);
    if (addr >= 0xFF40u && addr <= 0xFF4Bu)
        return gb_ppu_read_reg(&gb->ppu, addr);
    if (addr >= 0xFF80u && addr <= 0xFFFEu)
        return gb->mem.hram[addr - 0xFF80u];
    if (addr == 0xFFFFu)
        return gb->mem.ie;
    return 0xFFu; /* unmapped I/O */
}

void gb_bus_write(struct mgbx *gb, uint16_t addr, uint8_t v)
{
    if (addr < 0x8000u) {
        gb_cart_write(&gb->cart, addr, v);
        return;
    }
    if (addr < 0xA000u) {
        gb_ppu_vram_write(&gb->ppu, addr, v);
        return;
    }
    if (addr < 0xC000u) {
        gb_cart_write(&gb->cart, addr, v);
        return;
    }
    if (addr < 0xFE00u) { /* WRAM (C000-DFFF) + echo (E000-FDFF) */
        gb->mem.wram[addr & 0x1FFFu] = v;
        return;
    }
    if (addr < 0xFEA0u) {
        gb_ppu_oam_write(&gb->ppu, addr, v);
        return;
    }
    if (addr < 0xFF00u)
        return; /* prohibited: ignored */

    switch (addr) {
    case 0xFF00:
        gb_joypad_write(&gb->joypad, v, gb);
        return;
    case 0xFF01:
        gb->mem.sb = v;
        return;
    case 0xFF02:
        gb->mem.sc = (uint8_t)(v & 0x81u);
        return; /* serial transfers not implemented (documented) */
    case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
        gb_timer_write(gb, addr, v);
        return;
    case 0xFF0F:
        gb->mem.if_reg = (uint8_t)(v & 0x1Fu);
        return;
    case 0xFF46: /* OAM DMA */
    {
        uint8_t page = v > 0xF1u ? (uint8_t)0xF1u : v;
        gb->mem.oam_dma_page = page;
        gb->mem.dma_active = 1;
        gb->mem.dma_index = 0;
        gb->mem.dma_value = gb_bus_read(gb, (uint16_t)(page * 0x100u));
        return;
    }
    case 0xFF50:
        return; /* boot ROM disable: no boot ROM is bundled */
    default:
        break;
    }
    if (addr >= 0xFF10u && addr <= 0xFF3Fu) {
        gb_apu_write(&gb->apu, addr, v);
        return;
    }
    if (addr >= 0xFF40u && addr <= 0xFF4Bu) {
        gb_ppu_write_reg(gb, addr, v);
        return;
    }
    if (addr >= 0xFF80u && addr <= 0xFFFEu) {
        gb->mem.hram[addr - 0xFF80u] = v;
        return;
    }
    if (addr == 0xFFFFu) {
        gb->mem.ie = v;
        return;
    }
    /* unmapped I/O: ignored */
}
