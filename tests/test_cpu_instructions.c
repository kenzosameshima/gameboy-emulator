#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <memory.h>
#include <timer.h>

static InterruptRegisters interrupt_registers;

static void setup_cpu(
    CPU *cpu,
    Bus *bus,
    Memory *memory,
    Cartridge *cartridge
)
{
    cartridge_init(cartridge);
    cartridge->rom_size = 0x8000;
    cartridge->rom = calloc(cartridge->rom_size, sizeof(uint8_t));
    assert(cartridge->rom != NULL);

    memory_init(memory);
    interrupt_registers.interrupt_flag = 0;
    interrupt_registers.interrupt_enable = 0;
    bus_init(bus, cartridge, memory, &interrupt_registers);
    cpu_init(cpu, bus);
}

static void cleanup_cpu(Cartridge *cartridge)
{
    cartridge_destroy(cartridge);
}

static void test_simple_loads(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    setup_cpu(&cpu, &bus, &memory, &cartridge);

    cartridge.rom[0x0100] = 0x00;
    assert(cpu_step(&cpu) == 4);
    assert(cpu.registers.pc == 0x0101);
    assert(cpu.registers.f == 0xB0);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x3E;
    cartridge.rom[0x0101] = 0x42;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.a == 0x42);
    assert(cpu.registers.pc == 0x0102);
    assert(cpu.registers.f == 0xB0);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x01;
    cartridge.rom[0x0101] = 0x34;
    cartridge.rom[0x0102] = 0x12;
    cpu.registers.f = 0xF0;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.b == 0x12);
    assert(cpu.registers.c == 0x34);
    assert(cpu.registers.pc == 0x0103);
    assert(cpu.registers.f == 0xF0);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x31;
    cartridge.rom[0x0101] = 0xFE;
    cartridge.rom[0x0102] = 0xFF;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.sp == 0xFFFE);
    assert(cpu.registers.pc == 0x0103);

    cleanup_cpu(&cartridge);
}

static void test_direct_memory_loads_and_stores(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    setup_cpu(&cpu, &bus, &memory, &cartridge);

    cpu.registers.a = 0x5A;
    cpu.registers.b = 0xC0;
    cpu.registers.c = 0x00;
    cartridge.rom[0x0100] = 0x02;
    assert(cpu_step(&cpu) == 8);
    assert(memory_read(&memory, 0xC000) == 0x5A);

    cpu_init(&cpu, &bus);
    cpu.registers.a = 0x6B;
    cpu.registers.d = 0xC0;
    cpu.registers.e = 0x01;
    cartridge.rom[0x0100] = 0x12;
    assert(cpu_step(&cpu) == 8);
    assert(memory_read(&memory, 0xC001) == 0x6B);

    memory_write(&memory, 0xC000, 0x71);
    cpu_init(&cpu, &bus);
    cpu.registers.b = 0xC0;
    cpu.registers.c = 0x00;
    cartridge.rom[0x0100] = 0x0A;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.a == 0x71);

    memory_write(&memory, 0xC001, 0x82);
    cpu_init(&cpu, &bus);
    cpu.registers.d = 0xC0;
    cpu.registers.e = 0x01;
    cartridge.rom[0x0100] = 0x1A;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.a == 0x82);

    cleanup_cpu(&cartridge);
}

static void test_hl_increment_decrement_loads(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    setup_cpu(&cpu, &bus, &memory, &cartridge);

    cpu.registers.a = 0x91;
    cpu.registers.h = 0xC0;
    cpu.registers.l = 0x00;
    cartridge.rom[0x0100] = 0x22;
    assert(cpu_step(&cpu) == 8);
    assert(memory_read(&memory, 0xC000) == 0x91);
    assert(cpu.registers.h == 0xC0);
    assert(cpu.registers.l == 0x01);

    cpu_init(&cpu, &bus);
    cpu.registers.a = 0xA2;
    cpu.registers.h = 0xC0;
    cpu.registers.l = 0x01;
    cartridge.rom[0x0100] = 0x32;
    assert(cpu_step(&cpu) == 8);
    assert(memory_read(&memory, 0xC001) == 0xA2);
    assert(cpu.registers.h == 0xC0);
    assert(cpu.registers.l == 0x00);

    memory_write(&memory, 0xC000, 0xB3);
    cpu_init(&cpu, &bus);
    cpu.registers.h = 0xC0;
    cpu.registers.l = 0x00;
    cartridge.rom[0x0100] = 0x2A;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.a == 0xB3);
    assert(cpu.registers.l == 0x01);

    memory_write(&memory, 0xC001, 0xC4);
    cpu_init(&cpu, &bus);
    cpu.registers.h = 0xC0;
    cpu.registers.l = 0x01;
    cartridge.rom[0x0100] = 0x3A;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.a == 0xC4);
    assert(cpu.registers.l == 0x00);

    cleanup_cpu(&cartridge);
}

static void test_jump(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    setup_cpu(&cpu, &bus, &memory, &cartridge);

    cartridge.rom[0x0100] = 0xC3;
    cartridge.rom[0x0101] = 0x20;
    cartridge.rom[0x0102] = 0x01;
    cpu.registers.f = 0xF0;

    assert(cpu_step(&cpu) == 16);
    assert(cpu.registers.pc == 0x0120);
    assert(cpu.registers.f == 0xF0);

    cleanup_cpu(&cartridge);
}

static void test_relative_jumps(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    setup_cpu(&cpu, &bus, &memory, &cartridge);

    cartridge.rom[0x0100] = 0x18;
    cartridge.rom[0x0101] = 0x05;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.pc == 0x0107);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x18;
    cartridge.rom[0x0101] = 0xFE;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.pc == 0x0100);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x18;
    cartridge.rom[0x0101] = 0x00;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.pc == 0x0102);

    cpu_init(&cpu, &bus);
    cpu.registers.pc = 0xFFFE;
    memory_write(&memory, 0xFFFE, 0x18);
    bus_write(&bus, INTERRUPT_ENABLE_ADDRESS, 0x01);
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.pc == 0x0001);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x20;
    cartridge.rom[0x0101] = 0x02;
    cpu.registers.f = 0;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.pc == 0x0104);

    cpu_init(&cpu, &bus);
    cpu.registers.f = FLAG_Z;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.pc == 0x0102);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x28;
    cartridge.rom[0x0101] = 0x02;
    cpu.registers.f = FLAG_Z;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.pc == 0x0104);

    cpu_init(&cpu, &bus);
    cpu.registers.f = 0;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.pc == 0x0102);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x30;
    cartridge.rom[0x0101] = 0x02;
    cpu.registers.f = 0;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.pc == 0x0104);

    cpu_init(&cpu, &bus);
    cpu.registers.f = FLAG_C;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.pc == 0x0102);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x38;
    cartridge.rom[0x0101] = 0x02;
    cpu.registers.f = FLAG_C;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.pc == 0x0104);

    cpu_init(&cpu, &bus);
    cpu.registers.f = 0;
    assert(cpu_step(&cpu) == 8);
    assert(cpu.registers.pc == 0x0102);

    cleanup_cpu(&cartridge);
}

static void test_xor_a_and_ldh(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    Timer timer;
    InterruptRegisters interrupts;

    cartridge_init(&cartridge);
    cartridge.rom_size = 0x8000;
    cartridge.rom = calloc(cartridge.rom_size, sizeof(uint8_t));
    assert(cartridge.rom != NULL);
    memory_init(&memory);
    interrupts.interrupt_flag = 0;
    interrupts.interrupt_enable = 0;
    timer_init(&timer, &interrupts);
    bus_init(&bus, &cartridge, &memory, &interrupts);
    bus_attach_timer(&bus, &timer);
    cpu_init(&cpu, &bus);

    cartridge.rom[0x0100] = 0xAF;
    cpu.registers.a = 0xA5;
    cpu.registers.f = (uint8_t)(FLAG_N | FLAG_H | FLAG_C);
    cpu.registers.b = 0x12;
    assert(cpu_step(&cpu) == 4);
    assert(cpu.registers.a == 0);
    assert(cpu.registers.f == FLAG_Z);
    assert(cpu.registers.b == 0x12);
    assert(cpu.registers.pc == 0x0101);

    cpu_init(&cpu, &bus);
    cpu.registers.a = 0x5A;
    cartridge.rom[0x0100] = 0xE0;
    cartridge.rom[0x0101] = 0x0F;
    assert(cpu_step(&cpu) == 12);
    assert(bus_read(&bus, INTERRUPT_FLAG_ADDRESS) == 0x1A);
    assert(cpu.registers.pc == 0x0102);

    cpu_init(&cpu, &bus);
    bus_write(&bus, INTERRUPT_ENABLE_ADDRESS, 0x12);
    cartridge.rom[0x0100] = 0xF0;
    cartridge.rom[0x0101] = 0xFF;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.a == 0x12);
    assert(cpu.registers.pc == 0x0102);

    cpu_init(&cpu, &bus);
    bus_write(&bus, TIMER_DIV_ADDRESS, 0xFF);
    cartridge.rom[0x0100] = 0xF0;
    cartridge.rom[0x0101] = 0x04;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.a == 0);

    cpu_init(&cpu, &bus);
    bus_write(&bus, TIMER_TIMA_ADDRESS, 0x34);
    cartridge.rom[0x0100] = 0xF0;
    cartridge.rom[0x0101] = 0x05;
    assert(cpu_step(&cpu) == 12);
    assert(cpu.registers.a == 0x34);

    cartridge_destroy(&cartridge);
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
