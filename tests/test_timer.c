#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <memory.h>
#include <timer.h>

static void setup_timer(
    Timer *timer,
    InterruptRegisters *interrupts,
    Bus *bus,
    Cartridge *cartridge,
    Memory *memory
)
{
    cartridge_init(cartridge);
    cartridge->rom_size = 0x8000;
    cartridge->rom = calloc(cartridge->rom_size, sizeof(uint8_t));
    assert(cartridge->rom != NULL);

    memory_init(memory);
    interrupts->interrupt_flag = 0;
    interrupts->interrupt_enable = 0;
    timer_init(timer, interrupts);
    bus_init(bus, cartridge, memory, interrupts);
    bus_attach_timer(bus, timer);
}

static void cleanup_timer(Cartridge *cartridge)
{
    cartridge_destroy(cartridge);
}

static void advance_timer(Timer *timer, uint16_t cycles)
{
    for (uint16_t cycle = 0; cycle < cycles; cycle++) {
        timer_step(timer, 1);
    }
}

static void test_registers(void)
{
    Timer timer;
    InterruptRegisters interrupts;
    Bus bus;
    Cartridge cartridge;
    Memory memory;
    setup_timer(&timer, &interrupts, &bus, &cartridge, &memory);

    assert(bus_read(&bus, TIMER_DIV_ADDRESS) == 0);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);
    assert(bus_read(&bus, TIMER_TMA_ADDRESS) == 0);
    assert(bus_read(&bus, TIMER_TAC_ADDRESS) == 0);

    advance_timer(&timer, 256);
    assert(bus_read(&bus, TIMER_DIV_ADDRESS) == 1);

    bus_write(&bus, TIMER_DIV_ADDRESS, 0xFF);
    assert(bus_read(&bus, TIMER_DIV_ADDRESS) == 0);

    bus_write(&bus, TIMER_TIMA_ADDRESS, 0x12);
    bus_write(&bus, TIMER_TMA_ADDRESS, 0x34);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0x12);
    assert(bus_read(&bus, TIMER_TMA_ADDRESS) == 0x34);

    bus_write(&bus, TIMER_TAC_ADDRESS, 0xFF);
    assert(bus_read(&bus, TIMER_TAC_ADDRESS) == TIMER_TAC_VALID_MASK);

    cleanup_timer(&cartridge);
}

static void test_disabled_timer(void)
{
    Timer timer;
    InterruptRegisters interrupts;
    Bus bus;
    Cartridge cartridge;
    Memory memory;
    setup_timer(&timer, &interrupts, &bus, &cartridge, &memory);

    bus_write(&bus, TIMER_TIMA_ADDRESS, 0);
    bus_write(&bus, TIMER_TAC_ADDRESS, 0);
    advance_timer(&timer, 1024);

    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);
    assert(interrupts.interrupt_flag == 0);

    cleanup_timer(&cartridge);
}

static void test_frequencies(void)
{
    const uint16_t periods[] = {1024, 16, 64, 256};

    for (uint8_t mode = 0; mode < 4; mode++) {
        Timer timer;
        InterruptRegisters interrupts;
        Bus bus;
        Cartridge cartridge;
        Memory memory;
        setup_timer(
            &timer,
            &interrupts,
            &bus,
            &cartridge,
            &memory
        );

        bus_write(
            &bus,
            TIMER_TAC_ADDRESS,
            (uint8_t)(TIMER_TAC_ENABLE | mode)
        );
        advance_timer(&timer, (uint16_t)(periods[mode] - 1));
        assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);
        timer_step(&timer, 1);
        assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 1);

        cleanup_timer(&cartridge);
    }
}

static void test_overflow_and_interrupt_request(void)
{
    Timer timer;
    InterruptRegisters interrupts;
    Bus bus;
    Cartridge cartridge;
    Memory memory;
    setup_timer(&timer, &interrupts, &bus, &cartridge, &memory);

    bus_write(&bus, TIMER_TIMA_ADDRESS, 0xFF);
    bus_write(&bus, TIMER_TMA_ADDRESS, 0x42);
    bus_write(
        &bus,
        TIMER_TAC_ADDRESS,
        (uint8_t)(TIMER_TAC_ENABLE | 1)
    );

    advance_timer(&timer, 16);

    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);
    assert(interrupts.interrupt_flag == 0);

    advance_timer(&timer, 3);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);
    assert(interrupts.interrupt_flag == 0);

    timer_step(&timer, 1);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0x42);
    assert(
        (interrupts.interrupt_flag & INTERRUPT_TIMER) != 0
    );

    cleanup_timer(&cartridge);
}

static void test_reload_window_writes_and_repeated_overflow(void)
{
    Timer timer;
    InterruptRegisters interrupts;
    Bus bus;
    Cartridge cartridge;
    Memory memory;
    setup_timer(&timer, &interrupts, &bus, &cartridge, &memory);

    bus_write(&bus, TIMER_TIMA_ADDRESS, 0xFF);
    bus_write(&bus, TIMER_TMA_ADDRESS, 0x12);
    bus_write(&bus, TIMER_TAC_ADDRESS, (uint8_t)(TIMER_TAC_ENABLE | 1));
    advance_timer(&timer, 16);

    bus_write(&bus, TIMER_TIMA_ADDRESS, 0x55);
    advance_timer(&timer, 4);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0x55);
    assert(interrupts.interrupt_flag == 0);

    timer_init(&timer, &interrupts);
    interrupts.interrupt_flag = 0;
    bus_write(&bus, TIMER_TIMA_ADDRESS, 0xFF);
    bus_write(&bus, TIMER_TMA_ADDRESS, 0x12);
    bus_write(&bus, TIMER_TAC_ADDRESS, (uint8_t)(TIMER_TAC_ENABLE | 1));
    advance_timer(&timer, 16);
    advance_timer(&timer, 3);
    bus_write(&bus, TIMER_TIMA_ADDRESS, 0x66);
    bus_write(&bus, TIMER_TMA_ADDRESS, 0x77);
    timer_step(&timer, 1);

    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0x77);
    assert((interrupts.interrupt_flag & INTERRUPT_TIMER) != 0);

    timer_init(&timer, &interrupts);
    interrupts.interrupt_flag = 0;
    bus_write(&bus, TIMER_TIMA_ADDRESS, 0xFF);
    bus_write(&bus, TIMER_TMA_ADDRESS, 0xFF);
    bus_write(&bus, TIMER_TAC_ADDRESS, (uint8_t)(TIMER_TAC_ENABLE | 1));
    advance_timer(&timer, 16);
    advance_timer(&timer, 4);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0xFF);
    assert((interrupts.interrupt_flag & INTERRUPT_TIMER) != 0);

    interrupts.interrupt_flag = 0;
    advance_timer(&timer, 12);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);
    advance_timer(&timer, 4);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0xFF);
    assert((interrupts.interrupt_flag & INTERRUPT_TIMER) != 0);

    cleanup_timer(&cartridge);
}

static void test_div_and_tac_edges(void)
{
    Timer timer;
    InterruptRegisters interrupts;
    Bus bus;
    Cartridge cartridge;
    Memory memory;
    setup_timer(&timer, &interrupts, &bus, &cartridge, &memory);

    bus_write(
        &bus,
        TIMER_TAC_ADDRESS,
        (uint8_t)(TIMER_TAC_ENABLE | 1)
    );
    timer_step(&timer, 8);
    bus_write(&bus, TIMER_DIV_ADDRESS, 0);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 1);

    bus_write(&bus, TIMER_TIMA_ADDRESS, 0);
    timer_step(&timer, 8);
    bus_write(&bus, TIMER_TAC_ADDRESS, 0);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 1);

    cleanup_timer(&cartridge);
}

static void test_timer_during_halt_and_pending_if(void)
{
    Timer timer;
    InterruptRegisters interrupts;
    Bus bus;
    Cartridge cartridge;
    Memory memory;
    CPU cpu;
    setup_timer(&timer, &interrupts, &bus, &cartridge, &memory);
    cpu_init(&cpu, &bus);

    cartridge.rom[0x0100] = 0x76;
    bus_write(
        &bus,
        TIMER_TAC_ADDRESS,
        (uint8_t)(TIMER_TAC_ENABLE | 1)
    );
    bus_write(&bus, INTERRUPT_FLAG_ADDRESS, INTERRUPT_TIMER);

    assert(cpu_step(&cpu) == 4);
    timer_step(&timer, 4);
    assert(cpu.halted);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);

    assert(cpu_step(&cpu) == 4);
    timer_step(&timer, 4);
    assert(cpu.halted);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);

    assert(cpu_step(&cpu) == 4);
    timer_step(&timer, 4);
    assert(cpu.halted);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 0);

    assert(cpu_step(&cpu) == 4);
    timer_step(&timer, 4);
    assert(cpu.halted);
    assert(bus_read(&bus, TIMER_TIMA_ADDRESS) == 1);
    assert(
        (interrupts.interrupt_flag & INTERRUPT_TIMER) != 0
    );

    cleanup_timer(&cartridge);
}

int main(void)
{
    test_registers();
    test_disabled_timer();
    test_frequencies();
    test_overflow_and_interrupt_request();
    test_reload_window_writes_and_repeated_overflow();
    test_div_and_tac_edges();
    test_timer_during_halt_and_pending_if();

    printf("Timer tests passed!\n");
    return 0;
}
