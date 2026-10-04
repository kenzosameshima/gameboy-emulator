#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <cartridge.h>
#include <memory_map.h>

#include "mapper.h"

enum {
    CARTRIDGE_TYPE_ROM_ONLY = 0x00,
    CARTRIDGE_TYPE_MBC1 = 0x01,
    CARTRIDGE_TYPE_MBC1_RAM = 0x02,
    CARTRIDGE_TYPE_MBC1_RAM_BATTERY = 0x03,
    CARTRIDGE_TYPE_MBC3_TIMER_BATTERY = 0x0F,
    CARTRIDGE_TYPE_MBC3_TIMER_RAM_BATTERY = 0x10,
    CARTRIDGE_TYPE_MBC3 = 0x11,
    CARTRIDGE_TYPE_MBC3_RAM = 0x12,
    CARTRIDGE_TYPE_MBC3_RAM_BATTERY = 0x13
};

static size_t cartridge_ram_size_from_header(uint8_t code)
{
    switch (code) {
        case 0x01:
            return 0x0800;

        case 0x02:
            return 0x2000;

        case 0x03:
            return 0x8000;

        default:
            return 0;
    }
}

/* Reads the header. Returns false for unsupported cartridge types. */
static bool cartridge_configure(
    const uint8_t *rom,
    size_t rom_size,
    CartridgeMapper *mapper,
    size_t *ram_size
)
{
    *mapper = CARTRIDGE_MAPPER_NONE;
    *ram_size = 0;

    if (rom_size <= CARTRIDGE_HEADER_RAM_SIZE) {
        return true;
    }

    switch (rom[CARTRIDGE_HEADER_TYPE]) {
        case CARTRIDGE_TYPE_ROM_ONLY:
            return true;

        case CARTRIDGE_TYPE_MBC1:
            *mapper = CARTRIDGE_MAPPER_MBC1;
            return true;

        case CARTRIDGE_TYPE_MBC1_RAM:
        case CARTRIDGE_TYPE_MBC1_RAM_BATTERY:
            *mapper = CARTRIDGE_MAPPER_MBC1;
            *ram_size = cartridge_ram_size_from_header(
                rom[CARTRIDGE_HEADER_RAM_SIZE]
            );
            return true;

        case CARTRIDGE_TYPE_MBC3_TIMER_BATTERY:
        case CARTRIDGE_TYPE_MBC3:
            *mapper = CARTRIDGE_MAPPER_MBC3;
            return true;

        case CARTRIDGE_TYPE_MBC3_TIMER_RAM_BATTERY:
        case CARTRIDGE_TYPE_MBC3_RAM:
        case CARTRIDGE_TYPE_MBC3_RAM_BATTERY:
            *mapper = CARTRIDGE_MAPPER_MBC3;
            *ram_size = cartridge_ram_size_from_header(
                rom[CARTRIDGE_HEADER_RAM_SIZE]
            );
            return true;

        default:
            return false;
    }
}

/* Number of 16 KiB banks, rounded up to a power of two, minus one. */
static size_t cartridge_rom_bank_mask(size_t rom_size)
{
    size_t banks = 1;

    while (banks * CARTRIDGE_ROM_BANK_SIZE < rom_size) {
        banks <<= 1;
    }

    return banks - 1;
}

static const MapperOps *const MAPPER_TABLE[] = {
    [CARTRIDGE_MAPPER_NONE] = &MAPPER_ROM_ONLY,
    [CARTRIDGE_MAPPER_MBC1] = &MAPPER_MBC1,
    [CARTRIDGE_MAPPER_MBC3] = &MAPPER_MBC3
};

const MapperOps *cartridge_mapper_ops(CartridgeMapper mapper)
{
    return MAPPER_TABLE[mapper];
}

uint8_t cartridge_rom_byte(const Cartridge *cartridge, size_t offset)
{
    return offset < cartridge->rom_size ? cartridge->rom[offset] : 0xFF;
}

void cartridge_init(Cartridge *cartridge)
{
    if (cartridge == NULL) {
        return;
    }

    cartridge->rom = NULL;
    cartridge->rom_size = 0;
    cartridge->rom_bank_mask = 0;
    cartridge->mapper = CARTRIDGE_MAPPER_NONE;
    cartridge->ram = NULL;
    cartridge->ram_size = 0;
    memset(&cartridge->state, 0, sizeof(cartridge->state));
}

CartridgeLoadStatus cartridge_load(
    Cartridge *cartridge,
    const char *path,
    uint8_t *unsupported_type
)
{
    if (cartridge == NULL || path == NULL) {
        return CARTRIDGE_LOAD_INVALID_ARGUMENT;
    }

    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return CARTRIDGE_LOAD_IO_ERROR;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return CARTRIDGE_LOAD_IO_ERROR;
    }

    long size = ftell(file);

    if (size <= 0) {
        fclose(file);
        return CARTRIDGE_LOAD_IO_ERROR;
    }

    rewind(file);

    size_t rom_size = (size_t)size;

    uint8_t *rom = malloc(rom_size);

    if (rom == NULL) {
        fclose(file);
        return CARTRIDGE_LOAD_OUT_OF_MEMORY;
    }

    size_t read = fread(
        rom,
        1,
        rom_size,
        file
    );

    fclose(file);

    if (read != rom_size) {
        free(rom);

        return CARTRIDGE_LOAD_IO_ERROR;
    }

    CartridgeMapper mapper;
    size_t ram_size;

    if (!cartridge_configure(rom, rom_size, &mapper, &ram_size)) {
        /* Only a ROM with a complete header can be unsupported. */
        if (unsupported_type != NULL) {
            *unsupported_type = rom[CARTRIDGE_HEADER_TYPE];
        }

        free(rom);

        return CARTRIDGE_LOAD_UNSUPPORTED_TYPE;
    }

    uint8_t *ram = NULL;

    if (ram_size != 0) {
        ram = calloc(ram_size, sizeof(uint8_t));

        if (ram == NULL) {
            free(rom);

            return CARTRIDGE_LOAD_OUT_OF_MEMORY;
        }
    }

    cartridge_destroy(cartridge);
    cartridge_init(cartridge);
    cartridge->rom = rom;
    cartridge->rom_size = rom_size;
    cartridge->rom_bank_mask = cartridge_rom_bank_mask(rom_size);
    cartridge->mapper = mapper;
    cartridge->ram = ram;
    cartridge->ram_size = ram_size;
    cartridge_mapper_ops(mapper)->reset(cartridge);

    return CARTRIDGE_LOAD_OK;
}

void cartridge_destroy(Cartridge *cartridge)
{
    if (cartridge == NULL) {
        return;
    }

    free(cartridge->rom);
    free(cartridge->ram);

    cartridge->rom = NULL;
    cartridge->rom_size = 0;
    cartridge->rom_bank_mask = 0;
    cartridge->ram = NULL;
    cartridge->ram_size = 0;
}

uint8_t cartridge_read(
    const Cartridge *cartridge,
    uint16_t address
)
{
    if (cartridge == NULL ||
        cartridge->rom == NULL ||
        address > MEM_ROM_END) {
        return 0xFF;
    }

    return cartridge_mapper_ops(cartridge->mapper)->read_rom(
        cartridge,
        address
    );
}

void cartridge_write(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    if (cartridge == NULL || address > MEM_ROM_END) {
        return;
    }

    cartridge_mapper_ops(cartridge->mapper)->write_rom(
        cartridge,
        address,
        value
    );
}

void cartridge_step(Cartridge *cartridge, CpuCycles cycles)
{
    if (cartridge == NULL) {
        return;
    }

    const MapperOps *ops = cartridge_mapper_ops(cartridge->mapper);

    if (ops->step != NULL) {
        ops->step(cartridge, cycles);
    }
}

uint8_t cartridge_read_ram(
    const Cartridge *cartridge,
    uint16_t address
)
{
    if (cartridge == NULL ||
        address < MEM_CART_RAM_START ||
        address > MEM_CART_RAM_END) {
        return 0xFF;
    }

    return cartridge_mapper_ops(cartridge->mapper)->read_ram(
        cartridge,
        address
    );
}

void cartridge_write_ram(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    if (cartridge == NULL ||
        address < MEM_CART_RAM_START ||
        address > MEM_CART_RAM_END) {
        return;
    }

    cartridge_mapper_ops(cartridge->mapper)->write_ram(
        cartridge,
        address,
        value
    );
}
