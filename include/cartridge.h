#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    CARTRIDGE_MAPPER_NONE,
    CARTRIDGE_MAPPER_MBC1
} CartridgeMapper;

enum {
    CARTRIDGE_ROM_BANK_SIZE = 0x4000,
    CARTRIDGE_RAM_BANK_SIZE = 0x2000,

    CARTRIDGE_HEADER_TYPE = 0x0147,
    CARTRIDGE_HEADER_RAM_SIZE = 0x0149
};

/* MBC1 registers. Private to the MBC1 mapper (src/mapper_mbc1.c). */
typedef struct Mbc1State {
    bool ram_enabled;
    uint8_t bank_low;     /* 5 bits, written 0 is stored as 1 */
    uint8_t bank_high;    /* 2 bits */
    bool banking_mode;    /* false: simple, true: advanced */
} Mbc1State;

/*
 * A Cartridge initialized with cartridge_init() and given a ROM buffer
 * behaves as a ROM-only cartridge, mapped directly at 0000-7FFF.
 *
 * The mapper decides how addresses map into the ROM and RAM buffers; see
 * src/mapper.h. `state` holds that mapper's registers and is private to it.
 */
typedef struct Cartridge {
    uint8_t *rom;
    size_t rom_size;
    /* Number of 16 KiB banks rounded up to a power of two, minus one. */
    size_t rom_bank_mask;

    CartridgeMapper mapper;

    uint8_t *ram;
    size_t ram_size;

    union {
        Mbc1State mbc1;
    } state;
} Cartridge;

void cartridge_init(Cartridge *cartridge);

/* Result of cartridge_load(). CARTRIDGE_LOAD_OK is zero. */
typedef enum {
    CARTRIDGE_LOAD_OK = 0,
    CARTRIDGE_LOAD_INVALID_ARGUMENT,
    /* The file cannot be opened or read, or it is empty. */
    CARTRIDGE_LOAD_IO_ERROR,
    CARTRIDGE_LOAD_OUT_OF_MEMORY,
    /* The header names a cartridge type that is not implemented. */
    CARTRIDGE_LOAD_UNSUPPORTED_TYPE
} CartridgeLoadStatus;

/*
 * Loads a ROM file and configures the mapper from the header.
 * Supported cartridge types: 00 (ROM only) and 01-03 (MBC1, +RAM,
 * +BATTERY; battery contents are not persisted). Files too small to
 * contain a header are loaded as ROM only.
 *
 * A failed load leaves the cartridge unchanged. For
 * CARTRIDGE_LOAD_UNSUPPORTED_TYPE, `unsupported_type` (which may be NULL)
 * receives the cartridge type byte from the header (0x0147).
 */
CartridgeLoadStatus cartridge_load(
    Cartridge *cartridge,
    const char *path,
    uint8_t *unsupported_type
);
void cartridge_destroy(Cartridge *cartridge);

/* 0000-7FFF, through the mapper. */
uint8_t cartridge_read(const Cartridge *cartridge, uint16_t address);

/* Writes to 0000-7FFF program the mapper registers. */
void cartridge_write(Cartridge *cartridge, uint16_t address, uint8_t value);

/* A000-BFFF. Reads 0xFF and ignores writes without enabled RAM. */
uint8_t cartridge_read_ram(const Cartridge *cartridge, uint16_t address);
void cartridge_write_ram(Cartridge *cartridge, uint16_t address, uint8_t value);

#endif
