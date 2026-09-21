#include <stdbool.h>
#include <stdint.h>

#include <bus.h>
#include <cartridge.h>
#include <memory.h>
#include <memory_map.h>
#include <serial.h>
#include <timer.h>

static bool bus_is_timer_address(uint16_t address)
{
    return address >= TIMER_DIV_ADDRESS &&
           address <= TIMER_TAC_ADDRESS;
}

static bool bus_is_serial_address(uint16_t address)
{
    return address == SERIAL_SB_ADDRESS ||
           address == SERIAL_SC_ADDRESS;
}

static bool bus_is_cartridge_ram_address(uint16_t address)
{
    return address >= MEM_CART_RAM_START &&
           address <= MEM_CART_RAM_END;
}

static bool bus_is_wram_address(uint16_t address)
{
    return address >= MEM_WRAM_START && address <= MEM_WRAM_END;
}

static bool bus_is_echo_address(uint16_t address)
{
    return address >= MEM_ECHO_START && address <= MEM_ECHO_END;
}

static bool bus_is_hram_address(uint16_t address)
{
    return address >= MEM_HRAM_START && address <= MEM_HRAM_END;
}

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
    bus->serial = NULL;
}

void bus_attach_timer(Bus *bus, Timer *timer)
{
    bus->timer = timer;
}

void bus_attach_serial(Bus *bus, Serial *serial)
{
    bus->serial = serial;
}

uint8_t bus_read(Bus *bus, uint16_t address)
{
    if (address <= MEM_ROM_END) {
        return cartridge_read(bus->cartridge, address);
    }

    if (bus_is_cartridge_ram_address(address)) {
        return cartridge_read_ram(bus->cartridge, address);
    }

    if (bus_is_wram_address(address) || bus_is_hram_address(address)) {
        return memory_read(bus->memory, address);
    }

    if (bus_is_echo_address(address)) {
        return memory_read(
            bus->memory,
            (uint16_t)(address - MEM_ECHO_OFFSET)
        );
    }

    if (bus_is_serial_address(address)) {
        return bus->serial == NULL ? 0xFF : serial_read(bus->serial, address);
    }

    if (bus_is_timer_address(address)) {
        return bus->timer == NULL ? 0xFF : timer_read(bus->timer, address);
    }

    if (address == INTERRUPT_FLAG_ADDRESS) {
        return bus->interrupts->interrupt_flag;
    }

    if (address == INTERRUPT_ENABLE_ADDRESS) {
        return bus->interrupts->interrupt_enable;
    }

    /*
     * Unimplemented memory region.
     */
    return 0xFF;
}

void bus_write(Bus *bus, uint16_t address, uint8_t value)
{
    if (address <= MEM_ROM_END) {
        cartridge_write(bus->cartridge, address, value);
        return;
    }

    if (bus_is_cartridge_ram_address(address)) {
        cartridge_write_ram(bus->cartridge, address, value);
        return;
    }

    if (bus_is_wram_address(address) || bus_is_hram_address(address)) {
        memory_write(bus->memory, address, value);
        return;
    }

    if (bus_is_echo_address(address)) {
        memory_write(
            bus->memory,
            (uint16_t)(address - MEM_ECHO_OFFSET),
            value
        );
        return;
    }

    if (bus_is_serial_address(address)) {
        if (bus->serial != NULL) {
            serial_write(bus->serial, address, value);
        }
        return;
    }

    if (bus_is_timer_address(address)) {
        if (bus->timer != NULL) {
            timer_write(bus->timer, address, value);
        }
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
    }
}
