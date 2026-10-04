/*
 * Cross-checks the CPU against opcodes.json: every one of the 512 opcodes
 * must take the documented number of cycles, consume the documented number
 * of bytes, and consume its time as whole M-cycles ticked to the machine.
 * Also checks the ordering of bus accesses and ticks inside an instruction.
 */


#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_util.h"

/*
 * The expected values come from opcodes.json, and asserting on them is
 * exactly what this test is for, so the analyzer's check against
 * asserting on data read from a file does not apply here.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wanalyzer-tainted-assertion"
#endif


enum {
    PROGRAM_ADDRESS = 0x0100,
    OPCODE_COUNT = 256
};

typedef struct OpcodeInfo {
    bool present;
    char mnemonic[24];
    int bytes;
    int cycles_taken;
    int cycles_not_taken;
} OpcodeInfo;

typedef struct OpcodeTable {
    OpcodeInfo unprefixed[OPCODE_COUNT];
    OpcodeInfo cbprefixed[OPCODE_COUNT];
} OpcodeTable;

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");

    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);

    long size = ftell(file);

    assert(size > 0);
    rewind(file);

    char *text = malloc((size_t)size + 1);

    assert(text != NULL);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    text[size] = '\0';

    fclose(file);

    return text;
}

/*
 * opcodes.json is pretty-printed with one entry per opcode:
 *
 *     "0xC4": {
 *       "mnemonic": "CALL",
 *       "bytes": 3,
 *       "cycles": [24, 12],
 *
 * so the entries of a section can be read by scanning for the fields.
 */
static void parse_section(
    const char *begin,
    const char *end,
    OpcodeInfo *table
)
{
    const char *cursor = begin;

    while ((cursor = strstr(cursor, "\n    \"0x")) != NULL && cursor < end) {
        cursor += strlen("\n    \"0x");

        char *after = NULL;
        unsigned long opcode = strtoul(cursor, &after, 16);

        assert(opcode < OPCODE_COUNT);

        const char *mnemonic = strstr(cursor, "\"mnemonic\": \"");
        const char *bytes = strstr(cursor, "\"bytes\": ");
        const char *cycles = strstr(cursor, "\"cycles\": [");

        assert(mnemonic != NULL && bytes != NULL && cycles != NULL);

        OpcodeInfo *info = &table[opcode];

        assert(!info->present);

        mnemonic += strlen("\"mnemonic\": \"");
        size_t length = strcspn(mnemonic, "\"");

        assert(length < sizeof(info->mnemonic));
        memcpy(info->mnemonic, mnemonic, length);
        info->mnemonic[length] = '\0';

        info->bytes = atoi(bytes + strlen("\"bytes\": "));

        char *cycles_end = NULL;

        info->cycles_taken =
            (int)strtol(cycles + strlen("\"cycles\": ["), &cycles_end, 10);
        info->cycles_not_taken = info->cycles_taken;

        if (*cycles_end == ',') {
            info->cycles_not_taken = (int)strtol(cycles_end + 1, NULL, 10);
        }

        info->present = true;
    }
}

static void load_opcode_table(OpcodeTable *table)
{
    char *text = read_file("opcodes.json");
    const char *unprefixed = strstr(text, "\"unprefixed\"");
    const char *cbprefixed = strstr(text, "\"cbprefixed\"");

    assert(unprefixed != NULL && cbprefixed != NULL);
    assert(unprefixed < cbprefixed);

    memset(table, 0, sizeof(*table));

    parse_section(unprefixed, cbprefixed, table->unprefixed);
    parse_section(cbprefixed, text + strlen(text), table->cbprefixed);

    for (int opcode = 0; opcode < OPCODE_COUNT; opcode++) {
        assert(table->unprefixed[opcode].present);
        assert(table->cbprefixed[opcode].present);
    }

    free(text);
}

static bool is_branch(const char *mnemonic)
{
    return strcmp(mnemonic, "JP") == 0 ||
           strcmp(mnemonic, "JR") == 0 ||
           strcmp(mnemonic, "CALL") == 0 ||
           strcmp(mnemonic, "RET") == 0 ||
           strcmp(mnemonic, "RETI") == 0 ||
           strcmp(mnemonic, "RST") == 0;
}

/*
 * Registers chosen so that every memory operand lands in RAM: BC, DE and
 * HL point into work RAM, C into high RAM, SP into work RAM, and the two
 * operand bytes form the address 0xC080 (also a valid a8 and e8).
 */
static void prepare_cpu(TestMachine *machine, uint8_t f)
{
    test_machine_reset_cpu(machine);

    machine->cpu.registers.b = 0xC0;
    machine->cpu.registers.c = 0x80;
    machine->cpu.registers.d = 0xC0;
    machine->cpu.registers.e = 0x80;
    machine->cpu.registers.h = 0xC0;
    machine->cpu.registers.l = 0x80;
    machine->cpu.registers.sp = 0xC100;
    machine->cpu.registers.f = f;
}

/* F value making condition cc (0=NZ 1=Z 2=NC 3=C) true or false. */
static uint8_t flags_for_condition(unsigned cc, bool taken)
{
    switch (cc) {
        case 0:
            return taken ? 0x00 : FLAG_Z;

        case 1:
            return taken ? FLAG_Z : 0x00;

        case 2:
            return taken ? 0x00 : FLAG_C;

        default:
            return taken ? FLAG_C : 0x00;
    }
}

static void check_instruction(
    TestMachine *machine,
    const OpcodeInfo *info,
    const uint8_t *program,
    size_t program_size,
    uint8_t flags,
    int expected_cycles,
    bool expect_sequential_pc
)
{
    test_machine_load(machine, PROGRAM_ADDRESS, program, program_size);
    prepare_cpu(machine, flags);

    CpuCycles cycles = cpu_step(&machine->cpu);

    if ((int)cycles != expected_cycles) {
        printf(
            "%s (opcode 0x%02X): took %d cycles, opcodes.json says %d\n",
            info->mnemonic, program[0], (int)cycles, expected_cycles
        );
    }

    assert((int)cycles == expected_cycles);
    assert(machine->cpu.step_status != CPU_STEP_UNIMPLEMENTED_OPCODE);

    /* All the time is consumed as M-cycles ticked to the machine. */
    assert(machine->ticked_cycles == cycles);
    assert(machine->tick_count * CYCLES_PER_MCYCLE == (unsigned)cycles);

    if (expect_sequential_pc) {
        assert(machine->cpu.registers.pc == PROGRAM_ADDRESS + info->bytes);
    }
}

static void test_unprefixed_opcodes(const OpcodeTable *table)
{
    TestMachine machine;
    int illegal = 0;

    test_machine_init(&machine);

    for (unsigned opcode = 0; opcode < OPCODE_COUNT; opcode++) {
        const OpcodeInfo *info = &table->unprefixed[opcode];
        const uint8_t program[] = {
            (uint8_t)opcode, 0x80, 0xC0
        };

        if (strncmp(info->mnemonic, "ILLEGAL", 7) == 0) {
            illegal++;

            test_machine_load(
                &machine,
                PROGRAM_ADDRESS,
                program,
                sizeof(program)
            );
            prepare_cpu(&machine, 0);

            /* Undefined opcodes take no time and leave PC on the opcode. */
            assert(cpu_step(&machine.cpu) == 0);
            assert(machine.cpu.step_status == CPU_STEP_UNIMPLEMENTED_OPCODE);
            assert(machine.cpu.registers.pc == PROGRAM_ADDRESS);
            assert(machine.cpu.fault_pc == PROGRAM_ADDRESS);
            assert(machine.cpu.fault_opcode == (uint8_t)opcode);
            assert(machine.ticked_cycles == 0);
            continue;
        }

        if (opcode == 0xCB) {
            /* The prefix is timed together with the opcode after it. */
            continue;
        }

        bool conditional = info->cycles_taken != info->cycles_not_taken;
        unsigned cc = (opcode >> 3) & 0x03;

        if (conditional) {
            check_instruction(
                &machine, info, program, sizeof(program),
                flags_for_condition(cc, true),
                info->cycles_taken,
                false
            );
            check_instruction(
                &machine, info, program, sizeof(program),
                flags_for_condition(cc, false),
                info->cycles_not_taken,
                true
            );
        } else {
            check_instruction(
                &machine, info, program, sizeof(program),
                0x00,
                info->cycles_taken,
                !is_branch(info->mnemonic)
            );
        }
    }

    assert(illegal == 11);

    test_machine_destroy(&machine);
}

static void test_cb_prefixed_opcodes(const OpcodeTable *table)
{
    TestMachine machine;

    test_machine_init(&machine);

    for (unsigned opcode = 0; opcode < OPCODE_COUNT; opcode++) {
        const OpcodeInfo *info = &table->cbprefixed[opcode];
        const uint8_t program[] = {
            0xCB, (uint8_t)opcode
        };

        assert(info->cycles_taken == info->cycles_not_taken);
        assert(info->bytes == 2);

        check_instruction(
            &machine, info, program, sizeof(program),
            0x00,
            info->cycles_taken,
            true
        );
    }

    test_machine_destroy(&machine);
}

/*
 * A bus access observes the machine as it is after the earlier M-cycles
 * of the same instruction, and its own M-cycle is ticked after it.
 */
static void test_access_ordering(void)
{
    TestMachine machine;

    test_machine_init(&machine);

    /*
     * LDH A,[DIV]: two fetch M-cycles pass before DIV is read, so a
     * divider that is 8 cycles short of 0x0100 already reads as 1.
     */
    const uint8_t read_div[] = { 0xF0, 0x04 };

    test_machine_load(&machine, PROGRAM_ADDRESS, read_div, sizeof(read_div));
    test_machine_reset_cpu(&machine);
    machine.timer.divider = 0x00F8;

    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.cpu.registers.a == 0x01);

    /*
     * LDH [DIV],A: DIV is cleared by the third M-cycle, whose own tick
     * comes after the write, so 4 cycles of the new divider have elapsed.
     */
    const uint8_t write_div[] = { 0xE0, 0x04 };

    test_machine_load(&machine, PROGRAM_ADDRESS, write_div, sizeof(write_div));
    test_machine_reset_cpu(&machine);
    machine.timer.divider = 0x1234;

    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.timer.divider == 4);

    /*
     * INC [HL] on TIMA reads it in one M-cycle and writes it in the next.
     * With a 16-cycle clock, a falling edge (divider 0x0F -> 0x10) during
     * the tick after the read increments TIMA, but the write that follows
     * overwrites it. Ticking only after the whole instruction would let
     * the increment land last and leave 0x12.
     */
    const uint8_t inc_hl[] = { 0x34 };

    test_machine_load(&machine, PROGRAM_ADDRESS, inc_hl, sizeof(inc_hl));
    test_machine_reset_cpu(&machine);
    machine.cpu.registers.h = 0xFF;
    machine.cpu.registers.l = 0x05;
    machine.timer.tac = TIMER_TAC_ENABLE | 0x01;
    machine.timer.tima = 0x10;
    machine.timer.divider = 0x0008;

    assert(cpu_step(&machine.cpu) == 12);
    assert(machine.timer.tima == 0x11);

    test_machine_destroy(&machine);
}

int main(void)
{
    OpcodeTable *table = malloc(sizeof(OpcodeTable));

    assert(table != NULL);

    load_opcode_table(table);

    test_unprefixed_opcodes(table);
    test_cb_prefixed_opcodes(table);
    test_access_ordering();

    free(table);

    printf("Opcode timing tests passed!\n");

    return 0;
}
