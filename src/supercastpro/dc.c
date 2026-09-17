/*
 * supercastpro: Sega Dreamcast core (SKELETON).
 *
 * Status: IP.BIN disc header validation only. Requires the SH-4 CPU with
 * 3D FPU, PowerVR2 GPU, AICA ARM7 sound + DSP and a GD-ROM drive model.
 * run_frame() returns EMU_ENOTIMPL.
 */
#include "../common/skeleton.h"

#include <string.h>

static emu_result_t dc_validate(const uint8_t *d, size_t s)
{
    if (s < 0x800)
        return EMU_EBADROM;
    if (memcmp(d, "SEGA SEGAKATANA", 15) != 0)
        return EMU_EBADROM;
    return EMU_OK;
}

static const emu_skel_desc_t dc_desc = {
    "supercastpro", "Sega Dreamcast", 640, 480, 44100, dc_validate
};

static const emu_core_vtable_t dc_vtable;

static emu_result_t dc_create(emu_core_t **out)
{
    emu_result_t r = emu_skel_create(out, &dc_desc);
    if (r == EMU_OK)
        (*out)->vtable = &dc_vtable;
    return r;
}
static emu_result_t dc_load(emu_core_t *c, const uint8_t *d, size_t s)
{
    return emu_skel_load_rom(c, d, s);
}
static const uint32_t *dc_fb(emu_core_t *c, uint32_t *w, uint32_t *h)
{
    return emu_skel_fb(c, w, h);
}

static const emu_core_vtable_t dc_vtable = {
    "supercastpro", "Sega Dreamcast", 640, 480, 44100,
    dc_create, emu_skel_destroy, dc_load, emu_skel_reset,
    emu_skel_run_frame, dc_fb, emu_skel_set_input, NULL,
    emu_skel_state_size, emu_skel_save, emu_skel_load,
};

const emu_core_vtable_t *emu_core_supercastpro(void)
{
    return &dc_vtable;
}
