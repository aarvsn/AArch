/*
 * finalburn: cartridge (ROM + optional 64 KiB SRAM at $200000).
 *
 * ROM mirroring: power-of-two images mirror with a mask; non-power-of-two
 * images use modulo. Reads beyond the image inside the cart area therefore
 * wrap like on a real multicart-era board; areas far beyond any real cart
 * ($800000+) are open bus handled by the bus layer.
 *
 * SRAM is enabled by the standard "RA" / SRAM header at the end of the
 * domestic/overseas name fields (0x1B0/0x1B1 == 'R','A' and 0x1B2 != 0x20)
 * or, failing that, when a write lands in the SRAM window (documented
 * approximation: write-enable is not gated by the header alone).
 */
#include "fb_md.h"

#include <stdlib.h>
#include <string.h>

void fb_cart_init(struct fb_cart *c)
{
    memset(c, 0, sizeof *c);
}

void fb_cart_free(struct fb_cart *c)
{
    free(c->rom);
    free(c->sram);
    memset(c, 0, sizeof *c);
}

static int is_pow2(size_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

emu_result_t fb_cart_load(struct fb_cart *c, const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    /* 512-byte interleaved .smd files are rejected; raw images only. */
    if (size >= 512 && (size % 512) == 1)
        return EMU_EBADROM;
    if (size > 8u * 1024u * 1024u)
        return EMU_EBADROM;

    uint8_t *rom = malloc(size);
    if (rom == NULL)
        return EMU_EINVAL;
    memcpy(rom, data, size);

    fb_cart_free(c);
    c->rom = rom;
    c->rom_size = size;
    c->rom_mask = is_pow2(size) ? (uint32_t)(size - 1) : 0;

    /* SRAM header detection (documented heuristic) */
    if (size >= 0x1B4 && rom[0x1B0] == 'R' && rom[0x1B1] == 'A') {
        c->sram = calloc(1, 0x10000);
        if (c->sram == NULL)
            return EMU_EINVAL;
        c->sram_en = 1;
    }
    return EMU_OK;
}

uint8_t fb_cart_read8(struct fb_cart *c, uint32_t addr)
{
    if (c->rom == NULL || c->rom_size == 0)
        return 0xFF; /* no cartridge: open bus (reset vectors read 0xFF) */
    if (c->sram_en && addr >= 0x200000 && addr < 0x20FFFF + 1)
        return c->sram[addr & 0xFFFFu];
    uint32_t off;
    if (c->rom_mask)
        off = addr & c->rom_mask;
    else
        off = addr % (uint32_t)c->rom_size;
    return c->rom[off];
}

void fb_cart_sram_write8(struct fb_cart *c, uint32_t addr, uint8_t v)
{
    if (c->sram == NULL) {
        c->sram = calloc(1, 0x10000);
        if (c->sram == NULL)
            return;
        c->sram_en = 1;
    }
    c->sram[addr & 0xFFFFu] = v;
}

uint8_t fb_cart_sram_read8(struct fb_cart *c, uint32_t addr)
{
    if (c->sram == NULL)
        return 0xFF;
    return c->sram[addr & 0xFFFFu];
}
