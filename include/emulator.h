#ifndef EMULATOR_H
#define EMULATOR_H

#include <stdbool.h>
#include <stdint.h>


typedef struct Emulator Emulator;


enum {
    EMULATOR_SCREEN_WIDTH = 160,
    EMULATOR_SCREEN_HEIGHT = 144
};


/* Bits of the mask given to emulator_set_buttons(). */
enum {
    EMULATOR_BUTTON_RIGHT = 0x01,
    EMULATOR_BUTTON_LEFT = 0x02,
    EMULATOR_BUTTON_UP = 0x04,
    EMULATOR_BUTTON_DOWN = 0x08,
    EMULATOR_BUTTON_A = 0x10,
    EMULATOR_BUTTON_B = 0x20,
    EMULATOR_BUTTON_SELECT = 0x40,
    EMULATOR_BUTTON_START = 0x80
};


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
    EMULATOR_STALLED,
    /* Loading a battery save: there is no file yet, which is normal the
     * first time and not an error. */
    EMULATOR_NO_SAVE_FILE,
    /* The cartridge has no battery, so there is nothing to save or load. */
    EMULATOR_ERROR_NOT_BATTERY_BACKED,
    EMULATOR_ERROR_SAVE_IO,
    /* The save file is not the size this cartridge saves. */
    EMULATOR_ERROR_SAVE_SIZE
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
 * The request is not lost if the loop has not started yet: the next
 * emulator_run*() call returns EMULATOR_OK without executing anything. The
 * run that observes the request consumes it, and emulator_load_rom() drops
 * a pending one.
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
 * Checks whether the CPU is halted with no interrupt that could ever wake it
 * up (HALT with IE = 0). STOP is never stalled: a button press ends it.
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


/**
 * The LCD picture: EMULATOR_SCREEN_WIDTH x EMULATOR_SCREEN_HEIGHT shades,
 * row-major, 0 (lightest) to 3 (darkest) after the palette registers. It is
 * drawn line by line as the machine runs, so a complete picture is there
 * once emulator_frame_count() has gone up. The pointer stays valid for the
 * life of the emulator.
 *
 * @return the framebuffer, or NULL if `emulator` is NULL.
 */
const uint8_t *emulator_framebuffer(const Emulator *emulator);


/** Frames completed (VBlank entered) since the last ROM load. */
uint64_t emulator_frame_count(const Emulator *emulator);


/**
 * Reports which buttons are held down, as a mask of EMULATOR_BUTTON_* bits.
 * Call it whenever the set changes; it replaces the previous set. Pressing
 * a button requests the joypad interrupt (and ends STOP) as on hardware.
 * The set is cleared by a ROM load.
 */
void emulator_set_buttons(Emulator *emulator, uint8_t pressed);


/**
 * Whether the loaded cartridge has a battery: RAM, and for some MBC3
 * cartridges a clock, that a real console keeps while it is off.
 */
bool emulator_has_battery(const Emulator *emulator);


/**
 * Saves the cartridge RAM (and the MBC3 clock) to `path`: a raw dump of the
 * RAM, which other emulators read too, with the 48-byte clock footer BGB and
 * VBA-M use when there is a clock. The file is written to a temporary name
 * and renamed into place, so a failed save leaves the old one alone.
 *
 * The core has no clock: `unix_time` is the current time in seconds since
 * 1970, recorded with the clock so a later load can add the time that
 * passed.
 *
 * @return EMULATOR_OK, EMULATOR_ERROR_NOT_BATTERY_BACKED,
 *         EMULATOR_ERROR_SAVE_IO or EMULATOR_ERROR_NO_ROM.
 */
EmulatorStatus emulator_save_battery(
    const Emulator *emulator,
    const char *path,
    uint64_t unix_time
);


/**
 * Loads a save written by emulator_save_battery() into the cartridge, and
 * advances an MBC3 clock by the time between the save and `unix_time` unless
 * it is halted. Call it after emulator_load_rom(), which clears the RAM.
 * Nothing changes if it fails.
 *
 * @return EMULATOR_OK; EMULATOR_NO_SAVE_FILE if there is no file yet (not an
 *         error); EMULATOR_ERROR_SAVE_SIZE for a file of the wrong size; or
 *         EMULATOR_ERROR_NOT_BATTERY_BACKED, EMULATOR_ERROR_SAVE_IO,
 *         EMULATOR_ERROR_NO_ROM.
 */
EmulatorStatus emulator_load_battery(
    Emulator *emulator,
    const char *path,
    uint64_t unix_time
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
