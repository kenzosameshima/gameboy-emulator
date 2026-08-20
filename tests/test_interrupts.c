#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <memory.h>

static void setup_cpu(
    CPU *cpu,
    Bus *bus,
    Memory *memory,
    Cartridge *cartridge,
    InterruptRegisters *interrupts
)
{
    cartridge_init(cartridge);
    cartridge->rom_size = 0x8000;
    cartridge->rom = calloc(cartridge->rom_size, sizeof(uint8_t));
    assert(cartridge->rom != NULL);

    memory_init(memory);
    interrupts->interrupt_flag = 0;
    interrupts->interrupt_enable = 0;
    bus_init(bus, cartridge, memory, interrupts);
    cpu_init(cpu, bus);
}

static void cleanup_cpu(Cartridge *cartridge)
{
    cartridge_destroy(cartridge);
}

static void test_interrupt_registers(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    InterruptRegisters interrupts;
    setup_cpu(&cpu, &bus, &memory, &cartridge, &interrupts);

    assert(bus_read(&bus, 0xFF0F) == 0);
    assert(bus_read(&bus, 0xFFFF) == 0);

    bus_write(&bus, 0xFF0F, (uint8_t)(INTERRUPT_VBLANK | 0xE0));
    bus_write(&bus, 0xFFFF, (uint8_t)(INTERRUPT_TIMER | 0xE0));

    assert(bus_read(&bus, 0xFF0F) == INTERRUPT_VBLANK);
    assert(bus_read(&bus, 0xFFFF) == INTERRUPT_TIMER);

    cleanup_cpu(&cartridge);
}

static void test_halt_wakeup_without_service(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    InterruptRegisters interrupts;
    setup_cpu(&cpu, &bus, &memory, &cartridge, &interrupts);

    cartridge.rom[0x0100] = 0x76;
    assert(cpu_step(&cpu) == 4);
    assert(cpu.halted);

    assert(cpu_step(&cpu) == 4);
    assert(cpu.halted);
    assert(cpu.step_status == CPU_STEP_HALTED);

    bus_write(&bus, INTERRUPT_ENABLE_ADDRESS, INTERRUPT_VBLANK);
    assert(cpu_step(&cpu) == 4);
    assert(cpu.halted);
    assert(cpu.step_status == CPU_STEP_HALTED);

    bus_write(&bus, 0xFF0F, INTERRUPT_VBLANK);
    bus_write(&bus, 0xFFFF, 0);
    assert(cpu_step(&cpu) == 4);
    assert(cpu.halted);
    assert(cpu.step_status == CPU_STEP_HALTED);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0x76;
    assert(cpu_step(&cpu) == 4);
    assert(cpu.halted);

    bus_write(&bus, 0xFF0F, INTERRUPT_VBLANK);
    bus_write(&bus, 0xFFFF, INTERRUPT_VBLANK);
    cpu.ime = false;
    assert(cpu_step(&cpu) == 4);
    assert(!cpu.halted);
    assert(cpu.step_status == CPU_STEP_WOKE_FROM_HALT);
    assert(cpu.ime == false);
    assert(bus_read(&bus, 0xFF0F) == INTERRUPT_VBLANK);
    assert(cpu.registers.pc == 0x0101);

    cleanup_cpu(&cartridge);
}

static void test_interrupt_service(
    uint8_t interrupt_mask,
    uint16_t vector
)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    InterruptRegisters interrupts;
    setup_cpu(&cpu, &bus, &memory, &cartridge, &interrupts);

    const uint16_t return_pc = 0x2345;
    cpu.registers.pc = return_pc;
    cpu.registers.sp = 0xC100;
    cpu.ime = true;

    bus_write(
        &bus,
        INTERRUPT_FLAG_ADDRESS,
        (uint8_t)(interrupt_mask | INTERRUPT_TIMER)
    );
    bus_write(&bus, INTERRUPT_ENABLE_ADDRESS, interrupt_mask);

    assert(cpu_step(&cpu) == 20);
    assert(cpu.step_status == CPU_STEP_INTERRUPT_SERVICED);
    assert(!cpu.halted);
    assert(!cpu.ime);
    assert(cpu.registers.pc == vector);
    assert(cpu.registers.sp == 0xC0FE);
    assert(bus_read(&bus, 0xC0FE) == 0x45);
    assert(bus_read(&bus, 0xC0FF) == 0x23);
    assert(
        bus_read(&bus, INTERRUPT_FLAG_ADDRESS) ==
        (uint8_t)(INTERRUPT_TIMER & (uint8_t)~interrupt_mask)
    );

    cleanup_cpu(&cartridge);
}

static void test_interrupt_priority(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    InterruptRegisters interrupts;
    setup_cpu(&cpu, &bus, &memory, &cartridge, &interrupts);

    cpu.registers.pc = 0x3456;
    cpu.registers.sp = 0xC100;
    cpu.ime = true;

    bus_write(
        &bus,
        INTERRUPT_FLAG_ADDRESS,
        (uint8_t)(INTERRUPT_VBLANK | INTERRUPT_TIMER)
    );
    bus_write(
        &bus,
        INTERRUPT_ENABLE_ADDRESS,
        (uint8_t)(INTERRUPT_VBLANK | INTERRUPT_TIMER)
    );

    assert(cpu_step(&cpu) == 20);
    assert(cpu.registers.pc == 0x0040);
    assert(
        bus_read(&bus, INTERRUPT_FLAG_ADDRESS) == INTERRUPT_TIMER
    );

    cleanup_cpu(&cartridge);
}

static void test_halted_interrupt_service(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    InterruptRegisters interrupts;
    setup_cpu(&cpu, &bus, &memory, &cartridge, &interrupts);

    cartridge.rom[0x0100] = 0x76;
    cpu.registers.sp = 0xC100;
    assert(cpu_step(&cpu) == 4);
    assert(cpu.halted);

    bus_write(&bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_VBLANK);
    bus_write(&bus, INTERRUPT_ENABLE_ADDRESS, INTERRUPT_VBLANK);
    cpu.ime = true;

    assert(cpu_step(&cpu) == 20);
    assert(cpu.step_status == CPU_STEP_INTERRUPT_SERVICED);
    assert(!cpu.halted);
    assert(cpu.registers.pc == 0x0040);
    assert(cpu.registers.sp == 0xC0FE);
    assert(bus_read(&bus, 0xC0FE) == 0x01);
    assert(bus_read(&bus, 0xC0FF) == 0x01);

    cleanup_cpu(&cartridge);
}

static void test_normal_execution_without_pending(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    InterruptRegisters interrupts;
    setup_cpu(&cpu, &bus, &memory, &cartridge, &interrupts);

    cartridge.rom[0x0100] = 0x00;
    cpu.ime = true;

    assert(cpu_step(&cpu) == 4);
    assert(cpu.step_status == CPU_STEP_EXECUTED);
    assert(cpu.registers.pc == 0x0101);

    cleanup_cpu(&cartridge);
}

static void test_di_ei_reti(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge;
    InterruptRegisters interrupts;
    setup_cpu(&cpu, &bus, &memory, &cartridge, &interrupts);

    cartridge.rom[0x0100] = 0xF3;
    cpu.ime = true;
    assert(cpu_step(&cpu) == 4);
    assert(cpu.step_status == CPU_STEP_EXECUTED);
    assert(!cpu.ime);
    assert(cpu.registers.pc == 0x0101);

    cpu_init(&cpu, &bus);
    cartridge.rom[0x0100] = 0xFB;
    cartridge.rom[0x0101] = 0x00;
    assert(cpu_step(&cpu) == 4);
    assert(!cpu.ime);
    assert(cpu.ime_enable_delay == 1);
    assert(cpu.registers.pc == 0x0101);

    assert(cpu_step(&cpu) == 4);
    assert(cpu.ime);
    assert(cpu.ime_enable_delay == 0);
    assert(cpu.registers.pc == 0x0102);

    cpu_init(&cpu, &bus);
    cpu.registers.sp = 0xC100;
    cartridge.rom[0x0100] = 0xFB;
    cartridge.rom[0x0101] = 0x00;
    bus_write(&bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_VBLANK);
    bus_write(&bus, INTERRUPT_ENABLE_ADDRESS, INTERRUPT_VBLANK);

    assert(cpu_step(&cpu) == 4);
    assert(!cpu.ime);
    assert(cpu.registers.pc == 0x0101);
    assert(bus_read(&bus, INTERRUPT_FLAG_ADDRESS) == INTERRUPT_VBLANK);

    assert(cpu_step(&cpu) == 4);
    assert(cpu.ime);
    assert(cpu.registers.pc == 0x0102);

    assert(cpu_step(&cpu) == 20);
    assert(cpu.step_status == CPU_STEP_INTERRUPT_SERVICED);
    assert(cpu.registers.pc == 0x0040);
    assert(bus_read(&bus, 0xC0FE) == 0x02);
    assert(bus_read(&bus, 0xC0FF) == 0x01);

    cpu_init(&cpu, &bus);
    cpu.registers.sp = 0xFFFC;
    memory_write(&memory, 0xFFFC, 0x78);
    memory_write(&memory, 0xFFFD, 0x56);
    cartridge.rom[0x0100] = 0xD9;

    assert(cpu_step(&cpu) == 16);
    assert(cpu.step_status == CPU_STEP_EXECUTED);
    assert(cpu.registers.pc == 0x5678);
    assert(cpu.registers.sp == 0xFFFE);
    assert(cpu.ime);
    assert(cpu.ime_enable_delay == 0);

    cpu_init(&cpu, &bus);
    cpu.registers.sp = 0xC100;
    cpu.ime = true;
    bus_write(&bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_VBLANK);
    bus_write(&bus, INTERRUPT_ENABLE_ADDRESS, INTERRUPT_VBLANK);
    cartridge.rom[0x0040] = 0xD9;
    cartridge.rom[0x0100] = 0x00;

    assert(cpu_step(&cpu) == 20);
    assert(cpu.registers.pc == 0x0040);
    assert(!cpu.ime);

    assert(cpu_step(&cpu) == 16);
    assert(cpu.registers.pc == 0x0100);
    assert(cpu.registers.sp == 0xC100);
    assert(cpu.ime);

    assert(cpu_step(&cpu) == 4);
    assert(cpu.step_status == CPU_STEP_EXECUTED);
    assert(cpu.registers.pc == 0x0101);

    cleanup_cpu(&cartridge);
}

static void test_pending_interrupts_and_halt(void)
{
    test_halt_wakeup_without_service();

    test_interrupt_service(INTERRUPT_VBLANK, 0x0040);
    test_interrupt_service(INTERRUPT_LCD_STAT, 0x0048);
    test_interrupt_service(INTERRUPT_TIMER, 0x0050);
    test_interrupt_service(INTERRUPT_SERIAL, 0x0058);
    test_interrupt_service(INTERRUPT_JOYPAD, 0x0060);

    test_interrupt_priority();
    test_halted_interrupt_service();
    test_normal_execution_without_pending();
    test_di_ei_reti();
}

int main(void)
{
    test_interrupt_registers();
    test_pending_interrupts_and_halt();

    printf("Interrupt tests passed!\n");
    return 0;
}
