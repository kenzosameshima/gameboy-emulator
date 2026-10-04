#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <memory_map.h>

#include "mapper.h"

/*
 * MBC1: up to 2 MiB of ROM in 16 KiB banks and 32 KiB of RAM in 8 KiB
 * banks. Register writes land in 0000-7FFF:
 *
 *   0000-1FFF  RAM enable (low nibble 0xA enables)
 *   2000-3FFF  ROM bank, low 5 bits (written 0 is stored as 1)
 *   4000-5FFF  ROM bank high 2 bits, or the RAM bank in banking mode 1
 *   6000-7FFF  banking mode (0 simple, 1 advanced)
 */

enum {
    MBC1_RAM_ENABLE_END = 0x1FFF,
    MBC1_ROM_BANK_END = 0x3FFF,
    MBC1_BANK_HIGH_END = 0x5FFF,
    MBC1_RAM_ENABLE_VALUE = 0x0A
};

static void mbc1_reset(Cartridge *cartridge)
{
    cartridge->state.mbc1 = (Mbc1State){
        .ram_enabled = false,
        .bank_low = 1,
        .bank_high = 0,
        .banking_mode = false
    };
}

static uint8_t mbc1_read_rom(const Cartridge *cartridge, uint16_t address)
{
    const Mbc1State *mbc1 = &cartridge->state.mbc1;
    size_t bank;

    if (address < CARTRIDGE_ROM_BANK_SIZE) {
        bank = mbc1->banking_mode ? (size_t)mbc1->bank_high << 5 : 0;
    } else {
        bank = ((size_t)mbc1->bank_high << 5) | mbc1->bank_low;
    }

    bank &= cartridge->rom_bank_mask;

    return cartridge_rom_byte(
        cartridge,
        bank * CARTRIDGE_ROM_BANK_SIZE +
            (address & (CARTRIDGE_ROM_BANK_SIZE - 1))
    );
}

static void mbc1_write_rom(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    Mbc1State *mbc1 = &cartridge->state.mbc1;

    if (address <= MBC1_RAM_ENABLE_END) {
        mbc1->ram_enabled = (value & 0x0F) == MBC1_RAM_ENABLE_VALUE;
    } else if (address <= MBC1_ROM_BANK_END) {
        uint8_t bank = (uint8_t)(value & 0x1F);

        mbc1->bank_low = bank == 0 ? 1 : bank;
    } else if (address <= MBC1_BANK_HIGH_END) {
        mbc1->bank_high = (uint8_t)(value & 0x03);
    } else {
        mbc1->banking_mode = (value & 0x01) != 0;
    }
}

static bool mbc1_ram_available(const Cartridge *cartridge)
{
    return cartridge->ram != NULL &&
           cartridge->ram_size != 0 &&
           cartridge->state.mbc1.ram_enabled;
}

static size_t mbc1_ram_offset(const Cartridge *cartridge, uint16_t address)
{
    const Mbc1State *mbc1 = &cartridge->state.mbc1;
    size_t bank = mbc1->banking_mode ? mbc1->bank_high : 0;
    size_t offset = bank * CARTRIDGE_RAM_BANK_SIZE +
                    (size_t)(address - MEM_CART_RAM_START);

    return offset % cartridge->ram_size;
}

static uint8_t mbc1_read_ram(const Cartridge *cartridge, uint16_t address)
{
    if (!mbc1_ram_available(cartridge)) {
        return 0xFF;
    }

    return cartridge->ram[mbc1_ram_offset(cartridge, address)];
}

static void mbc1_write_ram(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    if (!mbc1_ram_available(cartridge)) {
        return;
    }

    cartridge->ram[mbc1_ram_offset(cartridge, address)] = value;
}

const MapperOps MAPPER_MBC1 = {
    .reset = mbc1_reset,
    .read_rom = mbc1_read_rom,
    .write_rom = mbc1_write_rom,
    .read_ram = mbc1_read_ram,
    .write_ram = mbc1_write_ram
};
