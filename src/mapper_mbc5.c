#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <memory_map.h>

#include "mapper.h"

/*
 * MBC5: up to 8 MiB of ROM in 16 KiB banks and 128 KiB of RAM in 8 KiB
 * banks. Register writes land in 0000-5FFF:
 *
 *   0000-1FFF  RAM enable (exactly 0x0A enables)
 *   2000-2FFF  ROM bank, low 8 bits
 *   3000-3FFF  ROM bank, bit 8 (bit 0 of the value)
 *   4000-5FFF  RAM bank, 4 bits (bit 3 drives the rumble motor on rumble
 *              cartridges, which this emulator ignores)
 *
 * Unlike MBC1 and MBC3, bank 0 is an ordinary choice for the switchable
 * window, so there is no 0 -> 1 remap.
 */

enum {
    MBC5_RAM_ENABLE_END = 0x1FFF,
    MBC5_ROM_BANK_LOW_END = 0x2FFF,
    MBC5_ROM_BANK_HIGH_END = 0x3FFF,
    MBC5_RAM_BANK_END = 0x5FFF,
    MBC5_RAM_ENABLE_VALUE = 0x0A
};

static void mbc5_reset(Cartridge *cartridge)
{
    cartridge->state.mbc5 = (Mbc5State){
        .ram_enabled = false,
        .rom_bank = 1,
        .ram_bank = 0
    };
}

static uint8_t mbc5_read_rom(const Cartridge *cartridge, uint16_t address)
{
    size_t bank = 0;

    if (address >= CARTRIDGE_ROM_BANK_SIZE) {
        bank = cartridge->state.mbc5.rom_bank & cartridge->rom_bank_mask;
    }

    return cartridge_rom_byte(
        cartridge,
        bank * CARTRIDGE_ROM_BANK_SIZE +
            (address & (CARTRIDGE_ROM_BANK_SIZE - 1))
    );
}

static void mbc5_write_rom(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    Mbc5State *mbc5 = &cartridge->state.mbc5;

    if (address <= MBC5_RAM_ENABLE_END) {
        mbc5->ram_enabled = value == MBC5_RAM_ENABLE_VALUE;
    } else if (address <= MBC5_ROM_BANK_LOW_END) {
        mbc5->rom_bank = (uint16_t)((mbc5->rom_bank & 0x100) | value);
    } else if (address <= MBC5_ROM_BANK_HIGH_END) {
        mbc5->rom_bank = (uint16_t)(
            (mbc5->rom_bank & 0x0FF) | ((value & 0x01) << 8)
        );
    } else if (address <= MBC5_RAM_BANK_END) {
        mbc5->ram_bank = (uint8_t)(value & 0x0F);
    }
}

static bool mbc5_ram_available(const Cartridge *cartridge)
{
    return cartridge->ram != NULL &&
           cartridge->ram_size != 0 &&
           cartridge->state.mbc5.ram_enabled;
}

static size_t mbc5_ram_offset(const Cartridge *cartridge, uint16_t address)
{
    size_t offset = (size_t)cartridge->state.mbc5.ram_bank *
                        CARTRIDGE_RAM_BANK_SIZE +
                    (size_t)(address - MEM_CART_RAM_START);

    return offset % cartridge->ram_size;
}

static uint8_t mbc5_read_ram(const Cartridge *cartridge, uint16_t address)
{
    if (!mbc5_ram_available(cartridge)) {
        return 0xFF;
    }

    return cartridge->ram[mbc5_ram_offset(cartridge, address)];
}

static void mbc5_write_ram(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    if (!mbc5_ram_available(cartridge)) {
        return;
    }

    cartridge->ram[mbc5_ram_offset(cartridge, address)] = value;
}

const MapperOps MAPPER_MBC5 = {
    .reset = mbc5_reset,
    .read_rom = mbc5_read_rom,
    .write_rom = mbc5_write_rom,
    .read_ram = mbc5_read_ram,
    .write_ram = mbc5_write_ram
};
