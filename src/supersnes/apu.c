/*
 * supersnes APU stub.
 *
 * The S-SMP (SPC700) and S-DSP are NOT implemented in this milestone. The
 * APU communication ports ($2140-$2143, mirrored through $217F) answer with
 * fixed handshake-style values so that games which poll the ports do not
 * hang forever. Audio output is silent. This is a documented limitation.
 */
#include "snes.h"

#include <string.h>

void snes_apu_reset(snes_apu *a)
{
    memset(a, 0, sizeof *a);
}

uint8_t snes_apu_read(snes_apu *a, uint16_t addr)
{
    uint8_t port = (uint8_t)(addr & 3u);
    /* CPU->APU ports return the last written value with a fixed handshake
     * bit pattern; APU->CPU ports return 0xAA/0xBB style markers. */
    if (port == 0u)
        return (uint8_t)(a->ports[0] | 0xA0u);
    if (port == 1u)
        return (uint8_t)(a->ports[1] | 0xB0u);
    return a->ports[port];
}

void snes_apu_write(snes_apu *a, uint16_t addr, uint8_t v)
{
    uint8_t port = (uint8_t)(addr & 3u);
    a->ports[port] = v;
}
