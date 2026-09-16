/* mgbx joypad: P1 register with select lines and press interrupts. */
#include "mgbx.h"

#define JOYPAD_INT_BIT 4

static uint8_t compose_p1(gb_joypad *jp)
{
    uint8_t p1 = 0xC0u | 0x0Fu; /* upper bits read 1 */
    uint32_t b = jp->buttons;
    uint8_t dirs = 0x0Fu, acts = 0x0Fu;
    if (b & GB_BTN_RIGHT) dirs &= (uint8_t)~0x01u;
    if (b & GB_BTN_LEFT)  dirs &= (uint8_t)~0x02u;
    if (b & GB_BTN_UP)    dirs &= (uint8_t)~0x04u;
    if (b & GB_BTN_DOWN)  dirs &= (uint8_t)~0x08u;
    if (b & GB_BTN_A)     acts &= (uint8_t)~0x01u;
    if (b & GB_BTN_B)     acts &= (uint8_t)~0x02u;
    if (b & GB_BTN_SELECT) acts &= (uint8_t)~0x04u;
    if (b & GB_BTN_START) acts &= (uint8_t)~0x08u;

    uint8_t sel = (uint8_t)(jp->select & 0x30u);
    if ((sel & 0x10u) == 0u) /* direction keys selected */
        p1 &= dirs;
    if ((sel & 0x20u) == 0u) /* action keys selected */
        p1 &= acts;
    return p1;
}

uint8_t gb_joypad_read(gb_joypad *jp)
{
    return compose_p1(jp);
}

void gb_joypad_write(gb_joypad *jp, uint8_t v, struct mgbx *gb)
{
    uint8_t old = compose_p1(jp);
    jp->select = (uint8_t)(v & 0x30u);
    uint8_t now = compose_p1(jp);
    /* interrupt on any selected line newly pressed (1 -> 0) */
    if ((old & ~now) != 0u)
        gb_request_interrupt(gb, JOYPAD_INT_BIT);
}

/* Called when the host changes buttons (via set_input). */
void gb_joypad_set_buttons(gb_joypad *jp, uint32_t buttons, struct mgbx *gb)
{
    uint8_t old = compose_p1(jp);
    jp->buttons = buttons & 0xFFu;
    uint8_t now = compose_p1(jp);
    if ((old & ~now) != 0u)
        gb_request_interrupt(gb, JOYPAD_INT_BIT);
}
