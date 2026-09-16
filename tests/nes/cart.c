/*
 * beatle-nes-redux cartridge/mapper tests: NROM mapping, CNROM CHR switch,
 * UNROM PRG switch + fixed bank, MMC1 shift register + PRG modes + CHR,
 * MMC3 banking and IRQ (edge counts computed from the documented fetch
 * pattern, not from emulator internals).
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "beatle-nes-redux/nes.h"

#include <stdlib.h>
#include <string.h>

static struct nes *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_beatle_nes_redux()->create(&c) != EMU_OK)
        return NULL;
    return (struct nes *)c;
}

static void cart_nrom(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(2, 1, 0, 0, &sz);
    T_CHECK_EQ(emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz), EMU_OK);
    free(rom);
    /* PRG pattern: byte i of the file = i & 0xFF */
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8000), 0x00);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8001), 0x01);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xBFFF), 0xFF);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xC000), 0x00); /* second bank */
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xFFFF), 0xFF);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cart_nrom_16k_mirror(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(1, 1, 0, 0, &sz); /* 16K -> mirrored */
    /* 16 KiB with 1 bank: file has minimum 2 banks enforced by helper; use
     * raw load instead */
    free(rom);
    /* build manually: header + 16K prg + 8K chr */
    uint8_t *raw = malloc(16 + 0x4000 + 0x2000);
    memset(raw, 0, 16 + 0x4000 + 0x2000);
    memcpy(raw, "NES\x1A", 4);
    raw[4] = 1;
    raw[5] = 1;
    for (size_t i = 0; i < 0x4000u; i++)
        raw[16 + i] = (uint8_t)i;
    T_CHECK_EQ(emu_core_beatle_nes_redux()->load_rom(&n->base, raw,
                                                     16 + 0x4000 + 0x2000),
               EMU_OK);
    free(raw);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8000), 0x00);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xBFFF), 0xFF);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xC000), 0x00); /* mirror */
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xFFFF), 0xFF);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cart_unrom(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(4, 0, 2, 0, &sz); /* 64K PRG, CHR RAM */
    T_CHECK_EQ(emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz), EMU_OK);
    free(rom);
    T_CHECK_EQ(n->cart.chr_ram, 1);
    /* write bank 2 -> 8000-BFFF shows PRG bank 2, C000 fixed = bank 3 */
    nes_cart_cpu_write(&n->cart, 0x8000, 2);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8000), (uint8_t)(0x8000u & 0xFFu));
    /* bank 2 starts at file offset 2*0x4000; first byte = 2*0x4000 & 0xFF = 0 */
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8001), 0x01);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xC000), 0x00); /* bank 3: offset 0xC000 & 0xFF */
    /* write bank 0 via register at FFFF (any write works) */
    nes_cart_cpu_write(&n->cart, 0xFFFF, 0);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8000), 0x00);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cart_cnrom(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(2, 2, 3, 0, &sz);
    T_CHECK_EQ(emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz), EMU_OK);
    free(rom);
    /* CHR pattern byte i = ((i>>8)+3)&0xFF: distinct per 256-byte page */
    uint8_t b0 = nes_cart_chr_read(&n->cart, 0x0000);
    nes_cart_cpu_write(&n->cart, 0x8000, 1); /* switch CHR bank */
    uint8_t b1 = nes_cart_chr_read(&n->cart, 0x0000);
    T_CHECK(b0 != b1);
    T_CHECK_EQ(b0, 0x03u);
    T_CHECK_EQ(b1, (uint8_t)(((0x2000u >> 8) + 3u) & 0xFFu));
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cart_mmc1(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(8, 2, 1, 0, &sz); /* 128K PRG, 16K CHR */
    T_CHECK_EQ(emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz), EMU_OK);
    free(rom);

    /* MMC1 default PRG mode after reset writes: R3 reset to mode 3 */
    /* Program R3 with bank 5 via 5 writes: bit sequence of 0x05 = 10100 */
    nes_cart_cpu_write(&n->cart, 0x8000, 0x00); /* clear shift (bit7 set? no:
                                                   0x00 has bit7=0 -> shifts 0) */
    /* use explicit reset first */
    nes_cart_cpu_write(&n->cart, 0x8000, 0x80);
    const int bits5[5] = { 1, 0, 1, 0, 0 }; /* LSB-first of 0x05 */
    for (int i = 0; i < 5; i++)
        nes_cart_cpu_write(&n->cart, 0xE000, (uint8_t)bits5[i]);
    T_CHECK_EQ(n->cart.mmc1_regs[3] & 0x0Fu, 5);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8000), 0x00); /* bank5 first byte:
                                                              offset 5*0x4000=0x14000
                                                              -> &0xFF = 0 */
    /* PRG mode 3: C000 fixed to last bank (7) */
    T_CHECK_EQ(n->cart.prg_banks, 8u);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xC000), 0x00);

    /* 32K mode (R3 mode 2): program R3 = 0x06 (110): bits LSB-first 0,1,1,0,0
     * with PRG mode bits in R0... R3 mode bits are (val>>1)&3? MMC1 R3:
     * bits 0-3 = bank, bits 2-3 = PRG mode. Setting R3=0x06 -> mode (0x06>>1)&3
     * = 3 -> still mode 3. To get 32K mode, R3 low two bits of mode: value
     * 0x0A? mode = (r3 >> 1) & 3? No: MMC1 R3: bit0-1 = unused? PRG mode =
     * (R3 >> 1) & 3? Correct: R3 bits 2-3 = mode, bits 0-1 = bank (when 16K
     * mode). For 32K mode, mode=(R3>>1)&3==2: R3 = 0b10100 = 0x14 & 0x0F = 4?
     * mode bits are 2-3: R3=0x04 -> mode 2, bank ignored. */
    nes_cart_cpu_write(&n->cart, 0x8000, 0x80);
    const int bits32[5] = { 0, 0, 1, 0, 1 }; /* 0x14: 00101 LSB first */
    for (int i = 0; i < 5; i++)
        nes_cart_cpu_write(&n->cart, 0xE000, (uint8_t)bits32[i]);
    T_CHECK_EQ((n->cart.mmc1_regs[3] >> 1) & 3u, 2u); /* 32K mode */
    /* 32K mode: 8000-FFFF uses banks (r3&0xFE=0x14&0xE=0x14&0xFE... R3=0x14:
     * low bank = R3 & 0xFE = 0x14 & 0xFE = 0x14 (bit0=0 already) -> bank 4 */
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8000), (uint8_t)((4u * 0x4000u) & 0xFFu));
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cart_mmc3_irq(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(4, 4, 4, 0, &sz);
    T_CHECK_EQ(emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz), EMU_OK);
    free(rom);

    /* Directly clock the mapper through CHR reads with tile >= 0x100
     * (A12 set): pattern table fetch at $1FF0 etc. */
    nes_cart_cpu_write(&n->cart, 0xC000, 3);   /* IRQ latch = 3 */
    nes_cart_cpu_write(&n->cart, 0xC001, 0);   /* request reload */
    nes_cart_cpu_write(&n->cart, 0xE001, 0);   /* enable IRQ */
    /* Model: the edge after a reload request loads the counter (no assert);
     * further edges decrement; reaching 0 asserts the IRQ. */
    nes_cart_chr_read(&n->cart, 0x1FF0); /* edge 1: reload to 3 */
    T_CHECK_EQ(n->cart.mmc3_irq_counter, 3);
    for (int i = 0; i < 3; i++) {
        nes_cart_chr_read(&n->cart, 0x0FF0); /* A12 falls (no count) */
        nes_cart_chr_read(&n->cart, 0x1FF0); /* rising edge: decrement */
    }
    T_CHECK_EQ(n->cart.mmc3_irq_counter, 0);
    T_CHECK_EQ(n->cart.mmc3_irq_asserted, 1);
    nes_cart_cpu_write(&n->cart, 0xE000, 0); /* disable + acknowledge */
    T_CHECK_EQ(n->cart.mmc3_irq_asserted, 0);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cart_mmc3_prg_banking(void)
{
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(4, 2, 4, 0, &sz);
    T_CHECK_EQ(emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz), EMU_OK);
    free(rom);
    /* R6 -> bank 1 at $8000 (mode 0), R7 -> bank 2 at $A000, $C000-$FFFF fixed 3 */
    nes_cart_cpu_write(&n->cart, 0x8000, 6);
    nes_cart_cpu_write(&n->cart, 0x8001, 1);
    nes_cart_cpu_write(&n->cart, 0x8000, 7);
    nes_cart_cpu_write(&n->cart, 0x8001, 2);
    /* bank 1 at 0x8000: file offset 0x4000: first byte 0x00 */
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8000), 0x00);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0x8001), 0x01);
    /* bank 2 at 0xA000: offset 0x8000: byte 0x00 */
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xA000), 0x00);
    /* fixed last bank (3): offset 0xC000 */
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xC000), 0x00);
    T_CHECK_EQ(nes_cart_cpu_read(&n->cart, 0xFFFF), 0xFF);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

static void cart_unsupported_mapper(void)
{
    size_t sz = 0;
    uint8_t *rom = nes_make_rom(2, 1, 7, 0, &sz); /* mapper 7: not supported */
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n) {
        free(rom);
        return;
    }
    emu_result_t r = emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sz);
    free(rom);
    emu_core_beatle_nes_redux()->destroy(&n->base);
    T_CHECK_EQ(r, EMU_EUNSUPPORTED);
}

static void cart_bad_header(void)
{
    uint8_t rom[32] = { 0 };
    rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'X'; /* wrong magic */
    struct nes *n = mk_core();
    T_CHECK(n != NULL);
    if (!n)
        return;
    T_CHECK_EQ(emu_core_beatle_nes_redux()->load_rom(&n->base, rom, sizeof rom),
               EMU_EBADROM);
    emu_core_beatle_nes_redux()->destroy(&n->base);
}

T_SUITE_BEGIN(nes_cart)
{ "nrom_32k", cart_nrom },
{ "nrom_16k_mirror", cart_nrom_16k_mirror },
{ "unrom", cart_unrom },
{ "cnrom", cart_cnrom },
{ "mmc1_shift_and_modes", cart_mmc1 },
{ "mmc3_irq_counter", cart_mmc3_irq },
{ "mmc3_prg_banking", cart_mmc3_prg_banking },
{ "unsupported_mapper", cart_unsupported_mapper },
{ "bad_header", cart_bad_header },
T_SUITE_END

T_SUITE_REG(nes_cart)
