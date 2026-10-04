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

    /*
     * Advances time-driven mapper state by `cycles` T-cycles. NULL for
     * mappers with nothing that runs off the CPU clock.
     */
    void (*step)(Cartridge *cartridge, CpuCycles cycles);

    /*
     * Battery-backed state beyond the RAM, saved right after it (the MBC3
     * clock). All three are NULL for a mapper with none; the size may be 0
     * for a cartridge of that mapper without it.
     */
    size_t (*save_extra_size)(const Cartridge *cartridge);
    void (*save_extra)(
        const Cartridge *cartridge,
        uint8_t *out,
        uint64_t unix_time
    );
    void (*load_extra)(
        Cartridge *cartridge,
        const uint8_t *in,
        uint64_t unix_time
    );
} MapperOps;

extern const MapperOps MAPPER_ROM_ONLY;
extern const MapperOps MAPPER_MBC1;
extern const MapperOps MAPPER_MBC2;
extern const MapperOps MAPPER_MBC3;
extern const MapperOps MAPPER_MBC5;

/* The MapperOps for `mapper`. Never NULL. */
const MapperOps *cartridge_mapper_ops(CartridgeMapper mapper);

/* ROM byte at a byte offset into the whole image, or 0xFF past its end. */
uint8_t cartridge_rom_byte(const Cartridge *cartridge, size_t offset);

#endif
