#include <stdio.h>

#include <emulator.h>


int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom>\n", argv[0]);
        return 1;
    }

    Emulator *emulator = emulator_create();

    if (emulator == NULL) {
        fprintf(stderr, "Failed to create emulator\n");
        return 1;
    }

    if (emulator_load_rom(emulator, argv[1]) != 0) {
        fprintf(
            stderr,
            "Failed to load ROM: %s\n",
            argv[1]
        );

        emulator_destroy(emulator);
        return 1;
    }

    if (emulator_run(emulator) != 0) {
        fprintf(stderr, "Emulation failed\n");

        emulator_destroy(emulator);
        return 1;
    }

    emulator_destroy(emulator);

    return 0;
}
