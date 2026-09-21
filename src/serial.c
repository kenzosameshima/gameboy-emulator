#include <stddef.h>

#include <serial.h>

void serial_reset(Serial *serial)
{
    serial->sb = 0;
    serial->sc = 0;
    serial->transferring = false;
    serial->cycles_remaining = 0;
}

void serial_init(Serial *serial, InterruptRegisters *interrupts)
{
    serial_reset(serial);
    serial->interrupts = interrupts;
    serial->output = NULL;
    serial->output_context = NULL;
}

void serial_set_output(Serial *serial, SerialOutputFn output, void *context)
{
    serial->output = output;
    serial->output_context = context;
}

uint8_t serial_read(const Serial *serial, uint16_t address)
{
    switch (address) {
        case SERIAL_SB_ADDRESS:
            return serial->sb;

        case SERIAL_SC_ADDRESS:
            return (uint8_t)(serial->sc | SERIAL_SC_UNUSED_BITS);

        default:
            return 0xFF;
    }
}

void serial_write(Serial *serial, uint16_t address, uint8_t value)
{
    switch (address) {
        case SERIAL_SB_ADDRESS:
            serial->sb = value;
            break;

        case SERIAL_SC_ADDRESS:
            serial->sc = (uint8_t)(value & SERIAL_SC_WRITE_MASK);

            if ((serial->sc & SERIAL_SC_START) == 0) {
                serial->transferring = false;
                break;
            }

            serial->transferring =
                (serial->sc & SERIAL_SC_INTERNAL_CLOCK) != 0;
            serial->cycles_remaining = SERIAL_TRANSFER_CYCLES;

            if (serial->transferring && serial->output != NULL) {
                serial->output(serial->output_context, serial->sb);
            }
            break;

        default:
            break;
    }
}

void serial_step(Serial *serial, CpuCycles cycles)
{
    if (!serial->transferring) {
        return;
    }

    if (cycles < serial->cycles_remaining) {
        serial->cycles_remaining = (uint16_t)(serial->cycles_remaining - cycles);
        return;
    }

    serial->transferring = false;
    serial->cycles_remaining = 0;
    serial->sb = 0xFF;
    serial->sc = (uint8_t)(serial->sc & ~SERIAL_SC_START);
    interrupts_request(serial->interrupts, INTERRUPT_SERIAL);
}
