/*
 * Run loop: cycle budgets, stall detection, fault reporting and serial
 * output through the whole machine.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/emulator_internal.h"
#include "test_util.h"

enum {
    ROM_SIZE = 0x8000
};

static const char *rom_path = "test_emulator_run_rom.gb";

typedef struct Program {
    uint8_t rom[ROM_SIZE];
} Program;

static void place(
    Program *program,
    uint16_t address,
    const uint8_t *bytes,
    size_t size
)
{
    assert(address + size <= ROM_SIZE);
    memcpy(program->rom + address, bytes, size);
}

static Emulator *load(const Program *program)
{
    Emulator *emulator = emulator_create();

    assert(emulator != NULL);

    test_write_file(rom_path, program->rom, ROM_SIZE);
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);

    return emulator;
}

/* Keep the three picture consumers on the tested frame timing interface. */
static void test_frame_callers(void)
{
    const char *paths[] = {
        "frontend/sdl_main.c", "tools/frame_dump.c", "tests/test_acid2.c"
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *file = fopen(paths[i], "r");
        assert(file != NULL);
        char source[32768];
        size_t size = fread(source, 1, sizeof(source) - 1, file);
        assert(!ferror(file) && feof(file));
        source[size] = '\0';
        assert(fclose(file) == 0);
        assert(strstr(source, "emulator_run_frame(emulator)") != NULL);
        assert(strstr(source, "emulator_run_cycles(") == NULL);
    }
}

static void test_frame_timing(void)
{
    Program program = {0};
    const uint8_t spin[] = { 0x18, 0xFE };
    place(&program, 0x0100, spin, sizeof(spin));
    Emulator *emulator = load(&program);

    /* Start partway through a visible frame, as every caller can. */
    assert(emulator_run_cycles(emulator, 12348) == EMULATOR_OK);
    for (unsigned i = 0; i < 6; i++) {
        uint64_t frame = emulator_frame_count(emulator);
        uint64_t before = emulator_cycles(emulator);
        assert(emulator_run_frame(emulator) == EMULATOR_OK);
        assert(emulator_frame_count(emulator) == frame + 1);
        assert(emulator->ppu.mode == PPU_MODE_VBLANK);
        assert(emulator->ppu.ly == 144);
        assert(emulator->ppu.line_dot < 12);
        if (i != 0) {
            assert(emulator_cycles(emulator) - before == 70224);
        }
    }
    bus_write(&emulator->bus, PPU_LCDC_ADDRESS, 0);
    uint64_t before = emulator_cycles(emulator);
    uint64_t frame = emulator_frame_count(emulator);
    assert(emulator_run_frame(emulator) == EMULATOR_OK);
    assert(emulator_cycles(emulator) - before == 70224);
    assert(emulator_frame_count(emulator) == frame);
    emulator_stop(emulator);
    before = emulator_cycles(emulator);
    assert(emulator_run_frame(emulator) == EMULATOR_OK);
    assert(emulator_cycles(emulator) == before);
    assert(!emulator_is_running(emulator));
    emulator_destroy(emulator);
    assert(emulator_run_frame(NULL) == EMULATOR_ERROR_INVALID_ARGUMENT);
    emulator = emulator_create();
    assert(emulator != NULL);
    assert(emulator_run_frame(emulator) == EMULATOR_ERROR_NO_ROM);
    emulator_destroy(emulator);
}

static void test_cycle_budget(void)
{
    Program program = {0};
    const uint8_t spin[] = { 0x18, 0xFE }; /* JR -2 */

    place(&program, 0x0100, spin, sizeof(spin));

    Emulator *emulator = load(&program);

    assert(emulator_cycles(emulator) == 0);
    assert(emulator_run_cycles(emulator, 1000) == EMULATOR_OK);
    assert(!emulator_is_running(emulator));

    /* The budget can be overshot by less than one instruction (12 cycles). */
    assert(emulator_cycles(emulator) >= 1000);
    assert(emulator_cycles(emulator) < 1012);

    /* The budget counts from the call, not from the ROM load. */
    uint64_t before = emulator_cycles(emulator);

    assert(emulator_run_cycles(emulator, 500) == EMULATOR_OK);
    assert(emulator_cycles(emulator) >= before + 500);
    assert(emulator_cycles(emulator) < before + 512);

    /* A zero budget runs nothing. */
    uint64_t now = emulator_cycles(emulator);

    assert(emulator_run_cycles(emulator, 0) == EMULATOR_OK);
    assert(emulator_cycles(emulator) == now);

    /* Loading a ROM starts a new machine, including the cycle counter. */
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);
    assert(emulator_cycles(emulator) == 0);

    emulator_destroy(emulator);
}

/*
 * A stop request can arrive just before the run loop starts, for example
 * from a signal handler or another thread. It must not be lost.
 */
static void test_stop_before_run_is_not_lost(void)
{
    Program program = {0};
    const uint8_t spin[] = { 0x18, 0xFE }; /* JR -2 */

    place(&program, 0x0100, spin, sizeof(spin));

    Emulator *emulator = load(&program);

    emulator_stop(emulator);

    assert(emulator_run_cycles(emulator, 1000) == EMULATOR_OK);
    assert(emulator_cycles(emulator) == 0);
    assert(!emulator_is_running(emulator));

    /* The run consumed the request, so the next one runs normally. */
    assert(emulator_run_cycles(emulator, 1000) == EMULATOR_OK);
    assert(emulator_cycles(emulator) >= 1000);

    /* Loading a ROM starts a new machine and drops a pending request. */
    emulator_stop(emulator);
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);
    assert(emulator_run_cycles(emulator, 1000) == EMULATOR_OK);
    assert(emulator_cycles(emulator) >= 1000);

    emulator_destroy(emulator);
}

static void test_run_with_invalid_state(void)
{
    assert(emulator_run(NULL) == EMULATOR_ERROR_INVALID_ARGUMENT);
    assert(emulator_step(NULL) == EMULATOR_ERROR_INVALID_ARGUMENT);
    assert(emulator_load_rom(NULL, "x") == EMULATOR_ERROR_INVALID_ARGUMENT);

    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    assert(emulator_run_cycles(emulator, 100) == EMULATOR_ERROR_NO_ROM);
    assert(emulator_step(emulator) == EMULATOR_ERROR_NO_ROM);
    assert(emulator_load_rom(emulator, "roms/does-not-exist.gb")
           == EMULATOR_ERROR_ROM_LOAD_FAILED);
    assert(!emulator_is_stalled(emulator));

    emulator_destroy(emulator);
}

static void test_halt_without_wakeup_stalls(void)
{
    Program program = {0};
    const uint8_t halt[] = { 0x76 };

    place(&program, 0x0100, halt, sizeof(halt));

    Emulator *emulator = load(&program);

    assert(!emulator_is_stalled(emulator));

    /* IE = 0: nothing can ever end this HALT, so the loop must not spin. */
    assert(emulator_run(emulator) == EMULATOR_STALLED);
    assert(emulator_is_stalled(emulator));
    assert(emulator_cycles(emulator) < 100);

    /* Stepping by hand is still allowed. */
    assert(emulator_step(emulator) == EMULATOR_OK);

    emulator_destroy(emulator);
}

static void test_halt_with_wakeup_source_runs_on(void)
{
    Program program = {0};

    /*
     * LD A,04 ; LDH [FF],A     IE = timer
     * LD A,05 ; LDH [07],A     TAC = enabled, 262144 Hz
     * HALT ; JR -2
     *
     * IME stays off, so each timer overflow wakes the HALT without being
     * serviced, and the loop just keeps going.
     */
    const uint8_t code[] = {
        0x3E, 0x04, 0xE0, 0xFF,
        0x3E, 0x05, 0xE0, 0x07,
        0x76,
        0x18, 0xFE
    };

    place(&program, 0x0100, code, sizeof(code));

    Emulator *emulator = load(&program);

    assert(emulator_run_cycles(emulator, 50000) == EMULATOR_OK);
    assert(!emulator_is_stalled(emulator));
    assert(emulator_cycles(emulator) >= 50000);

    emulator_destroy(emulator);
}

static void test_illegal_opcode_fault(void)
{
    Program program = {0};
    const uint8_t code[] = { 0x00, 0x00, 0xDD };

    place(&program, 0x0100, code, sizeof(code));

    Emulator *emulator = load(&program);
    EmulatorFault fault;

    assert(!emulator_get_fault(emulator, &fault));
    assert(emulator_run(emulator) == EMULATOR_ERROR_ILLEGAL_OPCODE);
    assert(emulator_get_fault(emulator, &fault));

    /* The fault names the instruction itself, not the byte after it. */
    assert(fault.pc == 0x0102);
    assert(fault.opcode == 0xDD);

    /* The faulting opcode consumed no time. */
    assert(emulator_cycles(emulator) == 8);

    assert(!emulator_get_fault(NULL, &fault));
    assert(!emulator_get_fault(emulator, NULL));

    /* Reloading clears the fault. */
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);
    assert(!emulator_get_fault(emulator, &fault));

    emulator_destroy(emulator);
}

typedef struct SerialLog {
    Emulator *emulator;
    char text[16];
    size_t length;
    char stop_after;
} SerialLog;

static void log_serial(void *context, uint8_t byte)
{
    SerialLog *log = context;

    assert(log->length + 1 < sizeof(log->text));
    log->text[log->length++] = (char)byte;
    log->text[log->length] = '\0';

    if ((char)byte == log->stop_after) {
        emulator_stop(log->emulator);
    }
}

static void test_serial_output_and_stop(void)
{
    Program program = {0};

    /* Sends "Hi!" one byte at a time, then spins. */
    const uint8_t code[] = {
        0x3E, 'H', 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
        0x3E, 'i', 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
        0x3E, '!', 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
        0x18, 0xFE
    };

    place(&program, 0x0100, code, sizeof(code));

    Emulator *emulator = load(&program);
    SerialLog log = { .emulator = emulator, .stop_after = '!' };

    emulator_set_serial_output(emulator, log_serial, &log);

    /* The callback stops the run long before the budget is used up. */
    assert(emulator_run_cycles(emulator, 10000000) == EMULATOR_OK);
    assert(strcmp(log.text, "Hi!") == 0);
    assert(emulator_cycles(emulator) < 1000);

    /* The callback survives loading another ROM. */
    log.length = 0;
    log.text[0] = '\0';
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);
    assert(emulator_run(emulator) == EMULATOR_OK);
    assert(strcmp(log.text, "Hi!") == 0);

    emulator_destroy(emulator);
}

static void test_serial_interrupt_end_to_end(void)
{
    Program program = {0};

    /*
     * IE = serial, EI, start a transfer, HALT. The serial interrupt fires
     * 4096 cycles later, wakes the HALT and jumps to 0x0058.
     */
    const uint8_t main_code[] = {
        0x3E, 0x08, 0xE0, 0xFF,
        0xFB,
        0x3E, 0x81, 0xE0, 0x02,
        0x76,
        0x18, 0xFE
    };
    const uint8_t handler[] = {
        0x06, 0x77,       /* LD B,0x77 */
        0x18, 0xFE        /* JR -2 */
    };

    place(&program, 0x0100, main_code, sizeof(main_code));
    place(&program, 0x0058, handler, sizeof(handler));

    Emulator *emulator = load(&program);

    assert(emulator_run_cycles(emulator, 3000) == EMULATOR_OK);
    assert(emulator->cpu.registers.b != 0x77);
    assert(emulator->cpu.halted);

    assert(emulator_run_cycles(emulator, 3000) == EMULATOR_OK);
    assert(emulator->cpu.registers.b == 0x77);
    assert(emulator->cpu.registers.pc == 0x005A);
    assert((emulator->interrupts.interrupt_flag & INTERRUPT_SERIAL) == 0);

    emulator_destroy(emulator);
}

/*
 * A DIV write clocks serial and the APU by the end of the instruction that
 * makes it: the timer records the counter bits that fell, and the tick of
 * the same M-cycle delivers them. Each check runs one LDH (04),A, which
 * writes in its third M-cycle, so 8 cycles after the counter value the test
 * sets before it. The counter is only set between instructions, when no
 * edge is pending.
 */
static Emulator *load_div_writes(void)
{
    static const uint8_t write_div[] = { 0xE0, 0x04 };   /* LDH (04),A */
    Program program = {0};

    for (uint16_t pc = 0x0100; pc < 0x0100 + 2 * 32; pc += 2) {
        place(&program, pc, write_div, sizeof(write_div));
    }

    return load(&program);
}

static void test_div_write_clocks_serial(void)
{
    Emulator *emulator = load_div_writes();

    bus_write(&emulator->bus, SERIAL_SC_ADDRESS, 0x81);
    for (unsigned i = 0; i < 8; i++) {
        /* 0000 + 8: bit 8 is low at the write, so nothing shifts. */
        emulator->timer.divider = 0x0000;
        assert(emulator_step(emulator) == EMULATOR_OK);
        assert(emulator->serial.bits_remaining == 8 - i);

        /* 0100 + 8: bit 8 is high at the write, so exactly one bit shifts. */
        emulator->timer.divider = 0x0100;
        assert(emulator_step(emulator) == EMULATOR_OK);
        assert(emulator->timer.divider == 4);
        assert(emulator->serial.bits_remaining == 7 - i);
        assert(emulator->serial.transferring == (i != 7));
    }
    assert(bus_read(&emulator->bus, SERIAL_SB_ADDRESS) == 0xFF);
    assert((emulator->interrupts.interrupt_flag & INTERRUPT_SERIAL) != 0);

    /* With the external clock there is no transfer to clock. */
    bus_write(&emulator->bus, SERIAL_SC_ADDRESS, 0x80);
    emulator->timer.divider = 0x0100;
    assert(emulator_step(emulator) == EMULATOR_OK);
    assert(emulator->serial.bits_remaining == 8);
    emulator_destroy(emulator);
}

static void test_div_write_clocks_the_apu(void)
{
    for (unsigned bit_12_high = 0; bit_12_high <= 1; bit_12_high++) {
        Emulator *emulator = load_div_writes();
        Bus *bus = &emulator->bus;

        /* Power-cycling puts the frame sequencer at step 0, whose tick
         * clocks length. Channel 2 then plays with a length of 1. */
        bus_write(bus, APU_NR52_ADDRESS, 0x00);
        bus_write(bus, APU_NR52_ADDRESS, 0x80);
        bus_write(bus, APU_NR22_ADDRESS, 0xF0);
        bus_write(bus, APU_NR21_ADDRESS, 0x3F);
        bus_write(bus, APU_NR24_ADDRESS, 0xC0);
        assert((bus_read(bus, APU_NR52_ADDRESS) & 0x02) != 0);

        /* 1000 + 8 has bit 12 set at the write and 0000 + 8 does not. */
        emulator->timer.divider = bit_12_high ? 0x1000 : 0x0000;
        assert(emulator_step(emulator) == EMULATOR_OK);
        assert(emulator->timer.divider == 4);

        bool playing = (bus_read(bus, APU_NR52_ADDRESS) & 0x02) != 0;
        assert(playing == !bit_12_high);
        emulator_destroy(emulator);
    }
}

/*
 * A fall from counting reaches serial and the APU in the M-cycle it happens
 * in, not the next one: the tick advances the timer before it takes the
 * edges. A NOP is one M-cycle, so with the counter 4 short of 0200 (or
 * 2000) the NOP's own tick is the one in which bit 8 (or 12) falls.
 */
static void test_counting_edges_arrive_in_their_m_cycle(void)
{
    Program program = {0};                         /* all NOPs */
    Emulator *emulator = load(&program);

    bus_write(&emulator->bus, SERIAL_SC_ADDRESS, 0x81);
    emulator->timer.divider = 0x01F8;
    assert(emulator_step(emulator) == EMULATOR_OK);   /* 01F8 -> 01FC */
    assert(emulator->serial.bits_remaining == 8);
    assert(emulator_step(emulator) == EMULATOR_OK);   /* 01FC -> 0200 */
    assert(emulator->timer.divider == 0x0200);
    assert(emulator->serial.bits_remaining == 7);
    emulator_destroy(emulator);

    emulator = load(&program);
    Bus *bus = &emulator->bus;
    bus_write(bus, APU_NR52_ADDRESS, 0x00);       /* sequencer at step 0 */
    bus_write(bus, APU_NR52_ADDRESS, 0x80);
    bus_write(bus, APU_NR22_ADDRESS, 0xF0);
    bus_write(bus, APU_NR21_ADDRESS, 0x3F);       /* length 1 */
    bus_write(bus, APU_NR24_ADDRESS, 0xC0);
    emulator->timer.divider = 0x1FF8;
    assert(emulator_step(emulator) == EMULATOR_OK);   /* 1FF8 -> 1FFC */
    assert((bus_read(bus, APU_NR52_ADDRESS) & 0x02) != 0);
    assert(emulator_step(emulator) == EMULATOR_OK);   /* 1FFC -> 2000 */
    assert(emulator->timer.divider == 0x2000);
    assert((bus_read(bus, APU_NR52_ADDRESS) & 0x02) == 0);
    emulator_destroy(emulator);
}

static void test_rom_load_failures(void)
{
    Program program = {0};
    const uint8_t nop = 0x00;
    uint8_t type = 0;

    place(&program, 0x0100, &nop, sizeof(nop));

    Emulator *emulator = load(&program);

    assert(!emulator_get_unsupported_cartridge_type(emulator, &type));

    /* 0x20 is MBC6, which is not implemented. */
    program.rom[CARTRIDGE_HEADER_TYPE] = 0x20;
    test_write_file(rom_path, program.rom, ROM_SIZE);

    assert(emulator_load_rom(emulator, rom_path) ==
           EMULATOR_ERROR_UNSUPPORTED_CARTRIDGE);
    assert(emulator_get_unsupported_cartridge_type(emulator, &type));
    assert(type == 0x20);
    assert(!emulator_get_unsupported_cartridge_type(emulator, NULL));

    /* The previously loaded ROM keeps running. */
    assert(emulator_step(emulator) == EMULATOR_OK);

    /* A file that cannot be read is a different failure and clears it. */
    assert(emulator_load_rom(emulator, "roms/does-not-exist.gb") ==
           EMULATOR_ERROR_ROM_LOAD_FAILED);
    assert(!emulator_get_unsupported_cartridge_type(emulator, &type));

    const uint8_t no_bytes[1] = {0};

    test_write_file(rom_path, no_bytes, 0);
    assert(emulator_load_rom(emulator, rom_path) ==
           EMULATOR_ERROR_ROM_LOAD_FAILED);

    /* A successful load clears the unsupported type too. */
    program.rom[CARTRIDGE_HEADER_TYPE] = 0x20;
    test_write_file(rom_path, program.rom, ROM_SIZE);
    assert(emulator_load_rom(emulator, rom_path) ==
           EMULATOR_ERROR_UNSUPPORTED_CARTRIDGE);

    program.rom[CARTRIDGE_HEADER_TYPE] = 0x00;
    test_write_file(rom_path, program.rom, ROM_SIZE);
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);
    assert(!emulator_get_unsupported_cartridge_type(emulator, &type));

    emulator_destroy(emulator);
}

static void test_status_strings(void)
{
    assert(strcmp(emulator_status_string(EMULATOR_OK), "ok") == 0);
    assert(emulator_status_string(EMULATOR_STALLED) != NULL);
    assert(emulator_status_string(EMULATOR_ERROR_ILLEGAL_OPCODE) != NULL);
    assert(emulator_status_string((EmulatorStatus)99) != NULL);

    /* Every defined status has its own text, not the fallback. */
    const char *unknown = emulator_status_string((EmulatorStatus)99);

    for (int status = EMULATOR_OK; status <= EMULATOR_ERROR_SAVE_SIZE; status++) {
        assert(strcmp(emulator_status_string((EmulatorStatus)status),
                      unknown) != 0);
    }
}

int main(void)
{
    test_frame_callers();
    test_frame_timing();
    test_cycle_budget();
    test_stop_before_run_is_not_lost();
    test_run_with_invalid_state();
    test_rom_load_failures();
    test_halt_without_wakeup_stalls();
    test_halt_with_wakeup_source_runs_on();
    test_illegal_opcode_fault();
    test_serial_output_and_stop();
    test_serial_interrupt_end_to_end();
    test_div_write_clocks_serial();
    test_div_write_clocks_the_apu();
    test_counting_edges_arrive_in_their_m_cycle();
    test_status_strings();

    assert(remove(rom_path) == 0);

    printf("Emulator run loop tests passed!\n");

    return 0;
}
