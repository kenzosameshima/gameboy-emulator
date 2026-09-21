#ifndef BUS_H
#define BUS_H

#include <stdint.h>

#include <interrupts.h>

typedef struct Cartridge Cartridge;
typedef struct Memory Memory;
typedef struct Timer Timer;
typedef struct Serial Serial;

typedef struct Bus {
    Cartridge *cartridge;
    Memory *memory;
    InterruptRegisters *interrupts;
    Timer *timer;
    Serial *serial;
} Bus;

/*
 * Implemented address map (see memory_map.h):
 * 0000-7FFF cartridge ROM; writes program the cartridge mapper
 * A000-BFFF cartridge RAM
 * C000-DFFF work RAM
 * E000-FDFF echo RAM
 * FF01-FF02 serial registers
 * FF04-FF07 timer registers
 * FF0F       interrupt flag (IF)
 * FF80-FFFE high RAM
 * FFFF       interrupt enable (IE)
 * All other addresses currently return 0xFF or ignore writes.
 *
 * The timer and serial are optional: without them their registers read
 * 0xFF and ignore writes.
 */

void bus_init(
    Bus *bus,
    Cartridge *cartridge,
    Memory *memory,
    InterruptRegisters *interrupts
);

void bus_attach_timer(Bus *bus, Timer *timer);
void bus_attach_serial(Bus *bus, Serial *serial);

uint8_t bus_read(Bus *bus, uint16_t address);
void bus_write(Bus *bus, uint16_t address, uint8_t value);

#endif
