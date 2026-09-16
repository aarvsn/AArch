#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "beatle-nes-redux/nes.h"
uint8_t *nes_make_rom(uint32_t prg_banks, uint32_t chr_banks, uint8_t mapper, uint8_t flags6_extra, size_t *size_out);
int main(void) {
    emu_core_t *c = NULL;
    emu_core_beatle_nes_redux()->create(&c);
    struct nes *n = (struct nes *)c;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(2, 1, 0, 0, &sz);
    emu_core_beatle_nes_redux()->load_rom(c, rom, sz);
    free(rom);
    for (int r = 0; r < 8; r++) {
        n->cart.chr[(size_t)r] = 0xFF;
        n->cart.chr[(size_t)r + 8] = 0xAA;
    }
    n->ppu.vram[0] = 0;
    n->ppu.vram[0x3C0] = 0xFF;
    n->ppu.palette[0x0F] = 0x27;
    n->ppu.ctrl = 0x00;
    n->ppu.mask = 0x08;
    n->ppu.scanline = 0;
    n->ppu.dot = 0;
    n->ppu.v = 0;
    printf("scanline=%d dot=%d mask=%02X\n", n->ppu.scanline, n->ppu.dot, n->ppu.mask);
    nes_ppu_run(n, 86);
    printf("after run: scanline=%d dot=%d\n", n->ppu.scanline, n->ppu.dot);
    printf("line_bg[0]=%02X [1]=%02X fb[0]=%08X\n", n->ppu.line_bg[0], n->ppu.line_bg[1], n->ppu.fb[0]);
    printf("chr[0]=%02X chr[8]=%02X vram[0]=%02X vram[3C0]=%02X\n", n->cart.chr[0], n->cart.chr[8], n->ppu.vram[0], n->ppu.vram[0x3C0]);
    return 0;
}
