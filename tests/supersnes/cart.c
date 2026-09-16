/*
 * supersnes cartridge tests + suite aggregator for the supersnes tests.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "supersnes/snes.h"

#include <stdlib.h>
#include <string.h>

void t_register_snes_cpu(void);
void t_register_snes_cart(void);
void t_register_snes_dma(void);
void t_register_snes_ppu(void);
void t_register_snes_state(void);

void t_register_supersnes(void)
{
    t_register_snes_cpu();
    t_register_snes_cart();
    t_register_snes_dma();
    t_register_snes_ppu();
    t_register_snes_state();
}

static struct snes *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_supersnes()->create(&c) != EMU_OK)
        return NULL;
    return (struct snes *)c;
}

static struct snes *mk_core_with_rom(void)
{
    struct snes *s = mk_core();
    if (s == NULL)
        return NULL;
    size_t sz = 0;
    uint8_t *rom = snes_make_lorom(16, &sz);
    emu_result_t r = emu_core_supersnes()->load_rom(&s->base, rom, sz);
    free(rom);
    if (r != EMU_OK) {
        emu_core_supersnes()->destroy(&s->base);
        return NULL;
    }
    return s;
}

static void cart_lorom_mapping(void)
{
    struct snes *s = mk_core_with_rom();
    T_CHECK(s != NULL);
    if (!s)
        return;
    T_CHECK_EQ(s->cart.lorom, 1);
    /* bank i (file offset i*0x8000) filled with byte i: $00:8000 -> 0,
     * $01:8000 -> 1 (LoROM: bank 1 = file bank 1) */
    T_CHECK_EQ(snes_cart_read(&s->cart, 0x008000u), 0x00);
    T_CHECK_EQ(snes_cart_read(&s->cart, 0x018000u), 0x01);
    T_CHECK_EQ(snes_cart_read(&s->cart, 0x028000u), 0x02);
    emu_core_supersnes()->destroy(&s->base);
}

T_SUITE_BEGIN(snes_cart)
{ "lorom_mapping", cart_lorom_mapping },
T_SUITE_END
T_SUITE_REG(snes_cart)
