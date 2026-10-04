/*
 * OAM DMA through the dma.h interface and the Bus: what it copies and from
 * where, when it starts and ends, and what the CPU can reach meanwhile.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <dma.h>
#include <interrupts.h>
#include <memory.h>
#include <ppu.h>

typedef struct Machine {
    Cartridge cartridge;
    Memory memory;
    InterruptRegisters interrupts;
    Ppu ppu;
    Dma dma;
    Bus bus;
} Machine;

static void machine_init(Machine *m)
{
    cartridge_init(&m->cartridge);
    m->cartridge.rom_size = 0x8000;
    m->cartridge.rom = calloc(m->cartridge.rom_size, 1);
    assert(m->cartridge.rom != NULL);

    for (unsigned i = 0; i < 0x100; i++) {
        m->cartridge.rom[i] = (uint8_t)(0x40 + i);
    }

    memory_init(&m->memory);
    interrupts_init(&m->interrupts);
    ppu_init(&m->ppu, &m->interrupts);
    bus_init(&m->bus, &m->cartridge, &m->memory, &m->interrupts);
    bus_attach_ppu(&m->bus, &m->ppu);
    dma_init(&m->dma, &m->bus, &m->ppu);
    bus_attach_dma(&m->bus, &m->dma);

    /* Work RAM page C0: byte i holds 3 * i + 1. */
    for (unsigned i = 0; i < 0x100; i++) {
        bus_write(&m->bus, (uint16_t)(0xC000 + i), (uint8_t)(3 * i + 1));
        bus_write(&m->bus, (uint16_t)(0xC100 + i), (uint8_t)(0xFF - i));
    }
}

static void machine_destroy(Machine *m)
{
    cartridge_destroy(&m->cartridge);
}

static void m_cycles(Machine *m, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        dma_step(&m->dma, 4);
    }
}

static void test_copies_one_byte_per_m_cycle(void)
{
    Machine m;

    machine_init(&m);
    assert(!dma_is_copying(&m.dma));

    bus_write(&m.bus, DMA_ADDRESS, 0xC0);

    /*
     * Counting the write as M-cycle 0: the CPU can still use the bus at
     * M = 1, the transfer takes it from M = 2, and byte 0 is copied during
     * M = 2. The machine ticks after each M-cycle, so the transfer owns the
     * bus after the second tick.
     */
    assert(!dma_is_copying(&m.dma));
    m_cycles(&m, 1);
    assert(!dma_is_copying(&m.dma));
    m_cycles(&m, 1);
    assert(dma_is_copying(&m.dma));
    assert(m.ppu.oam[0] == 0);

    m_cycles(&m, 1);
    assert(m.ppu.oam[0] == 1);
    assert(m.ppu.oam[1] == 0);

    m_cycles(&m, 79);
    for (unsigned i = 0; i < 80; i++) {
        assert(m.ppu.oam[i] == (uint8_t)(3 * i + 1));
    }
    assert(m.ppu.oam[80] == 0);
    assert(dma_is_copying(&m.dma));

    m_cycles(&m, 79);
    assert(dma_is_copying(&m.dma));
    m_cycles(&m, 1);

    /* 160 copies, one per M-cycle, and then the bus is free again. */
    assert(!dma_is_copying(&m.dma));

    for (unsigned i = 0; i < 160; i++) {
        assert(m.ppu.oam[i] == (uint8_t)(3 * i + 1));
    }

    /* Nothing more happens afterwards. */
    m.ppu.oam[0] = 0x99;
    m_cycles(&m, 10);
    assert(m.ppu.oam[0] == 0x99);

    machine_destroy(&m);
}

static void test_register_reads_back(void)
{
    Machine m;

    machine_init(&m);
    bus_write(&m.bus, DMA_ADDRESS, 0xC1);
    assert(bus_read(&m.bus, DMA_ADDRESS) == 0xC1);
    assert(dma_read(&m.dma) == 0xC1);

    machine_destroy(&m);
}

static void test_the_cpu_bus_is_taken(void)
{
    Machine m;

    machine_init(&m);
    bus_write(&m.bus, 0xFF80, 0x77);

    /* Before the transfer everything is reachable. */
    assert(bus_read(&m.bus, 0xC000) == 1);
    assert(bus_read(&m.bus, 0x0000) == 0x40);

    bus_write(&m.bus, DMA_ADDRESS, 0xC0);
    m_cycles(&m, 2);
    assert(dma_is_copying(&m.dma));

    /* The source is work RAM, so the external bus is taken: ROM, work RAM,
     * its echo and cartridge RAM are blocked, and OAM always is. */
    assert(bus_read(&m.bus, 0x0000) == 0xFF);
    assert(bus_read(&m.bus, 0xC000) == 0xFF);
    assert(bus_read(&m.bus, 0xE000) == 0xFF);
    assert(bus_read(&m.bus, 0xFE00) == 0xFF);
    assert(bus_read(&m.bus, 0xA000) == 0xFF);

    /* Video RAM is on the other bus and stays reachable. */
    m.ppu.vram[0x10] = 0x6B;
    assert(bus_read(&m.bus, 0x8010) == 0x6B);

    /* High RAM and the I/O registers are not. */
    assert(bus_read(&m.bus, 0xFF80) == 0x77);
    bus_write(&m.bus, 0xFF81, 0x88);
    assert(bus_read(&m.bus, 0xFF81) == 0x88);
    bus_write(&m.bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_TIMER);
    assert(bus_read(&m.bus, INTERRUPT_FLAG_ADDRESS) == (INTERRUPT_TIMER | 0xE0));
    assert(bus_read(&m.bus, INTERRUPT_ENABLE_ADDRESS) == 0);

    /* Writes to blocked areas are dropped. */
    bus_write(&m.bus, 0xC000, 0xEE);

    m_cycles(&m, 160);
    assert(!dma_is_copying(&m.dma));
    assert(bus_read(&m.bus, 0xC000) == 1);
    assert(bus_read(&m.bus, 0x0000) == 0x40);

    machine_destroy(&m);
}

static void test_sources(void)
{
    Machine m;

    /* ROM. */
    machine_init(&m);
    bus_write(&m.bus, DMA_ADDRESS, 0x00);
    m_cycles(&m, 162);
    for (unsigned i = 0; i < 160; i++) {
        assert(m.ppu.oam[i] == (uint8_t)(0x40 + i));
    }
    machine_destroy(&m);

    /* VRAM, even though the CPU could not read it right now. */
    machine_init(&m);
    for (unsigned i = 0; i < 160; i++) {
        m.ppu.vram[0x100 + i] = (uint8_t)(0xA0 + i);
    }
    bus_write(&m.bus, DMA_ADDRESS, 0x81);
    m_cycles(&m, 162);
    for (unsigned i = 0; i < 160; i++) {
        assert(m.ppu.oam[i] == (uint8_t)(0xA0 + i));
    }
    machine_destroy(&m);

    /* Pages E0 and up read the work RAM mirror: E0 is C0, FE is DE. */
    machine_init(&m);
    bus_write(&m.bus, DMA_ADDRESS, 0xE0);
    m_cycles(&m, 162);
    assert(m.ppu.oam[0] == 1);
    assert(m.ppu.oam[159] == (uint8_t)(3 * 159 + 1));
    machine_destroy(&m);

    machine_init(&m);
    for (unsigned i = 0; i < 160; i++) {
        bus_write(&m.bus, (uint16_t)(0xDE00 + i), (uint8_t)(0x10 + i));
    }
    bus_write(&m.bus, DMA_ADDRESS, 0xFE);
    m_cycles(&m, 162);
    for (unsigned i = 0; i < 160; i++) {
        assert(m.ppu.oam[i] == (uint8_t)(0x10 + i));
    }
    machine_destroy(&m);
}

static void test_restart(void)
{
    Machine m;

    machine_init(&m);
    bus_write(&m.bus, DMA_ADDRESS, 0xC0);
    m_cycles(&m, 42);
    assert(m.ppu.oam[39] == (uint8_t)(3 * 39 + 1));

    /*
     * Writing again starts over from the new page. The old transfer is not
     * stopped at once: it keeps the bus through the two start-up M-cycles,
     * so OAM stays blocked, where a fresh transfer would leave it open.
     */
    bus_write(&m.bus, DMA_ADDRESS, 0xC1);
    assert(bus_read(&m.bus, DMA_ADDRESS) == 0xC1);
    assert(dma_is_copying(&m.dma));
    m_cycles(&m, 1);
    assert(dma_is_copying(&m.dma));
    m_cycles(&m, 1);
    assert(dma_is_copying(&m.dma));

    /* The new transfer then runs its full 160 M-cycles. */
    m_cycles(&m, 159);
    assert(dma_is_copying(&m.dma));
    m_cycles(&m, 1);
    assert(!dma_is_copying(&m.dma));

    for (unsigned i = 0; i < 160; i++) {
        assert(m.ppu.oam[i] == (uint8_t)(0xFF - i));
    }

    machine_destroy(&m);
}

/*
 * The CPU shares a bus with the transfer: the external bus (ROM, cartridge
 * RAM, work RAM and its echo) or the video bus (VRAM), depending on where
 * the source is. OAM is always taken. A VRAM source leaves code running
 * from work RAM or the echo, which the Mooneye timing ROMs rely on.
 */
static void test_vram_source_blocks_only_vram(void)
{
    Machine m;

    machine_init(&m);
    m.ppu.vram[0x20] = 0x3D;
    bus_write(&m.bus, DMA_ADDRESS, 0x80);
    m_cycles(&m, 2);
    assert(dma_is_copying(&m.dma));

    assert(bus_read(&m.bus, 0x8020) == 0xFF);   /* video bus: taken */
    assert(bus_read(&m.bus, 0xFE00) == 0xFF);   /* OAM: always */

    assert(bus_read(&m.bus, 0x0000) == 0x40);   /* external bus: free */
    assert(bus_read(&m.bus, 0xC000) == 1);
    assert(bus_read(&m.bus, 0xE000) == 1);      /* echo of C000 */
    bus_write(&m.bus, 0xC200, 0x99);
    bus_write(&m.bus, 0xE201, 0x98);

    m_cycles(&m, 160);
    assert(!dma_is_copying(&m.dma));
    assert(bus_read(&m.bus, 0xC200) == 0x99);
    assert(bus_read(&m.bus, 0xC201) == 0x98);
    assert(bus_read(&m.bus, 0x8020) == 0x3D);

    machine_destroy(&m);
}

int main(void)
{
    test_copies_one_byte_per_m_cycle();
    test_register_reads_back();
    test_the_cpu_bus_is_taken();
    test_sources();
    test_restart();
    test_vram_source_blocks_only_vram();

    printf("OAM DMA tests passed!\n");

    return 0;
}
