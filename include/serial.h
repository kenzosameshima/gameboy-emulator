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
    SERIAL_TRANSFER_CYCLES = 4096
};

/* Called with the byte shifted out when a transfer starts. */
typedef void (*SerialOutputFn)(void *context, uint8_t byte);

typedef struct Serial {
    uint8_t sb;
    uint8_t sc;

    bool transferring;
    uint16_t cycles_remaining;

    InterruptRegisters *interrupts;

    SerialOutputFn output;
    void *output_context;
} Serial;

/*
 * There is no link partner: a transfer started with the internal clock
 * (SC = 0x81) reports its byte to the output callback, then completes
 * after SERIAL_TRANSFER_CYCLES with SB = 0xFF, SC bit 7 cleared and a
 * serial interrupt request. Writing SC = 0x81 again restarts the transfer.
 * With the external clock (SC = 0x80) the transfer never completes.
 */

/* Clears state and the output callback. */
void serial_init(Serial *serial, InterruptRegisters *interrupts);

/* Clears state and keeps the output callback. */
void serial_reset(Serial *serial);

void serial_set_output(Serial *serial, SerialOutputFn output, void *context);

uint8_t serial_read(const Serial *serial, uint16_t address);
void serial_write(Serial *serial, uint16_t address, uint8_t value);

void serial_step(Serial *serial, CpuCycles cycles);

#endif
