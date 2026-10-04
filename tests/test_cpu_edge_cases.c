/*
 * CPU and interrupt edge cases found by the Mooneye acceptance ROMs, each
 * run through cpu_step() on the shared TestMachine:
 *
 *   - EI while an enable is already pending does not delay it
 *   - HALT with IME off continues at once when an interrupt is pending
 *   - interrupt dispatch picks its vector after the high byte of PC is
 *     pushed, so a push that lands on IE can cancel or redirect it
 *   - IF reads with its upper three bits set, and IE keeps all 8 bits
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "test_util.h"

enum {
    PROGRAM_ADDRESS = 0x0100
};

static void load(TestMachine *m, const uint8_t *program, size_t size)
{
    test_machine_load(m, PROGRAM_ADDRESS, program, size);
    test_machine_reset_cpu(m);
}

static uint16_t stack_word(TestMachine *m, uint16_t address)
{
    return (uint16_t)(bus_read(&m->bus, address) |
                      (bus_read(&m->bus, (uint16_t)(address + 1)) << 8));
}

static void test_ei_sequence(TestMachine *m)
{
    static const uint8_t program[] = { 0xFB, 0xFB, 0x00, 0x00 }; /* EI EI NOP */

    load(m, program, sizeof(program));
    m->interrupts.interrupt_enable = INTERRUPT_SERIAL;
    m->interrupts.interrupt_flag = INTERRUPT_SERIAL;

    assert(cpu_step(&m->cpu) == 4);
    assert(!m->cpu.ime);

    /*
     * The enable takes effect after the instruction that follows the first
     * EI, even though that instruction is another EI: it must not push the
     * enable back.
     */
    assert(cpu_step(&m->cpu) == 4);
    assert(m->cpu.ime);

    assert(cpu_step(&m->cpu) == 20);
    assert(m->cpu.step_status == CPU_STEP_INTERRUPT_SERVICED);
    assert(m->cpu.registers.pc == 0x0058);
    assert(stack_word(m, m->cpu.registers.sp) == 0x0102);

    /* A single EI still waits one instruction. */
    static const uint8_t single[] = { 0xFB, 0x00, 0x00 };

    load(m, single, sizeof(single));
    m->interrupts.interrupt_flag = INTERRUPT_SERIAL;

    assert(cpu_step(&m->cpu) == 4);
    assert(!m->cpu.ime);
    assert(cpu_step(&m->cpu) == 4);
    assert(m->cpu.ime);

    /* EI while IME is already on changes nothing. */
    static const uint8_t again[] = { 0xFB, 0x00, 0xFB, 0x00 };

    load(m, again, sizeof(again));
    m->interrupts.interrupt_flag = 0;
    cpu_step(&m->cpu);
    cpu_step(&m->cpu);
    assert(m->cpu.ime);
    cpu_step(&m->cpu);
    cpu_step(&m->cpu);
    assert(m->cpu.ime);
}

static void test_halt_wakes_without_extra_cycle(TestMachine *m)
{
    static const uint8_t program[] = { 0x76, 0x00, 0x00 }; /* HALT NOP NOP */

    load(m, program, sizeof(program));
    m->interrupts.interrupt_enable = INTERRUPT_TIMER;
    m->interrupts.interrupt_flag = 0;

    assert(cpu_step(&m->cpu) == 4);
    assert(m->cpu.halted);

    /* Nothing pending: one idle M-cycle at a time. */
    assert(cpu_step(&m->cpu) == 4);
    assert(m->cpu.step_status == CPU_STEP_HALTED);
    assert(m->cpu.registers.pc == PROGRAM_ADDRESS + 1);

    /*
     * With IME off and an interrupt pending, HALT ends and the next
     * instruction runs at once, as if NOPs had been used to wait.
     */
    m->interrupts.interrupt_flag = INTERRUPT_TIMER;
    assert(cpu_step(&m->cpu) == 4);
    assert(!m->cpu.halted);
    assert(m->cpu.step_status == CPU_STEP_EXECUTED);
    assert(m->cpu.registers.pc == PROGRAM_ADDRESS + 2);

    /* The interrupt was not serviced: IME is off. */
    assert(m->interrupts.interrupt_flag == INTERRUPT_TIMER);
}

/* Starts a dispatch with the stack where its pushes will hit IE (FFFF). */
static void start_dispatch(TestMachine *m, uint16_t pc, uint16_t sp,
                           uint8_t enable, uint8_t flags)
{
    static const uint8_t nop[] = { 0x00 };

    load(m, nop, sizeof(nop));
    m->cpu.registers.pc = pc;
    m->cpu.registers.sp = sp;
    m->cpu.ime = true;
    m->interrupts.interrupt_enable = enable;
    m->interrupts.interrupt_flag = flags;
}

static void test_dispatch_with_ie_push(TestMachine *m)
{
    /*
     * The high byte of PC (0x02) lands on IE, which clears the timer
     * enable: the dispatch is cancelled, PC becomes 0 and IF is untouched.
     */
    start_dispatch(m, 0x0202, 0x0000, INTERRUPT_TIMER, INTERRUPT_TIMER);
    assert(cpu_step(&m->cpu) == 20);
    assert(m->cpu.registers.pc == 0x0000);
    assert(!m->cpu.ime);
    assert((m->interrupts.interrupt_flag & INTERRUPT_VALID_MASK) ==
           INTERRUPT_TIMER);
    assert(m->interrupts.interrupt_enable == 0x02);

    /*
     * The low byte (0x35) lands on IE after the vector is chosen: too late
     * to cancel, so the serial interrupt is dispatched and acknowledged.
     */
    start_dispatch(m, 0x0035, 0x0001, INTERRUPT_SERIAL, INTERRUPT_SERIAL);
    assert(cpu_step(&m->cpu) == 20);
    assert(m->cpu.registers.pc == 0x0058);
    assert((m->interrupts.interrupt_flag & INTERRUPT_VALID_MASK) == 0);
    assert(m->interrupts.interrupt_enable == 0x35);

    /*
     * Two interrupts pending, and the push removes the higher priority
     * one: the other is dispatched instead.
     */
    start_dispatch(m, 0x0202, 0x0000, INTERRUPT_LCD_STAT | INTERRUPT_VBLANK,
                   INTERRUPT_LCD_STAT | INTERRUPT_VBLANK);
    assert(cpu_step(&m->cpu) == 20);
    assert(m->cpu.registers.pc == 0x0048);
    assert((m->interrupts.interrupt_flag & INTERRUPT_VALID_MASK) ==
           INTERRUPT_VBLANK);

    /* Without any interference the highest priority one is dispatched. */
    start_dispatch(m, 0x0202, 0xFFFE, INTERRUPT_LCD_STAT | INTERRUPT_VBLANK,
                   INTERRUPT_LCD_STAT | INTERRUPT_VBLANK);
    assert(cpu_step(&m->cpu) == 20);
    assert(m->cpu.registers.pc == 0x0040);
    assert((m->interrupts.interrupt_flag & INTERRUPT_VALID_MASK) ==
           INTERRUPT_LCD_STAT);
    assert(stack_word(m, m->cpu.registers.sp) == 0x0202);
}

static void test_if_and_ie_bits(TestMachine *m)
{
    test_machine_reset_cpu(m);
    m->interrupts.interrupt_flag = 0;
    m->interrupts.interrupt_enable = 0;

    /* IF reads with its upper three bits set. */
    assert(bus_read(&m->bus, INTERRUPT_FLAG_ADDRESS) == 0xE0);
    interrupts_request(&m->interrupts, INTERRUPT_SERIAL);
    assert(bus_read(&m->bus, INTERRUPT_FLAG_ADDRESS) == 0xE8);

    bus_write(&m->bus, INTERRUPT_FLAG_ADDRESS, 0xFF);
    assert(m->interrupts.interrupt_flag == INTERRUPT_VALID_MASK);
    assert(bus_read(&m->bus, INTERRUPT_FLAG_ADDRESS) == 0xFF);

    /* IE keeps all 8 bits, but only the low five are interrupt sources. */
    bus_write(&m->bus, INTERRUPT_ENABLE_ADDRESS, 0xE5);
    assert(bus_read(&m->bus, INTERRUPT_ENABLE_ADDRESS) == 0xE5);
    assert(interrupts_pending(&m->interrupts) == 0x05);

    bus_write(&m->bus, INTERRUPT_ENABLE_ADDRESS, 0xE0);
    assert(interrupts_pending(&m->interrupts) == 0x00);
}

int main(void)
{
    TestMachine machine;

    test_machine_init(&machine);

    test_ei_sequence(&machine);
    test_halt_wakes_without_extra_cycle(&machine);
    test_dispatch_with_ie_push(&machine);
    test_if_and_ie_bits(&machine);

    test_machine_destroy(&machine);

    printf("CPU edge case tests passed!\n");

    return 0;
}
