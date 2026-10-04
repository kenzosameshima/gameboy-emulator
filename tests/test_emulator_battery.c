/*
 * Battery saves through the emulator.h interface: a program writes to
 * cartridge RAM, the emulator saves it, and a fresh emulator loads it and
 * a second program reads it back, reporting over serial.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <emulator.h>

#include "test_util.h"

enum {
    ROM_SIZE = 0x8000,
    HEADER_TYPE = 0x0147,
    HEADER_RAM_SIZE = 0x0149,

    TYPE_MBC1_RAM = 0x02,
    TYPE_MBC1_RAM_BATTERY = 0x03,
    RAM_CODE_8K = 0x02
};

static const char *rom_path = "test_emulator_battery_rom.gb";
static const char *save_path = "test_emulator_battery.sav";

typedef struct SerialLog {
    uint8_t bytes[8];
    size_t length;
} SerialLog;

static void log_serial(void *context, uint8_t byte)
{
    SerialLog *log = context;

    assert(log->length < sizeof(log->bytes));
    log->bytes[log->length++] = byte;
}

/* Boots `code` as a ROM of the given cartridge type. */
static Emulator *boot(
    uint8_t type,
    const uint8_t *code,
    size_t size,
    SerialLog *log
)
{
    static uint8_t rom[ROM_SIZE];

    memset(rom, 0, sizeof(rom));

    /* Like a real ROM: a jump at the entry point steps over the header. */
    static const uint8_t entry[] = { 0xC3, 0x50, 0x01 };

    memcpy(rom + 0x0100, entry, sizeof(entry));
    memcpy(rom + 0x0150, code, size);
    rom[HEADER_TYPE] = type;
    rom[HEADER_RAM_SIZE] = RAM_CODE_8K;
    test_write_file(rom_path, rom, ROM_SIZE);

    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    memset(log, 0, sizeof(*log));
    emulator_set_serial_output(emulator, log_serial, log);
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);

    return emulator;
}

static void test_save_and_load_round_trip(void)
{
    /* Enable RAM, write 0x5C to A000 and 0xA7 to BFFF, then spin. */
    static const uint8_t writer[] = {
        0x3E, 0x0A, 0xEA, 0x00, 0x00,
        0x3E, 0x5C, 0xEA, 0x00, 0xA0,
        0x3E, 0xA7, 0xEA, 0xFF, 0xBF,
        0x18, 0xFE
    };
    /* Enable RAM, send A000 and BFFF over serial, then spin. */
    static const uint8_t reader[] = {
        0x3E, 0x0A, 0xEA, 0x00, 0x00,
        0xFA, 0x00, 0xA0, 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
        0xFA, 0xFF, 0xBF, 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
        0x18, 0xFE
    };
    SerialLog log;

    Emulator *emulator = boot(TYPE_MBC1_RAM_BATTERY, writer, sizeof(writer),
                              &log);

    assert(emulator_has_battery(emulator));
    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    assert(emulator_save_battery(emulator, save_path, 1700000000) ==
           EMULATOR_OK);
    emulator_destroy(emulator);

    emulator = boot(TYPE_MBC1_RAM_BATTERY, reader, sizeof(reader), &log);
    assert(emulator_load_battery(emulator, save_path, 1700000000) ==
           EMULATOR_OK);
    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    assert(log.length == 2);
    assert(log.bytes[0] == 0x5C);
    assert(log.bytes[1] == 0xA7);
    emulator_destroy(emulator);

    /* Without loading the save, the RAM starts empty. */
    emulator = boot(TYPE_MBC1_RAM_BATTERY, reader, sizeof(reader), &log);
    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    assert(log.length == 2);
    assert(log.bytes[0] == 0x00);
    assert(log.bytes[1] == 0x00);
    emulator_destroy(emulator);

    remove(save_path);
}

static void test_statuses(void)
{
    static const uint8_t spin[] = { 0x18, 0xFE };
    SerialLog log;

    /* A cartridge with RAM but no battery has nothing to save. */
    Emulator *emulator = boot(TYPE_MBC1_RAM, spin, sizeof(spin), &log);

    assert(!emulator_has_battery(emulator));
    assert(emulator_save_battery(emulator, save_path, 0) ==
           EMULATOR_ERROR_NOT_BATTERY_BACKED);
    assert(emulator_load_battery(emulator, save_path, 0) ==
           EMULATOR_ERROR_NOT_BATTERY_BACKED);
    emulator_destroy(emulator);

    emulator = boot(TYPE_MBC1_RAM_BATTERY, spin, sizeof(spin), &log);

    /* The first run has no save yet: not an error. */
    remove(save_path);
    assert(emulator_load_battery(emulator, save_path, 0) ==
           EMULATOR_NO_SAVE_FILE);

    /* A file of the wrong size is refused, and a path that cannot be
     * written is reported. */
    static const uint8_t wrong[10] = { 1 };

    test_write_file(save_path, wrong, sizeof(wrong));
    assert(emulator_load_battery(emulator, save_path, 0) ==
           EMULATOR_ERROR_SAVE_SIZE);
    assert(emulator_save_battery(emulator, "no-such-dir/x.sav", 0) ==
           EMULATOR_ERROR_SAVE_IO);

    /* Arguments are checked. */
    assert(emulator_save_battery(NULL, save_path, 0) ==
           EMULATOR_ERROR_INVALID_ARGUMENT);
    assert(emulator_load_battery(emulator, NULL, 0) ==
           EMULATOR_ERROR_INVALID_ARGUMENT);
    assert(!emulator_has_battery(NULL));
    emulator_destroy(emulator);

    /* No ROM loaded. */
    emulator = emulator_create();
    assert(!emulator_has_battery(emulator));
    assert(emulator_save_battery(emulator, save_path, 0) ==
           EMULATOR_ERROR_NO_ROM);
    assert(emulator_load_battery(emulator, save_path, 0) ==
           EMULATOR_ERROR_NO_ROM);
    emulator_destroy(emulator);

    remove(save_path);
}

int main(void)
{
    test_save_and_load_round_trip();
    test_statuses();

    assert(remove(rom_path) == 0);

    printf("Emulator battery save tests passed!\n");

    return 0;
}
