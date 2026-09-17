/* AArch shared helpers: CRC32, endian-safe access, state serialization. */
#include "util.h"

#include <string.h>

const char *emu_result_str(emu_result_t r)
{
    switch (r) {
    case EMU_OK:            return "ok";
    case EMU_EINVAL:        return "invalid argument";
    case EMU_ENOROM:        return "no ROM loaded";
    case EMU_EUNSUPPORTED:  return "unsupported feature or mapping";
    case EMU_ENOSPACE:      return "destination buffer too small";
    case EMU_EBADSTATE:     return "save state corrupt or foreign";
    case EMU_EBADROM:       return "ROM header malformed";
    case EMU_ENOTIMPL:      return "not implemented by this core";
    }
    return "unknown error";
}

size_t emu_size_max(size_t a, size_t b)
{
    return a > b ? a : b;
}

emu_result_t emu_core_create(const emu_core_vtable_t *vt, emu_core_t **out)
{
    if (vt == NULL || out == NULL)
        return EMU_EINVAL;
    return vt->create(out);
}

void emu_core_destroy(emu_core_t *core)
{
    if (core == NULL)
        return;
    core->vtable->destroy(core);
}

uint16_t emu_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t emu_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void emu_store_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

void emu_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

uint32_t emu_crc32(const uint32_t *data, size_t count)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < count; i++) {
        uint32_t v = data[i];
        for (int b = 0; b < 4; b++) {
            crc ^= v & 0xFFu;
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            v >>= 8;
        }
    }
    return ~crc;
}

/* ---- state writer ---- */

void sw_u8(emu_state_writer *w, uint8_t v)
{
    if (w->buf != NULL) {
        if (w->pos < w->cap)
            w->buf[w->pos] = v;
        else
            w->overflow = 1;
    }
    w->pos++;
}

void sw_u16(emu_state_writer *w, uint16_t v)
{
    sw_u8(w, (uint8_t)(v & 0xFFu));
    sw_u8(w, (uint8_t)((v >> 8) & 0xFFu));
}

void sw_u32(emu_state_writer *w, uint32_t v)
{
    sw_u16(w, (uint16_t)(v & 0xFFFFu));
    sw_u16(w, (uint16_t)((v >> 16) & 0xFFFFu));
}

void sw_u64(emu_state_writer *w, uint64_t v)
{
    sw_u32(w, (uint32_t)(v & 0xFFFFFFFFu));
    sw_u32(w, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

void sw_i8(emu_state_writer *w, int8_t v)
{
    sw_u8(w, (uint8_t)v);
}

void sw_i16(emu_state_writer *w, int16_t v)
{
    sw_u16(w, (uint16_t)v);
}

void sw_i32(emu_state_writer *w, int32_t v)
{
    sw_u32(w, (uint32_t)v);
}

void sw_i64(emu_state_writer *w, int64_t v)
{
    sw_u64(w, (uint64_t)v);
}

void sw_mem(emu_state_writer *w, const void *p, size_t n)
{
    if (w->buf != NULL) {
        if (w->pos <= w->cap && n <= w->cap - w->pos) {
            if (n > 0)
                memcpy(&w->buf[w->pos], p, n);
        } else {
            w->overflow = 1;
        }
    }
    w->pos += n;
}

/* ---- state reader ---- */

uint8_t sr_u8(emu_state_reader *r)
{
    if (r->pos < r->size)
        return r->buf[r->pos++];
    r->bad = 1;
    return 0;
}

uint16_t sr_u16(emu_state_reader *r)
{
    uint16_t lo = sr_u8(r);
    uint16_t hi = sr_u8(r);
    return (uint16_t)(lo | (hi << 8));
}

uint32_t sr_u32(emu_state_reader *r)
{
    uint32_t lo = sr_u16(r);
    uint32_t hi = sr_u16(r);
    return lo | (hi << 16);
}

uint64_t sr_u64(emu_state_reader *r)
{
    uint64_t lo = sr_u32(r);
    uint64_t hi = sr_u32(r);
    return lo | (hi << 32);
}

int8_t sr_i8(emu_state_reader *r)
{
    return (int8_t)sr_u8(r);
}

int16_t sr_i16(emu_state_reader *r)
{
    return (int16_t)sr_u16(r);
}

int32_t sr_i32(emu_state_reader *r)
{
    return (int32_t)sr_u32(r);
}

int64_t sr_i64(emu_state_reader *r)
{
    return (int64_t)sr_u64(r);
}

void sr_mem(emu_state_reader *r, void *p, size_t n)
{
    if (r->pos > r->size || n > r->size - r->pos) {
        r->bad = 1;
        if (p != NULL && r->pos < r->size) {
            memcpy(p, &r->buf[r->pos], r->size - r->pos);
            memset((uint8_t *)p + (r->size - r->pos), 0, n - (r->size - r->pos));
        } else if (p != NULL) {
            memset(p, 0, n);
        }
        r->pos = r->size;
        return;
    }
    if (n > 0)
        memcpy(p, &r->buf[r->pos], n);
    r->pos += n;
}
