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

    atomic_bool running;
};

#endif
