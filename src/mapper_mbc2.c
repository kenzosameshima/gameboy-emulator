#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <memory_map.h>

#include "mapper.h"

/*
 * MBC2: up to 256 KiB of ROM in 16 banks, and 512 half-bytes of RAM built
 * into the chip. There is a single register area, 0000-3FFF, and address
 * bit 8 chooses the register:
 *
 *   bit 8 clear  RAM enable (low nibble 0xA enables)
 *   bit 8 set    ROM bank, 4 bits (written 0 is stored as 1)
 *
 * The RAM holds 4 bits per address, so the top nibble reads as 1, and its
 * 512 addresses repeat through A000-BFFF.
 */

enum {
    MBC2_REGISTER_END = 0x3FFF,
    MBC2_REGISTER_SELECT_BIT = 0x0100,
    MBC2_RAM_ENABLE_VALUE = 0x0A,
    MBC2_RAM_MASK = 0x01FF
};

static void mbc2_reset(Cartridge *cartridge)
{
    cartridge->state.mbc2 = (Mbc2State){
        .ram_enabled = false,
        .rom_bank = 1
    };
}

static uint8_t mbc2_read_rom(const Cartridge *cartridge, uint16_t address)
{
    size_t bank = 0;

    if (address >= CARTRIDGE_ROM_BANK_SIZE) {
        bank = cartridge->state.mbc2.rom_bank & cartridge->rom_bank_mask;
    }

    return cartridge_rom_byte(
        cartridge,
        bank * CARTRIDGE_ROM_BANK_SIZE +
            (address & (CARTRIDGE_ROM_BANK_SIZE - 1))
    );
}

static void mbc2_write_rom(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    Mbc2State *mbc2 = &cartridge->state.mbc2;

    if (address > MBC2_REGISTER_END) {
        return;
    }

    if ((address & MBC2_REGISTER_SELECT_BIT) == 0) {
        mbc2->ram_enabled = (value & 0x0F) == MBC2_RAM_ENABLE_VALUE;
    } else {
        uint8_t bank = (uint8_t)(value & 0x0F);

        mbc2->rom_bank = bank == 0 ? 1 : bank;
    }
}

static bool mbc2_ram_available(const Cartridge *cartridge)
{
    return cartridge->ram != NULL &&
           cartridge->ram_size != 0 &&
           cartridge->state.mbc2.ram_enabled;
}

static size_t mbc2_ram_offset(uint16_t address)
{
    return (size_t)(address - MEM_CART_RAM_START) & MBC2_RAM_MASK;
}

static uint8_t mbc2_read_ram(const Cartridge *cartridge, uint16_t address)
{
    if (!mbc2_ram_available(cartridge)) {
        return 0xFF;
    }

    return (uint8_t)(0xF0 | cartridge->ram[mbc2_ram_offset(address)]);
}

static void mbc2_write_ram(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    if (!mbc2_ram_available(cartridge)) {
        return;
    }

    cartridge->ram[mbc2_ram_offset(address)] = (uint8_t)(value & 0x0F);
}

const MapperOps MAPPER_MBC2 = {
    .reset = mbc2_reset,
    .read_rom = mbc2_read_rom,
    .write_rom = mbc2_write_rom,
    .read_ram = mbc2_read_ram,
    .write_ram = mbc2_write_ram
};
