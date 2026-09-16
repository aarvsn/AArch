/*
 * mgbx timer: 16-bit internal counter clocked at 1 MHz (per M-cycle).
 *   DIV  = counter >> 6 (increments every 256 T-cycles)
 *   TIMA tick bit (falling edge): 4096 Hz -> bit 8, 262144 Hz -> bit 2,
 *   65536 Hz -> bit 4, 16384 Hz -> bit 6 of the M-cycle counter.
 * Overflow: TIMA wraps to 0, reloads TMA and raises the timer interrupt
 * after a 4 T-cycle delay. DIV writes reset the counter and can trigger a
 * TIMA falling edge (documented DMG quirk).
 */
#include "mgbx.h"

enum { DIV_ADDR = 0xFF04, TIMA_ADDR = 0xFF05, TMA_ADDR = 0xFF06, TAC_ADDR = 0xFF07 };

static const uint16_t tick_bit[4] = { 1u << 8, 1u << 2, 1u << 4, 1u << 6 };

static int tac_enabled(const gb_timer *tm)
{
    return (tm->tac & 0x04u) != 0;
}

static int tac_select(const gb_timer *tm)
{
    return tm->tac & 3u;
}

static void timer_tick_edge(struct mgbx *gb, uint16_t old, uint16_t now)
{
    gb_timer *tm = &gb->timer;
    if (!tac_enabled(tm))
        return;
    uint16_t mask = tick_bit[tac_select(tm) & 3u];
    if ((old & mask) != 0u && (now & mask) == 0u) {
        tm->tima++;
        if (tm->tima == 0u) {
            tm->tima = 0;
            tm->tima_reload = 1; /* 4 T-cycles until reload+IRQ */
        }
    }
}

void gb_timer_reset(gb_timer *tm)
{
    tm->counter = 0xAB00u >> 2; /* DIV reads 0xAB post-boot */
    tm->tima = 0;
    tm->tma = 0;
    tm->tac = 0xF8u & 0x07u; /* enabled, 4096 Hz */
    tm->tima_reload = 0;
}

void gb_timer_step(struct mgbx *gb, uint32_t t_cycles)
{
    gb_timer *tm = &gb->timer;
    /* called once per M-cycle with t_cycles == 4 */
    (void)t_cycles;
    uint16_t old = tm->counter;
    uint16_t now = (uint16_t)(old + 1);
    tm->counter = now;
    timer_tick_edge(gb, old, now);

    if (tm->tima_reload != 0) {
        tm->tima_reload++;
        if (tm->tima_reload > 1) { /* one M-cycle delay elapsed */
            tm->tima = tm->tma;
            tm->tima_reload = 0;
            gb_request_interrupt(gb, 2);
        }
    }
}

uint8_t gb_timer_read(gb_timer *tm, uint16_t addr)
{
    switch (addr) {
    case DIV_ADDR: return (uint8_t)(tm->counter >> 6);
    case TIMA_ADDR: return tm->tima;
    case TMA_ADDR: return tm->tma;
    case TAC_ADDR: return (uint8_t)(tm->tac | 0xF8u);
    default: return 0xFFu;
    }
}

void gb_timer_write(struct mgbx *gb, uint16_t addr, uint8_t v)
{
    gb_timer *tm = &gb->timer;
    switch (addr) {
    case DIV_ADDR: {
        uint16_t old = tm->counter;
        tm->counter = 0;
        timer_tick_edge(gb, old, 0);
        break;
    }
    case TIMA_ADDR:
        tm->tima = v;
        tm->tima_reload = 0; /* write cancels pending reload (documented DMG behavior) */
        break;
    case TMA_ADDR:
        tm->tma = v;
        break;
    case TAC_ADDR:
        tm->tac = (uint8_t)(v & 0x07u);
        break;
    default:
        break;
    }
}
