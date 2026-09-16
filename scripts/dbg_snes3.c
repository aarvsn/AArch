#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "supersnes/snes.h"
uint8_t *snes_make_lorom(uint32_t banks, size_t *size_out);
int main(void) {
    emu_core_t *c = NULL;
    emu_core_supersnes()->create(&c);
    struct snes *s = (struct snes *)c;
    size_t sz = 0;
    uint8_t *rom = snes_make_lorom(16, &sz);
    emu_core_supersnes()->load_rom(c, rom, sz);
    free(rom);
    for (int b = 0; b < 32; b++)
        s->ppu.vram[(uint32_t)b + 32u] = 0xFF;
    s->ppu.vram[0] = 0x01;
    s->ppu.vram[1] = 0x04;
    s->ppu.cgram[0x26] = 0xFF;
    s->ppu.cgram[0x27] = 0x7F;
    s->ppu.bgmode = 0x01;
    s->ppu.tm = 0x01;
    s->ppu.inidisp = 0x0F;
    s->ppu.line = 0;
    printf("before: line=%d dot=%d tm=%02X bgmode=%02X inidisp=%02X\n",
           s->ppu.line, s->ppu.dot, s->ppu.tm, s->ppu.bgmode, s->ppu.inidisp);
    snes_ppu_run(s, 1364u * 4u);
    printf("after: line=%d dot=%d\n", s->ppu.line, s->ppu.dot);
    printf("fb[0]=%08X fb[1]=%08X\n", s->ppu.fb[0], s->ppu.fb[1]);
    printf("entry=%04X tile=%d\n", s->ppu.vram[0] | (s->ppu.vram[1] << 8), 1);
    /* replicate text_bg_pixel fetch for pixel 0 */
    uint16_t h = s->ppu.bg_scroll[0], v = s->ppu.bg_scroll[1];
    uint16_t eff_x = 0 + (h & 0x3FF), eff_y = 0 + (v & 0x3FF);
    uint16_t tx = (eff_x >> 3) & 0x3F, ty = (eff_y >> 3) & 0x3F;
    uint16_t map_addr = 0 + (ty & 31) * 32 + (tx & 31);
    uint16_t entry2 = s->ppu.vram[map_addr*2] | (s->ppu.vram[map_addr*2+1] << 8);
    printf("h=%04X v=%04X tx=%d ty=%d map_addr=%04X entry=%04X\n", h, v, tx, ty, map_addr, entry2);
    uint32_t plane_off = (entry2 & 0x3FF) * 32;
    printf("plane_off=%u lo0=%02X hi0=%02X lo1=%02X hi1=%02X\n", plane_off,
           s->ppu.vram[plane_off], s->ppu.vram[plane_off+1],
           s->ppu.vram[plane_off+8], s->ppu.vram[plane_off+9]);
    return 0;
}
