#include <stdint.h>

#include <interrupts.h>

void interrupts_init(InterruptRegisters *interrupts)
{
    interrupts->interrupt_flag = 0;
    interrupts->interrupt_enable = 0;
}

void interrupts_request(InterruptRegisters *interrupts, uint8_t mask)
{
    interrupts->interrupt_flag |= (uint8_t)(mask & INTERRUPT_VALID_MASK);
}

void interrupts_acknowledge(InterruptRegisters *interrupts, uint8_t mask)
{
    interrupts->interrupt_flag &= (uint8_t)~mask;
}

uint8_t interrupts_pending(const InterruptRegisters *interrupts)
{
    return (uint8_t)(
        interrupts->interrupt_flag &
        interrupts->interrupt_enable &
        INTERRUPT_VALID_MASK
    );
}

uint8_t interrupts_highest_priority(uint8_t pending)
{
    for (uint8_t source = 1; (source & INTERRUPT_VALID_MASK) != 0;
         source = (uint8_t)(source << 1)) {
        if ((pending & source) != 0) {
            return source;
        }
    }

    return 0;
}

uint16_t interrupts_vector(uint8_t source)
{
    uint16_t vector = 0x0040;

    for (uint8_t bit = 1; bit != source && bit != 0;
         bit = (uint8_t)(bit << 1)) {
        vector = (uint16_t)(vector + 8);
    }

    return vector;
}
