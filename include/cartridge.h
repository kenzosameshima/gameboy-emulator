#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <cycles.h>

typedef enum {
    CARTRIDGE_MAPPER_NONE,
    CARTRIDGE_MAPPER_MBC1,
    CARTRIDGE_MAPPER_MBC3
} CartridgeMapper;

enum {
    CARTRIDGE_ROM_BANK_SIZE = 0x4000,
    CARTRIDGE_RAM_BANK_SIZE = 0x2000,

    CARTRIDGE_HEADER_TYPE = 0x0147,
    CARTRIDGE_HEADER_RAM_SIZE = 0x0149
};

/* One set of MBC3 real-time clock registers. */
typedef struct RtcRegisters {
    uint8_t seconds;   /* 0-59 */
    uint8_t minutes;   /* 0-59 */
    uint8_t hours;     /* 0-23 */
    uint16_t days;     /* 0-511 */
    bool halted;
    bool day_carry;    /* set when days wrap past 511; stays set until cleared */
} RtcRegisters;

/* MBC3 registers. Private to the MBC3 mapper (src/mapper_mbc3.c). */
typedef struct Mbc3State {
    bool ram_and_clock_enabled;
    uint8_t rom_bank;      /* 7 bits, written 0 is stored as 1 */
    uint8_t select;        /* last value written to 4000-5FFF */
    uint8_t latch_write;   /* last value written to 6000-7FFF */
    bool has_clock;
    uint32_t subsecond_cycles;
    RtcRegisters live;     /* keeps running */
    RtcRegisters latched;  /* what the game reads */
} Mbc3State;

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
        Mbc3State mbc3;
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
 * Supported cartridge types: 00 (ROM only), 01-03 (MBC1, +RAM, +BATTERY)
 * and 0F-13 (MBC3, +TIMER, +RAM, +BATTERY). Battery contents and the clock
 * are not persisted. Files too small to
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

/*
 * Advances the cartridge clock (MBC3 real-time clock) by `cycles` T-cycles.
 * Cartridges without a clock ignore it.
 */
void cartridge_step(Cartridge *cartridge, CpuCycles cycles);

/* A000-BFFF. Reads 0xFF and ignores writes without enabled RAM. */
uint8_t cartridge_read_ram(const Cartridge *cartridge, uint16_t address);
void cartridge_write_ram(Cartridge *cartridge, uint16_t address, uint8_t value);

#endif
