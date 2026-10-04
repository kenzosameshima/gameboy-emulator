#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bus.h>
#include <cartridge.h>
#include <interrupts.h>
#include <memory.h>

#include "test_util.h"

enum {
    BANK_SIZE = 0x4000,
    TYPE_MBC1_RAM_BATTERY = 0x03,
    RAM_CODE_32K = 0x03
};

static const char *rom_path = "test_mbc1_rom.gb";

/*
 * Every 16 KiB bank starts with its own number, and the byte at offset 1
 * holds the bank number plus 0x80 so bank 0 is distinguishable from a
 * zero fill.
 */
static void write_banked_rom(
    unsigned banks,
    uint8_t cartridge_type,
    uint8_t ram_code
)
{
    size_t size = (size_t)banks * BANK_SIZE;
    uint8_t *rom = calloc(size, sizeof(uint8_t));

    assert(rom != NULL);

    for (unsigned bank = 0; bank < banks; bank++) {
        rom[(size_t)bank * BANK_SIZE] = (uint8_t)bank;
        rom[(size_t)bank * BANK_SIZE + 1] = (uint8_t)(bank + 0x80);
    }

    rom[CARTRIDGE_HEADER_TYPE] = cartridge_type;
    rom[CARTRIDGE_HEADER_RAM_SIZE] = ram_code;

    test_write_file(rom_path, rom, size);
    free(rom);
}

static void test_rom_banking(void)
{
    Cartridge cartridge;

    write_banked_rom(64, TYPE_MBC1_RAM_BATTERY, RAM_CODE_32K);
    cartridge_init(&cartridge);
    assert(
        cartridge_load(&cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK
    );
    assert(cartridge.mapper == CARTRIDGE_MAPPER_MBC1);
    assert(cartridge.ram_size == 0x8000);

    /* Power-on: bank 0 fixed, bank 1 switchable. */
    assert(cartridge_read(&cartridge, 0x0000) == 0);
    assert(cartridge_read(&cartridge, 0x4000) == 1);
    assert(cartridge_read(&cartridge, 0x4001) == 0x81);

    cartridge_write(&cartridge, 0x2000, 5);
    assert(cartridge_read(&cartridge, 0x4000) == 5);
    assert(cartridge_read(&cartridge, 0x0000) == 0);

    /* Only the low 5 bits count, and a bank number of 0 becomes 1. */
    cartridge_write(&cartridge, 0x3FFF, 0xE7);
    assert(cartridge_read(&cartridge, 0x4000) == 7);

    cartridge_write(&cartridge, 0x2000, 0x00);
    assert(cartridge_read(&cartridge, 0x4000) == 1);

    cartridge_write(&cartridge, 0x2000, 0x20);
    assert(cartridge_read(&cartridge, 0x4000) == 1);

    /* The 2-bit upper register extends the switchable bank... */
    cartridge_write(&cartridge, 0x2000, 2);
    cartridge_write(&cartridge, 0x4000, 1);
    assert(cartridge_read(&cartridge, 0x4000) == 34);

    cartridge_write(&cartridge, 0x2000, 0x1F);
    cartridge_write(&cartridge, 0x5FFF, 3);
    assert(cartridge_read(&cartridge, 0x4000) == 63);

    /* ...but only moves 0000-3FFF in advanced banking mode. */
    assert(cartridge_read(&cartridge, 0x0000) == 0);

    cartridge_write(&cartridge, 0x6000, 1);
    /* Upper register 3 selects bank 96, which wraps to 32 in a 64-bank ROM. */
    assert(cartridge_read(&cartridge, 0x0000) == 32);
    assert(cartridge_read(&cartridge, 0x0001) == 0xA0);

    cartridge_write(&cartridge, 0x7FFF, 0);
    assert(cartridge_read(&cartridge, 0x0000) == 0);

    /* Outside the ROM window there is nothing to read or program. */
    assert(cartridge_read(&cartridge, 0x8000) == 0xFF);
    cartridge_write(&cartridge, 0x8000, 0x01);
    assert(cartridge_read(&cartridge, 0x4000) == 63);

    cartridge_destroy(&cartridge);
    remove(rom_path);
}

static void test_bank_numbers_wrap_to_rom_size(void)
{
    Cartridge cartridge;

    /* 8 banks: the bank register is masked with 7. */
    write_banked_rom(8, 0x01, 0x00);
    cartridge_init(&cartridge);
    assert(
        cartridge_load(&cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK
    );
    assert(cartridge.ram_size == 0);

    cartridge_write(&cartridge, 0x2000, 9);
    assert(cartridge_read(&cartridge, 0x4000) == 1);

    cartridge_write(&cartridge, 0x2000, 0x1F);
    assert(cartridge_read(&cartridge, 0x4000) == 7);

    cartridge_destroy(&cartridge);
    remove(rom_path);
}

static void test_ram_banking(void)
{
    Cartridge cartridge;

    write_banked_rom(4, TYPE_MBC1_RAM_BATTERY, RAM_CODE_32K);
    cartridge_init(&cartridge);
    assert(
        cartridge_load(&cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK
    );

    /* RAM is disabled at power-on: reads float high, writes are lost. */
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write_ram(&cartridge, 0xA000, 0x11);

    cartridge_write(&cartridge, 0x0000, 0x0A);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x00);

    cartridge_write_ram(&cartridge, 0xA000, 0x11);
    cartridge_write_ram(&cartridge, 0xBFFF, 0x22);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x11);
    assert(cartridge_read_ram(&cartridge, 0xBFFF) == 0x22);

    /* In simple mode the RAM bank stays 0 whatever the upper register. */
    cartridge_write(&cartridge, 0x4000, 2);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x11);

    /* In advanced mode the upper register selects the RAM bank. */
    cartridge_write(&cartridge, 0x6000, 1);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x00);
    cartridge_write_ram(&cartridge, 0xA000, 0x33);

    cartridge_write(&cartridge, 0x4000, 0);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x11);

    cartridge_write(&cartridge, 0x4000, 2);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x33);

    /* Any value other than 0x0A in the low nibble disables it again. */
    cartridge_write(&cartridge, 0x1FFF, 0x00);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);

    cartridge_write(&cartridge, 0x0000, 0xFA);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x33);

    cartridge_destroy(&cartridge);
    remove(rom_path);
}

static void test_bus_routes_mapper_and_ram(void)
{
    Cartridge cartridge;
    Memory memory;
    InterruptRegisters interrupts;
    Bus bus;

    write_banked_rom(4, TYPE_MBC1_RAM_BATTERY, RAM_CODE_32K);
    cartridge_init(&cartridge);
    assert(
        cartridge_load(&cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK
    );

    memory_init(&memory);
    interrupts_init(&interrupts);
    bus_init(&bus, &cartridge, &memory, &interrupts);

    /* ROM writes reach the mapper... */
    bus_write(&bus, 0x2000, 3);
    assert(bus_read(&bus, 0x4000) == 3);

    /* ...and A000-BFFF reaches cartridge RAM once it is enabled. */
    bus_write(&bus, 0xA123, 0x44);
    assert(bus_read(&bus, 0xA123) == 0xFF);

    bus_write(&bus, 0x0000, 0x0A);
    bus_write(&bus, 0xA123, 0x44);
    assert(bus_read(&bus, 0xA123) == 0x44);

    /* Cartridge RAM does not alias work RAM. */
    assert(bus_read(&bus, 0xC123) == 0x00);

    cartridge_destroy(&cartridge);
    remove(rom_path);
}

static void test_unsupported_cartridge_is_rejected(void)
{
    Cartridge cartridge;

    write_banked_rom(4, TYPE_MBC1_RAM_BATTERY, RAM_CODE_32K);
    cartridge_init(&cartridge);
    assert(
        cartridge_load(&cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK
    );

    uint8_t *loaded_rom = cartridge.rom;

    /* 0x20 is MBC6, which is not implemented. */
    write_banked_rom(4, 0x20, 0x00);
    uint8_t unsupported_type = 0;

    assert(
        cartridge_load(&cartridge, rom_path, &unsupported_type) ==
        CARTRIDGE_LOAD_UNSUPPORTED_TYPE
    );
    assert(unsupported_type == 0x20);

    /* The type output is optional. */
    assert(
        cartridge_load(&cartridge, rom_path, NULL) ==
        CARTRIDGE_LOAD_UNSUPPORTED_TYPE
    );

    /* A failed load leaves the previous cartridge intact. */
    assert(cartridge.rom == loaded_rom);
    assert(cartridge.mapper == CARTRIDGE_MAPPER_MBC1);
    assert(cartridge.ram_size == 0x8000);

    cartridge_destroy(&cartridge);
    remove(rom_path);
}

/*
 * A multicart packs up to four 256 KiB games into one 1 MiB MBC1 ROM, each
 * starting with its own header. The bank registers are wired differently:
 * the 4 low bits pick a bank inside the game and the 2 high bits the game,
 * shifted by 4 instead of 5. Such a ROM is recognised by the Nintendo logo
 * at the start of more than one of its 256 KiB sections.
 */
static void write_multicart_rom(int with_logos)
{
    enum { BANKS = 64 };

    size_t size = (size_t)BANKS * BANK_SIZE;
    uint8_t *rom = calloc(size, sizeof(uint8_t));

    assert(rom != NULL);

    for (unsigned bank = 0; bank < BANKS; bank++) {
        rom[(size_t)bank * BANK_SIZE] = (uint8_t)bank;
    }

    if (with_logos) {
        static const uint8_t logo[48] = {
            0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
            0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
            0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
            0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
            0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
            0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
        };

        for (unsigned game = 0; game < 4; game++) {
            memcpy(&rom[(size_t)game * 16 * BANK_SIZE + 0x104], logo,
                   sizeof(logo));
        }
    }

    rom[CARTRIDGE_HEADER_TYPE] = 0x01;
    test_write_file(rom_path, rom, size);
    free(rom);
}

static void test_multicart_wiring(void)
{
    Cartridge cartridge;

    write_multicart_rom(1);
    cartridge_init(&cartridge);
    assert(
        cartridge_load(&cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK
    );

    for (unsigned game = 0; game < 4; game++) {
        cartridge_write(&cartridge, 0x4000, (uint8_t)game);

        /* Low bits 0 become 1; each game has 16 banks. */
        cartridge_write(&cartridge, 0x2000, 0x00);
        assert(cartridge_read(&cartridge, 0x4000) == game * 16 + 1);

        cartridge_write(&cartridge, 0x2000, 0x05);
        assert(cartridge_read(&cartridge, 0x4000) == game * 16 + 5);

        /* Only 4 bits count, but the 0 -> 1 remap looks at all 5: writing
         * 0x10 selects bank 0 of the game, not bank 1. */
        cartridge_write(&cartridge, 0x2000, 0x10);
        assert(cartridge_read(&cartridge, 0x4000) == game * 16);

        cartridge_write(&cartridge, 0x2000, 0x1F);
        assert(cartridge_read(&cartridge, 0x4000) == game * 16 + 15);

        /* Advanced mode puts the game's first bank at 0000-3FFF. */
        cartridge_write(&cartridge, 0x6000, 0x01);
        assert(cartridge_read(&cartridge, 0x0000) == game * 16);
        cartridge_write(&cartridge, 0x6000, 0x00);
        assert(cartridge_read(&cartridge, 0x0000) == 0);
    }

    cartridge_destroy(&cartridge);

    /* The same size without the logos is an ordinary MBC1 ROM. */
    write_multicart_rom(0);
    cartridge_init(&cartridge);
    assert(
        cartridge_load(&cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK
    );
    cartridge_write(&cartridge, 0x4000, 0x01);
    cartridge_write(&cartridge, 0x2000, 0x05);
    assert(cartridge_read(&cartridge, 0x4000) == 32 + 5);

    cartridge_destroy(&cartridge);
    remove(rom_path);
}

static void test_rom_only_ignores_mapper_writes(void)
{
    Cartridge cartridge;

    write_banked_rom(2, 0x00, 0x00);
    cartridge_init(&cartridge);
    assert(
        cartridge_load(&cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK
    );
    assert(cartridge.mapper == CARTRIDGE_MAPPER_NONE);

    cartridge_write(&cartridge, 0x2000, 0x05);
    assert(cartridge_read(&cartridge, 0x4000) == 1);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);

    /* RAM accesses outside A000-BFFF never reach the mapper. */
    cartridge_write_ram(&cartridge, 0x9FFF, 0x12);
    cartridge_write_ram(&cartridge, 0xC000, 0x12);
    assert(cartridge_read_ram(&cartridge, 0x9FFF) == 0xFF);
    assert(cartridge_read_ram(&cartridge, 0xC000) == 0xFF);

    cartridge_destroy(&cartridge);
    remove(rom_path);
}

int main(void)
{
    test_rom_banking();
    test_bank_numbers_wrap_to_rom_size();
    test_ram_banking();
    test_bus_routes_mapper_and_ram();
    test_unsupported_cartridge_is_rejected();
    test_rom_only_ignores_mapper_writes();
    test_multicart_wiring();

    printf("MBC1 cartridge tests passed!\n");

    return 0;
}
