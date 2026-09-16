/*
 * Shared test helpers: build synthetic ROMs for each system.
 *
 * Expected values in tests are derived from the system specifications
 * (hand-computed), never by calling emulator internals.
 */
#ifndef EMU_TESTUTIL_H
#define EMU_TESTUTIL_H

#include <stddef.h>
#include <stdint.h>

/* ---- Game Boy (mgbx) ---- */

#define GB_ROM_BANK_SIZE 0x4000u

/*
 * Build a GB ROM of `banks` banks (16 KiB each). Every bank is filled with the
 * pattern byte (index & 0xFF) so ROM banking is observable. Header fields:
 *   - cart_type: MBC type code (0x00, 0x01, 0x03, 0x05, 0x06, 0x0F..0x13, 0x19..0x1E)
 *   - ram_code: 0x149 RAM size code (0 = none, 1 = 2 KiB, 2 = 8 KiB, 3 = 32 KiB, 4 = 128 KiB)
 *   - rom_size_code: computed automatically from banks
 * Caller owns the returned buffer (free()).
 */
uint8_t *gb_make_rom(uint32_t banks, uint8_t cart_type, uint8_t ram_code,
                     size_t *size_out);

/* ---- NES (beatle-nes-redux) ---- */

/*
 * Build an iNES ROM: prg_banks 16 KiB units (min 2), chr_banks 8 KiB units
 * (may be 0 = CHR RAM), flags6_raw is OR-ed into the header flags6 (mirroring
 * bit 0, battery bit 1, 4-screen bit 3), mapper 0..255. PRG banks filled with
 * pattern (index & 0xFF); CHR banks likewise. Caller frees.
 */
uint8_t *nes_make_rom(uint32_t prg_banks, uint32_t chr_banks, uint8_t mapper,
                      uint8_t flags6_extra, size_t *size_out);

/* ---- SNES (supersnes) ---- */

/*
 * Build a LoROM SNES image of `banks` banks (32 KiB mapped per bank, file
 * size = banks * 0x8000). A valid SNES header is placed at the LoROM location
 * (bank 0, offset 0x7FC0) with checksum/complement consistent and mapping
 * mode 0x20. Each bank's mapped 32 KiB region is filled with (bank & 0xFF).
 * Caller frees.
 */
uint8_t *snes_make_lorom(uint32_t banks, size_t *size_out);

/* ---- GBA (mgbax) ---- */

/*
 * Build a GBA ROM image of `size` bytes (padded to 4), filled with `fill` and
 * a valid Nintendo logo? No: the logo is not required by our core (documented).
 * Caller frees.
 */
uint8_t *gba_make_rom(size_t size, uint8_t fill, size_t *size_out);

#endif /* EMU_TESTUTIL_H */
