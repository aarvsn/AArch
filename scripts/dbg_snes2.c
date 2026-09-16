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
    static const uint8_t prog[] = { 0x18, 0xFB, 0xC2, 0x20, 0xA9, 0xFF, 0xFF, 0x69, 0x01, 0x00 };
    memcpy(&rom[0x0000], prog, sizeof prog);
    printf("load=%d\n", emu_core_supersnes()->load_rom(c, rom, sz));
    free(rom);
    s->cpu.pc = 0x8000;
    for (int i = 0; i < 5; i++) {
        uint32_t cyc = snes_cpu_step(s);
        printf("step%d cyc=%u pc=%04X A=%04X P=%02X e=%d\n", i, cyc, s->cpu.pc, s->cpu.a, s->cpu.p, s->cpu.e);
    }
    return 0;
}
