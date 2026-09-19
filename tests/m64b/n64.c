/*
 * m64-b: Nintendo 64 core tests.
 *
 * Expected values derive from the MIPS III / R4300i architecture (hand
 * assembled opcodes) and from the documented core model in
 * src/m64-b/n64.h - never from emulator internals.
 */
#include "../tests.h"
#include "../../src/m64-b/n64.h"

#include <stdlib.h>
#include <string.h>

static void w32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)(v >> 24); /* big-endian ROM */
    p[off + 1] = (uint8_t)(v >> 16);
    p[off + 2] = (uint8_t)(v >> 8);
    p[off + 3] = (uint8_t)v;
}

static struct n64 *n64_new(void)
{
    emu_core_t *c = NULL;
    if (emu_core_m64_b()->create(&c) != EMU_OK)
        return NULL;
    return (struct n64 *)c;
}

static void n64_free(struct n64 *n)
{
    emu_core_m64_b()->destroy(&n->base);
}

/* Build a .z64 image whose IPL region (0x040..) is the given program. */
static uint8_t *n64_make_rom(const uint32_t *code, size_t words,
                             size_t *size_out)
{
    size_t size = 0x1000u;
    uint8_t *rom = calloc(1, size);
    if (rom == NULL)
        return NULL;
    rom[0] = 0x80u;
    rom[1] = 0x37u;
    rom[2] = 0x12u;
    rom[3] = 0x40u;
    for (size_t i = 0; i < words; i++)
        w32(rom, 0x040u + i * 4u, code[i]);
    *size_out = size;
    return rom;
}

static int n64_boot_prog(struct n64 **out, const uint32_t *code, size_t words)
{
    size_t size = 0;
    uint8_t *rom = n64_make_rom(code, words, &size);
    if (rom == NULL)
        return 0;
    struct n64 *n = n64_new();
    if (n == NULL) {
        free(rom);
        return 0;
    }
    if (emu_core_m64_b()->load_rom(&n->base, rom, size) != EMU_OK) {
        free(rom);
        n64_free(n);
        return 0;
    }
    free(rom);
    *out = n;
    return 1;
}

static void lifecycle_rejects_bad_images(void)
{
    emu_core_t *c = NULL;
    T_CHECK_EQ(emu_core_m64_b()->create(&c), EMU_OK);
    T_CHECK_EQ(emu_core_m64_b()->load_rom(c, NULL, 10), EMU_EINVAL);
    static const uint8_t bad[0x1000] = { 0 };
    T_CHECK_EQ(emu_core_m64_b()->load_rom(c, bad, sizeof bad), EMU_EBADROM);
    T_CHECK_EQ(emu_core_m64_b()->run_frame(c), EMU_ENOROM);
    emu_core_m64_b()->destroy(c);
}

static void boot_runs_marker_program(void)
{
    static const uint32_t prog[] = {
        0x3C01A000u, /* lui  r1, 0xA000            */
        0x34211000u, /* ori  r1, r1, 0x1000        */
        0x3C021234u, /* lui  r2, 0x1234            */
        0x34425678u, /* ori  r2, r2, 0x5678        */
        0xAC220000u, /* sw   r2, 0(r1)             */
        0x1000FFFFu, /* beq  r0, r0, -1 (self)     */
        0x00000000u, /* nop (delay slot)           */
    };
    struct n64 *n = NULL;
    if (!n64_boot_prog(&n, prog, 7)) {
        T_FAIL("boot failed");
        return;
    }
    T_CHECK_EQ(emu_core_m64_b()->run_frame(&n->base), EMU_OK);
    /* boot model: SP = 0xA4001FF0, CPU entered at 0xA4000040 */
    T_CHECK_EQ_U(n->r[29], 0xA4001FF0ull);
    T_CHECK_EQ_U(n64_bus_read32(n, 0xA0001000u), 0x12345678u);
    n64_free(n);
}

static void cpu_ops_and_delay_slot(void)
{
    static const uint32_t prog[] = {
        0x3C01A000u, /* lui   r1, 0xA000                 */
        0x34211000u, /* ori   r1, r1, 0x1000             */
        0x3C021234u, /* lui   r2, 0x1234                 */
        0x34425678u, /* ori   r2, r2, 0x5678             */
        0x00410018u, /* mult  r2, r1                     */
        0x00001012u, /* mflo  r2                         */
        0xAC220000u, /* sw    r2, 0(r1)                  */
        0x24030003u, /* addiu r3, r0, 3                  */
        0x10000002u, /* beq   r0, r0, +2                 */
        0x24040005u, /* addiu r4, r0, 5   (delay slot)   */
        0x00000000u, /* nop (skipped region)             */
        0x00000000u, /* nop                              */
        0xAC240004u, /* sw    r4, 4(r1)                  */
        0x1000FFFFu, /* beq r0, r0, -1 (self)            */
        0x00000000u, /* nop                              */
    };
    struct n64 *n = NULL;
    if (!n64_boot_prog(&n, prog, 15)) {
        T_FAIL("boot failed");
        return;
    }
    T_CHECK_EQ(emu_core_m64_b()->run_frame(&n->base), EMU_OK);
    /* MULT: low word of 0x12345678 * 0xA0001000 mod 2^32 = 0x45678000
     * (0x12345678*0xA0001 = 0xB60B72E45678; <<12 truncated to 32 bits) */
    T_CHECK_EQ_U(n64_bus_read32(n, 0xA0001000u), 0x45678000u);
    /* delay slot executed exactly once */
    T_CHECK_EQ_U(n64_bus_read32(n, 0xA0001004u), 5u);
    n64_free(n);
}

static void pi_dma_copies_rom_to_rdram(void)
{
    uint32_t prog[8] = { 0 };
    prog[0] = 0x1000FFFFu; /* beq r0, r0, -1 (self) */
    prog[1] = 0x00000000u; /* nop */
    struct n64 *n = NULL;
    if (!n64_boot_prog(&n, prog, 2)) {
        T_FAIL("boot failed");
        return;
    }
    /* program the PI: cart 0x10000000 -> DRAM 0x00100000, 0x100 bytes */
    n64_bus_write32(n, 0xA4600000u, 0x00100000u); /* PI_DRAM_ADDR */
    n64_bus_write32(n, 0xA4600004u, 0xB0000000u); /* PI_CART_ADDR */
    n64_bus_write32(n, 0xA4600008u, 0x000000FFu); /* PI_RD_LEN = len-1 */
    T_CHECK_EQ_U(n64_bus_read32(n, 0xA0100000u), 0x80371240u);
    T_CHECK_EQ_U(n64_bus_read32(n, 0xA01000FCu), 0u);
    n64_free(n);
}

static void vi_renders_16bpp_framebuffer(void)
{
    /* program stores a gradient into RDRAM at 0x00100000 and loops */
    static const uint32_t prog[] = {
        0x3C01A000u, /* lui  r1, 0xA000            */
        0x34211000u, /* ori  r1, r1, 0x1000        */
        0x3C028001u, /* lui  r2, 0x8001            */
        0x34420000u, /* ori  r2, r2, 0x0000        */
        0x24420001u, /* addiu r2, r2, 1            */
        0x1000FFFDu, /* beq r0, r0, -3 (self-ish)  */
        0x00000000u, /* nop                        */
    };
    struct n64 *n = NULL;
    if (!n64_boot_prog(&n, prog, 7)) {
        T_FAIL("boot failed");
        return;
    }
    /* red pixel (RGBA5551 0x7C00) at (0,0), green 0x03E0 at (1,0) */
    n64_bus_write32(n, 0xA0100000u, 0x7C0003E0u); /* red, green */
    /* VI: status type 1 (16bpp), origin, width */
    n64_bus_write32(n, 0xA4400000u, 0x00000002u); /* type 1 = (1<<1) */
    n64_bus_write32(n, 0xA4400004u, 0x00100000u); /* VI_ORIGIN */
    n64_bus_write32(n, 0xA4400008u, 320u);        /* VI_WIDTH */
    T_CHECK_EQ(emu_core_m64_b()->run_frame(&n->base), EMU_OK);
    T_CHECK_EQ_U(n->fb[0], EMU_PIXEL(255, 0, 0));
    T_CHECK_EQ_U(n->fb[1], EMU_PIXEL(0, 255, 0));
    n64_free(n);
}

static void syscall_enters_and_eret_returns(void)
{
    static const uint32_t prog[] = {
        0x3C01A000u, /* lui   r1, 0xA000            */
        0x34212000u, /* ori   r1, r1, 0x2000        */
        0x24020005u, /* addiu r2, r0, 5             */
        0xAC220000u, /* sw    r2, 0(r1)             */
        0x0000000Cu, /* syscall                     */
        0x3C02A000u, /* lui   r2, 0xA000            */
        0x34423000u, /* ori   r2, r2, 0x3000        */
        0x24030077u, /* addiu r3, r0, 0x77          */
        0xAC430000u, /* sw    r3, 0(r2)             */
        0x1000FFF6u, /* beq r0, r0, -10 (self)      */
        0x00000000u, /* nop                         */
    };
    struct n64 *n = NULL;
    if (!n64_boot_prog(&n, prog, 11)) {
        T_FAIL("boot failed");
        return;
    }
    /* handler at the BEV general vector: counter++ then ERET */
    static const uint32_t handler[] = {
        0x3C01A000u, /* lui   r1, 0xA000     */
        0x34212000u, /* ori   r1, r1, 0x2000 */
        0x8C220000u, /* lw    r2, 0(r1)      */
        0x24420001u, /* addiu r2, r2, 1      */
        0xAC220000u, /* sw    r2, 0(r1)      */
        0x42000018u, /* eret                 */
    };
    for (size_t i = 0; i < sizeof handler / sizeof handler[0]; i++)
        w32(n->pif_rom, 0x200u + i * 4u, handler[i]);

    T_CHECK_EQ(emu_core_m64_b()->run_frame(&n->base), EMU_OK);
    T_CHECK_EQ_U(n64_bus_read32(n, 0xA0002000u), 6u); /* handler ran */
    T_CHECK_EQ_U(n64_bus_read32(n, 0xA0003000u), 0x77u); /* EPC resume */
    n64_free(n);
}

static void vi_interrupt_fires(void)
{
    static const uint32_t prog[] = {
        0x3C01A000u, /* lui   r1, 0xA000            */
        0x34212000u, /* ori   r1, r1, 0x2000        */
        0x24020000u, /* addiu r2, r0, 0             */
        0xAC220000u, /* sw    r2, 0(r1)             */
        /* VI_INTR = 100 */
        0x3C03A440u, /* lui   r3, 0xA440            */
        0x3463000Cu, /* ori   r3, r3, 0x000C        */
        0x24040064u, /* addiu r4, r0, 100           */
        0xAC640000u, /* sw    r4, 0(r3)             */
        /* MI_INTR_MASK = VI (0x08) */
        0x3C03A430u, /* lui   r3, 0xA430            */
        0x3463000Cu, /* ori   r3, r3, 0x000C        */
        0x24040008u, /* addiu r4, r0, 0x08          */
        0xAC640000u, /* sw    r4, 0(r3)             */
        /* Status = BEV | IE | IMASK(IP3): ERL cleared, interrupts on */
        0x3C020040u, /* lui   r2, 0x0040            */
        0x34420801u, /* ori   r2, r2, 0x0801        */
        0x40826000u, /* mtc0  r2, $12               */
        0x1000FFFFu, /* beq r0, r0, -1 (self)       */
        0x00000000u, /* nop                         */
    };
    struct n64 *n = NULL;
    if (!n64_boot_prog(&n, prog, 17)) {
        T_FAIL("boot failed");
        return;
    }
    static const uint32_t handler[] = {
        0x3C01A000u, /* lui   r1, 0xA000     */
        0x34212000u, /* ori   r1, r1, 0x2000 */
        0x8C220000u, /* lw    r2, 0(r1)      */
        0x24420001u, /* addiu r2, r2, 1      */
        0xAC220000u, /* sw    r2, 0(r1)      */
        0x42000018u, /* eret                 */
    };
    for (size_t i = 0; i < sizeof handler / sizeof handler[0]; i++)
        w32(n->pif_rom, 0x200u + i * 4u, handler[i]);

    T_CHECK_EQ(emu_core_m64_b()->run_frame(&n->base), EMU_OK);
    T_CHECK(n64_bus_read32(n, 0xA0002000u) >= 1u); /* handler ran */
    n64_free(n);
}

static void state_roundtrip_resumes(void)
{
    static const uint32_t prog[] = {
        0x3C01A000u, /* lui   r1, 0xA000            */
        0x34211000u, /* ori   r1, r1, 0x1000        */
        0x8C220000u, /* lw    r2, 0(r1)             */
        0x24420001u, /* addiu r2, r2, 1             */
        0xAC220000u, /* sw    r2, 0(r1)             */
        0x1000FFFAu, /* beq r0, r0, -6 (self)       */
        0x00000000u, /* nop                         */
    };
    struct n64 *n = NULL;
    if (!n64_boot_prog(&n, prog, 7)) {
        T_FAIL("boot failed");
        return;
    }
    T_CHECK_EQ(emu_core_m64_b()->run_frame(&n->base), EMU_OK);
    T_CHECK_EQ(emu_core_m64_b()->run_frame(&n->base), EMU_OK);
    uint32_t after_two = n64_bus_read32(n, 0xA0001000u);
    T_CHECK(after_two >= 2);

    size_t cap = emu_core_m64_b()->state_size(&n->base);
    T_CHECK(cap > 0);
    uint8_t *snap = malloc(cap);
    uint8_t *resumed = malloc(cap);
    if (snap == NULL || resumed == NULL) {
        T_FAIL("alloc failed");
        free(snap);
        free(resumed);
        n64_free(n);
        return;
    }
    T_CHECK_EQ(emu_core_m64_b()->save_state(&n->base, snap, cap), EMU_OK);
    T_CHECK_EQ(emu_core_m64_b()->run_frame(&n->base), EMU_OK);
    T_CHECK_EQ(emu_core_m64_b()->load_state(&n->base, snap, cap), EMU_OK);
    T_CHECK_EQ(emu_core_m64_b()->save_state(&n->base, resumed, cap), EMU_OK);
    T_CHECK(memcmp(snap, resumed, cap) == 0);
    free(snap);
    free(resumed);
    n64_free(n);
}

T_SUITE_BEGIN(m64b)
{ "lifecycle_rejects_bad_images", lifecycle_rejects_bad_images },
{ "boot_runs_marker_program", boot_runs_marker_program },
{ "cpu_ops_and_delay_slot", cpu_ops_and_delay_slot },
{ "pi_dma_copies_rom_to_rdram", pi_dma_copies_rom_to_rdram },
{ "vi_renders_16bpp_framebuffer", vi_renders_16bpp_framebuffer },
{ "syscall_enters_and_eret_returns", syscall_enters_and_eret_returns },
{ "vi_interrupt_fires", vi_interrupt_fires },
{ "state_roundtrip_resumes", state_roundtrip_resumes },
T_SUITE_END
T_SUITE_REG(m64b)
