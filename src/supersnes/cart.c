/*
 * supersnes cartridge: LoROM / HiROM mapping with score-based header
 * detection (checksum complement consistency preferred).
 */
#include "snes.h"

#include <stdlib.h>
#include <string.h>

void snes_cart_init(snes_cart *c)
{
    memset(c, 0, sizeof *c);
}

void snes_cart_free(snes_cart *c)
{
    free(c->rom);
    free(c->sram);
    memset(c, 0, sizeof *c);
}

/* score a candidate header: mapping mode byte + checksum consistency */
static int header_score(const uint8_t *data, size_t size, size_t off, int lorom)
{
    if (off + 0x40u > size)
        return -1;
    const uint8_t *h = &data[off];
    uint8_t map = h[0x15];
    if (lorom) {
        if ((map & 0x0Fu) != 0x00u && (map & 0x0Fu) != 0x02u)
            return -1;
    } else {
        if ((map & 0x0Fu) != 0x01u && (map & 0x0Fu) != 0x03u)
            return -1;
    }
    uint16_t complement = (uint16_t)(h[0x1C] | (h[0x1D] << 8));
    uint16_t checksum = (uint16_t)(h[0x1E] | (h[0x1F] << 8));
    if ((uint16_t)(complement ^ 0xFFFFu) == checksum && checksum != 0u)
        return 2;
    if (checksum == complement)
        return 1;
    return 0;
}

emu_result_t snes_cart_load(snes_cart *c, const uint8_t *data, size_t size)
{
    snes_cart_free(c);
    if (size < 0x8000u)
        return EMU_EBADROM;

    int best = -1, best_lorom = 1;
    size_t best_off = 0;
    struct { size_t off; int lorom; } candidates[] = {
        { 0x7FC0u, 1 }, { 0xFFC0u, 0 }, { 0x40FFC0u % size, 0 }, { 0x407FC0u % size, 1 }
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        int sc = header_score(data, size, candidates[i].off, candidates[i].lorom);
        if (sc > best) {
            best = sc;
            best_off = candidates[i].off;
            best_lorom = candidates[i].lorom;
        }
    }
    if (best < 0)
        return EMU_EBADROM;
    (void)best_off; /* only the mapping mode matters for this milestone */

    c->rom = malloc(size);
    if (c->rom == NULL)
        return EMU_EINVAL;
    memcpy(c->rom, data, size);
    c->rom_size = size;
    c->lorom = (uint8_t)best_lorom;

    /* title for diagnostics */
    memcpy(c->title, &data[best_off], 21);
    c->title[21] = '\0';

    /* SRAM size from header byte (0x7FD4/0xFFD4 style: log2-based); accept 0 */
    uint8_t sram_code = data[best_off + 0x18];
    size_t sram = 0;
    if (sram_code != 0u && sram_code <= 5u)
        sram = (size_t)1024u << sram_code;
    c->sram_size = sram;
    if (sram > 0u) {
        c->sram = malloc(sram);
        if (c->sram == NULL) {
            snes_cart_free(c);
            return EMU_EINVAL;
        }
        memset(c->sram, 0, sram);
    }
    return EMU_OK;
}

uint8_t snes_cart_read(snes_cart *c, uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t offset = (uint16_t)(addr & 0xFFFFu);

    if (c->lorom) {
        if (offset < 0x8000u)
            return 0xFFu;
        uint32_t file = ((uint32_t)(bank & 0x3Fu) << 15) | (uint32_t)(offset & 0x7FFFu);
        if (file < c->rom_size)
            return c->rom[file];
        return 0xFFu;
    }
    uint32_t file = ((uint32_t)(bank & 0x3Fu) << 16) | offset;
    if (file < c->rom_size)
        return c->rom[file];
    return 0xFFu;
}

void snes_cart_write(snes_cart *c, uint32_t addr, uint8_t v)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t offset = (uint16_t)(addr & 0xFFFFu);

    /* SRAM: LoROM 00-3F/80-BF:6000-7FFF, HiROM 20-3F/A0-BF:6000-7FFF */
    if (offset >= 0x6000u && offset < 0x8000u && c->sram != NULL) {
        size_t off = ((size_t)(bank & 3u) * 0x2000u) + (offset - 0x6000u);
        if (off < c->sram_size)
            c->sram[off] = v;
        return;
    }
    /* ROM writes ignored (no special-chip mapping; documented) */
    (void)c;
}
