#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/emulator_internal.h"

static void create_timer_rom(const char *path)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);

    const uint8_t zero = 0;
    const uint8_t program[] = {
        0xFB,
        0x00,
        0x00,
        0x00,
        0x00
    };

    for (size_t index = 0; index < 0x0100; index++) {
        assert(fwrite(&zero, 1, 1, file) == 1);
    }

    assert(fwrite(program, 1, sizeof(program), file) == sizeof(program));
    assert(fclose(file) == 0);
}

int main(void)
{
    const char *rom_path = "test_emulator_timer_rom.gb";
    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    create_timer_rom(rom_path);
    assert(emulator_load_rom(emulator, rom_path) == 0);

    bus_write(
        &emulator->bus,
        TIMER_TIMA_ADDRESS,
        0xFF
    );
    bus_write(
        &emulator->bus,
        TIMER_TMA_ADDRESS,
        0x42
    );
    bus_write(
        &emulator->bus,
        TIMER_TAC_ADDRESS,
        (uint8_t)(TIMER_TAC_ENABLE | 1)
    );
    bus_write(
        &emulator->bus,
        INTERRUPT_ENABLE_ADDRESS,
        INTERRUPT_TIMER
    );

    assert(emulator_step(emulator) == 0);
    assert(!emulator->cpu.ime);
    assert(emulator->timer.divider == 4);

    assert(emulator_step(emulator) == 0);
    assert(emulator->cpu.ime);
    assert(emulator->timer.divider == 8);

    assert(emulator_step(emulator) == 0);
    assert(emulator->timer.divider == 12);
    assert(
        (emulator->interrupts.interrupt_flag & INTERRUPT_TIMER) == 0
    );

    assert(emulator_step(emulator) == 0);
    assert(emulator->timer.divider == 16);
    assert(
        (emulator->interrupts.interrupt_flag & INTERRUPT_TIMER) == 0
    );

    assert(emulator_step(emulator) == 0);
    assert(emulator->timer.divider == 20);
    assert(
        (emulator->interrupts.interrupt_flag & INTERRUPT_TIMER) != 0
    );

    uint16_t divider_before_service = emulator->timer.divider;
    assert(emulator_step(emulator) == 0);
    assert(
        emulator->cpu.step_status ==
        CPU_STEP_INTERRUPT_SERVICED
    );
    assert(emulator->cpu.registers.pc == 0x0050);
    assert(emulator->cpu.ime == false);
    assert(
        emulator->timer.divider ==
        (uint16_t)(divider_before_service + 20)
    );
    assert(
        (emulator->interrupts.interrupt_flag & INTERRUPT_TIMER) == 0
    );

    emulator_destroy(emulator);
    assert(remove(rom_path) == 0);

    printf("Emulator Timer integration tests passed!\n");
    return 0;
}
