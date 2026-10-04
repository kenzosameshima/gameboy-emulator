#include <stdint.h>

#include <joypad.h>

enum {
    SELECT_DIRECTIONS_BIT = 0x10,
    SELECT_BUTTONS_BIT = 0x20,
    SELECT_MASK = 0x30,
    LINES_MASK = 0x0F
};

/* The four input lines the selected groups drive; a pressed key reads 0. */
static uint8_t joypad_lines(const Joypad *joypad)
{
    uint8_t lines = LINES_MASK;

    if ((joypad->select & SELECT_DIRECTIONS_BIT) == 0) {
        lines &= (uint8_t)~(joypad->pressed & LINES_MASK);
    }

    if ((joypad->select & SELECT_BUTTONS_BIT) == 0) {
        lines &= (uint8_t)~((joypad->pressed >> 4) & LINES_MASK);
    }

    return lines;
}

/* Requests the interrupt when any line goes from high to low. */
static void joypad_update(Joypad *joypad)
{
    uint8_t lines = joypad_lines(joypad);

    if ((joypad->lines & (uint8_t)~lines & LINES_MASK) != 0) {
        interrupts_request(joypad->interrupts, INTERRUPT_JOYPAD);
    }

    joypad->lines = lines;
}

void joypad_init(Joypad *joypad, InterruptRegisters *interrupts)
{
    joypad->select = SELECT_MASK;
    joypad->pressed = 0;
    joypad->lines = LINES_MASK;
    joypad->interrupts = interrupts;
}

uint8_t joypad_read(const Joypad *joypad)
{
    return (uint8_t)(0xC0 | joypad->select | joypad_lines(joypad));
}

void joypad_write(Joypad *joypad, uint8_t value)
{
    joypad->select = (uint8_t)(value & SELECT_MASK);
    joypad_update(joypad);
}

void joypad_set_pressed(Joypad *joypad, uint8_t pressed)
{
    joypad->pressed = pressed;
    joypad_update(joypad);
}
