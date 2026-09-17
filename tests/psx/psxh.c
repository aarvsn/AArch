/* Shared helpers for the beatle-psx test suites (implementation). */
#include "psxh.h"

#include "emu/emu.h"
#include "beatle-psx/psx.h"

#include <stdlib.h>
#include <string.h>

static void w32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
}

uint8_t *psx_test_exe(uint32_t pc0, uint32_t t_addr, const uint32_t *code,
                      size_t words, uint32_t sp_in, size_t *size_out)
{
    size_t size = 0x800u + words * 4u;
    uint8_t *exe = calloc(1, size);
    if (exe == NULL)
        return NULL;
    memcpy(exe, "PS-X EXE", 8);
    w32(exe, 0x0C, 0x00008800u); /* spec: initial S/C marker */
    w32(exe, 0x10, pc0);
    w32(exe, 0x14, 0);
    w32(exe, 0x18, t_addr);
    w32(exe, 0x1C, (uint32_t)(words * 4u));
    w32(exe, 0x30, sp_in);
    w32(exe, 0x7C, 0x00000000u);
    for (size_t i = 0; i < words; i++)
        w32(exe, 0x800u + i * 4u, code[i]);
    *size_out = size;
    return exe;
}

void psx_exe_patch(uint8_t *exe, uint32_t word_index, uint32_t v)
{
    w32(exe, 0x800u + word_index * 4u, v);
}

struct psx *psx_boot(uint8_t *exe, size_t size)
{
    emu_core_t *c = NULL;
    if (emu_core_beatle_psx()->create(&c) != EMU_OK)
        return NULL;
    if (emu_core_beatle_psx()->load_rom(c, exe, size) != EMU_OK) {
        emu_core_beatle_psx()->destroy(c);
        return NULL;
    }
    return (struct psx *)c;
}

struct psx *psx_bare(void)
{
    emu_core_t *c = NULL;
    if (emu_core_beatle_psx()->create(&c) != EMU_OK)
        return NULL;
    return (struct psx *)c;
}

void psx_run_n(struct psx *p, int count)
{
    for (int i = 0; i < count; i++)
        psx_cpu_step(&p->cpu);
}

int psx_run_until(struct psx *p, uint32_t watch_addr, uint32_t watch_val,
                  int max_steps)
{
    uint32_t off = watch_addr & (PSX_RAM_SIZE - 1u);
    for (int i = 0; i < max_steps; i++) {
        uint32_t v = (uint32_t)p->ram[off] |
                     ((uint32_t)p->ram[off + 1] << 8) |
                     ((uint32_t)p->ram[off + 2] << 16) |
                     ((uint32_t)p->ram[off + 3] << 24);
        if (v == watch_val)
            return 1;
        psx_cpu_step(&p->cpu);
    }
    return 0;
}
