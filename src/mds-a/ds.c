/*
 * mds-a: Nintendo DS core (SKELETON).
 *
 * Status: NDS header validation only. Requires dual CPUs (ARM946E-S +
 * ARM7TDMI), two 2D PPUs, a 3D engine and Wi-Fi model to emulate.
 * run_frame() returns EMU_ENOTIMPL.
 */
#include "../common/skeleton.h"

#include <string.h>

static const uint8_t nds_logo8[8] = {
    0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21
};

static emu_result_t ds_validate(const uint8_t *d, size_t s)
{
    if (s < 0x200)
        return EMU_EBADROM;
    if (memcmp(d + 0x160, nds_logo8, 8) != 0)
        return EMU_EBADROM;
    return EMU_OK;
}

static const emu_skel_desc_t ds_desc = {
    "mds-a", "Nintendo DS", 256, 192, 32768, ds_validate
};

static const emu_core_vtable_t ds_vtable;

static emu_result_t ds_create(emu_core_t **out)
{
    emu_result_t r = emu_skel_create(out, &ds_desc);
    if (r == EMU_OK)
        (*out)->vtable = &ds_vtable;
    return r;
}
static emu_result_t ds_load(emu_core_t *c, const uint8_t *d, size_t s)
{
    return emu_skel_load_rom(c, d, s);
}
static const uint32_t *ds_fb(emu_core_t *c, uint32_t *w, uint32_t *h)
{
    return emu_skel_fb(c, w, h);
}

static const emu_core_vtable_t ds_vtable = {
    "mds-a", "Nintendo DS", 256, 192, 32768,
    ds_create, emu_skel_destroy, ds_load, emu_skel_reset,
    emu_skel_run_frame, ds_fb, emu_skel_set_input, NULL,
    emu_skel_state_size, emu_skel_save, emu_skel_load,
};

const emu_core_vtable_t *emu_core_mds_a(void)
{
    return &ds_vtable;
}
