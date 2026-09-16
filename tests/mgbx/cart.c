/*
 * mgbx cartridge/mapper tests: ROM banking, RAM banking, RAM enable gating,
 * MBC1 mode behavior, MBC3 RTC select, MBC5 9-bit banking, MBC2 nibble RAM.
 * Expected values are derived from the MBC specifications.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbx/mgbx.h"

#include <stdlib.h>
#include <string.h>

/* Direct cart-level test: load ROM into a bare gb_cart. */
static void cart_load_bare(gb_cart *cart, uint32_t banks, uint8_t type,
                           uint8_t ram_code, size_t *rom_size_out)
{
    size_t rom_size = 0;
    uint8_t *rom = gb_make_rom(banks, type, ram_code, &rom_size);
    T_CHECK_EQ(gb_cart_load(cart, rom, rom_size), EMU_OK);
    free(rom);
    if (rom_size_out)
        *rom_size_out = rom_size;
}

static void cart_rom_only(void)
{
    gb_cart cart;
    gb_cart_init(&cart);
    cart_load_bare(&cart, 2, GB_CART_ROM_ONLY, 0, NULL);
    /* bank pattern: bank i is filled with byte i */
    T_CHECK_EQ(gb_cart_read(&cart, 0x0000), 0x00);
    T_CHECK_EQ(gb_cart_read(&cart, 0x3FFF), 0x00);
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x01);
    T_CHECK_EQ(gb_cart_read(&cart, 0x7FFF), 0x01);
    T_CHECK_EQ(cart.rom_banks, 2u);
    gb_cart_free(&cart);
}

static void cart_mbc1_banking(void)
{
    gb_cart cart;
    gb_cart_init(&cart);
    cart_load_bare(&cart, 32, GB_CART_MBC1_RAM, 2, NULL); /* 512 KiB, 8 KiB RAM */

    /* default bank 1 */
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x01);
    /* select bank 5 */
    gb_cart_write(&cart, 0x2000, 0x05);
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x05);
    /* bank number 0 must alias to 1 in 4000-7FFF */
    gb_cart_write(&cart, 0x2000, 0x00);
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x01);
    /* RAM: disabled by default -> reads 0xFF; enable, write, read back */
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0xFF);
    gb_cart_write(&cart, 0x0000, 0x0A);
    gb_cart_write(&cart, 0xA123, 0x5A);
    T_CHECK_EQ(gb_cart_read(&cart, 0xA123), 0x5A);
    /* RAM bank 0 in mode 0 */
    gb_cart_write(&cart, 0x0000, 0x00);
    T_CHECK_EQ(gb_cart_read(&cart, 0xA123), 0xFF);
    gb_cart_free(&cart);
}

static void cart_mbc1_mode1(void)
{
    /* 1 MiB MBC1 (64 banks, but MBC1 can only reach banks 0x00-0x1F and
     * 0x20/0x40/0x60 groups via bank2). With 64 banks: mode 1 + bank2=1 +
     * bank1=0 -> 0x20&0x1F==0 -> bank 0x21. */
    gb_cart cart;
    gb_cart_init(&cart);
    cart_load_bare(&cart, 64, GB_CART_MBC1_RAM_BAT, 3, NULL);
    gb_cart_write(&cart, 0x6000, 0x01); /* mode 1 */
    gb_cart_write(&cart, 0x4000, 0x01); /* bank2 = 1 */
    gb_cart_write(&cart, 0x2000, 0x00); /* bank1 = 0 */
    /* expected bank = (1<<5)|0 = 0x20; low5==0 -> +1 -> 0x21 */
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x21);
    /* RAM bank follows bank2 in mode 1: bank 1 */
    gb_cart_write(&cart, 0x0000, 0x0A);
    gb_cart_write(&cart, 0xA000, 0x11);
    gb_cart_write(&cart, 0x4000, 0x02); /* bank2 = 2 -> RAM bank 2 */
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0x00); /* fresh RAM reads zero */
    gb_cart_write(&cart, 0xA000, 0x22);
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0x22);
    gb_cart_write(&cart, 0x4000, 0x01);
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0x11);
    gb_cart_free(&cart);
}

static void cart_mbc2(void)
{
    gb_cart cart;
    gb_cart_init(&cart);
    cart_load_bare(&cart, 8, GB_CART_MBC2_BAT, 0, NULL);
    /* MBC2: bank select via 2000-3FFF with addr bit 8 = 1; 0 aliases to 1 */
    gb_cart_write(&cart, 0x2100, 0x03);
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x03);
    gb_cart_write(&cart, 0x2100, 0x00);
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x01);
    /* built-in RAM: 512 x 4-bit; upper nibble reads 0xF */
    gb_cart_write(&cart, 0x0000, 0x0A); /* RAM enable (addr bit 8 = 0) */
    gb_cart_write(&cart, 0xA001, 0xAB); /* only low nibble stored */
    T_CHECK_EQ(gb_cart_read(&cart, 0xA001), 0xFB); /* 0xB | 0xF0 */
    gb_cart_free(&cart);
}

static void cart_mbc3_rtc(void)
{
    gb_cart cart;
    gb_cart_init(&cart);
    cart_load_bare(&cart, 32, GB_CART_MBC3_TIMER_RAM, 3, NULL);
    /* 7-bit banking, 0 -> 1 */
    gb_cart_write(&cart, 0x2000, 0x00);
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x01);
    gb_cart_write(&cart, 0x2000, 0x1F);
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x1F);
    /* RTC register select 0x08..0x0C */
    gb_cart_write(&cart, 0x4000, 0x08);
    T_CHECK(gb_cart_read(&cart, 0xA000) <= 59u); /* seconds register */
    gb_cart_write(&cart, 0xA000, 0x2A);          /* write seconds */
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0x2A);
    /* latch protocol: 0x00 then 0x01 freezes the registers */
    gb_cart_write(&cart, 0xA000, 0x10); /* seconds = 0x10 */
    gb_cart_write(&cart, 0x6000, 0x00);
    gb_cart_write(&cart, 0x6000, 0x01);
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0x10);
    gb_cart_write(&cart, 0xA000, 0x20); /* live reg changes */
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0x10); /* latched stays */
    gb_cart_free(&cart);
}

static void cart_mbc5_nine_bit(void)
{
    gb_cart cart;
    gb_cart_init(&cart);
    cart_load_bare(&cart, 512, GB_CART_MBC5_RAM, 3, NULL); /* 8 MiB */
    /* MBC5 allows bank 0 unlike MBC1 */
    gb_cart_write(&cart, 0x2000, 0x00);
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), 0x00);
    /* 9-bit bank: low 8 via 2000-2FFF, bit 8 via 3000-3FFF */
    gb_cart_write(&cart, 0x2000, 0xFF);
    gb_cart_write(&cart, 0x3000, 0x01); /* bank = 0x1FF = 511 */
    T_CHECK_EQ(gb_cart_read(&cart, 0x4000), (uint8_t)(511u & 0xFFu));
    /* RAM bank 0-15 */
    gb_cart_write(&cart, 0x4000, 0x03);
    gb_cart_write(&cart, 0x0000, 0x0A);
    gb_cart_write(&cart, 0xA000, 0x77);
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0x77);
    gb_cart_write(&cart, 0x4000, 0x04);
    T_CHECK_EQ(gb_cart_read(&cart, 0xA000), 0xFF);
    gb_cart_free(&cart);
}

static void cart_unsupported_type(void)
{
    size_t rom_size = 0;
    uint8_t *rom = gb_make_rom(2, 0xFC /* POCKET CAMERA: unsupported */, 0,
                               &rom_size);
    gb_cart cart;
    gb_cart_init(&cart);
    T_CHECK_EQ(gb_cart_load(&cart, rom, rom_size), EMU_EUNSUPPORTED);
    free(rom);
}

static void cart_truncated_rom(void)
{
    gb_cart cart;
    gb_cart_init(&cart);
    /* the buffer is large enough to build a header, but the ROM passed to
     * the loader is truncated before the header is complete */
    uint8_t tiny[0x150] = { 0 };
    tiny[0x147] = GB_CART_ROM_ONLY;
    T_CHECK_EQ(gb_cart_load(&cart, tiny, 0x80), EMU_EBADROM);
    gb_cart_free(&cart);
}

T_SUITE_BEGIN(gb_cart)
{ "rom_only_mapping", cart_rom_only },
{ "mbc1_banking", cart_mbc1_banking },
{ "mbc1_mode1", cart_mbc1_mode1 },
{ "mbc2", cart_mbc2 },
{ "mbc3_rtc", cart_mbc3_rtc },
{ "mbc5_nine_bit", cart_mbc5_nine_bit },
{ "unsupported_type", cart_unsupported_type },
{ "truncated_rom", cart_truncated_rom },
T_SUITE_END

T_SUITE_REG(gb_cart)
