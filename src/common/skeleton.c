/* Shared skeleton-core implementation; see skeleton.h for the contract. */
#include "skeleton.h"

#include <stdlib.h>
#include <string.h>

struct emu_skel {
    emu_core_t base;
    const emu_skel_desc_t *desc;
    uint8_t *rom;
    size_t rom_size;
    uint32_t input;
    uint32_t *fb;
};

emu_result_t emu_skel_create(emu_core_t **out, const emu_skel_desc_t *desc)
{
    if (out == NULL || desc == NULL)
        return EMU_EINVAL;
    struct emu_skel *k = calloc(1, sizeof *k);
    if (k == NULL)
        return EMU_EINVAL;
    k->fb = calloc(desc->w * desc->h, sizeof(uint32_t));
    if (k->fb == NULL) {
        free(k);
        return EMU_EINVAL;
    }
    k->desc = desc;
    k->base.vtable = NULL; /* set by the per-core vtable wrapper */
    *out = &k->base;
    return EMU_OK;
}

void emu_skel_destroy(emu_core_t *core)
{
    struct emu_skel *k = (struct emu_skel *)core;
    if (k == NULL)
        return;
    free(k->rom);
    free(k->fb);
    free(k);
}

emu_result_t emu_skel_load_rom(emu_core_t *core, const uint8_t *data, size_t size)
{
    struct emu_skel *k = (struct emu_skel *)core;
    if (data == NULL || size == 0)
        return EMU_EINVAL;
    if (k->desc->validate != NULL) {
        emu_result_t r = k->desc->validate(data, size);
        if (r != EMU_OK)
            return r;
    }
    uint8_t *copy = malloc(size);
    if (copy == NULL)
        return EMU_EINVAL;
    memcpy(copy, data, size);
    free(k->rom);
    k->rom = copy;
    k->rom_size = size;
    return EMU_OK;
}

void emu_skel_reset(emu_core_t *core)
{
    (void)core; /* nothing to reset yet (documented) */
}

emu_result_t emu_skel_run_frame(emu_core_t *core)
{
    struct emu_skel *k = (struct emu_skel *)core;
    if (k->rom == NULL)
        return EMU_ENOROM;
    return EMU_ENOTIMPL;
}

const uint32_t *emu_skel_fb(emu_core_t *core, uint32_t *w, uint32_t *h)
{
    struct emu_skel *k = (struct emu_skel *)core;
    if (w)
        *w = k->desc->w;
    if (h)
        *h = k->desc->h;
    return k->fb;
}

void emu_skel_set_input(emu_core_t *core, uint32_t buttons)
{
    struct emu_skel *k = (struct emu_skel *)core;
    k->input = buttons; /* stored, never read (documented) */
}

size_t emu_skel_state_size(emu_core_t *core)
{
    (void)core;
    return 0;
}

emu_result_t emu_skel_save(emu_core_t *core, uint8_t *buf, size_t cap)
{
    (void)core;
    (void)buf;
    (void)cap;
    return EMU_ENOTIMPL;
}

emu_result_t emu_skel_load(emu_core_t *core, const uint8_t *buf, size_t size)
{
    (void)core;
    (void)buf;
    (void)size;
    return EMU_ENOTIMPL;
}
