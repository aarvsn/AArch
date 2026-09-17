#include "fbh.h"
#include "emu/emu.h"
#include "finalburn/fb_md.h"

#include <stdlib.h>
#include <string.h>

void fb_rom_w16(uint8_t *rom, size_t off, uint16_t v)
{
    rom[off] = (uint8_t)(v >> 8);
    rom[off + 1] = (uint8_t)v;
}

void fb_rom_w32(uint8_t *rom, size_t off, uint32_t v)
{
    fb_rom_w16(rom, off, (uint16_t)(v >> 16));
    fb_rom_w16(rom, off + 2, (uint16_t)v);
}

uint8_t *fb_test_rom(size_t *size_out, uint32_t entry)
{
    size_t size = 0x20000; /* 128 KiB */
    uint8_t *rom = calloc(1, size);
    if (rom == NULL)
        return NULL;
    memcpy(rom + 0x100, "SEGA GENESIS    ", 16);
    /* initial SSP and PC (big-endian vectors at 0 and 4) */
    fb_rom_w32(rom, 0, 0x01000000u);
    fb_rom_w32(rom, 4, entry);
    *size_out = size;
    return rom;
}

struct fb_md *fb_boot(uint8_t *rom, size_t size)
{
    emu_core_t *core = NULL;
    if (emu_core_create(emu_core_finalburn(), &core) != EMU_OK)
        return NULL;
    if (emu_core_finalburn()->load_rom(core, rom, size) != EMU_OK) {
        emu_core_destroy(core);
        return NULL;
    }
    return (struct fb_md *)core;
}

struct fb_md *fb_create_bare(void)
{
    emu_core_t *core = NULL;
    if (emu_core_create(emu_core_finalburn(), &core) != EMU_OK)
        return NULL;
    return (struct fb_md *)core;
}

void fb_frame(struct fb_md *md)
{
    emu_core_finalburn()->run_frame(&md->base);
}
