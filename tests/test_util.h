#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <interrupts.h>
#include <memory.h>
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
 * A CPU wired to a Bus, 32 KiB of ROM, memory, a Timer and the interrupt
 * registers, with a tick handler that counts the time the CPU consumes and
 * advances the Timer, as the Emulator does.
 */
typedef struct TestMachine {
    Cartridge cartridge;
    Memory memory;
    InterruptRegisters interrupts;
    Timer timer;
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

    bus_init(
        &machine->bus,
        &machine->cartridge,
        &machine->memory,
        &machine->interrupts
    );
    bus_attach_timer(&machine->bus, &machine->timer);

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
