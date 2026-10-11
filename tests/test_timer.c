#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cpu.h>
#include <timer.h>

#include "test_util.h"

static void advance_timer(Timer *timer, uint16_t cycles)
{
    for (uint16_t cycle = 0; cycle < cycles; cycle++) {
        timer_step(timer, 1);
    }
}

static void test_registers(void)
{
    TestMachine machine;
    test_machine_init(&machine);

    assert(bus_read(&machine.bus, TIMER_DIV_ADDRESS) == 0);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);
    assert(bus_read(&machine.bus, TIMER_TMA_ADDRESS) == 0);
    assert(bus_read(&machine.bus, TIMER_TAC_ADDRESS) == 0xF8);

    advance_timer(&machine.timer, 256);
    assert(bus_read(&machine.bus, TIMER_DIV_ADDRESS) == 1);

    bus_write(&machine.bus, TIMER_DIV_ADDRESS, 0xFF);
    assert(bus_read(&machine.bus, TIMER_DIV_ADDRESS) == 0);

    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0x12);
    bus_write(&machine.bus, TIMER_TMA_ADDRESS, 0x34);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0x12);
    assert(bus_read(&machine.bus, TIMER_TMA_ADDRESS) == 0x34);

    bus_write(&machine.bus, TIMER_TAC_ADDRESS, 0xFF);
    assert(
        bus_read(&machine.bus, TIMER_TAC_ADDRESS) ==
        (TIMER_TAC_VALID_MASK | 0xF8)
    );

    test_machine_destroy(&machine);
}

static void test_disabled_timer(void)
{
    TestMachine machine;
    test_machine_init(&machine);

    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0);
    bus_write(&machine.bus, TIMER_TAC_ADDRESS, 0);
    advance_timer(&machine.timer, 1024);

    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);
    assert(machine.interrupts.interrupt_flag == 0);

    test_machine_destroy(&machine);
}

static void test_frequencies(void)
{
    const uint16_t periods[] = {1024, 16, 64, 256};

    for (uint8_t mode = 0; mode < 4; mode++) {
        TestMachine machine;
        test_machine_init(&machine);

        bus_write(
            &machine.bus,
            TIMER_TAC_ADDRESS,
            (uint8_t)(TIMER_TAC_ENABLE | mode)
        );
        advance_timer(&machine.timer, (uint16_t)(periods[mode] - 1));
        assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);
        timer_step(&machine.timer, 1);
        assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 1);

        test_machine_destroy(&machine);
    }
}

static void test_overflow_and_interrupt_request(void)
{
    TestMachine machine;
    test_machine_init(&machine);

    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0xFF);
    bus_write(&machine.bus, TIMER_TMA_ADDRESS, 0x42);
    bus_write(
        &machine.bus,
        TIMER_TAC_ADDRESS,
        (uint8_t)(TIMER_TAC_ENABLE | 1)
    );

    advance_timer(&machine.timer, 16);

    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);
    assert(machine.interrupts.interrupt_flag == 0);

    advance_timer(&machine.timer, 3);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);
    assert(machine.interrupts.interrupt_flag == 0);

    timer_step(&machine.timer, 1);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0x42);
    assert(
        (machine.interrupts.interrupt_flag & INTERRUPT_TIMER) != 0
    );

    test_machine_destroy(&machine);
}

static void test_reload_window_writes_and_repeated_overflow(void)
{
    TestMachine machine;
    test_machine_init(&machine);

    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0xFF);
    bus_write(&machine.bus, TIMER_TMA_ADDRESS, 0x12);
    bus_write(&machine.bus, TIMER_TAC_ADDRESS, (uint8_t)(TIMER_TAC_ENABLE | 1));
    advance_timer(&machine.timer, 16);

    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0x55);
    advance_timer(&machine.timer, 4);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0x55);
    assert(machine.interrupts.interrupt_flag == 0);

    timer_init(&machine.timer, &machine.interrupts);
    machine.interrupts.interrupt_flag = 0;
    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0xFF);
    bus_write(&machine.bus, TIMER_TMA_ADDRESS, 0x12);
    bus_write(&machine.bus, TIMER_TAC_ADDRESS, (uint8_t)(TIMER_TAC_ENABLE | 1));
    advance_timer(&machine.timer, 16);
    advance_timer(&machine.timer, 3);
    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0x66);
    bus_write(&machine.bus, TIMER_TMA_ADDRESS, 0x77);
    timer_step(&machine.timer, 1);

    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0x77);
    assert((machine.interrupts.interrupt_flag & INTERRUPT_TIMER) != 0);

    timer_init(&machine.timer, &machine.interrupts);
    machine.interrupts.interrupt_flag = 0;
    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0xFF);
    bus_write(&machine.bus, TIMER_TMA_ADDRESS, 0xFF);
    bus_write(&machine.bus, TIMER_TAC_ADDRESS, (uint8_t)(TIMER_TAC_ENABLE | 1));
    advance_timer(&machine.timer, 16);
    advance_timer(&machine.timer, 4);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0xFF);
    assert((machine.interrupts.interrupt_flag & INTERRUPT_TIMER) != 0);

    machine.interrupts.interrupt_flag = 0;
    advance_timer(&machine.timer, 12);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);
    advance_timer(&machine.timer, 4);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0xFF);
    assert((machine.interrupts.interrupt_flag & INTERRUPT_TIMER) != 0);

    test_machine_destroy(&machine);
}

static void test_div_and_tac_edges(void)
{
    TestMachine machine;
    test_machine_init(&machine);

    bus_write(
        &machine.bus,
        TIMER_TAC_ADDRESS,
        (uint8_t)(TIMER_TAC_ENABLE | 1)
    );
    timer_step(&machine.timer, 8);
    bus_write(&machine.bus, TIMER_DIV_ADDRESS, 0);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 1);

    bus_write(&machine.bus, TIMER_TIMA_ADDRESS, 0);
    timer_step(&machine.timer, 8);
    bus_write(&machine.bus, TIMER_TAC_ADDRESS, 0);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 1);

    test_machine_destroy(&machine);
}

static void test_timer_during_halt_and_pending_if(void)
{
    TestMachine machine;
    test_machine_init(&machine);
    /* No tick handler: the test steps the Timer itself. */
    cpu_init(&machine.cpu, &machine.bus, machine.bus.interrupts);

    machine.cartridge.rom[0x0100] = 0x76;
    bus_write(
        &machine.bus,
        TIMER_TAC_ADDRESS,
        (uint8_t)(TIMER_TAC_ENABLE | 1)
    );
    bus_write(&machine.bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_TIMER);

    assert(cpu_step(&machine.cpu) == 4);
    timer_step(&machine.timer, 4);
    assert(machine.cpu.halted);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);

    assert(cpu_step(&machine.cpu) == 4);
    timer_step(&machine.timer, 4);
    assert(machine.cpu.halted);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);

    assert(cpu_step(&machine.cpu) == 4);
    timer_step(&machine.timer, 4);
    assert(machine.cpu.halted);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 0);

    assert(cpu_step(&machine.cpu) == 4);
    timer_step(&machine.timer, 4);
    assert(machine.cpu.halted);
    assert(bus_read(&machine.bus, TIMER_TIMA_ADDRESS) == 1);
    assert(
        (machine.interrupts.interrupt_flag & INTERRUPT_TIMER) != 0
    );

    test_machine_destroy(&machine);
}

int main(void)
{
    test_registers();
    test_disabled_timer();
    test_frequencies();
    test_overflow_and_interrupt_request();
    test_reload_window_writes_and_repeated_overflow();
    test_div_and_tac_edges();
    test_timer_during_halt_and_pending_if();

    printf("Timer tests passed!\n");
    return 0;
}
