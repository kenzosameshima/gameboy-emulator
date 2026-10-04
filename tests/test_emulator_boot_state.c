/*
 * The machine state a ROM sees at its entry point, after the DMG boot ROM
 * has run, checked with real programs through the emulator.h interface.
 * The values are the ones the Mooneye boot tests verified on hardware.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <emulator.h>

#include "test_util.h"

enum {
    ROM_SIZE = 0x8000,
    LOG_CAPACITY = 16
};

static const char *rom_path = "test_emulator_boot_state_rom.gb";

typedef struct SerialLog {
    uint8_t bytes[LOG_CAPACITY];
    size_t length;
} SerialLog;

static void log_serial(void *context, uint8_t byte)
{
    SerialLog *log = context;

    assert(log->length < LOG_CAPACITY);
    log->bytes[log->length++] = byte;
}

/* Runs `code` from the entry point and returns what it sent over serial. */
static void run_program(const uint8_t *code, size_t size, SerialLog *log)
{
    static uint8_t rom[ROM_SIZE];

    memset(rom, 0, sizeof(rom));
    memcpy(rom + 0x0100, code, size);
    test_write_file(rom_path, rom, ROM_SIZE);

    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    memset(log, 0, sizeof(*log));
    emulator_set_serial_output(emulator, log_serial, log);
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);
    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    emulator_destroy(emulator);
}

/* The boot ROM leaves the divider at 0xABCC, so DIV reads 0xAB. */
static void test_div_at_entry(void)
{
    static const uint8_t code[] = {
        0xF0, 0x04,         /* LDH A,[DIV] */
        0xE0, 0x01,         /* LDH [SB],A */
        0x3E, 0x81,         /* LD A,0x81 */
        0xE0, 0x02,         /* LDH [SC],A */
        0x18, 0xFE          /* spin */
    };
    SerialLog log;

    run_program(code, sizeof(code), &log);
    assert(log.length == 1);
    assert(log.bytes[0] == 0xAB);
}

int main(void)
{
    test_div_at_entry();

    assert(remove(rom_path) == 0);

    printf("Boot state tests passed!\n");

    return 0;
}
