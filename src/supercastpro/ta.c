/*
 * supercastpro: Tile Accelerator subset (see dc.h for the documented
 * model). The SH-4 pushes parameter words into a FIFO; a STARTRENDER
 * pass walks the stream and rasterizes flat-shaded triangles, strips
 * and axis-aligned sprites into the VRAM framebuffer in stream order.
 */
#include "dc.h"

#include <math.h>
#include <string.h>

#include "../common/util.h"

/* ---- FIFO ----------------------------------------------------------------------------- */

void dc_ta_push(struct dc *d, uint32_t word)
{
    if (d->ta.count >= DC_TA_FIFO_WORDS) {
        d->ta.overflow = 1;
        return;
    }
    d->ta.fifo[d->ta.count++] = word;
}

/* ---- render target (VRAM framebuffer via FB_W_*) --------------------------------------- */

struct ta_target {
    uint8_t *base;     /* VRAM pointer at SOF1 */
    uint32_t stride;   /* bytes per line */
    uint32_t bpp;      /* bytes per pixel */
    uint8_t pack;      /* FB_W_CTRL pack field (bits 2:0) */
    uint32_t w, h;     /* clip bounds (640x480) */
};

static int ta_target_setup(struct dc *d, struct ta_target *t)
{
    uint32_t ctrl = emu_le32(&d->pvr_regs[DC_PVR_FB_W_CTRL]);
    uint32_t sof = emu_le32(&d->pvr_regs[DC_PVR_FB_W_SOF1]) & 0x03FFFFFFu;
    uint32_t stride_words = emu_le32(&d->pvr_regs[DC_PVR_FB_W_LINESTRIDE]) & 0xFFFFu;

    memset(t, 0, sizeof *t);
    t->pack = (uint8_t)(ctrl & 7u);
    switch (t->pack) {
    case 0u:
    case 1u:
        t->bpp = 2;
        break; /* RGB565 */
    case 3u:
        t->bpp = 3;
        break; /* RGB888 */
    case 4u:
        t->bpp = 4;
        break; /* ARGB8888 */
    default:
        return 0; /* unsupported pack: pass is a no-op (documented) */
    }

    /* The TA covers the full 640x480 tile space; the stride comes from
     * FB_W_LINESTRIDE (32-bit words, 0 = w*bpp/4). */
    t->w = DC_SCREEN_W;
    t->h = DC_SCREEN_H;
    t->stride = (stride_words != 0u) ? stride_words * 4u : t->w * t->bpp;

    if (sof >= DC_VRAM_SIZE)
        return 0;
    uint64_t span = (uint64_t)t->stride * (t->h - 1u) + (uint64_t)t->w * t->bpp;
    if (sof + span > DC_VRAM_SIZE)
        return 0;
    t->base = d->vram + sof;
    return 1;
}

static void ta_put_pixel(const struct ta_target *t, int32_t x, int32_t y,
                         uint32_t argb)
{
    if (x < 0 || y < 0 || (uint32_t)x >= t->w || (uint32_t)y >= t->h)
        return;
    uint8_t *px = t->base + (uint32_t)y * t->stride + (uint32_t)x * t->bpp;
    uint8_t a = (uint8_t)(argb >> 24), r = (uint8_t)(argb >> 16),
            g = (uint8_t)(argb >> 8), b = (uint8_t)argb;
    switch (t->pack) {
    case 0u:
    case 1u: {
        uint16_t c = (uint16_t)(((uint32_t)(r >> 3) << 11) |
                                ((uint32_t)(g >> 2) << 5) | (b >> 3));
        px[0] = (uint8_t)c;
        px[1] = (uint8_t)(c >> 8);
        break;
    }
    case 3u: /* RGB888: R,G,B byte order (mirrors the scanout decode) */
        px[0] = r;
        px[1] = g;
        px[2] = b;
        break;
    default: /* ARGB8888: B,G,R,A byte order (mirrors the scanout decode) */
        px[0] = b;
        px[1] = g;
        px[2] = r;
        px[3] = a;
        break;
    }
}

/* ---- rasterization ---------------------------------------------------------------------- */

/* Screen mapping: real TA convention (origin center, Y up). */
static void ta_screen(float x, float y, float *sx, float *sy)
{
    *sx = x + 320.0f;
    *sy = 240.0f - y;
}

static float ta_edge(float ax, float ay, float bx, float by, float px,
                     float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void ta_triangle(const struct ta_target *t, float x0, float y0,
                        float x1, float y1, float x2, float y2,
                        uint32_t color)
{
    /* Bounding box against the clip rect (pixel centers at +0.5). */
    float minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    float maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    float miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    float maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int32_t ix0 = (int32_t)minx;
    int32_t ix1 = (int32_t)maxx;
    int32_t iy0 = (int32_t)miny;
    int32_t iy1 = (int32_t)maxy;
    if (ix0 < 0)
        ix0 = 0;
    if (iy0 < 0)
        iy0 = 0;
    if (ix1 >= (int32_t)t->w)
        ix1 = (int32_t)t->w - 1;
    if (iy1 >= (int32_t)t->h)
        iy1 = (int32_t)t->h - 1;

    float area = ta_edge(x0, y0, x1, y1, x2, y2);
    if (area == 0.0f)
        return;
    for (int32_t y = iy0; y <= iy1; y++) {
        for (int32_t x = ix0; x <= ix1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = ta_edge(x0, y0, x1, y1, px, py);
            float w1 = ta_edge(x1, y1, x2, y2, px, py);
            float w2 = ta_edge(x2, y2, x0, y0, px, py);
            /* Inside if all edge functions share the winding sign. */
            if ((w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f))
                ta_put_pixel(t, x, y, color);
        }
    }
}

static void ta_rect(const struct ta_target *t, float xs, float ys, float xe,
                    float ye, uint32_t color)
{
    /* Pixel-center rule (consistent with the triangle test): pixel x is
     * inside iff lo <= x+0.5 < hi, i.e. the rect is half-open. */
    float xl = xs < xe ? xs : xe, xh = xs > xe ? xs : xe;
    float yl = ys < ye ? ys : ye, yh = ys > ye ? ys : ye;
    int32_t ix0 = (int32_t)ceilf(xl - 0.5f);
    int32_t ix1 = (int32_t)ceilf(xh - 0.5f) - 1;
    int32_t iy0 = (int32_t)ceilf(yl - 0.5f);
    int32_t iy1 = (int32_t)ceilf(yh - 0.5f) - 1;
    if (ix1 < 0 || iy1 < 0 || ix0 >= (int32_t)t->w ||
        iy0 >= (int32_t)t->h)
        return;
    if (ix0 < 0)
        ix0 = 0;
    if (iy0 < 0)
        iy0 = 0;
    if (ix1 >= (int32_t)t->w)
        ix1 = (int32_t)t->w - 1;
    if (iy1 >= (int32_t)t->h)
        iy1 = (int32_t)t->h - 1;
    for (int32_t y = iy0; y <= iy1; y++)
        for (int32_t x = ix0; x <= ix1; x++)
            ta_put_pixel(t, x, y, color);
}

/* ---- parameter stream ------------------------------------------------------------------- */

static float ta_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* Header word counts per the dc.h ParaType table: opaque poly types
 * 0-4 = 3/4/4/5/5 words, sprite = 3; transparent (0x90-0x95) and
 * punch-through (0x98-0x9D) variants add one word. */
static int ta_is_header(uint32_t byte)
{
    return (byte >= 0x88u && byte <= 0x8Du) ||
           (byte >= 0x90u && byte <= 0x95u) ||
           (byte >= 0x98u && byte <= 0x9Du);
}

static uint32_t ta_header_words(uint32_t byte)
{
    static const uint32_t poly[5] = { 3u, 4u, 4u, 5u, 5u };
    uint32_t extra = (byte >= 0x90u) ? 1u : 0u; /* transparent/punch */
    if (byte == 0x8Du || byte == 0x95u || byte == 0x9Du)
        return 3u + extra; /* sprite */
    if (byte >= 0x88u && byte <= 0x8Cu)
        return poly[byte - 0x88u] + extra;
    if (byte >= 0x90u && byte <= 0x94u)
        return poly[byte - 0x90u] + extra;
    return poly[byte - 0x98u] + extra; /* punch-through */
}

struct ta_vert {
    float sx, sy;
    uint32_t color;
};

static int ta_read_vertex(const uint32_t *fifo, uint32_t count, uint32_t *i,
                          struct ta_vert *v)
{
    if (*i + 5u > count)
        return 0; /* truncated stream (documented: stop) */
    if ((fifo[*i] & 0xF0000000u) != 0xE0000000u)
        return 0; /* tag word 0xE0-0xEF lives in the top byte */
    float x = ta_float(fifo[*i + 1u]);
    float y = ta_float(fifo[*i + 2u]);
    v->color = fifo[*i + 4u];
    ta_screen(x, y, &v->sx, &v->sy);
    *i += 5u;
    return 1;
}

void dc_ta_render(struct dc *d)
{
    struct ta_target t;    if (!ta_target_setup(d, &t)) {
        d->ta.count = 0;
        return;
    }

    const uint32_t *fifo = d->ta.fifo;
    uint32_t count = d->ta.count;
    uint32_t i = 0;

    while (i < count) {
        uint32_t full = fifo[i];
        uint32_t byte = full >> 24;
        if (byte == 0x83u)
            break; /* end of parameter list */
        if (byte == 0x80u || byte == 0x81u || byte == 0x82u ||
            byte == 0x87u) {
            i += 1u; /* skipped control words (documented: 1 word) */
            continue;
        }
        if (ta_is_header(byte)) {
            uint32_t sprite = (byte == 0x8Du || byte == 0x95u ||
                               byte == 0x9Du);
            i += ta_header_words(byte);
            if (sprite) {
                struct ta_vert a, b, c;
                if (!ta_read_vertex(fifo, count, &i, &a))
                    break;
                if (!ta_read_vertex(fifo, count, &i, &b))
                    break;
                if (!ta_read_vertex(fifo, count, &i, &c))
                    break;
                /* A = base, B = x edge, C = y edge (axis-aligned). */
                ta_rect(&t, a.sx, a.sy, b.sx, c.sy, a.color);
            } else {
                struct ta_vert v0, v1, v2;
                if (!ta_read_vertex(fifo, count, &i, &v0))
                    break;
                if (!ta_read_vertex(fifo, count, &i, &v1))
                    break;
                for (;;) {
                    if (!ta_read_vertex(fifo, count, &i, &v2))
                        break;
                    ta_triangle(&t, v0.sx, v0.sy, v1.sx, v1.sy, v2.sx,
                                v2.sy, v0.color);
                    v0 = v1;
                    v1 = v2;
                }
            }
            continue;
        }
        i += 1u; /* unknown control word: skip one (documented) */
    }

    d->ta.count = 0; /* consumed (documented) */
}
