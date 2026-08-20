#ifndef EMULATOR_H
#define EMULATOR_H

#include <stdbool.h>


typedef struct Emulator Emulator;


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
 * A successful load starts a new machine state and resets CPU and Memory.
 * A failed load leaves the currently loaded ROM unchanged.
 *
 * @return 0 on success, non-zero on failure.
 */
int emulator_load_rom(
    Emulator *emulator,
    const char *path
);

/*
 * Advances CPU and, later, the other machine components by one step.
 * CPU HALT is a valid step; emulator_stop() is the machine stop request.
 */
int emulator_step(Emulator *emulator);


/**
 * Runs the emulation loop.
 *
 * @param emulator Emulator instance.
 *
 * @return 0 on success, non-zero on failure.
 */
int emulator_run(Emulator *emulator);


/**
 * Requests the emulation loop to stop.
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
 * Destroys an emulator instance and releases its resources.
 *
 * @param emulator Emulator instance.
 */
void emulator_destroy(Emulator *emulator);

#endif
