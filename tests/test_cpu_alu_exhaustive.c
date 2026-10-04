/*
 * Exhaustive ALU checks, run through cpu_step() on real opcodes.
 *
 * Every 8-bit input combination is compared against an independent model
 * that is deliberately written differently from the implementation: half
 * carries come from the XOR of operands and result, borrows from the sign
 * of a widened difference, and DAA is checked against decimal arithmetic
 * on BCD operands instead of against its own adjustment rules.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_util.h"

enum {
    PROGRAM_ADDRESS = 0x0100,

    Z = FLAG_Z,
    N = FLAG_N,
    H = FLAG_H,
    C = FLAG_C
};

#define EXPECT(condition, ...)                                          \
    do {                                                                \
        if (!(condition)) {                                             \
            fprintf(stderr, "FAILED: %s\n  ", #condition);              \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
            abort();                                                    \
        }                                                               \
    } while (0)

/* Puts `opcode` (and an optional second byte) at the reset vector. */
static void load_program(TestMachine *m, const uint8_t *bytes, size_t size)
{
    test_machine_load(m, PROGRAM_ADDRESS, bytes, size);
}

/* Resets the CPU to PC = 0x0100 with the given A, B and F. */
static void prepare(TestMachine *m, uint8_t a, uint8_t b, uint8_t f)
{
    test_machine_reset_cpu(m);
    m->cpu.registers.a = a;
    m->cpu.registers.b = b;
    m->cpu.registers.f = f;
}

static uint8_t flags_of(bool z, bool n, bool h, bool c)
{
    return (uint8_t)((z ? Z : 0) | (n ? N : 0) | (h ? H : 0) | (c ? C : 0));
}

/* ALU A,B for op = ADD ADC SUB SBC AND XOR OR CP, all 256 * 256 * 2 inputs. */
static void test_alu_a_r8(TestMachine *m)
{
    static const char *const names[] = {
        "ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"
    };

    for (unsigned op = 0; op < 8; op++) {
        const uint8_t opcode = (uint8_t)(0x80 | (op << 3)); /* ... A,B */

        load_program(m, &opcode, 1);

        for (unsigned a = 0; a < 256; a++) {
            for (unsigned b = 0; b < 256; b++) {
                for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
                    prepare(m, (uint8_t)a, (uint8_t)b,
                            carry_in ? C : 0);
                    cpu_step(&m->cpu);

                    int carry = (op == 1 || op == 3) ? (int)carry_in : 0;
                    int wide;
                    uint8_t result;
                    uint8_t flags;

                    switch (op) {
                        case 0:
                        case 1:
                            wide = (int)a + (int)b + carry;
                            result = (uint8_t)wide;
                            flags = flags_of(
                                result == 0, false,
                                ((a ^ b ^ (unsigned)wide) & 0x10) != 0,
                                ((a ^ b ^ (unsigned)wide) & 0x100) != 0
                            );
                            break;

                        case 2:
                        case 3:
                        case 7:
                            wide = (int)a - (int)b - carry;
                            result = (uint8_t)wide;
                            flags = flags_of(
                                result == 0, true,
                                ((a ^ b ^ (unsigned)wide) & 0x10) != 0,
                                wide < 0
                            );
                            break;

                        case 4:
                            result = (uint8_t)(a & b);
                            flags = flags_of(result == 0, false, true, false);
                            break;

                        case 5:
                            result = (uint8_t)(a ^ b);
                            flags = flags_of(result == 0, false, false, false);
                            break;

                        default:
                            result = (uint8_t)(a | b);
                            flags = flags_of(result == 0, false, false, false);
                            break;
                    }

                    if (op == 7) {
                        result = (uint8_t)a; /* CP does not store. */
                    }

                    EXPECT(m->cpu.registers.a == result &&
                               m->cpu.registers.f == flags,
                           "%s A=%02X B=%02X carry=%u: got A=%02X F=%02X, "
                           "want A=%02X F=%02X",
                           names[op], a, b, carry_in, m->cpu.registers.a,
                           m->cpu.registers.f, result, flags);
                }
            }
        }
    }
}

static unsigned bcd_to_decimal(unsigned bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static unsigned decimal_to_bcd(unsigned decimal)
{
    return ((decimal / 10) << 4) | (decimal % 10);
}

/*
 * ADD/ADC/SUB/SBC followed by DAA on every pair of valid BCD operands must
 * give the decimal result, with C as the decimal carry or borrow.
 */
static void test_daa_matches_decimal_arithmetic(TestMachine *m)
{
    for (unsigned subtract = 0; subtract < 2; subtract++) {
        /* ADC A,B / SBC A,B, then DAA. */
        const uint8_t program[] = { subtract ? 0x98 : 0x88, 0x27 };

        load_program(m, program, sizeof(program));

        for (unsigned a = 0; a < 0x100; a++) {
            for (unsigned b = 0; b < 0x100; b++) {
                if ((a & 0x0F) > 9 || (a >> 4) > 9 ||
                    (b & 0x0F) > 9 || (b >> 4) > 9) {
                    continue;
                }

                for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
                    prepare(m, (uint8_t)a, (uint8_t)b, carry_in ? C : 0);
                    cpu_step(&m->cpu);
                    cpu_step(&m->cpu);

                    int decimal = (int)bcd_to_decimal(a);
                    bool carry_out;

                    if (subtract) {
                        decimal -= (int)bcd_to_decimal(b) - 0;
                        decimal -= (int)carry_in;
                        carry_out = decimal < 0;

                        if (carry_out) {
                            decimal += 100;
                        }
                    } else {
                        decimal += (int)bcd_to_decimal(b) + (int)carry_in;
                        carry_out = decimal >= 100;

                        if (carry_out) {
                            decimal -= 100;
                        }
                    }

                    uint8_t result = (uint8_t)decimal_to_bcd((unsigned)decimal);
                    uint8_t flags = flags_of(result == 0, subtract != 0,
                                             false, carry_out);

                    EXPECT(m->cpu.registers.a == result &&
                               m->cpu.registers.f == flags,
                           "%s A=%02X B=%02X carry=%u then DAA: got "
                           "A=%02X F=%02X, want A=%02X F=%02X",
                           subtract ? "SBC" : "ADC", a, b, carry_in,
                           m->cpu.registers.a, m->cpu.registers.f, result,
                           flags);
                }
            }
        }
    }
}

/* INC B and DEC B on every value; C is preserved. */
static void test_inc_dec_r8(TestMachine *m)
{
    for (unsigned dec = 0; dec < 2; dec++) {
        const uint8_t opcode = dec ? 0x05 : 0x04;

        load_program(m, &opcode, 1);

        for (unsigned value = 0; value < 256; value++) {
            for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
                prepare(m, 0, (uint8_t)value, carry_in ? C : 0);
                cpu_step(&m->cpu);

                unsigned wide = dec ? value - 1 : value + 1;
                uint8_t result = (uint8_t)wide;
                uint8_t flags = flags_of(
                    result == 0, dec != 0,
                    ((value ^ 1 ^ wide) & 0x10) != 0, carry_in != 0
                );

                EXPECT(m->cpu.registers.b == result &&
                           m->cpu.registers.f == flags,
                       "%s B=%02X carry=%u: got B=%02X F=%02X, "
                       "want B=%02X F=%02X",
                       dec ? "DEC" : "INC", value, carry_in,
                       m->cpu.registers.b, m->cpu.registers.f, result,
                       flags);
            }
        }
    }
}

/* CB rotates and shifts on B for every value and carry-in. */
static void test_cb_rotate_shift(TestMachine *m)
{
    static const char *const names[] = {
        "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"
    };

    for (unsigned op = 0; op < 8; op++) {
        const uint8_t program[] = { 0xCB, (uint8_t)(op << 3) }; /* op B */

        load_program(m, program, sizeof(program));

        for (unsigned value = 0; value < 256; value++) {
            for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
                prepare(m, 0, (uint8_t)value, carry_in ? C : 0);
                cpu_step(&m->cpu);

                unsigned high = value >> 7;
                unsigned low = value & 1;
                unsigned result;
                bool carry_out;

                switch (op) {
                    case 0:
                        result = (value << 1) | high;
                        carry_out = high != 0;
                        break;

                    case 1:
                        result = (value >> 1) | (low << 7);
                        carry_out = low != 0;
                        break;

                    case 2:
                        result = (value << 1) | carry_in;
                        carry_out = high != 0;
                        break;

                    case 3:
                        result = (value >> 1) | (carry_in << 7);
                        carry_out = low != 0;
                        break;

                    case 4:
                        result = value << 1;
                        carry_out = high != 0;
                        break;

                    case 5:
                        result = (value >> 1) | (high << 7);
                        carry_out = low != 0;
                        break;

                    case 6:
                        result = (value >> 4) | (value << 4);
                        carry_out = false;
                        break;

                    default:
                        result = value >> 1;
                        carry_out = low != 0;
                        break;
                }

                result &= 0xFF;

                uint8_t flags = flags_of(result == 0, false, false,
                                         carry_out);

                EXPECT(m->cpu.registers.b == result &&
                           m->cpu.registers.f == flags,
                       "%s B=%02X carry=%u: got B=%02X F=%02X, "
                       "want B=%02X F=%02X",
                       names[op], value, carry_in, m->cpu.registers.b,
                       m->cpu.registers.f, result, flags);
            }
        }
    }
}

/* RLCA RRCA RLA RRA: the CB result, but Z is always cleared. */
static void test_accumulator_rotates(TestMachine *m)
{
    for (unsigned op = 0; op < 4; op++) {
        const uint8_t opcode = (uint8_t)(0x07 | (op << 3));

        load_program(m, &opcode, 1);

        for (unsigned value = 0; value < 256; value++) {
            for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
                prepare(m, (uint8_t)value, 0, carry_in ? C : 0);
                cpu_step(&m->cpu);

                unsigned high = value >> 7;
                unsigned low = value & 1;
                unsigned result;
                bool carry_out;

                switch (op) {
                    case 0:
                        result = (value << 1) | high;
                        carry_out = high != 0;
                        break;

                    case 1:
                        result = (value >> 1) | (low << 7);
                        carry_out = low != 0;
                        break;

                    case 2:
                        result = (value << 1) | carry_in;
                        carry_out = high != 0;
                        break;

                    default:
                        result = (value >> 1) | (carry_in << 7);
                        carry_out = low != 0;
                        break;
                }

                result &= 0xFF;

                uint8_t flags = flags_of(false, false, false, carry_out);

                EXPECT(m->cpu.registers.a == result &&
                           m->cpu.registers.f == flags,
                       "opcode %02X A=%02X carry=%u: got A=%02X F=%02X, "
                       "want A=%02X F=%02X",
                       opcode, value, carry_in, m->cpu.registers.a,
                       m->cpu.registers.f, result, flags);
            }
        }
    }
}

/* Deterministic pseudo-random 16-bit values: no dependence on rand(). */
static uint16_t next_value(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;

    return (uint16_t)(*state >> 16);
}

/* ADD HL,BC: boundaries plus a deterministic random sample. Z is kept. */
static void test_add_hl(TestMachine *m)
{
    const uint8_t opcode = 0x09; /* ADD HL,BC */
    static const uint16_t edges[] = {
        0x0000, 0x0001, 0x0FFF, 0x1000, 0x7FFF, 0x8000, 0xF000, 0xFFFF
    };

    load_program(m, &opcode, 1);

    uint32_t state = 12345;

    for (unsigned i = 0; i < 100000; i++) {
        uint16_t hl;
        uint16_t bc;

        if (i < 64) {
            hl = edges[i / 8];
            bc = edges[i % 8];
        } else {
            hl = next_value(&state);
            bc = next_value(&state);
        }

        for (unsigned z_in = 0; z_in < 2; z_in++) {
            test_machine_reset_cpu(m);
            m->cpu.registers.h = (uint8_t)(hl >> 8);
            m->cpu.registers.l = (uint8_t)hl;
            m->cpu.registers.b = (uint8_t)(bc >> 8);
            m->cpu.registers.c = (uint8_t)bc;
            m->cpu.registers.f = (uint8_t)((z_in ? Z : 0) | N);

            cpu_step(&m->cpu);

            uint32_t sum = (uint32_t)hl + bc;
            uint16_t result = (uint16_t)sum;
            uint8_t flags = flags_of(
                z_in != 0, false,
                ((hl ^ bc ^ sum) & 0x1000) != 0,
                ((hl ^ bc ^ sum) & 0x10000) != 0
            );
            uint16_t got = (uint16_t)((m->cpu.registers.h << 8) |
                                      m->cpu.registers.l);

            EXPECT(got == result && m->cpu.registers.f == flags,
                   "ADD HL,BC HL=%04X BC=%04X: got HL=%04X F=%02X, "
                   "want HL=%04X F=%02X",
                   hl, bc, got, m->cpu.registers.f, result, flags);
        }
    }
}

/*
 * ADD SP,e8 and LD HL,SP+e8. The flags depend only on the low byte of SP,
 * so every low byte and offset is covered with a few high bytes.
 */
static void test_sp_plus_offset(TestMachine *m)
{
    static const unsigned highs[] = { 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF };

    for (unsigned load_hl = 0; load_hl < 2; load_hl++) {
        const uint8_t program[] = { load_hl ? 0xF8 : 0xE8, 0x00 };

        for (unsigned offset = 0; offset < 256; offset++) {
            uint8_t patched[] = { program[0], (uint8_t)offset };

            load_program(m, patched, sizeof(patched));

            for (unsigned low = 0; low < 256; low++) {
                for (size_t h = 0; h < sizeof(highs) / sizeof(highs[0]);
                     h++) {
                    uint16_t sp = (uint16_t)((highs[h] << 8) | low);

                    test_machine_reset_cpu(m);
                    m->cpu.registers.sp = sp;
                    m->cpu.registers.f = (uint8_t)(Z | N | H | C);

                    cpu_step(&m->cpu);

                    unsigned low_sum = low + offset;
                    uint8_t flags = flags_of(
                        false, false,
                        ((low ^ offset ^ low_sum) & 0x10) != 0,
                        ((low ^ offset ^ low_sum) & 0x100) != 0
                    );
                    uint16_t expected =
                        (uint16_t)(sp + (int8_t)(uint8_t)offset);
                    uint16_t got = (uint16_t)(load_hl
                        ? (m->cpu.registers.h << 8) | m->cpu.registers.l
                        : m->cpu.registers.sp);

                    EXPECT(got == expected && m->cpu.registers.f == flags,
                           "%s SP=%04X e8=%02X: got %04X F=%02X, "
                           "want %04X F=%02X",
                           load_hl ? "LD HL,SP+e8" : "ADD SP,e8", sp, offset,
                           got, m->cpu.registers.f, expected, flags);
                }
            }
        }
    }
}

int main(void)
{
    TestMachine machine;

    test_machine_init(&machine);

    test_alu_a_r8(&machine);
    test_daa_matches_decimal_arithmetic(&machine);
    test_inc_dec_r8(&machine);
    test_cb_rotate_shift(&machine);
    test_accumulator_rotates(&machine);
    test_add_hl(&machine);
    test_sp_plus_offset(&machine);

    test_machine_destroy(&machine);

    printf("Exhaustive CPU ALU tests passed!\n");

    return 0;
}
