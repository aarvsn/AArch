#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mgbax/gba.h"
#include "emu/emu.h"
#include "../tests/testutil.h"

int main(void)
{
    emu_core_t *c = NULL;
    emu_core_mgbax()->create(&c);
    struct gba *g = (struct gba *)c;
    size_t sz = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &sz);
    emu_core_mgbax()->load_rom(c, rom, sz);
    free(rom);
    gba_io_write16(g, 0x04000060u, 0xF080u);
    gba_io_write16(g, 0x04000064u, 0x8400u);
    printf("apu: duty=%u env=%u vol=%u active=%u period=%u\n",
           g->apu.sq_duty[0], g->apu.sq_env_vol[0], g->apu.sq_volume[0],
           g->apu.sq_active[0], g->apu.sq_period[0]);
    /* confirm the writes actually landed in the APU struct */
    gba_io_write16(g, 0x04000088u, 0x0800u);
    printf("bias=%04X\n", g->apu.soundbias);
    emu_core_mgbax()->run_frame(c);
    printf("after frame: active=%u pos=%u out_pos=%zu first samples:",
           g->apu.sq_active[0], g->apu.sq_duty_pos[0], g->apu.out_pos);
    for (size_t i = 0; i < 16 && i < g->apu.out_pos; i++)
        printf(" %d", g->audio[i]);
    printf("\n");
    emu_core_mgbax()->destroy(c);
    return 0;
}
