/* Shared helpers for the beatle-psx test suites. */
#ifndef EMU_TESTS_PSXH_H
#define EMU_TESTS_PSXH_H

#include <stddef.h>
#include <stdint.h>

struct psx;

/*
 * Build a PS-X EXE image: 0x800-byte header + code/data. The program is
 * placed at `t_addr` (e.g. 0x80010000), entry at pc0. Caller frees.
 * SP defaults to 801FFFF0h when sp_in is 0.
 */
uint8_t *psx_test_exe(uint32_t pc0, uint32_t t_addr, const uint32_t *code,
                      size_t words, uint32_t sp_in, size_t *size_out);

/* Patch a word inside the EXE payload (offset in words from code start). */
void psx_exe_patch(uint8_t *exe, uint32_t word_index, uint32_t v);

/* Create core, load EXE, return the white-box machine. */
struct psx *psx_boot(uint8_t *exe, size_t size);

/* Create a bare core (no ROM) for GPU white-box tests. */
struct psx *psx_bare(void);

/* Execute N CPU instructions (white-box). */
void psx_run_n(struct psx *p, int count);

/* Run until a RAM watch address equals a value (bounded by max_steps). */
int psx_run_until(struct psx *p, uint32_t watch_addr, uint32_t watch_val,
                  int max_steps);

#endif /* EMU_TESTS_PSXH_H */
