/*
 * supersaturn: VDP1 implementation.
 *
 * Source of truth: VDP1 User's Manual (ST-013-R3). Everything here maps to
 * that document: address map (figure 2.1), system registers (table 2.1),
 * command table layout (chapter 6 tables: CMDCTRL/CMDLINK/CMDPMOD/CMDCOLR/
 * CMDSRCA/CMDSIZE/CMDXA-YD), draw commands (chapter 7) and the
 * erase/write + frame change registers (sections 4.1-4.4).
 *
 * Documented subset:
 *  - TVM = 0 (normal, 16 bpp) only; PTM 01B (write trigger) and 10B
 *    (frame-start trigger) both start the command list.
 *  - Frame buffer change: 1-cycle mode (auto swap every frame). Manual
 *    modes (FCM=1) keep the current buffer pairing (documented gap).
 *  - Draw commands: normal sprite, scaled sprite (all nine zoom points),
 *    polygon, polyline, line; distorted sprites fall back to a flat
 *    polygon fill (textured quad mapping not implemented - documented).
 *  - Color modes 0-5 with color-bank lookups in VDP2 CRAM, lookup tables
 *    in VDP1 VRAM, and direct RGB555. SPD/ECD/mesh are implemented.
 *    Half-transparency and Gouraud shading (CMDPMOD color-calc bits) are
 *    not implemented; those bits are ignored (documented).
 */
#include "saturn.h"

#include <string.h>

/* ---- VRAM / register access ---------------------------------------------------- */

uint16_t sat_vdp1_vram16(const struct saturn *s, uint32_t byte_addr)
{
    byte_addr &= 0x7FFFFu;
    return (uint16_t)((s->vdp1_vram[byte_addr & ~1u] << 8) |
                      s->vdp1_vram[byte_addr | 1u]);
}

void sat_vdp1_set_vram16(struct saturn *s, uint32_t byte_addr, uint16_t v)
{
    byte_addr &= 0x7FFFFu;
    s->vdp1_vram[byte_addr & ~1u] = (uint8_t)(v >> 8);
    s->vdp1_vram[byte_addr | 1u] = (uint8_t)v;
}

static uint16_t vdp2_cram16(const struct saturn *s, uint32_t word_index)
{
    word_index &= 0x7FFu; /* 4 KiB CRAM = 2048 words */
    const uint8_t *p = &s->vdp2_cram[word_index * 2u];
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint16_t sat_vdp1_reg_read(const struct saturn *s, uint32_t word_index)
{
    word_index &= 0xFu;
    /* TVHR..ENDR are write-only; EDSR..MODR read-only (table 2.1). */
    if (word_index >= SAT_VDP1_EDSR)
        return s->vdp1_reg[word_index];
    return 0;
}

void sat_vdp1_reg_write(struct saturn *s, uint32_t word_index, uint16_t v)
{
    word_index &= 0xFu;
    if (word_index > SAT_VDP1_ENDR)
        return; /* read-only slots */
    switch (word_index) {
    case SAT_VDP1_PTMR:
        s->vdp1_reg[SAT_VDP1_PTMR] = (uint16_t)(v & 3u);
        s->vdp1_ptmr_run = ((v & 3u) == 2u) ? 1u : 0u;
        if ((v & 3u) == 1u)
            sat_vdp1_execute(s); /* PTM=01B: start on write */
        break;
    case SAT_VDP1_ENDR:
        /* forced termination: abort auto-run */
        s->vdp1_ptmr_run = 0;
        s->vdp1_reg[SAT_VDP1_PTMR] = 0;
        break;
    default:
        s->vdp1_reg[word_index] = v;
        break;
    }
}

/* ---- drawing helpers -------------------------------------------------------------- */

static inline int32_t sext16(uint32_t v) { return (int32_t)(int16_t)v; }

static inline uint32_t rgb555(uint16_t c)
{
    uint32_t r = ((c >> 10) & 0x1Fu) << 3;
    uint32_t g = ((c >> 5) & 0x1Fu) << 3;
    uint32_t b = (c & 0x1Fu) << 3;
    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return EMU_PIXEL((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* Convert a command color code to RGB555. Color modes 0-4 (bank/LUT)
 * resolve through VDP2 CRAM; mode 5 is direct RGB (documented). */
static uint16_t resolve_color(struct saturn *s, uint16_t pmod,
                              uint16_t colr, uint32_t pixel)
{
    uint32_t mode = (pmod >> 3) & 7u;
    switch (mode) {
    case 0: /* 16-color bank, 4 bpp */
        return vdp2_cram16(s, (((colr & 0xFFF0u) + (pixel & 0xFu)) & 0x7FFu));
    case 1: /* 16-color lookup table, 4 bpp */
        return sat_vdp1_vram16(
            s, ((uint32_t)colr * 8u + (pixel & 0xFu) * 2u) & 0x7FFFFu);
    case 2: /* 64-color bank, 8 bpp */
        return vdp2_cram16(s, (((colr & 0xFFC0u) + (pixel & 0xFFu)) & 0x7FFu));
    case 3: /* 128-color bank, 8 bpp */
        return vdp2_cram16(s, (((colr & 0xFF80u) + (pixel & 0xFFu)) & 0x7FFu));
    case 4: /* 256-color bank, 8 bpp */
        return vdp2_cram16(s, (((colr & 0xFF00u) + (pixel & 0xFFu)) & 0x7FFu));
    default: /* mode 5: RGB 1-5-5-5 (MSB is a transparency flag) */
        return (uint16_t)(pixel & 0x7FFFu);
    }
}

/* Plot with system/user clipping, mesh and transparency (manual sections
 * 6.3 CMDPMOD and 7.1/7.2 clipping commands). */
static void put_pixel(struct saturn *s, int32_t x, int32_t y, uint16_t pmod,
                      uint16_t c16)
{
    if (x < 0 || y < 0 || x >= (int32_t)SAT_FB_W || y >= (int32_t)SAT_FB_H)
        return;
    if (x > (int32_t)s->sys_clip[0] || y > (int32_t)s->sys_clip[1])
        return;
    if ((pmod & 0x0200u) != 0u) { /* user clipping enabled */
        int inside = x >= (int32_t)s->user_clip[0] &&
                     y >= (int32_t)s->user_clip[1] &&
                     x <= (int32_t)s->user_clip[2] &&
                     y <= (int32_t)s->user_clip[3];
        int want_inside = (pmod & 0x0400u) == 0u; /* Clip bit: 0 = inside */
        if (inside != want_inside)
            return;
    }
    if ((pmod & 0x0100u) != 0u && ((x + y) & 1u) != 0u)
        return; /* mesh: every other pixel */
    /* the frame buffer holds raw 16-bit color codes, as on hardware;
     * RGB555 expansion happens at display time (sat_render_output) */
    s->vdp1_fb[s->vdp1_draw_sel][(uint32_t)y * SAT_FB_W + (uint32_t)x] =
        c16;
}

/* Per-pixel color pipeline for textured draws: end-code handling (ECD),
 * transparent codes (SPD), then color resolution and plot. Manual
 * section 6.3: end codes are pixels 0xF/0xFF (4/8 bpp); the transparent
 * code is color code 0x0000 in bank modes, LUT index 0 in lookup mode,
 * and the MSB in RGB mode. */
static void draw_tex_pixel(struct saturn *s, int32_t x, int32_t y,
                           uint16_t pmod, uint16_t colr, uint32_t px)
{
    uint32_t mode = (pmod >> 3) & 7u;
    uint32_t spd = (pmod >> 6) & 1u;
    uint32_t ecd = (pmod >> 7) & 1u;
    if (mode == 5u) { /* RGB 1-5-5-5 */
        if ((px & 0x8000u) != 0u && spd == 0u)
            return;
        put_pixel(s, x, y, pmod, (uint16_t)(px & 0x7FFFu));
        return;
    }
    int endcode = (mode <= 1u) ? (px == 0xFu) : (px == 0xFFu);
    if (endcode != 0 && ecd == 0u)
        return; /* end code: treated as transparent unless disabled     */
    if (mode == 1u && px == 0u && spd == 0u)
        return; /* LUT mode: pixel data 0 is the transparent code       */
    uint16_t c16 = resolve_color(s, pmod, colr, px);
    if (c16 == 0u && spd == 0u)
        return; /* bank modes: color code 0x0000 is transparent         */
    put_pixel(s, x, y, pmod, c16);
}

/* Flat-color triangle fill via bounding box + edge functions. Used by
 * polygon draws (manual section 7.7). */
static void fill_tri(struct saturn *s, int32_t x0, int32_t y0, int32_t x1,
                     int32_t y1, int32_t x2, int32_t y2, uint16_t pmod,
                     uint16_t c16)
{
    int64_t area = (int64_t)(x1 - x0) * (y2 - y0) -
                   (int64_t)(x2 - x0) * (y1 - y0);
    if (area == 0)
        return;
    int sign = area > 0 ? 1 : -1;
    int32_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int32_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int32_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int32_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx >= (int32_t)SAT_FB_W)
        maxx = (int32_t)SAT_FB_W - 1;
    if (maxy >= (int32_t)SAT_FB_H)
        maxy = (int32_t)SAT_FB_H - 1;
    if ((pmod & 0x0200u) != 0u && (pmod & 0x0400u) == 0u) {
        /* inside-user-clip fill: clamp the scan bounds to the clip rect */
        if (minx < (int32_t)s->user_clip[0])
            minx = (int32_t)s->user_clip[0];
        if (miny < (int32_t)s->user_clip[1])
            miny = (int32_t)s->user_clip[1];
        if (maxx > (int32_t)s->user_clip[2])
            maxx = (int32_t)s->user_clip[2];
        if (maxy > (int32_t)s->user_clip[3])
            maxy = (int32_t)s->user_clip[3];
    }
    for (int32_t y = miny; y <= maxy; y++) {
        for (int32_t x = minx; x <= maxx; x++) {
            int64_t e0 = (int64_t)(x1 - x0) * (y - y0) -
                         (int64_t)(y1 - y0) * (x - x0);
            int64_t e1 = (int64_t)(x2 - x1) * (y - y1) -
                         (int64_t)(y2 - y1) * (x - x1);
            int64_t e2 = (int64_t)(x0 - x2) * (y - y2) -
                         (int64_t)(y0 - y2) * (x - x2);
            if ((e0 * sign >= 0) && (e1 * sign >= 0) && (e2 * sign >= 0))
                put_pixel(s, x, y, pmod, c16);
        }
    }
}

static void fill_quad(struct saturn *s, const int32_t *x, const int32_t *y,
                      uint16_t pmod, uint16_t c16)
{
    fill_tri(s, x[0], y[0], x[1], y[1], x[2], y[2], pmod, c16);
    fill_tri(s, x[0], y[0], x[2], y[2], x[3], y[3], pmod, c16);
}

/* Bresenham line with clipping (polyline/line commands, sections 7.8/7.9). */
static void draw_line(struct saturn *s, int32_t x0, int32_t y0, int32_t x1,
                      int32_t y1, uint16_t pmod, uint16_t c16)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int guard = 0; guard < 2048; guard++) {
        put_pixel(s, x0, y0, pmod, c16);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* ---- texture fetch ------------------------------------------------------------------ */

static uint32_t tex_fetch(const struct saturn *s, uint32_t srca, uint32_t pmod,
                          int32_t tw, int32_t th, int32_t tx, int32_t ty)
{
    if ((pmod & 0x0010u) != 0u) /* Dir bit 4: horizontal flip */
        tx = tw - 1 - tx;
    if ((pmod & 0x0020u) != 0u) /* Dir bit 5: vertical flip */
        ty = th - 1 - ty;
    uint32_t mode = (pmod >> 3) & 7u;
    uint32_t addr = srca * 8u;
    if (mode == 0u || mode == 1u) { /* 4 bpp */
        addr += (uint32_t)ty * ((uint32_t)tw >> 1) + ((uint32_t)tx >> 1);
        uint8_t b = s->vdp1_vram[addr & 0x7FFFFu];
        return ((tx & 1u) != 0u) ? (b & 0xFu) : (b >> 4);
    }
    if (mode >= 2u && mode <= 4u) { /* 8 bpp */
        addr += (uint32_t)ty * (uint32_t)tw + (uint32_t)tx;
        return s->vdp1_vram[addr & 0x7FFFFu];
    }
    /* 16 bpp */
    addr += (uint32_t)ty * (uint32_t)tw * 2u + (uint32_t)tx * 2u;
    return sat_vdp1_vram16(s, addr);
}

/* ---- draw commands ------------------------------------------------------------------- */

static void cmd_normal_sprite(struct saturn *s, uint32_t a)
{
    uint16_t pmod = sat_vdp1_vram16(s, a + 4u);
    uint16_t colr = sat_vdp1_vram16(s, a + 6u);
    uint16_t srca = sat_vdp1_vram16(s, a + 8u);
    uint16_t size = sat_vdp1_vram16(s, a + 0xAu);
    int32_t tw = (int32_t)((size >> 8) & 0x3Fu) * 8;
    int32_t th = (int32_t)(size & 0xFFu);
    int32_t x0 = s->local_x + sext16(sat_vdp1_vram16(s, a + 0xCu));
    int32_t y0 = s->local_y + sext16(sat_vdp1_vram16(s, a + 0xEu));

    for (int32_t dy = 0; dy < th; dy++)
        for (int32_t dx = 0; dx < tw; dx++)
            draw_tex_pixel(s, x0 + dx, y0 + dy, pmod, colr,
                           tex_fetch(s, srca, pmod, tw, th, dx, dy));
}

static void blit_scaled(struct saturn *s, uint16_t pmod, uint16_t colr,
                        uint16_t srca, int32_t tw, int32_t th, int32_t x0,
                        int32_t y0, int32_t x1, int32_t y1)
{
    if (x1 < x0 || y1 < y0)
        return;
    int32_t dw = x1 - x0 + 1;
    int32_t dh = y1 - y0 + 1;
    for (int32_t dy = 0; dy < dh; dy++) {
        int32_t sy = (th * dy) / dh;
        for (int32_t dx = 0; dx < dw; dx++) {
            int32_t sx = (tw * dx) / dw;
            draw_tex_pixel(s, x0 + dx, y0 + dy, pmod, colr,
                           tex_fetch(s, srca, pmod, tw, th, sx, sy));
        }
    }
}

static void cmd_scaled_sprite(struct saturn *s, uint32_t a)
{
    uint16_t pmod = sat_vdp1_vram16(s, a + 4u);
    uint16_t colr = sat_vdp1_vram16(s, a + 6u);
    uint16_t srca = sat_vdp1_vram16(s, a + 8u);
    uint16_t size = sat_vdp1_vram16(s, a + 0xAu);
    uint16_t ctrl = sat_vdp1_vram16(s, a);
    uint32_t zp = (ctrl >> 8) & 0xFu;
    int32_t tw = (int32_t)((size >> 8) & 0x3Fu) * 8;
    int32_t th = (int32_t)(size & 0xFFu);

    if (zp == 0u) { /* two vertices (section 7.5) */
        int32_t x0 = s->local_x + sext16(sat_vdp1_vram16(s, a + 0xCu));
        int32_t y0 = s->local_y + sext16(sat_vdp1_vram16(s, a + 0xEu));
        int32_t x1 = s->local_x + sext16(sat_vdp1_vram16(s, a + 0x14u));
        int32_t y1 = s->local_y + sext16(sat_vdp1_vram16(s, a + 0x16u));
        blit_scaled(s, pmod, colr, srca, tw, th, x0, y0, x1, y1);
        return;
    }
    /* zoom-point form (section 7.5, figure 6.2): position = zoom point
     * coordinates (XA/YA), size = display width (XB/YB at +10/+12). */
    int32_t xa = s->local_x + sext16(sat_vdp1_vram16(s, a + 0xCu));
    int32_t ya = s->local_y + sext16(sat_vdp1_vram16(s, a + 0xEu));
    int32_t xb = sext16(sat_vdp1_vram16(s, a + 0x10u));
    int32_t yb = sext16(sat_vdp1_vram16(s, a + 0x12u));
    if (xb < 0 || yb < 0)
        return; /* negative widths: drawing not guaranteed (manual) */
    uint32_t hz = zp & 3u, vt = (zp >> 2) & 3u;
    int32_t left, right, top, bottom;
    switch (hz) {
    case 1u: left = xa; right = xa + xb; break;              /* left   */
    case 2u: left = xa - xb / 2; right = xa + (xb + 1) / 2; break; /* ctr */
    default: left = xa - xb; right = xa; break;              /* right  */
    }
    switch (vt) {
    case 1u: top = ya; bottom = ya + yb; break;
    case 2u: top = ya - yb / 2; bottom = ya + (yb + 1) / 2; break;
    default: top = ya - yb; bottom = ya; break;
    }
    blit_scaled(s, pmod, colr, srca, tw, th, left, top, right - 1,
                bottom - 1);
}

static void cmd_polygon(struct saturn *s, uint32_t a)
{
    uint16_t pmod = sat_vdp1_vram16(s, a + 4u);
    uint16_t colr = sat_vdp1_vram16(s, a + 6u);
    int32_t x[4], y[4];
    for (int i = 0; i < 4; i++) {
        x[i] = s->local_x + sext16(sat_vdp1_vram16(s, a + 0xCu + 4u * (uint32_t)i));
        y[i] = s->local_y + sext16(sat_vdp1_vram16(s, a + 0xEu + 4u * (uint32_t)i));
    }
    /* non-textured: CMDPMOD color mode must be 000B (manual); the mesh
     * and user-clipping bits still apply. */
    fill_quad(s, x, y, (uint16_t)(pmod & 0x0F00u), colr);
}

static void cmd_polyline(struct saturn *s, uint32_t a)
{
    uint16_t colr = sat_vdp1_vram16(s, a + 6u);
    int32_t x[4], y[4];
    for (int i = 0; i < 4; i++) {
        x[i] = s->local_x + sext16(sat_vdp1_vram16(s, a + 0xCu + 4u * (uint32_t)i));
        y[i] = s->local_y + sext16(sat_vdp1_vram16(s, a + 0xEu + 4u * (uint32_t)i));
    }
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        draw_line(s, x[i], y[i], x[j], y[j], 0, colr);
    }
}

static void cmd_line(struct saturn *s, uint32_t a)
{
    uint16_t colr = sat_vdp1_vram16(s, a + 6u);
    int32_t x0 = s->local_x + sext16(sat_vdp1_vram16(s, a + 0xCu));
    int32_t y0 = s->local_y + sext16(sat_vdp1_vram16(s, a + 0xEu));
    int32_t x1 = s->local_x + sext16(sat_vdp1_vram16(s, a + 0x10u));
    int32_t y1 = s->local_y + sext16(sat_vdp1_vram16(s, a + 0x12u));
    draw_line(s, x0, y0, x1, y1, 0, colr);
}

/* ---- command list ---------------------------------------------------------------------- */

void sat_vdp1_execute(struct saturn *s)
{
    s->vdp1_reg[SAT_VDP1_EDSR] = 0;
    uint32_t addr = 0;
    uint32_t ret_addr = 0xFFFFFFFFu; /* one nesting level (manual) */

    for (int guard = 0; guard < 4096; guard++) {
        if (addr + 0x20u > 0x80000u)
            break;
        uint16_t ctrl = sat_vdp1_vram16(s, addr);
        uint16_t link = sat_vdp1_vram16(s, addr + 2u);
        s->vdp1_reg[SAT_VDP1_COPR] = (uint16_t)(addr >> 3);
        uint32_t jp = ((ctrl >> 12) & 7u);
        int skip = (int)((jp >> 2) & 1u);
        uint32_t comm = ctrl & 0xFu;

        if ((ctrl & 0x8000u) != 0u) {
            /* draw end command (table 6.1) */
            s->vdp1_reg[SAT_VDP1_LOPR] = (uint16_t)(addr >> 3);
            break;
        }
        if (!skip) {
            switch (comm) {
            case 0x0u: cmd_normal_sprite(s, addr); break;
            case 0x1u: cmd_scaled_sprite(s, addr); break;
            case 0x2u: /* distorted sprite: flat fill fallback */
            case 0x4u: cmd_polygon(s, addr); break;
            case 0x5u: cmd_polyline(s, addr); break;
            case 0x6u: cmd_line(s, addr); break;
            case 0x8u: /* user clipping coordinate set */
                s->user_clip[0] = sat_vdp1_vram16(s, addr + 0xCu);
                s->user_clip[1] = sat_vdp1_vram16(s, addr + 0xEu);
                s->user_clip[2] = sat_vdp1_vram16(s, addr + 0x10u);
                s->user_clip[3] = sat_vdp1_vram16(s, addr + 0x12u);
                break;
            case 0x9u: /* system clipping coordinate set */
                s->sys_clip[0] = sat_vdp1_vram16(s, addr + 0x10u);
                s->sys_clip[1] = sat_vdp1_vram16(s, addr + 0x12u);
                break;
            case 0xAu: /* local coordinate set */
                s->local_x = sext16(sat_vdp1_vram16(s, addr + 0xCu));
                s->local_y = sext16(sat_vdp1_vram16(s, addr + 0xEu));
                break;
            default:
                break; /* setting-prohibited codes ignored */
            }
        }

        switch (jp) { /* jump modes, manual section 6.1 */
        case 0: addr += 0x20u; break;               /* jump next       */
        case 1: addr = (uint32_t)link * 8u; break;  /* jump assign     */
        case 2:                                     /* jump call       */
            ret_addr = addr + 0x20u;
            addr = (uint32_t)link * 8u;
            break;
        case 3: addr = ret_addr; break;             /* jump return     */
        case 4: addr += 0x20u; break;               /* skip next       */
        case 5: addr = (uint32_t)link * 8u; break;  /* skip assign     */
        case 6:                                     /* skip call       */
            ret_addr = addr + 0x20u;
            addr = (uint32_t)link * 8u;
            break;
        default: addr = ret_addr; break;            /* skip return     */
        }
        if (addr >= 0x80000u)
            break;
    }

    s->vdp1_reg[SAT_VDP1_EDSR] = 1; /* transfer end status (table 2.1) */
    /* sprite draw end interrupt: SCU bit 13, vector 0x4D, level 2 */
    sat_scu_send(s, SAT_IST_DRAWEND, 0x4Du, 2u);
}

/* Frame change: erase/write into the new drawing buffer (VBE honored)
 * and swap display/draw buffers (1-cycle mode; manual sections 4.1-4.4). */
void sat_vdp1_frame_change(struct saturn *s)
{
    uint16_t *dst = s->vdp1_fb[s->vdp1_draw_sel ^ 1u];
    if ((s->vdp1_reg[SAT_VDP1_TVMR] & SAT_TVMR_VBE) != 0u) {
        uint16_t ewdr = s->vdp1_reg[SAT_VDP1_EWDR];
        uint32_t x1 = ((s->vdp1_reg[SAT_VDP1_EWLR] >> 9) & 0x3Fu) * 8u;
        uint32_t y1 = s->vdp1_reg[SAT_VDP1_EWLR] & 0x1FFu;
        uint32_t x3 = ((s->vdp1_reg[SAT_VDP1_EWRR] >> 9) & 0x7Fu) * 8u;
        uint32_t y3 = s->vdp1_reg[SAT_VDP1_EWRR] & 0x1FFu;
        if (x3 > SAT_FB_W)
            x3 = SAT_FB_W;
        if (y3 > SAT_FB_H)
            y3 = SAT_FB_H;
        for (uint32_t y = y1; y < y3; y++) {
            uint16_t *row = &dst[y * SAT_FB_W];
            for (uint32_t x = x1; x < x3; x++)
                row[x] = ewdr;
        }
    } else {
        memset(dst, 0, SAT_FB_W * SAT_FB_H * sizeof(uint16_t));
    }
    if ((s->vdp1_reg[SAT_VDP1_FBCR] & SAT_FBCR_FCM) == 0u)
        s->vdp1_draw_sel = (uint8_t)(s->vdp1_draw_sel ^ 1u);
}

/* Display the non-drawing buffer (only the drawing screen is
 * CPU-accessible, manual figure 2.1 note *2). */
void sat_render_output(struct saturn *s)
{
    const uint16_t *src = s->vdp1_fb[s->vdp1_draw_sel ^ 1u];
    /* display window = frame buffer origin (normal TVM, 320x224) */
    for (uint32_t y = 0; y < SAT_SCREEN_H; y++) {
        const uint16_t *row = &src[y * SAT_FB_W];
        uint32_t *out = &s->fb[y * SAT_SCREEN_W];
        for (uint32_t x = 0; x < SAT_SCREEN_W; x++)
            out[x] = rgb555(row[x]);
    }
}
