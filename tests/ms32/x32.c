/*
 * ms-32 (Sega 32X) tests: adapter registers through the 68K window, SH-2
 * execution from cartridge ROM into the framebuffers, the shared COMM
 * ports, the VDP fill command and the save-state contract. SH-2 opcodes
 * are hand-assembled from the Hitachi manual.
 */
#include "../tests.h"
#include "../testutil.h"
#include "ms-32/x32.h"

#include <stdlib.h>
#include <string.h>

/* Build a minimal 32X cartridge: Genesis header + a 68K idle loop and
 * SH-2 programs at ROM offset 0x400 (SH-2 address 0x02000400). */
static struct x32 *boot32x(const uint16_t *sh2_code, size_t words)
{
    size_t rom_size = 0x800u;
    uint8_t *rom = calloc(1, rom_size);
    if (rom == NULL)
        return NULL;
    memcpy(rom + 0x100, "SEGA", 4);
    /* 68K: initial SSP (0) and entry (4) -> idle loop at 0x400 */
    uint32_t ssp = 0x00FF0000u, entry = 0x00000400u;
    rom[0] = (uint8_t)(ssp >> 24); rom[1] = (uint8_t)(ssp >> 16);
    rom[2] = (uint8_t)(ssp >> 8);  rom[3] = (uint8_t)ssp;
    rom[4] = (uint8_t)(entry >> 24); rom[5] = (uint8_t)(entry >> 16);
    rom[6] = (uint8_t)(entry >> 8);  rom[7] = (uint8_t)entry;
    /* 68K idle loop at 0x400: BRA 0 (branch to self), then padding */
    rom[0x400] = 0x60u; rom[0x401] = 0xFEu; /* bra -2 */
    /* SH-2 code (big-endian words) at 0x404 */
    for (size_t i = 0; i < words; i++) {
        rom[0x404 + i * 2u] = (uint8_t)(sh2_code[i] >> 8);
        rom[0x405 + i * 2u] = (uint8_t)sh2_code[i];
    }
    emu_core_t *c = NULL;
    if (emu_core_ms_32()->create(&c) != EMU_OK) {
        free(rom);
        return NULL;
    }
    if (emu_core_ms_32()->load_rom(c, rom, rom_size) != EMU_OK) {
        emu_core_ms_32()->destroy(c);
        free(rom);
        return NULL;
    }
    free(rom);
    return (struct x32 *)c;
}

/* Start the SH-2s the way the adapter does: SP/PC from the cart vectors,
 * overridden here to point at the test program (white-box boot). */
static void sh2_boot_at(struct x32 *x, uint32_t pc)
{
    for (int i = 0; i < 2; i++) {
        sh2_reset(&x->sh2[i]);
        x->sh2[i].r[15] = 0x06010000u;
        x->sh2[i].pc = 0x02000000u + pc;
        x->sh2[i].next_pc = x->sh2[i].pc + 2u;
    }
}

static void ms32_adapter_window(void)
{
    struct x32 *x = boot32x(NULL, 0);
    T_CHECK(x != NULL);
    if (!x)
        return;
    /* 68K writes flow through the adapter hook */
    fb_md_68k_write16(x->md, 0xA15100u, 0x0001u);
    T_CHECK_EQ(x->reg_res, 0x0001u);
    fb_md_68k_write16(x->md, 0xA15120u, 0x1234u); /* COMM0 */
    T_CHECK_EQ(x->comm[0], 0x1234u);
    T_CHECK_EQ(fb_md_68k_read16(x->md, 0xA15120u), 0x1234u);
    T_CHECK_EQ(fb_md_68k_read16(x->md, 0xA15100u), 0x0001u);
    /* SH-2 side sees the same COMM through its window */
    T_CHECK_EQ(x32_sh2_read16(x, 0x4020u), 0x1234u);
    emu_core_ms_32()->destroy(&x->base);
}

static void ms32_sh2_framebuffer(void)
{
    /* SH-2 program: fill the first words of framebuffer A with a color.
     *   MOV.L @(disp,PC),r1    ; load 0x04000000 base
     *   MOV #0x1F,r2
     *   MOV.W r2,@r1  (x N)
     *   ... then idle */
    static const uint16_t code[] = {
        0xE104u, /* MOV #4,r1 */
        0x4128u, /* SHLL16 r1 -> 0x00040000 */
        0x4118u, /* SHLL8  r1 -> 0x04000000 */
        0xE21Fu, /* MOV #0x1F,r2 */
        0x2121u, /* MOV.W r2,@r1 */
        0x7104u, /* ADD #4,r1 */
        0x2121u, /* MOV.W r2,@r1 */
        0x7104u, /* ADD #4,r1 */
        0x2121u, /* MOV.W r2,@r1 */
        0x0009u, /* NOP (idle) */
    };
    struct x32 *x = boot32x(code, sizeof code / sizeof code[0]);
    T_CHECK(x != NULL);
    if (!x)
        return;
    sh2_boot_at(x, 0x404u);
    emu_core_ms_32()->run_frame(&x->base);
    /* framebuffer A words 0/2/4 hold the 0x001F color written by the SH-2 */
    T_CHECK_EQ(x->fb_a[0], 0x001Fu);
    T_CHECK_EQ(x->fb_a[2], 0x001Fu);
    T_CHECK_EQ(x->fb_a[4], 0x001Fu);
    emu_core_ms_32()->destroy(&x->base);
}

static void ms32_vdp_fill(void)
{
    struct x32 *x = boot32x(NULL, 0);
    T_CHECK(x != NULL);
    if (!x)
        return;
    /* SH-2 writes FILL start + data through the VDP window */
    x32_sh2_write16(x, 0x4104u, 0x0100u); /* FILLA = word 0x100 */
    x32_sh2_write16(x, 0x4106u, 0x1F0Cu); /* FILLD triggers the fill */
    T_CHECK_EQ(x->fb_a[0x100u], 0x1F0Cu);
    T_CHECK_EQ(x->fb_a[0x101u], 0x1F0Cu);
    T_CHECK_EQ(x->fb_a[0x3FFFu], 0x1F0Cu);
    T_CHECK_EQ(x->fb_a[0x0FFu], 0u);  /* below start untouched */
    emu_core_ms_32()->destroy(&x->base);
}

static void ms32_comm_sh2_68k(void)
{
    struct x32 *x = boot32x(NULL, 0);
    T_CHECK(x != NULL);
    if (!x)
        return;
    /* SH-2 writes COMM3; the 68K reads it back through its window */
    x32_sh2_write16(x, 0x4026u, 0xBEEFu);
    T_CHECK_EQ(fb_md_68k_read16(x->md, 0xA15126u), 0xBEEFu);
    /* and the slave sees the master's write */
    T_CHECK_EQ(x32_sh2_read16(x, 0x4026u), 0xBEEFu);
    emu_core_ms_32()->destroy(&x->base);
}

static void ms32_state_roundtrip(void)
{
    struct x32 *x = boot32x(NULL, 0);
    T_CHECK(x != NULL);
    if (!x)
        return;
    x32_sh2_write16(x, 0x4020u, 0x4321u);
    size_t size = emu_core_ms_32()->state_size(&x->base);
    uint8_t *blob = malloc(size);
    T_CHECK(blob != NULL);
    if (blob) {
        emu_result_t r = emu_core_ms_32()->save_state(&x->base, blob, size);
        T_CHECK_EQ(r, EMU_OK);
        struct x32 *b = boot32x(NULL, 0);
        T_CHECK(b != NULL);
        if (b) {
            r = emu_core_ms_32()->load_state(&b->base, blob, size);
            T_CHECK_EQ(r, EMU_OK);
            T_CHECK_EQ(b->comm[0], 0x4321u);
            T_CHECK_EQ(b->sh2[0].pc, x->sh2[0].pc);
            emu_core_ms_32()->destroy(&b->base);
        }
        free(blob);
    }
    emu_core_ms_32()->destroy(&x->base);
}

T_SUITE_BEGIN(ms32)
{ "adapter_window", ms32_adapter_window },
{ "sh2_framebuffer", ms32_sh2_framebuffer },
{ "vdp_fill", ms32_vdp_fill },
{ "comm_sh2_68k", ms32_comm_sh2_68k },
{ "state_roundtrip", ms32_state_roundtrip },
T_SUITE_END

T_SUITE_REG(ms32)
