/*
 * MBC2 through the public cartridge interface: a 4-bit ROM bank register
 * and 512 half-bytes of built-in RAM, both programmed through writes to
 * 0000-3FFF where address bit 8 picks the register.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cartridge.h>

#include "test_util.h"

enum {
    BANK_SIZE = 0x4000,
    TYPE_MBC2 = 0x05,
    TYPE_MBC2_BATTERY = 0x06
};

static const char *rom_path = "test_mbc2_rom.gb";

/* Every 16 KiB bank starts with its own number. */
static void load(Cartridge *cartridge, unsigned banks, uint8_t type)
{
    size_t size = (size_t)banks * BANK_SIZE;
    uint8_t *rom = calloc(size, sizeof(uint8_t));

    assert(rom != NULL);

    for (unsigned bank = 0; bank < banks; bank++) {
        rom[(size_t)bank * BANK_SIZE] = (uint8_t)bank;
    }

    rom[CARTRIDGE_HEADER_TYPE] = type;
    /* MBC2 cartridges say 0 here: the RAM is inside the chip. */
    rom[CARTRIDGE_HEADER_RAM_SIZE] = 0x00;
    test_write_file(rom_path, rom, size);
    free(rom);

    cartridge_init(cartridge);
    assert(cartridge_load(cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK);
    assert(cartridge->mapper == CARTRIDGE_MAPPER_MBC2);
}

static void test_rom_banking(void)
{
    Cartridge cartridge;

    load(&cartridge, 16, TYPE_MBC2);

    assert(cartridge_read(&cartridge, 0x0000) == 0);
    assert(cartridge_read(&cartridge, 0x4000) == 1);

    /* Address bit 8 set selects the ROM bank register, 4 bits wide. */
    for (uint8_t bank = 1; bank < 16; bank++) {
        cartridge_write(&cartridge, 0x2100, bank);
        assert(cartridge_read(&cartridge, 0x4000) == bank);
        assert(cartridge_read(&cartridge, 0x0000) == 0);
    }

    /* Bank 0 reads as bank 1, and upper bits are ignored. */
    cartridge_write(&cartridge, 0x2100, 0x00);
    assert(cartridge_read(&cartridge, 0x4000) == 1);
    cartridge_write(&cartridge, 0x2100, 0xF5);
    assert(cartridge_read(&cartridge, 0x4000) == 5);
    cartridge_write(&cartridge, 0x2100, 0x10);
    assert(cartridge_read(&cartridge, 0x4000) == 1);

    /* The register answers everywhere in 0000-3FFF with bit 8 set. */
    cartridge_write(&cartridge, 0x0100, 0x07);
    assert(cartridge_read(&cartridge, 0x4000) == 7);
    cartridge_write(&cartridge, 0x3FFF, 0x09);
    assert(cartridge_read(&cartridge, 0x4000) == 9);

    /* Writes above 3FFF do nothing. */
    cartridge_write(&cartridge, 0x4100, 0x03);
    cartridge_write(&cartridge, 0x6100, 0x03);
    assert(cartridge_read(&cartridge, 0x4000) == 9);

    cartridge_destroy(&cartridge);

    /* A smaller ROM wraps the bank number. */
    load(&cartridge, 4, TYPE_MBC2);
    cartridge_write(&cartridge, 0x2100, 0x06);
    assert(cartridge_read(&cartridge, 0x4000) == 2);

    cartridge_destroy(&cartridge);
}

static void test_built_in_ram(void)
{
    Cartridge cartridge;

    load(&cartridge, 4, TYPE_MBC2_BATTERY);
    assert(cartridge.ram_size == 512);

    /* Disabled at power-on. */
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write_ram(&cartridge, 0xA000, 0x05);

    /* Address bit 8 clear selects the RAM enable; the low nibble must be
     * 0xA, and only the low nibble counts. */
    cartridge_write(&cartridge, 0x0000, 0x0B);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write(&cartridge, 0x0100, 0x0A);   /* bit 8 set: ROM bank */
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write(&cartridge, 0x00FF, 0xFA);
    assert((cartridge_read_ram(&cartridge, 0xA000) & 0x0F) == 0x00);

    /* Only 4 bits are stored; the top nibble reads as 1. */
    cartridge_write_ram(&cartridge, 0xA000, 0x05);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xF5);
    cartridge_write_ram(&cartridge, 0xA001, 0xA7);
    assert(cartridge_read_ram(&cartridge, 0xA001) == 0xF7);
    cartridge_write_ram(&cartridge, 0xA1FF, 0x3C);
    assert(cartridge_read_ram(&cartridge, 0xA1FF) == 0xFC);

    /* The 512 entries repeat through A000-BFFF. */
    assert(cartridge_read_ram(&cartridge, 0xA200) == 0xF5);
    assert(cartridge_read_ram(&cartridge, 0xA201) == 0xF7);
    assert(cartridge_read_ram(&cartridge, 0xBFFF) == 0xFC);
    cartridge_write_ram(&cartridge, 0xA400, 0x0E);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFE);

    /* Disabling keeps the data. */
    cartridge_write(&cartridge, 0x0000, 0x00);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write(&cartridge, 0x0000, 0x0A);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFE);

    cartridge_destroy(&cartridge);
}

int main(void)
{
    test_rom_banking();
    test_built_in_ram();

    assert(remove(rom_path) == 0);

    printf("MBC2 cartridge tests passed!\n");

    return 0;
}
