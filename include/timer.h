#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include <cycles.h>
#include <interrupts.h>

enum {
    TIMER_DIV_ADDRESS = 0xFF04,
    TIMER_TIMA_ADDRESS = 0xFF05,
    TIMER_TMA_ADDRESS = 0xFF06,
    TIMER_TAC_ADDRESS = 0xFF07,
    TIMER_TAC_ENABLE = 0x04,
    TIMER_TAC_FREQUENCY_MASK = 0x03,
    TIMER_TAC_VALID_MASK = 0x07
};

typedef struct Timer {
    uint16_t divider;
    uint8_t tima;
    uint8_t tma;
    uint8_t tac;

    bool reload_pending;
    uint8_t reload_delay;
    /* M-cycle after the reload, when writes behave differently. */
    uint8_t reload_hold;

    InterruptRegisters *interrupts;
} Timer;

/*
 * TIMA is clocked on falling edges of the selected divider bit.
 * Overflow exposes TIMA as 00 for one M-cycle (a write to TIMA then cancels
 * the reload), then reloads TMA and requests the timer interrupt. During the
 * M-cycle after the reload, writes to TIMA are ignored and a write to TMA is
 * also loaded into TIMA.
 */

void timer_init(Timer *timer, InterruptRegisters *interrupts);

uint8_t timer_read(const Timer *timer, uint16_t address);
void timer_write(Timer *timer, uint16_t address, uint8_t value);

void timer_step(Timer *timer, CpuCycles cycles);

#endif
