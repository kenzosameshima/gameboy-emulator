#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

/*
 * A multicart packs up to four 256 KiB games into a 1 MiB ROM, each starting
 * with its own header. Its bank registers are wired differently: the 4 low
 * bits pick a bank inside the game and the 2 high bits the game, shifted by
 * 4 rather than 5. It is recognised by the Nintendo logo at the start of
 * more than one of those 256 KiB sections.
 */
enum {
    MBC1_MULTICART_ROM_SIZE = 0x100000,
    MBC1_MULTICART_SECTION_SIZE = 0x40000,
    MBC1_LOGO_OFFSET = 0x104,
    MBC1_LOGO_SIZE = 48
};

static const uint8_t NINTENDO_LOGO[MBC1_LOGO_SIZE] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
};

static bool mbc1_is_multicart(const Cartridge *cartridge)
{
    if (cartridge->rom_size != MBC1_MULTICART_ROM_SIZE) {
        return false;
    }

    unsigned logos = 0;

    for (size_t section = 0; section < 4; section++) {
        const uint8_t *header = cartridge->rom +
            section * MBC1_MULTICART_SECTION_SIZE + MBC1_LOGO_OFFSET;

        if (memcmp(header, NINTENDO_LOGO, MBC1_LOGO_SIZE) == 0) {
            logos++;
        }
    }

    return logos >= 2;
}

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
        .banking_mode = false,
        .multicart = mbc1_is_multicart(cartridge)
    };
}

static uint8_t mbc1_read_rom(const Cartridge *cartridge, uint16_t address)
{
    const Mbc1State *mbc1 = &cartridge->state.mbc1;
    unsigned high_shift = mbc1->multicart ? 4 : 5;
    size_t bank;

    if (address < CARTRIDGE_ROM_BANK_SIZE) {
        bank = mbc1->banking_mode ? (size_t)mbc1->bank_high << high_shift : 0;
    } else {
        /* The 0 -> 1 remap was applied to all 5 bits when written; a
         * multicart then uses only the low 4. */
        size_t low = mbc1->multicart ? mbc1->bank_low & 0x0F
                                     : mbc1->bank_low;

        bank = ((size_t)mbc1->bank_high << high_shift) | low;
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
