/* Debug traces for mgbax remaining test failures */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mgbax/gba.h"
#include "emu/emu.h"
#include "../tests/testutil.h"

static struct gba *mk(void)
{
    emu_core_t *c = NULL;
    emu_core_mgbax()->create(&c);
    struct gba *g = (struct gba *)c;
    size_t sz = 0;
    uint8_t *rom = gba_make_rom(0x1000, 0, &sz);
    emu_core_mgbax()->load_rom(c, rom, sz);
    free(rom);
    return g;
}

static void thumb_load_store(void)
{
    printf("== thumb load_store_offsets ==\n");
    struct gba *g = mk();
    static const uint16_t prog[] = {
        0x4A08u, 0x2078u, 0x2134u, 0x6010u, 0x61D1u,
        0x6853u, 0x7C93u, 0x6994u, 0x69D5u, 0x6816u,
        0xE7FEu, 0, 0, 0, 0, 0, 0, 0, 0x0000u, 0x0200u
    };
    memcpy(g->cart.rom, prog, sizeof prog);
    g->cpu.cpsr |= GBA_T;
    for (int i = 0; i < 10; i++) {
        uint32_t pc = g->cpu.r[15];
        uint16_t instr = gba_mem_read16(g, pc);
        gba_cpu_step(g);
        fprintf(stderr, "  step %d pc=%08X instr=%04X r6=%08X ewram0=%02X ewram1C=%02X\n",
               i, pc, instr, g->cpu.r[6], g->mem.ewram[0],
               g->mem.ewram[0x1C]);
    }
    emu_core_mgbax()->destroy(&g->base);
}

static void dma_vblank(void)
{
    printf("== dma vblank_trigger ==\n");
    struct gba *g = mk();
    gba_mem_write32(g, 0x02000000u, 0xCAFEBABEu);
    gba_dma_write(g, 0x040000C8u, 0x0000u);
    gba_dma_write(g, 0x040000CAu, 0x0200u);
    gba_dma_write(g, 0x040000CCu, 0x0000u);
    gba_dma_write(g, 0x040000CEu, 0x0202u);
    gba_dma_write(g, 0x040000D0u, 1u);
    printf("  before: enabled=%u sad=%08X dad=%08X count=%u\n",
           g->dma.enabled[2], g->dma.sad[2], g->dma.dad[2], g->dma.count[2]);
    gba_dma_write(g, 0x040000D2u, 0x9400u);
    printf("  after ctrl write: enabled=%u count=%u ctrl=%04X\n",
           g->dma.enabled[2], g->dma.count[2], g->dma.ctrl[2]);
    gba_dma_run(g, 1u);
    printf("  mem[0x02020000]=%08X ctrl=%04X\n",
           gba_mem_read32(g, 0x02020000u), g->dma.ctrl[2]);
    emu_core_mgbax()->destroy(&g->base);
}

static void sprite(void)
{
    printf("== ppu sprite ==\n");
    struct gba *g = mk();
    gba_mem_write16(g, 0x07000000u, 0x0000u);
    gba_mem_write16(g, 0x07000002u, 0x0004u);
    gba_mem_write16(g, 0x07000004u, 0x0000u);
    gba_mem_write8(g, 0x06010000u, 0x01u);
    gba_mem_write16(g, 0x05000202u, 0x03E0u);
    gba_io_write16(g, 0x04000000u, 0x1100u);
    printf("  dispcnt=%04X oam0=%02X%02X%02X%02X vram[10000]=%02X pal[202]=%04X\n",
           gba_mem_read16(g, 0x04000000u), g->mem.oam[0], g->mem.oam[1],
           g->mem.oam[2], g->mem.oam[3], g->mem.vram[0x10000],
           gba_mem_read16(g, 0x05000202u));
    gba_ppu_render_line(g, 0);
    printf("  fb[4]=%08X\n", g->ppu.fb[4]);
    emu_core_mgbax()->destroy(&g->base);
}

static void irq(void)
{
    printf("== irq dispatch ==\n");
    struct gba *g = mk();
    static const uint32_t prog[] = { 0xE28F0000u, 0xEAFFFFFEu };
    memcpy(g->cart.rom, prog, sizeof prog);
    static const uint32_t handler[] = {
        0xE3A04404u, 0xE3844C02u, 0xE3844002u, 0xE3A03001u,
        0xE1C430B0u, 0xE59F100Cu, 0xE5912000u, 0xE2822001u,
        0xE5812000u, 0xE12FFF1Eu, 0x03000140u
    };
    memcpy(&g->mem.iwram[0x100], handler, sizeof handler);
    gba_mem_write32(g, 0x03007FFCu, 0x03000100u);
    g->mem.ie = 1;
    g->mem.if_reg = 1;
    g->cpu.cpsr &= (uint32_t)~GBA_I;
    for (int i = 0; i < 13; i++) {
        gba_cpu_step(g);
        printf("  step %2d: r15=%08X mode=%02X sp=%08X\n", i, g->cpu.r[15],
               g->cpu.mode, g->cpu.r[13]);
    }
    printf("  counter[0x03000140]=%08X IF=%04X\n",
           gba_mem_read32(g, 0x03000140u), g->mem.if_reg);
    emu_core_mgbax()->destroy(&g->base);
}

static void str_micro(void)
{
    printf("== str micro ==\n");
    struct gba *g = mk();
    static const uint16_t prog[] = { 0x4880u, 0x2078u, 0x6041u, 0xE7FEu, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0000u, 0x0200u };
    /* 0: LDR R0, [PC, #512->clamped?] use 0x4880: imm8=0x80*4=512?? too far.
       simpler: LDR R0 literal at [PC,#128]: base=4+128=132?? Let me instead
       set r2 directly via memory peek: we just want STR R0, [r2] with r2
       preset. Use gdb-style direct register setup. */
    (void)prog;
    g->cpu.cpsr |= GBA_T;
    g->cpu.r[2] = 0x02000000u;
    g->cpu.r[0] = 0x78u;
    g->cpu.r[15] = 0x08000100u; /* scratch area: write STR there */
    uint16_t str_instr = 0x6010u; /* STR R0, [R2] */
    memcpy(&g->cart.rom[0x100], &str_instr, 2);
    gba_cpu_step(g);
    printf("  after STR@100: r15=%08X ewram[0]=%02X\n", g->cpu.r[15],
           g->mem.ewram[0]);
    emu_core_mgbax()->destroy(&g->base);

    /* variant B: same instruction at 0x08000006, MOVs before like the test */
    struct gba *g2 = mk();
    static const uint16_t prog2[] = { 0x2078u, 0x2134u, 0x6010u, 0xE7FEu };
    memcpy(g2->cart.rom, prog2, sizeof prog2);
    g2->cpu.cpsr |= GBA_T;
    g2->cpu.r[2] = 0x02000000u;
    gba_cpu_step(g2); /* MOV R0 */
    gba_cpu_step(g2); /* MOV R1 */
    printf("  before STR@6: r0=%08X r2=%08X pc=%08X instr=%04X\n",
           g2->cpu.r[0], g2->cpu.r[2], g2->cpu.r[15],
           gba_mem_read16(g2, g2->cpu.r[15]));
    gba_cpu_step(g2); /* STR R0, [R2] */
    printf("  after STR@6: ewram[0]=%02X r15=%08X\n", g2->mem.ewram[0],
           g2->cpu.r[15]);
    emu_core_mgbax()->destroy(&g2->base);
}

static void dma_reload(void)
{
    printf("== dma dst_reload_repeat ==\n");
    struct gba *g = mk();
    gba_mem_write32(g, 0x02000000u, 0x00000001u);
    gba_mem_write32(g, 0x02000004u, 0x00000002u);
    gba_dma_write(g, 0x040000D4u, 0x0000u);
    gba_dma_write(g, 0x040000D6u, 0x0200u);
    gba_dma_write(g, 0x040000D8u, 0x0020u);
    gba_dma_write(g, 0x040000DAu, 0x0300u);
    gba_dma_write(g, 0x040000DCu, 2u);
    gba_dma_write(g, 0x040000DEu, 0x9660u);
    printf("  ch3: enabled=%u sad=%08X dad=%08X count=%u ctrl=%04X\n",
           g->dma.enabled[3], g->dma.sad[3], g->dma.dad[3], g->dma.count[3],
           g->dma.ctrl[3]);
    gba_dma_run(g, 1u);
    printf("  mem[03000020]=%08X mem[03000024]=%08X dad=%08X\n",
           gba_mem_read32(g, 0x03000020u), gba_mem_read32(g, 0x03000024u),
           g->dma.dad[3]);
    emu_core_mgbax()->destroy(&g->base);
}

static void irq2(void)
{
    printf("== irq2 ==\n");
    struct gba *g = mk();
    static const uint32_t prog[] = { 0xE28F0000u, 0xEAFFFFFEu };
    memcpy(g->cart.rom, prog, sizeof prog);
    static const uint32_t handler[] = {
        0xE3A04404u, 0xE3844C02u, 0xE3844002u, 0xE3A03001u,
        0xE1C430B0u, 0xE59F100Cu, 0xE5912000u, 0xE2822001u,
        0xE5812000u, 0xE12FFF1Eu, 0x03000140u
    };
    memcpy(&g->mem.iwram[0x100], handler, sizeof handler);
    gba_mem_write32(g, 0x03007FFCu, 0x03000100u);
    g->mem.ie = 1;
    g->mem.if_reg = 1;
    g->cpu.cpsr &= (uint32_t)~GBA_I;
    for (int i = 0; i < 12; i++) {
        gba_cpu_step(g);
        fprintf(stderr, "  s%02d r15=%08X r3=%08X r4=%08X IF=%04X\n", i,
                g->cpu.r[15], g->cpu.r[3], g->cpu.r[4], g->mem.if_reg);
    }
    emu_core_mgbax()->destroy(&g->base);
}

int main(void)
{
    dma_reload();
    irq2();
    str_micro();
    thumb_load_store();
    dma_vblank();
    sprite();
    irq();
    return 0;
}
/* appended: micro STR test */
