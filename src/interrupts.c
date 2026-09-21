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

uint8_t interrupts_pending(const InterruptRegisters *interrupts)
{
    return (uint8_t)(
        interrupts->interrupt_flag &
        interrupts->interrupt_enable &
        INTERRUPT_VALID_MASK
    );
}
