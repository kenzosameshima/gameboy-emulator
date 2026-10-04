/*
 * Register and memory wiring of the basic CPU instructions: INC/DEC r8 and
 * r16, LD r8,r8 and the HALT/illegal-opcode control states. The flag
 * arithmetic itself is covered exhaustively by test_cpu_alu_exhaustive.
 *
 * Every instruction runs through cpu_step() on the shared TestMachine, and
 * the whole register file is compared afterwards, so an instruction that
 * touches the wrong register fails.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_util.h"

enum {
    PROGRAM_ADDRESS = 0x0100,
    HL_ADDRESS = 0xC000,
    HL_INDEX = 6
};

/* r8 operand encoding: 0=B 1=C 2=D 3=E 4=H 5=L 6=[HL] 7=A. */
static uint8_t *r8_register(Registers *registers, unsigned index)
{
    switch (index) {
        case 0: return &registers->b;
        case 1: return &registers->c;
        case 2: return &registers->d;
        case 3: return &registers->e;
        case 4: return &registers->h;
        case 5: return &registers->l;
        case 7: return &registers->a;
        default:
            assert(false && "[HL] is memory, not a register");
            return NULL;
    }
}

static void load_opcode(TestMachine *m, uint8_t opcode)
{
    test_machine_load(m, PROGRAM_ADDRESS, &opcode, 1);
}

/* Resets the CPU to distinct register values, with HL in work RAM. */
static void reset_known(TestMachine *m, uint8_t flags)
{
    Registers *r = &m->cpu.registers;

    test_machine_reset_cpu(m);

    r->a = 0x87;
    r->b = 0x10;
    r->c = 0x21;
    r->d = 0x32;
    r->e = 0x43;
    r->h = HL_ADDRESS >> 8;
    r->l = HL_ADDRESS & 0xFF;
    r->f = flags;
    r->sp = 0xFFF0;
}

static void assert_registers_equal(const Registers *got, const Registers *want)
{
    assert(memcmp(got, want, sizeof(Registers)) == 0);
}

/* INC r8 / DEC r8 for every operand, including [HL]. */
static void test_inc_dec_r8_wiring(TestMachine *m)
{
    static const uint8_t values[] = { 0x00, 0x0F, 0x10, 0x80, 0xFF };

    for (unsigned dec = 0; dec < 2; dec++) {
        for (unsigned index = 0; index < 8; index++) {
            load_opcode(m, (uint8_t)(0x04 | (index << 3) | (dec ? 1 : 0)));

            for (size_t i = 0; i < sizeof(values); i++) {
                for (unsigned carry = 0; carry < 2; carry++) {
                    const uint8_t value = values[i];

                    reset_known(m, carry ? FLAG_C : 0);

                    if (index == HL_INDEX) {
                        memory_write(&m->memory, HL_ADDRESS, value);
                    } else {
                        *r8_register(&m->cpu.registers, index) = value;
                    }

                    Registers expected = m->cpu.registers;
                    const uint8_t result =
                        (uint8_t)(dec ? value - 1 : value + 1);

                    expected.pc = PROGRAM_ADDRESS + 1;
                    expected.f = (uint8_t)(
                        (result == 0 ? FLAG_Z : 0) |
                        (dec ? FLAG_N : 0) |
                        (((value ^ 1 ^ result) & 0x10) != 0 ? FLAG_H : 0) |
                        (carry ? FLAG_C : 0)
                    );

                    if (index != HL_INDEX) {
                        *r8_register(&expected, index) = result;
                    }

                    CpuCycles cycles = cpu_step(&m->cpu);

                    assert(cycles == (index == HL_INDEX ? 12 : 4));
                    assert_registers_equal(&m->cpu.registers, &expected);

                    if (index == HL_INDEX) {
                        assert(memory_read(&m->memory, HL_ADDRESS) == result);
                    }
                }
            }
        }
    }
}

static void store_r16(Registers *registers, unsigned pair, uint16_t value)
{
    switch (pair) {
        case 0:
            registers->b = (uint8_t)(value >> 8);
            registers->c = (uint8_t)value;
            break;

        case 1:
            registers->d = (uint8_t)(value >> 8);
            registers->e = (uint8_t)value;
            break;

        case 2:
            registers->h = (uint8_t)(value >> 8);
            registers->l = (uint8_t)value;
            break;

        default:
            registers->sp = value;
            break;
    }
}

/* INC rr / DEC rr: BC, DE, HL, SP. Wraps and never touches the flags. */
static void test_inc_dec_r16(TestMachine *m)
{
    static const struct {
        uint16_t low;  /* the smaller value; INC goes low -> high */
        uint16_t high;
    } cases[] = {
        { 0x12FF, 0x1300 },
        { 0xFFFF, 0x0000 }, /* wraps: INC 0xFFFF = 0x0000 */
        { 0x0000, 0x0001 },
        { 0x7FFF, 0x8000 }
    };

    for (unsigned dec = 0; dec < 2; dec++) {
        for (unsigned pair = 0; pair < 4; pair++) {
            load_opcode(m, (uint8_t)(0x03 | (pair << 4) | (dec ? 8 : 0)));

            for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                const uint16_t before = dec ? cases[i].high : cases[i].low;
                const uint16_t after = dec ? cases[i].low : cases[i].high;

                for (unsigned flags = 0x00; flags <= 0xF0; flags += 0xF0) {
                    reset_known(m, (uint8_t)flags);
                    store_r16(&m->cpu.registers, pair, before);

                    Registers expected = m->cpu.registers;

                    store_r16(&expected, pair, after);
                    expected.pc = PROGRAM_ADDRESS + 1;

                    CpuCycles cycles = cpu_step(&m->cpu);

                    assert(cycles == 8);
                    assert_registers_equal(&m->cpu.registers, &expected);
                }
            }
        }
    }
}

/* LD r8,r8 for every operand pair, including [HL]; flags are untouched. */
static void test_ld_r8_r8(TestMachine *m)
{
    for (unsigned destination = 0; destination < 8; destination++) {
        for (unsigned source = 0; source < 8; source++) {
            if (destination == HL_INDEX && source == HL_INDEX) {
                continue; /* 0x76 is HALT. */
            }

            load_opcode(m, (uint8_t)(0x40 | (destination << 3) | source));
            reset_known(m, 0xF0);
            memory_write(&m->memory, HL_ADDRESS, 0x5A);

            Registers expected = m->cpu.registers;
            const uint8_t moved = source == HL_INDEX
                ? 0x5A
                : *r8_register(&expected, source);

            expected.pc = PROGRAM_ADDRESS + 1;

            if (destination != HL_INDEX) {
                *r8_register(&expected, destination) = moved;
            }

            CpuCycles cycles = cpu_step(&m->cpu);

            assert(cycles ==
                   (source == HL_INDEX || destination == HL_INDEX ? 8 : 4));
            assert_registers_equal(&m->cpu.registers, &expected);

            if (destination == HL_INDEX) {
                /* The address is HL as it was before the instruction. */
                assert(memory_read(&m->memory, HL_ADDRESS) == moved);
            }
        }
    }
}

static void test_control_states_and_flags(TestMachine *m)
{
    CPU *cpu = &m->cpu;

    /* Post-boot state. */
    test_machine_reset_cpu(m);
    assert(cpu->registers.f == 0xB0);
    assert(cpu->registers.pc == PROGRAM_ADDRESS);

    /* HALT, then a halted step. */
    load_opcode(m, 0x76);

    assert(cpu_step(cpu) == 4);
    assert(cpu->halted);
    assert(cpu->step_status == CPU_STEP_EXECUTED);
    assert(cpu->registers.pc == PROGRAM_ADDRESS + 1);

    assert(cpu_step(cpu) == 4);
    assert(cpu->halted);
    assert(cpu->step_status == CPU_STEP_HALTED);
    assert(cpu->registers.pc == PROGRAM_ADDRESS + 1);

    /* An undefined opcode consumes no time and leaves PC on it. */
    load_opcode(m, 0xD3);
    test_machine_reset_cpu(m);

    assert(cpu_step(cpu) == 0);
    assert(!cpu->halted);
    assert(cpu->step_status == CPU_STEP_UNIMPLEMENTED_OPCODE);
    assert(cpu->registers.pc == PROGRAM_ADDRESS);
    assert(cpu->fault_pc == PROGRAM_ADDRESS);
    assert(cpu->fault_opcode == 0xD3);

    /* The low nibble of F always reads as zero. */
    load_opcode(m, 0x04); /* INC B */
    test_machine_reset_cpu(m);
    cpu->registers.b = 0x00;
    cpu->registers.f = 0x1F;

    assert(cpu_step(cpu) == 4);
    assert(cpu->registers.f == FLAG_C);
}

int main(void)
{
    TestMachine machine;

    test_machine_init(&machine);

    test_inc_dec_r8_wiring(&machine);
    test_inc_dec_r16(&machine);
    test_ld_r8_r8(&machine);
    test_control_states_and_flags(&machine);

    test_machine_destroy(&machine);

    printf("All CPU tests passed!\n");

    return 0;
}
