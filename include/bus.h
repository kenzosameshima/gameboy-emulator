#ifndef BUS_H
#define BUS_H

#include <stdint.h>

typedef struct Cartridge Cartridge;
typedef struct Memory Memory;
typedef struct Timer Timer;

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

typedef struct Bus {
    Cartridge *cartridge;
    Memory *memory;
    InterruptRegisters *interrupts;
    Timer *timer;
} Bus;

/*
 * Implemented address map:
 * 0000-7FFF cartridge ROM
 * C000-DFFF work RAM
 * FF04-FF07 timer registers
 * FF0F       interrupt flag (IF)
 * FF80-FFFE high RAM
 * FFFF       interrupt enable (IE)
 * All other addresses currently return 0xFF or ignore writes.
 */

void bus_init(
    Bus *bus,
    Cartridge *cartridge,
    Memory *memory,
    InterruptRegisters *interrupts
);

void bus_attach_timer(Bus *bus, Timer *timer);

uint8_t bus_read(Bus *bus, uint16_t address);
void bus_write(Bus *bus, uint16_t address, uint8_t value);

#endif
