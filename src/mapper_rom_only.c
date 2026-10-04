#include <stdint.h>

#include "mapper.h"

/* A cartridge with no mapper: the ROM is wired straight to 0000-7FFF. */

static void rom_only_reset(Cartridge *cartridge)
{
    (void)cartridge;
}

static uint8_t rom_only_read_rom(const Cartridge *cartridge, uint16_t address)
{
    return cartridge_rom_byte(cartridge, address);
}

static void rom_only_write_rom(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    (void)cartridge;
    (void)address;
    (void)value;
}

static uint8_t rom_only_read_ram(const Cartridge *cartridge, uint16_t address)
{
    (void)cartridge;
    (void)address;

    return 0xFF;
}

static void rom_only_write_ram(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    (void)cartridge;
    (void)address;
    (void)value;
}

const MapperOps MAPPER_ROM_ONLY = {
    .reset = rom_only_reset,
    .read_rom = rom_only_read_rom,
    .write_rom = rom_only_write_rom,
    .read_ram = rom_only_read_ram,
    .write_ram = rom_only_write_ram
};
