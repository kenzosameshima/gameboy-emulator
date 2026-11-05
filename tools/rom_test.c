/*
 * Runs a test ROM headless under a cycle budget and reports its verdict.
 *
 * Three ways of reporting are recognised. Two go through the serial port:
 *
 *   Blargg-style ROMs print text that ends with "Passed" or "Failed".
 *   Mooneye ROMs send six bytes: 3 5 8 13 21 34 (the Fibonacci numbers)
 *   for a pass, or 0x42 six times for a failure.
 *
 * The third is for Blargg ROMs that only print on screen: they leave the
 * result in cartridge RAM, with the signature DE B0 61 at A001, a status at
 * A000 (0x80 while running, 0 for a pass) and the text from A004. That needs
 * a battery-backed cartridge, since it is read back through the save.
 *
 * With --microtest the ROM is a gbmicrotest ROM, which leaves its verdict in
 * high RAM instead: 0x01 at FF82 for a pass, 0xFF for a failure. Those tests
 * were checked on a real DMG, and most finish within a frame or two.
 *
 * The run stops as soon as a verdict is seen.
 *
 * Usage: rom_test <rom> [max T-cycles] [--microtest]
 * Exit code: 0 if the ROM passed, 1 otherwise.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <emulator.h>

#include "../src/emulator_internal.h"

enum {
    DEFAULT_MAX_CYCLES = 400000000,
    OUTPUT_CAPACITY = 4096,

    /* gbmicrotest: the verdict byte, how often it is read, and the default
     * budget (is_if_set_during_ime0 needs about 380 ms of emulated time). */
    MICROTEST_RESULT_ADDRESS = 0xFF82,
    MICROTEST_PASS = 0x01,
    MICROTEST_FAIL = 0xFF,
    MICROTEST_CHECK_CYCLES = 4096,
    MICROTEST_MAX_CYCLES = 2000000,

    /* How often cartridge RAM is checked for a verdict. */
    RAM_CHECK_CYCLES = 1000000,
    RAM_TEXT_START = 4
};

/* The signature that tells a Blargg result in cartridge RAM from garbage. */
static const uint8_t BLARGG_RAM_SIGNATURE[3] = { 0xDE, 0xB0, 0x61 };

#define BLARGG_RAM_RUNNING 0x80

/* The six bytes a passing Mooneye ROM sends. */
static const uint8_t MOONEYE_PASS[6] = { 3, 5, 8, 13, 21, 34 };

typedef struct Capture {
    Emulator *emulator;
    char text[OUTPUT_CAPACITY];
    size_t length;
    int passed;
    int failed;
    int mooneye;    /* the verdict came as register bytes, not text */
} Capture;

/* True if the last six bytes received are `expected`, or all 0x42 when it
 * is NULL. */
static int ends_with_mooneye(const Capture *capture, const uint8_t *expected)
{
    if (capture->length < 6) {
        return 0;
    }

    for (size_t i = 0; i < 6; i++) {
        uint8_t byte = (uint8_t)capture->text[capture->length - 6 + i];

        if (byte != (expected != NULL ? expected[i] : 0x42)) {
            return 0;
        }
    }

    return 1;
}

static void capture_byte(void *context, uint8_t byte)
{
    Capture *capture = context;

    if (capture->length + 1 < sizeof(capture->text)) {
        capture->text[capture->length++] = (char)byte;
        capture->text[capture->length] = '\0';
    }

    if (strstr(capture->text, "Passed") != NULL) {
        capture->passed = 1;
    }

    if (strstr(capture->text, "Failed") != NULL) {
        capture->failed = 1;
    }

    if (ends_with_mooneye(capture, MOONEYE_PASS)) {
        capture->passed = 1;
        capture->mooneye = 1;
    }

    if (ends_with_mooneye(capture, NULL)) {
        capture->failed = 1;
        capture->mooneye = 1;
    }

    if (capture->passed || capture->failed) {
        emulator_stop(capture->emulator);
    }
}

/* Looks for a finished result in the cartridge's battery-backed RAM. */
static void check_cartridge_ram(Emulator *emulator, Capture *capture)
{
    char path[64];
    uint8_t ram[OUTPUT_CAPACITY];
    size_t count = 0;

    snprintf(path, sizeof(path), "rom_test_%ld.sav", (long)getpid());

    if (emulator_save_battery(emulator, path, 0) != EMULATOR_OK) {
        return;
    }

    FILE *file = fopen(path, "rb");

    if (file != NULL) {
        count = fread(ram, 1, sizeof(ram), file);
        fclose(file);
    }

    remove(path);

    if (count <= RAM_TEXT_START || ram[0] == BLARGG_RAM_RUNNING ||
        memcmp(ram + 1, BLARGG_RAM_SIGNATURE, sizeof(BLARGG_RAM_SIGNATURE)) !=
            0) {
        return;
    }

    for (size_t i = RAM_TEXT_START; i < count && ram[i] != 0 &&
                                    capture->length + 1 < sizeof(capture->text);
         i++) {
        capture->text[capture->length++] = (char)ram[i];
    }

    capture->text[capture->length] = '\0';

    if (ram[0] == 0) {
        capture->passed = 1;
    } else {
        capture->failed = 1;
    }
}

/* Reads the verdict a gbmicrotest ROM leaves in high RAM. */
static void check_microtest(Emulator *emulator, Capture *capture)
{
    uint8_t result = memory_read(&emulator->memory, MICROTEST_RESULT_ADDRESS);

    if (result == MICROTEST_PASS) {
        capture->passed = 1;
    } else if (result == MICROTEST_FAIL) {
        capture->failed = 1;
    }
}

/* Runs until a verdict arrives or `max_cycles` are used up. */
static EmulatorStatus run_until_verdict(
    Emulator *emulator,
    Capture *capture,
    uint64_t max_cycles,
    int microtest
)
{
    int check_ram = !microtest && emulator_has_battery(emulator);
    uint64_t check_cycles = microtest ? MICROTEST_CHECK_CYCLES : RAM_CHECK_CYCLES;
    uint64_t start = emulator_cycles(emulator);
    EmulatorStatus status = EMULATOR_OK;

    while (status == EMULATOR_OK && !capture->passed && !capture->failed) {
        uint64_t done = emulator_cycles(emulator) - start;

        if (done >= max_cycles) {
            break;
        }

        uint64_t slice = max_cycles - done;

        if (slice > check_cycles) {
            slice = check_cycles;
        }

        status = emulator_run_cycles(emulator, slice);

        if (microtest) {
            check_microtest(emulator, capture);
        } else if (check_ram && status == EMULATOR_OK) {
            check_cartridge_ram(emulator, capture);
        }
    }

    return status;
}

int main(int argc, char **argv)
{
    int microtest = 0;
    const char *rom = NULL;
    const char *cycles_text = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--microtest") == 0) {
            microtest = 1;
        } else if (rom == NULL) {
            rom = argv[i];
        } else if (cycles_text == NULL) {
            cycles_text = argv[i];
        } else {
            rom = NULL;
            break;
        }
    }

    if (rom == NULL) {
        fprintf(
            stderr,
            "Usage: %s <rom> [max T-cycles] [--microtest]\n",
            argv[0]
        );
        return 1;
    }

    uint64_t max_cycles = microtest ? MICROTEST_MAX_CYCLES : DEFAULT_MAX_CYCLES;

    if (cycles_text != NULL) {
        char *end = NULL;

        errno = 0;
        max_cycles = strtoull(cycles_text, &end, 10);

        if (errno != 0 || end == cycles_text || *end != '\0') {
            fprintf(stderr, "Invalid cycle count: %s\n", cycles_text);
            return 1;
        }
    }

    Capture *capture = calloc(1, sizeof(Capture));
    Emulator *emulator = emulator_create();

    if (capture == NULL || emulator == NULL) {
        fprintf(stderr, "Out of memory\n");
        free(capture);
        emulator_destroy(emulator);
        return 1;
    }

    capture->emulator = emulator;
    emulator_set_serial_output(emulator, capture_byte, capture);

    EmulatorStatus status = emulator_load_rom(emulator, rom);

    if (status == EMULATOR_OK) {
        status = run_until_verdict(emulator, capture, max_cycles, microtest);
    }

    const char *verdict;

    if (capture->passed) {
        verdict = "PASS";
    } else if (capture->failed) {
        verdict = "FAIL";
    } else if (status == EMULATOR_OK) {
        verdict = "TIMEOUT";
    } else {
        verdict = "ERROR";
    }

    printf("[%s] %s (%llu cycles)", verdict, rom,
           (unsigned long long)emulator_cycles(emulator));

    if (!capture->passed && status != EMULATOR_OK) {
        EmulatorFault fault;

        printf(": %s", emulator_status_string(status));

        if (emulator_get_fault(emulator, &fault)) {
            printf(" 0x%02X at PC=0x%04X",
                   (unsigned)fault.opcode, (unsigned)fault.pc);
        }
    }

    printf("\n");

    /* Serial text, indented, without the blank lines Blargg ROMs emit. A
     * Mooneye verdict is register bytes, not text, so it is not echoed. */
    for (size_t i = 0; i < capture->length && !capture->mooneye; i++) {
        char c = capture->text[i];

        if (c == '\n') {
            printf("\n");
        } else if (i == 0 || capture->text[i - 1] == '\n') {
            printf("    %c", c);
        } else {
            printf("%c", c);
        }
    }

    if (!capture->mooneye && capture->length != 0 &&
        capture->text[capture->length - 1] != '\n') {
        printf("\n");
    }

    int passed = capture->passed;

    free(capture);
    emulator_destroy(emulator);

    return passed ? 0 : 1;
}
