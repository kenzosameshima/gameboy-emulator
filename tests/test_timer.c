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

/* ---- Falling edges of the system counter ---- */

/* A timer whose counter starts at `divider`, with no edges pending. */
static void edge_timer(
    Timer *timer,
    InterruptRegisters *interrupts,
    uint16_t divider
)
{
    interrupts_init(interrupts);
    timer_init(timer, interrupts);
    timer->divider = divider;
}

static void test_counting_records_falls(void)
{
    Timer timer;
    InterruptRegisters interrupts;

    /* 01FE to 0202: 01FF -> 0200 drops bits 0-8 and 0201 -> 0202 drops bit
     * 0 again. Bits that rise, like bit 9, are not recorded. */
    edge_timer(&timer, &interrupts, 0x01FE);
    timer_step(&timer, 4);
    assert(timer_take_falling_edges(&timer) == 0x01FF);

    /* A step in which no bit falls records nothing. */
    edge_timer(&timer, &interrupts, 0x0000);
    timer_step(&timer, 1);
    assert(timer_take_falling_edges(&timer) == 0);
}

static void test_div_write_records_falls(void)
{
    Timer timer;
    InterruptRegisters interrupts;

    /* Resetting the counter drops every bit that was set. */
    edge_timer(&timer, &interrupts, 0x1234);
    timer_write(&timer, TIMER_DIV_ADDRESS, 0xFF);
    assert(timer_take_falling_edges(&timer) == 0x1234);

    /* Writing DIV while it is already 0 drops nothing. */
    edge_timer(&timer, &interrupts, 0x0000);
    timer_write(&timer, TIMER_DIV_ADDRESS, 0);
    assert(timer_take_falling_edges(&timer) == 0);
}

/* The set records whether a bit fell, not how often. */
static void test_repeated_falls_are_coalesced(void)
{
    Timer timer;
    InterruptRegisters interrupts;

    /* An M-cycle with a DIV write before its four increments: the write
     * drops bit 8, then counting 0 -> 4 drops bit 0 twice and bit 1 once. */
    edge_timer(&timer, &interrupts, 0x0100);
    timer_write(&timer, TIMER_DIV_ADDRESS, 0);
    timer_step(&timer, 4);
    assert(timer_take_falling_edges(&timer) == 0x0103);

    /* Over 1024 cycles bit 8 falls twice and is recorded once. */
    edge_timer(&timer, &interrupts, 0x0000);
    timer_step(&timer, 1024);
    assert(timer_take_falling_edges(&timer) == 0x03FF);
}

static void test_counter_wrap_drops_every_bit(void)
{
    Timer timer;
    InterruptRegisters interrupts;

    edge_timer(&timer, &interrupts, 0xFFFE);
    timer_step(&timer, 4);
    assert(timer.divider == 0x0002);
    assert(timer_take_falling_edges(&timer) == 0xFFFF);
}

/* Each check starts from a set that is not empty. */
static void test_take_and_resets_clear_the_set(void)
{
    Timer timer;
    InterruptRegisters interrupts;

    edge_timer(&timer, &interrupts, 0x8001);
    timer_write(&timer, TIMER_DIV_ADDRESS, 0);
    assert(timer_take_falling_edges(&timer) == 0x8001);
    assert(timer_take_falling_edges(&timer) == 0);

    edge_timer(&timer, &interrupts, 0x8001);
    timer_write(&timer, TIMER_DIV_ADDRESS, 0);
    assert(timer.falling_edges == 0x8001);
    timer_init(&timer, &interrupts);
    assert(timer_take_falling_edges(&timer) == 0);

    edge_timer(&timer, &interrupts, 0x8001);
    timer_write(&timer, TIMER_DIV_ADDRESS, 0);
    assert(timer.falling_edges == 0x8001);
    timer_power_on(&timer);
    assert(timer.divider == 0xABCC);
    assert(timer_take_falling_edges(&timer) == 0);
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
    test_counting_records_falls();
    test_div_write_records_falls();
    test_repeated_falls_are_coalesced();
    test_counter_wrap_drops_every_bit();
    test_take_and_resets_clear_the_set();

    printf("Timer tests passed!\n");
    return 0;
}
