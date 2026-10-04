/*
 * Battery saves through the public cartridge interface: which cartridges
 * have one, the file format (a raw dump of the cartridge RAM, plus a 48-byte
 * clock footer for MBC3 cartridges with a timer, as BGB and VBA-M write it),
 * and how the clock catches up for the time the console was off.
 *
 * The core never reads a wall clock: the caller passes the time in.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cartridge.h>

#include "test_util.h"

enum {
    BANK_SIZE = 0x4000,
    FOOTER_SIZE = 48,

    TYPE_ROM_ONLY = 0x00,
    TYPE_MBC1_RAM = 0x02,
    TYPE_MBC1_RAM_BATTERY = 0x03,
    TYPE_MBC2_BATTERY = 0x06,
    TYPE_MBC3_TIMER_RAM_BATTERY = 0x10,
    TYPE_MBC3_RAM_BATTERY = 0x13,
    TYPE_MBC5_RAM_BATTERY = 0x1B,

    RAM_CODE_8K = 0x02,
    RAM_CODE_32K = 0x03
};

static const char *rom_path = "test_save_rom.gb";
static const char *save_path = "test_save.sav";

static void load(Cartridge *cartridge, uint8_t type, uint8_t ram_code)
{
    size_t size = 4 * BANK_SIZE;
    uint8_t *rom = calloc(size, sizeof(uint8_t));

    assert(rom != NULL);
    rom[CARTRIDGE_HEADER_TYPE] = type;
    rom[CARTRIDGE_HEADER_RAM_SIZE] = ram_code;
    test_write_file(rom_path, rom, size);
    free(rom);

    cartridge_init(cartridge);
    assert(cartridge_load(cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK);
}

static size_t file_size(const char *path)
{
    FILE *file = fopen(path, "rb");

    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);

    long size = ftell(file);

    fclose(file);

    return (size_t)size;
}

static void read_file(const char *path, uint8_t *out, size_t size)
{
    FILE *file = fopen(path, "rb");

    assert(file != NULL);
    assert(fread(out, 1, size, file) == size);
    fclose(file);
}

static uint32_t word(const uint8_t *bytes, size_t index)
{
    return (uint32_t)bytes[index * 4] |
           ((uint32_t)bytes[index * 4 + 1] << 8) |
           ((uint32_t)bytes[index * 4 + 2] << 16) |
           ((uint32_t)bytes[index * 4 + 3] << 24);
}

static void test_which_cartridges_have_a_battery(void)
{
    static const struct {
        uint8_t type;
        uint8_t ram_code;
        bool battery;
    } cases[] = {
        { TYPE_ROM_ONLY, 0x00, false },
        { TYPE_MBC1_RAM, RAM_CODE_8K, false },
        { TYPE_MBC1_RAM_BATTERY, RAM_CODE_8K, true },
        { TYPE_MBC2_BATTERY, 0x00, true },
        { TYPE_MBC3_RAM_BATTERY, RAM_CODE_32K, true },
        { TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_32K, true },
        { TYPE_MBC5_RAM_BATTERY, RAM_CODE_32K, true },
        { 0x11, 0x00, false },   /* MBC3 with nothing to keep */
        { 0x0F, 0x00, true }     /* MBC3 + timer + battery, no RAM */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Cartridge cartridge;

        load(&cartridge, cases[i].type, cases[i].ram_code);
        assert(cartridge.has_battery == cases[i].battery);

        if (!cases[i].battery) {
            assert(cartridge_save_battery(&cartridge, save_path, 0) ==
                   CARTRIDGE_SAVE_NOT_BATTERY_BACKED);
            assert(cartridge_load_battery(&cartridge, save_path, 0) ==
                   CARTRIDGE_SAVE_NOT_BATTERY_BACKED);
        }

        cartridge_destroy(&cartridge);
    }

    remove(save_path);
}

static void test_ram_round_trip(void)
{
    Cartridge cartridge;

    load(&cartridge, TYPE_MBC1_RAM_BATTERY, RAM_CODE_32K);
    assert(cartridge.ram_size == 0x8000);

    /* Fill RAM through the mapper, across all four banks. */
    cartridge_write(&cartridge, 0x0000, 0x0A);
    cartridge_write(&cartridge, 0x6000, 0x01);

    for (uint8_t bank = 0; bank < 4; bank++) {
        cartridge_write(&cartridge, 0x4000, bank);
        cartridge_write_ram(&cartridge, 0xA000, (uint8_t)(0x10 + bank));
        cartridge_write_ram(&cartridge, 0xBFFF, (uint8_t)(0x20 + bank));
    }

    assert(cartridge_save_battery(&cartridge, save_path, 0) ==
           CARTRIDGE_SAVE_OK);

    /* The file is the RAM and nothing else. */
    assert(file_size(save_path) == 0x8000);

    uint8_t saved[0x8000];

    read_file(save_path, saved, sizeof(saved));
    assert(saved[0x0000] == 0x10);
    assert(saved[0x1FFF] == 0x20);
    assert(saved[0x2000] == 0x11);
    assert(saved[0x6000] == 0x13);
    assert(saved[0x7FFF] == 0x23);
    assert(memcmp(saved, cartridge.ram, sizeof(saved)) == 0);

    cartridge_destroy(&cartridge);

    /* A fresh cartridge of the same game gets it back. */
    load(&cartridge, TYPE_MBC1_RAM_BATTERY, RAM_CODE_32K);
    assert(cartridge.ram[0] == 0);
    assert(cartridge_load_battery(&cartridge, save_path, 0) ==
           CARTRIDGE_SAVE_OK);
    assert(memcmp(saved, cartridge.ram, sizeof(saved)) == 0);

    cartridge_destroy(&cartridge);
    remove(save_path);
}

static void test_failed_loads_change_nothing(void)
{
    Cartridge cartridge;

    load(&cartridge, TYPE_MBC1_RAM_BATTERY, RAM_CODE_8K);
    cartridge_write(&cartridge, 0x0000, 0x0A);
    cartridge_write_ram(&cartridge, 0xA000, 0x77);

    /* No file yet is a normal first run, not an error. */
    remove(save_path);
    assert(cartridge_load_battery(&cartridge, save_path, 0) ==
           CARTRIDGE_SAVE_NO_FILE);
    assert(cartridge.ram[0] == 0x77);

    /* A file of the wrong size is refused. */
    static const uint8_t small[100] = { 0xEE };
    test_write_file(save_path, small, sizeof(small));
    assert(cartridge_load_battery(&cartridge, save_path, 0) ==
           CARTRIDGE_SAVE_BAD_SIZE);
    assert(cartridge.ram[0] == 0x77);

    uint8_t large[0x2000 + 1] = { 0 };
    test_write_file(save_path, large, sizeof(large));
    assert(cartridge_load_battery(&cartridge, save_path, 0) ==
           CARTRIDGE_SAVE_BAD_SIZE);
    assert(cartridge.ram[0] == 0x77);

    /* A save that cannot be written reports it. */
    assert(cartridge_save_battery(&cartridge, "no-such-dir/x.sav", 0) ==
           CARTRIDGE_SAVE_IO_ERROR);

    cartridge_destroy(&cartridge);
    remove(save_path);
}

static void test_mbc2_save(void)
{
    Cartridge cartridge;

    load(&cartridge, TYPE_MBC2_BATTERY, 0x00);
    cartridge_write(&cartridge, 0x0000, 0x0A);
    cartridge_write_ram(&cartridge, 0xA000, 0x0B);
    cartridge_write_ram(&cartridge, 0xA1FF, 0x05);

    assert(cartridge_save_battery(&cartridge, save_path, 0) ==
           CARTRIDGE_SAVE_OK);
    assert(file_size(save_path) == 512);

    cartridge_destroy(&cartridge);
    load(&cartridge, TYPE_MBC2_BATTERY, 0x00);
    assert(cartridge_load_battery(&cartridge, save_path, 0) ==
           CARTRIDGE_SAVE_OK);
    cartridge_write(&cartridge, 0x0000, 0x0A);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFB);
    assert(cartridge_read_ram(&cartridge, 0xA1FF) == 0xF5);

    cartridge_destroy(&cartridge);
    remove(save_path);
}

static void test_mbc3_without_a_clock_has_no_footer(void)
{
    Cartridge cartridge;

    load(&cartridge, TYPE_MBC3_RAM_BATTERY, RAM_CODE_8K);
    assert(cartridge_save_battery(&cartridge, save_path, 12345) ==
           CARTRIDGE_SAVE_OK);
    assert(file_size(save_path) == 0x2000);

    cartridge_destroy(&cartridge);
    remove(save_path);
}

/* The footer layout, byte for byte. */
static void test_clock_footer_format(void)
{
    Cartridge cartridge;

    load(&cartridge, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_8K);

    Mbc3State *mbc3 = &cartridge.state.mbc3;

    mbc3->live = (RtcRegisters){
        .seconds = 5, .minutes = 6, .hours = 7, .days = 0x123,
        .halted = false, .day_carry = true
    };
    mbc3->latched = (RtcRegisters){
        .seconds = 1, .minutes = 2, .hours = 3, .days = 0x045,
        .halted = true, .day_carry = false
    };

    assert(cartridge_save_battery(&cartridge, save_path,
                                  0x1122334455667788ull) == CARTRIDGE_SAVE_OK);
    assert(file_size(save_path) == 0x2000 + FOOTER_SIZE);

    uint8_t footer[FOOTER_SIZE];
    FILE *file = fopen(save_path, "rb");

    assert(file != NULL);
    assert(fseek(file, 0x2000, SEEK_SET) == 0);
    assert(fread(footer, 1, sizeof(footer), file) == sizeof(footer));
    fclose(file);

    /* Live seconds, minutes, hours, day low, day high, each a 32-bit word. */
    assert(word(footer, 0) == 5);
    assert(word(footer, 1) == 6);
    assert(word(footer, 2) == 7);
    assert(word(footer, 3) == 0x23);
    assert(word(footer, 4) == (0x01 | 0x80));   /* day bit 8 and carry */

    /* The latched copy in the same order. */
    assert(word(footer, 5) == 1);
    assert(word(footer, 6) == 2);
    assert(word(footer, 7) == 3);
    assert(word(footer, 8) == 0x45);
    assert(word(footer, 9) == 0x40);            /* halt */

    /* The time it was saved, as a 64-bit little-endian value. */
    assert(word(footer, 10) == 0x55667788u);
    assert(word(footer, 11) == 0x11223344u);

    cartridge_destroy(&cartridge);
    remove(save_path);
}

static void save_clock(Cartridge *cartridge, RtcRegisters live,
                       uint64_t saved_at)
{
    cartridge->state.mbc3.live = live;
    cartridge->state.mbc3.latched = (RtcRegisters){
        .seconds = 9, .minutes = 8, .hours = 7, .days = 6
    };
    assert(cartridge_save_battery(cartridge, save_path, saved_at) ==
           CARTRIDGE_SAVE_OK);
}

/* Loading adds the time that passed since the save to the running clock. */
static void test_clock_catches_up(void)
{
    Cartridge cartridge;

    load(&cartridge, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_8K);
    save_clock(&cartridge, (RtcRegisters){ .seconds = 50, .minutes = 59,
                                           .hours = 23, .days = 10 }, 1000);
    cartridge_destroy(&cartridge);

    /* 90 seconds later: 50 s + 90 s = 2 min 20 s, rolling every field. */
    load(&cartridge, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_8K);
    assert(cartridge_load_battery(&cartridge, save_path, 1000 + 90) ==
           CARTRIDGE_SAVE_OK);

    const RtcRegisters *live = &cartridge.state.mbc3.live;

    assert(live->seconds == 20);
    assert(live->minutes == 1);
    assert(live->hours == 0);
    assert(live->days == 11);
    assert(!live->day_carry);

    /* The latched copy is what it was. */
    assert(cartridge.state.mbc3.latched.seconds == 9);
    assert(cartridge.state.mbc3.latched.days == 6);
    cartridge_destroy(&cartridge);

    /* No time passed, or the clock went backwards: nothing changes. */
    for (int skew = 0; skew < 2; skew++) {
        load(&cartridge, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_8K);
        assert(cartridge_load_battery(&cartridge, save_path,
                                      skew == 0 ? 1000 : 500) ==
               CARTRIDGE_SAVE_OK);
        assert(cartridge.state.mbc3.live.seconds == 50);
        assert(cartridge.state.mbc3.live.minutes == 59);
        cartridge_destroy(&cartridge);
    }

    /* A long time: 600 days, which wraps the 9-bit day counter and sets
     * the carry. */
    load(&cartridge, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_8K);
    assert(cartridge_load_battery(&cartridge, save_path,
                                  1000 + 600ull * 86400) == CARTRIDGE_SAVE_OK);
    assert(cartridge.state.mbc3.live.days == (10 + 600) % 512);
    assert(cartridge.state.mbc3.live.day_carry);
    cartridge_destroy(&cartridge);

    /* A halted clock does not run while the console is off. */
    load(&cartridge, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_8K);
    save_clock(&cartridge, (RtcRegisters){ .seconds = 50, .halted = true },
               1000);
    cartridge_destroy(&cartridge);

    load(&cartridge, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_8K);
    assert(cartridge_load_battery(&cartridge, save_path, 1000 + 90) ==
           CARTRIDGE_SAVE_OK);
    assert(cartridge.state.mbc3.live.seconds == 50);
    assert(cartridge.state.mbc3.live.halted);

    cartridge_destroy(&cartridge);
    remove(save_path);
}

/* A save without the footer (RAM only) is still accepted. */
static void test_clock_save_without_footer(void)
{
    Cartridge cartridge;

    load(&cartridge, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_8K);

    static uint8_t ram_only[0x2000];

    ram_only[0] = 0x42;
    test_write_file(save_path, ram_only, sizeof(ram_only));

    assert(cartridge_load_battery(&cartridge, save_path, 99999) ==
           CARTRIDGE_SAVE_OK);
    assert(cartridge.ram[0] == 0x42);
    assert(cartridge.state.mbc3.live.seconds == 0);

    cartridge_destroy(&cartridge);
    remove(save_path);
}

int main(void)
{
    test_which_cartridges_have_a_battery();
    test_ram_round_trip();
    test_failed_loads_change_nothing();
    test_mbc2_save();
    test_mbc3_without_a_clock_has_no_footer();
    test_clock_footer_format();
    test_clock_catches_up();
    test_clock_save_without_footer();

    assert(remove(rom_path) == 0);

    printf("Battery save tests passed!\n");

    return 0;
}
