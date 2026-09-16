/* mgbax cartridge: ROM image + 64 KiB backup media (SRAM-style 8-bit). */
#include "gba.h"

#include <stdlib.h>
#include <string.h>

void gba_cart_init(gba_cart *c)
{
    memset(c, 0, sizeof *c);
}

void gba_cart_free(gba_cart *c)
{
    free(c->rom);
    c->rom = NULL;
    c->rom_size = 0;
}

emu_result_t gba_cart_load(gba_cart *c, const uint8_t *data, size_t size)
{
    gba_cart_free(c);
    if (size < 0x100u || size > 32u * 1024u * 1024u)
        return EMU_EBADROM;
    c->rom = malloc(size);
    if (c->rom == NULL)
        return EMU_EINVAL;
    memcpy(c->rom, data, size);
    c->rom_size = size;
    memset(c->sram, 0xFF, sizeof c->sram);
    return EMU_OK;
}

uint8_t gba_cart_read8(gba_cart *c, uint32_t addr)
{
    uint32_t region = addr >> 24;
    if (region == 0x0Eu || region == 0x0Fu)
        return c->sram[addr & 0xFFFFu];
    if (addr < c->rom_size)
        return c->rom[addr];
    return 0x00u; /* open bus beyond ROM: documented simplification */
}

void gba_cart_write8(gba_cart *c, uint32_t addr, uint8_t v)
{
    uint32_t region = addr >> 24;
    if (region == 0x0Eu || region == 0x0Fu)
        c->sram[addr & 0xFFFFu] = v;
}
