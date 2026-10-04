/*
 * MBC3 through the public cartridge interface: ROM and RAM banking and the
 * real-time clock with its latch, halt, rollover and carry.
 *
 * The clock runs off the CPU clock, so a "second" is 4194304 T-cycles.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cartridge.h>

#include "test_util.h"

enum {
    BANK_SIZE = 0x4000,

    TYPE_MBC3 = 0x11,
    TYPE_MBC3_RAM_BATTERY = 0x13,
    TYPE_MBC3_TIMER_RAM_BATTERY = 0x10,
    RAM_CODE_32K = 0x03,

    RTC_SECONDS = 0x08,
    RTC_MINUTES = 0x09,
    RTC_HOURS = 0x0A,
    RTC_DAY_LOW = 0x0B,
    RTC_DAY_HIGH = 0x0C,

    DAY_HIGH_BIT8 = 0x01,
    DAY_HIGH_HALT = 0x40,
    DAY_HIGH_CARRY = 0x80
};

#define CYCLES_PER_SECOND 4194304u

static const char *rom_path = "test_mbc3_rom.gb";

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

static void load(Cartridge *cartridge, unsigned banks, uint8_t type,
                 uint8_t ram_code)
{
    write_banked_rom(banks, type, ram_code);
    cartridge_init(cartridge);
    assert(cartridge_load(cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK);
    assert(cartridge->mapper == CARTRIDGE_MAPPER_MBC3);
}

static void enable_ram_and_clock(Cartridge *cartridge)
{
    cartridge_write(cartridge, 0x0000, 0x0A);
}

static void select_register(Cartridge *cartridge, uint8_t select)
{
    cartridge_write(cartridge, 0x4000, select);
}

static uint8_t read_register(Cartridge *cartridge, uint8_t select)
{
    select_register(cartridge, select);

    return cartridge_read_ram(cartridge, 0xA000);
}

static void write_register(Cartridge *cartridge, uint8_t select,
                           uint8_t value)
{
    select_register(cartridge, select);
    cartridge_write_ram(cartridge, 0xA000, value);
}

static void latch(Cartridge *cartridge)
{
    cartridge_write(cartridge, 0x6000, 0x00);
    cartridge_write(cartridge, 0x6000, 0x01);
}

static void step_seconds(Cartridge *cartridge, unsigned seconds)
{
    for (unsigned i = 0; i < seconds; i++) {
        cartridge_step(cartridge, CYCLES_PER_SECOND);
    }
}

static void test_rom_banking(void)
{
    Cartridge cartridge;

    load(&cartridge, 8, TYPE_MBC3, 0x00);

    /* Bank 0 is fixed at 0000-3FFF; bank 1 is the power-on bank. */
    assert(cartridge_read(&cartridge, 0x0000) == 0);
    assert(cartridge_read(&cartridge, 0x4000) == 1);
    assert(cartridge_read(&cartridge, 0x4001) == 0x81);

    for (uint8_t bank = 1; bank < 8; bank++) {
        cartridge_write(&cartridge, 0x2000, bank);
        assert(cartridge_read(&cartridge, 0x4000) == bank);
        assert(cartridge_read(&cartridge, 0x0000) == 0);
    }

    /* Bank 0 cannot be selected into the switchable window. */
    cartridge_write(&cartridge, 0x2000, 0x00);
    assert(cartridge_read(&cartridge, 0x4000) == 1);

    /* Seven bits are used, and bank numbers wrap to the ROM size. */
    cartridge_write(&cartridge, 0x2000, 0x8A);
    assert(cartridge_read(&cartridge, 0x4000) == 2);

    /* The 4000-5FFF register picks RAM or clock registers, never ROM. */
    cartridge_write(&cartridge, 0x4000, 0x02);
    assert(cartridge_read(&cartridge, 0x4000) == 2);

    cartridge_destroy(&cartridge);
}

/* The ROM bank register is 7 bits wide, so it reaches 128 banks (2 MiB). */
static void test_large_rom_banking(void)
{
    Cartridge cartridge;

    load(&cartridge, 128, TYPE_MBC3, 0x00);

    static const uint8_t banks[] = { 31, 32, 33, 64, 100, 127 };

    for (size_t i = 0; i < sizeof(banks); i++) {
        cartridge_write(&cartridge, 0x2000, banks[i]);
        assert(cartridge_read(&cartridge, 0x4000) == banks[i]);
        assert(cartridge_read(&cartridge, 0x4001) == (uint8_t)(banks[i] + 0x80));
    }

    cartridge_destroy(&cartridge);

    /* A smaller ROM wraps the bank number to its size. */
    load(&cartridge, 64, TYPE_MBC3, 0x00);
    cartridge_write(&cartridge, 0x2000, 0x7F);
    assert(cartridge_read(&cartridge, 0x4000) == 63);
    /* The 0 -> 1 remap applies to the value written, not after wrapping. */
    cartridge_write(&cartridge, 0x2000, 0x40);
    assert(cartridge_read(&cartridge, 0x4000) == 0);

    cartridge_destroy(&cartridge);
}

static void test_ram_banking(void)
{
    Cartridge cartridge;

    load(&cartridge, 4, TYPE_MBC3_RAM_BATTERY, RAM_CODE_32K);
    assert(cartridge.ram_size == 0x8000);

    /* RAM is disabled at power-on. */
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write_ram(&cartridge, 0xA000, 0x55);

    enable_ram_and_clock(&cartridge);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x00);

    for (uint8_t bank = 0; bank < 4; bank++) {
        select_register(&cartridge, bank);
        cartridge_write_ram(&cartridge, 0xA000, (uint8_t)(0x10 + bank));
        cartridge_write_ram(&cartridge, 0xBFFF, (uint8_t)(0x20 + bank));
    }

    for (uint8_t bank = 0; bank < 4; bank++) {
        select_register(&cartridge, bank);
        assert(cartridge_read_ram(&cartridge, 0xA000) == 0x10 + bank);
        assert(cartridge_read_ram(&cartridge, 0xBFFF) == 0x20 + bank);
    }

    /* Anything but 0x0A in the low nibble disables it again, keeping data. */
    cartridge_write(&cartridge, 0x0000, 0x00);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write_ram(&cartridge, 0xA000, 0x99);

    cartridge_write(&cartridge, 0x0000, 0x3A);
    select_register(&cartridge, 3);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0x13);

    /* A cartridge without a clock has nothing behind 08-0C. */
    for (uint8_t select = RTC_SECONDS; select <= RTC_DAY_HIGH; select++) {
        assert(read_register(&cartridge, select) == 0xFF);
    }

    cartridge_destroy(&cartridge);
}

static void test_cartridge_without_ram(void)
{
    Cartridge cartridge;

    load(&cartridge, 4, TYPE_MBC3, 0x00);
    assert(cartridge.ram_size == 0);

    enable_ram_and_clock(&cartridge);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);
    cartridge_write_ram(&cartridge, 0xA000, 0x12);
    assert(cartridge_read_ram(&cartridge, 0xA000) == 0xFF);

    cartridge_destroy(&cartridge);
}

static void test_clock_latch(void)
{
    Cartridge cartridge;

    load(&cartridge, 4, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_32K);
    enable_ram_and_clock(&cartridge);

    for (uint8_t select = RTC_SECONDS; select <= RTC_DAY_HIGH; select++) {
        assert(read_register(&cartridge, select) == 0);
    }

    /* The clock runs, but reads see the value from the last latch. */
    cartridge_step(&cartridge, CYCLES_PER_SECOND * 5 + 123);
    assert(read_register(&cartridge, RTC_SECONDS) == 0);

    /* Only a write of 0 followed by 1 latches. */
    cartridge_write(&cartridge, 0x6000, 0x01);
    assert(read_register(&cartridge, RTC_SECONDS) == 0);

    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 5);

    step_seconds(&cartridge, 1);
    assert(read_register(&cartridge, RTC_SECONDS) == 5);

    /* Writing 1 again without a 0 in between does not latch. */
    cartridge_write(&cartridge, 0x6000, 0x01);
    assert(read_register(&cartridge, RTC_SECONDS) == 5);

    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 6);

    /* After a 0, only a 1 latches; any other value does not. */
    step_seconds(&cartridge, 1);
    cartridge_write(&cartridge, 0x6000, 0x00);
    cartridge_write(&cartridge, 0x6000, 0x02);
    assert(read_register(&cartridge, RTC_SECONDS) == 6);
    cartridge_write(&cartridge, 0x6000, 0x00);
    cartridge_write(&cartridge, 0x6000, 0x00);
    assert(read_register(&cartridge, RTC_SECONDS) == 6);
    cartridge_write(&cartridge, 0x6000, 0x01);
    assert(read_register(&cartridge, RTC_SECONDS) == 7);

    /* Register numbers outside 00-03 and 08-0C select nothing. */
    assert(read_register(&cartridge, 0x05) == 0xFF);
    assert(read_register(&cartridge, 0x0D) == 0xFF);

    /* Disabled, the clock registers read as open bus too. */
    cartridge_write(&cartridge, 0x0000, 0x00);
    assert(read_register(&cartridge, RTC_SECONDS) == 0xFF);

    cartridge_destroy(&cartridge);
}

static void test_clock_rollover(void)
{
    Cartridge cartridge;

    load(&cartridge, 4, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_32K);
    enable_ram_and_clock(&cartridge);

    /* 58 s -> 59 s */
    write_register(&cartridge, RTC_SECONDS, 58);
    step_seconds(&cartridge, 1);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 59);
    assert(read_register(&cartridge, RTC_MINUTES) == 0);

    /* 59 s -> 0 s and one minute */
    step_seconds(&cartridge, 1);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 0);
    assert(read_register(&cartridge, RTC_MINUTES) == 1);

    /* 23:59:59 on day 0 -> 00:00:00 on day 1 */
    write_register(&cartridge, RTC_SECONDS, 59);
    write_register(&cartridge, RTC_MINUTES, 59);
    write_register(&cartridge, RTC_HOURS, 23);
    step_seconds(&cartridge, 1);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 0);
    assert(read_register(&cartridge, RTC_MINUTES) == 0);
    assert(read_register(&cartridge, RTC_HOURS) == 0);
    assert(read_register(&cartridge, RTC_DAY_LOW) == 1);
    assert(read_register(&cartridge, RTC_DAY_HIGH) == 0);

    /* Day 255 -> 256 sets the ninth day bit. */
    write_register(&cartridge, RTC_DAY_LOW, 0xFF);
    write_register(&cartridge, RTC_SECONDS, 59);
    write_register(&cartridge, RTC_MINUTES, 59);
    write_register(&cartridge, RTC_HOURS, 23);
    step_seconds(&cartridge, 1);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_DAY_LOW) == 0);
    assert(read_register(&cartridge, RTC_DAY_HIGH) == DAY_HIGH_BIT8);

    /* Day 511 -> 0 sets the carry, which stays set until it is cleared. */
    write_register(&cartridge, RTC_DAY_LOW, 0xFF);
    write_register(&cartridge, RTC_DAY_HIGH, DAY_HIGH_BIT8);
    write_register(&cartridge, RTC_SECONDS, 59);
    write_register(&cartridge, RTC_MINUTES, 59);
    write_register(&cartridge, RTC_HOURS, 23);
    step_seconds(&cartridge, 1);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_DAY_LOW) == 0);
    assert(read_register(&cartridge, RTC_DAY_HIGH) == DAY_HIGH_CARRY);

    step_seconds(&cartridge, 1);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_DAY_HIGH) == DAY_HIGH_CARRY);

    write_register(&cartridge, RTC_DAY_HIGH, 0x00);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_DAY_HIGH) == 0x00);

    cartridge_destroy(&cartridge);
}

static void test_clock_halt_and_writes(void)
{
    Cartridge cartridge;

    load(&cartridge, 4, TYPE_MBC3_TIMER_RAM_BATTERY, RAM_CODE_32K);
    enable_ram_and_clock(&cartridge);

    /* Halted, the clock keeps its time. */
    write_register(&cartridge, RTC_DAY_HIGH, DAY_HIGH_HALT);
    step_seconds(&cartridge, 10);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 0);
    assert(read_register(&cartridge, RTC_DAY_HIGH) == DAY_HIGH_HALT);

    write_register(&cartridge, RTC_DAY_HIGH, 0x00);
    step_seconds(&cartridge, 3);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 3);

    /* Writing the seconds register restarts the current second. */
    cartridge_step(&cartridge, CYCLES_PER_SECOND * 9 / 10);
    write_register(&cartridge, RTC_SECONDS, 10);
    cartridge_step(&cartridge, CYCLES_PER_SECOND / 2);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 10);

    cartridge_step(&cartridge, CYCLES_PER_SECOND / 2);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_SECONDS) == 11);

    /* Writes go to the live clock, not to the latched copy. */
    write_register(&cartridge, RTC_MINUTES, 30);
    assert(read_register(&cartridge, RTC_MINUTES) == 0);
    latch(&cartridge);
    assert(read_register(&cartridge, RTC_MINUTES) == 30);

    cartridge_destroy(&cartridge);
}

int main(void)
{
    test_rom_banking();
    test_large_rom_banking();
    test_ram_banking();
    test_cartridge_without_ram();
    test_clock_latch();
    test_clock_rollover();
    test_clock_halt_and_writes();

    assert(remove(rom_path) == 0);

    printf("MBC3 cartridge tests passed!\n");

    return 0;
}
