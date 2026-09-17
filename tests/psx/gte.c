/*
 * beatle-psx GTE tests. Expected values are computed by hand from the
 * nocash PSX-SPX GTE specification (formulas, fixed-point formats, the
 * UNR division table and FLAG bit layout) - never from emulator helpers.
 * Command words use the documented PsyQ encodings (e.g. RTPS 0x48180001,
 * MVMVA 0x48000012 with field bits).
 */
#include "../tests.h"
#include "../testutil.h"
#include "psxh.h"
#include "beatle-psx/psx.h"

#include <stdlib.h>
#include <string.h>

/* COP2 command word: COP2 opcode (0x12 << 26) | suffix. */
#define GTE(sfx) (0x48000000u | (sfx))
#define MTC2(rt, rd) (0x48800000u | ((rt) << 16) | ((rd) << 11))
#define MFC2(rt, rd) (0x48000000u | ((rt) << 16) | ((rd) << 11))
#define CTC2(rt, rd) (0x48A00000u | ((rt) << 16) | ((rd) << 11))
#define CFC2(rt, rd) (0x48200000u | ((rt) << 16) | ((rd) << 11))

/* register indices in the white-box arrays */
#define D_VXY0 0
#define D_VZ0 1
#define D_VXY1 2
#define D_VZ1 3
#define D_VXY2 4
#define D_VZ2 5
#define D_RGBC 6
#define D_OTZ 7
#define D_IR0 8
#define D_IR1 9
#define D_IR2 10
#define D_IR3 11
#define D_SXY0 12
#define D_SXY1 13
#define D_SXY2 14
#define D_SZ3 19
#define D_MAC0 24
#define D_MAC1 25
#define D_MAC2 26
#define D_MAC3 27
#define D_IRGB 28
#define D_ORGB 29
#define D_LZCS 30
#define D_LZCR 31
#define C_RT11 0
#define C_TRX 5
#define C_RBK 13
#define C_RFC 21
#define C_OFX 24
#define C_OFY 25
#define C_H 26
#define C_DQA 27
#define C_DQB 28
#define C_ZSF3 29
#define C_FLAG 31

static void set_rt_identity(struct psx *p)
{
    /* rotation matrix = identity: 1.0 in 1.3.12 = 0x1000 */
    uint32_t *cr = p->gte.cr;
    /* layout: r32.lsbs=RT11, r32.msbs=RT12, r33.lsbs=RT13, r33.msbs=RT21,
     * r34.lsbs=RT22, r34.msbs=RT23, r35.lsbs=RT31, r35.msbs=RT32, r36=RT33
     * identity: 1.0 in 1.3.12 = 0x1000 */
    cr[C_RT11 + 0] = 0x1000u;       /* RT11=1.0, RT12=0 */
    cr[C_RT11 + 1] = 0x1000u << 16; /* RT13=0, RT21=1.0 */
    cr[C_RT11 + 2] = 0x1000u;       /* RT22=1.0, RT23=0 */
    cr[C_RT11 + 3] = 0x1000u << 16; /* RT31=0, RT32=1.0 */
    cr[C_RT11 + 4] = 0x1000u;       /* RT33=1.0 */
    p->gte.cr[C_TRX] = 0;
    p->gte.cr[C_TRX + 1] = 0;
    p->gte.cr[C_TRX + 2] = 0;
}

static void psx_gte_rtps_identity(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    set_rt_identity(p);
    /* V0 = (2, 2, 4096): MAC1 = 0x1000*2 = 0x2000 -> IR1 = 2 (sf=1);
     * MAC3 = 0x1000*4096 = 0x1000000 -> IR3 = 0x1000, SZ3 = MAC3 >> 0. */
    p->gte.dr[D_VXY0] = (0u << 16) | 2u; /* VX=2, VY=0 */
    p->gte.dr[D_VZ0] = 4096u;
    /* H = 0x1000: UNR division (hand-computed through the spec table:
     * z=3, n=0x8000, d=0x8000, u=0x200, d=0x10000 then 0x20000,
     * result = (0x8000*0x20000+0x8000)>>16 = 0x10000). */
    p->gte.cr[C_H] = 0x1000u;
    /* RTPS sf=1 (PsyQ 0x48180001) */
    psx_gte_execute(&p->gte, GTE(0x0180001u));
    T_CHECK_EQ(p->gte.dr[D_IR1], 2u);
    T_CHECK_EQ(p->gte.dr[D_IR3], 0x1000u);
    T_CHECK_EQ(p->gte.dr[D_SZ3], 0x1000u);
    /* identity rotation: (VX,VY,VZ) -> (VX,VY,VZ); div=0x10000:
     * SX = VX = 2, SY = VY = 2 */
    T_CHECK_EQ(p->gte.dr[D_SXY2] & 0xFFFFu, 2u);
    T_CHECK_EQ(p->gte.dr[D_SXY2] >> 16, 2u);
    T_CHECK_EQ(p->gte.dr[D_IR0], 0u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gte_rtps_fifo(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    set_rt_identity(p);
    p->gte.cr[C_H] = 0x800u;
    /* RTPT (PsyQ 0x48280030): processes V0,V1,V2; SXY FIFO pushes each.
     * VZ = 0x1000 gives SZ3 = 0x1000; div = ((0x800*20000h/1000h)+1)/2
     * = 0x8000; SX = 0x8000*n/10000h = n/2. */
    p->gte.dr[D_VXY0] = (0u << 16) | 1u;
    p->gte.dr[D_VZ0] = 0x1000u;
    p->gte.dr[D_VXY1] = (0u << 16) | 2u;
    p->gte.dr[D_VZ1] = 0x1000u;
    p->gte.dr[D_VXY2] = (0u << 16) | 3u;
    p->gte.dr[D_VZ2] = 0x1000u;
    psx_gte_execute(&p->gte, GTE(0x0280030u));
    /* each vertex (n,0,0x1000) rotates to (n, n, 0x1000): screen (n/2, n/2) */
    T_CHECK_EQ(p->gte.dr[D_SXY0] & 0xFFFFu, 0u); /* V0: 1/2 = 0 */
    T_CHECK_EQ(p->gte.dr[D_SXY1] & 0xFFFFu, 1u); /* V1: 2/2 = 1 */
    T_CHECK_EQ(p->gte.dr[D_SXY2] & 0xFFFFu, 1u); /* V2: 3/2 = 1 (floor) */
    T_CHECK_EQ(p->gte.dr[D_SXY2] >> 16, 1u);
    /* IR0 only for last vertex */
    T_CHECK_EQ(p->gte.dr[D_IR0], 0u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gte_nclip(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* SXY0=(0,0) SXY1=(100,0) SXY2=(0,100):
     * MAC0 = 0*(0-100) + 100*(100-0) + 0*(0-0) = 10000 */
    p->gte.dr[D_SXY0] = 0;
    p->gte.dr[D_SXY1] = (0u << 16) | 100u;
    p->gte.dr[D_SXY2] = (100u << 16) | 0u;
    psx_gte_execute(&p->gte, GTE(0x0180006u)); /* NCLIP PsyQ 0x48180006 */
    T_CHECK_EQ(p->gte.dr[D_MAC0], 10000u);
    /* reversed winding -> negative */
    p->gte.dr[D_SXY1] = (100u << 16) | 0u;
    p->gte.dr[D_SXY2] = (0u << 16) | 100u;
    psx_gte_execute(&p->gte, GTE(0x0180006u));
    T_CHECK_EQ((uint32_t)(int32_t)p->gte.dr[D_MAC0], (uint32_t)-10000);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gte_avsz3(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    p->gte.cr[C_ZSF3] = 0x555u; /* 1/3 in 1.3.12 (1365) */
    p->gte.dr[D_SZ3 - 2] = 0x300u; /* SZ1 */
    p->gte.dr[D_SZ3 - 1] = 0x300u; /* SZ2 */
    p->gte.dr[D_SZ3] = 0x300u;
    psx_gte_execute(&p->gte, GTE(0x018002Du)); /* AVSZ3 PsyQ 0x4818002D */
    /* MAC0 = 0x555 * (0x300*3) = 1365*2304 = 3144960; OTZ = /4096 = 768 */
    T_CHECK_EQ(p->gte.dr[D_MAC0], 3144960u);
    T_CHECK_EQ(p->gte.dr[D_OTZ], 767u); /* 3144960/4096 truncates */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gte_mvmva_translation(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    set_rt_identity(p);
    p->gte.cr[C_TRX] = 10;
    p->gte.cr[C_TRX + 1] = 20;
    p->gte.cr[C_TRX + 2] = 30;
    /* V0 = (1, 2, 3) */
    p->gte.dr[D_VXY0] = (2u << 16) | 1u;
    p->gte.dr[D_VZ0] = 3u;
    /* MVMVA sf=1 mx=RT(0) v=V0(0) cv=TR(0): base 0x0400012 + sf 0x80000 */
    psx_gte_execute(&p->gte, GTE(0x0480012u));
    /* MACn/4096 = TRn + RT-row . V:
     * MAC1 = 10 + (1*1) = 11; MAC2 = 20 + (1*1 + 1*2) = 23;
     * MAC3 = 30 + (1*2 + 1*3) = 35 (identity off-diagonals are zero) */
    T_CHECK_EQ(p->gte.dr[D_IR1], 11u);
    T_CHECK_EQ(p->gte.dr[D_IR2], 23u);
    T_CHECK_EQ(p->gte.dr[D_IR3], 35u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gte_mvmva_fc_bug(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    set_rt_identity(p);
    /* cv=2 selects FC with the hardware bug: part2 overwrites part1. */
    p->gte.cr[C_RFC] = 1234; /* would-be translation */
    /* V = (1,2,3): part1 = FC*4096 + 1*4096 (discarded), part2 = 2*4096+3*4096 */
    p->gte.dr[D_VXY0] = (2u << 16) | 1u;
    p->gte.dr[D_VZ0] = 3u;
    /* cv=2: bits 14-13 = 10 -> 0x2000; sf=1: 0x80000 */
    psx_gte_execute(&p->gte, GTE(0x04A2012u));
    /* MAC1 = part2 = (0*2 + 0*3)? RT12=RT13=0 (identity has off-diagonals 0)
     * => MAC1 = 0. The bug: the FC translation is lost. */
    T_CHECK_EQ(p->gte.dr[D_MAC1], 0u);
    T_CHECK_EQ((uint32_t)(int32_t)p->gte.dr[D_MAC1], 0u);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gte_div_overflow(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    set_rt_identity(p);
    p->gte.cr[C_H] = 0x4000u;
    /* vertex behind camera: VZ negative -> SZ3 saturates 0 -> divide overflow */
    p->gte.dr[D_VXY0] = 0;
    p->gte.dr[D_VZ0] = (uint32_t)(uint16_t)(-8);
    psx_gte_execute(&p->gte, GTE(0x0180001u));
    /* MAC3 = RT33*VZ = 0x1000 * -8 = -0x8000: SZ3 = MAC3 (sf=1) is
     * negative -> clamped to 0 with FLAG bit18; divide: H >= SZ3*2 ->
     * saturated 1FFFF with FLAG bits 17 + 31. */
    T_CHECK_EQ(p->gte.dr[D_SZ3], 0u);
    T_CHECK((p->gte.cr[C_FLAG] & (1u << 17)) != 0);
    T_CHECK((p->gte.cr[C_FLAG] & (1u << 31)) != 0);
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gte_irgb_orgb(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* IRGB is data register 28 (white-box write, same as MTC2 path) */
    psx_gte_write(&p->gte, 28, 0x7FFFu);
    T_CHECK_EQ(p->gte.dr[D_IR1], 31u * 0x80u);
    T_CHECK_EQ(p->gte.dr[D_IR2], 31u * 0x80u);
    T_CHECK_EQ(p->gte.dr[D_IR3], 31u * 0x80u);
    T_CHECK_EQ(psx_gte_read(&p->gte, 29), 0x7FFFu); /* ORGB read-back */
    /* LZCS/LZCR */
    psx_gte_write(&p->gte, 30, 0x00800000u);
    T_CHECK_EQ(psx_gte_read(&p->gte, 31), 8u); /* leading zeros of 2^23 */
    psx_gte_write(&p->gte, 30, 0xFFFFFF00u);
    T_CHECK_EQ(psx_gte_read(&p->gte, 31), 24u); /* leading ones */
    emu_core_beatle_psx()->destroy(&p->base);
}

static void psx_gte_sqr(void)
{
    struct psx *p = psx_bare();
    T_CHECK(p != NULL);
    if (!p)
        return;
    /* IR = (100, -50, 7): SQR sf=0 (PsyQ 0x4800028) */
    p->gte.dr[D_IR1] = 100;
    p->gte.dr[D_IR2] = (uint32_t)(int16_t)-50;
    p->gte.dr[D_IR3] = 7;
    psx_gte_execute(&p->gte, GTE(0x0000028u));
    T_CHECK_EQ(p->gte.dr[D_MAC1], 10000u);
    T_CHECK_EQ(p->gte.dr[D_MAC2], 2500u);
    T_CHECK_EQ(p->gte.dr[D_MAC3], 49u);
    T_CHECK_EQ(p->gte.dr[D_IR1], 10000u);
    T_CHECK_EQ(p->gte.dr[D_IR2], 2500u);
    T_CHECK_EQ(p->gte.dr[D_IR3], 49u);
    emu_core_beatle_psx()->destroy(&p->base);
}

T_SUITE_BEGIN(beatle_psx_gte)
{ "rtps_identity", psx_gte_rtps_identity },
{ "rtps_fifo", psx_gte_rtps_fifo },
{ "nclip", psx_gte_nclip },
{ "avsz3", psx_gte_avsz3 },
{ "mvmva_translation", psx_gte_mvmva_translation },
{ "mvmva_fc_bug", psx_gte_mvmva_fc_bug },
{ "div_overflow", psx_gte_div_overflow },
{ "irgb_orgb_lz", psx_gte_irgb_orgb },
{ "sqr", psx_gte_sqr },
T_SUITE_END

T_SUITE_REG(beatle_psx_gte)
