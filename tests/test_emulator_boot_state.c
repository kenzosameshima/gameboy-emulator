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

/* Reads one high-page register at the entry point and returns its value. */
static uint8_t register_at_entry(uint8_t low_address)
{
    const uint8_t code[] = {
        0xF0, low_address,  /* LDH A,[address] */
        0xE0, 0x01,         /* LDH [SB],A */
        0x3E, 0x81,         /* LD A,0x81 */
        0xE0, 0x02,         /* LDH [SC],A */
        0x18, 0xFE          /* spin */
    };
    SerialLog log;

    run_program(code, sizeof(code), &log);
    assert(log.length == 1);

    return log.bytes[0];
}

/*
 * Registers the boot ROM leaves in a known state, from the Mooneye
 * boot_hwio test: both joypad groups selected, a VBlank request pending, and
 * the sound registers the start-up chime leaves behind.
 */
static void test_io_registers_at_entry(void)
{
    static const struct {
        uint8_t address;
        uint8_t value;
    } expected[] = {
        { 0x00, 0xCF },                                 /* P1 */
        { 0x0F, 0xE1 },                                 /* IF */
        { 0x10, 0x80 }, { 0x11, 0xBF }, { 0x12, 0xF3 }, /* NR10-NR12 */
        { 0x14, 0xBF },                                 /* NR14 */
        { 0x24, 0x77 }, { 0x25, 0xF3 }, { 0x26, 0xF1 }, /* NR50-NR52 */
        { 0x40, 0x91 }                                  /* LCDC */
    };

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (register_at_entry(expected[i].address) != expected[i].value) {
            fprintf(stderr, "FF%02X: got %02X, want %02X\n",
                    expected[i].address, register_at_entry(expected[i].address),
                    expected[i].value);
        }

        assert(register_at_entry(expected[i].address) == expected[i].value);
    }
}

int main(void)
{
    test_div_at_entry();
    test_io_registers_at_entry();

    assert(remove(rom_path) == 0);

    printf("Boot state tests passed!\n");

    return 0;
}
