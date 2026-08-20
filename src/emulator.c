#include <stdlib.h>

#include "emulator_internal.h"


Emulator *emulator_create(void)
{
    Emulator *emulator = malloc(sizeof(Emulator));

    if (emulator == NULL) {
        return NULL;
    }

    /*
     * Initialize components.
     *
     * The Emulator owns all components, while the Bus
     * only keeps references to the Cartridge and Memory.
     */
    cartridge_init(&emulator->cartridge);

    memory_init(&emulator->memory);

    /* Initial interrupt state: IF = 0, IE = 0, IME = false. */
    emulator->interrupts.interrupt_flag = 0;
    emulator->interrupts.interrupt_enable = 0;
    timer_init(&emulator->timer, &emulator->interrupts);

    bus_init(
        &emulator->bus,
        &emulator->cartridge,
        &emulator->memory,
        &emulator->interrupts
    );
    bus_attach_timer(&emulator->bus, &emulator->timer);

    cpu_init(
        &emulator->cpu,
        &emulator->bus
    );

    emulator->running = false;

    return emulator;
}


int emulator_load_rom(
    Emulator *emulator,
    const char *path
)
{
    if (emulator == NULL || path == NULL) {
        return 1;
    }

    if (!cartridge_load(
        &emulator->cartridge,
        path
    )) {
        return 1;
    }

    /* Loading a ROM starts a new machine execution state. */
    memory_init(&emulator->memory);
    emulator->interrupts.interrupt_flag = 0;
    emulator->interrupts.interrupt_enable = 0;
    timer_init(&emulator->timer, &emulator->interrupts);
    cpu_init(&emulator->cpu, &emulator->bus);
    emulator->running = false;

    return 0;
}

int emulator_step(Emulator *emulator)
{
    if (emulator == NULL || emulator->cartridge.rom == NULL) {
        return 1;
    }

    CpuCycles cycles = cpu_step(&emulator->cpu);
    timer_step(&emulator->timer, cycles);

    if (emulator->cpu.step_status == CPU_STEP_UNIMPLEMENTED_OPCODE) {
        return 1;
    }

    return 0;
}


int emulator_run(Emulator *emulator)
{
    if (emulator == NULL || emulator->cartridge.rom == NULL) {
        return 1;
    }

    emulator->running = true;

    while (emulator->running) {
        if (emulator_step(emulator) != 0) {
            emulator->running = false;
            return 1;
        }
    }

    return 0;
}


void emulator_stop(Emulator *emulator)
{
    if (emulator == NULL) {
        return;
    }

    emulator->running = false;
}


bool emulator_is_running(const Emulator *emulator)
{
    if (emulator == NULL) {
        return false;
    }

    return emulator->running;
}


void emulator_destroy(Emulator *emulator)
{
    if (emulator == NULL) {
        return;
    }

    emulator->running = false;

    cartridge_destroy(&emulator->cartridge);

    free(emulator);
}
