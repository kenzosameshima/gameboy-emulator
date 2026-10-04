#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <interrupts.h>
#include <serial.h>

typedef struct Capture {
    uint8_t bytes[16];
    unsigned count;
} Capture;

static void capture_byte(void *context, uint8_t byte)
{
    Capture *capture = context;

    assert(capture->count < sizeof(capture->bytes));
    capture->bytes[capture->count++] = byte;
}

/* The system counter the serial clock is divided from; the Emulator passes
 * the timer's divider after each M-cycle. These tests drive it themselves. */
static uint16_t divider;

static void step_one(Serial *serial)
{
    divider = (uint16_t)(divider + 4);
    serial_step(serial, 4, divider);
}

static void step_cycles(Serial *serial, unsigned cycles)
{
    for (unsigned done = 0; done < cycles; done += 4) {
        step_one(serial);
    }
}

static void test_registers(void)
{
    InterruptRegisters interrupts;
    Serial serial;

    interrupts_init(&interrupts);
    serial_init(&serial, &interrupts);
    divider = 0;

    assert(serial_read(&serial, SERIAL_SB_ADDRESS) == 0x00);
    /* Only bits 7 and 0 of SC are implemented; the rest read as 1. */
    assert(serial_read(&serial, SERIAL_SC_ADDRESS) == 0x7E);

    serial_write(&serial, SERIAL_SB_ADDRESS, 0xA5);
    assert(serial_read(&serial, SERIAL_SB_ADDRESS) == 0xA5);

    serial_write(&serial, SERIAL_SC_ADDRESS, 0x00);
    assert(serial_read(&serial, SERIAL_SC_ADDRESS) == 0x7E);

    assert(serial_read(&serial, 0xFF03) == 0xFF);
}

static void test_transfer_with_internal_clock(void)
{
    InterruptRegisters interrupts;
    Serial serial;
    Capture capture = {0};

    interrupts_init(&interrupts);
    serial_init(&serial, &interrupts);
    divider = 0;
    serial_set_output(&serial, capture_byte, &capture);

    serial_write(&serial, SERIAL_SB_ADDRESS, 'A');
    assert(capture.count == 0);

    serial_write(&serial, SERIAL_SC_ADDRESS, 0x81);

    /* The byte is reported as soon as the transfer starts. */
    assert(capture.count == 1);
    assert(capture.bytes[0] == 'A');
    assert(serial_read(&serial, SERIAL_SC_ADDRESS) == 0xFF);

    step_cycles(&serial, SERIAL_TRANSFER_CYCLES - 4);
    assert(serial_read(&serial, SERIAL_SC_ADDRESS) == 0xFF);
    assert(interrupts.interrupt_flag == 0);

    step_one(&serial);

    /* No partner: the line reads all ones, the transfer flag clears. */
    assert(serial_read(&serial, SERIAL_SB_ADDRESS) == 0xFF);
    assert(serial_read(&serial, SERIAL_SC_ADDRESS) == 0x7F);
    assert(interrupts.interrupt_flag == INTERRUPT_SERIAL);

    /* Nothing more happens without a new transfer. */
    interrupts.interrupt_flag = 0;
    step_cycles(&serial, 2 * SERIAL_TRANSFER_CYCLES);
    assert(interrupts.interrupt_flag == 0);
    assert(capture.count == 1);
}

/*
 * The serial clock is the system counter divided down: a transfer shifts one
 * bit on each falling edge of counter bit 8, which is every 512 T-cycles at
 * multiples of 512, and completes on the eighth. A transfer started between
 * edges therefore finishes sooner than 4096 cycles after the write, and one
 * started just after an edge almost exactly 4096.
 */
static unsigned cycles_to_complete(uint16_t start_divider)
{
    InterruptRegisters interrupts;
    Serial serial;

    interrupts_init(&interrupts);
    serial_init(&serial, &interrupts);
    divider = start_divider;
    serial_write(&serial, SERIAL_SC_ADDRESS, 0x81);

    unsigned cycles = 0;

    while (interrupts.interrupt_flag == 0) {
        step_one(&serial);
        cycles += 4;
        assert(cycles <= 2 * SERIAL_TRANSFER_CYCLES);
    }

    return cycles;
}

static void test_transfer_aligns_to_the_system_counter(void)
{
    /* On an edge boundary: the first edge is 512 cycles away. */
    assert(cycles_to_complete(0x0000) == 4096);

    /* Halfway to the next edge. */
    assert(cycles_to_complete(0x0100) == 4096 - 256);

    /* Just after an edge, and just before one. */
    assert(cycles_to_complete(0x0004) == 4096 - 4);
    assert(cycles_to_complete(0x01FC) == 4096 - 508);

    /* The same offsets in another 512-cycle window of the counter. */
    assert(cycles_to_complete(0x1100) == 4096 - 256);
    assert(cycles_to_complete(0xFE00) == 4096);

    /* The phase the DMG boot ROM leaves: 0xABCC, 52 cycles before an edge. */
    assert(cycles_to_complete(0xABCC) == 52 + 7 * 512);

    /* The counter wraps. */
    assert(cycles_to_complete(0xFF00) == 4096 - 256);
}

static void test_restart_and_external_clock(void)
{
    InterruptRegisters interrupts;
    Serial serial;
    Capture capture = {0};

    interrupts_init(&interrupts);
    serial_init(&serial, &interrupts);
    divider = 0;
    serial_set_output(&serial, capture_byte, &capture);

    /* Back-to-back writes each report their byte, as a game printing text. */
    serial_write(&serial, SERIAL_SB_ADDRESS, 'H');
    serial_write(&serial, SERIAL_SC_ADDRESS, 0x81);
    step_cycles(&serial, 100);
    serial_write(&serial, SERIAL_SB_ADDRESS, 'i');
    serial_write(&serial, SERIAL_SC_ADDRESS, 0x81);

    assert(capture.count == 2);
    assert(capture.bytes[0] == 'H');
    assert(capture.bytes[1] == 'i');

    /*
     * The restart counts eight edges again from where the counter is: it
     * was written at counter 100, so the eighth edge is the one at 4096.
     */
    step_cycles(&serial, SERIAL_TRANSFER_CYCLES - 100 - 4);
    assert(interrupts.interrupt_flag == 0);
    step_one(&serial);
    assert(interrupts.interrupt_flag == INTERRUPT_SERIAL);

    /* External clock: waits for a partner that never comes. */
    interrupts.interrupt_flag = 0;
    serial_write(&serial, SERIAL_SB_ADDRESS, 'X');
    serial_write(&serial, SERIAL_SC_ADDRESS, 0x80);
    step_cycles(&serial, 3 * SERIAL_TRANSFER_CYCLES);

    assert(capture.count == 2);
    assert(interrupts.interrupt_flag == 0);
    assert(serial_read(&serial, SERIAL_SC_ADDRESS) == 0xFE);
}

static void test_reset_keeps_output(void)
{
    InterruptRegisters interrupts;
    Serial serial;
    Capture capture = {0};

    interrupts_init(&interrupts);
    serial_init(&serial, &interrupts);
    divider = 0;
    serial_set_output(&serial, capture_byte, &capture);

    serial_write(&serial, SERIAL_SB_ADDRESS, 'Z');
    serial_write(&serial, SERIAL_SC_ADDRESS, 0x81);
    serial_reset(&serial);

    assert(serial_read(&serial, SERIAL_SB_ADDRESS) == 0x00);
    assert(serial_read(&serial, SERIAL_SC_ADDRESS) == 0x7E);

    /* The interrupted transfer is gone... */
    step_cycles(&serial, 2 * SERIAL_TRANSFER_CYCLES);
    assert(interrupts.interrupt_flag == 0);

    /* ...but the callback survives a reset. */
    serial_write(&serial, SERIAL_SB_ADDRESS, 'Y');
    serial_write(&serial, SERIAL_SC_ADDRESS, 0x81);
    assert(capture.count == 2);
    assert(capture.bytes[1] == 'Y');
}

int main(void)
{
    test_registers();
    test_transfer_with_internal_clock();
    test_transfer_aligns_to_the_system_counter();
    test_restart_and_external_clock();
    test_reset_keeps_output();

    printf("Serial tests passed!\n");

    return 0;
}
