/* Synthetic ROM builders for tests. */
#include "testutil.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *alloc_fill(size_t n, uint8_t v)
{
    uint8_t *p = malloc(n);
    if (p == NULL)
        abort();
    memset(p, v, n);
    return p;
}

uint8_t *gb_make_rom(uint32_t banks, uint8_t cart_type, uint8_t ram_code,
                     size_t *size_out)
{
    if (banks == 0)
        banks = 2;
    size_t size = (size_t)banks * GB_ROM_BANK_SIZE;
    uint8_t *rom = alloc_fill(size, 0x00);
    for (uint32_t b = 0; b < banks; b++)
        memset(&rom[(size_t)b * GB_ROM_BANK_SIZE], (uint8_t)(b & 0xFFu),
               GB_ROM_BANK_SIZE);

    /* Entry point at 0x0100: nop; jp 0x0150 (boring but valid). */
    rom[0x0100] = 0x00;
    rom[0x0101] = 0xC3;
    rom[0x0102] = 0x50;
    rom[0x0103] = 0x01;

    uint8_t *hdr = &rom[0x0134];
    memcpy(hdr, "EMUFWTEST", 9);      /* 0x134 title */
    hdr[0x147] = cart_type;           /* cartridge type */
    /* 0x148 ROM size code: log2(size / 32768) */
    uint8_t rom_code = 0;
    while ((0x8000u << rom_code) < size)
        rom_code++;
    hdr[0x148] = rom_code;
    hdr[0x149] = ram_code;
    hdr[0x14D] = 0x00;                /* header checksum not validated (no boot ROM) */
    if (size_out)
        *size_out = size;
    return rom;
}

uint8_t *nes_make_rom(uint32_t prg_banks, uint32_t chr_banks, uint8_t mapper,
                      uint8_t flags6_extra, size_t *size_out)
{
    if (prg_banks < 2)
        prg_banks = 2;
    size_t prg = (size_t)prg_banks * 0x4000u;
    size_t chr = (size_t)chr_banks * 0x2000u;
    size_t size = 16 + prg + chr;
    uint8_t *rom = alloc_fill(size, 0x00);
    rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'S'; rom[3] = 0x1A;
    rom[4] = (uint8_t)prg_banks;
    rom[5] = (uint8_t)chr_banks;
    rom[6] = (uint8_t)(flags6_extra | (uint8_t)((mapper & 0x0Fu) << 4));
    rom[7] = (uint8_t)((mapper & 0xF0u) & 0xF0u);
    for (size_t i = 0; i < prg; i++)
        rom[16 + i] = (uint8_t)(i & 0xFFu);
    for (size_t i = 0; i < chr; i++)
        rom[16 + prg + i] = (uint8_t)((i * 7 + 3) & 0xFFu);
    if (size_out)
        *size_out = size;
    return rom;
}

uint8_t *snes_make_lorom(uint32_t banks, size_t *size_out)
{
    if (banks == 0)
        banks = 16;
    size_t size = (size_t)banks * 0x8000u;
    uint8_t *rom = alloc_fill(size, 0x00);
    for (uint32_t b = 0; b < banks; b++)
        memset(&rom[(size_t)b * 0x8000u], (uint8_t)(b & 0xFFu), 0x8000u);

    /* SNES header at LoROM location: file offset 0x7FC0 == bank 0, 0x7FC0. */
    uint8_t *hdr = &rom[0x7FC0];
    memcpy(hdr, "EMUFW           ", 21);   /* title 21 bytes */
    hdr[0x15] = 0x20;   /* 0x7FD5 mapping: LoROM, slow */
    hdr[0x16] = 0x00;   /* 0x7FD6 chips: ROM only */
    hdr[0x17] = 0x08;   /* 0x7FD7 ROM size: log2 => 0x08 = 256 KiB */
    hdr[0x18] = 0x00;   /* 0x7FD8 RAM size: none */
    hdr[0x19] = 0x02;   /* 0x7FD9 country */
    hdr[0x1A] = 0x33;   /* 0x7FDA license */
    hdr[0x1B] = 0x01;   /* 0x7FDB version */
    uint16_t complement = 0x0000;   /* set below; checksum must equal ~complement */
    /* checksum = complement ^ 0xFFFF; any consistent pair passes validation */
    uint16_t checksum = (uint16_t)~complement;
    hdr[0x1C] = (uint8_t)(complement & 0xFFu);
    hdr[0x1D] = (uint8_t)((complement >> 8) & 0xFFu);
    hdr[0x1E] = (uint8_t)(checksum & 0xFFu);
    hdr[0x1F] = (uint8_t)((checksum >> 8) & 0xFFu);
    if (size_out)
        *size_out = size;
    return rom;
}

uint8_t *gba_make_rom(size_t size, uint8_t fill, size_t *size_out)
{
    size = (size + 3u) & ~(size_t)3u;
    if (size < 0x1000u)
        size = 0x1000u;
    uint8_t *rom = alloc_fill(size, fill);
    if (size_out)
        *size_out = size;
    return rom;
}
