/* Shared helpers for the finalburn (Genesis) test suites. */
#ifndef EMU_TESTS_FBH_H
#define EMU_TESTS_FBH_H

#include <stddef.h>
#include <stdint.h>

struct fb_md;

/*
 * Build a 128 KiB Genesis ROM with a valid "SEGA GENESIS" header and the
 * initial SSP ($01000000) and PC vectors. `entry` is the 68K start address.
 * Code and data can be patched with fb_rom_w16/w32. Caller frees.
 */
uint8_t *fb_test_rom(size_t *size_out, uint32_t entry);

void fb_rom_w16(uint8_t *rom, size_t off, uint16_t v);
void fb_rom_w32(uint8_t *rom, size_t off, uint32_t v);

/* Create a core, load `rom`, return the white-box machine pointer. */
struct fb_md *fb_boot(uint8_t *rom, size_t size);

/* Create a core without a ROM (for VDP/PSG/YM white-box tests). */
struct fb_md *fb_create_bare(void);

/* Run exactly one video frame through the public vtable. */
void fb_frame(struct fb_md *md);

/* Genesis ROM with a PSG tone program (shared by audio/state suites). */
uint8_t *psg_rom_shared(size_t *size);

#endif /* EMU_TESTS_FBH_H */
