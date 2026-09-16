/*
 * mgbx cartridge: header parsing and MBC1 / MBC2 / MBC3 / MBC5 mappers.
 * ROM-only carts are type 0x00. The MBC3 RTC is driven by emulation time
 * (deterministic; never the host wall clock).
 */
#include "mgbx.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t supported_types[] = {
    GB_CART_ROM_ONLY,
    GB_CART_MBC1, GB_CART_MBC1_RAM, GB_CART_MBC1_RAM_BAT,
    GB_CART_MBC2, GB_CART_MBC2_BAT,
    GB_CART_MBC3_TIMER, GB_CART_MBC3_TIMER_RAM, GB_CART_MBC3,
    GB_CART_MBC3_RAM, GB_CART_MBC3_RAM_BAT,
    GB_CART_MBC5, GB_CART_MBC5_RAM, GB_CART_MBC5_RAM_BAT,
    GB_CART_MBC5_RUMBLE, GB_CART_MBC5_RUMBLE_RAM, GB_CART_MBC5_RUMBLE_RAM_BAT
};

static int type_supported(uint8_t t)
{
    for (size_t i = 0; i < sizeof supported_types; i++)
        if (supported_types[i] == t)
            return 1;
    return 0;
}

void gb_cart_init(gb_cart *cart)
{
    memset(cart, 0, sizeof *cart);
}

void gb_cart_free(gb_cart *cart)
{
    free(cart->rom);
    free(cart->ram);
    cart->rom = NULL;
    cart->ram = NULL;
    cart->rom_size = 0;
    cart->ram_size = 0;
}

static size_t ram_size_from_code(uint8_t code, uint8_t type)
{
    if (type == GB_CART_MBC2 || type == GB_CART_MBC2_BAT)
        return 512; /* built-in 512 x 4-bit */
    switch (code) {
    case 0: return 0;
    case 1: return 2048;
    case 2: return 8192;
    case 3: return 32768;
    case 4: return 131072;
    case 5: return 65536;
    default: return 0;
    }
}

emu_result_t gb_cart_load(gb_cart *cart, const uint8_t *data, size_t size)
{
    gb_cart_free(cart);
    if (size < 0x150u)
        return EMU_EBADROM;

    uint8_t type = data[0x147];
    if (!type_supported(type))
        return EMU_EUNSUPPORTED;

    uint8_t rom_code = data[0x148];
    uint32_t banks;
    if (rom_code <= 8u) {
        banks = 2u << rom_code;
    } else if (rom_code == 0x52u) {
        banks = 72;
    } else if (rom_code == 0x53u) {
        banks = 80;
    } else if (rom_code == 0x54u) {
        banks = 96;
    } else {
        return EMU_EBADROM;
    }
    size_t want = (size_t)banks * 0x4000u;
    if (size > want) /* tolerate padding */
        size = want;
    else if (size < want)
        return EMU_EBADROM; /* truncated image */

    cart->rom = malloc(size);
    if (cart->rom == NULL)
        return EMU_EINVAL;
    memcpy(cart->rom, data, size);
    cart->rom_size = size;
    cart->rom_banks = banks;
    cart->type = type;

    uint8_t ram_code = data[0x149];
    cart->ram_size = ram_size_from_code(ram_code, type);
    int has_ram_pin = (type == GB_CART_MBC1_RAM || type == GB_CART_MBC1_RAM_BAT ||
                       type == GB_CART_MBC3_RAM || type == GB_CART_MBC3_RAM_BAT ||
                       type == GB_CART_MBC3_TIMER_RAM ||
                       type == GB_CART_MBC5_RAM || type == GB_CART_MBC5_RAM_BAT ||
                       type == GB_CART_MBC5_RUMBLE_RAM ||
                       type == GB_CART_MBC5_RUMBLE_RAM_BAT);
    if (has_ram_pin && cart->ram_size == 0)
        cart->ram_size = 8192; /* RAM pin present but size code 0: assume 8 KiB */

    if (cart->ram_size > 0) {
        cart->ram = malloc(cart->ram_size);
        if (cart->ram == NULL) {
            gb_cart_free(cart);
            return EMU_EINVAL;
        }
        memset(cart->ram, 0, cart->ram_size);
    }

    gb_cart_reset(cart);
    return EMU_OK;
}

void gb_cart_reset(gb_cart *cart)
{
    cart->ram_enabled = 0;
    cart->bank1 = 1;
    cart->bank2 = 0;
    cart->mode = 0;
    cart->rom_bank = 1;
    cart->rtc_halt = 0;
    cart->rtc_latch_state = 0;
    cart->rtc_latched_valid = 0;
    memset(cart->rtc, 0, sizeof cart->rtc);
    memset(cart->rtc_latched, 0, sizeof cart->rtc_latched);
    cart->rtc_divider = 0;
    if (cart->ram != NULL)
        memset(cart->ram, 0, cart->ram_size);
    memset(cart->mbc2_ram, 0, sizeof cart->mbc2_ram);
}

static uint32_t rom_bank_mask(const gb_cart *cart)
{
    return cart->rom_banks - 1u; /* bank counts are powers of two except 72/80/96 */
}

static uint8_t *rom_bank_ptr(gb_cart *cart, uint32_t bank)
{
    uint32_t nb = cart->rom_banks;
    if (nb == 72u || nb == 80u || nb == 96u)
        bank %= nb;
    else
        bank &= rom_bank_mask(cart);
    return &cart->rom[(size_t)bank * 0x4000u];
}

uint8_t gb_cart_read(gb_cart *cart, uint16_t addr)
{
    if (addr < 0x8000u) {
        const uint8_t *bank0 = cart->rom;
        const uint8_t *bankn;
        if (addr < 0x4000u) {
            if (cart->type == GB_CART_MBC1 || cart->type == GB_CART_MBC1_RAM ||
                cart->type == GB_CART_MBC1_RAM_BAT) {
                if (cart->mode == 1 && cart->rom_banks > 32u)
                    bank0 = rom_bank_ptr(cart, cart->bank2 << 5);
            }
            return bank0[addr];
        }
        switch (cart->type) {
        case GB_CART_ROM_ONLY:
            return cart->rom[addr];
        case GB_CART_MBC1: case GB_CART_MBC1_RAM: case GB_CART_MBC1_RAM_BAT: {
            uint32_t bank = ((uint32_t)(cart->bank2 & 3u) << 5) | (cart->bank1 & 0x1Fu);
            if ((bank & 0x1Fu) == 0u)
                bank |= 1u;
            bankn = rom_bank_ptr(cart, bank);
            return bankn[addr - 0x4000u];
        }
        case GB_CART_MBC2: case GB_CART_MBC2_BAT: {
            uint32_t bank = cart->bank1 & 0x0Fu;
            if (bank == 0u)
                bank = 1u;
            bankn = rom_bank_ptr(cart, bank);
            return bankn[addr - 0x4000u];
        }
        case GB_CART_MBC3_TIMER: case GB_CART_MBC3_TIMER_RAM:
        case GB_CART_MBC3: case GB_CART_MBC3_RAM: case GB_CART_MBC3_RAM_BAT: {
            uint32_t bank = cart->bank1 & 0x7Fu;
            if (bank == 0u)
                bank = 1u;
            bankn = rom_bank_ptr(cart, bank);
            return bankn[addr - 0x4000u];
        }
        case GB_CART_MBC5: case GB_CART_MBC5_RAM: case GB_CART_MBC5_RAM_BAT:
        case GB_CART_MBC5_RUMBLE: case GB_CART_MBC5_RUMBLE_RAM:
        case GB_CART_MBC5_RUMBLE_RAM_BAT: {
            uint32_t bank = cart->rom_bank & 0x1FFu;
            bankn = rom_bank_ptr(cart, bank);
            return bankn[addr - 0x4000u];
        }
        default:
            return 0xFFu;
        }
    }

    /* 0xA000-0xBFFF: cart RAM / RTC */
    int mbc1 = (cart->type == GB_CART_MBC1 || cart->type == GB_CART_MBC1_RAM ||
                cart->type == GB_CART_MBC1_RAM_BAT);
    int mbc3 = (cart->type == GB_CART_MBC3_TIMER || cart->type == GB_CART_MBC3_TIMER_RAM ||
                cart->type == GB_CART_MBC3 || cart->type == GB_CART_MBC3_RAM ||
                cart->type == GB_CART_MBC3_RAM_BAT);
    int mbc5 = (cart->type >= GB_CART_MBC5 && cart->type <= GB_CART_MBC5_RUMBLE_RAM_BAT);

    if (mbc3 && cart->bank2 >= 0x08u && cart->bank2 <= 0x0Cu) {
        uint8_t r = (uint8_t)(cart->bank2 - 0x08u);
        /* reads return live registers until the first latch, latched after */
        return cart->rtc_latched_valid ? cart->rtc_latched[r] : cart->rtc[r];
    }

    if (cart->type == GB_CART_MBC2 || cart->type == GB_CART_MBC2_BAT)
        return (uint8_t)(0xF0u | (cart->mbc2_ram[addr & 0x1FFu] & 0x0Fu));

    if (!cart->ram_enabled || cart->ram == NULL || cart->ram_size == 0)
        return 0xFFu;

    uint32_t bank;
    if (mbc1)
        bank = (cart->mode == 1) ? (cart->bank2 & 3u) : 0u;
    else if (mbc3)
        bank = cart->bank2 & 3u;
    else if (mbc5)
        bank = cart->bank2 & 0x0Fu;
    else
        bank = 0;

    size_t offset = (size_t)bank * 0x2000u + (addr - 0xA000u);
    if (offset >= cart->ram_size)
        return 0xFFu;
    return cart->ram[offset];
}

void gb_cart_write(gb_cart *cart, uint16_t addr, uint8_t v)
{
    if (addr < 0x8000u) {
        int mbc1 = (cart->type == GB_CART_MBC1 || cart->type == GB_CART_MBC1_RAM ||
                    cart->type == GB_CART_MBC1_RAM_BAT);
        int mbc2 = (cart->type == GB_CART_MBC2 || cart->type == GB_CART_MBC2_BAT);
        int mbc3 = (cart->type == GB_CART_MBC3_TIMER || cart->type == GB_CART_MBC3_TIMER_RAM ||
                    cart->type == GB_CART_MBC3 || cart->type == GB_CART_MBC3_RAM ||
                    cart->type == GB_CART_MBC3_RAM_BAT);
        int mbc5 = (cart->type >= GB_CART_MBC5 && cart->type <= GB_CART_MBC5_RUMBLE_RAM_BAT);

        if (addr < 0x2000u) {
            if (mbc2) {
                if ((addr & 0x0100u) == 0u)
                    cart->ram_enabled = ((v & 0x0Fu) == 0x0Au);
                return;
            }
            cart->ram_enabled = ((v & 0x0Fu) == 0x0Au);
            return;
        }
        if (addr < 0x4000u) {
            if (mbc2) {
                if (addr & 0x0100u)
                    cart->bank1 = (uint8_t)(v & 0x0Fu);
                return;
            }
            if (mbc5) {
                if (addr < 0x3000u)
                    cart->rom_bank = (uint16_t)((cart->rom_bank & 0x100u) | v);
                else
                    cart->rom_bank = (uint16_t)((cart->rom_bank & 0xFFu) |
                                                ((uint16_t)(v & 1u) << 8));
                return;
            }
            cart->bank1 = v;
            return;
        }
        if (addr < 0x6000u) {
            if (mbc5) {
                cart->bank2 = (uint8_t)(v & 0x0Fu);
                return;
            }
            cart->bank2 = v;
            return;
        }
        /* 0x6000-0x7FFF */
        if (mbc1) {
            cart->mode = (uint8_t)(v & 1u);
            return;
        }
        if (mbc3) { /* RTC latch: write 0x00 then 0x01 */
            if (v == 0x00u) {
                cart->rtc_latch_state = 1;
            } else if (v == 0x01u && cart->rtc_latch_state == 1) {
                memcpy(cart->rtc_latched, cart->rtc, sizeof cart->rtc);
                cart->rtc_latched_valid = 1;
                cart->rtc_latch_state = 0;
            }
            return;
        }
        return;
    }

    /* 0xA000-0xBFFF */
    int mbc3 = (cart->type == GB_CART_MBC3_TIMER || cart->type == GB_CART_MBC3_TIMER_RAM ||
                cart->type == GB_CART_MBC3 || cart->type == GB_CART_MBC3_RAM ||
                cart->type == GB_CART_MBC3_RAM_BAT);
    if (mbc3 && cart->bank2 >= 0x08u && cart->bank2 <= 0x0Cu) {
        uint8_t r = (uint8_t)(cart->bank2 - 0x08u);
        if (r == 4) {
            cart->rtc[4] = (uint8_t)(v & 0xC1u); /* halt + carry flags */
            cart->rtc_halt = (uint8_t)((v & 0x40u) != 0);
        } else {
            cart->rtc[r] = v;
        }
        return;
    }
    if (cart->type == GB_CART_MBC2 || cart->type == GB_CART_MBC2_BAT) {
        if (cart->ram_enabled)
            cart->mbc2_ram[addr & 0x1FFu] = (uint8_t)(v & 0x0Fu);
        return;
    }
    if (!cart->ram_enabled || cart->ram == NULL || cart->ram_size == 0)
        return;

    uint32_t bank;
    if (cart->type == GB_CART_MBC1 || cart->type == GB_CART_MBC1_RAM ||
        cart->type == GB_CART_MBC1_RAM_BAT)
        bank = (cart->mode == 1) ? (cart->bank2 & 3u) : 0u;
    else if (mbc3)
        bank = cart->bank2 & 3u;
    else if (cart->type >= GB_CART_MBC5)
        bank = cart->bank2 & 0x0Fu;
    else
        bank = 0;

    size_t offset = (size_t)bank * 0x2000u + (addr - 0xA000u);
    if (offset < cart->ram_size)
        cart->ram[offset] = v;
}

void gb_cart_tick_rtc(gb_cart *cart, uint32_t t_cycles)
{
    if (cart->rtc_halt)
        return;
    cart->rtc_divider += t_cycles;
    while (cart->rtc_divider >= 4194304u) {
        cart->rtc_divider -= 4194304u;
        if (++cart->rtc[0] == 0u) {
            if (++cart->rtc[1] == 0u) {
                if (++cart->rtc[2] == 0u) {
                    if (++cart->rtc[3] == 0u) {
                        cart->rtc[3] = 0;
                        cart->rtc[4] |= 0x80u; /* day carry */
                    }
                }
            }
        }
    }
}

/* MBC5 9-bit bank handling lives in gb_cart_write: 2000-2FFF sets low 8 bits,
 * 3000-3FFF sets bit 8. */
