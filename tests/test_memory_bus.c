#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <memory.h>

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
    Cartridge cartridge;
    Memory memory;
    Bus bus;
    InterruptRegisters interrupts = {0};

    cartridge_init(&cartridge);
    cartridge.rom_size = 0x8000;
    cartridge.rom = calloc(cartridge.rom_size, sizeof(uint8_t));
    assert(cartridge.rom != NULL);

    cartridge.rom[0x0000] = 0x12;
    cartridge.rom[0x7FFF] = 0x34;

    memory_init(&memory);
    bus_init(&bus, &cartridge, &memory, &interrupts);

    assert(bus_read(&bus, 0x0000) == 0x12);
    assert(bus_read(&bus, 0x7FFF) == 0x34);
    assert(bus_read(&bus, 0x8000) == 0xFF);

    bus_write(&bus, 0x0000, 0x56);
    bus_write(&bus, 0x7FFF, 0x78);

    assert(cartridge.rom[0x0000] == 0x12);
    assert(cartridge.rom[0x7FFF] == 0x34);

    bus_write(&bus, 0xC000, 0x9A);
    bus_write(&bus, 0xDFFF, 0xBC);

    assert(bus_read(&bus, 0xC000) == 0x9A);
    assert(bus_read(&bus, 0xDFFF) == 0xBC);
    assert(bus_read(&bus, 0xBFFF) == 0xFF);
    assert(bus_read(&bus, 0xE000) == 0xFF);

    bus_write(&bus, 0xFF80, 0xDE);
    bus_write(&bus, 0xFFFE, 0xF0);

    assert(bus_read(&bus, 0xFF80) == 0xDE);
    assert(bus_read(&bus, 0xFFFE) == 0xF0);
    assert(bus_read(&bus, 0xFF7F) == 0xFF);
    assert(bus_read(&bus, 0xFF0F) == 0);
    assert(bus_read(&bus, 0xFFFF) == 0);

    bus_write(&bus, 0xFF7F, 0x01);
    bus_write(&bus, 0xFF0F, INTERRUPT_VBLANK);
    bus_write(&bus, 0xFFFF, INTERRUPT_TIMER);

    assert(bus_read(&bus, 0xFF7F) == 0xFF);
    assert(bus_read(&bus, 0xFF0F) == INTERRUPT_VBLANK);
    assert(bus_read(&bus, 0xFFFF) == INTERRUPT_TIMER);

    cartridge_destroy(&cartridge);
}

int main(void)
{
    test_memory_boundaries();
    test_bus_boundaries();

    printf("Memory and Bus tests passed!\n");
    return 0;
}
