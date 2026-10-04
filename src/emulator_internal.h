#ifndef EMULATOR_INTERNAL_H
#define EMULATOR_INTERNAL_H

#include <stdatomic.h>
#include <stdint.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <emulator.h>
#include <interrupts.h>
#include <memory.h>
#include <serial.h>
#include <timer.h>

struct Emulator {
    Cartridge cartridge;
    Memory memory;
    InterruptRegisters interrupts;
    Timer timer;
    Serial serial;
    Bus bus;
    CPU cpu;

    uint64_t cycles;

    /* Why the last emulator_load_rom() failed, if it was the cartridge type. */
    bool unsupported_cartridge;
    uint8_t unsupported_cartridge_type;

    atomic_bool running;

    /* Set by emulator_stop(), cleared by the run that ends because of it. */
    atomic_bool stop_requested;
};

/* emulator_stop() is documented as async-signal-safe, which needs this. */
static_assert(
    ATOMIC_BOOL_LOCK_FREE == 2,
    "atomic_bool must be lock-free for emulator_stop()"
);

#endif
