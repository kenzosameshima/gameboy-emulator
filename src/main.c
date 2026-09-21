#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <emulator.h>


static void print_serial_byte(void *context, uint8_t byte)
{
    (void)context;

    fputc(byte, stdout);
    fflush(stdout);
}

static int parse_cycles(const char *text, uint64_t *cycles)
{
    char *end = NULL;

    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }

    *cycles = value;

    return 1;
}

int main(int argc, char **argv)
{
    uint64_t max_cycles = UINT64_MAX;

    if (argc == 4 && argv[2][0] == '-' && argv[2][1] == 'c') {
        /* <rom> -c <cycles> */
        if (!parse_cycles(argv[3], &max_cycles)) {
            fprintf(stderr, "Invalid cycle count: %s\n", argv[3]);
            return 1;
        }
    } else if (argc != 2) {
        fprintf(stderr, "Usage: %s <rom> [-c <max T-cycles>]\n", argv[0]);
        return 1;
    }

    Emulator *emulator = emulator_create();

    if (emulator == NULL) {
        fprintf(stderr, "Failed to create emulator\n");
        return 1;
    }

    emulator_set_serial_output(emulator, print_serial_byte, NULL);

    EmulatorStatus status = emulator_load_rom(emulator, argv[1]);

    if (status != EMULATOR_OK) {
        fprintf(
            stderr,
            "Failed to load ROM %s: %s\n",
            argv[1],
            emulator_status_string(status)
        );

        emulator_destroy(emulator);
        return 1;
    }

    status = emulator_run_cycles(emulator, max_cycles);

    int exit_code = 0;

    if (status == EMULATOR_STALLED) {
        fprintf(
            stderr,
            "Stopped after %llu cycles: %s\n",
            (unsigned long long)emulator_cycles(emulator),
            emulator_status_string(status)
        );
    } else if (status != EMULATOR_OK) {
        EmulatorFault fault;

        fprintf(
            stderr,
            "Emulation failed after %llu cycles: %s",
            (unsigned long long)emulator_cycles(emulator),
            emulator_status_string(status)
        );

        if (emulator_get_fault(emulator, &fault)) {
            fprintf(
                stderr,
                " 0x%02X at PC=0x%04X",
                (unsigned)fault.opcode,
                (unsigned)fault.pc
            );
        }

        fputc('\n', stderr);
        exit_code = 1;
    }

    emulator_destroy(emulator);

    return exit_code;
}
