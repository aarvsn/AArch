/* Generates minimal valid ROMs for each core for CLI smoke tests. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }

int main(void)
{
    /* GB: 2 banks, ROM-only, valid logo + header checksum */
    {
        size_t sz = 0x8000;
        uint8_t *rom = calloc(1, sz);
        static const uint8_t logo[0x30] = {
            0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
            0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
            0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
        };
        memcpy(rom + 0x104, logo, sizeof logo);
        rom[0x147] = 0x00; /* ROM only */
        rom[0x148] = 0x00; /* 32 KiB = 2 banks */
        unsigned x = 0;
        for (int i = 0x134; i <= 0x14C; i++) x = (x - rom[i] - 1) & 0xFF;
        rom[0x14D] = (uint8_t)x;
        rom[0x150] = 0x00; /* NOP */
        FILE *f = fopen("/tmp/smoke_gb.gb", "wb");
        fwrite(rom, 1, sz, f); fclose(f); free(rom);
    }
    /* NES: iNES, mapper 0, 2 PRG + 1 CHR */
    {
        size_t sz = 16 + 0x8000 + 0x2000;
        uint8_t *rom = calloc(1, sz);
        memcpy(rom, "NES\x1A", 4);
        rom[4] = 2; rom[5] = 1;
        rom[6] = 0x00;
        for (size_t i = 16; i < sz; i++) rom[i] = 0xEA; /* NOPs */
        put16(rom + 16, 0x18); /* IRQ vector */
        FILE *f = fopen("/tmp/smoke_nes.nes", "wb");
        fwrite(rom, 1, sz, f); fclose(f); free(rom);
    }
    /* SNES LoROM: 16 banks of 32 KiB with a header in bank 0 */
    {
        size_t sz = 16 * 0x8000;
        uint8_t *rom = calloc(1, sz);
        uint8_t *hdr = rom + 0x7FC0;
        memcpy(hdr, "SMOKE", 5);
        hdr[0x15] = 0x20; /* LoROM */
        hdr[0x17] = 0x03; /* 128 KiB cart RAM? keep simple */
        for (size_t i = 0; i < sz; i += 2) { rom[i] = 0xC2; rom[i+1] = 0x30; } /* JML $30xxxx? NOPs-ish */
        FILE *f = fopen("/tmp/smoke_snes.sfc", "wb");
        fwrite(rom, 1, sz, f); fclose(f); free(rom);
    }
    /* GBA: 4 MiB-ish of Thumb NOPs + a loop */
    {
        size_t sz = 0x100000;
        uint8_t *rom = calloc(1, sz);
        for (size_t i = 0; i < sz; i += 2) { rom[i] = 0xFE; rom[i+1] = 0xE7; } /* B self */
        FILE *f = fopen("/tmp/smoke_gba.gba", "wb");
        fwrite(rom, 1, sz, f); fclose(f); free(rom);
    }
    printf("smoke ROMs written\n");
    return 0;
}
