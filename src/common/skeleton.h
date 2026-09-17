/*
 * Skeleton-core scaffolding (shared implementation).
 *
 * Skeleton cores parse and validate their ROM/disc format, hold the image,
 * and report EMU_ENOTIMPL for anything that needs real emulation. This is
 * the honest contract used by the CLI: a skeleton shows up in --list with
 * status "skeleton" and refuses to run rather than pretending.
 */
#ifndef EMU_COMMON_SKELETON_H
#define EMU_COMMON_SKELETON_H

#include "emu/emu.h"

typedef struct {
    const char *name;    /* core short name */
    const char *system;  /* human-readable system name */
    uint32_t w, h;       /* nominal framebuffer size */
    uint32_t rate;       /* nominal audio rate */
    emu_result_t (*validate)(const uint8_t *data, size_t size); /* may be NULL */
} emu_skel_desc_t;

emu_result_t emu_skel_create(emu_core_t **out, const emu_skel_desc_t *desc);
void emu_skel_destroy(emu_core_t *core);
emu_result_t emu_skel_load_rom(emu_core_t *core, const uint8_t *data, size_t size);
void emu_skel_reset(emu_core_t *core);
emu_result_t emu_skel_run_frame(emu_core_t *core);
const uint32_t *emu_skel_fb(emu_core_t *core, uint32_t *w, uint32_t *h);
void emu_skel_set_input(emu_core_t *core, uint32_t buttons);
size_t emu_skel_state_size(emu_core_t *core);
emu_result_t emu_skel_save(emu_core_t *core, uint8_t *buf, size_t cap);
emu_result_t emu_skel_load(emu_core_t *core, const uint8_t *buf, size_t size);

#endif /* EMU_COMMON_SKELETON_H */
