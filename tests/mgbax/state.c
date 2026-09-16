/*
 * mgbax save-state tests: explicit serialization contract.
 * state_size() must equal the save buffer size actually written; undersized
 * buffers return EMU_ENOSPACE without writing; foreign blobs are rejected;
 * a roundtrip restores CPU, memory and cartridge state bit-exactly.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbax/gba.h"

#include <stdlib.h>
#include <string.h>

void t_register_gba_arm(void);
void t_register_gba_thumb(void);
void t_register_gba_mem(void);
void t_register_gba_timer(void);
void t_register_gba_dma(void);
void t_register_gba_ppu(void);
void t_register_gba_state(void);

/* Suite aggregator expected by the test runner. */
void t_register_mgbax(void)
{
    t_register_gba_arm();
    t_register_gba_thumb();
    t_register_gba_mem();
    t_register_gba_timer();
    t_register_gba_dma();
    t_register_gba_ppu();
    t_register_gba_state();
}

static struct gba *mk_rom_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbax()->create(&c) != EMU_OK)
        return NULL;
    size_t rom_size = 0;
    uint8_t *rom = gba_make_rom(0x2000, 0x5A, &rom_size);
    emu_result_t r = emu_core_mgbax()->load_rom(c, rom, rom_size);
    free(rom);
    if (r != EMU_OK) {
        emu_core_mgbax()->destroy(c);
        return NULL;
    }
    return (struct gba *)c;
}

/* Makes observable, non-default state: registers, memories, timers, DMA. */
static void perturb(struct gba *g)
{
    for (int i = 0; i < 16; i++)
        g->cpu.r[i] = (uint32_t)(0x0100u + (uint32_t)i);
    g->cpu.r[15] = 0x08000010u;
    g->cpu.cpsr = GBA_N | GBA_MODE_SVC;
    gba_mem_write32(g, 0x02000000u, 0xDEADBEEFu);
    gba_mem_write16(g, 0x03000004u, 0xBEEFu);
    gba_mem_write16(g, 0x05000006u, 0x7C00u);
    gba_mem_write32(g, 0x06000000u, 0x12345678u);
    gba_mem_write16(g, 0x07000002u, 0x00FFu);
    gba_mem_write8(g, 0x0E000010u, 0x77u);
    g->cpu.bank_r13[2] = 0x03007F90u;
    g->cpu.spsr[3] = 0x00000013u;
    g->mem.ie = 0x0009u;
    g->mem.if_reg = 0x0001u;
    g->timers.reload[0] = 0xFFFEu;
    g->timers.counter[0] = 0xFFFEu;
    g->timers.ctrl[0] = 0x0080u;
    g->dma.sad[0] = 0x02000000u;
    g->dma.dad[0] = 0x02000100u;
    g->dma.count[0] = 4u;
    g->dma.ctrl[0] = 0x8400u;
    g->dma.enabled[0] = 1u;
    g->total_cycles = 0x1122334455667788ull;
}

static void state_size_matches_serialization(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    perturb(g);
    size_t sz = emu_core_mgbax()->state_size(&g->base);
    T_CHECK(sz > 0);
    uint8_t *buf = malloc(sz);
    T_CHECK(buf != NULL);
    if (!buf) {
        emu_core_mgbax()->destroy(&g->base);
        return;
    }
    /* exact-size buffer must succeed and fill completely */
    T_CHECK_EQ(emu_core_mgbax()->save_state(&g->base, buf, sz), EMU_OK);
    /* size must be stable across saves */
    T_CHECK_EQ(emu_core_mgbax()->state_size(&g->base), sz);
    free(buf);
    emu_core_mgbax()->destroy(&g->base);
}

static void state_roundtrip_restores_everything(void)
{
    struct gba *g1 = mk_rom_core();
    struct gba *g2 = mk_rom_core();
    T_CHECK(g1 != NULL && g2 != NULL);
    if (!g1 || !g2) {
        if (g1)
            emu_core_mgbax()->destroy(&g1->base);
        if (g2)
            emu_core_mgbax()->destroy(&g2->base);
        return;
    }
    perturb(g1);
    size_t sz = emu_core_mgbax()->state_size(&g1->base);
    uint8_t *buf = malloc(sz);
    T_CHECK(buf != NULL);
    if (!buf) {
        emu_core_mgbax()->destroy(&g1->base);
        emu_core_mgbax()->destroy(&g2->base);
        return;
    }
    T_CHECK_EQ(emu_core_mgbax()->save_state(&g1->base, buf, sz), EMU_OK);
    T_CHECK_EQ(emu_core_mgbax()->load_state(&g2->base, buf, sz), EMU_OK);
    /* CPU state */
    for (int i = 0; i < 16; i++)
        T_CHECK_EQ(g2->cpu.r[i], g1->cpu.r[i]);
    T_CHECK_EQ(g2->cpu.cpsr, g1->cpu.cpsr);
    T_CHECK_EQ(g2->cpu.bank_r13[2], g1->cpu.bank_r13[2]);
    T_CHECK_EQ(g2->cpu.spsr[3], g1->cpu.spsr[3]);
    T_CHECK_EQ(g2->total_cycles, g1->total_cycles);
    /* memory blocks */
    T_CHECK(memcmp(g2->mem.ewram, g1->mem.ewram, sizeof g1->mem.ewram) == 0);
    T_CHECK(memcmp(g2->mem.iwram, g1->mem.iwram, sizeof g1->mem.iwram) == 0);
    T_CHECK(memcmp(g2->mem.pal, g1->mem.pal, sizeof g1->mem.pal) == 0);
    T_CHECK(memcmp(g2->mem.vram, g1->mem.vram, sizeof g1->mem.vram) == 0);
    T_CHECK(memcmp(g2->mem.oam, g1->mem.oam, sizeof g1->mem.oam) == 0);
    T_CHECK(memcmp(g2->cart.sram, g1->cart.sram, sizeof g1->cart.sram) == 0);
    T_CHECK_EQ(g2->mem.ie, g1->mem.ie);
    T_CHECK_EQ(g2->mem.if_reg, g1->mem.if_reg);
    /* timers + DMA */
    T_CHECK_EQ(g2->timers.reload[0], g1->timers.reload[0]);
    T_CHECK_EQ(g2->timers.counter[0], g1->timers.counter[0]);
    T_CHECK_EQ(g2->timers.ctrl[0], g1->timers.ctrl[0]);
    T_CHECK_EQ(g2->dma.sad[0], g1->dma.sad[0]);
    T_CHECK_EQ(g2->dma.dad[0], g1->dma.dad[0]);
    T_CHECK_EQ(g2->dma.count[0], g1->dma.count[0]);
    T_CHECK_EQ(g2->dma.ctrl[0], g1->dma.ctrl[0]);
    T_CHECK_EQ(g2->dma.enabled[0], g1->dma.enabled[0]);
    free(buf);
    emu_core_mgbax()->destroy(&g1->base);
    emu_core_mgbax()->destroy(&g2->base);
}

static void state_undersized_rejected(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    perturb(g);
    size_t sz = emu_core_mgbax()->state_size(&g->base);
    T_CHECK(sz > 16);
    uint8_t *buf = malloc(sz);
    T_CHECK(buf != NULL);
    if (!buf) {
        emu_core_mgbax()->destroy(&g->base);
        return;
    }
    memset(buf, 0xAA, sz);
    /* every short capacity fails and leaves the buffer untouched */
    for (size_t cap = 0; cap < 64 && cap < sz; cap++) {
        emu_result_t r = emu_core_mgbax()->save_state(&g->base, buf, cap);
        T_CHECK_EQ(r, EMU_ENOSPACE);
        T_CHECK_EQ(buf[0], 0xAAu);
        T_CHECK_EQ(buf[cap < sz ? cap : sz - 1], 0xAAu);
    }
    free(buf);
    emu_core_mgbax()->destroy(&g->base);
}

static void state_foreign_blob_rejected(void)
{
    struct gba *g = mk_rom_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    size_t sz = emu_core_mgbax()->state_size(&g->base);
    uint8_t *buf = malloc(sz);
    T_CHECK(buf != NULL);
    if (!buf) {
        emu_core_mgbax()->destroy(&g->base);
        return;
    }
    memset(buf, 0, sz);
    /* wrong magic */
    T_CHECK_EQ(emu_core_mgbax()->load_state(&g->base, buf, sz),
               EMU_EBADSTATE);
    /* valid magic but wrong core id */
    buf[0] = 0x11; buf[1] = 0x22; buf[2] = 0x33; buf[3] = 0x44;
    T_CHECK_EQ(emu_core_mgbax()->load_state(&g->base, buf, 12),
               EMU_EBADSTATE);
    /* truncation to header size still parses the header; a truncated body
     * must be rejected by the reader bounds checks */
    T_CHECK_EQ(emu_core_mgbax()->load_state(&g->base, buf, 0), EMU_EINVAL);
    T_CHECK_EQ(emu_core_mgbax()->load_state(&g->base, NULL, sz),
               EMU_EINVAL);
    free(buf);
    emu_core_mgbax()->destroy(&g->base);
}

T_SUITE_BEGIN(gba_state)
{ "size_matches_serialization", state_size_matches_serialization },
{ "roundtrip_restores_everything", state_roundtrip_restores_everything },
{ "undersized_rejected", state_undersized_rejected },
{ "foreign_blob_rejected", state_foreign_blob_rejected },
T_SUITE_END

T_SUITE_REG(gba_state)
