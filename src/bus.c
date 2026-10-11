#include <stdbool.h>
#include <stdint.h>

#include <apu.h>
#include <bus.h>
#include <cartridge.h>
#include <dma.h>
#include <joypad.h>
#include <memory.h>
#include <memory_map.h>
#include <ppu.h>
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

/* NR10-NR52 and wave RAM; the addresses between them are unused. */
static bool bus_is_apu_address(uint16_t address)
{
    return (address >= APU_REGISTERS_START && address <= APU_REGISTERS_END) ||
           (address >= APU_WAVE_RAM_START && address <= APU_WAVE_RAM_END);
}

static bool bus_is_dma_address(uint16_t address)
{
    return address == DMA_ADDRESS;
}

static bool bus_is_joypad_address(uint16_t address)
{
    return address == JOYPAD_ADDRESS;
}

/*
 * While OAM DMA copies, the CPU shares a bus with it: OAM is always taken,
 * and so is the bus the source is on, the external bus (ROM, cartridge RAM,
 * work RAM, echo) or the video bus (VRAM). The I/O registers and HRAM are
 * free.
 */
static bool bus_blocked_by_dma(const Bus *bus, uint16_t address)
{
    if (!dma_is_copying(bus->dma)) {
        return false;
    }

    if (address >= PPU_OAM_START && address <= PPU_OAM_END) {
        return true;
    }

    if (address >= PPU_VRAM_START && address <= PPU_VRAM_END) {
        return dma_source_is_vram(bus->dma);
    }

    bool external = address <= MEM_ROM_END ||
                    (address >= MEM_CART_RAM_START && address <= MEM_ECHO_END);

    return external && !dma_source_is_vram(bus->dma);
}

static bool bus_is_ppu_address(uint16_t address)
{
    return (address >= PPU_VRAM_START && address <= PPU_VRAM_END) ||
           (address >= PPU_OAM_START && address <= PPU_OAM_END) ||
           (address >= PPU_LCDC_ADDRESS && address <= PPU_LYC_ADDRESS) ||
           (address >= PPU_BGP_ADDRESS && address <= PPU_WX_ADDRESS);
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
    InterruptRegisters *interrupts,
    Timer *timer,
    Serial *serial,
    Ppu *ppu,
    Dma *dma,
    Joypad *joypad,
    Apu *apu
)
{
    bus->cartridge = cartridge;
    bus->memory = memory;
    bus->interrupts = interrupts;
    bus->timer = timer;
    bus->serial = serial;
    bus->ppu = ppu;
    bus->dma = dma;
    bus->joypad = joypad;
    bus->apu = apu;
}

static uint8_t bus_read_unlocked(Bus *bus, uint16_t address)
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

    if (bus_is_ppu_address(address)) {
        return ppu_read(bus->ppu, address);
    }

    if (bus_is_apu_address(address)) {
        return apu_read(bus->apu, address);
    }

    if (bus_is_joypad_address(address)) {
        return joypad_read(bus->joypad);
    }

    if (bus_is_dma_address(address)) {
        return dma_read(bus->dma);
    }

    if (bus_is_serial_address(address)) {
        return serial_read(bus->serial, address);
    }

    if (bus_is_timer_address(address)) {
        return timer_read(bus->timer, address);
    }

    if (address == INTERRUPT_FLAG_ADDRESS) {
        /* The upper three bits of IF read as 1. */
        return (uint8_t)(bus->interrupts->interrupt_flag | 0xE0);
    }

    if (address == INTERRUPT_ENABLE_ADDRESS) {
        return bus->interrupts->interrupt_enable;
    }

    /*
     * Unimplemented memory region.
     */
    return 0xFF;
}

uint8_t bus_read(Bus *bus, uint16_t address)
{
    if (bus_blocked_by_dma(bus, address)) {
        return 0xFF;
    }

    return bus_read_unlocked(bus, address);
}

uint8_t bus_dma_read(Bus *bus, uint16_t address)
{
    if (address >= MEM_ECHO_START) {
        address = (uint16_t)(address - MEM_ECHO_OFFSET);
    }

    return bus_read_unlocked(bus, address);
}

void bus_write(Bus *bus, uint16_t address, uint8_t value)
{
    if (bus_blocked_by_dma(bus, address)) {
        return;
    }

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

    if (bus_is_ppu_address(address)) {
        ppu_write(bus->ppu, address, value);
        return;
    }

    if (bus_is_joypad_address(address)) {
        joypad_write(bus->joypad, value);
        return;
    }

    if (bus_is_apu_address(address)) {
        apu_write(bus->apu, address, value);
        return;
    }

    if (bus_is_dma_address(address)) {
        dma_write(bus->dma, value);
        return;
    }

    if (bus_is_serial_address(address)) {
        serial_write(bus->serial, address, value);
        return;
    }

    if (bus_is_timer_address(address)) {
        uint16_t before = bus->timer->divider;
        timer_write(bus->timer, address, value);
        if (address == TIMER_DIV_ADDRESS) {
            serial_divider_changed(bus->serial, before, bus->timer->divider);
        }
        return;
    }

    if (address == INTERRUPT_FLAG_ADDRESS) {
        bus->interrupts->interrupt_flag =
            (uint8_t)(value & INTERRUPT_VALID_MASK);
        return;
    }

    if (address == INTERRUPT_ENABLE_ADDRESS) {
        /* IE keeps all 8 bits, though only the low five are sources. */
        bus->interrupts->interrupt_enable = value;
    }
}
