/*
 * AArch core registry and ROM signature probing.
 *
 * The registry is plain data: it maps core short names to their vtable
 * accessor (NULL when the core is not compiled into this build) and carries
 * an honest status value maintained alongside each core's test suite.
 *
 * emu_rom_probe() sniffs well-known format signatures on the raw image bytes.
 * It is a best-effort hint, not a validator: cores re-verify whatever they
 * load. Detection is intentionally conservative - unknown formats return
 * "unknown" rather than a guess.
 */
#include "emu/emu.h"
#include "util.h"

#include <string.h>

/* ---- per-core vtable accessors, guarded by build configuration ---------- */

static const emu_core_vtable_t *ref_mgbx(void)
{
#ifdef EMU_BUILD_MGBX
    return emu_core_mgbx();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_nes(void)
{
#ifdef EMU_BUILD_BEATLE_NES_REDUX
    return emu_core_beatle_nes_redux();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_snes(void)
{
#ifdef EMU_BUILD_SUPERSNES
    return emu_core_supersnes();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_gba(void)
{
#ifdef EMU_BUILD_MGBAX
    return emu_core_mgbax();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_finalburn(void)
{
#ifdef EMU_BUILD_FINALBURN
    return emu_core_finalburn();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_psx(void)
{
#ifdef EMU_BUILD_BEATLE_PSX
    return emu_core_beatle_psx();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_ds(void)
{
#ifdef EMU_BUILD_MDS_A
    return emu_core_mds_a();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_32x(void)
{
#ifdef EMU_BUILD_MS_32
    return emu_core_ms_32();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_saturn(void)
{
#ifdef EMU_BUILD_SUPERSATURN
    return emu_core_supersaturn();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_n64(void)
{
#ifdef EMU_BUILD_M64_B
    return emu_core_m64_b();
#else
    (void)0;
    return NULL;
#endif
}

static const emu_core_vtable_t *ref_dc(void)
{
#ifdef EMU_BUILD_SUPERCASTPRO
    return emu_core_supercastpro();
#else
    (void)0;
    return NULL;
#endif
}

/*
 * The registry. Status values must reflect reality:
 *  - WORKING  : boots real-ish content paths exercised by the core suite
 *  - PARTIAL  : runs, with functional gaps documented in README.md
 *  - SKELETON : load/validate only; run_frame returns EMU_ENOTIMPL
 */
static const emu_core_info_t registry[] = {
    { "mgbx", EMU_STATUS_WORKING,
      "Game Boy/DMG: SM83 CPU, MBC1/2/3/5+RTC, dot PPU, PSG, save states",
      ref_mgbx },
    { "beatle-nes-redux", EMU_STATUS_WORKING,
      "NES: 6502, mappers 0/1/2/3/4, dot PPU, APU+DMC, save states",
      ref_nes },
    { "supersnes", EMU_STATUS_PARTIAL,
      "SNES: 65C816, LoROM/HiROM, DMA+HDMA, PPU modes 0/1/7; S-SMP/DSP stub",
      ref_snes },
    { "mgbax", EMU_STATUS_WORKING,
      "GBA: ARM7TDMI+Thumb, HLE BIOS, PPU modes 0-4, DMA, PSG+FIFO audio",
      ref_gba },
    { "finalburn", EMU_STATUS_PARTIAL,
      "Genesis: 68000 subset + Z80, VDP mode-4, PSG, YM2612 subset; see README",
      ref_finalburn },
    { "beatle-psx", EMU_STATUS_PARTIAL,
      "PlayStation: R3000A+GTE+GPU+DMA+timers, PS-X EXE loader; no CD/SPU",
      ref_psx },
    { "mds-a", EMU_STATUS_PARTIAL,
      "Nintendo DS: ARM946E-S + ARM7TDMI (ARM+Thumb), direct-boot from "
      ".nds header, timers, IPC, IRQ, full 2D compositing per engine "
      "(text/affine/extended BGs, OBJs, priorities, master brightness); "
      "no 3D, blending/windows, sound, touch or card bus",
      ref_ds },
    { "ms-32", EMU_STATUS_PARTIAL,
      "Sega 32X: SH-2 x2 + adapter + COMM + VDP packed-pixel over finalburn; "
      "RLE/autosprites/PWM pending",
      ref_32x },
    { "supersaturn", EMU_STATUS_PARTIAL,
      "Sega Saturn: SH-2 x2, SCU DMA+IRQ+timers, VDP1 sprites/polys, SMPC "
      "INTBACK, IP.BIN boot; VDP2/SCSP/CD stub",
      ref_saturn },
    { "m64-b", EMU_STATUS_PARTIAL,
      "Nintendo 64: R4300i (MIPS III) + CP0/exceptions, PI DMA, SI, VI "
      "framebuffer output, no-PIF boot model; RSP, audio and TLB are stubs",
      ref_n64 },
    { "supercastpro", EMU_STATUS_PARTIAL,
      "Dreamcast: SH-4 (integer + SP FPU subset), TMU, direct-boot from "
      "IP.BIN, PVR2 scanout + Tile Accelerator subset (flat-shaded "
      "strips/sprites), AICA ARM7 + PCM16/8 voices; no TA textures, "
      "alpha, ADPCM/DSP, GD-ROM or Maple",
      ref_dc },
};

const emu_core_info_t *emu_core_registry(size_t *count_out)
{
    if (count_out != NULL)
        *count_out = sizeof registry / sizeof registry[0];
    return registry;
}

const char *emu_core_status_str(emu_core_status_t s)
{
    switch (s) {
    case EMU_STATUS_WORKING:  return "working";
    case EMU_STATUS_PARTIAL:  return "partial";
    case EMU_STATUS_SKELETON: return "skeleton";
    default:                  return "unknown";
    }
}


/* ---- ROM signature probing ------------------------------------------------ */

static int memeq(const uint8_t *p, size_t size, size_t off,
                 const char *sig, size_t len)
{
    if (size < off + len)
        return 0;
    return memcmp(p + off, sig, len) == 0;
}

/* Nintendo logo prefix as stored in the GBA header (0x04) and NDS header
 * (0x160). The first 8 bytes of the LZ-compressed logo stream are checked;
 * checking all 156 bytes is unnecessary for a hint. */
static const uint8_t nintendo_logo8[8] = {
    0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21
};

/* Game Boy logo prefix at header offset 0x104 (first 16 of 48 bytes). */
static const uint8_t gb_logo16[16] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D
};

static int snes_checksum_ok(const uint8_t *p, size_t size, size_t off)
{
    uint16_t complement, checksum;
    if (size < off + 0x20)
        return 0;
    complement = emu_le16(p + off + 0x1E);
    checksum = emu_le16(p + off + 0x1C);
    return checksum != 0 && (uint16_t)(complement ^ checksum) == 0xFFFFu;
}

const char *emu_rom_probe(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0)
        return "unknown";

    /* iNES */
    if (memeq(data, size, 0, "NES\x1A", 4))
        return "beatle-nes-redux";

    /* Nintendo 64: big-endian .z64 and the two byte-swapped variants. */
    if (size >= 4) {
        if (data[0] == 0x80 && data[1] == 0x37 && data[2] == 0x12 && data[3] == 0x40)
            return "m64-b";
        if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12)
            return "m64-b";
        if (data[0] == 0x12 && data[1] == 0x40 && data[2] == 0x80 && data[3] == 0x37)
            return "m64-b";
    }

    /* PlayStation raw executable. */
    if (memeq(data, size, 0, "PS-X EXE", 8))
        return "beatle-psx";

    /* Disc images with an IP.BIN first sector (sector 0, 2048-byte sectors). */
    if (memeq(data, size, 0, "SEGA SEGASATURN", 15))
        return "supersaturn";
    if (memeq(data, size, 0, "SEGA SEGAKATANA", 15))
        return "supercastpro";

    /* Sega 32X cartridge header overrides plain Genesis (same layout). */
    if (memeq(data, size, 0x100, "SEGA 32X", 8))
        return "ms-32";
    if (size >= 0x200 && memeq(data, size, 0x100, "SEGA", 4))
        return "finalburn";

    /* Nintendo DS: 156-byte Nintendo logo at 0x160. */
    if (size >= 0x200 && memeq(data, size, 0x160, (const char *)nintendo_logo8, 8))
        return "mds-a";

    /* Game Boy / Game Boy Color: logo at 0x104. */
    if (size >= 0x150 && memeq(data, size, 0x104, (const char *)gb_logo16, 16))
        return "mgbx";

    /* Game Boy Advance: logo at 0x04. */
    if (size >= 0x120 && memeq(data, size, 4, (const char *)nintendo_logo8, 8))
        return "mgbax";

    /* SNES: LoROM / HiROM header checksum complement consistency. */
    if (snes_checksum_ok(data, size, 0x7FC0) || snes_checksum_ok(data, size, 0xFFC0))
        return "supersnes";

    /* Generic ISO9660 with the PlayStation system identifier (PVD sector 16). */
    if (size >= 32800 && memeq(data, size, 32769, "CD001", 5) &&
        memeq(data, size, 32776, "PLAYSTATION", 11))
        return "beatle-psx";

    return "unknown";
}
