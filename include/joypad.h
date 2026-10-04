#ifndef JOYPAD_H
#define JOYPAD_H

#include <stdint.h>

#include <interrupts.h>

enum {
    JOYPAD_ADDRESS = 0xFF00
};

/* Bits of the pressed-buttons mask. */
enum {
    JOYPAD_RIGHT = 0x01,
    JOYPAD_LEFT = 0x02,
    JOYPAD_UP = 0x04,
    JOYPAD_DOWN = 0x08,
    JOYPAD_A = 0x10,
    JOYPAD_B = 0x20,
    JOYPAD_SELECT = 0x40,
    JOYPAD_START = 0x80
};

typedef struct Joypad {
    uint8_t select;    /* bits 4 and 5 of P1 as written; 0 selects a group */
    uint8_t pressed;   /* JOYPAD_* bits held down */
    uint8_t lines;     /* the four P1 input lines last seen; 0 means pressed */

    InterruptRegisters *interrupts;
} Joypad;

/*
 * The P1 register (FF00). The game picks the direction keys (bit 4 low) or
 * the buttons (bit 5 low) and reads the selected group in bits 0-3, where
 * 0 means pressed; bits 6 and 7 read as 1. Pressing a key of a selected
 * group, or selecting a group that has one held, pulls a line low and
 * requests the joypad interrupt.
 *
 * The front end reports the whole set of held keys with
 * joypad_set_pressed().
 */

void joypad_init(Joypad *joypad, InterruptRegisters *interrupts);

uint8_t joypad_read(const Joypad *joypad);
void joypad_write(Joypad *joypad, uint8_t value);

void joypad_set_pressed(Joypad *joypad, uint8_t pressed);

#endif
