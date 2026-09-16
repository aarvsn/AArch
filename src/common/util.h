/* Internal shared helpers for emu-fw cores. Not part of the public API. */
#ifndef EMU_COMMON_UTIL_H
#define EMU_COMMON_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "emu/emu.h"

/* Round-up helpers for allocation sizing. */
size_t emu_size_max(size_t a, size_t b);

/* Little-endian byte access (endian-safe blob reading for state/ROM). */
uint16_t emu_le16(const uint8_t *p);
uint32_t emu_le32(const uint8_t *p);
void emu_store_le16(uint8_t *p, uint16_t v);
void emu_store_le32(uint8_t *p, uint32_t v);

/*
 * Save-state serialization helpers.
 *
 * Every core serializes with ONE function, emu_<core>_serialize(), that writes
 * through an emu_state_io. For sizing, pass a writer with buf == NULL:
 * fields are counted only. For saving, a real buffer is required and overflow
 * is detected and flagged (EMU_ENOSPACE, nothing may be assumed about partial
 * content after a flagged overflow — the core returns ENOSPACE and the caller
 * must discard the buffer).
 *
 * Multi-byte values are written little-endian byte-wise so blobs are portable
 * across host endianness. Pointers are never serialized.
 */
typedef struct {
    uint8_t *buf;   /* NULL => count-only mode            */
    size_t cap;     /* buffer capacity (0 in count mode)  */
    size_t pos;     /* current write offset               */
    int overflow;   /* set when a write exceeded cap      */
} emu_state_writer;

void sw_u8(emu_state_writer *w, uint8_t v);
void sw_u16(emu_state_writer *w, uint16_t v);
void sw_u32(emu_state_writer *w, uint32_t v);
void sw_u64(emu_state_writer *w, uint64_t v);
void sw_i8(emu_state_writer *w, int8_t v);
void sw_i16(emu_state_writer *w, int16_t v);
void sw_i32(emu_state_writer *w, int32_t v);
void sw_i64(emu_state_writer *w, int64_t v);
void sw_mem(emu_state_writer *w, const void *p, size_t n);

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
    int bad;        /* set when a read exceeded size (blob truncated) */
} emu_state_reader;

uint8_t sr_u8(emu_state_reader *r);
uint16_t sr_u16(emu_state_reader *r);
uint32_t sr_u32(emu_state_reader *r);
uint64_t sr_u64(emu_state_reader *r);
int8_t sr_i8(emu_state_reader *r);
int16_t sr_i16(emu_state_reader *r);
int32_t sr_i32(emu_state_reader *r);
int64_t sr_i64(emu_state_reader *r);
void sr_mem(emu_state_reader *r, void *p, size_t n);

/* Every core's state blob starts with this header. */
#define EMU_STATE_MAGIC 0x46554D45u /* "EMUF" little-endian */
#define EMU_STATE_VERSION 1u

#endif /* EMU_COMMON_UTIL_H */
