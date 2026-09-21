#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

enum {
    INTERRUPT_VBLANK = 0x01,
    INTERRUPT_LCD_STAT = 0x02,
    INTERRUPT_TIMER = 0x04,
    INTERRUPT_SERIAL = 0x08,
    INTERRUPT_JOYPAD = 0x10
};

enum {
    INTERRUPT_VALID_MASK = 0x1F,
    INTERRUPT_FLAG_ADDRESS = 0xFF0F,
    INTERRUPT_ENABLE_ADDRESS = 0xFFFF
};

typedef struct InterruptRegisters {
    uint8_t interrupt_flag;
    uint8_t interrupt_enable;
} InterruptRegisters;

/* Initial state: IF = 0, IE = 0. IME lives in the CPU. */
void interrupts_init(InterruptRegisters *interrupts);

/* Hardware components raise interrupts through this call. */
void interrupts_request(InterruptRegisters *interrupts, uint8_t mask);

/* Requested and enabled interrupts: IF & IE & INTERRUPT_VALID_MASK. */
uint8_t interrupts_pending(const InterruptRegisters *interrupts);

#endif
