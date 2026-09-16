/*
 * mgbax DMA tests. Per the GBA specification: 4 channels with registers
 * at 0x040000B0 + 12*ch (SAD, DAD, count, control); control bits:
 * dst[5:6], src[7:8], word=bit10, repeat=bit9, trigger[12:13], enable=bit15.
 * Immediate (trigger 0) transfers run on the control write.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbax/gba.h"

#include <stdlib.h>
#include <string.h>

#define DMA_REG(ch, off) (0x040000B0u + (uint32_t)(ch) * 12u + (off))

static struct gba *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbax()->create(&c) != EMU_OK)
        return NULL;
    return (struct gba *)c;
}

static void dma_immediate_word_copy(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* source pattern in EWRAM */
    gba_mem_write32(g, 0x02000000u, 0x11223344u);
    gba_mem_write32(g, 0x02000004u, 0x55667788u);
    gba_mem_write32(g, 0x02000008u, 0x99AABBCCu);
    gba_mem_write32(g, 0x0200000Cu, 0xDDEEFF00u);
    /* ch0: word copy, increment both, count 4, immediate */
    gba_dma_write(g, DMA_REG(0, 0), 0x0000u); /* SAD low */
    gba_dma_write(g, DMA_REG(0, 2), 0x0200u); /* SAD high */
    gba_dma_write(g, DMA_REG(0, 4), 0x0100u); /* DAD low */
    gba_dma_write(g, DMA_REG(0, 6), 0x0200u); /* DAD high */
    gba_dma_write(g, DMA_REG(0, 8), 4u);      /* count */
    gba_dma_write(g, DMA_REG(0, 10), 0x8400u); /* enable | word */
    /* immediate transfer ran inside the control write */
    T_CHECK_EQ(gba_mem_read32(g, 0x02000100u), 0x11223344u);
    T_CHECK_EQ(gba_mem_read32(g, 0x02000104u), 0x55667788u);
    T_CHECK_EQ(gba_mem_read32(g, 0x02000108u), 0x99AABBCCu);
    T_CHECK_EQ(gba_mem_read32(g, 0x0200010Cu), 0xDDEEFF00u);
    /* enable bit self-clears; count latches back */
    T_CHECK_EQ(gba_dma_read_ctrl(&g->dma, DMA_REG(0, 10)), 0x0400u);
    T_CHECK_EQ(gba_dma_read_ctrl(&g->dma, DMA_REG(0, 8)), 4u);
    emu_core_mgbax()->destroy(&g->base);
}

static void dma_src_fixed_fill(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_mem_write16(g, 0x02000000u, 0x7FFFu);
    /* ch1: halfword fill (src fixed = bits[8:7]=10), count 8, immediate */
    gba_dma_write(g, DMA_REG(1, 0), 0x0000u);
    gba_dma_write(g, DMA_REG(1, 2), 0x0200u);
    gba_dma_write(g, DMA_REG(1, 4), 0x0000u);
    gba_dma_write(g, DMA_REG(1, 6), 0x0201u); /* dst 0x02010000 */
    gba_dma_write(g, DMA_REG(1, 8), 8u);
    gba_dma_write(g, DMA_REG(1, 10), 0x8100u); /* enable | src fixed */
    for (int i = 0; i < 8; i++)
        T_CHECK_EQ(gba_mem_read16(g, (uint32_t)(0x02010000u + 2u * i)),
                   0x7FFFu);
    /* source pointer unchanged (fixed), destination advanced */
    T_CHECK_EQ(gba_dma_read_ctrl(&g->dma, DMA_REG(1, 0)), 0x0000u);
    T_CHECK_EQ(gba_dma_read_ctrl(&g->dma, DMA_REG(1, 4)), 0x0010u);
    emu_core_mgbax()->destroy(&g->base);
}

static void dma_vblank_trigger(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    gba_mem_write32(g, 0x02000000u, 0xCAFEBABEu);
    /* ch2: word copy, trigger = VBlank (bits[13:12] = 1), count 1 */
    gba_dma_write(g, DMA_REG(2, 0), 0x0000u);
    gba_dma_write(g, DMA_REG(2, 2), 0x0200u);
    gba_dma_write(g, DMA_REG(2, 4), 0x0000u);
    gba_dma_write(g, DMA_REG(2, 6), 0x0202u); /* dst 0x02020000 */
    gba_dma_write(g, DMA_REG(2, 8), 1u);
    gba_dma_write(g, DMA_REG(2, 10), 0x9400u); /* enable | word | vblank */
    /* nothing transferred yet */
    T_CHECK_EQ(gba_mem_read32(g, 0x02020000u), 0x00000000u);
    /* HBlank trigger flag does not service a VBlank channel */
    gba_dma_run(g, 2u);
    T_CHECK_EQ(gba_mem_read32(g, 0x02020000u), 0x00000000u);
    /* VBlank flag services it */
    gba_dma_run(g, 1u);
    T_CHECK_EQ(gba_mem_read32(g, 0x02020000u), 0xCAFEBABEu);
    /* enable self-cleared; trigger/word bits remain */
    T_CHECK_EQ(gba_dma_read_ctrl(&g->dma, DMA_REG(2, 10)), 0x1400u);
    emu_core_mgbax()->destroy(&g->base);
}

static void dma_dst_reload_repeat(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* Source pattern repeats every 2 words: per the GBA specification the
     * SOURCE address is NOT reloaded on repeat DMAs - it continues from
     * where the previous transfer ended. */
    gba_mem_write32(g, 0x02000000u, 0x00000001u);
    gba_mem_write32(g, 0x02000004u, 0x00000002u);
    gba_mem_write32(g, 0x02000008u, 0x00000001u);
    gba_mem_write32(g, 0x0200000Cu, 0x00000002u);
    /* ch3: word, dst increment+reload (bits[6:5] = 11), repeat (bit 9),
     * VBlank trigger, count 2 */
    gba_dma_write(g, DMA_REG(3, 0), 0x0000u);
    gba_dma_write(g, DMA_REG(3, 2), 0x0200u);
    gba_dma_write(g, DMA_REG(3, 4), 0x0020u);
    gba_dma_write(g, DMA_REG(3, 6), 0x0300u); /* dst 0x03000020 (IWRAM) */
    gba_dma_write(g, DMA_REG(3, 8), 2u);
    gba_dma_write(g, DMA_REG(3, 10), 0x9660u); /* en|word|dst_reload|rep|vbl */
    gba_dma_run(g, 1u); /* first VBlank */
    T_CHECK_EQ(gba_mem_read32(g, 0x03000020u), 0x00000001u);
    T_CHECK_EQ(gba_mem_read32(g, 0x03000024u), 0x00000002u);
    /* DAD reloaded to the latched value (0x03000020) after the transfer */
    T_CHECK_EQ(gba_dma_read_ctrl(&g->dma, DMA_REG(3, 4)), 0x0020u);
    T_CHECK_EQ(gba_dma_read_ctrl(&g->dma, DMA_REG(3, 6)), 0x0300u);
    /* repeat: enable bit stays set; second VBlank re-copies from SAD */
    T_CHECK_EQ(gba_dma_read_ctrl(&g->dma, DMA_REG(3, 10)) & 0x8000u, 0x8000u);
    gba_dma_run(g, 1u);
    /* second VBlank: source continued into the repeated pattern, dst
     * reloaded -> same destination values again */
    T_CHECK_EQ(gba_mem_read32(g, 0x03000020u), 0x00000001u);
    T_CHECK_EQ(gba_mem_read32(g, 0x03000024u), 0x00000002u);
    emu_core_mgbax()->destroy(&g->base);
}

T_SUITE_BEGIN(gba_dma)
{ "immediate_word_copy", dma_immediate_word_copy },
{ "src_fixed_fill", dma_src_fixed_fill },
{ "vblank_trigger", dma_vblank_trigger },
{ "dst_reload_repeat", dma_dst_reload_repeat },
T_SUITE_END

T_SUITE_REG(gba_dma)
