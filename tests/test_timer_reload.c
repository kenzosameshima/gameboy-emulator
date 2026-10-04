/*
 * What the timer does in the M-cycles after TIMA overflows, through the
 * timer.h interface. Counting from the M-cycle whose end sees the overflow:
 *
 *   A (the next M-cycle)   TIMA reads 0. Writing TIMA cancels the reload and
 *                          the interrupt.
 *   B (the one after)      TIMA has been reloaded from TMA and the interrupt
 *                          is requested. Writes to TIMA are ignored, and a
 *                          write to TMA is loaded into TIMA as well.
 *   C onwards              ordinary behaviour.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <interrupts.h>
#include <timer.h>

enum {
    TAC_ENABLED_16_CYCLES = 0x05   /* bit 3 of the divider, a 16-cycle period */
};

static Timer timer;
static InterruptRegisters interrupts;

/*
 * TMA = 0xAB and TIMA = 0xFF, counting every 16 T-cycles, stepped in
 * M-cycles of 4 T-cycles. The divider bit falls at the end of the fourth.
 */
static void setup(void)
{
    interrupts_init(&interrupts);
    timer_init(&timer, &interrupts);
    timer_write(&timer, TIMER_TMA_ADDRESS, 0xAB);
    timer_write(&timer, TIMER_TIMA_ADDRESS, 0xFF);
    timer_write(&timer, TIMER_TAC_ADDRESS, TAC_ENABLED_16_CYCLES);
}

static void m_cycles(unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        timer_step(&timer, 4);
    }
}

static uint8_t tima(void)
{
    return timer_read(&timer, TIMER_TIMA_ADDRESS);
}

static int irq(void)
{
    return (interrupts.interrupt_flag & INTERRUPT_TIMER) != 0;
}

static void test_overflow_timeline(void)
{
    setup();

    m_cycles(3);
    assert(tima() == 0xFF);

    /* The overflow: TIMA reads 0 during cycle A, with no interrupt yet. */
    m_cycles(1);
    assert(tima() == 0x00);
    assert(!irq());

    /* Cycle B: reloaded, and the interrupt is requested. */
    m_cycles(1);
    assert(tima() == 0xAB);
    assert(irq());
}

static void test_write_to_tima_in_cycle_a_cancels_the_reload(void)
{
    setup();
    m_cycles(4);
    assert(tima() == 0x00);

    timer_write(&timer, TIMER_TIMA_ADDRESS, 0x42);
    assert(tima() == 0x42);

    m_cycles(1);
    assert(tima() == 0x42);
    assert(!irq());
}

static void test_cycle_b_ignores_tima_and_loads_tma(void)
{
    /* A write to TIMA in cycle B is ignored. */
    setup();
    m_cycles(5);
    assert(tima() == 0xAB);
    timer_write(&timer, TIMER_TIMA_ADDRESS, 0x11);
    assert(tima() == 0xAB);
    m_cycles(1);
    assert(tima() == 0xAB);

    /* A write to TMA in cycle B reaches TIMA too. */
    setup();
    m_cycles(5);
    timer_write(&timer, TIMER_TMA_ADDRESS, 0xCD);
    assert(tima() == 0xCD);
    assert(timer_read(&timer, TIMER_TMA_ADDRESS) == 0xCD);
    m_cycles(1);
    assert(tima() == 0xCD);

    /* A write to TMA in cycle A is picked up by the reload. */
    setup();
    m_cycles(4);
    timer_write(&timer, TIMER_TMA_ADDRESS, 0xEF);
    m_cycles(1);
    assert(tima() == 0xEF);
}

static void test_cycle_c_is_ordinary_again(void)
{
    setup();
    m_cycles(6);

    timer_write(&timer, TIMER_TIMA_ADDRESS, 0x22);
    assert(tima() == 0x22);

    setup();
    m_cycles(6);
    timer_write(&timer, TIMER_TMA_ADDRESS, 0x77);
    assert(tima() == 0xAB);
    assert(timer_read(&timer, TIMER_TMA_ADDRESS) == 0x77);
}

int main(void)
{
    test_overflow_timeline();
    test_write_to_tima_in_cycle_a_cancels_the_reload();
    test_cycle_b_ignores_tima_and_loads_tma();
    test_cycle_c_is_ordinary_again();

    printf("Timer reload cycle tests passed!\n");

    return 0;
}
