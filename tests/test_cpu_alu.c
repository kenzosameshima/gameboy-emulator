/*
 * Flag and result checks for the ALU, rotate/shift, CB, stack and 16-bit
 * arithmetic instructions, using known reference values.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "test_util.h"

enum {
    PROGRAM_ADDRESS = 0x0100,

    Z = FLAG_Z,
    N = FLAG_N,
    H = FLAG_H,
    C = FLAG_C
};

/* Loads a program at 0x0100, resets the CPU and runs `steps` instructions. */
static void run(
    TestMachine *machine,
    const uint8_t *program,
    size_t size,
    unsigned steps
)
{
    test_machine_load(machine, PROGRAM_ADDRESS, program, size);
    test_machine_reset_cpu(machine);

    for (unsigned step = 0; step < steps; step++) {
        cpu_step(&machine->cpu);
    }
}

#define RUN(machine, steps, ...)                                    \
    do {                                                            \
        const uint8_t program_[] = { __VA_ARGS__ };                 \
        run((machine), program_, sizeof(program_), (steps));        \
    } while (0)

static void test_add_adc(TestMachine *m)
{
    /* ADD A,n8: 0x3A + 0xC6 = 0x100 */
    RUN(m, 2, 0x3E, 0x3A, 0xC6, 0xC6);
    assert(m->cpu.registers.a == 0x00);
    assert(m->cpu.registers.f == (Z | H | C));

    /* ADC A,n8 with carry: 0xE1 + 0x0F + 1 = 0xF1 */
    RUN(m, 3, 0x3E, 0xE1, 0x37, 0xCE, 0x0F);
    assert(m->cpu.registers.a == 0xF1);
    assert(m->cpu.registers.f == H);

    /* ADD A,B with no carries */
    RUN(m, 2, 0x06, 0x01, 0x80);
    assert(m->cpu.registers.a == 0x02);
    assert(m->cpu.registers.f == 0);
}

static void test_sub_sbc_cp(TestMachine *m)
{
    RUN(m, 2, 0x3E, 0x3E, 0xD6, 0x3E);
    assert(m->cpu.registers.a == 0x00);
    assert(m->cpu.registers.f == (Z | N));

    RUN(m, 2, 0x3E, 0x3E, 0xD6, 0x40);
    assert(m->cpu.registers.a == 0xFE);
    assert(m->cpu.registers.f == (N | C));

    RUN(m, 2, 0x3E, 0x3E, 0xD6, 0x0F);
    assert(m->cpu.registers.a == 0x2F);
    assert(m->cpu.registers.f == (N | H));

    /* SBC A,n8 with carry: 0x3B - 0x2A - 1 = 0x10 */
    RUN(m, 3, 0x3E, 0x3B, 0x37, 0xDE, 0x2A);
    assert(m->cpu.registers.a == 0x10);
    assert(m->cpu.registers.f == N);

    /* CP leaves A alone: 0x3C - 0x2F borrows from bit 4 only. */
    RUN(m, 2, 0x3E, 0x3C, 0xFE, 0x2F);
    assert(m->cpu.registers.a == 0x3C);
    assert(m->cpu.registers.f == (N | H));

    /* CP A,A */
    RUN(m, 1, 0xBF);
    assert(m->cpu.registers.f == (Z | N));
}

static void test_logic(TestMachine *m)
{
    RUN(m, 2, 0x3E, 0x5A, 0xE6, 0x3F);
    assert(m->cpu.registers.a == 0x1A);
    assert(m->cpu.registers.f == H);

    RUN(m, 2, 0x3E, 0x5A, 0xF6, 0x0F);
    assert(m->cpu.registers.a == 0x5F);
    assert(m->cpu.registers.f == 0);

    RUN(m, 2, 0x3E, 0xFF, 0xEE, 0xFF);
    assert(m->cpu.registers.a == 0x00);
    assert(m->cpu.registers.f == Z);

    /* AND on [HL] reads memory: HL = 0xC000. */
    RUN(m, 4, 0x21, 0x00, 0xC0, 0x36, 0x0F, 0x3E, 0xF3, 0xA6);
    assert(m->cpu.registers.a == 0x03);
}

static void test_daa(TestMachine *m)
{
    /* BCD 45 + 38 = 83 */
    RUN(m, 3, 0x3E, 0x45, 0xC6, 0x38, 0x27);
    assert(m->cpu.registers.a == 0x83);
    assert(m->cpu.registers.f == 0);

    /* BCD 83 - 38 = 45 */
    RUN(m, 3, 0x3E, 0x83, 0xD6, 0x38, 0x27);
    assert(m->cpu.registers.a == 0x45);
    assert(m->cpu.registers.f == N);

    /* BCD 99 + 01 = 100: A wraps to 00 with carry. */
    RUN(m, 3, 0x3E, 0x99, 0xC6, 0x01, 0x27);
    assert(m->cpu.registers.a == 0x00);
    assert(m->cpu.registers.f == (Z | C));
}

static void test_accumulator_ops(TestMachine *m)
{
    /* RLCA never sets Z, and moves bit 7 to C and bit 0. */
    RUN(m, 2, 0x3E, 0x85, 0x07);
    assert(m->cpu.registers.a == 0x0B);
    assert(m->cpu.registers.f == C);

    /* OR A clears the carry, so RRA rotates a 0 in and bit 0 out. */
    RUN(m, 3, 0x3E, 0x01, 0xB7, 0x1F);
    assert(m->cpu.registers.a == 0x00);
    assert(m->cpu.registers.f == C);

    /* CPL, SCF, CCF */
    RUN(m, 2, 0x3E, 0x35, 0x2F);
    assert(m->cpu.registers.a == 0xCA);
    assert((m->cpu.registers.f & (N | H)) == (N | H));

    RUN(m, 1, 0x37);
    assert((m->cpu.registers.f & (N | H | C)) == C);

    RUN(m, 2, 0x37, 0x3F);
    assert((m->cpu.registers.f & (N | H | C)) == 0);
}

static void test_cb_instructions(TestMachine *m)
{
    /* SWAP A */
    RUN(m, 2, 0x3E, 0xF0, 0xCB, 0x37);
    assert(m->cpu.registers.a == 0x0F);
    assert(m->cpu.registers.f == 0);

    /* SRL A: 1 -> 0, Z and C */
    RUN(m, 2, 0x3E, 0x01, 0xCB, 0x3F);
    assert(m->cpu.registers.a == 0x00);
    assert(m->cpu.registers.f == (Z | C));

    /* SRA A keeps the sign bit: 0x81 -> 0xC0, C from bit 0. */
    RUN(m, 2, 0x3E, 0x81, 0xCB, 0x2F);
    assert(m->cpu.registers.a == 0xC0);
    assert(m->cpu.registers.f == C);

    /* SLA B */
    RUN(m, 2, 0x06, 0x80, 0xCB, 0x20);
    assert(m->cpu.registers.b == 0x00);
    assert(m->cpu.registers.f == (Z | C));

    /* BIT 7,A leaves C alone and sets H. */
    RUN(m, 3, 0x3E, 0x80, 0x37, 0xCB, 0x7F);
    assert(m->cpu.registers.f == (H | C));

    /* The CPU starts with F = 0xB0, so C is still set here. */
    RUN(m, 2, 0x3E, 0x7F, 0xCB, 0x7F);
    assert(m->cpu.registers.f == (Z | H | C));

    /* SET 0,B / RES 7,A do not touch flags. */
    RUN(m, 2, 0x06, 0x00, 0xCB, 0xC0);
    assert(m->cpu.registers.b == 0x01);
    assert(m->cpu.registers.f == 0xB0);

    RUN(m, 2, 0x3E, 0xFF, 0xCB, 0xBF);
    assert(m->cpu.registers.a == 0x7F);

    /* RLC [HL] with HL = 0xC000. */
    RUN(m, 3, 0x21, 0x00, 0xC0, 0x36, 0x85, 0xCB, 0x06);
    assert(m->memory.wram[0] == 0x0B);
    assert(m->cpu.registers.f == C);
}

static void test_stack_and_calls(TestMachine *m)
{
    /* PUSH BC; POP AF: the low nibble of F is always zero. */
    RUN(m, 3, 0x01, 0xFF, 0x12, 0xC5, 0xF1);
    assert(m->cpu.registers.a == 0x12);
    assert(m->cpu.registers.f == 0xF0);
    assert(m->cpu.registers.sp == 0xFFFE);

    /* CALL 0x0200 pushes the return address 0x0103; RET returns to it. */
    RUN(m, 1, 0xCD, 0x00, 0x02);
    assert(m->cpu.registers.pc == 0x0200);
    assert(m->cpu.registers.sp == 0xFFFC);
    assert(m->memory.hram[0xFFFC - 0xFF80] == 0x03);
    assert(m->memory.hram[0xFFFD - 0xFF80] == 0x01);

    const uint8_t ret[] = { 0xC9 };

    test_machine_load(m, 0x0200, ret, sizeof(ret));
    cpu_step(&m->cpu);
    assert(m->cpu.registers.pc == 0x0103);
    assert(m->cpu.registers.sp == 0xFFFE);

    /* RST 28h */
    RUN(m, 1, 0xEF);
    assert(m->cpu.registers.pc == 0x0028);
    assert(m->cpu.registers.sp == 0xFFFC);

    /* JP HL and LD SP,HL */
    RUN(m, 2, 0x21, 0x34, 0x12, 0xE9);
    assert(m->cpu.registers.pc == 0x1234);

    RUN(m, 2, 0x21, 0x34, 0xC1, 0xF9);
    assert(m->cpu.registers.sp == 0xC134);

    /* LD [a16],SP stores the low byte first. */
    RUN(m, 2, 0x31, 0x34, 0xC2, 0x08, 0x00, 0xC0);
    assert(m->memory.wram[0] == 0x34);
    assert(m->memory.wram[1] == 0xC2);
}

static void test_16_bit_arithmetic(TestMachine *m)
{
    /* ADD HL,BC: Z is kept, H from bit 11. */
    RUN(m, 2, 0x21, 0x23, 0x8A, 0x01, 0x05, 0x06, 0x09);
    m->cpu.registers.f = Z;
    cpu_step(&m->cpu);
    assert(m->cpu.registers.pc == 0x0107);
    assert(m->cpu.registers.h == 0x90);
    assert(m->cpu.registers.l == 0x28);
    assert(m->cpu.registers.f == (Z | H));

    /* ADD SP,e8: flags come from the low byte. 0xFFF8 + 8 = 0x0000. */
    RUN(m, 2, 0x31, 0xF8, 0xFF, 0xE8, 0x08);
    assert(m->cpu.registers.sp == 0x0000);
    assert(m->cpu.registers.f == (H | C));

    /* LD HL,SP+e8 with a negative offset. */
    RUN(m, 2, 0x31, 0x00, 0xFF, 0xF8, 0xFE);
    assert(m->cpu.registers.h == 0xFE);
    assert(m->cpu.registers.l == 0xFE);
    assert(m->cpu.registers.f == 0);

    /* INC/DEC r16 leave the flags alone and wrap. */
    RUN(m, 2, 0x01, 0xFF, 0xFF, 0x03);
    assert(m->cpu.registers.b == 0x00);
    assert(m->cpu.registers.c == 0x00);
    assert(m->cpu.registers.f == 0xB0);
}

static void test_loads_and_misc(TestMachine *m)
{
    /* LD [a16],A and LD A,[a16] */
    RUN(m, 4, 0x3E, 0x5C, 0xEA, 0x10, 0xC0, 0xAF, 0xFA, 0x10, 0xC0);
    assert(m->cpu.registers.a == 0x5C);

    /* LDH [C],A and LDH A,[C] through high RAM */
    RUN(m, 5, 0x0E, 0x90, 0x3E, 0x77, 0xE2, 0xAF, 0xF2);
    assert(m->cpu.registers.a == 0x77);

    /* STOP consumes its second byte and stops the CPU. */
    RUN(m, 1, 0x10, 0x00);
    assert(m->cpu.stopped);
    assert(m->cpu.registers.pc == 0x0102);

    assert(cpu_step(&m->cpu) == 4);
    assert(m->cpu.step_status == CPU_STEP_STOPPED);
    assert(m->cpu.registers.pc == 0x0102);

    /* A joypad interrupt request ends STOP. */
    m->interrupts.interrupt_flag = INTERRUPT_JOYPAD;
    cpu_step(&m->cpu);
    assert(!m->cpu.stopped);
}

int main(void)
{
    TestMachine machine;

    test_machine_init(&machine);

    test_add_adc(&machine);
    test_sub_sbc_cp(&machine);
    test_logic(&machine);
    test_daa(&machine);
    test_accumulator_ops(&machine);
    test_cb_instructions(&machine);
    test_stack_and_calls(&machine);
    test_16_bit_arithmetic(&machine);
    test_loads_and_misc(&machine);

    test_machine_destroy(&machine);

    printf("CPU ALU tests passed!\n");

    return 0;
}
