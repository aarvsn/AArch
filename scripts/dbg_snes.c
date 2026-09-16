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
    static const uint8_t prog[] = { 0xE2, 0x20, 0x18, 0xA9, 0x80, 0x69, 0x80 };
    memcpy(&rom[0x0000], prog, sizeof prog);
    printf("load = %d\n", emu_core_supersnes()->load_rom(c, rom, sz));
    free(rom);
    printf("cart.rom=%p size=%zu lorom=%d\n", (void*)s->cart.rom, s->cart.rom_size, s->cart.lorom);
    printf("rom[0]=%02X\n", s->cart.rom ? s->cart.rom[0] : 0);
    s->cpu.pc = 0x8000;
    printf("fetch at 00:8000 = %02X (want E2)\n", snes_bus_read(s, 0x008000u));
    printf("fetch at 00:8001 = %02X (want 20)\n", snes_bus_read(s, 0x008001u));
    printf("fetch at 00:8004 = %02X (want 80)\n", snes_bus_read(s, 0x008004u));
    printf("fetch at 00:8003 = %02X (want A9)\n", snes_bus_read(s, 0x008003u));
    for (int i = 0; i < 4; i++) {
        uint32_t cyc = snes_cpu_step(s);
        printf("step%d cyc=%u pc=%04X A=%04X P=%02X (e=%d) rom[pc]=%02X\n", i, cyc, s->cpu.pc, s->cpu.a, s->cpu.p, s->cpu.e, s->cart.rom[s->cpu.pc & 0x7FFF]);
    }
    return 0;
}
