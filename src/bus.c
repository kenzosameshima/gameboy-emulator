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

/* The regions of the address map; see the table in bus.h. */
typedef enum BusRegion {
    BUS_REGION_ROM,
    BUS_REGION_VRAM,
    BUS_REGION_CART_RAM,
    BUS_REGION_WRAM,
    BUS_REGION_ECHO,
    BUS_REGION_OAM,
    BUS_REGION_JOYPAD,
    BUS_REGION_SERIAL,
    BUS_REGION_TIMER,
    BUS_REGION_IF,
    BUS_REGION_APU,
    BUS_REGION_LCD,
    BUS_REGION_DMA,
    BUS_REGION_HRAM,
    BUS_REGION_IE,
    BUS_REGION_UNMAPPED
} BusRegion;

/*
 * The only place that knows where each region starts and ends. Reads,
 * writes and the OAM DMA's bus conflicts all switch on its answer.
 */
static BusRegion bus_decode(uint16_t address)
{
    /* 0000-FE9F: each region starts where the one before it ends. */
    if (address <= MEM_ROM_END) {
        return BUS_REGION_ROM;
    }
    if (address <= PPU_VRAM_END) {
        return BUS_REGION_VRAM;
    }
    if (address <= MEM_CART_RAM_END) {
        return BUS_REGION_CART_RAM;
    }
    if (address <= MEM_WRAM_END) {
        return BUS_REGION_WRAM;
    }
    if (address <= MEM_ECHO_END) {
        return BUS_REGION_ECHO;
    }
    if (address <= PPU_OAM_END) {
        return BUS_REGION_OAM;
    }

    /* FEA0-FFFF: registers and high RAM, with unused gaps between. */
    if (address == JOYPAD_ADDRESS) {
        return BUS_REGION_JOYPAD;
    }
    if (address >= SERIAL_SB_ADDRESS && address <= SERIAL_SC_ADDRESS) {
        return BUS_REGION_SERIAL;
    }
    if (address >= TIMER_DIV_ADDRESS && address <= TIMER_TAC_ADDRESS) {
        return BUS_REGION_TIMER;
    }
    if (address == INTERRUPT_FLAG_ADDRESS) {
        return BUS_REGION_IF;
    }
    if (address >= APU_REGISTERS_START && address <= APU_REGISTERS_END) {
        return BUS_REGION_APU;
    }
    if (address >= APU_WAVE_RAM_START && address <= APU_WAVE_RAM_END) {
        return BUS_REGION_APU;
    }
    if (address >= PPU_LCDC_ADDRESS && address <= PPU_LYC_ADDRESS) {
        return BUS_REGION_LCD;
    }
    if (address == DMA_ADDRESS) {
        return BUS_REGION_DMA;
    }
    if (address >= PPU_BGP_ADDRESS && address <= PPU_WX_ADDRESS) {
        return BUS_REGION_LCD;
    }
    if (address >= MEM_HRAM_START && address <= MEM_HRAM_END) {
        return BUS_REGION_HRAM;
    }
    if (address == INTERRUPT_ENABLE_ADDRESS) {
        return BUS_REGION_IE;
    }

    return BUS_REGION_UNMAPPED;
}

/*
 * While OAM DMA copies, the CPU shares a bus with it: OAM is always taken,
 * and so is the bus the source is on, the external bus (ROM, cartridge RAM,
 * work RAM, echo) or the video bus (VRAM).
 */
static bool bus_blocked_by_dma(const Bus *bus, BusRegion region)
{
    if (!dma_is_copying(bus->dma)) {
        return false;
    }

    switch (region) {
        case BUS_REGION_OAM:
            return true;

        case BUS_REGION_VRAM:
            return dma_source_is_vram(bus->dma);

        case BUS_REGION_ROM:
        case BUS_REGION_CART_RAM:
        case BUS_REGION_WRAM:
        case BUS_REGION_ECHO:
            return !dma_source_is_vram(bus->dma);

        case BUS_REGION_JOYPAD:
        case BUS_REGION_SERIAL:
        case BUS_REGION_TIMER:
        case BUS_REGION_IF:
        case BUS_REGION_APU:
        case BUS_REGION_LCD:
        case BUS_REGION_DMA:
        case BUS_REGION_HRAM:
        case BUS_REGION_IE:
        case BUS_REGION_UNMAPPED:
            break;
    }

    /* The I/O registers and HRAM are free. */
    return false;
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

static uint8_t bus_read_region(Bus *bus, BusRegion region, uint16_t address)
{
    switch (region) {
        case BUS_REGION_ROM:
            return cartridge_read(bus->cartridge, address);

        case BUS_REGION_CART_RAM:
            return cartridge_read_ram(bus->cartridge, address);

        case BUS_REGION_WRAM:
        case BUS_REGION_HRAM:
            return memory_read(bus->memory, address);

        case BUS_REGION_ECHO:
            return memory_read(
                bus->memory,
                (uint16_t)(address - MEM_ECHO_OFFSET)
            );

        case BUS_REGION_VRAM:
        case BUS_REGION_OAM:
        case BUS_REGION_LCD:
            return ppu_read(bus->ppu, address);

        case BUS_REGION_APU:
            return apu_read(bus->apu, address);

        case BUS_REGION_JOYPAD:
            return joypad_read(bus->joypad);

        case BUS_REGION_DMA:
            return dma_read(bus->dma);

        case BUS_REGION_SERIAL:
            return serial_read(bus->serial, address);

        case BUS_REGION_TIMER:
            return timer_read(bus->timer, address);

        case BUS_REGION_IF:
            /* The upper three bits of IF read as 1. */
            return (uint8_t)(bus->interrupts->interrupt_flag | 0xE0);

        case BUS_REGION_IE:
            return bus->interrupts->interrupt_enable;

        case BUS_REGION_UNMAPPED:
            break;
    }

    return 0xFF;
}

uint8_t bus_read(Bus *bus, uint16_t address)
{
    BusRegion region = bus_decode(address);

    if (bus_blocked_by_dma(bus, region)) {
        return 0xFF;
    }

    return bus_read_region(bus, region, address);
}

uint8_t bus_dma_read(Bus *bus, uint16_t address)
{
    if (address >= MEM_ECHO_START) {
        address = (uint16_t)(address - MEM_ECHO_OFFSET);
    }

    return bus_read_region(bus, bus_decode(address), address);
}

void bus_write(Bus *bus, uint16_t address, uint8_t value)
{
    BusRegion region = bus_decode(address);

    if (bus_blocked_by_dma(bus, region)) {
        return;
    }

    switch (region) {
        case BUS_REGION_ROM:
            cartridge_write(bus->cartridge, address, value);
            break;

        case BUS_REGION_CART_RAM:
            cartridge_write_ram(bus->cartridge, address, value);
            break;

        case BUS_REGION_WRAM:
        case BUS_REGION_HRAM:
            memory_write(bus->memory, address, value);
            break;

        case BUS_REGION_ECHO:
            memory_write(
                bus->memory,
                (uint16_t)(address - MEM_ECHO_OFFSET),
                value
            );
            break;

        case BUS_REGION_VRAM:
        case BUS_REGION_OAM:
        case BUS_REGION_LCD:
            ppu_write(bus->ppu, address, value);
            break;

        case BUS_REGION_JOYPAD:
            joypad_write(bus->joypad, value);
            break;

        case BUS_REGION_APU:
            apu_write(bus->apu, address, value);
            break;

        case BUS_REGION_DMA:
            dma_write(bus->dma, value);
            break;

        case BUS_REGION_SERIAL:
            serial_write(bus->serial, address, value);
            break;

        case BUS_REGION_TIMER: {
            uint16_t before = bus->timer->divider;
            timer_write(bus->timer, address, value);
            if (address == TIMER_DIV_ADDRESS) {
                serial_divider_changed(
                    bus->serial,
                    before,
                    bus->timer->divider
                );
            }
            break;
        }

        case BUS_REGION_IF:
            bus->interrupts->interrupt_flag =
                (uint8_t)(value & INTERRUPT_VALID_MASK);
            break;

        case BUS_REGION_IE:
            /* IE keeps all 8 bits, though only the low five are sources. */
            bus->interrupts->interrupt_enable = value;
            break;

        case BUS_REGION_UNMAPPED:
            break;
    }
}
