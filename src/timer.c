#include <timer.h>

#include <bus.h>

enum {
    TIMER_RELOAD_DELAY_CYCLES = 4
};

static uint8_t timer_selected_bit(const Timer *timer)
{
    switch (timer->tac & TIMER_TAC_FREQUENCY_MASK) {
        case 0:
            return 9;

        case 1:
            return 3;

        case 2:
            return 5;

        default:
            return 7;
    }
}

static bool timer_signal(const Timer *timer)
{
    if ((timer->tac & TIMER_TAC_ENABLE) == 0) {
        return false;
    }

    uint8_t bit = timer_selected_bit(timer);
    uint16_t mask = (uint16_t)(1U << bit);

    return (timer->divider & mask) != 0;
}

static void timer_increment_tima(Timer *timer)
{
    if (timer->reload_pending) {
        return;
    }

    if (timer->tima == 0xFF) {
        timer->tima = 0;
        timer->reload_pending = true;
        timer->reload_delay = TIMER_RELOAD_DELAY_CYCLES;
        return;
    }

    timer->tima++;
}

void timer_init(Timer *timer, InterruptRegisters *interrupts)
{
    timer->divider = 0;
    timer->tima = 0;
    timer->tma = 0;
    timer->tac = 0;
    timer->reload_pending = false;
    timer->reload_delay = 0;
    timer->interrupts = interrupts;
}

static void timer_advance_reload(Timer *timer)
{
    if (!timer->reload_pending) {
        return;
    }

    timer->reload_delay--;

    if (timer->reload_delay == 0) {
        timer->tima = timer->tma;
        timer->interrupts->interrupt_flag |= INTERRUPT_TIMER;
        timer->reload_pending = false;
    }
}

uint8_t timer_read(const Timer *timer, uint16_t address)
{
    switch (address) {
        case TIMER_DIV_ADDRESS:
            return (uint8_t)(timer->divider >> 8);

        case TIMER_TIMA_ADDRESS:
            return timer->tima;

        case TIMER_TMA_ADDRESS:
            return timer->tma;

        case TIMER_TAC_ADDRESS:
            return timer->tac;

        default:
            return 0xFF;
    }
}

void timer_write(Timer *timer, uint16_t address, uint8_t value)
{
    switch (address) {
        case TIMER_DIV_ADDRESS:
            {
                bool old_signal = timer_signal(timer);
                timer->divider = 0;

                if (old_signal && !timer_signal(timer)) {
                    timer_increment_tima(timer);
                }
            }
            break;

        case TIMER_TIMA_ADDRESS:
            if (!timer->reload_pending ||
                timer->reload_delay == TIMER_RELOAD_DELAY_CYCLES) {
                timer->tima = value;

                if (timer->reload_pending) {
                    timer->reload_pending = false;
                    timer->reload_delay = 0;
                }
            }
            break;

        case TIMER_TMA_ADDRESS:
            timer->tma = value;
            break;

        case TIMER_TAC_ADDRESS:
            {
                bool old_signal = timer_signal(timer);
                timer->tac = (uint8_t)(value & TIMER_TAC_VALID_MASK);

                if (old_signal && !timer_signal(timer)) {
                    timer_increment_tima(timer);
                }
            }
            break;

        default:
            break;
    }
}

void timer_step(Timer *timer, CpuCycles cycles)
{
    for (CpuCycles cycle = 0; cycle < cycles; cycle++) {
        timer_advance_reload(timer);

        bool old_signal = timer_signal(timer);
        timer->divider = (uint16_t)(timer->divider + 1);

        if (old_signal && !timer_signal(timer)) {
            timer_increment_tima(timer);
        }
    }
}
