/*
 * supersaturn machine tests.
 *
 * Expected values are derived from the Sega Saturn hardware documents
 * (SCU/VDP1/SMPC user manuals and the disc format spec, see
 * src/supersaturn/saturn.h) and from hand-assembled SH-2 code, never by
 * calling emulator internals.
 */
#include "../tests.h"
#include "../../src/supersaturn/saturn.h"

#include <stdlib.h>
#include <string.h>

#define AIP_FILE_OFF 0x800u /* AIP offset inside the IP.BIN image       */
#define AIP_RAM_ADDR 0x06002000u

static void w16(uint8_t *p, uint32_t off, uint16_t v)
{
    p[off] = (uint8_t)(v >> 8); /* big-endian bus order */
    p[off + 1] = (uint8_t)v;
}

static void w32(uint8_t *p, uint32_t off, uint32_t v)
{
    w16(p, off, (uint16_t)(v >> 16));
    w16(p, off + 2, (uint16_t)v);
}

static uint32_t r32(struct saturn *s, uint32_t addr)
{
    return sat_bus_read32(s, addr);
}

static struct saturn *sat_new(void)
{
    emu_core_t *c = NULL;
    if (emu_core_supersaturn()->create(&c) != EMU_OK)
        return NULL;
    return (struct saturn *)c;
}

static void sat_free(struct saturn *s)
{
    emu_core_supersaturn()->destroy(&s->base);
}

/*
 * Synthetic IP.BIN: "SEGA SEGASATURN" header, IP size 0x1000 at 0xE0,
 * and an SH-2 program at the AIP offset that stores the longword
 * `marker` to `marker_addr`, then loops forever.
 *
 * Hand-assembled (SH-2 encodings from the Hitachi programming manual):
 *   0x06002000: D1 02        mov.l @(2,pc), r1    ; data at  +0x0C
 *   0x06002002: D2 06        mov.l @(6,pc), r2    ; data at  +0x1C
 *   0x06002004: 22 12        mov.l r1, @r2
 *   0x06002006: B0 FE        bra self
 *   0x06002008: 00 09        nop                  ; delay slot
 *   0x0600200A: 00 09        nop
 *   0x0600200C: <marker>
 *   0x0600201C: <marker_addr>
 */
static uint8_t *sat_make_disc(uint32_t marker, uint32_t marker_addr,
                              size_t *size_out)
{
    size_t size = 0x2000u; /* 16 sectors allowed per the spec; the boot
                            * model copies up to 0x7800 bytes of AIP */
    uint8_t *d = calloc(1, size);
    if (d == NULL)
        return NULL;
    memcpy(d, "SEGA SEGASATURN", 15);
    w32(d, 0xE0, 0x00001000u); /* IP SIZE (spec range 1000-8000H)      */
    /* STACK-M/STACK-S left 0: documented defaults apply                  */

    w16(d, AIP_FILE_OFF + 0x00u, 0xD102u);
    w16(d, AIP_FILE_OFF + 0x02u, 0xD206u);
    w16(d, AIP_FILE_OFF + 0x04u, 0x2212u);
    w16(d, AIP_FILE_OFF + 0x06u, 0xAFFEu);
    w16(d, AIP_FILE_OFF + 0x08u, 0x0009u);
    w16(d, AIP_FILE_OFF + 0x0Au, 0x0009u);
    w32(d, AIP_FILE_OFF + 0x0Cu, marker);
    w32(d, AIP_FILE_OFF + 0x1Cu, marker_addr);
    *size_out = size;
    return d;
}

static int sat_boot(struct saturn **out, uint32_t marker,
                    uint32_t marker_addr)
{
    size_t size = 0;
    uint8_t *disc = sat_make_disc(marker, marker_addr, &size);
    if (disc == NULL)
        return 0;
    struct saturn *s = sat_new();
    if (s == NULL) {
        free(disc);
        return 0;
    }
    if (emu_core_supersaturn()->load_rom(&s->base, disc, size) != EMU_OK) {
        free(disc);
        sat_free(s);
        return 0;
    }
    free(disc);
    *out = s;
    return 1;
}

/* ---- lifecycle ------------------------------------------------------------------ */

static void lifecycle_rejects_bad_images(void)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(emu_core_supersaturn()->create(&c), EMU_OK);
    T_CHECK_EQ(emu_core_supersaturn()->load_rom(c, NULL, 10), EMU_EINVAL);
    static const uint8_t bad[0x800] = { 0 };
    T_CHECK_EQ(emu_core_supersaturn()->load_rom(c, bad, sizeof bad),
               EMU_EBADROM);
    T_CHECK_EQ(emu_core_supersaturn()->run_frame(c), EMU_ENOROM);
    emu_core_supersaturn()->destroy(c);

    /* short image without the full 0x100-byte header is rejected */
    T_CHECK_EQ(emu_core_supersaturn()->create(&c), EMU_OK);
    static const uint8_t tiny[16] = { 0 };
    T_CHECK_EQ(emu_core_supersaturn()->load_rom(c, tiny, sizeof tiny),
               EMU_EBADROM);
    emu_core_supersaturn()->destroy(c);
}

static void boot_executes_marker_program(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 0x12345678u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }
    T_CHECK_EQ(emu_core_supersaturn()->run_frame(&s->base), EMU_OK);
    T_CHECK_EQ_U(r32(s, 0x0600A000u), 0x12345678u);
    /* master started at the AIP load address with the default stacks */
    T_CHECK_EQ_U(s->boot_sp_m, 0x06002000u);
    T_CHECK_EQ_U(s->boot_sp_s, 0x06001000u);
    sat_free(s);
}

static void sinit_starts_slave(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 0xCAFEBABEu, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }
    T_CHECK_EQ(s->slave_on, 0);
    /* SINIT write releases the slave (SCU map region 0x01800004) */
    sat_bus_write8(s, 0x01800004u, 0x01u);
    T_CHECK_EQ(s->slave_on, 1);
    T_CHECK_EQ(emu_core_supersaturn()->run_frame(&s->base), EMU_OK);
    T_CHECK_EQ_U(r32(s, 0x0600A000u), 0xCAFEBABEu);
    sat_free(s);

    /* without a disc image the SINIT write must not start the slave */
    struct saturn *empty = sat_new();
    sat_bus_write8(empty, 0x01800004u, 0x01u);
    T_CHECK_EQ(empty->slave_on, 0);
    sat_free(empty);
}

/* ---- SCU ------------------------------------------------------------------------ */

static void scu_dma_direct_copies(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }

    /* source pattern in Work RAM-L */
    for (uint32_t i = 0; i < 16u; i++)
        sat_bus_write32(s, 0x00201000u + i * 4u,
                        0x11000000u + i * 0x010101u);

    /* level 0: direct mode, immediate start (factor 7) */
    uint32_t base = 0x05FE0000u;
    sat_bus_write32(s, base + 0x00u, 0x00201000u); /* D0R: read address   */
    sat_bus_write32(s, base + 0x04u, 0x00210000u); /* D0W: write address  */
    sat_bus_write32(s, base + 0x08u, 64u);         /* D0C: 64 bytes       */
    sat_bus_write32(s, base + 0x0Cu, 0x00000101u); /* D0AD: +4/+2 per unit*/
    sat_bus_write32(s, base + 0x14u, 0x00010107u); /* D0MD: keep addrs, f7*/
    sat_bus_write32(s, base + 0x10u, 0x00000001u); /* D0EN: enable -> go  */

    sat_line_tick(s);
    for (uint32_t i = 0; i < 16u; i++)
        T_CHECK_EQ_U(r32(s, 0x00210000u + i * 4u),
                     0x11000000u + i * 0x010101u);
    /* enable bit dropped, end interrupt pending (level 0 = IST bit 11) */
    T_CHECK_EQ_U(r32(s, base + 0x10u) & 1u, 0u);
    T_CHECK_EQ_U(r32(s, base + 0xA4u) & 0x00000800u, 0x00000800u);
    sat_free(s);
}

static void scu_dma_indirect_list(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }

    for (uint32_t i = 0; i < 12u; i++)
        sat_bus_write32(s, 0x06008100u + i * 4u,
                        0xAA000000u + i * 0x010101u);

    /* header at 0x06008000: two entries; the second carries the end flag
     * in bit 31 of its read-address word (SCU manual section 2) */
    uint32_t tb = 0x06008000u - 0x06000000u;
    w32(s->wramh, tb + 0x00u, 32u);
    w32(s->wramh, tb + 0x04u, 0x06009000u);
    w32(s->wramh, tb + 0x08u, 0x06008100u);
    w32(s->wramh, tb + 0x0Cu, 8u);
    w32(s->wramh, tb + 0x10u, 0x06009100u);
    w32(s->wramh, tb + 0x14u, 0x86008120u); /* end flag                  */

    uint32_t base = 0x05FE0000u;
    sat_bus_write32(s, base + 0x04u, 0x06008000u); /* DxW = table address */
    sat_bus_write32(s, base + 0x0Cu, 0x00000101u);
    sat_bus_write32(s, base + 0x14u, 0x01010107u); /* bit 24: indirect    */
    sat_bus_write32(s, base + 0x10u, 0x00000001u);

    sat_line_tick(s);
    for (uint32_t i = 0; i < 8u; i++)
        T_CHECK_EQ_U(r32(s, 0x06009000u + i * 4u),
                     0xAA000000u + i * 0x010101u);
    for (uint32_t i = 0; i < 2u; i++)
        T_CHECK_EQ_U(r32(s, 0x06009100u + i * 4u),
                     0xAA000000u + (8u + i) * 0x010101u);
    T_CHECK_EQ_U(r32(s, base + 0xA4u) & 0x00000800u, 0x00000800u);
    sat_free(s);
}

/* IRQ handler shared by the interrupt tests: increments the byte at
 * 0x0600B000, then RTE (encodings from the Hitachi manual).
 *   D0 02  mov.l @(2,pc), r0    ; counter address at +0x0C
 *   61 00  mov.b @r0, r1
 *   71 01  add #1, r1
 *   20 10  mov.b r1, @r0
 *   00 2B  rte
 *   00 09  nop (delay slot)
 */
static const uint16_t kIrqHandler[6] = { 0xD002u, 0x6100u, 0x7101u,
                                         0x2010u, 0x002Bu, 0x0009u };

static void install_irq_handler(struct saturn *s, uint32_t vector)
{
    uint32_t vec_addr = 0x06000000u + 0x600u + vector * 4u;
    /* code block at 0x06000900, one 32-byte slot per vector so the
     * handlers cannot clobber each other */
    uint32_t handler = 0x06000900u + vector * 0x20u;
    w32(s->wramh, vec_addr - 0x06000000u, handler);
    for (int i = 0; i < 6; i++)
        w16(s->wramh, handler - 0x06000000u + (uint32_t)i * 2u,
            kIrqHandler[i]);
    w32(s->wramh, handler - 0x06000000u + 0x0Cu, 0x0600B000u);
}

static void scu_interrupts_fire(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }

    /* program loops (VBR = 0x06000000 comes from the boot model):
     *   A0 FE  bra self; nop */
    w16(s->wramh, 0x06002004u - 0x06000000u, 0xAFFEu);
    w16(s->wramh, 0x06002006u - 0x06000000u, 0x0009u);
    for (uint32_t v = 0x40u; v <= 0x4Du; v++)
        install_irq_handler(s, v);
    /* unmask V-Blank-IN/OUT + H-Blank-IN (IMS bit = 1 masks) */
    sat_bus_write32(s, 0x05FE00A0u, 0xFFFFFFF8u);

    T_CHECK_EQ(emu_core_supersaturn()->run_frame(&s->base), EMU_OK);
    uint8_t count = s->wramh[0x0600B000u - 0x06000000u];
    /* one frame = V-Blank-IN (0x40) + V-Blank-OUT (0x41) at minimum; the
     * handler is shared, so the counter reflects all handled factors */
    T_CHECK(count >= 2);
    /* the master returned to its idle loop after interrupt handling */
    T_CHECK(s->msh2.pc >= 0x06002004u && s->msh2.pc <= 0x06002008u);
    sat_free(s);
}

/* ---- SMPC ------------------------------------------------------------------------ */

static void smpc_intback_reports_pad(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }

    /* released state */
    emu_core_supersaturn()->set_input(&s->base, 0);
    sat_bus_write8(s, 0x0010001Fu, 0x0Du); /* COMREG: INTBACK */
    T_CHECK_EQ(sat_bus_read8(s, 0x00100021u), 0xF1u); /* 1 peripheral    */
    T_CHECK_EQ(sat_bus_read8(s, 0x00100023u), 0x02u); /* digital pad     */
    T_CHECK_EQ(sat_bus_read8(s, 0x00100025u), 0x00u);
    T_CHECK_EQ(sat_bus_read8(s, 0x00100027u), 0xFFu); /* active-low data */
    T_CHECK_EQ(sat_bus_read8(s, 0x00100063u), 0u);    /* SF: done        */

    /* Start + A pressed: bits 3 and 2 of the first data byte go low */
    emu_core_supersaturn()->set_input(&s->base,
                                      SAT_BTN_START | SAT_BTN_A);
    sat_bus_write8(s, 0x0010001Fu, 0x0Du);
    T_CHECK_EQ(sat_bus_read8(s, 0x00100027u),
               (uint8_t)(0xFFu & ~(0x08u | 0x04u)));
    T_CHECK_EQ(sat_bus_read8(s, 0x00100029u), 0x8Fu);
    sat_free(s);
}

/* ---- VDP1 ------------------------------------------------------------------------ */

static void vdp1_polygon_draws(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }

    /* command table at VRAM 0: local coord (0,0), polygon quad */
    uint32_t t = 0u;
    w16(s->vdp1_vram, t + 0x00u, 0x000Au); /* local coordinate set       */
    w16(s->vdp1_vram, t + 0x0Cu, 0x0000u);
    w16(s->vdp1_vram, t + 0x0Eu, 0x0000u);
    t += 0x20u;
    w16(s->vdp1_vram, t + 0x00u, 0x0004u); /* polygon, jump next         */
    w16(s->vdp1_vram, t + 0x06u, 0x7FFFu); /* color: white RGB555        */
    w16(s->vdp1_vram, t + 0x0Cu, 10u);     /* A(10,10)                   */
    w16(s->vdp1_vram, t + 0x0Eu, 10u);
    w16(s->vdp1_vram, t + 0x10u, 60u);     /* B(60,10)                   */
    w16(s->vdp1_vram, t + 0x12u, 10u);
    w16(s->vdp1_vram, t + 0x14u, 60u);     /* C(60,50)                   */
    w16(s->vdp1_vram, t + 0x16u, 50u);
    w16(s->vdp1_vram, t + 0x18u, 10u);     /* D(10,50)                   */
    w16(s->vdp1_vram, t + 0x1Au, 50u);
    t += 0x20u;
    w16(s->vdp1_vram, t, 0x8000u); /* draw end                          */

    sat_vdp1_execute(s);
    /* EDSR bit 0: transfer end status (VDP1 manual table 2.1) */
    T_CHECK_EQ_U(s->vdp1_reg[SAT_VDP1_EDSR] & 1u, 1u);
    T_CHECK_EQ_U(s->vdp1_fb[0][30u * SAT_FB_W + 30u], 0x7FFFu);
    T_CHECK_EQ_U(s->vdp1_fb[0][12u * SAT_FB_W + 12u], 0x7FFFu);
    T_CHECK_EQ_U(s->vdp1_fb[0][5u * SAT_FB_W + 5u], 0u); /* outside quad */
    T_CHECK_EQ_U(s->vdp1_fb[0][55u * SAT_FB_W + 62u], 0u);
    sat_free(s);
}

static void vdp1_sprite_4bpp_bank(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }

    /* CRAM color bank entries (16-bit RGB555 words) */
    s->vdp2_cram[0x22u] = 0x7Cu; /* code 0x0011 -> red   */
    s->vdp2_cram[0x23u] = 0x00u;
    s->vdp2_cram[0x24u] = 0x03u; /* code 0x0012 -> green */
    s->vdp2_cram[0x25u] = 0xE0u;
    /* texture: 8x1 pixel, 4 bpp nibbles 0 1 2 F (F = end code)          */
    uint32_t tex = 0x100u;
    s->vdp1_vram[tex] = 0x01u;
    s->vdp1_vram[tex + 1u] = 0x2Fu;

    uint32_t t = 0u;
    w16(s->vdp1_vram, t + 0x00u, 0x000Au); /* local coord (0,0)          */
    t += 0x20u;
    w16(s->vdp1_vram, t + 0x00u, 0x0000u); /* normal sprite, jump next   */
    w16(s->vdp1_vram, t + 0x04u, 0x0000u); /* CMDPMOD: mode 0, no SPD/ECD*/
    w16(s->vdp1_vram, t + 0x06u, 0x0010u); /* color bank 0x0010          */
    w16(s->vdp1_vram, t + 0x08u, (uint16_t)(tex / 8u)); /* CMDSRCA = /8  */
    w16(s->vdp1_vram, t + 0x0Au, 0x0101u); /* 8x1 pixels                 */
    w16(s->vdp1_vram, t + 0x0Cu, 20u);     /* at (20,20)                 */
    w16(s->vdp1_vram, t + 0x0Eu, 20u);
    t += 0x20u;
    w16(s->vdp1_vram, t, 0x8000u);

    sat_vdp1_execute(s);
    T_CHECK_EQ_U(s->vdp1_fb[0][20u * SAT_FB_W + 20u], 0u);
    /* code 0x0010 -> CRAM entry 0 = 0x0000 = transparent (code 0) */
    T_CHECK_EQ_U(s->vdp1_fb[0][20u * SAT_FB_W + 21u],
                 0x7C00u); /* red                                        */
    T_CHECK_EQ_U(s->vdp1_fb[0][20u * SAT_FB_W + 22u],
                 0x03E0u); /* green                                      */
    T_CHECK_EQ_U(s->vdp1_fb[0][20u * SAT_FB_W + 23u],
                 0u); /* end code 0xF: transparent (ECD=0)                 */
    sat_free(s);
}

static void vdp1_user_clipping(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }

    uint32_t t = 0u;
    w16(s->vdp1_vram, t + 0x00u, 0x000Au); /* local coord (0,0)          */
    t += 0x20u;
    w16(s->vdp1_vram, t + 0x00u, 0x0008u); /* user clipping set          */
    w16(s->vdp1_vram, t + 0x0Cu, 16u);
    w16(s->vdp1_vram, t + 0x0Eu, 16u);
    w16(s->vdp1_vram, t + 0x10u, 31u);
    w16(s->vdp1_vram, t + 0x12u, 31u);
    t += 0x20u;
    /* polygon spanning the clip border with Cmod=1 (clip inside)        */
    w16(s->vdp1_vram, t + 0x00u, 0x0004u); /* polygon, jump next         */
    w16(s->vdp1_vram, t + 0x04u, 0x0200u); /* CMDPMOD: Cmod=1, inside    */
    w16(s->vdp1_vram, t + 0x06u, 0x7FFFu);
    w16(s->vdp1_vram, t + 0x0Cu, 0u);
    w16(s->vdp1_vram, t + 0x0Eu, 0u);
    w16(s->vdp1_vram, t + 0x10u, 40u);
    w16(s->vdp1_vram, t + 0x12u, 0u);
    w16(s->vdp1_vram, t + 0x14u, 40u);
    w16(s->vdp1_vram, t + 0x16u, 40u);
    w16(s->vdp1_vram, t + 0x18u, 0u);
    w16(s->vdp1_vram, t + 0x1Au, 40u);
    t += 0x20u;
    w16(s->vdp1_vram, t, 0x8000u);

    sat_vdp1_execute(s);
    T_CHECK_EQ_U(s->vdp1_fb[0][20u * SAT_FB_W + 20u], 0x7FFFu); /* inside */
    T_CHECK_EQ_U(s->vdp1_fb[0][35u * SAT_FB_W + 35u], 0u);      /* outside*/
    sat_free(s);
}

static void vdp1_erase_write_and_swap(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }

    /* draw a white polygon into buffer 0 (see polygon test) */
    w16(s->vdp1_vram, 0x00u, 0x000Au);
    w16(s->vdp1_vram, 0x20u, 0x0004u);
    w16(s->vdp1_vram, 0x26u, 0x7FFFu);
    w16(s->vdp1_vram, 0x2Cu, 0u);
    w16(s->vdp1_vram, 0x2Eu, 0u);
    w16(s->vdp1_vram, 0x30u, 8u);
    w16(s->vdp1_vram, 0x32u, 0u);
    w16(s->vdp1_vram, 0x34u, 8u);
    w16(s->vdp1_vram, 0x36u, 8u);
    w16(s->vdp1_vram, 0x38u, 0u);
    w16(s->vdp1_vram, 0x3Au, 8u);
    w16(s->vdp1_vram, 0x40u, 0x8000u);
    sat_vdp1_execute(s);

    /* arm V-blank erase with pattern 0x7C00 (red) over the full screen:
     * EWLR X1=0,Y1=0; EWRR X3=64 units*8=512, Y3=256                   */
    s->vdp1_reg[SAT_VDP1_TVMR] = SAT_TVMR_VBE;
    s->vdp1_reg[SAT_VDP1_EWDR] = 0x7C00u;
    s->vdp1_reg[SAT_VDP1_EWLR] = 0x0000u;
    s->vdp1_reg[SAT_VDP1_EWRR] = (uint16_t)((64u << 9) | 256u);

    sat_vdp1_frame_change(s);
    /* after the swap the drawing buffer is 1 and the erase landed there */
    T_CHECK_EQ(s->vdp1_draw_sel, 1);
    T_CHECK_EQ_U(s->vdp1_fb[1][100u * SAT_FB_W + 100u], 0x7C00u);
    /* rendered output shows buffer 0 (the polygon) */
    sat_render_output(s);
    T_CHECK_EQ_U(s->fb[4u * SAT_SCREEN_W + 4u], EMU_PIXEL(255, 255, 255));
    T_CHECK_EQ_U(s->fb[100u * SAT_SCREEN_W + 100u], EMU_PIXEL(0, 0, 0));
    sat_free(s);
}

/* ---- save states ------------------------------------------------------------------ */

static void state_roundtrip_resumes(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 0x00C0FFEEu, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }
    /* one VDP1 command so framebuffer state participates in the blob */
    w16(s->vdp1_vram, 0x20u, 0x0004u);
    w16(s->vdp1_vram, 0x26u, 0x03E0u);
    w16(s->vdp1_vram, 0x2Cu, 100u);
    w16(s->vdp1_vram, 0x2Eu, 100u);
    w16(s->vdp1_vram, 0x30u, 140u);
    w16(s->vdp1_vram, 0x32u, 100u);
    w16(s->vdp1_vram, 0x34u, 140u);
    w16(s->vdp1_vram, 0x36u, 140u);
    w16(s->vdp1_vram, 0x38u, 100u);
    w16(s->vdp1_vram, 0x3Au, 140u);
    w16(s->vdp1_vram, 0x40u, 0x8000u);
    s->vdp1_reg[SAT_VDP1_PTMR] = 2u; /* auto-run each frame */

    T_CHECK_EQ(emu_core_supersaturn()->run_frame(&s->base), EMU_OK);
    T_CHECK_EQ(emu_core_supersaturn()->run_frame(&s->base), EMU_OK);

    size_t cap = emu_core_supersaturn()->state_size(&s->base);
    T_CHECK(cap > 0);
    uint8_t *snap = malloc(cap);
    uint8_t *later = malloc(cap);
    uint8_t *resumed = malloc(cap);
    if (snap == NULL || later == NULL || resumed == NULL) {
        T_FAIL("alloc failed");
        free(snap);
        free(later);
        free(resumed);
        sat_free(s);
        return;
    }
    T_CHECK_EQ(emu_core_supersaturn()->save_state(&s->base, snap, cap),
               EMU_OK);

    T_CHECK_EQ(emu_core_supersaturn()->run_frame(&s->base), EMU_OK);
    T_CHECK_EQ(emu_core_supersaturn()->save_state(&s->base, later, cap),
               EMU_OK);
    T_CHECK_EQ(emu_core_supersaturn()->load_state(&s->base, snap, cap),
               EMU_OK);
    T_CHECK_EQ(emu_core_supersaturn()->save_state(&s->base, resumed, cap),
               EMU_OK);
    T_CHECK(memcmp(snap, resumed, cap) == 0);
    sat_free(s);

    /* two fresh cores running identical programs from identical states
     * must stay bit-identical */
    struct saturn *a = NULL;
    struct saturn *b = NULL;
    int ok2 = sat_boot(&a, 0x00C0FFEEu, 0x0600A000u) &&
              sat_boot(&b, 0x00C0FFEEu, 0x0600A000u);
    T_CHECK(ok2);
    if (ok2) {
        T_CHECK_EQ(emu_core_supersaturn()->load_state(&a->base, snap, cap),
                   EMU_OK);
        T_CHECK_EQ(emu_core_supersaturn()->load_state(&b->base, snap, cap),
                   EMU_OK);
        T_CHECK_EQ(emu_core_supersaturn()->run_frame(&a->base), EMU_OK);
        T_CHECK_EQ(emu_core_supersaturn()->run_frame(&b->base), EMU_OK);
        size_t cap2 = emu_core_supersaturn()->state_size(&a->base);
        uint8_t *sa = malloc(cap2);
        uint8_t *sb = malloc(cap2);
        int ok3 = (sa != NULL && sb != NULL);
        T_CHECK(ok3);
        if (ok3) {
            T_CHECK_EQ(emu_core_supersaturn()->save_state(&a->base, sa, cap2),
                       EMU_OK);
            T_CHECK_EQ(emu_core_supersaturn()->save_state(&b->base, sb, cap2),
                       EMU_OK);
            T_CHECK(memcmp(sa, sb, cap2) == 0);
        }
        free(sa);
        free(sb);
    }
    sat_free(a);
    sat_free(b);
    free(snap);
    free(later);
    free(resumed);
}

static void state_rejects_corrupt(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }
    size_t cap = emu_core_supersaturn()->state_size(&s->base);
    uint8_t *buf = malloc(cap);
    if (buf == NULL) {
        T_FAIL("alloc failed");
        free(buf);
        sat_free(s);
        return;
    }
    T_CHECK_EQ(emu_core_supersaturn()->save_state(&s->base, buf, cap),
               EMU_OK);
    T_CHECK_EQ(emu_core_supersaturn()->load_state(&s->base, buf, cap - 1u),
               EMU_EBADSTATE);
    buf[0] ^= 0xFFu; /* magic broken */
    T_CHECK_EQ(emu_core_supersaturn()->load_state(&s->base, buf, cap),
               EMU_EBADSTATE);
    free(buf);
    sat_free(s);
}

/* ---- video output ---------------------------------------------------------------- */

static void framebuffer_reports_dimensions(void)
{
    struct saturn *s = NULL;
    if (!sat_boot(&s, 1u, 0x0600A000u)) {
        T_FAIL("boot failed");
        return;
    }
    uint32_t w = 0, h = 0;
    const uint32_t *fb =
        emu_core_supersaturn()->framebuffer(&s->base, &w, &h);
    T_CHECK(fb != NULL);
    T_CHECK_EQ_U(w, SAT_SCREEN_W);
    T_CHECK_EQ_U(h, SAT_SCREEN_H);
    T_CHECK_EQ(emu_core_supersaturn()->run_frame(&s->base), EMU_OK);
    sat_free(s);
}

T_SUITE_BEGIN(saturn)
{ "lifecycle_rejects_bad_images", lifecycle_rejects_bad_images },
{ "boot_executes_marker_program", boot_executes_marker_program },
{ "sinit_starts_slave", sinit_starts_slave },
{ "scu_dma_direct_copies", scu_dma_direct_copies },
{ "scu_dma_indirect_list", scu_dma_indirect_list },
{ "scu_interrupts_fire", scu_interrupts_fire },
{ "smpc_intback_reports_pad", smpc_intback_reports_pad },
{ "vdp1_polygon_draws", vdp1_polygon_draws },
{ "vdp1_sprite_4bpp_bank", vdp1_sprite_4bpp_bank },
{ "vdp1_user_clipping", vdp1_user_clipping },
{ "vdp1_erase_write_and_swap", vdp1_erase_write_and_swap },
{ "state_roundtrip_resumes", state_roundtrip_resumes },
{ "state_rejects_corrupt", state_rejects_corrupt },
{ "framebuffer_reports_dimensions", framebuffer_reports_dimensions },
T_SUITE_END
T_SUITE_REG(saturn)
