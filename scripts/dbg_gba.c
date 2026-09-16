/* Debug trace for mgbax thumb hi-reg ops */
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
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &rom_size);
    static const uint16_t prog[] = {
        0x2020u, /* MOV R0, #0x20 */
        0x4680u, /* MOV R8, R0 */
        0x4480u, /* ADD R8, R0 */
        0x2101u, /* MOV R1, #1 */
        0x4588u, /* CMP R8, R1 */
        0xE7FEu
    };
    memcpy(rom, prog, sizeof prog);
    emu_core_mgbax()->load_rom(&g->base, rom, rom_size);
    free(rom);
    g->cpu.cpsr |= GBA_T;
    for (int i = 0; i < 5; i++) {
        gba_cpu_step(g);
        printf("step %d: r0=%08X r1=%08X r8=%08X r15=%08X cpsr=%08X\n", i,
               g->cpu.r[0], g->cpu.r[1], g->cpu.r[8], g->cpu.r[15],
               g->cpu.cpsr);
    }
    emu_core_mgbax()->destroy(&g->base);
    return 0;
}
