#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <cartridge.h>
#include <memory_map.h>

enum {
    CARTRIDGE_TYPE_ROM_ONLY = 0x00,
    CARTRIDGE_TYPE_MBC1 = 0x01,
    CARTRIDGE_TYPE_MBC1_RAM = 0x02,
    CARTRIDGE_TYPE_MBC1_RAM_BATTERY = 0x03,

    MBC1_RAM_ENABLE_END = 0x1FFF,
    MBC1_ROM_BANK_END = 0x3FFF,
    MBC1_BANK_HIGH_END = 0x5FFF,
    MBC1_RAM_ENABLE_VALUE = 0x0A
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

static size_t cartridge_rom_offset(
    const Cartridge *cartridge,
    uint16_t address
)
{
    if (cartridge->mapper != CARTRIDGE_MAPPER_MBC1) {
        return address;
    }

    size_t bank;

    if (address < CARTRIDGE_ROM_BANK_SIZE) {
        bank = cartridge->banking_mode
            ? (size_t)cartridge->bank_high << 5
            : 0;
    } else {
        bank = ((size_t)cartridge->bank_high << 5) |
               cartridge->bank_low;
    }

    bank &= cartridge->rom_bank_mask;

    return bank * CARTRIDGE_ROM_BANK_SIZE +
           (address & (CARTRIDGE_ROM_BANK_SIZE - 1));
}

static bool cartridge_ram_available(const Cartridge *cartridge)
{
    return cartridge->ram != NULL &&
           cartridge->ram_size != 0 &&
           cartridge->ram_enabled;
}

static size_t cartridge_ram_offset(
    const Cartridge *cartridge,
    uint16_t address
)
{
    size_t bank = cartridge->banking_mode ? cartridge->bank_high : 0;
    size_t offset = bank * CARTRIDGE_RAM_BANK_SIZE +
                    (size_t)(address - MEM_CART_RAM_START);

    return offset % cartridge->ram_size;
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
    cartridge->ram_enabled = false;
    cartridge->bank_low = 1;
    cartridge->bank_high = 0;
    cartridge->banking_mode = false;
}

int cartridge_load(Cartridge *cartridge, const char *path)
{
    if (cartridge == NULL || path == NULL) {
        return 0;
    }

    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }

    long size = ftell(file);

    if (size <= 0) {
        fclose(file);
        return 0;
    }

    rewind(file);

    size_t rom_size = (size_t)size;

    uint8_t *rom = malloc(rom_size);

    if (rom == NULL) {
        fclose(file);
        return 0;
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

        return 0;
    }

    CartridgeMapper mapper;
    size_t ram_size;

    if (!cartridge_configure(rom, rom_size, &mapper, &ram_size)) {
        free(rom);

        return 0;
    }

    uint8_t *ram = NULL;

    if (ram_size != 0) {
        ram = calloc(ram_size, sizeof(uint8_t));

        if (ram == NULL) {
            free(rom);

            return 0;
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

    return 1;
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

    size_t offset = cartridge_rom_offset(cartridge, address);

    if (offset >= cartridge->rom_size) {
        return 0xFF;
    }

    return cartridge->rom[offset];
}

void cartridge_write(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    if (cartridge == NULL ||
        cartridge->mapper != CARTRIDGE_MAPPER_MBC1 ||
        address > MEM_ROM_END) {
        return;
    }

    if (address <= MBC1_RAM_ENABLE_END) {
        cartridge->ram_enabled =
            (value & 0x0F) == MBC1_RAM_ENABLE_VALUE;
    } else if (address <= MBC1_ROM_BANK_END) {
        uint8_t bank = (uint8_t)(value & 0x1F);

        cartridge->bank_low = bank == 0 ? 1 : bank;
    } else if (address <= MBC1_BANK_HIGH_END) {
        cartridge->bank_high = (uint8_t)(value & 0x03);
    } else {
        cartridge->banking_mode = (value & 0x01) != 0;
    }
}

uint8_t cartridge_read_ram(
    const Cartridge *cartridge,
    uint16_t address
)
{
    if (cartridge == NULL || !cartridge_ram_available(cartridge)) {
        return 0xFF;
    }

    return cartridge->ram[cartridge_ram_offset(cartridge, address)];
}

void cartridge_write_ram(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    if (cartridge == NULL || !cartridge_ram_available(cartridge)) {
        return;
    }

    cartridge->ram[cartridge_ram_offset(cartridge, address)] = value;
}
