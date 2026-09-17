/*
 * beatle-psx: Sony PlayStation core (SKELETON).
 *
 * Status: ROM/disc detection only. The real implementation needs a MIPS
 * R3000A (+GTE coprocessor), CD-ROM/SPU/GPU models and a BIOS image, which
 * cannot be bundled for legal reasons. run_frame() returns EMU_ENOTIMPL.
 */
#include "../common/skeleton.h"

#include <string.h>

static emu_result_t psx_validate(const uint8_t *d, size_t s)
{
    if (s >= 8 && memcmp(d, "PS-X EXE", 8) == 0)
        return EMU_OK; /* raw executable */
    if (s >= 32800 && memcmp(d + 32769, "CD001", 5) == 0 &&
        memcmp(d + 32776, "PLAYSTATION", 11) == 0)
        return EMU_OK; /* ISO9660 image */
    return EMU_EBADROM;
}

static const emu_skel_desc_t psx_desc = {
    "beatle-psx", "Sony PlayStation", 320, 240, 44100, psx_validate
};

static const emu_core_vtable_t psx_vtable;

static emu_result_t psx_create(emu_core_t **out)
{
    emu_result_t r = emu_skel_create(out, &psx_desc);
    if (r == EMU_OK)
        (*out)->vtable = &psx_vtable;
    return r;
}
static emu_result_t psx_load(emu_core_t *c, const uint8_t *d, size_t s)
{
    return emu_skel_load_rom(c, d, s);
}
static const uint32_t *psx_fb(emu_core_t *c, uint32_t *w, uint32_t *h)
{
    return emu_skel_fb(c, w, h);
}

static const emu_core_vtable_t psx_vtable = {
    "beatle-psx", "Sony PlayStation", 320, 240, 44100,
    psx_create, emu_skel_destroy, psx_load, emu_skel_reset,
    emu_skel_run_frame, psx_fb, emu_skel_set_input, NULL,
    emu_skel_state_size, emu_skel_save, emu_skel_load,
};

const emu_core_vtable_t *emu_core_beatle_psx(void)
{
    return &psx_vtable;
}
