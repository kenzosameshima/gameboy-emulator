/*
 * The P1 register through the joypad.h interface: group selection, the
 * active-low button lines, and when the joypad interrupt is requested.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <interrupts.h>
#include <joypad.h>

static Joypad joypad;
static InterruptRegisters interrupts;

enum {
    SELECT_DIRECTIONS = 0x20,   /* bit 4 low */
    SELECT_BUTTONS = 0x10,      /* bit 5 low */
    SELECT_BOTH = 0x00,
    SELECT_NONE = 0x30
};

static void setup(void)
{
    interrupts_init(&interrupts);
    joypad_init(&joypad, &interrupts);
}

static bool irq(void)
{
    return (interrupts.interrupt_flag & INTERRUPT_JOYPAD) != 0;
}

static void clear_irq(void)
{
    interrupts.interrupt_flag = 0;
}

static void test_reset_and_masks(void)
{
    setup();

    /* The boot ROM leaves both groups selected; nothing pressed, so all lines
     * read high. */
    assert(joypad_read(&joypad) == 0xCF);

    /* Only bits 4 and 5 can be written; bits 6 and 7 always read 1. */
    joypad_write(&joypad, 0x00);
    assert(joypad_read(&joypad) == 0xCF);
    joypad_write(&joypad, 0xFF);
    assert(joypad_read(&joypad) == 0xFF);
    joypad_write(&joypad, 0x2F);
    assert(joypad_read(&joypad) == 0xEF);

    /* Written bits 0-3 are not stored: with Right held, the line still reads
     * low. */
    joypad_set_pressed(&joypad, JOYPAD_RIGHT);
    joypad_write(&joypad, 0x2F);
    assert(joypad_read(&joypad) == 0xEE);
}

static void test_group_selection(void)
{
    setup();
    joypad_set_pressed(&joypad, JOYPAD_RIGHT | JOYPAD_A);

    /* Directions: Right is bit 0. */
    joypad_write(&joypad, SELECT_DIRECTIONS);
    assert(joypad_read(&joypad) == (0xC0 | SELECT_DIRECTIONS | 0x0E));

    /* Buttons: A is bit 0. */
    joypad_write(&joypad, SELECT_BUTTONS);
    assert(joypad_read(&joypad) == (0xC0 | SELECT_BUTTONS | 0x0E));

    /* Neither group: everything reads as released. */
    joypad_write(&joypad, SELECT_NONE);
    assert(joypad_read(&joypad) == 0xFF);

    /* Each key lands on its own bit. */
    static const struct {
        uint8_t mask;
        uint8_t select;
        uint8_t low_nibble;
    } keys[] = {
        { JOYPAD_RIGHT, SELECT_DIRECTIONS, 0x0E },
        { JOYPAD_LEFT, SELECT_DIRECTIONS, 0x0D },
        { JOYPAD_UP, SELECT_DIRECTIONS, 0x0B },
        { JOYPAD_DOWN, SELECT_DIRECTIONS, 0x07 },
        { JOYPAD_A, SELECT_BUTTONS, 0x0E },
        { JOYPAD_B, SELECT_BUTTONS, 0x0D },
        { JOYPAD_SELECT, SELECT_BUTTONS, 0x0B },
        { JOYPAD_START, SELECT_BUTTONS, 0x07 }
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        joypad_set_pressed(&joypad, keys[i].mask);
        joypad_write(&joypad, keys[i].select);
        assert(joypad_read(&joypad) ==
               (0xC0 | keys[i].select | keys[i].low_nibble));

        /* The other group does not see it. */
        joypad_write(&joypad, (uint8_t)(keys[i].select ^ SELECT_NONE));
        assert((joypad_read(&joypad) & 0x0F) == 0x0F);
    }

    /* With both groups selected, held keys are combined. */
    joypad_set_pressed(&joypad, JOYPAD_DOWN | JOYPAD_B);
    joypad_write(&joypad, SELECT_BOTH);
    assert(joypad_read(&joypad) == (0xC0 | 0x05));
}

static void test_interrupt_on_falling_line(void)
{
    setup();
    joypad_write(&joypad, SELECT_DIRECTIONS);
    assert(!irq());

    /* Pressing a key of the selected group requests it. */
    joypad_set_pressed(&joypad, JOYPAD_UP);
    assert(irq());
    clear_irq();

    /* Reporting the same keys again, or releasing them, does not. */
    joypad_set_pressed(&joypad, JOYPAD_UP);
    assert(!irq());
    joypad_set_pressed(&joypad, 0);
    assert(!irq());

    /* A key of the other group does not. */
    joypad_set_pressed(&joypad, JOYPAD_A);
    assert(!irq());

    /* Selecting its group while it is held pulls the line low. */
    joypad_write(&joypad, SELECT_BUTTONS);
    assert(irq());
    clear_irq();

    /* Selecting a group with nothing held does not. */
    joypad_set_pressed(&joypad, 0);
    joypad_write(&joypad, SELECT_DIRECTIONS);
    joypad_write(&joypad, SELECT_BUTTONS);
    joypad_write(&joypad, SELECT_NONE);
    assert(!irq());

    /* A second key on a line that is already low is not a new edge. */
    joypad_write(&joypad, SELECT_BOTH);
    joypad_set_pressed(&joypad, JOYPAD_RIGHT);
    assert(irq());
    clear_irq();
    joypad_set_pressed(&joypad, JOYPAD_RIGHT | JOYPAD_A);
    assert(!irq());

    /* But a key on a new line is. */
    joypad_set_pressed(&joypad, JOYPAD_RIGHT | JOYPAD_A | JOYPAD_B);
    assert(irq());

    /* With nothing selected, presses never interrupt. */
    setup();
    joypad_write(&joypad, SELECT_NONE);
    joypad_set_pressed(&joypad, 0xFF);
    assert(!irq());
}

int main(void)
{
    test_reset_and_masks();
    test_group_selection();
    test_interrupt_on_falling_line();

    printf("Joypad tests passed!\n");

    return 0;
}
