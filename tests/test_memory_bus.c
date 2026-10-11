#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <memory.h>

#include "test_util.h"

enum {
    TYPE_MBC1_RAM_BATTERY = 0x03,
    RAM_CODE_8K = 0x02
};

static const char *rom_path = "test_memory_bus_rom.gb";

static void test_memory_boundaries(void)
{
    Memory memory;
    memory_init(&memory);

    memory_write(&memory, 0xC000, 0x11);
    memory_write(&memory, 0xDFFF, 0x22);

    assert(memory_read(&memory, 0xC000) == 0x11);
    assert(memory_read(&memory, 0xDFFF) == 0x22);
    assert(memory_read(&memory, 0xBFFF) == 0xFF);
    assert(memory_read(&memory, 0xE000) == 0xFF);

    memory_write(&memory, 0xBFFF, 0x33);
    memory_write(&memory, 0xE000, 0x44);

    assert(memory_read(&memory, 0xBFFF) == 0xFF);
    assert(memory_read(&memory, 0xE000) == 0xFF);

    memory_write(&memory, 0xFF80, 0x55);
    memory_write(&memory, 0xFFFE, 0x66);

    assert(memory_read(&memory, 0xFF80) == 0x55);
    assert(memory_read(&memory, 0xFFFE) == 0x66);
    assert(memory_read(&memory, 0xFF7F) == 0xFF);
    assert(memory_read(&memory, 0xFFFF) == 0xFF);

    memory_write(&memory, 0xFF7F, 0x77);
    memory_write(&memory, 0xFFFF, 0x88);

    assert(memory_read(&memory, 0xFF7F) == 0xFF);
    assert(memory_read(&memory, 0xFFFF) == 0xFF);
}

static void test_bus_boundaries(void)
{
    TestMachine machine;
    Bus *bus = &machine.bus;

    test_machine_init(&machine);
    machine.cartridge.rom[0x0000] = 0x12;
    machine.cartridge.rom[0x7FFF] = 0x34;

    assert(bus_read(bus, 0x0000) == 0x12);
    assert(bus_read(bus, 0x7FFF) == 0x34);

    bus_write(bus, 0x0000, 0x56);
    bus_write(bus, 0x7FFF, 0x78);

    assert(machine.cartridge.rom[0x0000] == 0x12);
    assert(machine.cartridge.rom[0x7FFF] == 0x34);

    bus_write(bus, 0xC000, 0x9A);
    bus_write(bus, 0xDFFF, 0xBC);

    assert(bus_read(bus, 0xC000) == 0x9A);
    assert(bus_read(bus, 0xDFFF) == 0xBC);
    assert(bus_read(bus, 0xBFFF) == 0xFF);

    /* E000-FDFF mirrors C000-DDFF. */
    assert(bus_read(bus, 0xE000) == 0x9A);
    bus_write(bus, 0xFDFF, 0x5A);
    assert(bus_read(bus, 0xDDFF) == 0x5A);
    bus_write(bus, 0xE001, 0xA5);
    assert(bus_read(bus, 0xC001) == 0xA5);

    bus_write(bus, 0xFF80, 0xDE);
    bus_write(bus, 0xFFFE, 0xF0);

    assert(bus_read(bus, 0xFF80) == 0xDE);
    assert(bus_read(bus, 0xFFFE) == 0xF0);
    assert(bus_read(bus, 0xFF7F) == 0xFF);
    assert(bus_read(bus, 0xFF0F) == 0xE0);
    assert(bus_read(bus, 0xFFFF) == 0);

    bus_write(bus, 0xFF7F, 0x01);
    bus_write(bus, 0xFF0F, INTERRUPT_VBLANK);
    bus_write(bus, 0xFFFF, INTERRUPT_TIMER);

    assert(bus_read(bus, 0xFF7F) == 0xFF);
    assert(bus_read(bus, 0xFF0F) == (INTERRUPT_VBLANK | 0xE0));
    assert(bus_read(bus, 0xFFFF) == INTERRUPT_TIMER);

    test_machine_destroy(&machine);
}

/* ---- Routing: every address reaches the part the DMG's map gives it ---- */

/* A byte that depends on the address and the region, and is never 0xFF, so
 * no region's contents look like another region's or like an open bus. */
static uint8_t fill(uint16_t address, uint8_t region)
{
    uint8_t value = (uint8_t)(address ^ (address >> 8) ^ region);

    return value == 0xFF ? 0x00 : value;
}

static void write_routing_rom(void)
{
    static uint8_t rom[0x8000];

    for (unsigned address = 0; address < sizeof(rom); address++) {
        rom[address] = fill((uint16_t)address, 0x11);
    }
    rom[CARTRIDGE_HEADER_TYPE] = TYPE_MBC1_RAM_BATTERY;
    rom[CARTRIDGE_HEADER_RAM_SIZE] = RAM_CODE_8K;

    test_write_file(rom_path, rom, sizeof(rom));
}

/*
 * A machine in which every region holds bytes no other region holds: an MBC1
 * cartridge with its RAM enabled, the LCD off so VRAM and OAM are open, and
 * registers that read as something other than 0xFF. Everything is set through
 * the devices themselves, not the bus under test.
 */
static void routing_machine_init(TestMachine *m)
{
    test_machine_init(m);
    cartridge_destroy(&m->cartridge);
    cartridge_init(&m->cartridge);
    assert(cartridge_load(&m->cartridge, rom_path, NULL) == CARTRIDGE_LOAD_OK);

    cartridge_write(&m->cartridge, 0x0000, 0x0A);
    for (uint16_t a = 0xA000; a <= 0xBFFF; a++) {
        cartridge_write_ram(&m->cartridge, a, fill(a, 0x22));
    }
    for (uint16_t a = 0xC000; a <= 0xDFFF; a++) {
        memory_write(&m->memory, a, fill(a, 0x33));
    }
    for (uint16_t a = 0xFF80; a <= 0xFFFE; a++) {
        memory_write(&m->memory, a, fill(a, 0x44));
    }

    ppu_write(&m->ppu, 0xFF40, 0x00);
    for (size_t i = 0; i < sizeof(m->ppu.vram); i++) {
        m->ppu.vram[i] = fill((uint16_t)(0x8000 + i), 0x55);
    }
    for (size_t i = 0; i < sizeof(m->ppu.oam); i++) {
        m->ppu.oam[i] = fill((uint16_t)(0xFE00 + i), 0x66);
    }
    for (uint16_t a = 0xFF42; a <= 0xFF4B; a++) {
        if (a != 0xFF44 && a != 0xFF46) {
            ppu_write(&m->ppu, a, fill(a, 0x77));
        }
    }

    apu_write(&m->apu, 0xFF26, 0x80);
    for (uint16_t a = 0xFF10; a <= 0xFF25; a++) {
        apu_write(&m->apu, a, fill(a, 0x08));
    }
    /* Wave RAM reads 0xFF while channel 3 plays: turn its DAC off. */
    apu_write(&m->apu, 0xFF1A, 0x00);
    for (uint16_t a = 0xFF30; a <= 0xFF3F; a++) {
        apu_write(&m->apu, a, fill(a, 0x99));
    }

    joypad_write(&m->joypad, 0x10);
    serial_write(&m->serial, 0xFF01, 0x3C);
    serial_write(&m->serial, 0xFF02, 0x01);
    m->timer.divider = 0x3400;
    timer_write(&m->timer, 0xFF05, 0x56);
    timer_write(&m->timer, 0xFF06, 0x78);
    timer_write(&m->timer, 0xFF07, 0x05);
    m->dma.register_value = 0x9C;
    m->interrupts.interrupt_flag = 0x0A;
    m->interrupts.interrupt_enable = 0x15;
}

/* What a read of each address returns, from the DMG's memory map. */
static uint8_t expected_read(const TestMachine *m, uint16_t a)
{
    if (a <= 0x7FFF) {
        return cartridge_read(&m->cartridge, a);
    }
    if (a <= 0x9FFF) {
        return ppu_read(&m->ppu, a);
    }
    if (a <= 0xBFFF) {
        return cartridge_read_ram(&m->cartridge, a);
    }
    if (a <= 0xDFFF) {
        return memory_read(&m->memory, a);
    }
    if (a <= 0xFDFF) {
        return memory_read(&m->memory, (uint16_t)(a - 0x2000));
    }
    if (a <= 0xFE9F) {
        return ppu_read(&m->ppu, a);
    }
    if (a == 0xFF00) {
        return joypad_read(&m->joypad);
    }
    if (a == 0xFF01 || a == 0xFF02) {
        return serial_read(&m->serial, a);
    }
    if (a >= 0xFF04 && a <= 0xFF07) {
        return timer_read(&m->timer, a);
    }
    if (a == 0xFF0F) {
        return (uint8_t)(m->interrupts.interrupt_flag | 0xE0);
    }
    if ((a >= 0xFF10 && a <= 0xFF26) || (a >= 0xFF30 && a <= 0xFF3F)) {
        return apu_read(&m->apu, a);
    }
    if ((a >= 0xFF40 && a <= 0xFF45) || (a >= 0xFF47 && a <= 0xFF4B)) {
        return ppu_read(&m->ppu, a);
    }
    if (a == 0xFF46) {
        return dma_read(&m->dma);
    }
    if (a >= 0xFF80 && a <= 0xFFFE) {
        return memory_read(&m->memory, a);
    }
    if (a == 0xFFFF) {
        return m->interrupts.interrupt_enable;
    }
    return 0xFF;
}

/* Where a write to each address goes, from the DMG's memory map. */
static void expected_write(TestMachine *m, uint16_t a, uint8_t value)
{
    if (a <= 0x7FFF) {
        cartridge_write(&m->cartridge, a, value);
    } else if (a <= 0x9FFF) {
        ppu_write(&m->ppu, a, value);
    } else if (a <= 0xBFFF) {
        cartridge_write_ram(&m->cartridge, a, value);
    } else if (a <= 0xDFFF) {
        memory_write(&m->memory, a, value);
    } else if (a <= 0xFDFF) {
        memory_write(&m->memory, (uint16_t)(a - 0x2000), value);
    } else if (a <= 0xFE9F) {
        ppu_write(&m->ppu, a, value);
    } else if (a == 0xFF00) {
        joypad_write(&m->joypad, value);
    } else if (a == 0xFF01 || a == 0xFF02) {
        serial_write(&m->serial, a, value);
    } else if (a >= 0xFF04 && a <= 0xFF07) {
        uint16_t before = m->timer.divider;
        timer_write(&m->timer, a, value);
        /* Resetting DIV can clock the serial port. */
        serial_divider_changed(&m->serial, before, m->timer.divider);
    } else if (a == 0xFF0F) {
        m->interrupts.interrupt_flag = (uint8_t)(value & 0x1F);
    } else if ((a >= 0xFF10 && a <= 0xFF26) || (a >= 0xFF30 && a <= 0xFF3F)) {
        apu_write(&m->apu, a, value);
    } else if ((a >= 0xFF40 && a <= 0xFF45) || (a >= 0xFF47 && a <= 0xFF4B)) {
        ppu_write(&m->ppu, a, value);
    } else if (a == 0xFF46) {
        dma_write(&m->dma, value);
    } else if (a >= 0xFF80 && a <= 0xFFFE) {
        memory_write(&m->memory, a, value);
    } else if (a == 0xFFFF) {
        m->interrupts.interrupt_enable = value;
    }
}

static void assert_reads(Bus *bus, uint16_t address, uint8_t want)
{
    uint8_t got = bus_read(bus, address);

    if (got != want) {
        fprintf(stderr, "%04X reads %02X, expected %02X\n", address, got, want);
    }
    assert(got == want);
}

static void test_every_address_reads_its_owner(void)
{
    TestMachine m;

    routing_machine_init(&m);
    for (unsigned a = 0; a <= 0xFFFF; a++) {
        assert_reads(&m.bus, (uint16_t)a, expected_read(&m, (uint16_t)a));
    }
    test_machine_destroy(&m);
}

/*
 * A write through the bus to the first or last address of each region, and
 * of each unused gap, changes the machine exactly as writing to the owner
 * directly does.
 */
static void test_region_edges_write_to_their_owner(void)
{
    static const uint16_t edges[] = {
        0x0000, 0x7FFF, 0x8000, 0x9FFF, 0xA000, 0xBFFF, 0xC000, 0xDFFF,
        0xE000, 0xFDFF, 0xFE00, 0xFE9F, 0xFEA0, 0xFEFF, 0xFF00, 0xFF01,
        0xFF02, 0xFF03, 0xFF04, 0xFF07, 0xFF08, 0xFF0E, 0xFF0F, 0xFF10,
        0xFF26, 0xFF27, 0xFF2F, 0xFF30, 0xFF3F, 0xFF40, 0xFF45, 0xFF46,
        0xFF47, 0xFF4B, 0xFF4C, 0xFF7F, 0xFF80, 0xFFFE, 0xFFFF
    };

    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        TestMachine through_bus;
        TestMachine direct;

        routing_machine_init(&through_bus);
        routing_machine_init(&direct);
        bus_write(&through_bus.bus, edges[i], 0x6D);
        expected_write(&direct, edges[i], 0x6D);

        for (unsigned a = 0; a <= 0xFFFF; a++) {
            assert_reads(
                &through_bus.bus,
                (uint16_t)a,
                bus_read(&direct.bus, (uint16_t)a)
            );
        }
        test_machine_destroy(&through_bus);
        test_machine_destroy(&direct);
    }
}

int main(void)
{
    test_memory_boundaries();
    test_bus_boundaries();

    write_routing_rom();
    test_every_address_reads_its_owner();
    test_region_edges_write_to_their_owner();
    assert(remove(rom_path) == 0);

    printf("Memory and Bus tests passed!\n");
    return 0;
}
