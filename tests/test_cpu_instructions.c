#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <memory.h>
#include <timer.h>

#include "test_util.h"

/* The CPU runs without a tick handler, so nothing else on the bus moves. */
static void setup_cpu(TestMachine *machine)
{
    test_machine_init(machine);
    cpu_init(&machine->cpu, &machine->bus, &machine->interrupts);
}

static void test_simple_loads(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cartridge.rom[0x0100] = 0x00;
    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.registers.pc == 0x0101);
    assert(machine.cpu.registers.f == 0xB0);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x3E;
    machine.cartridge.rom[0x0101] = 0x42;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.a == 0x42);
    assert(machine.cpu.registers.pc == 0x0102);
    assert(machine.cpu.registers.f == 0xB0);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x01;
    machine.cartridge.rom[0x0101] = 0x34;
    machine.cartridge.rom[0x0102] = 0x12;
    machine.cpu.registers.f = 0xF0;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.b == 0x12);
    assert(machine.cpu.registers.c == 0x34);
    assert(machine.cpu.registers.pc == 0x0103);
    assert(machine.cpu.registers.f == 0xF0);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x31;
    machine.cartridge.rom[0x0101] = 0xFE;
    machine.cartridge.rom[0x0102] = 0xFF;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.sp == 0xFFFE);
    assert(machine.cpu.registers.pc == 0x0103);

    test_machine_destroy(&machine);
}

static void test_direct_memory_loads_and_stores(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cpu.registers.a = 0x5A;
    machine.cpu.registers.b = 0xC0;
    machine.cpu.registers.c = 0x00;
    machine.cartridge.rom[0x0100] = 0x02;
    assert(cpu_step(&machine.cpu) == 8);
    assert(memory_read(&machine.memory, 0xC000) == 0x5A);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.a = 0x6B;
    machine.cpu.registers.d = 0xC0;
    machine.cpu.registers.e = 0x01;
    machine.cartridge.rom[0x0100] = 0x12;
    assert(cpu_step(&machine.cpu) == 8);
    assert(memory_read(&machine.memory, 0xC001) == 0x6B);

    memory_write(&machine.memory, 0xC000, 0x71);
    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.b = 0xC0;
    machine.cpu.registers.c = 0x00;
    machine.cartridge.rom[0x0100] = 0x0A;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.a == 0x71);

    memory_write(&machine.memory, 0xC001, 0x82);
    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.d = 0xC0;
    machine.cpu.registers.e = 0x01;
    machine.cartridge.rom[0x0100] = 0x1A;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.a == 0x82);

    test_machine_destroy(&machine);
}

static void test_hl_increment_decrement_loads(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cpu.registers.a = 0x91;
    machine.cpu.registers.h = 0xC0;
    machine.cpu.registers.l = 0x00;
    machine.cartridge.rom[0x0100] = 0x22;
    assert(cpu_step(&machine.cpu) == 8);
    assert(memory_read(&machine.memory, 0xC000) == 0x91);
    assert(machine.cpu.registers.h == 0xC0);
    assert(machine.cpu.registers.l == 0x01);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.a = 0xA2;
    machine.cpu.registers.h = 0xC0;
    machine.cpu.registers.l = 0x01;
    machine.cartridge.rom[0x0100] = 0x32;
    assert(cpu_step(&machine.cpu) == 8);
    assert(memory_read(&machine.memory, 0xC001) == 0xA2);
    assert(machine.cpu.registers.h == 0xC0);
    assert(machine.cpu.registers.l == 0x00);

    memory_write(&machine.memory, 0xC000, 0xB3);
    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.h = 0xC0;
    machine.cpu.registers.l = 0x00;
    machine.cartridge.rom[0x0100] = 0x2A;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.a == 0xB3);
    assert(machine.cpu.registers.l == 0x01);

    memory_write(&machine.memory, 0xC001, 0xC4);
    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.h = 0xC0;
    machine.cpu.registers.l = 0x01;
    machine.cartridge.rom[0x0100] = 0x3A;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.a == 0xC4);
    assert(machine.cpu.registers.l == 0x00);

    test_machine_destroy(&machine);
}

static void test_jump(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cartridge.rom[0x0100] = 0xC3;
    machine.cartridge.rom[0x0101] = 0x20;
    machine.cartridge.rom[0x0102] = 0x01;
    machine.cpu.registers.f = 0xF0;

    assert(cpu_step(&machine.cpu) == 16);
    assert(machine.cpu.registers.pc == 0x0120);
    assert(machine.cpu.registers.f == 0xF0);

    test_machine_destroy(&machine);
}

static void test_relative_jumps(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cartridge.rom[0x0100] = 0x18;
    machine.cartridge.rom[0x0101] = 0x05;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.pc == 0x0107);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x18;
    machine.cartridge.rom[0x0101] = 0xFE;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.pc == 0x0100);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x18;
    machine.cartridge.rom[0x0101] = 0x00;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.pc == 0x0102);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.pc = 0xFFFE;
    memory_write(&machine.memory, 0xFFFE, 0x18);
    bus_write(&machine.bus, INTERRUPT_ENABLE_ADDRESS, 0x01);
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.pc == 0x0001);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x20;
    machine.cartridge.rom[0x0101] = 0x02;
    machine.cpu.registers.f = 0;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.pc == 0x0104);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.f = FLAG_Z;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.pc == 0x0102);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x28;
    machine.cartridge.rom[0x0101] = 0x02;
    machine.cpu.registers.f = FLAG_Z;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.pc == 0x0104);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.f = 0;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.pc == 0x0102);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x30;
    machine.cartridge.rom[0x0101] = 0x02;
    machine.cpu.registers.f = 0;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.pc == 0x0104);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.f = FLAG_C;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.pc == 0x0102);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x38;
    machine.cartridge.rom[0x0101] = 0x02;
    machine.cpu.registers.f = FLAG_C;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.pc == 0x0104);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.f = 0;
    assert(cpu_step(&machine.cpu) == 8);
    assert(machine.cpu.registers.pc == 0x0102);

    test_machine_destroy(&machine);
}

static void test_xor_a_and_ldh(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cartridge.rom[0x0100] = 0xAF;
    machine.cpu.registers.a = 0xA5;
    machine.cpu.registers.f = (uint8_t)(FLAG_N | FLAG_H | FLAG_C);
    machine.cpu.registers.b = 0x12;
    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.registers.a == 0);
    assert(machine.cpu.registers.f == FLAG_Z);
    assert(machine.cpu.registers.b == 0x12);
    assert(machine.cpu.registers.pc == 0x0101);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.a = 0x5A;
    machine.cartridge.rom[0x0100] = 0xE0;
    machine.cartridge.rom[0x0101] = 0x0F;
    assert(cpu_step(&machine.cpu) == 12);
    assert(bus_read(&machine.bus, INTERRUPT_FLAG_ADDRESS) == (0x1A | 0xE0));
    assert(machine.cpu.registers.pc == 0x0102);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    bus_write(&machine.bus, INTERRUPT_ENABLE_ADDRESS, 0x12);
    machine.cartridge.rom[0x0100] = 0xF0;
    machine.cartridge.rom[0x0101] = 0xFF;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.a == 0x12);
    assert(machine.cpu.registers.pc == 0x0102);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    bus_write(&machine.bus, TIMER_DIV_ADDRESS, 0xFF);
    machine.cartridge.rom[0x0100] = 0xF0;
    machine.cartridge.rom[0x0101] = 0x04;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.a == 0);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0x34);
    machine.cartridge.rom[0x0100] = 0xF0;
    machine.cartridge.rom[0x0101] = 0x05;
    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.a == 0x34);

    test_machine_destroy(&machine);
}

int main(void)
{
    test_simple_loads();
    test_direct_memory_loads_and_stores();
    test_hl_increment_decrement_loads();
    test_jump();
    test_relative_jumps();
    test_xor_a_and_ldh();

    printf("CPU instruction tests passed!\n");
    return 0;
}
