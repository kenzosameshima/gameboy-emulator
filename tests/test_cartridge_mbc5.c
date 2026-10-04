/*
 * MBC5 through the public cartridge interface: 9-bit ROM banking up to
 * 8 MiB (bank 0 can be selected into the switchable window) and RAM
 * banking up to 128 KiB.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cartridge.h>

#include "test_util.h"

enum {
    BANK_SIZE = 0x4000,

    TYPE_MBC5 = 0x19,
    TYPE_MBC5_RAM_BATTERY = 0x1B,
    TYPE_MBC5_RUMBLE_RAM = 0x1D,
    RAM_CODE_8K = 0x02,
    RAM_CODE_128K = 0x04
};

static const char *rom_path = "test_mbc5_rom.gb";

/*
 * Every 16 KiB bank starts with its own number as two bytes, low then
 * high, so banks above 255 can be told apart.
 */
static void load(Cartridge *cartridge, unsigned banks, uint8_t type,
                 uint8_t ram_code)
{
    size_t size = (size_t)banks * BANK_SIZE;
    uint8_t *rom = calloc(size, sizeof(uint8_t));

    assert(rom != NULL);

    for (unsigned bank = 0; bank < banks; bank++) {
        rom[(size_t)bank * BANK_SIZE] = (uint8_t)(bank & 0xFF);
        rom[(size_t)bank * BANK_SIZE + 1] = (uint8_t)(bank >> 8);
    }

    rom[CARTRIDGE_HEADER_TYPE] = type;
    rom[CARTRIDGE_HEADER_RAM_SIZE] = ram_code;
    test_write_file(rom_path, rom, size);
    free(rom);

    cartridge_init(cartridge);
    assert(cartridge_load(cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK);
    assert(cartridge->mapper == CARTRIDGE_MAPPER_MBC5);
}

static unsigned bank_at_4000(const Cartridge *cartridge)
{
    return cartridge_read(cartridge, 0x4000) |
           ((unsigned)cartridge_read(cartridge, 0x4001) << 8);
}

static void select_rom_bank(Cartridge *cartridge, unsigned bank)
{
    cartridge_write(cartridge, 0x2000, (uint8_t)(bank & 0xFF));
    cartridge_write(cartridge, 0x3000, (uint8_t)(bank >> 8));
}

static void test_rom_banking(void)
{
    Cartridge cartridge;

    load(&cartridge, 512, TYPE_MBC5, 0x00);

    /* Bank 1 is the power-on bank, and bank 0 is fixed below it. */
    assert(bank_at_4000(&cartridge) == 1);
    assert(cartridge_read(&cartridge, 0x0000) == 0);

    /* Unlike MBC1 and MBC3, bank 0 can be selected into the window. */
    select_rom_bank(&cartridge, 0);
    assert(bank_at_4000(&cartridge) == 0);

    static const unsigned banks[] = { 1, 2, 127, 128, 255, 256, 257, 400, 511 };

    for (size_t i = 0; i < sizeof(banks) / sizeof(banks[0]); i++) {
        select_rom_bank(&cartridge, banks[i]);
        assert(bank_at_4000(&cartridge) == banks[i]);
        assert(cartridge_read(&cartridge, 0x0000) == 0);
    }

    /* The low and high bank registers are independent. */
    select_rom_bank(&cartridge, 0x1AB);
    cartridge_write(&cartridge, 0x2000, 0x05);
    assert(bank_at_4000(&cartridge) == 0x105);
    cartridge_write(&cartridge, 0x3000, 0x00);
    assert(bank_at_4000(&cartridge) == 0x005);

    /* Only bit 0 of the high register counts. */
    cartridge_write(&cartridge, 0x3000, 0xFE);
    assert(bank_at_4000(&cartridge) == 0x005);
    cartridge_write(&cartridge, 0x3000, 0xFF);
    assert(bank_at_4000(&cartridge) == 0x105);

    /* The two registers cover 2000-2FFF and 3000-3FFF. */
    cartridge_write(&cartridge, 0x2FFF, 0x07);
    assert(bank_at_4000(&cartridge) == 0x107);
    cartridge_write(&cartridge, 0x3FFF, 0x00);
    assert(bank_at_4000(&cartridge) == 0x007);

    cartridge_destroy(&cartridge);

    /* A smaller ROM wraps the bank number to its size. */
    load(&cartridge, 4, TYPE_MBC5, 0x00);
    select_rom_bank(&cartridge, 0x1FD);
    assert(bank_at_4000(&cartridge) == 1);
    select_rom_bank(&cartridge, 0x1FE);
    assert(bank_at_4000(&cartridge) == 2);

    cartridge_destroy(&cartridge);
}

static void test_ram_banking(void)
{
    Cartridge cartridge;

    load(&cartridge, 4, TYPE_MBC5_RAM_BATTERY, RAM_CODE_128K);
    assert(cartridge.ram_size == 0x20000);

    /* Disabled at power-on. */
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write_ram(&cartridge, 0xA000, 0x11);

    /* Only 0x0A enables it; MBC5 does not look at the low nibble alone. */
    cartridge_write(&cartridge, 0x0000, 0x2A);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write(&cartridge, 0x0000, 0x0A);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x00);

    for (uint8_t bank = 0; bank < 16; bank++) {
        cartridge_write(&cartridge, 0x4000, bank);
        cartridge_write_ram(&cartridge, 0xA000, (uint8_t)(0x40 + bank));
        cartridge_write_ram(&cartridge, 0xBFFF, (uint8_t)(0x80 + bank));
    }

    for (uint8_t bank = 0; bank < 16; bank++) {
        cartridge_write(&cartridge, 0x4000, bank);
        assert(cartridge_read_ram(&cartridge, 0xA000) == 0x40 + bank);
        assert(cartridge_read_ram(&cartridge, 0xBFFF) == 0x80 + bank);
    }

    /* Four bits select the bank; the rumble bit and above are ignored. */
    cartridge_write(&cartridge, 0x4000, 0x0A);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x4A);
    cartridge_write(&cartridge, 0x4000, 0xFA);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x4A);

    /* Disabling keeps the data. */
    cartridge_write(&cartridge, 0x0000, 0x00);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write(&cartridge, 0x0000, 0x0A);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x4A);

    cartridge_destroy(&cartridge);

    /* 8 KiB of RAM repeats for higher bank numbers. */
    load(&cartridge, 4, TYPE_MBC5_RUMBLE_RAM, RAM_CODE_8K);
    assert(cartridge.ram_size == 0x2000);
    cartridge_write(&cartridge, 0x0000, 0x0A);
    cartridge_write_ram(&cartridge, 0xA000, 0x5C);
    cartridge_write(&cartridge, 0x4000, 0x03);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x5C);

    cartridge_destroy(&cartridge);

    /* No RAM: nothing there. */
    load(&cartridge, 4, TYPE_MBC5, 0x00);
    cartridge_write(&cartridge, 0x0000, 0x0A);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);

    cartridge_destroy(&cartridge);
}

int main(void)
{
    test_rom_banking();
    test_ram_banking();

    assert(remove(rom_path) == 0);

    printf("MBC5 cartridge tests passed!\n");

    return 0;
}
