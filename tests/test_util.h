#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <apu.h>
#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <dma.h>
#include <interrupts.h>
#include <joypad.h>
#include <memory.h>
#include <ppu.h>
#include <serial.h>
#include <timer.h>

static inline void test_write_file(
    const char *path,
    const uint8_t *data,
    size_t size
)
{
    FILE *file = fopen(path, "wb");

    assert(file != NULL);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

/*
 * A CPU wired to a Bus with 32 KiB of ROM and every device on it. The tick
 * handler counts the time the CPU consumes and advances only the Timer: the
 * other devices answer on the bus but stand still unless a test steps them.
 */
typedef struct TestMachine {
    Cartridge cartridge;
    Memory memory;
    InterruptRegisters interrupts;
    Timer timer;
    Serial serial;
    Ppu ppu;
    Dma dma;
    Joypad joypad;
    Apu apu;
    Bus bus;
    CPU cpu;

    uint64_t ticked_cycles;
    unsigned tick_count;
} TestMachine;

static inline void test_machine_tick(void *context, CpuCycles cycles)
{
    TestMachine *machine = context;

    machine->ticked_cycles += cycles;
    machine->tick_count++;

    timer_step(&machine->timer, cycles);
}

static inline void test_machine_reset_cpu(TestMachine *machine)
{
    cpu_init(&machine->cpu, &machine->bus, &machine->interrupts);
    cpu_set_tick_handler(&machine->cpu, test_machine_tick, machine);

    machine->ticked_cycles = 0;
    machine->tick_count = 0;
}

static inline void test_machine_init(TestMachine *machine)
{
    cartridge_init(&machine->cartridge);
    machine->cartridge.rom_size = 0x8000;
    machine->cartridge.rom = calloc(
        machine->cartridge.rom_size,
        sizeof(uint8_t)
    );
    assert(machine->cartridge.rom != NULL);

    memory_init(&machine->memory);
    interrupts_init(&machine->interrupts);
    timer_init(&machine->timer, &machine->interrupts);
    serial_init(&machine->serial, &machine->interrupts);
    ppu_init(&machine->ppu, &machine->interrupts);
    joypad_init(&machine->joypad, &machine->interrupts);
    apu_init(&machine->apu);

    bus_init(
        &machine->bus,
        &machine->cartridge,
        &machine->memory,
        &machine->interrupts,
        &machine->timer,
        &machine->serial,
        &machine->ppu,
        &machine->dma,
        &machine->joypad,
        &machine->apu
    );
    dma_init(&machine->dma, &machine->bus, &machine->ppu);

    test_machine_reset_cpu(machine);
}

static inline void test_machine_destroy(TestMachine *machine)
{
    cartridge_destroy(&machine->cartridge);
}

/* Copies a program into ROM. Execution starts at 0x0100 after a reset. */
static inline void test_machine_load(
    TestMachine *machine,
    uint16_t address,
    const uint8_t *bytes,
    size_t size
)
{
    assert(address + size <= machine->cartridge.rom_size);

    memcpy(machine->cartridge.rom + address, bytes, size);
}

#endif
