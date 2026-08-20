#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <emulator.h>

static void create_test_rom(
    const char *path,
    const uint8_t *program,
    size_t program_size
)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);

    const uint8_t zero = 0;

    for (size_t i = 0; i < 0x0100; i++) {
        assert(fwrite(&zero, 1, 1, file) == 1);
    }

    assert(fwrite(program, 1, program_size, file) == program_size);
    assert(fclose(file) == 0);
}

int main(void)
{
    const char *rom_path = "test_emulator_rom.gb";
    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    assert(!emulator_is_running(emulator));
    assert(emulator_step(emulator) != 0);

    const uint8_t halt_program[] = {
        0x76
    };

    create_test_rom(
        rom_path,
        halt_program,
        sizeof(halt_program)
    );
    assert(emulator_load_rom(emulator, rom_path) == 0);

    assert(!emulator_is_running(emulator));
    assert(emulator_step(emulator) == 0);
    assert(emulator_step(emulator) == 0);

    assert(emulator_load_rom(emulator, "roms/does-not-exist.gb") != 0);
    assert(emulator_step(emulator) == 0);

    const uint8_t integration_program[] = {
        0x3E, 0x10,
        0x3C,
        0x76
    };

    create_test_rom(
        rom_path,
        integration_program,
        sizeof(integration_program)
    );
    assert(emulator_load_rom(emulator, rom_path) == 0);
    assert(emulator_step(emulator) == 0);
    assert(emulator_step(emulator) == 0);
    assert(emulator_step(emulator) == 0);
    assert(emulator_step(emulator) == 0);

    const uint8_t invalid_program[] = {
        0xD3
    };

    create_test_rom(
        rom_path,
        invalid_program,
        sizeof(invalid_program)
    );
    assert(emulator_load_rom(emulator, rom_path) == 0);
    assert(emulator_step(emulator) != 0);
    assert(emulator_run(emulator) != 0);

    emulator_destroy(emulator);
    assert(remove(rom_path) == 0);

    printf("Emulator tests passed!\n");
    return 0;
}
