#ifndef MAPPER_H
#define MAPPER_H

#include <stddef.h>
#include <stdint.h>

#include <cartridge.h>

/*
 * The seam between a Cartridge and its memory bank controller.
 *
 * Cartridge owns loading, header parsing and the ROM and RAM buffers; a
 * mapper decides how CPU addresses map into them and holds the registers
 * the game programs. There is one MapperOps per CartridgeMapper value,
 * returned by cartridge_mapper_ops(), and the Cartridge calls it after it
 * has checked that a ROM is loaded and the address is in range, so a
 * mapper never sees an address outside the region it serves.
 *
 * A mapper keeps its registers in Cartridge.state.
 */
typedef struct MapperOps {
    /* Puts Cartridge.state in its power-on state. */
    void (*reset)(Cartridge *cartridge);

    /* 0000-7FFF. */
    uint8_t (*read_rom)(const Cartridge *cartridge, uint16_t address);

    /* Writes to 0000-7FFF program the mapper registers. */
    void (*write_rom)(Cartridge *cartridge, uint16_t address, uint8_t value);

    /* A000-BFFF. Reads 0xFF and ignores writes while RAM is unavailable. */
    uint8_t (*read_ram)(const Cartridge *cartridge, uint16_t address);
    void (*write_ram)(Cartridge *cartridge, uint16_t address, uint8_t value);
} MapperOps;

extern const MapperOps MAPPER_ROM_ONLY;
extern const MapperOps MAPPER_MBC1;

/* The MapperOps for `mapper`. Never NULL. */
const MapperOps *cartridge_mapper_ops(CartridgeMapper mapper);

/* ROM byte at a byte offset into the whole image, or 0xFF past its end. */
uint8_t cartridge_rom_byte(const Cartridge *cartridge, size_t offset);

#endif
