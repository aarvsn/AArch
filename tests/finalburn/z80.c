/*
 * finalburn Z80 tests: register-level instruction checks with programs
 * loaded into Z80 RAM. Encodings hand-assembled from the Zilog manual.
 */
#include "tests.h"
#include "fbh.h"
#include "finalburn/fb_md.h"
#include "emu/emu.h"

#include <stdlib.h>
#include <string.h>

static struct fb_md *z80_boot(void)
{
    return fb_create_bare();
}

static void test_ld_add(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    static const uint8_t prog[] = { 0x3E, 0x05, 0xC6, 0x03, 0x76 }; /* LD A,5; ADD A,3; HALT */
    memcpy(md->zram, prog, sizeof prog);
    z->pc = 0;
    fb_z80_run(md, 100);
    T_CHECK_EQ_U(z->af >> 8, 0x08);
    T_CHECK_EQ_U(z->af & 0x40u, 0); /* Z clear */
    T_CHECK_EQ_U(z->halted, 1);
    T_CHECK_EQ_U(z->pc, 5);         /* one past the HALT byte */
    emu_core_destroy(&md->base);
}

static void test_add_overflow_flag(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    /* 0x7F + 0x01 = 0x80: S and PV set */
    static const uint8_t prog[] = { 0x3E, 0x7F, 0xC6, 0x01, 0x76 };
    memcpy(md->zram, prog, sizeof prog);
    z->pc = 0;
    fb_z80_run(md, 100);
    T_CHECK_EQ_U(z->af >> 8, 0x80);
    T_CHECK_EQ_U(z->af & 0x04u, 0x04u); /* PV */
    T_CHECK_EQ_U(z->af & 0x80u, 0x80u); /* S */
    T_CHECK_EQ_U(z->halted, 1);
    emu_core_destroy(&md->base);
}

static void test_inc_hl(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    static const uint8_t prog[] = { 0x21, 0xFF, 0x0F, 0x23, 0x76 }; /* LD HL,$0FFF; INC HL; HALT */
    memcpy(md->zram, prog, sizeof prog);
    z->pc = 0;
    fb_z80_run(md, 100);
    T_CHECK_EQ_U(z->hl, 0x1000);
    T_CHECK_EQ_U(z->halted, 1);
    emu_core_destroy(&md->base);
}

static void test_ldir(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    static const uint8_t prog[] = {
        0x21, 0x00, 0x10, /* LD HL,$1000 */
        0x11, 0x00, 0x11, /* LD DE,$1100 */
        0x01, 0x04, 0x00, /* LD BC,4 */
        0xED, 0xB0,       /* LDIR */
        0x76,             /* HALT */
    };
    memcpy(md->zram, prog, sizeof prog);
    for (int i = 0; i < 4; i++)
        md->zram[0x1000 + i] = (uint8_t)(0xA0 + i);
    z->pc = 0;
    fb_z80_run(md, 400);
    T_CHECK_EQ_U(md->zram[0x1100], 0xA0);
    T_CHECK_EQ_U(md->zram[0x1103], 0xA3);
    T_CHECK_EQ_U(z->bc, 0);
    T_CHECK_EQ_U(z->halted, 1);
    T_CHECK_EQ_U(z->pc, 12); /* one past the HALT */
    emu_core_destroy(&md->base);
}

static void test_djnz(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    /* $00: LD B,3 ; $02: DJNZ -2 (loop on itself at $02) ; $04: done */
    static const uint8_t prog[] = { 0x06, 0x03, 0x10, 0xFE, 0x76 }; /* LD B,3; DJNZ -2; HALT */
    memcpy(md->zram, prog, sizeof prog);
    z->pc = 0;
    fb_z80_run(md, 200);
    T_CHECK_EQ_U(z->bc >> 8, 0);
    T_CHECK_EQ_U(z->pc, 5); /* stopped after the HALT */
    emu_core_destroy(&md->base);
}

static void test_push_pop(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    static const uint8_t prog[] = { 0x21, 0x34, 0x12, 0xE5, 0x21, 0x00, 0x00,
                                    0xE1, 0x76 }; /* ... POP HL; HALT */
    memcpy(md->zram, prog, sizeof prog);
    z->sp = 0x1FF0; /* stack inside Z80 RAM */
    z->pc = 0;
    fb_z80_run(md, 200);
    T_CHECK_EQ_U(z->hl, 0x1234);
    T_CHECK_EQ_U(z->halted, 1);
    emu_core_destroy(&md->base);
}

static void test_im_and_ei(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    static const uint8_t prog[] = { 0xED, 0x5E, 0xFB, 0x76 }; /* IM 2; EI; HALT */
    memcpy(md->zram, prog, sizeof prog);
    z->pc = 0;
    fb_z80_run(md, 100);
    T_CHECK_EQ_U(z->im, 2);
    T_CHECK_EQ_U(z->iff1, 1);
    T_CHECK_EQ_U(z->halted, 1);
    emu_core_destroy(&md->base);
}

static void test_im1_interrupt(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    z->im = 1;
    z->iff1 = 1;
    z->pc = 0x100;
    uint16_t ret = 0x100;
    z->sp = 0x1FF0;
    md->zram[0x38] = 0x76; /* HALT at the IM1 vector */
    z->int_pending = 1;
    fb_z80_run(md, 60);
    T_CHECK_EQ_U(z->pc, 0x39);
    T_CHECK_EQ_U(z->halted, 1);
    T_CHECK_EQ_U(z->iff1, 0);
    /* return address pushed: lo at [sp], hi at [sp+1] */
    uint16_t pushed = (uint16_t)(md->zram[z->sp & 0x1FFF] |
                                 (md->zram[(z->sp + 1) & 0x1FFF] << 8));
    T_CHECK_EQ_U(pushed, ret);
    emu_core_destroy(&md->base);
}

static void test_indexed_ld(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    /* LD IX,$0100 ; LD B,(IX+3) with zram[0x103]=0x5A */
    static const uint8_t prog[] = { 0xDD, 0x21, 0x00, 0x01, 0xDD, 0x46, 0x03 };
    memcpy(md->zram, prog, sizeof prog);
    md->zram[0x103] = 0x5A;
    z->pc = 0;
    fb_z80_run(md, 200);
    T_CHECK_EQ_U(z->ix, 0x0100);
    T_CHECK_EQ_U(z->bc >> 8, 0x5A);
    emu_core_destroy(&md->base);
}

static void test_cpl_daa(void)
{
    struct fb_md *md = z80_boot();
    if (!md) { T_FAIL("create failed"); return; }
    struct fb_z80 *z = &md->z80;
    /* BCD 0x45 + 0x38 via binary add + DAA = 0x83, carry 0 */
    static const uint8_t prog[] = { 0x3E, 0x45, 0xC6, 0x38, 0x27 }; /* ... DAA */
    memcpy(md->zram, prog, sizeof prog);
    z->pc = 0;
    fb_z80_run(md, 200);
    T_CHECK_EQ_U(z->af >> 8, 0x83);
    emu_core_destroy(&md->base);
}

T_SUITE_BEGIN(finalburn_z80)
{ "ld_add", test_ld_add },
{ "add_overflow_flags", test_add_overflow_flag },
{ "inc_hl_16bit", test_inc_hl },
{ "ldir_block", test_ldir },
{ "djnz_loop", test_djnz },
{ "push_pop", test_push_pop },
{ "im2_ei", test_im_and_ei },
{ "im1_interrupt", test_im1_interrupt },
{ "dd_indexed_ld", test_indexed_ld },
{ "daa_bcd", test_cpl_daa },
T_SUITE_END
T_SUITE_REG(finalburn_z80)
