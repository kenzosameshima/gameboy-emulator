#ifndef EMULATOR_H
#define EMULATOR_H

#include <stdbool.h>
#include <stdint.h>


typedef struct Emulator Emulator;


/*
 * Result of emulator operations. EMULATOR_OK is zero, so callers that only
 * care about success can keep testing for non-zero.
 */
typedef enum {
    EMULATOR_OK = 0,
    EMULATOR_ERROR_INVALID_ARGUMENT,
    EMULATOR_ERROR_NO_ROM,
    /* The ROM file could not be opened or read, or it is empty. */
    EMULATOR_ERROR_ROM_LOAD_FAILED,
    /* The ROM's cartridge type is not implemented; see
     * emulator_get_unsupported_cartridge_type(). */
    EMULATOR_ERROR_UNSUPPORTED_CARTRIDGE,
    EMULATOR_ERROR_OUT_OF_MEMORY,
    /* The CPU reached one of the undefined opcodes; see emulator_get_fault(). */
    EMULATOR_ERROR_ILLEGAL_OPCODE,
    /*
     * The CPU is halted (or stopped) and nothing can wake it up, so
     * running further would spin forever. Not an emulation error.
     */
    EMULATOR_STALLED
} EmulatorStatus;

typedef struct EmulatorFault {
    uint16_t pc;
    uint8_t opcode;
} EmulatorFault;

/* Called with each byte the game sends through the serial port. */
typedef void (*EmulatorSerialFn)(void *context, uint8_t byte);


/**
 * Creates and initializes an emulator instance.
 *
 * @return Pointer to the emulator, or NULL on failure.
 */
Emulator *emulator_create(void);


/**
 * Loads a Game Boy ROM into the emulator.
 *
 * @param emulator Emulator instance.
 * @param path Path to the ROM file.
 *
 * A successful load starts a new machine state and resets CPU, Memory,
 * Timer, Serial and the cycle counter.
 * A failed load leaves the currently loaded ROM unchanged.
 *
 * @return EMULATOR_OK on success, EMULATOR_ERROR_ROM_LOAD_FAILED if the
 *         file cannot be read, EMULATOR_ERROR_UNSUPPORTED_CARTRIDGE if its
 *         header names an unimplemented cartridge type, or
 *         EMULATOR_ERROR_OUT_OF_MEMORY.
 */
EmulatorStatus emulator_load_rom(
    Emulator *emulator,
    const char *path
);

/*
 * Advances the whole machine by one CPU step (one instruction, one
 * interrupt dispatch or one halted M-cycle). The other components are
 * advanced in lockstep, once per M-cycle the CPU consumes.
 * CPU HALT is a valid step; emulator_stop() is the machine stop request.
 */
EmulatorStatus emulator_step(Emulator *emulator);


/**
 * Runs the emulation loop until at least `cycles` T-cycles have elapsed,
 * emulator_stop() is called, or an error occurs. Can overshoot the budget
 * by less than one instruction.
 *
 * @return EMULATOR_OK when the budget was used up or a stop was requested,
 *         EMULATOR_STALLED if the CPU can never wake up again, otherwise
 *         the error that ended the run.
 */
EmulatorStatus emulator_run_cycles(Emulator *emulator, uint64_t cycles);


/**
 * Runs the emulation loop with no cycle budget: until emulator_stop() is
 * called, the CPU stalls, or an error occurs.
 */
EmulatorStatus emulator_run(Emulator *emulator);


/**
 * Requests the emulation loop to stop. Safe to call from a serial callback,
 * a signal handler or another thread.
 *
 * @param emulator Emulator instance.
 */
void emulator_stop(Emulator *emulator);


/**
 * Checks whether the emulator is currently running.
 *
 * @param emulator Emulator instance.
 *
 * @return true if running, false otherwise.
 */
bool emulator_is_running(
    const Emulator *emulator
);


/**
 * Checks whether the CPU is halted or stopped with no interrupt that
 * could ever wake it up (HALT with IE = 0, or STOP without joypad input).
 */
bool emulator_is_stalled(const Emulator *emulator);


/** T-cycles elapsed since the last ROM load. */
uint64_t emulator_cycles(const Emulator *emulator);


/**
 * Describes the undefined opcode that ended emulation.
 *
 * @return true and fills `fault` if the last step hit an undefined opcode.
 */
bool emulator_get_fault(const Emulator *emulator, EmulatorFault *fault);


/**
 * Reports the cartridge type byte (header address 0x0147) that made the
 * last emulator_load_rom() fail with EMULATOR_ERROR_UNSUPPORTED_CARTRIDGE.
 *
 * @return true and fills `type` if the last load failed for that reason.
 */
bool emulator_get_unsupported_cartridge_type(
    const Emulator *emulator,
    uint8_t *type
);


/** Human-readable text for a status value. */
const char *emulator_status_string(EmulatorStatus status);


/**
 * Sets the serial output callback. Blargg-style test ROMs report their
 * results through it. Pass NULL to remove it.
 */
void emulator_set_serial_output(
    Emulator *emulator,
    EmulatorSerialFn output,
    void *context
);


/**
 * Destroys an emulator instance and releases its resources.
 *
 * @param emulator Emulator instance.
 */
void emulator_destroy(Emulator *emulator);

#endif
