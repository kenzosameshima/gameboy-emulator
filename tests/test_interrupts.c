#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <memory.h>

#include "test_util.h"

/* The CPU runs without a tick handler, so nothing else on the bus moves. */
static void setup_cpu(TestMachine *machine)
{
    test_machine_init(machine);
    cpu_init(&machine->cpu, &machine->bus, &machine->interrupts);
}

static void test_interrupt_registers(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    /* IF reads with its upper three bits set; IE keeps all eight bits. */
    assert(bus_read(&machine.bus, 0xFF0F) == 0xE0);
    assert(bus_read(&machine.bus, 0xFFFF) == 0);

    bus_write(&machine.bus, 0xFF0F, (uint8_t)(INTERRUPT_VBLANK | 0xE0));
    bus_write(&machine.bus, 0xFFFF, (uint8_t)(INTERRUPT_TIMER | 0xE0));

    assert(bus_read(&machine.bus, 0xFF0F) == (INTERRUPT_VBLANK | 0xE0));
    assert(bus_read(&machine.bus, 0xFFFF) == (INTERRUPT_TIMER | 0xE0));

    test_machine_destroy(&machine);
}

static void test_halt_wakeup_without_service(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cartridge.rom[0x0100] = 0x76;
    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.halted);

    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.halted);
    assert(machine.cpu.step_status == CPU_STEP_HALTED);

    bus_write(&machine.bus, INTERRUPT_ENABLE_ADDRESS, INTERRUPT_VBLANK);
    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.halted);
    assert(machine.cpu.step_status == CPU_STEP_HALTED);

    bus_write(&machine.bus, 0xFF0F, INTERRUPT_VBLANK);
    bus_write(&machine.bus, 0xFFFF, 0);
    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.halted);
    assert(machine.cpu.step_status == CPU_STEP_HALTED);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0x76;
    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.halted);

    bus_write(&machine.bus, 0xFF0F, INTERRUPT_VBLANK);
    bus_write(&machine.bus, 0xFFFF, INTERRUPT_VBLANK);
    machine.cpu.ime = false;

    /* HALT ends at once and the next instruction (a NOP) runs in the same step. */
    assert(cpu_step(&machine.cpu) == 4);
    assert(!machine.cpu.halted);
    assert(machine.cpu.step_status == CPU_STEP_EXECUTED);
    assert(machine.cpu.ime == false);
    assert(bus_read(&machine.bus, 0xFF0F) == (INTERRUPT_VBLANK | 0xE0));
    assert(machine.cpu.registers.pc == 0x0102);

    test_machine_destroy(&machine);
}

static void test_interrupt_service(
    uint8_t interrupt_mask,
    uint16_t vector
)
{
    TestMachine machine;
    setup_cpu(&machine);

    const uint16_t return_pc = 0x2345;
    machine.cpu.registers.pc = return_pc;
    machine.cpu.registers.sp = 0xC100;
    machine.cpu.ime = true;

    bus_write(
        &machine.bus,
        INTERRUPT_FLAG_ADDRESS,
        (uint8_t)(interrupt_mask | INTERRUPT_TIMER)
    );
    bus_write(&machine.bus, INTERRUPT_ENABLE_ADDRESS, interrupt_mask);

    assert(cpu_step(&machine.cpu) == 20);
    assert(machine.cpu.step_status == CPU_STEP_INTERRUPT_SERVICED);
    assert(!machine.cpu.halted);
    assert(!machine.cpu.ime);
    assert(machine.cpu.registers.pc == vector);
    assert(machine.cpu.registers.sp == 0xC0FE);
    assert(bus_read(&machine.bus, 0xC0FE) == 0x45);
    assert(bus_read(&machine.bus, 0xC0FF) == 0x23);
    assert(
        bus_read(&machine.bus, INTERRUPT_FLAG_ADDRESS) ==
        (uint8_t)((INTERRUPT_TIMER & (uint8_t)~interrupt_mask) | 0xE0)
    );

    test_machine_destroy(&machine);
}

static void test_interrupt_priority(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cpu.registers.pc = 0x3456;
    machine.cpu.registers.sp = 0xC100;
    machine.cpu.ime = true;

    bus_write(
        &machine.bus,
        INTERRUPT_FLAG_ADDRESS,
        (uint8_t)(INTERRUPT_VBLANK | INTERRUPT_TIMER)
    );
    bus_write(
        &machine.bus,
        INTERRUPT_ENABLE_ADDRESS,
        (uint8_t)(INTERRUPT_VBLANK | INTERRUPT_TIMER)
    );

    assert(cpu_step(&machine.cpu) == 20);
    assert(machine.cpu.registers.pc == 0x0040);
    assert(
        bus_read(&machine.bus, INTERRUPT_FLAG_ADDRESS) ==
        (INTERRUPT_TIMER | 0xE0)
    );

    test_machine_destroy(&machine);
}

static void test_halted_interrupt_service(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cartridge.rom[0x0100] = 0x76;
    machine.cpu.registers.sp = 0xC100;
    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.halted);

    bus_write(&machine.bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_VBLANK);
    bus_write(&machine.bus, INTERRUPT_ENABLE_ADDRESS, INTERRUPT_VBLANK);
    machine.cpu.ime = true;

    assert(cpu_step(&machine.cpu) == 20);
    assert(machine.cpu.step_status == CPU_STEP_INTERRUPT_SERVICED);
    assert(!machine.cpu.halted);
    assert(machine.cpu.registers.pc == 0x0040);
    assert(machine.cpu.registers.sp == 0xC0FE);
    assert(bus_read(&machine.bus, 0xC0FE) == 0x01);
    assert(bus_read(&machine.bus, 0xC0FF) == 0x01);

    test_machine_destroy(&machine);
}

static void test_normal_execution_without_pending(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cartridge.rom[0x0100] = 0x00;
    machine.cpu.ime = true;

    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.step_status == CPU_STEP_EXECUTED);
    assert(machine.cpu.registers.pc == 0x0101);

    test_machine_destroy(&machine);
}

static void test_di_ei_reti(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    machine.cartridge.rom[0x0100] = 0xF3;
    machine.cpu.ime = true;
    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.step_status == CPU_STEP_EXECUTED);
    assert(!machine.cpu.ime);
    assert(machine.cpu.registers.pc == 0x0101);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cartridge.rom[0x0100] = 0xFB;
    machine.cartridge.rom[0x0101] = 0x00;
    assert(cpu_step(&machine.cpu) == 4);
    assert(!machine.cpu.ime);
    assert(machine.cpu.ime_enable_delay == 1);
    assert(machine.cpu.registers.pc == 0x0101);

    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.ime);
    assert(machine.cpu.ime_enable_delay == 0);
    assert(machine.cpu.registers.pc == 0x0102);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.sp = 0xC100;
    machine.cartridge.rom[0x0100] = 0xFB;
    machine.cartridge.rom[0x0101] = 0x00;
    bus_write(&machine.bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_VBLANK);
    bus_write(&machine.bus, INTERRUPT_ENABLE_ADDRESS, INTERRUPT_VBLANK);

    assert(cpu_step(&machine.cpu) == 4);
    assert(!machine.cpu.ime);
    assert(machine.cpu.registers.pc == 0x0101);
    assert(
        bus_read(&machine.bus, INTERRUPT_FLAG_ADDRESS) ==
        (INTERRUPT_VBLANK | 0xE0)
    );

    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.ime);
    assert(machine.cpu.registers.pc == 0x0102);

    assert(cpu_step(&machine.cpu) == 20);
    assert(machine.cpu.step_status == CPU_STEP_INTERRUPT_SERVICED);
    assert(machine.cpu.registers.pc == 0x0040);
    assert(bus_read(&machine.bus, 0xC0FE) == 0x02);
    assert(bus_read(&machine.bus, 0xC0FF) == 0x01);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.sp = 0xFFFC;
    memory_write(&machine.memory, 0xFFFC, 0x78);
    memory_write(&machine.memory, 0xFFFD, 0x56);
    machine.cartridge.rom[0x0100] = 0xD9;

    assert(cpu_step(&machine.cpu) == 16);
    assert(machine.cpu.step_status == CPU_STEP_EXECUTED);
    assert(machine.cpu.registers.pc == 0x5678);
    assert(machine.cpu.registers.sp == 0xFFFE);
    assert(machine.cpu.ime);
    assert(machine.cpu.ime_enable_delay == 0);

    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);
    machine.cpu.registers.sp = 0xC100;
    machine.cpu.ime = true;
    bus_write(&machine.bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_VBLANK);
    bus_write(&machine.bus, INTERRUPT_ENABLE_ADDRESS, INTERRUPT_VBLANK);
    machine.cartridge.rom[0x0040] = 0xD9;
    machine.cartridge.rom[0x0100] = 0x00;

    assert(cpu_step(&machine.cpu) == 20);
    assert(machine.cpu.registers.pc == 0x0040);
    assert(!machine.cpu.ime);

    assert(cpu_step(&machine.cpu) == 16);
    assert(machine.cpu.registers.pc == 0x0100);
    assert(machine.cpu.registers.sp == 0xC100);
    assert(machine.cpu.ime);

    assert(cpu_step(&machine.cpu) == 4);
    assert(machine.cpu.step_status == CPU_STEP_EXECUTED);
    assert(machine.cpu.registers.pc == 0x0101);

    test_machine_destroy(&machine);
}

/*
 * Priority is by bit: VBlank beats LCD STAT beats Timer beats Serial beats
 * Joypad. Vectors are 0x40 + 8 * bit. Bits above the five sources are
 * not machine.interrupts.
 */
static void test_priority_and_vectors(void)
{
    static const struct {
        uint8_t pending;
        uint8_t highest;
    } cases[] = {
        { 0x00, 0x00 },
        { 0xE0, 0x00 },
        { INTERRUPT_VBLANK, INTERRUPT_VBLANK },
        { INTERRUPT_LCD_STAT, INTERRUPT_LCD_STAT },
        { INTERRUPT_TIMER, INTERRUPT_TIMER },
        { INTERRUPT_SERIAL, INTERRUPT_SERIAL },
        { INTERRUPT_JOYPAD, INTERRUPT_JOYPAD },
        { 0x1F, INTERRUPT_VBLANK },
        { 0x1E, INTERRUPT_LCD_STAT },
        { 0x1C, INTERRUPT_TIMER },
        { 0x18, INTERRUPT_SERIAL },
        { 0x14, INTERRUPT_TIMER },
        { 0xE4, INTERRUPT_TIMER },
        { 0xFF, INTERRUPT_VBLANK }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(interrupts_highest_priority(cases[i].pending) ==
               cases[i].highest);
    }

    assert(interrupts_vector(INTERRUPT_VBLANK) == 0x0040);
    assert(interrupts_vector(INTERRUPT_LCD_STAT) == 0x0048);
    assert(interrupts_vector(INTERRUPT_TIMER) == 0x0050);
    assert(interrupts_vector(INTERRUPT_SERIAL) == 0x0058);
    assert(interrupts_vector(INTERRUPT_JOYPAD) == 0x0060);
}

/*
 * A CPU is stalled when it is waiting in HALT and nothing in this machine
 * can ever end the wait. STOP is not, because input can end it.
 */
static void test_cpu_is_stalled(void)
{
    TestMachine machine;
    setup_cpu(&machine);

    assert(!cpu_is_stalled(&machine.cpu));

    /* HALT ends on IF & IE, so with every source masked it never ends. */
    machine.cpu.halted = true;
    assert(cpu_is_stalled(&machine.cpu));

    machine.interrupts.interrupt_enable = INTERRUPT_TIMER;
    assert(!cpu_is_stalled(&machine.cpu));

    /* Bits above the five sources do not count as interrupt sources. */
    machine.interrupts.interrupt_enable = 0xE0;
    assert(cpu_is_stalled(&machine.cpu));

    /* STOP ends on a button press, which can come at any time. */
    machine.cpu.halted = false;
    machine.cpu.stopped = true;
    machine.interrupts.interrupt_enable = 0;
    assert(!cpu_is_stalled(&machine.cpu));

    test_machine_destroy(&machine);
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
    test_priority_and_vectors();
    test_cpu_is_stalled();
    test_pending_interrupts_and_halt();

    printf("Interrupt tests passed!\n");
    return 0;
}
