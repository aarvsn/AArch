/*
 * m64-b: Nintendo 64 core (SKELETON).
 *
 * Status: ROM signature + byte-order detection only (z64 big-endian,
 * v64 byteswapped, n64 little-endian). The real implementation needs the
 * VR4300 CPU, RSP/RDP RCP engines and RDRAM/PI bus model. run_frame()
 * returns EMU_ENOTIMPL.
 */
#include "../common/skeleton.h"

#include <string.h>

static emu_result_t n64_validate(const uint8_t *d, size_t s)
{
    if (s < 0x1000)
        return EMU_EBADROM;
    if (d[0] == 0x80 && d[1] == 0x37 && d[2] == 0x12 && d[3] == 0x40)
        return EMU_OK; /* .z64 big-endian */
    if (d[0] == 0x37 && d[1] == 0x80 && d[2] == 0x40 && d[3] == 0x12)
        return EMU_OK; /* .v64 byteswapped */
    if (d[0] == 0x12 && d[1] == 0x40 && d[2] == 0x80 && d[3] == 0x37)
        return EMU_OK; /* .n64 little-endian */
    return EMU_EBADROM;
}

static const emu_skel_desc_t n64_desc = {
    "m64-b", "Nintendo 64", 320, 240, 32000, n64_validate
};

static const emu_core_vtable_t n64_vtable;

static emu_result_t n64_create(emu_core_t **out)
{
    emu_result_t r = emu_skel_create(out, &n64_desc);
    if (r == EMU_OK)
        (*out)->vtable = &n64_vtable;
    return r;
}
static emu_result_t n64_load(emu_core_t *c, const uint8_t *d, size_t s)
{
    return emu_skel_load_rom(c, d, s);
}
static const uint32_t *n64_fb(emu_core_t *c, uint32_t *w, uint32_t *h)
{
    return emu_skel_fb(c, w, h);
}

static const emu_core_vtable_t n64_vtable = {
    "m64-b", "Nintendo 64", 320, 240, 32000,
    n64_create, emu_skel_destroy, n64_load, emu_skel_reset,
    emu_skel_run_frame, n64_fb, emu_skel_set_input, NULL,
    emu_skel_state_size, emu_skel_save, emu_skel_load,
};

const emu_core_vtable_t *emu_core_m64_b(void)
{
    return &n64_vtable;
}
