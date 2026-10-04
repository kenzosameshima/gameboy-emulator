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
    serial_step(&emulator->serial, cycles, emulator->timer.divider);
    dma_step(&emulator->dma, cycles);
    ppu_step(&emulator->ppu, cycles);
    cartridge_step(&emulator->cartridge, cycles);
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
    timer_power_on(&emulator->timer);
    ppu_init(&emulator->ppu, &emulator->interrupts);
    dma_init(&emulator->dma, &emulator->bus, &emulator->ppu);
    joypad_init(&emulator->joypad, &emulator->interrupts);
    serial_reset(&emulator->serial);
    cpu_init(&emulator->cpu, &emulator->bus, &emulator->interrupts);
    cpu_set_tick_handler(&emulator->cpu, emulator_tick, emulator);

    emulator->cycles = 0;
    atomic_store(&emulator->running, false);
    atomic_store(&emulator->stop_requested, false);
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
    atomic_init(&emulator->stop_requested, false);
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
    bus_attach_ppu(&emulator->bus, &emulator->ppu);
    bus_attach_dma(&emulator->bus, &emulator->dma);
    bus_attach_joypad(&emulator->bus, &emulator->joypad);

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

    while (!atomic_load(&emulator->stop_requested) &&
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

    /* The run that observed a stop request consumes it. */
    atomic_store(&emulator->running, false);
    atomic_store(&emulator->stop_requested, false);

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

    atomic_store(&emulator->stop_requested, true);
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

    return cpu_is_stalled(&emulator->cpu);
}


uint64_t emulator_cycles(const Emulator *emulator)
{
    if (emulator == NULL) {
        return 0;
    }

    return emulator->cycles;
}


void emulator_set_buttons(Emulator *emulator, uint8_t pressed)
{
    if (emulator == NULL) {
        return;
    }

    joypad_set_pressed(&emulator->joypad, pressed);
}


const uint8_t *emulator_framebuffer(const Emulator *emulator)
{
    return emulator == NULL ? NULL : emulator->ppu.framebuffer;
}


uint64_t emulator_frame_count(const Emulator *emulator)
{
    return emulator == NULL ? 0 : emulator->ppu.frames;
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


/* Maps a battery save result to the emulator status. */
static EmulatorStatus emulator_save_status(CartridgeSaveStatus status)
{
    switch (status) {
        case CARTRIDGE_SAVE_OK:
            return EMULATOR_OK;

        case CARTRIDGE_SAVE_NOT_BATTERY_BACKED:
            return EMULATOR_ERROR_NOT_BATTERY_BACKED;

        case CARTRIDGE_SAVE_NO_FILE:
            return EMULATOR_NO_SAVE_FILE;

        case CARTRIDGE_SAVE_BAD_SIZE:
            return EMULATOR_ERROR_SAVE_SIZE;

        case CARTRIDGE_SAVE_IO_ERROR:
            break;
    }

    return EMULATOR_ERROR_SAVE_IO;
}


bool emulator_has_battery(const Emulator *emulator)
{
    return emulator != NULL && emulator->cartridge.rom != NULL &&
           emulator->cartridge.has_battery;
}


EmulatorStatus emulator_save_battery(
    const Emulator *emulator,
    const char *path,
    uint64_t unix_time
)
{
    if (emulator == NULL || path == NULL) {
        return EMULATOR_ERROR_INVALID_ARGUMENT;
    }

    if (emulator->cartridge.rom == NULL) {
        return EMULATOR_ERROR_NO_ROM;
    }

    return emulator_save_status(
        cartridge_save_battery(&emulator->cartridge, path, unix_time)
    );
}


EmulatorStatus emulator_load_battery(
    Emulator *emulator,
    const char *path,
    uint64_t unix_time
)
{
    if (emulator == NULL || path == NULL) {
        return EMULATOR_ERROR_INVALID_ARGUMENT;
    }

    if (emulator->cartridge.rom == NULL) {
        return EMULATOR_ERROR_NO_ROM;
    }

    return emulator_save_status(
        cartridge_load_battery(&emulator->cartridge, path, unix_time)
    );
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
            return "unsupported cartridge type (supported: ROM-only, MBC1, MBC3)";

        case EMULATOR_ERROR_OUT_OF_MEMORY:
            return "out of memory";

        case EMULATOR_ERROR_ILLEGAL_OPCODE:
            return "undefined opcode";

        case EMULATOR_STALLED:
            return "CPU halted with no possible wake-up source";

        case EMULATOR_NO_SAVE_FILE:
            return "no save file yet";

        case EMULATOR_ERROR_NOT_BATTERY_BACKED:
            return "the cartridge has no battery";

        case EMULATOR_ERROR_SAVE_IO:
            return "the save file could not be read or written";

        case EMULATOR_ERROR_SAVE_SIZE:
            return "the save file is not the size this cartridge saves";
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
