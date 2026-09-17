/*
 * ROM signature probing and core registry tests.
 */
#include "tests.h"
#include "emu/emu.h"

#include <stdlib.h>
#include <string.h>

static void put(uint8_t *b, size_t off, const char *s)
{
    memcpy(b + off, s, strlen(s));
}

static void test_probe_nes(void)
{
    uint8_t rom[32] = { 0 };
    memcpy(rom, "NES\x1A", 4);
    T_CHECK_STR(emu_rom_probe(rom, sizeof rom), "beatle-nes-redux");
}

static void test_probe_n64_variants(void)
{
    uint8_t z64[4] = { 0x80, 0x37, 0x12, 0x40 };
    uint8_t v64[4] = { 0x37, 0x80, 0x40, 0x12 };
    uint8_t n64[4] = { 0x12, 0x40, 0x80, 0x37 };
    T_CHECK_STR(emu_rom_probe(z64, 4), "m64-b");
    T_CHECK_STR(emu_rom_probe(v64, 4), "m64-b");
    T_CHECK_STR(emu_rom_probe(n64, 4), "m64-b");
}

static void test_probe_genesis_and_32x(void)
{
    size_t size = 0x400;
    uint8_t *rom = calloc(1, size);
    put(rom, 0x100, "SEGA GENESIS");
    T_CHECK_STR(emu_rom_probe(rom, size), "finalburn");
    memset(rom, 0, size);
    put(rom, 0x100, "SEGA 32X");
    T_CHECK_STR(emu_rom_probe(rom, size), "ms-32");
    free(rom);
}

static void test_probe_disc_headers(void)
{
    size_t size = 0x1000;
    uint8_t *img = calloc(1, size);
    put(img, 0, "SEGA SEGASATURN");
    T_CHECK_STR(emu_rom_probe(img, size), "supersaturn");
    memset(img, 0, size);
    put(img, 0, "SEGA SEGAKATANA");
    T_CHECK_STR(emu_rom_probe(img, size), "supercastpro");
    /* PlayStation raw executable */
    memset(img, 0, size);
    put(img, 0, "PS-X EXE");
    T_CHECK_STR(emu_rom_probe(img, size), "beatle-psx");
    free(img);
}

static void test_probe_nintendo_logos(void)
{
    static const uint8_t logo8[8] = { 0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A,
                                      0xA2, 0x21 };
    static const uint8_t gblogo[16] = { 0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D,
                                        0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
                                        0x00, 0x0C, 0x00, 0x0D };
    size_t size = 0x400;
    uint8_t *rom = calloc(1, size);
    memcpy(rom + 0x04, logo8, 8); /* GBA */
    T_CHECK_STR(emu_rom_probe(rom, size), "mgbax");
    memset(rom, 0, size);
    memcpy(rom + 0x160, logo8, 8); /* NDS */
    T_CHECK_STR(emu_rom_probe(rom, size), "mds-a");
    memset(rom, 0, size);
    memcpy(rom + 0x104, gblogo, 16); /* GB */
    T_CHECK_STR(emu_rom_probe(rom, size), "mgbx");
    free(rom);
}

static void test_probe_snes_checksum(void)
{
    /* LoROM header at 0x7FC0: complement ^ checksum == 0xFFFF */
    size_t size = 0x8000;
    uint8_t *rom = calloc(1, size);
    uint16_t checksum = 0x1234;
    rom[0x7FC0 + 0x1C] = (uint8_t)checksum;
    rom[0x7FC0 + 0x1D] = (uint8_t)(checksum >> 8);
    uint16_t complement = (uint16_t)~checksum;
    rom[0x7FC0 + 0x1E] = (uint8_t)complement;
    rom[0x7FC0 + 0x1F] = (uint8_t)(complement >> 8);
    T_CHECK_STR(emu_rom_probe(rom, size), "supersnes");
    free(rom);
}

static void test_probe_unknown(void)
{
    uint8_t rom[64] = { 0 };
    T_CHECK_STR(emu_rom_probe(rom, sizeof rom), "unknown");
    T_CHECK_STR(emu_rom_probe(NULL, 0), "unknown");
}

static void test_registry_content(void)
{
    size_t count = 0;
    const emu_core_info_t *reg = emu_core_registry(&count);
    T_CHECK_EQ_U(count, 11);
    int saw_mgbx = 0, saw_finalburn = 0, saw_psx = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(reg[i].name, "mgbx") == 0) {
            saw_mgbx = 1;
            T_CHECK_EQ_U(reg[i].status, EMU_STATUS_WORKING);
        }
        if (strcmp(reg[i].name, "finalburn") == 0) {
            saw_finalburn = 1;
            T_CHECK_EQ_U(reg[i].status, EMU_STATUS_PARTIAL);
        }
        if (strcmp(reg[i].name, "beatle-psx") == 0) {
            saw_psx = 1;
            T_CHECK_EQ_U(reg[i].status, EMU_STATUS_SKELETON);
        }
        T_CHECK(reg[i].note != NULL && reg[i].note[0] != 0);
        T_CHECK(reg[i].vtable != NULL);
        /* Accessors return NULL for cores not compiled into this build;
         * built cores must expose a vtable whose name matches. */
        const emu_core_vtable_t *vt = reg[i].vtable();
        if (vt != NULL)
            T_CHECK_STR(vt->name, reg[i].name);
    }
    T_CHECK(saw_mgbx && saw_finalburn && saw_psx);
}

T_SUITE_BEGIN(romdetect)
{ "probe_nes", test_probe_nes },
{ "probe_n64_byte_orders", test_probe_n64_variants },
{ "probe_genesis_32x", test_probe_genesis_and_32x },
{ "probe_disc_headers", test_probe_disc_headers },
{ "probe_nintendo_logos", test_probe_nintendo_logos },
{ "probe_snes_checksum", test_probe_snes_checksum },
{ "probe_unknown", test_probe_unknown },
{ "registry_content", test_registry_content },
T_SUITE_END
T_SUITE_REG(romdetect)
