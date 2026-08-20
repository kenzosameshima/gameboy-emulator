#ifndef EMULATOR_INTERNAL_H
#define EMULATOR_INTERNAL_H

#include <stdbool.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <emulator.h>
#include <memory.h>
#include <timer.h>

struct Emulator {
    Cartridge cartridge;
    Memory memory;
    InterruptRegisters interrupts;
    Timer timer;
    Bus bus;
    CPU cpu;

    bool running;
};

#endif
