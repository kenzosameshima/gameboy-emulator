#include <stdint.h>

#include <bus.h>
#include <cartridge.h>
#include <memory.h>
#include <timer.h>

void bus_init(
    Bus *bus,
    Cartridge *cartridge,
    Memory *memory,
    InterruptRegisters *interrupts
)
{
    bus->cartridge = cartridge;
    bus->memory = memory;
    bus->interrupts = interrupts;
    bus->timer = NULL;
}

void bus_attach_timer(Bus *bus, Timer *timer)
{
    bus->timer = timer;
}

uint8_t bus_read(Bus *bus, uint16_t address)
{
    /*
     * Cartridge ROM
     * 0000 - 7FFF
     */
    if (address <= 0x7FFF) {
        return cartridge_read(bus->cartridge, address);
    }

    if (address >= TIMER_DIV_ADDRESS &&
        address <= TIMER_TAC_ADDRESS) {
        if (bus->timer == NULL) {
            return 0xFF;
        }

        return timer_read(bus->timer, address);
    }

    if (address == INTERRUPT_FLAG_ADDRESS) {
        return bus->interrupts->interrupt_flag;
    }

    if (address == INTERRUPT_ENABLE_ADDRESS) {
        return bus->interrupts->interrupt_enable;
    }

    /*
     * Work RAM / High RAM
     */
    if ((address >= 0xC000 && address <= 0xDFFF) ||
        (address >= 0xFF80 && address <= 0xFFFE)) {
        return memory_read(bus->memory, address);
    }

    /*
     * Unimplemented memory region.
     */
    return 0xFF;
}

void bus_write(Bus *bus, uint16_t address, uint8_t value)
{
    /*
     * Cartridge ROM is read-only for now.
     */
    if (address <= 0x7FFF) {
        return;
    }

    if (address >= TIMER_DIV_ADDRESS &&
        address <= TIMER_TAC_ADDRESS) {
        if (bus->timer == NULL) {
            return;
        }

        timer_write(bus->timer, address, value);
        return;
    }

    if (address == INTERRUPT_FLAG_ADDRESS) {
        bus->interrupts->interrupt_flag =
            (uint8_t)(value & INTERRUPT_VALID_MASK);
        return;
    }

    if (address == INTERRUPT_ENABLE_ADDRESS) {
        bus->interrupts->interrupt_enable =
            (uint8_t)(value & INTERRUPT_VALID_MASK);
        return;
    }

    /*
     * Work RAM / High RAM
     */
    if ((address >= 0xC000 && address <= 0xDFFF) ||
        (address >= 0xFF80 && address <= 0xFFFE)) {
        memory_write(bus->memory, address, value);
        return;
    }
}
