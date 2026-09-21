/*
 * Runs a test ROM headless under a cycle budget and reports its verdict.
 *
 * Blargg-style ROMs print their result through the serial port and end
 * with "Passed" or "Failed". The run stops as soon as either word is seen.
 *
 * Usage: rom_test <rom> [max T-cycles]
 * Exit code: 0 if the ROM printed "Passed", 1 otherwise.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emulator.h>

enum {
    DEFAULT_MAX_CYCLES = 400000000,
    OUTPUT_CAPACITY = 4096
};

typedef struct Capture {
    Emulator *emulator;
    char text[OUTPUT_CAPACITY];
    size_t length;
    int passed;
    int failed;
} Capture;

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

    if (capture->passed || capture->failed) {
        emulator_stop(capture->emulator);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <rom> [max T-cycles]\n", argv[0]);
        return 1;
    }

    uint64_t max_cycles = DEFAULT_MAX_CYCLES;

    if (argc == 3) {
        char *end = NULL;

        errno = 0;
        max_cycles = strtoull(argv[2], &end, 10);

        if (errno != 0 || end == argv[2] || *end != '\0') {
            fprintf(stderr, "Invalid cycle count: %s\n", argv[2]);
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

    EmulatorStatus status = emulator_load_rom(emulator, argv[1]);

    if (status == EMULATOR_OK) {
        status = emulator_run_cycles(emulator, max_cycles);
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

    printf("[%s] %s (%llu cycles)", verdict, argv[1],
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

    /* Serial text, indented, without the blank lines Blargg ROMs emit. */
    for (size_t i = 0; i < capture->length; i++) {
        char c = capture->text[i];

        if (c == '\n') {
            printf("\n");
        } else if (i == 0 || capture->text[i - 1] == '\n') {
            printf("    %c", c);
        } else {
            printf("%c", c);
        }
    }

    if (capture->length != 0 && capture->text[capture->length - 1] != '\n') {
        printf("\n");
    }

    int passed = capture->passed;

    free(capture);
    emulator_destroy(emulator);

    return passed ? 0 : 1;
}
