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
    uint8_t *rom = nes_make_rom(2, 1, 0, 0x01, &sz);
    static const uint8_t prog[] = { 0xA9, 0x99, 0x85, 0x00, 0xA2, 0x05, 0xB5, 0xFB, 0x4C, 0x08, 0xC0 };
    memcpy(&rom[16 + 0x4000], prog, sizeof prog);
    rom[16 + 0x7FFC] = 0x00; rom[16 + 0x7FFD] = 0xC0;
    emu_core_beatle_nes_redux()->load_rom(c, rom, sz);
    free(rom);
    printf("pc=%04X p=%02X\n", n->cpu.pc, n->cpu.p);
    for (int i = 0; i < 5; i++) {
        nes_cpu_step(n);
        printf("step%d pc=%04X a=%02X x=%02X ram[0]=%02X cyc=%llu\n", i, n->cpu.pc, n->cpu.a, n->cpu.x, n->bus.ram[0], (unsigned long long)n->total_cycles);
    }
    /* irq masked check */
    n->cpu.irq_line = 1;
    uint16_t pc0 = n->cpu.pc;
    nes_cpu_step(n);
    printf("irq test: pc=%04X (want %04X) p=%02X irq=%d\n", n->cpu.pc, pc0, n->cpu.p, n->cpu.irq_line);
    return 0;
}
