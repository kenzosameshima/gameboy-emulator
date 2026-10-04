#include <stdlib.h>

#include "emulator_internal.h"


/*
 * Advances every machine component that runs off the CPU clock. Called by
 * the CPU after each M-cycle it consumes, so new components (PPU, DMA,
 * APU) only need to be added here.
 */
static void emulator_tick(void *context, CpuCycles cycles)
{
    Emulator *emulator = context;

    emulator->cycles += cycles;

    timer_step(&emulator->timer, cycles);
    serial_step(&emulator->serial, cycles);
}


/*
 * Starts a new machine execution state. Does not touch the cartridge or
 * the serial output callback.
 */
static void emulator_reset(Emulator *emulator)
{
    memory_init(&emulator->memory);
    interrupts_init(&emulator->interrupts);
    timer_init(&emulator->timer, &emulator->interrupts);
    serial_reset(&emulator->serial);
    cpu_init(&emulator->cpu, &emulator->bus);
    cpu_set_tick_handler(&emulator->cpu, emulator_tick, emulator);

    emulator->cycles = 0;
    atomic_store(&emulator->running, false);
}


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
     * only keeps references to them.
     */
    atomic_init(&emulator->running, false);
    emulator->unsupported_cartridge = false;
    emulator->unsupported_cartridge_type = 0;
    cartridge_init(&emulator->cartridge);
    interrupts_init(&emulator->interrupts);
    serial_init(&emulator->serial, &emulator->interrupts);

    bus_init(
        &emulator->bus,
        &emulator->cartridge,
        &emulator->memory,
        &emulator->interrupts
    );
    bus_attach_timer(&emulator->bus, &emulator->timer);
    bus_attach_serial(&emulator->bus, &emulator->serial);

    emulator_reset(emulator);

    return emulator;
}


EmulatorStatus emulator_load_rom(
    Emulator *emulator,
    const char *path
)
{
    if (emulator == NULL || path == NULL) {
        return EMULATOR_ERROR_INVALID_ARGUMENT;
    }

    emulator->unsupported_cartridge = false;

    switch (cartridge_load(
        &emulator->cartridge,
        path,
        &emulator->unsupported_cartridge_type
    )) {
        case CARTRIDGE_LOAD_OK:
            break;

        case CARTRIDGE_LOAD_INVALID_ARGUMENT:
            return EMULATOR_ERROR_INVALID_ARGUMENT;

        case CARTRIDGE_LOAD_OUT_OF_MEMORY:
            return EMULATOR_ERROR_OUT_OF_MEMORY;

        case CARTRIDGE_LOAD_UNSUPPORTED_TYPE:
            emulator->unsupported_cartridge = true;
            return EMULATOR_ERROR_UNSUPPORTED_CARTRIDGE;

        case CARTRIDGE_LOAD_IO_ERROR:
            return EMULATOR_ERROR_ROM_LOAD_FAILED;
    }

    /* Loading a ROM starts a new machine execution state. */
    emulator_reset(emulator);

    return EMULATOR_OK;
}

EmulatorStatus emulator_step(Emulator *emulator)
{
    if (emulator == NULL) {
        return EMULATOR_ERROR_INVALID_ARGUMENT;
    }

    if (emulator->cartridge.rom == NULL) {
        return EMULATOR_ERROR_NO_ROM;
    }

    /* The CPU ticks the other components as it consumes M-cycles. */
    cpu_step(&emulator->cpu);

    if (emulator->cpu.step_status == CPU_STEP_UNIMPLEMENTED_OPCODE) {
        return EMULATOR_ERROR_ILLEGAL_OPCODE;
    }

    return EMULATOR_OK;
}


EmulatorStatus emulator_run_cycles(Emulator *emulator, uint64_t cycles)
{
    if (emulator == NULL) {
        return EMULATOR_ERROR_INVALID_ARGUMENT;
    }

    if (emulator->cartridge.rom == NULL) {
        return EMULATOR_ERROR_NO_ROM;
    }

    uint64_t start = emulator->cycles;
    EmulatorStatus status = EMULATOR_OK;

    atomic_store(&emulator->running, true);

    while (atomic_load(&emulator->running) &&
           emulator->cycles - start < cycles) {
        status = emulator_step(emulator);

        if (status != EMULATOR_OK) {
            break;
        }

        if (emulator_is_stalled(emulator)) {
            status = EMULATOR_STALLED;
            break;
        }
    }

    atomic_store(&emulator->running, false);

    return status;
}


EmulatorStatus emulator_run(Emulator *emulator)
{
    return emulator_run_cycles(emulator, UINT64_MAX);
}


void emulator_stop(Emulator *emulator)
{
    if (emulator == NULL) {
        return;
    }

    atomic_store(&emulator->running, false);
}


bool emulator_is_running(const Emulator *emulator)
{
    if (emulator == NULL) {
        return false;
    }

    return atomic_load(&emulator->running);
}


bool emulator_is_stalled(const Emulator *emulator)
{
    if (emulator == NULL) {
        return false;
    }

    if (emulator->cpu.halted) {
        /* HALT only ends when IF & IE is non-zero. */
        return (emulator->interrupts.interrupt_enable &
                INTERRUPT_VALID_MASK) == 0;
    }

    if (emulator->cpu.stopped) {
        /* There is no joypad yet, so nothing can request its interrupt. */
        return (emulator->interrupts.interrupt_flag &
                INTERRUPT_JOYPAD) == 0;
    }

    return false;
}


uint64_t emulator_cycles(const Emulator *emulator)
{
    if (emulator == NULL) {
        return 0;
    }

    return emulator->cycles;
}


bool emulator_get_fault(const Emulator *emulator, EmulatorFault *fault)
{
    if (emulator == NULL || fault == NULL ||
        emulator->cpu.step_status != CPU_STEP_UNIMPLEMENTED_OPCODE) {
        return false;
    }

    fault->pc = emulator->cpu.fault_pc;
    fault->opcode = emulator->cpu.fault_opcode;

    return true;
}


bool emulator_get_unsupported_cartridge_type(
    const Emulator *emulator,
    uint8_t *type
)
{
    if (emulator == NULL || type == NULL || !emulator->unsupported_cartridge) {
        return false;
    }

    *type = emulator->unsupported_cartridge_type;

    return true;
}


const char *emulator_status_string(EmulatorStatus status)
{
    switch (status) {
        case EMULATOR_OK:
            return "ok";

        case EMULATOR_ERROR_INVALID_ARGUMENT:
            return "invalid argument";

        case EMULATOR_ERROR_NO_ROM:
            return "no ROM loaded";

        case EMULATOR_ERROR_ROM_LOAD_FAILED:
            return "ROM file could not be read or is empty";

        case EMULATOR_ERROR_UNSUPPORTED_CARTRIDGE:
            return "unsupported cartridge type (only ROM-only and MBC1)";

        case EMULATOR_ERROR_OUT_OF_MEMORY:
            return "out of memory";

        case EMULATOR_ERROR_ILLEGAL_OPCODE:
            return "undefined opcode";

        case EMULATOR_STALLED:
            return "CPU halted with no possible wake-up source";
    }

    return "unknown status";
}


void emulator_set_serial_output(
    Emulator *emulator,
    EmulatorSerialFn output,
    void *context
)
{
    if (emulator == NULL) {
        return;
    }

    serial_set_output(&emulator->serial, output, context);
}


void emulator_destroy(Emulator *emulator)
{
    if (emulator == NULL) {
        return;
    }

    atomic_store(&emulator->running, false);

    cartridge_destroy(&emulator->cartridge);

    free(emulator);
}
