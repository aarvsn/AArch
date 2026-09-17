/*
 * ms-32: Sega 32X core (SKELETON).
 *
 * Status: ROM header validation only. Requires the Genesis base system
 * plus two SH-2 CPUs, the VDP pixmap mode and a Genesis cartridge with the
 * adapter handshake. run_frame() returns EMU_ENOTIMPL.
 */
#include "../common/skeleton.h"

#include <string.h>

static emu_result_t x32_validate(const uint8_t *d, size_t s)
{
    if (s < 0x200)
        return EMU_EBADROM;
    if (memcmp(d + 0x100, "SEGA 32X", 8) != 0)
        return EMU_EBADROM;
    return EMU_OK;
}

static const emu_skel_desc_t x32_desc = {
    "ms-32", "Sega 32X", 320, 224, 48000, x32_validate
};

static const emu_core_vtable_t x32_vtable;

static emu_result_t x32_create(emu_core_t **out)
{
    emu_result_t r = emu_skel_create(out, &x32_desc);
    if (r == EMU_OK)
        (*out)->vtable = &x32_vtable;
    return r;
}
static emu_result_t x32_load(emu_core_t *c, const uint8_t *d, size_t s)
{
    return emu_skel_load_rom(c, d, s);
}
static const uint32_t *x32_fb(emu_core_t *c, uint32_t *w, uint32_t *h)
{
    return emu_skel_fb(c, w, h);
}

static const emu_core_vtable_t x32_vtable = {
    "ms-32", "Sega 32X", 320, 224, 48000,
    x32_create, emu_skel_destroy, x32_load, emu_skel_reset,
    emu_skel_run_frame, x32_fb, emu_skel_set_input, NULL,
    emu_skel_state_size, emu_skel_save, emu_skel_load,
};

const emu_core_vtable_t *emu_core_ms_32(void)
{
    return &x32_vtable;
}
