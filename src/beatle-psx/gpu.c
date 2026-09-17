/*
 * beatle-psx: GPU - 1 MiB VRAM, GP0/GP1 command processing and rasterizer.
 *
 * Implemented per spec: fill, CPU->VRAM and VRAM->CPU transfers, VRAM->VRAM
 * copies, monochrome/gouraud/textured triangles and quads (texture-blending
 * and raw), lines and poly-lines, rectangles (variable/1x1/8x8/16x16, mono
 * and textured), draw mode/texpage, texture window, drawing area/offset,
 * mask settings, 4bpp/8bpp/15bit textures with CLUT, all four semi-
 * transparency modes, 4x4 dithering, 15/24-bit display output.
 * Documented approximation: triangle rasterization uses integer barycentric
 * coordinates with an inclusive fill rule (the hardware tie-breaking rule is
 * not replicated; results differ at most on shared polygon edges).
 */
#include "psx.h"

#include <stdlib.h>
#include <string.h>

#define VRAM_MASK 0x8000u /* bit15 = mask bit storage */

static const int gte_dither[4][4] = {
    { -4, 0, -3, 1 },
    { 2, -2, 3, -1 },
    { -3, 1, -4, 0 },
    { 3, -1, 2, -2 },
};

void psx_gpu_init(psx_gpu_t *g)
{
    psx_gpu_reset(g);
}

void psx_gpu_reset(psx_gpu_t *g)
{
    memset(g->vram, 0, sizeof g->vram);
    g->fifo_len = 0;
    g->fifo_need = 0;
    g->fifo_active = 0;
    g->tpage = 0;
    g->texwin = 0;
    g->area_x1 = 0;
    g->area_y1 = 0;
    g->area_x2 = 1023;
    g->area_y2 = 511;
    g->off_x = 0;
    g->off_y = 0;
    g->mask_set = 0;
    g->mask_check = 0;
    g->read_pending = 0;
    g->read_x = g->read_y = g->read_w = g->read_h = 0;
    g->read_px = g->read_py = 0;
    g->xfer_active = 0;
    g->xfer_left = 0;
    g->xfer_x = g->xfer_y = g->xfer_x0 = g->xfer_w = g->xfer_h = 0;
    g->gread = 0;
    g->gpustat = 0x14802000u; /* spec: value after GP1(00h) */
    g->disp_x = 0;
    g->disp_y = 0;
    g->disp_w = 320;
    g->disp_h = 240;
    g->disp_depth24 = 0;
    g->disp_enabled = 0;
}

/* ---- VRAM pixel helpers ------------------------------------------------------- */

static inline uint16_t vram_get(const psx_gpu_t *g, uint32_t x, uint32_t y)
{
    return g->vram[(y & (PSX_VRAM_H - 1u)) * PSX_VRAM_W +
                   (x & (PSX_VRAM_W - 1u))];
}

static inline void vram_put(psx_gpu_t *g, uint32_t x, uint32_t y, uint16_t v)
{
    g->vram[(y & (PSX_VRAM_H - 1u)) * PSX_VRAM_W +
            (x & (PSX_VRAM_W - 1u))] = v;
}

/* Write with mask handling. */
static inline void vram_put_masked(psx_gpu_t *g, uint32_t x, uint32_t y,
                                   uint16_t v)
{
    uint16_t *p =
        &g->vram[(y & (PSX_VRAM_H - 1u)) * PSX_VRAM_W + (x & (PSX_VRAM_W - 1u))];
    if (g->mask_check && (*p & VRAM_MASK))
        return;
    uint16_t nv = (uint16_t)(v & 0x7FFFu);
    if (g->mask_set)
        nv |= VRAM_MASK;
    *p = nv;
}

static inline int in_area(const psx_gpu_t *g, int32_t x, int32_t y)
{
    return x >= (int32_t)g->area_x1 && x <= (int32_t)g->area_x2 &&
           y >= (int32_t)g->area_y1 && y <= (int32_t)g->area_y2;
}

/* ---- color conversion ------------------------------------------------------------ */

static inline uint16_t rgb24_to_15(uint32_t c)
{
    uint32_t r = (c & 0xF8u) >> 3;
    uint32_t gg = ((c >> 8) & 0xF8u) >> 3;
    uint32_t b = ((c >> 16) & 0xF8u) >> 3;
    return (uint16_t)(r | (gg << 5) | (b << 10));
}

static inline uint32_t rgb15_to_24(uint16_t c)
{
    uint32_t r = (c & 0x1Fu) << 3;
    uint32_t gg = ((c >> 5) & 0x1Fu) << 3;
    uint32_t b = ((c >> 10) & 0x1Fu) << 3;
    r |= r >> 5;
    gg |= gg >> 5;
    b |= b >> 5;
    return 0xFF000000u | (b << 16) | (gg << 8) | r;
}

static inline uint32_t chan5(int32_t v, int32_t dith)
{
    int32_t x = v + dith;
    if (x < 0)
        x = 0;
    if (x > 255)
        x = 255;
    return (uint32_t)(x >> 3);
}

/* ---- semi-transparency ------------------------------------------------------------ */

static uint16_t blend(uint32_t mode, uint16_t bg, uint16_t fg)
{
    int32_t br = bg & 0x1Fu, bgg = (bg >> 5) & 0x1Fu, bb = (bg >> 10) & 0x1Fu;
    int32_t fr = fg & 0x1Fu, fgg = (fg >> 5) & 0x1Fu, fbb = (fg >> 10) & 0x1Fu;
    int32_t r, g2, b;
    switch (mode) {
    case 0: /* B/2 + F/2 (each channel halved separately, truncated) */
        r = br / 2 + fr / 2;
        g2 = bgg / 2 + fgg / 2;
        b = bb / 2 + fbb / 2;
        break;
    case 1: /* B + F */
        r = br + fr;
        g2 = bgg + fgg;
        b = bb + fbb;
        break;
    case 2: /* B - F */
        r = br - fr;
        g2 = bgg - fgg;
        b = bb - fbb;
        break;
    default: /* B + F/4 */
        r = br + fr / 4;
        g2 = bgg + fgg / 4;
        b = bb + fbb / 4;
        break;
    }
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g2 < 0) g2 = 0; else if (g2 > 31) g2 = 31;
    if (b < 0) b = 0; else if (b > 31) b = 31;
    return (uint16_t)(r | (g2 << 5) | (b << 10));
}

/* ---- pixel output ------------------------------------------------------------------ */

typedef struct {
    psx_gpu_t *g;
    uint32_t semi;   /* semi-transparency commanded */
    uint32_t blend;  /* texture modulation (else raw) */
    uint32_t dith;   /* dithering enabled */
    uint32_t clut;   /* CLUT attribute */
} raster_ctx;

static void put_pixel(raster_ctx *rc, int32_t x, int32_t y, int32_t r8,
                      int32_t g8, int32_t b8, int has_tex, int32_t u,
                      int32_t v)
{
    psx_gpu_t *g = rc->g;
    if (!in_area(g, x, y))
        return;

    uint32_t semi = rc->semi;
    uint16_t color;

    if (has_tex) {
        uint32_t tx = (g->tpage & 0xFu) * 64u;
        uint32_t ty = ((g->tpage >> 4) & 1u) * 256u + ((g->tpage >> 11) & 1u) * 512u;
        uint32_t depth = (g->tpage >> 7) & 3u;
        uint32_t clut_x = (uint32_t)(rc->clut & 0x3Fu) << 4;
        uint32_t clut_y = ((rc->clut >> 6) & 0x1FFu) |
                          (((rc->clut >> 15) & 1u) << 9);

        /* texture window repeat (spec formula) */
        uint32_t wmask_x = (g->texwin & 0x1Fu) * 8u;
        uint32_t wmask_y = ((g->texwin >> 5) & 0x1Fu) * 8u;
        uint32_t woff_x = ((g->texwin >> 10) & 0x1Fu) * 8u;
        uint32_t woff_y = ((g->texwin >> 15) & 0x1Fu) * 8u;
        uint32_t px = ((uint32_t)u & ~wmask_x) | (woff_x & wmask_x);
        uint32_t py = ((uint32_t)v & ~wmask_y) | (woff_y & wmask_y);

        uint16_t texel;
        if (depth == 0u) { /* 4bpp */
            uint16_t w = vram_get(g, tx + (px >> 2), ty + py);
            uint32_t idx = (w >> ((px & 3u) * 4u)) & 0xFu;
            texel = vram_get(g, clut_x + idx, clut_y);
        } else if (depth == 1u) { /* 8bpp */
            uint16_t w = vram_get(g, tx + (px >> 1), ty + py);
            uint32_t idx = (w >> ((px & 1u) * 8u)) & 0xFFu;
            texel = vram_get(g, clut_x + idx, clut_y);
        } else {
            texel = vram_get(g, tx + px, ty + py);
        }

        if (texel == 0u) {
            /* texel 0000h is fully transparent in all texture modes */
            return;
        }

        int32_t tr = (int32_t)((texel & 0x1Fu) << 3);
        int32_t tg = (int32_t)(((texel >> 5) & 0x1Fu) << 3);
        int32_t tb = (int32_t)(((texel >> 10) & 0x1Fu) << 3);

        int32_t mr, mg, mb;
        if (rc->blend) {
            /* modulation: texture * color / 128 (80h = 1.0x) */
            mr = (tr * r8) >> 7;
            mg = (tg * g8) >> 7;
            mb = (tb * b8) >> 7;
            if (mr > 255) mr = 255;
            if (mg > 255) mg = 255;
            if (mb > 255) mb = 255;
        } else {
            mr = tr;
            mg = tg;
            mb = tb;
        }

        if (rc->dith && rc->blend) {
            int32_t d = gte_dither[y & 3][x & 3];
            color = (uint16_t)(chan5(mr, d) | (chan5(mg, d) << 5) |
                               (chan5(mb, d) << 10));
        } else {
            color = (uint16_t)((uint32_t)(mr >> 3) | ((uint32_t)(mg >> 3) << 5) |
                               ((uint32_t)(mb >> 3) << 10));
        }

        /* semi transparency: commanded, or per-texel bit for CLUT/15bit */
        if (semi || (texel & 0x8000u))
            color = blend((g->tpage >> 5) & 3u,
                          vram_get(g, (uint32_t)x, (uint32_t)y), color);
    } else {
        if (rc->dith) {
            int32_t d = gte_dither[y & 3][x & 3];
            color = (uint16_t)(chan5(r8, d) | (chan5(g8, d) << 5) |
                               (chan5(b8, d) << 10));
        } else {
            color = (uint16_t)((uint32_t)(r8 >> 3) | ((uint32_t)(g8 >> 3) << 5) |
                               ((uint32_t)(b8 >> 3) << 10));
        }
        if (semi)
            color = blend((g->tpage >> 5) & 3u,
                          vram_get(g, (uint32_t)x, (uint32_t)y), color);
    }

    vram_put_masked(g, (uint32_t)x, (uint32_t)y, color);
}

/* ---- polygons ------------------------------------------------------------------------ */

typedef struct {
    int32_t x, y;
    int32_t r, g2, b; /* 8-bit color (gouraud) */
    int32_t u, v;
} gvtx;

static inline int64_t edge_fn(const gvtx *a, const gvtx *b, int32_t px,
                              int32_t py)
{
    return (int64_t)(b->x - a->x) * (py - a->y) -
           (int64_t)(b->y - a->y) * (px - a->x);
}

static void draw_tri(raster_ctx *rc, const gvtx *v0, const gvtx *v1,
                     const gvtx *v2, int textured)
{
    psx_gpu_t *g = rc->g;
    int64_t area = edge_fn(v0, v1, v2->x, v2->y);
    if (area == 0)
        return;

    int32_t minx = v0->x < v1->x ? (v0->x < v2->x ? v0->x : v2->x)
                                 : (v1->x < v2->x ? v1->x : v2->x);
    int32_t maxx = v0->x > v1->x ? (v0->x > v2->x ? v0->x : v2->x)
                                 : (v1->x > v2->x ? v1->x : v2->x);
    int32_t miny = v0->y < v1->y ? (v0->y < v2->y ? v0->y : v2->y)
                                 : (v1->y < v2->y ? v1->y : v2->y);
    int32_t maxy = v0->y > v1->y ? (v0->y > v2->y ? v0->y : v2->y)
                                 : (v1->y > v2->y ? v1->y : v2->y);

    /* spec: primitives wider than 1023 or taller than 511 are skipped */
    if (maxx - minx > 1023 || maxy - miny > 511)
        return;

    if (minx < (int32_t)g->area_x1)
        minx = (int32_t)g->area_x1;
    if (miny < (int32_t)g->area_y1)
        miny = (int32_t)g->area_y1;
    if (maxx > (int32_t)g->area_x2)
        maxx = (int32_t)g->area_x2;
    if (maxy > (int32_t)g->area_y2)
        maxy = (int32_t)g->area_y2;

    for (int32_t y = miny; y <= maxy; y++) {
        for (int32_t x = minx; x <= maxx; x++) {
            int64_t w0 = edge_fn(v1, v2, x, y);
            int64_t w1 = edge_fn(v2, v0, x, y);
            int64_t w2 = edge_fn(v0, v1, x, y);
            int inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                         (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!inside)
                continue;
            int64_t a = area < 0 ? -area : area;
            int64_t s0 = area < 0 ? -w0 : w0;
            int64_t s1 = area < 0 ? -w1 : w1;
            int64_t s2 = area < 0 ? -w2 : w2;

            int32_t r8 = (int32_t)((s0 * v0->r + s1 * v1->r + s2 * v2->r) / a);
            int32_t g8 = (int32_t)((s0 * v0->g2 + s1 * v1->g2 + s2 * v2->g2) / a);
            int32_t b8 = (int32_t)((s0 * v0->b + s1 * v1->b + s2 * v2->b) / a);
            int32_t u = 0, v = 0;
            if (textured) {
                u = (int32_t)((s0 * v0->u + s1 * v1->u + s2 * v2->u) / a);
                v = (int32_t)((s0 * v0->v + s1 * v1->v + s2 * v2->v) / a);
            }
            put_pixel(rc, x, y, r8, g8, b8, textured, u, v);
        }
    }
}

/* ---- command processing ----------------------------------------------------------------- */

static inline int32_t vcoord(uint32_t raw, int32_t off)
{
    return (int32_t)(int16_t)(raw & 0xFFFFu) + off;
}

static inline gvtx mkv(uint32_t coord, uint32_t colorw, uint32_t texw,
                       int32_t off_x, int32_t off_y)
{
    gvtx v;
    v.x = (int32_t)(int16_t)(coord & 0xFFFFu) + off_x;
    v.y = (int32_t)(int16_t)(coord >> 16) + off_y;
    v.r = (int32_t)(colorw & 0xFFu);
    v.g2 = (int32_t)((colorw >> 8) & 0xFFu);
    v.b = (int32_t)((colorw >> 16) & 0xFFu);
    v.u = (int16_t)(texw & 0xFFu);
    v.v = (int16_t)((texw >> 8) & 0xFFu);
    return v;
}

static void draw_line(raster_ctx *rc, const gvtx *a, const gvtx *b)
{
    int32_t dx = b->x - a->x;
    int32_t dy = b->y - a->y;
    int32_t steps = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    if (ady > steps)
        steps = ady;
    if (steps == 0) {
        put_pixel(rc, a->x, a->y, a->r, a->g2, a->b, 0, 0, 0);
        return;
    }
    for (int32_t i = 0; i <= steps; i++) {
        int32_t t = (i * 65536) / steps;
        int32_t x = a->x + (int32_t)(((int64_t)dx * i) / steps);
        int32_t y = a->y + (int32_t)(((int64_t)dy * i) / steps);
        int32_t r = a->r + (((b->r - a->r) * t) >> 16);
        int32_t g8 = a->g2 + (((b->g2 - a->g2) * t) >> 16);
        int32_t b8 = a->b + (((b->b - a->b) * t) >> 16);
        put_pixel(rc, x, y, r, g8, b8, 0, 0, 0);
    }
}

static void draw_rect(psx_gpu_t *g, raster_ctx *rc, uint32_t first,
                      uint32_t *pr)
{
    uint32_t cmd = first >> 24;
    int32_t x = vcoord(pr[0], g->off_x);
    int32_t y = vcoord(pr[0] >> 16, g->off_y);
    int32_t w, h;
    int textured = 0;
    uint32_t clut = 0;
    int32_t u = 0, v = 0;
    int xflip = (int)((g->tpage >> 12) & 1u);
    int yflip = (int)((g->tpage >> 13) & 1u);

    if (cmd < 0x68u) { /* variable size */
        w = (int32_t)(pr[1] & 0x3FFu);
        h = (int32_t)((pr[1] >> 16) & 0x1FFu);
    } else if (cmd < 0x70u) { /* 1x1 */
        w = 1;
        h = 1;
    } else if (cmd < 0x78u) { /* 8x8 */
        w = 8;
        h = 8;
    } else { /* 16x16 */
        w = 16;
        h = 16;
    }

    if ((cmd & 4u) != 0u) {
        /* textured: bit2 of the command selects the textured form within
         * each size group (60/68/70/78 mono; 64/6C/74/7C textured) */
        textured = 1;
        uint32_t texword = (cmd < 0x68u) ? pr[2] : pr[1];
        clut = texword >> 16;
        u = (int16_t)(texword & 0xFFu);
        v = (int16_t)((texword >> 8) & 0xFFu);
    }

    int32_t r8 = (int32_t)(first & 0xFFu);
    int32_t g8 = (int32_t)((first >> 8) & 0xFFu);
    int32_t b8 = (int32_t)((first >> 16) & 0xFFu);
    rc->clut = clut;

    for (int32_t yy = 0; yy < h; yy++) {
        for (int32_t xx = 0; xx < w; xx++) {
            int32_t tu = u, tv = v;
            if (textured) {
                tu = xflip ? (u + w - 1 - xx) : (u + xx);
                tv = yflip ? (v + h - 1 - yy) : (v + yy);
            }
            put_pixel(rc, x + xx, y + yy, r8, g8, b8, textured, tu, tv);
        }
    }
}

static void gp0_render(psx_gpu_t *g, uint32_t first, uint32_t *pr, int count)
{
    uint32_t cmd = first >> 24;
    uint32_t color = first & 0x00FFFFFFu;
    raster_ctx rc;
    rc.g = g;
    rc.semi = cmd & 2u;
    rc.clut = 0;
    int raw = (int)(cmd & 1u);
    uint32_t dith_bit = (g->tpage >> 9) & 1u;

    if (cmd >= 0x60u) {
        /* rectangles: never dithered */
        rc.blend = (uint32_t)(raw ? 0 : 1);
        rc.dith = 0;
        draw_rect(g, &rc, first, pr);
        return;
    }

    int textured = (cmd >= 0x24u && cmd <= 0x27u) ||
                   (cmd >= 0x2Cu && cmd <= 0x2Fu) ||
                   (cmd >= 0x34u && cmd <= 0x36u) ||
                   (cmd >= 0x3Cu && cmd <= 0x3Eu);
    rc.blend = (uint32_t)(raw ? 0 : 1);
    /* dithering applies to gouraud-shaded and texture-blended polygons,
     * and to all lines; never to monochrome polygons or rectangles. */
    int mono = (cmd < 0x24u) || (cmd >= 0x28u && cmd <= 0x2Au);
    rc.dith = (uint32_t)(mono ? 0 : (int)dith_bit);

    if (cmd <= 0x2Fu) { /* flat / textured triangles */
        if (!textured) {
            gvtx v1 = mkv(pr[0], color, 0, g->off_x, g->off_y);
            gvtx v2 = mkv(pr[1], color, 0, g->off_x, g->off_y);
            gvtx v3 = mkv(pr[2], color, 0, g->off_x, g->off_y);
            draw_tri(&rc, &v1, &v2, &v3, 0);
            if (cmd >= 0x28u) { /* quad */
                gvtx v4 = mkv(pr[3], color, 0, g->off_x, g->off_y);
                draw_tri(&rc, &v1, &v3, &v4, 0);
            }
        } else {
            uint32_t clut = pr[1] >> 16;
            uint32_t tpage = pr[3] >> 16;
            uint32_t save_tpage = g->tpage;
            g->tpage = (tpage & 0x9FFu) | (save_tpage & 0x600u);
            rc.clut = clut;
            gvtx v1 = mkv(pr[0], color, pr[1], g->off_x, g->off_y);
            gvtx v2 = mkv(pr[2], color, pr[3], g->off_x, g->off_y);
            gvtx v3 = mkv(pr[4], color, pr[5], g->off_x, g->off_y);
            draw_tri(&rc, &v1, &v2, &v3, 1);
            if (cmd >= 0x2Cu) { /* quad */
                gvtx v4 = mkv(pr[6], color, pr[7], g->off_x, g->off_y);
                draw_tri(&rc, &v1, &v3, &v4, 1);
            }
            g->tpage = save_tpage;
        }
        return;
    }

    if (cmd <= 0x3Fu) { /* gouraud shaded (optionally textured) triangles */
        if (cmd < 0x34u || (cmd >= 0x38u && cmd <= 0x3Au)) {
            gvtx v1 = mkv(pr[0], color, 0, g->off_x, g->off_y);
            gvtx v2 = mkv(pr[2], pr[1], 0, g->off_x, g->off_y);
            gvtx v3 = mkv(pr[4], pr[3], 0, g->off_x, g->off_y);
            draw_tri(&rc, &v1, &v2, &v3, 0);
            if (cmd >= 0x38u) {
                gvtx v4 = mkv(pr[6], pr[5], 0, g->off_x, g->off_y);
                draw_tri(&rc, &v1, &v3, &v4, 0);
            }
        } else {
            uint32_t clut = pr[1] >> 16;
            uint32_t tpage = pr[4] >> 16;
            uint32_t save_tpage = g->tpage;
            g->tpage = (tpage & 0x9FFu) | (save_tpage & 0x600u);
            rc.clut = clut;
            gvtx v1 = mkv(pr[0], color, pr[1], g->off_x, g->off_y);
            gvtx v2 = mkv(pr[3], pr[2], pr[4], g->off_x, g->off_y);
            gvtx v3 = mkv(pr[6], pr[5], pr[7], g->off_x, g->off_y);
            draw_tri(&rc, &v1, &v2, &v3, 1);
            if (cmd >= 0x3Cu) {
                gvtx v4 = mkv(pr[9], pr[8], pr[10], g->off_x, g->off_y);
                draw_tri(&rc, &v1, &v3, &v4, 1);
            }
            g->tpage = save_tpage;
        }
        return;
    }

    if (cmd <= 0x52u) { /* single lines: 40/42 mono, 50/52 shaded */
        if (cmd < 0x50u) {
            gvtx a = mkv(pr[0], color, 0, g->off_x, g->off_y);
            gvtx b = mkv(pr[1], color, 0, g->off_x, g->off_y);
            draw_line(&rc, &a, &b);
        } else {
            gvtx a = mkv(pr[0], color, 0, g->off_x, g->off_y);
            gvtx b = mkv(pr[2], pr[1], 0, g->off_x, g->off_y);
            draw_line(&rc, &a, &b);
        }
        return;
    }
    (void)count;
}

/* Poly-line variants need the parameter count; dispatched with terminator
 * stripped. 48/4A: c v1 v2 ... vn. 58/5A: c1 v1 c2 v2 ... cn vn. */
static void gp0_polyline(psx_gpu_t *g, uint32_t first, uint32_t *pr,
                         int count)
{
    uint32_t cmd = first >> 24;
    raster_ctx rc;
    rc.g = g;
    rc.semi = cmd & 2u;
    rc.blend = 1;
    rc.dith = (g->tpage >> 9) & 1u; /* lines are always dithered if enabled */
    rc.clut = 0;

    if (cmd <= 0x4Au) {
        uint32_t color = first & 0x00FFFFFFu;
        if (count < 3)
            return;
        gvtx prev = mkv(pr[1], color, 0, g->off_x, g->off_y);
        for (int i = 2; i < count; i++) {
            gvtx cur = mkv(pr[i], color, 0, g->off_x, g->off_y);
            draw_line(&rc, &prev, &cur);
            prev = cur;
        }
    } else {
        if (count < 4)
            return;
        gvtx prev = mkv(pr[1], pr[0], 0, g->off_x, g->off_y);
        for (int i = 2; i + 1 < count; i += 2) {
            gvtx cur = mkv(pr[i + 1], pr[i], 0, g->off_x, g->off_y);
            draw_line(&rc, &prev, &cur);
            prev = cur;
        }
    }
}

/* ---- GP0 dispatch -------------------------------------------------------------------- */

static void gp0_command(psx_gpu_t *g, struct psx *p, uint32_t first,
                        uint32_t *params, int count);

/* Execute a fully received GP0 command (`first` = raw command word with
 * inline color; params valid for count entries). */
static void exec_gp0(psx_gpu_t *g, struct psx *p, uint32_t first, uint32_t *pr,
                     int count)
{
    (void)p;
    uint32_t cmd = first >> 24;
    switch (cmd) {
    case 0x00: /* nop */
    case 0x01: /* clear cache */
    case 0x03: /* nop (ntsc/PAL?) */
    case 0x05:
    case 0x06:
    case 0x07:
        break;
    case 0x02: { /* fill rectangle: color carried in the command word */
        uint16_t c = rgb24_to_15(first & 0x00FFFFFFu);
        int32_t x = (int32_t)(pr[0] & 0x3F0u);
        int32_t y = (int32_t)((pr[0] >> 16) & 0x1FFu);
        int32_t w = (int32_t)(((pr[1] & 0x3FFu) + 0xFu) & ~0xFu);
        int32_t h = (int32_t)((pr[1] >> 16) & 0x1FFu);
        for (int32_t yy = y; yy < y + h; yy++)
            for (int32_t xx = x; xx < x + w; xx++)
                vram_put(g, (uint32_t)xx, (uint32_t)yy, c);
        break;
    }
    case 0x1F: /* IRQ request */
        g->gpustat |= (1u << 24);
        break;

    case 0xE1: /* draw mode */
        g->tpage = pr[0] & 0x9FFu; /* bits 0-8 and 11 */
        break;
    case 0xE2:
        g->texwin = pr[0] & 0xFFFFFu;
        break;
    case 0xE3:
        g->area_x1 = pr[0] & 0x3FFu;
        g->area_y1 = (pr[0] >> 10) & 0x1FFu;
        break;
    case 0xE4:
        g->area_x2 = pr[0] & 0x3FFu;
        g->area_y2 = (pr[0] >> 10) & 0x1FFu;
        break;
    case 0xE5:
        g->off_x = (int32_t)(pr[0] & 0x7FFu) - 1024;
        g->off_y = (int32_t)((pr[0] >> 11) & 0x7FFu) - 1024;
        break;
    case 0xE6:
        g->mask_set = pr[0] & 1u;
        g->mask_check = (pr[0] >> 1) & 1u;
        break;

    case 0x80: { /* VRAM -> VRAM copy */
        int32_t sx = (int32_t)(pr[0] & 0x3FFu);
        int32_t sy = (int32_t)((pr[0] >> 16) & 0x1FFu);
        int32_t dx = (int32_t)(pr[1] & 0x3FFu);
        int32_t dy = (int32_t)((pr[1] >> 16) & 0x1FFu);
        int32_t w = (int32_t)(pr[2] & 0x3FFu);
        int32_t h = (int32_t)((pr[2] >> 16) & 0x1FFu);
        if (w == 0 || h == 0)
            break;
        uint16_t *tmp = malloc(sizeof(uint16_t) * (uint32_t)(w * h));
        if (tmp != NULL) {
            for (int32_t yy = 0; yy < h; yy++)
                for (int32_t xx = 0; xx < w; xx++)
                    tmp[yy * w + xx] = vram_get(g, (uint32_t)(sx + xx),
                                                (uint32_t)(sy + yy));
            for (int32_t yy = 0; yy < h; yy++)
                for (int32_t xx = 0; xx < w; xx++)
                    vram_put_masked(g, (uint32_t)(dx + xx),
                                    (uint32_t)(dy + yy), tmp[yy * w + xx]);
            free(tmp);
        }
        break;
    }
    case 0xC0: { /* VRAM -> CPU */
        g->read_x = (int32_t)(pr[0] & 0x3FFu);
        g->read_y = (int32_t)((pr[0] >> 16) & 0x1FFu);
        g->read_w = (int32_t)(pr[1] & 0x3FFu);
        g->read_h = (int32_t)((pr[1] >> 16) & 0x1FFu);
        g->read_px = g->read_x;
        g->read_py = g->read_y;
        g->read_pending = (g->read_w > 0 && g->read_h > 0);
        break;
    }

    default:
        if (cmd >= 0x20u && cmd <= 0x7Fu) {
            if (cmd == 0x48u || cmd == 0x4Au || cmd == 0x58u || cmd == 0x5Au)
                gp0_polyline(g, first, pr, count);
            else
                gp0_render(g, first, pr, count);
        }
        break;
    }
}

/* Parameter-count table for GP0 command words (0 = handled inline). */
/* Parameter words following the command word (color rides in the command
 * word for all render commands except shaded variants' extra colors). */
static int gp0_param_count(uint32_t cmd)
{
    switch (cmd) {
    case 0x02: return 2;  /* fill: coord, size */
    case 0x1F: return 0;
    case 0x20: case 0x22: return 3; /* mono tri */
    case 0x28: case 0x2A: return 4; /* mono quad */
    case 0x24: case 0x25: case 0x26: case 0x27: return 6; /* tex tri */
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: return 8; /* tex quad */
    case 0x30: case 0x32: return 5; /* shaded tri */
    case 0x38: case 0x3A: return 7; /* shaded quad */
    case 0x34: case 0x36: return 8; /* shaded tex tri */
    case 0x3C: case 0x3E: return 11; /* shaded tex quad */
    case 0x40: case 0x42: return 2; /* mono line */
    case 0x50: case 0x52: return 3; /* shaded line */
    case 0x48: case 0x4A: case 0x58: case 0x5A: return -1; /* poly-line */
    case 0x60: case 0x62: return 2; /* mono rect variable */
    case 0x68: case 0x6A: return 1; /* mono dot */
    case 0x70: case 0x72: return 1; /* mono 8x8 */
    case 0x78: case 0x7A: return 1; /* mono 16x16 */
    case 0x64: case 0x65: case 0x66: case 0x67: return 3; /* tex rect var */
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: return 2; /* tex dot */
    case 0x74: case 0x75: case 0x76: case 0x77: return 2; /* tex 8x8 */
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: return 2; /* tex 16x16 */
    case 0x80: return 3; /* VRAM->VRAM */
    case 0xC0: return 2; /* VRAM->CPU: src coord, size */
    case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6:
        return 1;
    case 0x00: case 0x01: case 0x03: case 0x05: case 0x06: case 0x07:
        return 0;
    default:
        return 0; /* unknown/mirror commands ignored */
    }
}

static void gp0_command(psx_gpu_t *g, struct psx *p, uint32_t first,
                        uint32_t *params, int count)
{
    exec_gp0(g, p, first, params, count);
}

void psx_gpu_write_gp0(psx_gpu_t *g, struct psx *p, uint32_t v)
{
    /* CPU->VRAM streaming data (A0h in progress): consume as data words. */
    if (g->xfer_active) {
        for (int k = 0; k < 2; k++) {
            if (g->xfer_left == 0)
                break;
            uint16_t half = (uint16_t)(v >> (16u * (uint32_t)k));
            vram_put_masked(g, (uint32_t)g->xfer_x, (uint32_t)g->xfer_y, half);
            g->xfer_x++;
            if (g->xfer_x >= g->xfer_x0 + g->xfer_w) {
                g->xfer_x = g->xfer_x0;
                g->xfer_y++;
            }
            g->xfer_left--;
        }
        if (g->xfer_left == 0)
            g->xfer_active = 0;
        return;
    }

    if (!g->fifo_active) {
        uint32_t cmd = v >> 24;
        if (cmd == 0xA0u) {
            g->fifo[0] = v;
            g->fifo_len = 1;
            g->fifo_need = 2;
            g->fifo_active = 1;
            return;
        }
        int n = gp0_param_count(cmd);
        if (n == 0) {
            uint32_t zero = 0;
            gp0_command(g, p, v, &zero, 0);
            return;
        }
        g->fifo[0] = v;
        g->fifo_len = 1;
        g->fifo_need = (uint8_t)(n < 0 ? 0xFF : n);
        g->fifo_active = 1;
        return;
    }

    g->fifo[g->fifo_len++] = v;

    if (g->fifo_need == 0xFFu) {
        /* poly-line terminator (55555555h, Wild Arms 2 uses 50005000h) */
        if ((v & 0xF000F000u) == 0x50005000u || g->fifo_len >= 63u) {
            uint32_t params[64];
            int count = g->fifo_len - 2; /* drop terminator */
            if (count > 62)
                count = 62;
            for (int i = 0; i < count; i++)
                params[i] = g->fifo[i + 1];
            gp0_polyline(g, g->fifo[0] >> 24, params, count);
            g->fifo_len = 0;
            g->fifo_need = 0;
            g->fifo_active = 0;
        }
        return;
    }

    /* A0h: coordinate + size, then switch to streaming. */
    if ((g->fifo[0] >> 24) == 0xA0u && g->fifo_len == 3) {
        int32_t dx = (int32_t)(g->fifo[1] & 0x3FFu);
        int32_t dy = (int32_t)((g->fifo[1] >> 16) & 0x1FFu);
        int32_t w = (int32_t)(g->fifo[2] & 0x3FFu);
        int32_t h = (int32_t)((g->fifo[2] >> 16) & 0x1FFu);
        g->fifo_len = 0;
        g->fifo_need = 0;
        g->fifo_active = 0;
        if (w > 0 && h > 0) {
            g->xfer_active = 1;
            g->xfer_left = (uint32_t)w * (uint32_t)h;
            g->xfer_x = dx;
            g->xfer_y = dy;
            g->xfer_x0 = dx;
            g->xfer_w = w;
            g->xfer_h = h;
        }
        return;
    }

    if (g->fifo_len >= g->fifo_need + 1u) {
        uint32_t params[64];
        int count = g->fifo_len - 1;
        for (int i = 0; i < count; i++)
            params[i] = g->fifo[i + 1];
        uint32_t first = g->fifo[0];
        g->fifo_len = 0;
        g->fifo_need = 0;
        g->fifo_active = 0;
        gp0_command(g, p, first, params, count);
    }
}

void psx_gpu_write_gp1(psx_gpu_t *g, struct psx *p, uint32_t v)
{
    (void)p;
    uint32_t cmd = v >> 24;
    switch (cmd) {
    case 0x00: /* reset GPU */
        psx_gpu_reset(g);
        break;
    case 0x01: /* reset command buffer */
        g->fifo_len = 0;
        g->fifo_need = 0;
        g->fifo_active = 0;
        g->xfer_active = 0;
        break;
    case 0x02: /* acknowledge IRQ1 */
        g->gpustat &= ~(1u << 24);
        break;
    case 0x03:
        g->disp_enabled = (v & 1u) ? 0u : 1u; /* bit0: 1 = display off */
        break;
    case 0x04:
        g->gpustat = (g->gpustat & ~(3u << 29)) | ((v & 3u) << 29);
        break;
    case 0x05:
        g->disp_x = v & 0x3FEu; /* bit0 ignored per spec */
        g->disp_y = (v >> 10) & 0x1FFu;
        break;
    case 0x06: {
        uint32_t x1 = v & 0xFFFu;
        uint32_t x2 = (v >> 12) & 0xFFFu;
        uint32_t w = (x2 - x1) & 0xFFFu;
        g->disp_w = w > 1024u ? 1024u : w;
        break;
    }
    case 0x07: {
        uint32_t y1 = v & 0x3FFu;
        uint32_t y2 = (v >> 10) & 0x3FFu;
        uint32_t h = (y2 - y1) & 0x3FFu;
        g->disp_h = h > 512u ? 512u : h;
        break;
    }
    case 0x08:
        g->gpustat = (g->gpustat & ~((1u << 21) | (1u << 22))) |
                     (((v >> 4) & 1u) << 21) | (((v >> 5) & 1u) << 22);
        g->disp_depth24 = (v >> 4) & 1u;
        break;
    default:
        break; /* GP1(09h)+ debug commands: ignored */
    }
}

uint32_t psx_gpu_status(const psx_gpu_t *g)
{
    uint32_t s = g->gpustat;
    s |= (g->tpage & 0x9FFu); /* bits 0-8, 15 */
    s |= ((g->mask_set ? 1u : 0u) << 11);
    s |= ((g->mask_check ? 1u : 0u) << 12);
    s |= (1u << 26); /* ready to receive commands */
    s |= (1u << 28); /* command FIFO empty */
    s &= ~(1u << 25);
    if (g->read_pending)
        s |= (1u << 27);
    else
        s &= ~(1u << 27);
    return s;
}

uint32_t psx_gpu_read(psx_gpu_t *g)
{
    if (!g->read_pending)
        return g->gread;
    uint16_t h0 = vram_get(g, (uint32_t)g->read_px, (uint32_t)g->read_py);
    uint32_t word = h0;
    g->read_px++;
    if (g->read_px >= g->read_x + g->read_w) {
        g->read_px = g->read_x;
        g->read_py++;
        if (g->read_py >= g->read_y + g->read_h) {
            g->read_pending = 0;
            return word;
        }
    }
    uint16_t h1 = vram_get(g, (uint32_t)g->read_px, (uint32_t)g->read_py);
    word |= (uint32_t)h1 << 16;
    g->read_px++;
    if (g->read_px >= g->read_x + g->read_w) {
        g->read_px = g->read_x;
        g->read_py++;
        if (g->read_py >= g->read_y + g->read_h)
            g->read_pending = 0;
    }
    return word;
}

/* ---- display output ----------------------------------------------------------------------- */

void psx_gpu_render(const psx_gpu_t *g, uint32_t *fb)
{
    if (!g->disp_enabled) {
        memset(fb, 0, PSX_FB_W * PSX_FB_H * sizeof(uint32_t));
        return;
    }
    for (uint32_t y = 0; y < PSX_FB_H; y++) {
        for (uint32_t x = 0; x < PSX_FB_W; x++) {
            uint32_t sx = g->disp_x + (x * g->disp_w) / PSX_FB_W;
            uint32_t sy = g->disp_y + (y * g->disp_h) / PSX_FB_H;
            uint32_t *out = &fb[y * PSX_FB_W + x];
            if (g->disp_depth24) {
                uint32_t base = sx * 3u / 2u;
                uint32_t sh = (sx % 2u) * 8u;
                uint16_t w0 = vram_get(g, base, sy);
                uint16_t w1 = vram_get(g, base + 1u, sy);
                uint32_t v24 = ((uint32_t)w0 | ((uint32_t)w1 << 16)) >> sh;
                *out = 0xFF000000u | (v24 & 0xFFFFFFu);
            } else {
                *out = rgb15_to_24((uint16_t)(vram_get(g, sx, sy) & 0x7FFFu));
            }
        }
    }
}
