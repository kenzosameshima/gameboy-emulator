/*
 * Joypad and OAM DMA end to end: real programs run on the whole machine and
 * report what they saw over the serial port. Only the emulator.h interface
 * is used.
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

static const char *rom_path = "test_emulator_input_dma_rom.gb";

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

static Emulator *boot(const uint8_t *code, size_t size, SerialLog *log)
{
    static uint8_t rom[ROM_SIZE];

    memset(rom, 0, sizeof(rom));
    /* Like a real ROM: a jump at the entry point steps over the header. */
    static const uint8_t entry[] = { 0xC3, 0x50, 0x01 };

    assert(0x0150 + size <= ROM_SIZE);
    memcpy(rom + 0x0100, entry, sizeof(entry));
    memcpy(rom + 0x0150, code, size);
    test_write_file(rom_path, rom, ROM_SIZE);

    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    memset(log, 0, sizeof(*log));
    emulator_set_serial_output(emulator, log_serial, log);
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);

    return emulator;
}

/*
 * Selects the direction keys, polls P1 until a key is down, then sends the
 * low nibble over serial and spins.
 */
static void test_joypad_polling(void)
{
    static const uint8_t code[] = {
        0x3E, 0x20,         /* LD A,0x20       select directions */
        0xE0, 0x00,         /* LDH [P1],A */
        0xF0, 0x00,         /* loop: LDH A,[P1] */
        0xE6, 0x0F,         /* AND 0x0F */
        0xFE, 0x0F,         /* CP 0x0F */
        0x28, 0xF8,         /* JR Z,loop        nothing pressed */
        0xE0, 0x01,         /* LDH [SB],A       the key lines */
        0x3E, 0x81,         /* LD A,0x81 */
        0xE0, 0x02,         /* LDH [SC],A       send */
        0x18, 0xFE          /* spin */
    };
    SerialLog log;
    Emulator *emulator = boot(code, sizeof(code), &log);

    /* Nothing held: it keeps polling. */
    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    assert(log.length == 0);

    /* A button of the other group is not seen. */
    emulator_set_buttons(emulator, EMULATOR_BUTTON_A | EMULATOR_BUTTON_START);
    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    assert(log.length == 0);

    /* Down is bit 3 of the directions group, and a low bit means pressed. */
    emulator_set_buttons(emulator, EMULATOR_BUTTON_DOWN);
    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    assert(log.length == 1);
    assert(log.bytes[0] == 0x07);

    emulator_destroy(emulator);
}

/*
 * STOP ends when a button is pressed, whatever IE says, and the machine is
 * never reported as stalled while it waits.
 */
static void test_stop_wakes_on_button(void)
{
    static const uint8_t code[] = {
        0x3E, 0x10,         /* LD A,0x10       select the buttons */
        0xE0, 0x00,         /* LDH [P1],A */
        0x10, 0x00,         /* STOP */
        0x3E, 'K',          /* LD A,'K' */
        0xE0, 0x01,         /* LDH [SB],A */
        0x3E, 0x81,         /* LD A,0x81 */
        0xE0, 0x02,         /* LDH [SC],A */
        0x18, 0xFE          /* spin */
    };
    SerialLog log;
    Emulator *emulator = boot(code, sizeof(code), &log);

    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    assert(!emulator_is_stalled(emulator));
    assert(log.length == 0);

    emulator_set_buttons(emulator, EMULATOR_BUTTON_START);
    assert(emulator_run_cycles(emulator, 20000) == EMULATOR_OK);
    assert(log.length == 1);
    assert(log.bytes[0] == 'K');

    emulator_destroy(emulator);
}

/*
 * Starts a DMA from work RAM page C0 inside a routine copied to high RAM
 * (ROM is not reachable while it copies), reads work RAM during the
 * transfer, waits for it to finish and reports what it saw.
 */
static void test_oam_dma_from_a_program(void)
{
    static const uint8_t routine[] = {
        0x3E, 0xC0,         /* LD A,0xC0 */
        0xE0, 0x46,         /* LDH [DMA],A */
        0xFA, 0x00, 0xC0,   /* LD A,[0xC000]    blocked: reads 0xFF */
        0x47,               /* LD B,A */
        0x3E, 0x2A,         /* LD A,42 */
        0x3D,               /* wait: DEC A */
        0x20, 0xFD,         /* JR NZ,wait       168 M-cycles in all */
        0xC9                /* RET */
    };
    uint8_t code[128];
    size_t n = 0;

    /* Work RAM page C0 starts with 0x5A and ends with 0xA5. */
    static const uint8_t setup[] = {
        0x3E, 0x5A, 0xEA, 0x00, 0xC0,
        0x3E, 0xA5, 0xEA, 0x9F, 0xC0,
        0x21, 0x80, 0xFF    /* LD HL,0xFF80 */
    };

    memcpy(code + n, setup, sizeof(setup));
    n += sizeof(setup);

    for (size_t i = 0; i < sizeof(routine); i++) {
        code[n++] = 0x3E;           /* LD A,byte */
        code[n++] = routine[i];
        code[n++] = 0x22;           /* LD [HL+],A */
    }

    static const uint8_t report[] = {
        0xCD, 0x80, 0xFF,           /* CALL 0xFF80 */
        0x3E, 0x00, 0xE0, 0x40,     /* LCD off, so the CPU may read OAM */
        0x78,                       /* LD A,B */
        0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
        0xFA, 0x00, 0xFE,           /* LD A,[OAM + 0] */
        0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
        0xFA, 0x9F, 0xFE,           /* LD A,[OAM + 159] */
        0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
        0x18, 0xFE                  /* spin */
    };

    memcpy(code + n, report, sizeof(report));
    n += sizeof(report);

    SerialLog log;
    Emulator *emulator = boot(code, n, &log);

    assert(emulator_run_cycles(emulator, 100000) == EMULATOR_OK);
    assert(log.length == 3);
    assert(log.bytes[0] == 0xFF);   /* work RAM was blocked during DMA */
    assert(log.bytes[1] == 0x5A);   /* OAM[0] */
    assert(log.bytes[2] == 0xA5);   /* OAM[159] */

    emulator_destroy(emulator);
}

int main(void)
{
    test_joypad_polling();
    test_stop_wakes_on_button();
    test_oam_dma_from_a_program();

    assert(remove(rom_path) == 0);

    printf("Emulator joypad and DMA tests passed!\n");

    return 0;
}
