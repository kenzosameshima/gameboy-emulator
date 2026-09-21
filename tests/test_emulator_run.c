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

static void test_status_strings(void)
{
    assert(strcmp(emulator_status_string(EMULATOR_OK), "ok") == 0);
    assert(emulator_status_string(EMULATOR_STALLED) != NULL);
    assert(emulator_status_string(EMULATOR_ERROR_ILLEGAL_OPCODE) != NULL);
    assert(emulator_status_string((EmulatorStatus)99) != NULL);
}

int main(void)
{
    test_cycle_budget();
    test_run_with_invalid_state();
    test_halt_without_wakeup_stalls();
    test_halt_with_wakeup_source_runs_on();
    test_illegal_opcode_fault();
    test_serial_output_and_stop();
    test_serial_interrupt_end_to_end();
    test_status_strings();

    assert(remove(rom_path) == 0);

    printf("Emulator run loop tests passed!\n");

    return 0;
}
