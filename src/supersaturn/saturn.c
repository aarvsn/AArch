/*
 * supersaturn: Sega Saturn core (SKELETON).
 *
 * Status: IP.BIN disc header validation only. Requires two SH-2 CPUs, a
 * VDP1/VDP2 pair, the SCU DSP and a CD drive model. run_frame() returns
 * EMU_ENOTIMPL.
 */
#include "../common/skeleton.h"

#include <string.h>

static emu_result_t saturn_validate(const uint8_t *d, size_t s)
{
    if (s < 0x800)
        return EMU_EBADROM;
    if (memcmp(d, "SEGA SEGASATURN", 15) != 0)
        return EMU_EBADROM;
    return EMU_OK;
}

static const emu_skel_desc_t saturn_desc = {
    "supersaturn", "Sega Saturn", 320, 224, 44100, saturn_validate
};

static const emu_core_vtable_t saturn_vtable;

static emu_result_t saturn_create(emu_core_t **out)
{
    emu_result_t r = emu_skel_create(out, &saturn_desc);
    if (r == EMU_OK)
        (*out)->vtable = &saturn_vtable;
    return r;
}
static emu_result_t saturn_load(emu_core_t *c, const uint8_t *d, size_t s)
{
    return emu_skel_load_rom(c, d, s);
}
static const uint32_t *saturn_fb(emu_core_t *c, uint32_t *w, uint32_t *h)
{
    return emu_skel_fb(c, w, h);
}

static const emu_core_vtable_t saturn_vtable = {
    "supersaturn", "Sega Saturn", 320, 224, 44100,
    saturn_create, emu_skel_destroy, saturn_load, emu_skel_reset,
    emu_skel_run_frame, saturn_fb, emu_skel_set_input, NULL,
    emu_skel_state_size, emu_skel_save, emu_skel_load,
};

const emu_core_vtable_t *emu_core_supersaturn(void)
{
    return &saturn_vtable;
}
