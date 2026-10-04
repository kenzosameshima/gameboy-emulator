#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stdint.h>

#include <cycles.h>
#include <interrupts.h>

enum {
    SERIAL_SB_ADDRESS = 0xFF01,
    SERIAL_SC_ADDRESS = 0xFF02,
    SERIAL_SC_START = 0x80,
    SERIAL_SC_INTERNAL_CLOCK = 0x01,
    SERIAL_SC_WRITE_MASK = 0x81,
    SERIAL_SC_UNUSED_BITS = 0x7E,
    /* 8 bits at the internal 8192 Hz clock. */
    SERIAL_TRANSFER_CYCLES = 4096,
    /* One bit is shifted on each falling edge of system counter bit 8. */
    SERIAL_BIT_CYCLES = 512,
    SERIAL_BITS = 8
};

/* Called with the byte shifted out when a transfer starts. */
typedef void (*SerialOutputFn)(void *context, uint8_t byte);

typedef struct Serial {
    uint8_t sb;
    uint8_t sc;

    bool transferring;
    uint8_t bits_remaining;

    InterruptRegisters *interrupts;

    SerialOutputFn output;
    void *output_context;
} Serial;

/*
 * There is no link partner: a transfer started with the internal clock
 * (SC = 0x81) reports its byte to the output callback, then completes
 * with SB = 0xFF, SC bit 7 cleared and a serial interrupt request. Writing
 * SC = 0x81 again restarts the transfer. With the external clock (SC = 0x80)
 * the transfer never completes.
 *
 * The serial clock is the system counter (the timer divider) divided down:
 * one bit is shifted on each falling edge of counter bit 8, every 512
 * T-cycles at a multiple of 512, and the transfer completes on the eighth.
 * It does not start at the SC write but at the next edge, so it takes
 * between 7 * 512 and 8 * 512 cycles from the write, depending on where the
 * counter is. serial_step() is given the divider after the step.
 */

/* Clears state and the output callback. */
void serial_init(Serial *serial, InterruptRegisters *interrupts);

/* Clears state and keeps the output callback. */
void serial_reset(Serial *serial);

void serial_set_output(Serial *serial, SerialOutputFn output, void *context);

uint8_t serial_read(const Serial *serial, uint16_t address);
void serial_write(Serial *serial, uint16_t address, uint8_t value);

void serial_step(Serial *serial, CpuCycles cycles, uint16_t divider);

#endif
