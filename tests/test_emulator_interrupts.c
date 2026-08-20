#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/emulator_internal.h"

static void create_halt_rom(const char *path)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);

    const uint8_t zero = 0;
    const uint8_t halt = 0x76;

    for (size_t index = 0; index < 0x0100; index++) {
        assert(fwrite(&zero, 1, 1, file) == 1);
    }

    assert(fwrite(&halt, 1, 1, file) == 1);
    assert(fclose(file) == 0);
}

int main(void)
{
    const char *rom_path = "test_emulator_interrupts_rom.gb";
    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    create_halt_rom(rom_path);
    assert(emulator_load_rom(emulator, rom_path) == 0);
    assert(emulator_step(emulator) == 0);
    assert(emulator->cpu.halted);

    bus_write(
        &emulator->bus,
        INTERRUPT_FLAG_ADDRESS,
        INTERRUPT_VBLANK
    );
    bus_write(
        &emulator->bus,
        INTERRUPT_ENABLE_ADDRESS,
        INTERRUPT_VBLANK
    );
    emulator->cpu.ime = true;

    assert(emulator_step(emulator) == 0);
    assert(
        emulator->cpu.step_status ==
        CPU_STEP_INTERRUPT_SERVICED
    );
    assert(emulator->cpu.registers.pc == 0x0040);
    assert(emulator->cpu.registers.sp == 0xFFFC);
    assert(bus_read(&emulator->bus, 0xFFFC) == 0x01);
    assert(bus_read(&emulator->bus, 0xFFFD) == 0x01);

    emulator_destroy(emulator);
    assert(remove(rom_path) == 0);

    printf("Emulator interrupt integration tests passed!\n");
    return 0;
}
