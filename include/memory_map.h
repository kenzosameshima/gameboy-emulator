#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

/*
 * Game Boy address map. Register addresses owned by a single component
 * (Timer, Serial, interrupts) are declared in that component's header.
 *
 * 0000-7FFF cartridge ROM (mapper registers on write)
 * A000-BFFF cartridge RAM
 * C000-DFFF work RAM
 * E000-FDFF echo of C000-DDFF
 * FF01-FF02 serial
 * FF04-FF07 timer
 * FF0F      interrupt flag
 * FF80-FFFE high RAM
 * FFFF      interrupt enable
 */
enum {
    MEM_ROM_END = 0x7FFF,

    MEM_CART_RAM_START = 0xA000,
    MEM_CART_RAM_END = 0xBFFF,

    MEM_WRAM_START = 0xC000,
    MEM_WRAM_END = 0xDFFF,
    MEM_WRAM_SIZE = 0x2000,

    MEM_ECHO_START = 0xE000,
    MEM_ECHO_END = 0xFDFF,
    MEM_ECHO_OFFSET = 0x2000,

    MEM_HRAM_START = 0xFF80,
    MEM_HRAM_END = 0xFFFE,
    MEM_HRAM_SIZE = 0x7F
};

#endif
