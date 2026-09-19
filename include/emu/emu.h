/*
 * AArch: lightweight, modular multi-system emulator framework.
 *
 * Public core interface. This header is the ONLY dependency a frontend needs.
 * It intentionally exposes a small, explicit API; each emulator core keeps its
 * own CPU/memory/PPU/APU implementation and never leaks system internals here.
 *
 * ABI version 1 (unchanged since the first release; registry/probe helpers are
 * additive and live in separate functions below).
 */
#ifndef EMU_EMU_H
#define EMU_EMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU_ABI_VERSION 1u

/* Result codes returned by fallible core operations. */
typedef enum {
    EMU_OK = 0,          /* success                                          */
    EMU_EINVAL,          /* invalid argument (NULL pointer, zero size, ...)  */
    EMU_ENOROM,          /* operation requires a loaded ROM                  */
    EMU_EUNSUPPORTED,    /* ROM/cartridge uses an unsupported mapping        */
    EMU_ENOSPACE,        /* destination buffer too small (nothing written)   */
    EMU_EBADSTATE,       /* save-state blob corrupt or foreign               */
    EMU_EBADROM,         /* ROM header malformed                             */
    EMU_ENOTIMPL         /* operation not implemented by this core (yet)     */
} emu_result_t;

/* Human-readable name for a result code. Never NULL. */
const char *emu_result_str(emu_result_t r);

/*
 * Opaque emulator core handle. Each core allocates a state struct whose FIRST
 * member is an emu_core_t (the "base"); cores downcast internally, which keeps
 * the public API free of per-core headers.
 */
typedef struct emu_core emu_core_t;

/*
 * Audio delivery: cores call cb with interleaved stereo int16 PCM (L,R,L,R...)
 * of exactly `count` samples (count is always even). Cores deliver the whole
 * frame's audio when run_frame() finishes; the callback must not re-enter the
 * core. Buffers handed to the callback are owned by the core and valid only
 * for the duration of the call.
 */
typedef void (*emu_audio_cb_t)(void *user, const int16_t *samples, size_t count);

/*
 * Video: XRGB8888 (0x00RRGGBB), top-to-bottom rows, no padding.
 * Framebuffer dimensions are fixed per system and reported by the vtable.
 */
#define EMU_PIXEL(r, g, b) \
    ((uint32_t)(0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r)))
#define EMU_PIXEL_R(px) ((uint8_t)((px) & 0xFFu))
#define EMU_PIXEL_G(px) ((uint8_t)(((px) >> 8) & 0xFFu))
#define EMU_PIXEL_B(px) ((uint8_t)(((px) >> 16) & 0xFFu))

/*
 * Input: one uint32_t bitmask per core. Bit positions follow the physical
 * layout of each controller and are documented per core:
 *
 * mgbx (Game Boy):        A=bit0 B=bit1 Select=bit2 Start=bit3
 *                         Right=bit4 Left=bit5 Up=bit6 Down=bit7
 * beatle-nes-redux (NES): A=bit0 B=bit1 Select=bit2 Start=bit3
 *                         Up=bit4 Down=bit5 Left=bit6 Right=bit7
 * supersnes (SNES):       B=bit0 Y=bit1 Select=bit2 Start=bit3
 *                         Up=bit4 Down=bit5 Left=bit6 Right=bit7
 *                         A=bit8 X=bit9 L=bit10 R=bit11
 * mgbax (GBA):            same layout as SNES
 */
typedef struct emu_core_vtable {
    const char *name;       /* core short name, e.g. "mgbx"           */
    const char *system;     /* human readable system, e.g. "Game Boy" */
    uint32_t fb_width;      /* fixed framebuffer width in pixels      */
    uint32_t fb_height;     /* fixed framebuffer height in pixels     */
    uint32_t sample_rate;   /* audio output sample rate in Hz         */

    emu_result_t (*create)(emu_core_t **out);
    void (*destroy)(emu_core_t *core);

    /* load_rom takes a private copy of the data; caller retains ownership. */
    emu_result_t (*load_rom)(emu_core_t *core, const uint8_t *data, size_t size);
    void (*reset)(emu_core_t *core);   /* back to power-on state, ROM retained */

    /* Run exactly one video frame. Returns EMU_ENOROM if no ROM loaded. */
    emu_result_t (*run_frame)(emu_core_t *core);

    /* Returns the internal framebuffer; valid until next run_frame/reset. */
    const uint32_t *(*framebuffer)(emu_core_t *core, uint32_t *w, uint32_t *h);

    void (*set_input)(emu_core_t *core, uint32_t buttons);
    void (*set_audio_callback)(emu_core_t *core, emu_audio_cb_t cb, void *user);

    /* Save states: explicit serialization; state_size() == save size. */
    size_t (*state_size)(emu_core_t *core);
    emu_result_t (*save_state)(emu_core_t *core, uint8_t *buf, size_t cap);
    emu_result_t (*load_state)(emu_core_t *core, const uint8_t *buf, size_t size);
} emu_core_vtable_t;

struct emu_core {
    const emu_core_vtable_t *vtable;
};

/* Core registries (compiled in only when the core is enabled in the build). */
const emu_core_vtable_t *emu_core_mgbx(void);            /* Game Boy / DMG     */
const emu_core_vtable_t *emu_core_beatle_nes_redux(void);/* NES / Famicom      */
const emu_core_vtable_t *emu_core_supersnes(void);       /* SNES               */
const emu_core_vtable_t *emu_core_mgbax(void);           /* Game Boy Advance   */
const emu_core_vtable_t *emu_core_finalburn(void);       /* Genesis / Mega Drive */
const emu_core_vtable_t *emu_core_beatle_psx(void);      /* PlayStation (skeleton) */
const emu_core_vtable_t *emu_core_mds_a(void);           /* Nintendo DS (skeleton) */
const emu_core_vtable_t *emu_core_ms_32(void);           /* Sega 32X (skeleton) */
const emu_core_vtable_t *emu_core_supersaturn(void);     /* Sega Saturn (skeleton) */
const emu_core_vtable_t *emu_core_m64_b(void);           /* Nintendo 64 (skeleton) */
const emu_core_vtable_t *emu_core_supercastpro(void);    /* Dreamcast (skeleton) */

/*
 * Core status used by frontends to report real capability. Status values are
 * plain data: a core's own test suite is the source of truth for WORKING and
 * PARTIAL; SKELETON cores reject run_frame with EMU_ENOTIMPL by contract.
 */
typedef enum {
    EMU_STATUS_WORKING = 0, /* boots, runs, audio, save states; suite green */
    EMU_STATUS_PARTIAL,     /* runs with documented functional gaps         */
    EMU_STATUS_SKELETON     /* structure + ROM detection only; cannot run   */
} emu_core_status_t;

typedef struct {
    const char *name;   /* core short name, matches vtable->name          */
    emu_core_status_t status;
    const char *note;   /* human-readable capability summary, never NULL  */
    /* vtable accessor; NULL when the core is not compiled into this build */
    const emu_core_vtable_t *(*vtable)(void);
} emu_core_info_t;

/*
 * Registry of every core KNOWN to this build configuration (built or not).
 * Returns a static array of *count_out entries; order is stable. The array is
 * owned by the library and valid for the program lifetime.
 */
const emu_core_info_t *emu_core_registry(size_t *count_out);

/*
 * Probe a ROM image and suggest a core short name (one of the registry
 * names, or "unknown"). Pure signature sniffing on the raw bytes; never
 * loads, allocates or fails. Ambiguous formats may be overridden by the
 * user with an explicit core choice in the frontend.
 */
const char *emu_rom_probe(const uint8_t *data, size_t size);

/* Human-readable name for a core status value. Never NULL. */
const char *emu_core_status_str(emu_core_status_t s);

/* Convenience wrappers around a vtable instance (NULL-safe where noted). */
emu_result_t emu_core_create(const emu_core_vtable_t *vt, emu_core_t **out);
void emu_core_destroy(emu_core_t *core);                       /* NULL-safe */

/*
 * CRC32 (IEEE 802.3, reflected, poly 0xEDB88320, init/final XOR 0xFFFFFFFF)
 * over pixel data; used by tests and tools to compare framebuffers without
 * inspecting pixels.
 */
uint32_t emu_crc32(const uint32_t *data, size_t count);

/* Input bitmaps for the additional cores (see vtable comment above):
 * finalburn (Genesis 3-button pad), layout chosen for v1:
 *                         Up=bit0 Down=bit1 Left=bit2 Right=bit3
 *                         A=bit4 B=bit5 C=bit6 Start=bit7
 * supersaturn (Saturn digital pad):
 *                         Start=bit3 Up=bit4 Down=bit5 Left=bit6 Right=bit7
 *                         A=bit8 B=bit9 C=bit10 X=bit11 Y=bit12 Z=bit13
 *                         L=bit14 R=bit15
 * Skeleton cores accept and store the mask but never read it.
 */

#ifdef __cplusplus
}
#endif
#endif /* EMU_EMU_H */
