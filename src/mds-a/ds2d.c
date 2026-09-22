/*
 * mds-a: Nintendo DS 2D engines (engine A / engine B).
 *
 * Per-engine composition, derived from the DS hardware reference:
 *  - BG layers 0-3 with per-BG type from the DISPCNT mode:
 *      mode 0: text   text   text   text
 *      mode 1: text   text   affine affine
 *      mode 2: affine affine affine affine
 *      mode 3: text   text   affine extended
 *      mode 4: identical to mode 3 (documented; DS keeps mode 4 == mode 3)
 *      mode 5: text   text   ext    ext
 *      mode 6/7: unsupported (3D engine not modeled) -> layers disabled
 *  - Text BG: 4/8bpp 8x8 tiles, char base = BGxCNT bits 2-3 (16 KiB
 *    blocks), screen base = bits 8-12 (2 KiB blocks), sizes 256x256 /
 *    512x256 / 256x512 / 512x512, h/v flips, scroll registers; 512-wide
 *    and 512-tall maps use consecutive screen blocks (s+1, s+2/s+3).
 *  - Affine BG: 8bpp tiles only, char base = bits 2-7, map 16/32/64/128
 *    tiles per side, optional wraparound (bit 13), transform
 *      tex = PA*(sx-128) + PB*(sy-128) + ref  (in .8 fixed point, with the
 *    +0.5 rounding offset and screen center (128,128), the same documented
 *    convention as the mgbax GBA core).
 *  - Extended affine BG: BGxCNT bits 2-6 format:
 *      0: 16-color tiles   1: 256-color tiles (affine-mapped)
 *      2: 16bpp 128x128    3: 16bpp 256x256    4: 16bpp 512x256
 *      5: 16bpp 256x512    6: 256-color bitmap 256x256
 *    screen base = bits 8-12 (16 KiB blocks), wrap = bit 13 (bitmaps),
 *    sizes = bits 14-15 (tiled formats only). Tile data for the tiled
 *    formats is addressed from the start of the BG region (char base 0;
 *    documented approximation - the hardware reference does not define a
 *    separate char-base field for extended BGs). Direct-color pixel
 *    0x0000 is transparent; bit 15 is the alpha flag and is ignored
 *    (blending is not implemented).
 *  - OBJ: 128 sprites per engine from OAM (attr0/1/2 + pad), 1D/2D tile
 *    mapping (DISPCNT bit 14), h/v flips (non-rotated), 4/8bpp, shape/size
 *    table, X wraps in a 512-wide space, Y wraps at 256, priorities vs BGs
 *    (sprites win ties against same-priority BGs; lower OAM index wins
 *    between sprites). Rotation/scaling uses the 32 OAM parameter sets at
 *    OAM+0x300+set*8 (PA,PB,PC,PD; aliases sprite entries 96-127, as on
 *    hardware); X/Y remain the top-left of the non-enlarged box, the
 *    double-size box extends half a size in each direction. OBJ modes:
 *    0 normal, 1 semi-transparent (rendered opaque; blending is not
 *    implemented), 2 OBJ window (skipped: windows are not modeled),
 *    3 forbidden (skipped).
 *  - Composition: for each pixel the first opaque layer wins, scanning
 *    priorities 0..3; within one priority OBJs first, then BG0..BG3.
 *    Empty pixels show the backdrop color (BG palette entry 0).
 *  - Master brightness (DISPCNT+0x6C): mode 1 brightness down, mode 2 up,
 *    factor bits 0-5 (0-63), applied to the 8-bit output components
 *    (documented model).
 *
 * Memory windows (fixed linear placement, VRAMCNT ignored - documented):
 *  - engine A BG: 0x06000000 (VRAM A) then 0x06200000 (VRAM B)
 *  - engine B BG: 0x06200000 (VRAM B)
 *  - engine A OBJ: 0x06400000 (VRAM C), 32 KiB window (hardware size)
 *  - engine B OBJ: 0x06600000 (VRAM D), 16 KiB window (hardware size)
 *  - palettes/OAM as in ds.h (0x06880000 block / 0x07000000 blocks)
 */
#include "ds.h"

#include "../common/util.h"

/* ---- register access ------------------------------------------------------ */

#define ENG_IO(engine) ((engine) == 0u ? 0x0000u : 0x1000u)

static uint16_t io_rd16(struct ds *d, unsigned engine, uint32_t off)
{
    return emu_le16(&d->a9.io[ENG_IO(engine) + off]);
}

static uint32_t io_rd32(struct ds *d, unsigned engine, uint32_t off)
{
    return emu_le32(&d->a9.io[ENG_IO(engine) + off]);
}

/* BGxCNT offsets: BG0 0x08, BG1 0x0A, BG2 0x0C, BG3 0x0E. */
static uint16_t bg_cnt(struct ds *d, unsigned engine, unsigned bg)
{
    return io_rd16(d, engine, 0x08u + bg * 2u);
}

/* ---- memory windows -------------------------------------------------------- */

/* Engine A BG region: 0x06000000 (vram_a) then 0x06200000 (vram_b). */
static uint8_t bg_a_mem(const struct ds *d, uint32_t off)
{
    if (off < DS_VRAM_A_SIZE)
        return d->vram_a[off];
    if (off >= 0x200000u && off < 0x200000u + DS_VRAM_B_SIZE)
        return d->vram_b[off - 0x200000u];
    return 0; /* unmapped gap reads zero */
}

/* Engine B BG region: 0x06200000 (vram_b). */
static uint8_t bg_b_mem(const struct ds *d, uint32_t off)
{
    if (off < DS_VRAM_B_SIZE)
        return d->vram_b[off];
    return 0;
}

/* OBJ tile memory: engine A 32 KiB window, engine B 16 KiB (hardware sizes). */
static uint8_t obj_mem(const struct ds *d, unsigned engine, uint32_t off)
{
    if (engine == 0u) {
        off &= 0x7FFFu;
        return off < DS_VRAM_C_SIZE ? d->vram_c[off] : 0;
    }
    off &= 0x3FFFu;
    return off < DS_VRAM_D_SIZE ? d->vram_d[off] : 0;
}

static uint16_t pal16(const struct ds *d, unsigned engine, unsigned obj,
                      uint32_t idx)
{
    uint32_t base = (engine == 0u ? 0x000u : 0x400u) + (obj ? 0x200u : 0u);
    idx = (idx & 0xFFu) * 2u; /* palette entry index -> byte offset */
    return (uint16_t)(d->pal[base + idx] |
                      ((uint16_t)d->pal[base + idx + 1u] << 8));
}

static uint16_t oam_rd16(const struct ds *d, unsigned engine, uint32_t off)
{
    const uint8_t *oam = engine == 0u ? d->oam_a : d->oam_b;
    off &= 0x3FFu;
    return (uint16_t)(oam[off] | ((uint16_t)oam[off + 1u] << 8));
}

/* ---- color ------------------------------------------------------------------- */

static uint32_t bgr555_to_px(uint16_t c)
{
    uint32_t r = (uint32_t)(c & 31u), g = (uint32_t)((c >> 5) & 31u),
             b = (uint32_t)((c >> 10) & 31u);
    return EMU_PIXEL((r << 3) | (r >> 2), (g << 3) | (g >> 2),
                     (b << 3) | (b >> 2));
}

/* ---- BG layer types ------------------------------------------------------------ */

enum bg_type { BG_NONE = 0, BG_TEXT, BG_AFFINE, BG_EXT };

static enum bg_type bg_layer_type(uint32_t mode, unsigned bg)
{
    static const enum bg_type tab[6][4] = {
        { BG_TEXT, BG_TEXT, BG_TEXT, BG_TEXT },         /* mode 0 */
        { BG_TEXT, BG_TEXT, BG_AFFINE, BG_AFFINE },     /* mode 1 */
        { BG_AFFINE, BG_AFFINE, BG_AFFINE, BG_AFFINE }, /* mode 2 */
        { BG_TEXT, BG_TEXT, BG_AFFINE, BG_EXT },        /* mode 3 */
        { BG_TEXT, BG_TEXT, BG_AFFINE, BG_EXT },        /* mode 4 == mode 3 */
        { BG_TEXT, BG_TEXT, BG_EXT, BG_EXT },           /* mode 5 */
    };
    if (mode >= 6u)
        return BG_NONE; /* modes 6/7 need the 3D engine: not modeled */
    return tab[mode][bg];
}

static uint8_t bg_region_rd(const struct ds *d, unsigned engine, uint32_t off)
{
    return engine == 0u ? bg_a_mem(d, off) : bg_b_mem(d, off);
}

static uint16_t bg_region_rd16(const struct ds *d, unsigned engine,
                               uint32_t off)
{
    return (uint16_t)(bg_region_rd(d, engine, off) |
                      ((uint16_t)bg_region_rd(d, engine, off + 1u) << 8));
}

/* ---- text BG --------------------------------------------------------------------- */

static int text_bg_pixel(struct ds *d, unsigned engine, unsigned bg,
                         uint32_t sx, uint32_t sy, uint16_t *out)
{
    uint16_t cnt = bg_cnt(d, engine, bg);
    uint32_t hofs = io_rd16(d, engine, 0x10u + bg * 4u);
    uint32_t vofs = io_rd16(d, engine, 0x12u + bg * 4u);
    uint32_t bpp8 = (cnt >> 7) & 1u;
    uint32_t screen_base = (cnt >> 8) & 0x1Fu;
    uint32_t size = (cnt >> 14) & 3u;
    uint32_t px_w = (size & 1u) ? 512u : 256u;
    uint32_t px_h = (size & 2u) ? 512u : 256u;
    uint32_t ax = hofs + sx;
    uint32_t ay = vofs + sy;
    uint32_t tx = (ax & (px_w - 1u)) >> 3;
    uint32_t ty = (ay & (px_h - 1u)) >> 3;
    uint32_t fx = ax & 7u;
    uint32_t fy = ay & 7u;

    uint32_t block = screen_base;
    if (px_w == 512u && tx >= 32u) {
        block += 1u;
        tx -= 32u;
    }
    if (px_h == 512u && ty >= 32u) {
        block += 2u;
        ty -= 32u;
    }

    uint16_t entry = bg_region_rd16(d, engine,
                                    block * 2048u + ty * 64u + tx * 2u);
    uint32_t tile = entry & 0x3FFu;
    uint32_t hflip = (entry >> 10) & 1u;
    uint32_t vflip = (entry >> 11) & 1u;
    if (hflip)
        fx ^= 7u;
    if (vflip)
        fy ^= 7u;

    uint32_t char_base = ((cnt >> 2) & 3u) * 0x4000u; /* bits 2-3 */
    uint32_t tile_off = char_base + tile * (bpp8 ? 64u : 32u) +
                        fy * (bpp8 ? 8u : 4u) + (bpp8 ? fx : (fx >> 1));
    uint8_t byte = bg_region_rd(d, engine, tile_off);
    uint32_t idx = bpp8 ? byte : ((fx & 1u) ? (byte >> 4) : (byte & 0xFu));
    if (idx == 0u)
        return 0; /* transparent */
    if (bpp8) {
        *out = pal16(d, engine, 0, idx);
    } else {
        *out = pal16(d, engine, 0, ((entry >> 12) & 0xFu) * 16u + idx);
    }
    return 1;
}

/* ---- affine transform (shared by affine and extended BGs) --------------------------- */

static void affine_params(struct ds *d, unsigned engine, unsigned bg,
                          int16_t *pa, int16_t *pb, int16_t *pc, int16_t *pd,
                          uint32_t *refx, uint32_t *refy)
{
    /* BG2 params at +0x20, BG3 params at +0x30; X/Y refs are 32-bit. */
    uint32_t base = bg == 2u ? 0x20u : 0x30u;
    *pa = (int16_t)io_rd16(d, engine, base + 0u);
    *pb = (int16_t)io_rd16(d, engine, base + 2u);
    *pc = (int16_t)io_rd16(d, engine, base + 4u);
    *pd = (int16_t)io_rd16(d, engine, base + 6u);
    *refx = io_rd32(d, engine, base + 8u);
    *refy = io_rd32(d, engine, base + 12u);
}

static void affine_sample(int16_t pa, int16_t pb, int16_t pc, int16_t pd,
                          uint32_t refx, uint32_t refy, uint32_t sx,
                          uint32_t sy, int32_t *tx, int32_t *ty)
{
    /* .8 fixed point with the +0.5 rounding offset and screen center
     * (128,128); refx/refy are the 28.8 reference registers (two's
     * complement). Signed right shifts match the mgbax core convention. */
    int64_t fx = (int64_t)pa * ((int32_t)sx - 128) +
                 (int64_t)pb * ((int32_t)sy - 128) +
                 (int64_t)(int32_t)refx + 128;
    int64_t fy = (int64_t)pc * ((int32_t)sx - 128) +
                 (int64_t)pd * ((int32_t)sy - 128) +
                 (int64_t)(int32_t)refy + 128;
    *tx = (int32_t)(fx >> 8);
    *ty = (int32_t)(fy >> 8);
}

/* ---- affine BG (8bpp tiles) ----------------------------------------------------------- */

static int affine_bg_pixel(struct ds *d, unsigned engine, unsigned bg,
                           uint32_t sx, uint32_t sy, uint16_t *out)
{
    uint16_t cnt = bg_cnt(d, engine, bg);
    int16_t pa, pb, pc, pd;
    uint32_t refx, refy;
    affine_params(d, engine, bg, &pa, &pb, &pc, &pd, &refx, &refy);
    int32_t tex_x, tex_y;
    affine_sample(pa, pb, pc, pd, refx, refy, sx, sy, &tex_x, &tex_y);

    uint32_t map_wh = 16u << ((cnt >> 14) & 3u); /* tiles per side */
    int32_t lim = (int32_t)map_wh * 8;
    if (tex_x < 0 || tex_x >= lim || tex_y < 0 || tex_y >= lim) {
        if (!(cnt & 0x2000u))
            return 0; /* outside, no wraparound */
        tex_x &= lim - 1;
        tex_y &= lim - 1;
    }
    uint32_t tile_x = (uint32_t)tex_x >> 3;
    uint32_t tile_y = (uint32_t)tex_y >> 3;
    uint32_t fine_x = (uint32_t)tex_x & 7u;
    uint32_t fine_y = (uint32_t)tex_y & 7u;

    /* affine BG maps: ONE byte per tile (256-color only, no flips) */
    uint8_t entry = bg_region_rd(
        d, engine, ((cnt >> 8) & 0x1Fu) * 2048u + tile_y * map_wh + tile_x);
    uint32_t tile = entry;
    uint32_t char_base = (cnt & 0x3Fu) * 0x4000u;
    uint32_t tile_off = char_base + tile * 64u + fine_y * 8u + fine_x;
    uint8_t idx = bg_region_rd(d, engine, tile_off);
    if (idx == 0u)
        return 0;
    *out = pal16(d, engine, 0, idx);
    return 1;
}

/* ---- extended affine BG ------------------------------------------------------------------ */

static int ext_bg_pixel(struct ds *d, unsigned engine, unsigned bg,
                        uint32_t page_off, uint32_t sx, uint32_t sy,
                        uint16_t *out)
{
    uint16_t cnt = bg_cnt(d, engine, bg);
    uint32_t format = (cnt >> 2) & 0x1Fu;
    if (format > 6u)
        return 0; /* undefined format: layer renders nothing (documented) */

    int16_t pa, pb, pc, pd;
    uint32_t refx, refy;
    affine_params(d, engine, bg, &pa, &pb, &pc, &pd, &refx, &refy);
    int32_t tex_x, tex_y;
    affine_sample(pa, pb, pc, pd, refx, refy, sx, sy, &tex_x, &tex_y);

    uint32_t screen_base = (cnt >> 8) & 0x1Fu;
    uint32_t wrap = (cnt >> 13) & 1u;

    if (format <= 1u) {
        /* affine-mapped tiles; tile data from the BG region start (see
         * header note on the documented char-base approximation). */
        uint32_t map_wh = 16u << ((cnt >> 14) & 3u);
        int32_t lim = (int32_t)map_wh * 8;
        if (tex_x < 0 || tex_x >= lim || tex_y < 0 || tex_y >= lim) {
            if (!wrap)
                return 0;
            tex_x &= lim - 1;
            tex_y &= lim - 1;
        }
        uint32_t tile_x = (uint32_t)tex_x >> 3;
        uint32_t tile_y = (uint32_t)tex_y >> 3;
        uint32_t fine_x = (uint32_t)tex_x & 7u;
        uint32_t fine_y = (uint32_t)tex_y & 7u;
        uint16_t entry = bg_region_rd16(
            d, engine, page_off + screen_base * 0x4000u +
                           (tile_y * map_wh + tile_x) * 2u);
        uint32_t tile = entry & 0x3FFu;
        uint32_t bpp8 = format; /* 0: 16-color, 1: 256-color */
        uint32_t tile_off = page_off + tile * (bpp8 ? 64u : 32u) +
                            fine_y * (bpp8 ? 8u : 4u) +
                            (bpp8 ? fine_x : (fine_x >> 1));
        uint8_t byte = bg_region_rd(d, engine, tile_off);
        uint32_t idx = bpp8 ? byte
                            : ((fine_x & 1u) ? (byte >> 4) : (byte & 0xFu));
        if (idx == 0u)
            return 0;
        if (bpp8)
            *out = pal16(d, engine, 0, idx);
        else
            *out = pal16(d, engine, 0, ((entry >> 12) & 0xFu) * 16u + idx);
        return 1;
    }

    /* bitmap formats 2-6 */
    uint32_t bmp_w, bmp_h, stride;
    switch (format) {
    case 2u: bmp_w = 128u; bmp_h = 128u; stride = 256u; break;
    case 3u: bmp_w = 256u; bmp_h = 256u; stride = 512u; break;
    case 4u: bmp_w = 512u; bmp_h = 256u; stride = 1024u; break;
    case 5u: bmp_w = 256u; bmp_h = 512u; stride = 512u; break;
    default: bmp_w = 256u; bmp_h = 256u; stride = 256u; break; /* 6: 8bpp */
    }
    if (tex_x < 0 || tex_x >= (int32_t)bmp_w || tex_y < 0 ||
        tex_y >= (int32_t)bmp_h) {
        if (!wrap)
            return 0;
        tex_x &= (int32_t)bmp_w - 1;
        tex_y &= (int32_t)bmp_h - 1;
    }
    uint32_t off = page_off + screen_base * 0x4000u + (uint32_t)tex_y * stride +
                   (uint32_t)tex_x * (format == 6u ? 1u : 2u);
    if (format == 6u) {
        uint8_t idx = bg_region_rd(d, engine, off);
        if (idx == 0u)
            return 0;
        *out = pal16(d, engine, 0, idx);
        return 1;
    }
    uint16_t c = bg_region_rd16(d, engine, off);
    if (c == 0u)
        return 0; /* direct-color 0x0000 = transparent */
    *out = (uint16_t)(c & 0x7FFFu); /* bit 15 = alpha flag: ignored */
    return 1;
}

/* ---- OBJ (sprites) -------------------------------------------------------------------------- */

struct obj_attr {
    uint32_t y;        /* attr0 bits 0-7 */
    uint32_t rotscale; /* attr0 bit 8 */
    uint32_t dbl;      /* attr0 bit 9 (double size / disabled) */
    uint32_t mode;     /* attr0 bits 10-11 */
    uint32_t bpp8;     /* attr0 bit 13 (0: 16-color, 1: 256-color) */
    uint32_t shape;    /* attr0 bits 14-15 */
    uint32_t x;        /* attr1 bits 0-8 */
    uint32_t rot_idx;  /* attr1 bits 9-13 (rot/scale) */
    uint32_t hflip;    /* attr1 bit 12 (non-rot) */
    uint32_t vflip;    /* attr1 bit 13 (non-rot) */
    uint32_t size;     /* attr1 bits 14-15 */
    uint32_t tile;     /* attr2 bits 0-9 */
    uint32_t prio;     /* attr2 bits 10-11 */
    uint32_t pal;      /* attr2 bits 12-15 */
};

static void obj_decode(const struct ds *d, unsigned engine, unsigned i,
                       struct obj_attr *a)
{
    uint32_t off = (uint32_t)i * 8u;
    uint16_t a0 = oam_rd16(d, engine, off);
    uint16_t a1 = oam_rd16(d, engine, off + 2u);
    uint16_t a2 = oam_rd16(d, engine, off + 4u);
    a->y = a0 & 0xFFu;
    a->rotscale = (a0 >> 8) & 1u;
    a->dbl = (a0 >> 9) & 1u;
    a->mode = (a0 >> 10) & 3u;
    a->bpp8 = (a0 >> 13) & 1u;
    a->shape = (a0 >> 14) & 3u;
    a->x = a1 & 0x1FFu;
    a->rot_idx = (a1 >> 9) & 0x1Fu;
    a->hflip = (a1 >> 12) & 1u;
    a->vflip = (a1 >> 13) & 1u;
    a->size = (a1 >> 14) & 3u;
    a->tile = a2 & 0x3FFu;
    a->prio = (a2 >> 10) & 3u;
    a->pal = (a2 >> 12) & 0xFu;
}

static void obj_dims(const struct obj_attr *a, uint32_t *w, uint32_t *h)
{
    static const uint32_t dim[3][4][2] = {
        { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },  /* square */
        { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },  /* wide   */
        { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },  /* tall   */
    };
    uint32_t s = a->shape <= 2u ? a->shape : 0u; /* shape 3 forbidden: square */
    *w = dim[s][a->size][0];
    *h = dim[s][a->size][1];
}

/* OBJ tile base address in the OBJ region (engine differences are applied
 * by obj_mem's window mask). 1D: tiles advance per sprite row/column, 32B
 * (4bpp) / 64B (8bpp) each. 2D: tile numbers count in 32-byte units and
 * wrap at the map row boundary - 32 entries per row (4bpp) / 16 (8bpp) on
 * engine A, half that on engine B; 8bpp tiles occupy two consecutive
 * numbers. */
static uint32_t obj_tile_addr(unsigned engine, const struct obj_attr *a,
                              uint32_t tile_x, uint32_t tile_y, uint32_t w,
                              uint32_t one_d)
{
    uint32_t per_tile = a->bpp8 ? 64u : 32u;
    if (one_d)
        return a->tile * 32u +
               (tile_y * (w / 8u) + tile_x) * per_tile;
    uint32_t row_entries = engine == 0u ? 32u : 16u;
    if (a->bpp8)
        row_entries /= 2u;
    uint32_t n = a->tile +
                 (tile_x + tile_y * row_entries) * (a->bpp8 ? 2u : 1u);
    return n * 32u;
}

/* Fetch one OBJ pixel; returns 0 when transparent/outside. */
static int obj_pixel(struct ds *d, unsigned engine, const struct obj_attr *a,
                     uint32_t sx, uint32_t sy, uint16_t *out)
{
    uint32_t w, h;
    obj_dims(a, &w, &h);
    uint32_t dbl = a->rotscale && a->dbl;
    uint32_t disp_w = dbl ? w * 2u : w;
    uint32_t disp_h = dbl ? h * 2u : h;

    /* X wraps in a 512-wide space, Y in a 256-high space (hardware model);
     * a double-size box extends half a size up/left of the X/Y position. */
    int32_t dx = (int32_t)(sx & 511u) - (int32_t)a->x;
    int32_t dy = (int32_t)(sy & 255u) - (int32_t)a->y;
    if (dbl) {
        dx += (int32_t)w / 2;
        dy += (int32_t)h / 2;
    }
    if (dx < 0 || dy < 0 || (uint32_t)dx >= disp_w || (uint32_t)dy >= disp_h)
        return 0;

    uint32_t px, py; /* 0..w-1 / 0..h-1 in sprite space */
    if (a->rotscale) {
        uint32_t pbase = 0x300u + a->rot_idx * 8u;
        int16_t pa = (int16_t)oam_rd16(d, engine, pbase + 0u);
        int16_t pb = (int16_t)oam_rd16(d, engine, pbase + 2u);
        int16_t pc = (int16_t)oam_rd16(d, engine, pbase + 4u);
        int16_t pd = (int16_t)oam_rd16(d, engine, pbase + 6u);
        int32_t rx = dx - (int32_t)w / 2;
        int32_t ry = dy - (int32_t)h / 2;
        int32_t tx = (int32_t)(((int64_t)pa * rx + (int64_t)pb * ry) >> 8) +
                     (int32_t)w / 2;
        int32_t ty = (int32_t)(((int64_t)pc * rx + (int64_t)pd * ry) >> 8) +
                     (int32_t)h / 2;
        if (tx < 0 || ty < 0 || (uint32_t)tx >= w || (uint32_t)ty >= h)
            return 0; /* outside after transform */
        px = (uint32_t)tx;
        py = (uint32_t)ty;
    } else {
        px = (uint32_t)dx;
        py = (uint32_t)dy;
        if (a->hflip)
            px = w - 1u - px;
        if (a->vflip)
            py = h - 1u - py;
    }

    uint32_t one_d = (io_rd16(d, engine, 0x00u) >> 13) & 1u;
    uint32_t addr = obj_tile_addr(engine, a, px >> 3, py >> 3, w, one_d) +
                    (py & 7u) * (a->bpp8 ? 8u : 4u) +
                    (a->bpp8 ? (px & 7u) : ((px & 7u) >> 1));
    uint8_t byte = obj_mem(d, engine, addr);
    uint32_t idx = a->bpp8 ? byte : ((px & 1u) ? (byte >> 4) : (byte & 0xFu));
    if (idx == 0u)
        return 0;
    if (a->bpp8)
        *out = pal16(d, engine, 1, idx);
    else
        *out = pal16(d, engine, 1, a->pal * 16u + idx);
    return 1;
}

/* ---- engine composition ----------------------------------------------------------------------- */

void ds2d_render_engine(struct ds *d, unsigned engine, uint32_t *out)
{
    uint16_t disp = io_rd16(d, engine, 0x00u);
    if (disp & DS_DISP_FORCED_BLANK)
        return; /* framebuffer keeps the black fill */

    uint32_t mode = disp & DS_DISP_MODE;
    uint32_t page = (disp >> 4) & 1u;
    uint16_t mb = io_rd16(d, engine, 0x6Cu);
    uint32_t mb_mode = (mb >> 14) & 3u;
    uint32_t mb_f = mb & 0x3Fu;
    if (mb_mode == 3u)
        mb_mode = 2u; /* forbidden value: treated as brightness up */

    struct obj_attr objs[128];
    uint32_t n_objs = 0;
    if (disp & DS_DISP_OBJ_ENABLE) {
        for (unsigned i = 0; i < 128; i++) {
            struct obj_attr *a = &objs[n_objs];
            obj_decode(d, engine, i, a);
            if (a->mode == 2u || a->mode == 3u)
                continue; /* OBJ window / forbidden: skipped (documented) */
            if (!a->rotscale && a->dbl)
                continue; /* disabled sprite */
            n_objs++;
        }
    }

    uint32_t bg_type[4];
    uint32_t bg_prio[4];
    for (unsigned b = 0; b < 4; b++) {
        uint16_t cnt = bg_cnt(d, engine, b);
        bg_type[b] = bg_layer_type(mode, b);
        bg_prio[b] = cnt & 3u;
    }

    uint16_t backdrop = pal16(d, engine, 0, 0);
    uint32_t backdrop_px = bgr555_to_px(backdrop);

    for (uint32_t sy = 0; sy < DS_SCREEN_H; sy++) {
        uint32_t *row = &out[sy * DS_SCREEN_W];
        for (uint32_t sx = 0; sx < DS_SCREEN_W; sx++) {
            uint32_t px = backdrop_px;
            int found = 0;
            for (uint32_t prio = 0; prio < 4u && !found; prio++) {
                for (uint32_t i = 0; i < n_objs && !found; i++) {
                    if (objs[i].prio != prio)
                        continue;
                    uint16_t c;
                    if (obj_pixel(d, engine, &objs[i], sx, sy, &c)) {
                        px = bgr555_to_px(c);
                        found = 1;
                    }
                }
                for (unsigned b = 0; b < 4 && !found; b++) {
                    if (bg_type[b] == BG_NONE ||
                        bg_prio[b] != prio ||
                        !(disp & (DS_DISP_BG0_ENABLE << b)))
                        continue;
                    uint16_t c;
                    int hit = 0;
                    switch (bg_type[b]) {
                    case BG_TEXT:
                        hit = text_bg_pixel(d, engine, b, sx, sy, &c);
                        break;
                    case BG_AFFINE:
                        hit = affine_bg_pixel(d, engine, b, sx, sy, &c);
                        break;
                    case BG_EXT:
                        hit = ext_bg_pixel(d, engine, b, page * 0xA000u,
                                           sx, sy, &c);
                        break;
                    default:
                        break;
                    }
                    if (hit) {
                        px = bgr555_to_px(c);
                        found = 1;
                    }
                }
            }
            if (mb_f != 0u && mb_mode != 0u) {
                uint32_t r = EMU_PIXEL_R(px), g = EMU_PIXEL_G(px),
                         b = EMU_PIXEL_B(px);
                if (mb_mode == 1u) {
                    r = (r * (63u - mb_f)) / 63u;
                    g = (g * (63u - mb_f)) / 63u;
                    b = (b * (63u - mb_f)) / 63u;
                } else {
                    r = r + ((255u - r) * mb_f) / 63u;
                    g = g + ((255u - g) * mb_f) / 63u;
                    b = b + ((255u - b) * mb_f) / 63u;
                }
                px = EMU_PIXEL(r, g, b);
            }
            row[sx] = px;
        }
    }
}
