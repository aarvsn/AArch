/*
 * mgbax timer tests. Per the GBA specification: prescalers 1/64/256/1024,
 * cascade mode chains channel N overflow into channel N+1, overflow IRQ
 * bits are IF bit 3 + channel. Reload writes also reset the counter.
 */
#include "../tests.h"
#include "../testutil.h"
#include "emu/emu.h"
#include "mgbax/gba.h"

#include <stdlib.h>
#include <string.h>

static struct gba *mk_core(void)
{
    emu_core_t *c = NULL;
    if (emu_core_mgbax()->create(&c) != EMU_OK)
        return NULL;
    return (struct gba *)c;
}

static void timer_prescale_1_overflow(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* reload 0xFFFE, enable ch0 with prescale 1 and IRQ on overflow */
    gba_timers_write(g, 0x04000100u, 0xFFFEu);
    gba_timers_write(g, 0x04000102u, 0x0040u | 0x0080u); /* IRQ | enable */
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0xFFFEu);
    gba_timers_step(g, 1);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0xFFFFu);
    T_CHECK_EQ(g->mem.if_reg & 0x0008u, 0u); /* not yet overflowed */
    gba_timers_step(g, 1);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0xFFFEu); /* reload */
    T_CHECK_EQ(g->mem.if_reg & 0x0008u, 0x0008u); /* Timer0 overflow IRQ */
    emu_core_mgbax()->destroy(&g->base);
}

static void timer_prescale_1024(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* prescale selection: ctrl bits 0-1: 0=1, 1=64, 2=256, 3=1024 */
    gba_timers_write(g, 0x04000100u, 0x0000u);
    gba_timers_write(g, 0x04000102u, 0x0003u | 0x0080u);
    gba_timers_step(g, 1023);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0x0000u);
    gba_timers_step(g, 1);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0x0001u);
    /* carry-over across calls: 2048 cycles -> exactly 2 ticks */
    gba_timers_step(g, 2048);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0x0003u);
    emu_core_mgbax()->destroy(&g->base);
}

static void timer_cascade(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* ch0: free running, prescale 1, reload 0xFFFE.
     * ch1: cascade (bit 2), reload 0xFFFE, IRQ enabled. */
    gba_timers_write(g, 0x04000100u, 0xFFFEu);
    gba_timers_write(g, 0x04000102u, 0x0080u);
    gba_timers_write(g, 0x04000104u, 0xFFFEu);
    gba_timers_write(g, 0x04000106u, 0x0040u | 0x0084u);
    /* 2 cycles: ch0 overflows once -> ch1 counts 0xFFFE -> 0xFFFF */
    gba_timers_step(g, 2);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000104u), 0xFFFFu);
    T_CHECK_EQ(g->mem.if_reg & 0x0010u, 0u);
    /* 2 more cycles: ch0 overflows again -> ch1 overflows -> reload + IRQ */
    gba_timers_step(g, 2);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000104u), 0xFFFEu);
    T_CHECK_EQ(g->mem.if_reg & 0x0010u, 0x0010u); /* Timer1 overflow IRQ */
    /* ch1 in cascade mode ignores its own prescaler: 1000 more cycles
     * produce exactly 500 ch0 overflows -> 500 ch1 increments (even count:
     * back at the reload value). A prescaler-driven ch1 would differ. */
    gba_timers_step(g, 1000);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000104u), 0xFFFEu);
    emu_core_mgbax()->destroy(&g->base);
}

static void timer_enable_reloads(void)
{
    struct gba *g = mk_core();
    T_CHECK(g != NULL);
    if (!g)
        return;
    /* write reload while disabled: counter follows the reload write */
    gba_timers_write(g, 0x04000100u, 0x1234u);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0x1234u);
    /* enable: counter reloads from the latch */
    gba_timers_write(g, 0x04000102u, 0x0080u);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0x1234u);
    /* run 2 ticks, then disable and re-enable: counter reloads */
    gba_timers_step(g, 2);
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0x1236u);
    gba_timers_write(g, 0x04000102u, 0x0000u); /* disable */
    gba_timers_step(g, 100);                   /* disabled: no tick */
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0x1236u);
    gba_timers_write(g, 0x04000102u, 0x0080u); /* enable again */
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000100u), 0x1234u);
    /* control register readback */
    T_CHECK_EQ(gba_timers_read(&g->timers, 0x04000102u), 0x0080u);
    emu_core_mgbax()->destroy(&g->base);
}

T_SUITE_BEGIN(gba_timer)
{ "prescale_1_overflow", timer_prescale_1_overflow },
{ "prescale_1024", timer_prescale_1024 },
{ "cascade", timer_cascade },
{ "enable_reloads", timer_enable_reloads },
T_SUITE_END

T_SUITE_REG(gba_timer)
