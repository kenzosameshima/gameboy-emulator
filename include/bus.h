#ifndef BUS_H
#define BUS_H

#include <stdint.h>

#include <interrupts.h>

typedef struct Cartridge Cartridge;
typedef struct Memory Memory;
typedef struct Timer Timer;
typedef struct Serial Serial;
typedef struct Ppu Ppu;
typedef struct Dma Dma;
typedef struct Joypad Joypad;
typedef struct Apu Apu;

typedef struct Bus {
    Cartridge *cartridge;
    Memory *memory;
    InterruptRegisters *interrupts;
    Timer *timer;
    Serial *serial;
    Ppu *ppu;
    Dma *dma;
    Joypad *joypad;
    Apu *apu;
} Bus;

/*
 * The address map, in the order bus.c decodes it:
 * 0000-7FFF cartridge ROM; writes program the cartridge mapper
 * 8000-9FFF video RAM
 * A000-BFFF cartridge RAM
 * C000-DFFF work RAM
 * E000-FDFF echo of C000-DDFF
 * FE00-FE9F OAM
 * FF00      joypad (P1)
 * FF01-FF02 serial registers
 * FF04-FF07 timer registers
 * FF0F      interrupt flag (IF)
 * FF10-FF26 sound registers
 * FF30-FF3F wave RAM
 * FF40-FF45 LCD registers
 * FF46      OAM DMA
 * FF47-FF4B LCD registers
 * FF80-FFFE high RAM
 * FFFF      interrupt enable (IE)
 * Everything else (FEA0-FEFF and the unused I/O addresses) reads 0xFF and
 * ignores writes.
 *
 * While OAM DMA is copying, bus_read() and bus_write() cannot reach OAM or
 * the bus the DMA source is on (the external bus for ROM, cartridge RAM, work
 * RAM and echo; the video bus for VRAM): reads give 0xFF and writes are
 * ignored, as on hardware. The I/O registers and high RAM stay reachable.
 * The DMA itself reads through bus_dma_read().
 */

/*
 * Every device is required and must outlive the bus. The bus only keeps the
 * pointers, so the devices may be initialised after it; the DMA, which keeps
 * a pointer to the bus, usually is.
 */
void bus_init(
    Bus *bus,
    Cartridge *cartridge,
    Memory *memory,
    InterruptRegisters *interrupts,
    Timer *timer,
    Serial *serial,
    Ppu *ppu,
    Dma *dma,
    Joypad *joypad,
    Apu *apu
);

uint8_t bus_read(Bus *bus, uint16_t address);
void bus_write(Bus *bus, uint16_t address, uint8_t value);

/*
 * A read for the OAM DMA: not blocked by the transfer itself. Addresses from
 * E000 up read the work RAM mirror.
 */
uint8_t bus_dma_read(Bus *bus, uint16_t address);

#endif
