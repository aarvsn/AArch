/*
 * supercastpro: Sega Dreamcast core tests.
 *
 * Expected values derive from the SH-4 architecture and the documented
 * core model in src/supercastpro/dc.h (hand-assembled opcodes) - never
 * from emulator internals.
 */
#include "../tests.h"
#include "../../src/supercastpro/dc.h"

#include <stdlib.h>
#include <string.h>

#include "../../src/common/util.h"

static void w16(uint8_t *p, uint32_t off, uint16_t v)
{
    p[off] = (uint8_t)v; /* SH-4 code is little-endian */
    p[off + 1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
}

/* ---- standalone SH-4 harness (flat memory) ----------------------------------------- */

struct sh4_mem {
    uint8_t mem[0x1000];
};

static uint8_t h_rb(void *u, uint32_t a) { struct sh4_mem *m = u; return m->mem[a & 0xFFFu]; }
static uint16_t h_rh(void *u, uint32_t a)
{
    struct sh4_mem *m = u;
    uint16_t v;
    memcpy(&v, &m->mem[a & 0xFFFu], 2);
    return v;
}
static uint32_t h_rw(void *u, uint32_t a)
{
    struct sh4_mem *m = u;
    uint32_t v;
    memcpy(&v, &m->mem[a & 0xFFFu], 4);
    return v;
}
static void h_wb(void *u, uint32_t a, uint8_t v)
{
    struct sh4_mem *m = u;
    m->mem[a & 0xFFFu] = v;
}
static void h_wh(void *u, uint32_t a, uint16_t v)
{
    struct sh4_mem *m = u;
    memcpy(&m->mem[a & 0xFFFu], &v, 2);
}
static void h_ww(void *u, uint32_t a, uint32_t v)
{
    struct sh4_mem *m = u;
    memcpy(&m->mem[a & 0xFFFu], &v, 4);
}

static sh4_bus_t h_bus = { NULL, h_rb, h_rh, h_rw, h_wb, h_wh, h_ww };
static struct sh4_mem g_mem;

static void sh4_load_prog(sh4_t *c, const uint16_t *prog, size_t words)
{
    memset(&g_mem, 0, sizeof g_mem);
    h_bus.user = &g_mem;
    sh4_init(c, &h_bus);
    sh4_reset(c);
    c->pc = 0x100;
    c->next_pc = c->pc + 2u;
    c->sr = SH4_SR_MD;
    for (size_t i = 0; i < words; i++)
        w16(g_mem.mem, 0x100u + (uint32_t)i * 2u, prog[i]);
}

/* ---- disc image builder ------------------------------------------------------------- */

static uint8_t *dc_make_disc(const uint16_t *prog, size_t words,
                             size_t *size_out)
{
    size_t boot_off = 0x10000u; /* sector 32 */
    size_t total = boot_off + words * 2u;
    uint8_t *img = calloc(1, total);
    if (img == NULL)
        return NULL;
    memcpy(img, "SEGA SEGAKATANA", 15);
    emu_store_le32(img + 0x300u, (uint32_t)(boot_off / 2048u));
    emu_store_le32(img + 0x308u, (uint32_t)(words * 2u));
    for (size_t i = 0; i < words; i++) {
        img[boot_off + i * 2u] = (uint8_t)prog[i];
        img[boot_off + i * 2u + 1u] = (uint8_t)(prog[i] >> 8);
    }
    *size_out = total;
    return img;
}

static struct dc *dc_boot_prog(const uint16_t *prog, size_t words)
{
    size_t size = 0;
    uint8_t *img = dc_make_disc(prog, words, &size);
    if (img == NULL)
        return NULL;
    emu_core_t *c = NULL;
    if (emu_core_supercastpro()->create(&c) != EMU_OK) {
        free(img);
        return NULL;
    }
    if (emu_core_supercastpro()->load_rom(c, img, size) != EMU_OK) {
        free(img);
        emu_core_supercastpro()->destroy(c);
        return NULL;
    }
    free(img);
    return (struct dc *)c;
}

static void dc_free(struct dc *d)
{
    emu_core_supercastpro()->destroy(&d->base);
}

/* ---- CPU tests (standalone harness) -------------------------------------------------- */

static void sh4_basic_alu(void)
{
    /* mov #5,r1; mov #3,r2; add r2,r1 -> 8 */
    sh4_t c;
    static const uint16_t prog[] = { 0xE105u, 0xE203u, 0x312Cu, 0x0009u };
    sh4_load_prog(&c, prog, 4);
    sh4_step(&c);
    sh4_step(&c);
    sh4_step(&c);
    T_CHECK_EQ_U(c.r[1], 8u);
    /* sub: mov #8,r1; mov #10,r3; sub r1,r3 -> 2 (SUB: Rn = Rn - Rm) */
    static const uint16_t prog2[] = {
        0xE108u,             /* mov #8,r1    */
        0xE30Au,             /* mov #10,r3   */
        0x3318u,             /* sub r1,r3    */
        0x0009u,
    };
    sh4_load_prog(&c, prog2, 4);
    sh4_step(&c);
    sh4_step(&c);
    sh4_step(&c);
    T_CHECK_EQ_U(c.r[3], 2u);
}

static void sh4_muls_and_mac(void)
{
    /* dmuls.l r1,r2 with r1 = -3, r2 = 5 -> -15 */
    sh4_t c;
    static const uint16_t prog[] = {
        0xE1FDu, /* mov #-3,r1   */
        0xE205u, /* mov #5,r2    */
        0x321Du, /* dmuls.l r1,r2 */
        0x0A0Au, /* sts macl,r10?? -> use 0x001A|n<<8: sts macl,r0 = 0x001A */
        0x0009u,
    };
    (void)prog;
    static const uint16_t prog2[] = {
        0xE1FDu, /* mov #-3,r1    */
        0xE205u, /* mov #5,r2     */
        0x321Du, /* dmuls.l r1,r2 */
        0x001Au, /* sts macl,r0   */
        0x0009u,
    };
    sh4_load_prog(&c, prog2, 5);
    for (int i = 0; i < 4; i++)
        sh4_step(&c);
    T_CHECK_EQ_U(c.r[0], (uint32_t)-15);
    T_CHECK_EQ_U(c.mach, 0xFFFFFFFFu); /* high word of -15 */
    T_CHECK_EQ_U(c.macl, (uint32_t)-15);
}

static void sh4_fpu_math(void)
{
    /* lds r1,fpul; float fpul,fr10; fadd fr11,fr10; ftrc fr10,fpul;
     * sts fpul,r2 -- with r1 = 2 and fr11 = 3.5 -> 5.5 -> 5 */
    sh4_t c;
    static const uint16_t prog[] = {
        0xE102u,             /* mov #2,r1            */
        0x415Au,             /* lds r1,fpul          */
        0xFA2Du,             /* float fpul,fr10      */
        0xFB9Du,             /* fldi1 fr11           */
        0xF9ADu,             /* fldi0?? no: fldi0 = 8D; want fr11=3.5 */
        0x0009u,
    };
    (void)prog;
    /* simpler: fr10 = float(2), fr11 = 1 (fldi1), fadd -> 3.0, ftrc -> 3 */
    static const uint16_t prog3[] = {
        0xE102u, /* mov #2,r1        */
        0x415Au, /* lds r1,fpul      */
        0xFA2Du, /* float fpul,fr10  */
        0xFB9Du, /* fldi1 fr11       */
        0xFAB0u, /* fadd fr11,fr10   */
        0xFA3Du, /* ftrc fr10,fpul   */
        0x025Au, /* sts fpul,r2      */
        0x0009u,
    };
    sh4_load_prog(&c, prog3, 8);
    for (int i = 0; i < 7; i++)
        sh4_step(&c);
    T_CHECK_EQ_U(c.r[2], 3u);
}

static void sh4_delay_slot_branch(void)
{
    /* bra skip; nop(delay); mov #7,r1; skip: nop
     * delay slot executes; mov skipped */
    sh4_t c;
    static const uint16_t prog[] = {
        0xA003u, /* bra +3 (skip one instruction) */
        0xE106u, /* delay: mov #6,r1 (executes)   */
        0xE107u, /* skipped: mov #7,r1            */
        0x0009u, /* skip target: nop              */
    };
    sh4_load_prog(&c, prog, 4);
    sh4_step(&c); /* bra + delay slot */
    sh4_step(&c); /* delay slot */
    T_CHECK_EQ_U(c.r[1], 6u);
    T_CHECK_EQ_U(c.pc, 0x10Au); /* skip target: 0x100 + 4 + 2*2 */
}

static void sh4_banked_registers(void)
{
    /* r1 = 0x55 in bank 0; switch RB via ldc sr; r1 = 0xAA in bank 1;
     * switch back; bank 0 still holds 0x55.
     * SR literals (RB|MD = 0x40002000, MD = 0x40000000) load via
     * MOV.L @(disp,PC) since MOV #imm is sign-extended byte only. */
    sh4_t c;
    static const uint16_t prog[] = {
        0xD203u, /* 0x100: mov.l @(3,pc),r2  -> literal 0x110   */
        0xE155u, /* 0x102: mov #0x55,r1                        */
        0x420Eu, /* 0x104: ldc r2,sr (RB=1: bank 1)            */
        0xE12Au, /* 0x106: mov #0x2A,r1 (bank 1; 0x2A: byte imm) */
        0x6013u, /* 0x108: mov r1,r0 -> 0x2A (r0 = r1)        */
        0xD202u, /* 0x10A: mov.l @(2,pc),r2  -> literal 0x114  */
        0x420Eu, /* 0x10C: ldc r2,sr (bank 0)                  */
        0x0009u, /* 0x10E: nop                                 */
    };
    sh4_load_prog(&c, prog, 8);
    w32(g_mem.mem, 0x110u, 0x40002000u); /* RB|MD */
    w32(g_mem.mem, 0x114u, 0x40000000u); /* MD    */
    for (int i = 0; i < 5; i++)
        sh4_step(&c);
    T_CHECK_EQ_U(c.r[0], 0x2Au); /* read in bank 1 */
    T_CHECK_EQ_U(c.r[1], 0x2Au); /* bank 1 view    */
    sh4_step(&c);
    sh4_step(&c);
    T_CHECK_EQ_U(c.r[1], 0x55u); /* bank 0 restored */
}

/* ---- machine tests ------------------------------------------------------------------ */

static void lifecycle_rejects_bad_images(void)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(emu_core_supercastpro()->create(&c), EMU_OK);
    T_CHECK_EQ(emu_core_supercastpro()->load_rom(c, NULL, 10), EMU_EINVAL);
    static const uint8_t bad[0x9300] = { 0 };
    T_CHECK_EQ(emu_core_supercastpro()->load_rom(c, bad, sizeof bad),
               EMU_EBADROM);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(c), EMU_ENOROM);
    emu_core_supercastpro()->destroy(c);
}

static void boot_model_loads_and_runs(void)
{
    /* program at 0x8C010000:
     *   mov.l @(2,pc),r0   ; r0 = 0x8C020000 (literal)
     *   mov #0x44,r1
     *   mov.l r1,@r0       ; write marker
     *   bra self / nop
     *   .long 0x8C020000   (at pc+4+8 = boot+0x10)
     */
    static const uint16_t prog[] = {
        0xD002u, /* mov.l @(2,pc),r0 */
        0xE144u, /* mov #0x44,r1     */
        0x2012u, /* mov.l r1,@r0     */
        0xAFFEu, /* bra self         */
        0x0009u, /* delay: nop       */
    };
    struct dc *d = dc_boot_prog(prog, 5);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* place the literal where MOV.L @(disp,PC) expects it: 0x8C01000C */
    w32(d->ram, 0x0001000Cu, 0x8C020000u);
    T_CHECK_EQ(emu_core_supercastpro()->run_frame(&d->base), EMU_OK);
    /* boot model: PC entered at 0x8C010000, BL|MD set */
    /* self-loop bounces between the BRA (0x8C010006) and its slot */
    T_CHECK((d->cpu.pc & ~1u) >= 0x8C010006u && (d->cpu.pc & ~1u) <= 0x8C010008u);
    T_CHECK((d->cpu.sr & SH4_SR_BL) != 0);
    T_CHECK_EQ_U(dc_read32(d, 0x8C020000u), 0x44u);
    dc_free(d);
}

static void tmu_underflow_interrupts(void)
{
    static const uint16_t prog[] = { 0xAFFEu, 0x0009u }; /* self loop */
    struct dc *d = dc_boot_prog(prog, 2);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* vector table: VBR = 0x8C020000; TUNI0 handler at 0x8C020500
     * (RAM offsets: address - 0x8C000000). The test clears SR.BL/I
     * (the reset state masks everything) like real boot code would. */
    d->cpu.vbr = 0x8C020000u;
    d->cpu.sr = SH4_SR_MD;
    w32(d->ram, 0x00020400u, 0x8C020500u); /* VBR+0x400 vector */
    /* handler: mov.l r1,@r13 ; rte (r13/r1 preset by the test) */
    w16(d->ram, 0x00020500u, 0x2D12u); /* mov.l r1,@r13 */
    w16(d->ram, 0x00020502u, 0x002Bu); /* rte */
    w16(d->ram, 0x00020504u, 0x0009u); /* nop (delay slot) */
    d->cpu.r[1] = 0x77u;
    d->cpu.r[13] = 0x8C030000u;
    /* TMU0: TCOR = 100, TCNT = 100, TCR = UNIE|P/4, TSTR bit0 */
    dc_write32(d, DC_TMU_BASE + 0x008u, 100u); /* TCOR0 */
    dc_write32(d, DC_TMU_BASE + 0x00Cu, 100u); /* TCNT0 */
    dc_write32(d, DC_TMU_BASE + 0x010u, 0x0040u); /* TCR0: UNIE */
    dc_write32(d, DC_TMU_BASE + 0x004u, 1u); /* TSTR: ch0 on */
    for (int i = 0; i < 4000; i++)
        dc_step(d);
    T_CHECK((dc_read32(d, DC_TMU_BASE + 0x010u) & 0x20u) != 0); /* UNF */
    T_CHECK_EQ_U(dc_read32(d, 0x8C030000u), 0x77u); /* handler ran */
    dc_free(d);
}

static void pvr_rgb565_scanout(void)
{
    static const uint16_t prog[] = { 0xAFFEu, 0x0009u };
    struct dc *d = dc_boot_prog(prog, 2);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* framebuffer config: RGB565, 640x480, VRAM offset 0 */
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_R_CTRL, 0x00000000u);
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_R_SIZE,
               ((480u - 1u) << 10) | (640u / 4u - 1u));
    dc_write32(d, DC_PVR_BASE + DC_PVR_FB_R_SOF1, 0u);
    /* paint first pixel RGB565 green (0x07E0) and a white pixel */
    w16(d->vram, 0, 0x07E0u);
    w16(d->vram, 2, 0xFFFFu);
    dc_render(d);
    T_CHECK_EQ_U(d->fb[0], EMU_PIXEL(0, 255, 0));
    T_CHECK_EQ_U(d->fb[1], EMU_PIXEL(255, 255, 255));
    T_CHECK_EQ_U(d->fb[2], 0xFF000000u); /* untouched -> black */
    /* ID registers */
    T_CHECK_EQ_U(dc_read32(d, DC_PVR_BASE + 0x008u), 0x1Du);
    dc_free(d);
}

static void state_roundtrip_resumes(void)
{
    /* mov #5,r1; add #3,r1; self loop */
    static const uint16_t prog[] = {
        0xE105u, /* mov #5,r1     */
        0x7103u, /* add #3,r1     */
        0xAFFEu, /* bra self      */
        0x0009u,
    };
    struct dc *ref = dc_boot_prog(prog, 4);
    struct dc *cmp = dc_boot_prog(prog, 4);
    if (ref == NULL || cmp == NULL) {
        T_FAIL("boot failed");
        return;
    }
    dc_step(ref);
    dc_step(ref);
    dc_step(cmp);
    size_t sz = emu_core_supercastpro()->state_size(&ref->base);
    uint8_t *blob = malloc(sz);
    T_CHECK(blob != NULL);
    if (blob == NULL) {
        dc_free(ref);
        dc_free(cmp);
        return;
    }
    T_CHECK_EQ(emu_core_supercastpro()->save_state(&ref->base, blob, sz),
               EMU_OK);
    T_CHECK_EQ(emu_core_supercastpro()->load_state(&cmp->base, blob, sz),
               EMU_OK);
    free(blob);
    dc_step(ref);
    dc_step(cmp);
    T_CHECK_EQ_U(ref->cpu.pc, cmp->cpu.pc);
    T_CHECK_EQ_U(ref->cpu.r[1], cmp->cpu.r[1]);
    T_CHECK_EQ_U(ref->cpu.sr, cmp->cpu.sr);
    uint8_t tiny[8];
    T_CHECK_EQ(emu_core_supercastpro()->save_state(&ref->base, tiny,
                                                   sizeof tiny),
               EMU_ENOSPACE);
    dc_free(ref);
    dc_free(cmp);
}

static void address_windows_decode(void)
{
    static const uint16_t prog[] = { 0xAFFEu, 0x0009u };
    struct dc *d = dc_boot_prog(prog, 2);
    if (d == NULL) {
        T_FAIL("boot failed");
        return;
    }
    /* P1/P2/P0 windows hit the same physical RAM */
    dc_write32(d, 0x8C040000u, 0x11111111u); /* P1 */
    T_CHECK_EQ_U(dc_read32(d, 0xAC040000u), 0x11111111u); /* P2 */
    T_CHECK_EQ_U(dc_read32(d, 0x0C040000u), 0x11111111u); /* phys */
    /* VRAM via P2 0xA5000000 */
    dc_write32(d, 0xA5000004u, 0x22222222u);
    T_CHECK_EQ_U(dc_read32(d, 0x05000004u), 0x22222222u);
    /* URAM */
    dc_write32(d, 0x7C000000u, 0x33333333u);
    T_CHECK_EQ_U(dc_read32(d, 0xF4000000u), 0x33333333u);
    dc_free(d);
}

T_SUITE_BEGIN(dc)
{ "lifecycle_rejects_bad_images", lifecycle_rejects_bad_images },
{ "sh4_basic_alu", sh4_basic_alu },
{ "sh4_muls_and_mac", sh4_muls_and_mac },
{ "sh4_fpu_math", sh4_fpu_math },
{ "sh4_delay_slot_branch", sh4_delay_slot_branch },
{ "sh4_banked_registers", sh4_banked_registers },
{ "boot_model_loads_and_runs", boot_model_loads_and_runs },
{ "tmu_underflow_interrupts", tmu_underflow_interrupts },
{ "pvr_rgb565_scanout", pvr_rgb565_scanout },
{ "state_roundtrip_resumes", state_roundtrip_resumes },
{ "address_windows_decode", address_windows_decode },
T_SUITE_END
T_SUITE_REG(dc)
