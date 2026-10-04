#include <stdbool.h>
#include <stdint.h>

#include "cpu_internal.h"

/*
 * Opcode decoder
 *
 * An opcode is split into bit fields:
 *
 *   x = opcode >> 6
 *   y = (opcode >> 3) & 7
 *   z = opcode & 7
 *   p = y >> 1
 *   q = y & 1
 *
 * r8 operands use the index 0=B 1=C 2=D 3=E 4=H 5=L 6=[HL] 7=A and
 * r16 operands use 0=BC 1=DE 2=HL 3=SP (3=AF for PUSH and POP).
 *
 * Handlers return nothing: an instruction takes exactly as long as the
 * M-cycles it spends, and the opcode fetch was already accounted for by
 * cpu_step().
 */

bool cpu_opcode_is_illegal(uint8_t opcode)
{
    switch (opcode) {
        case 0xD3:
        case 0xDB:
        case 0xDD:
        case 0xE3:
        case 0xE4:
        case 0xEB:
        case 0xEC:
        case 0xED:
        case 0xF4:
        case 0xFC:
        case 0xFD:
            return true;

        default:
            return false;
    }
}

static void cpu_jump_relative(CPU *cpu, bool condition)
{
    int8_t offset = (int8_t)cpu_fetch8(cpu);

    if (condition) {
        cpu_idle(cpu);
        cpu->registers.pc = (uint16_t)(cpu->registers.pc + offset);
    }
}

static void cpu_jump_absolute(CPU *cpu, bool condition)
{
    uint16_t address = cpu_fetch16(cpu);

    if (condition) {
        cpu_idle(cpu);
        cpu->registers.pc = address;
    }
}

static void cpu_call(CPU *cpu, bool condition)
{
    uint16_t address = cpu_fetch16(cpu);

    if (condition) {
        cpu_idle(cpu);
        cpu_push16(cpu, cpu->registers.pc);
        cpu->registers.pc = address;
    }
}

/* Loads and stores through [BC], [DE], [HL+] and [HL-] (x=0, z=2). */
static void cpu_load_indirect(CPU *cpu, uint8_t y)
{
    uint8_t p = (uint8_t)(y >> 1);
    bool load = (y & 1) != 0;
    uint16_t address;

    switch (p) {
        case 0:
            address = cpu_read_r16(cpu, 0);
            break;

        case 1:
            address = cpu_read_r16(cpu, 1);
            break;

        case 2:
            address = cpu_get_hl(cpu);
            cpu_set_hl(cpu, (uint16_t)(address + 1));
            break;

        default:
            address = cpu_get_hl(cpu);
            cpu_set_hl(cpu, (uint16_t)(address - 1));
            break;
    }

    if (load) {
        cpu->registers.a = cpu_read8(cpu, address);
    } else {
        cpu_write8(cpu, address, cpu->registers.a);
    }
}

/* RLCA, RRCA, RLA, RRA, DAA, CPL, SCF, CCF (x=0, z=7). */
static void cpu_accumulator_op(CPU *cpu, uint8_t y)
{
    switch (y) {
        case 0:
        case 1:
        case 2:
        case 3:
            cpu->registers.a = cpu_rotate_shift(cpu, y, cpu->registers.a);
            cpu_set_flag(cpu, FLAG_Z, false);
            break;

        case 4:
            cpu_daa(cpu);
            break;

        case 5:
            cpu->registers.a = (uint8_t)~cpu->registers.a;
            cpu_set_flag(cpu, FLAG_N, true);
            cpu_set_flag(cpu, FLAG_H, true);
            break;

        case 6:
            cpu_set_flag(cpu, FLAG_N, false);
            cpu_set_flag(cpu, FLAG_H, false);
            cpu_set_flag(cpu, FLAG_C, true);
            break;

        default:
            cpu_set_flag(cpu, FLAG_N, false);
            cpu_set_flag(cpu, FLAG_H, false);
            cpu_set_flag(cpu, FLAG_C, !cpu_get_flag(cpu, FLAG_C));
            break;
    }
}

/* Instructions with x = 0. */
static void cpu_execute_block0(CPU *cpu, uint8_t y, uint8_t z)
{
    uint8_t p = (uint8_t)(y >> 1);
    bool q = (y & 1) != 0;

    switch (z) {
        case 0:
            switch (y) {
                case 0: /* NOP */
                    break;

                case 1: /* LD [a16],SP */
                    {
                        uint16_t address = cpu_fetch16(cpu);

                        cpu_write8(
                            cpu,
                            address,
                            (uint8_t)(cpu->registers.sp & 0xFF)
                        );
                        cpu_write8(
                            cpu,
                            (uint16_t)(address + 1),
                            (uint8_t)(cpu->registers.sp >> 8)
                        );
                    }
                    break;

                case 2: /* STOP: the byte after the opcode is skipped. */
                    cpu->registers.pc++;
                    cpu->stopped = true;
                    break;

                case 3: /* JR e8 */
                    cpu_jump_relative(cpu, true);
                    break;

                default: /* JR cc,e8 */
                    cpu_jump_relative(
                        cpu,
                        cpu_condition(cpu, (uint8_t)(y - 4))
                    );
                    break;
            }
            break;

        case 1:
            if (!q) { /* LD r16,d16 */
                cpu_write_r16(cpu, p, cpu_fetch16(cpu));
            } else { /* ADD HL,r16 */
                cpu_set_hl(
                    cpu,
                    cpu_add_hl(cpu, cpu_get_hl(cpu), cpu_read_r16(cpu, p))
                );
                cpu_idle(cpu);
            }
            break;

        case 2:
            cpu_load_indirect(cpu, y);
            break;

        case 3: /* INC r16, DEC r16 */
            cpu_write_r16(
                cpu,
                p,
                (uint16_t)(cpu_read_r16(cpu, p) + (q ? -1 : 1))
            );
            cpu_idle(cpu);
            break;

        case 4: /* INC r8 */
            cpu_write_r8(cpu, y, cpu_inc8(cpu, cpu_read_r8(cpu, y)));
            break;

        case 5: /* DEC r8 */
            cpu_write_r8(cpu, y, cpu_dec8(cpu, cpu_read_r8(cpu, y)));
            break;

        case 6: /* LD r8,n8 */
            cpu_write_r8(cpu, y, cpu_fetch8(cpu));
            break;

        default:
            cpu_accumulator_op(cpu, y);
            break;
    }
}

/* Instructions with x = 3, other than ALU A,n8 and RST. */
static void cpu_execute_block3(CPU *cpu, uint8_t y, uint8_t z)
{
    uint8_t p = (uint8_t)(y >> 1);
    bool q = (y & 1) != 0;

    switch (z) {
        case 0:
            switch (y) {
                case 4: /* LDH [a8],A */
                    {
                        uint8_t offset = cpu_fetch8(cpu);

                        cpu_write8(
                            cpu,
                            (uint16_t)(0xFF00U + offset),
                            cpu->registers.a
                        );
                    }
                    break;

                case 5: /* ADD SP,e8 */
                    cpu->registers.sp = cpu_sp_plus_offset(
                        cpu,
                        cpu_fetch8(cpu)
                    );
                    cpu_idle(cpu);
                    cpu_idle(cpu);
                    break;

                case 6: /* LDH A,[a8] */
                    {
                        uint8_t offset = cpu_fetch8(cpu);

                        cpu->registers.a = cpu_read8(
                            cpu,
                            (uint16_t)(0xFF00U + offset)
                        );
                    }
                    break;

                case 7: /* LD HL,SP+e8 */
                    cpu_set_hl(
                        cpu,
                        cpu_sp_plus_offset(cpu, cpu_fetch8(cpu))
                    );
                    cpu_idle(cpu);
                    break;

                default: /* RET cc */
                    cpu_idle(cpu);

                    if (cpu_condition(cpu, y)) {
                        cpu->registers.pc = cpu_pop16(cpu);
                        cpu_idle(cpu);
                    }
                    break;
            }
            break;

        case 1:
            if (!q) { /* POP r16 */
                cpu_write_r16_stack(cpu, p, cpu_pop16(cpu));
                break;
            }

            switch (p) {
                case 0: /* RET */
                    cpu->registers.pc = cpu_pop16(cpu);
                    cpu_idle(cpu);
                    break;

                case 1: /* RETI */
                    cpu->registers.pc = cpu_pop16(cpu);
                    cpu_idle(cpu);
                    cpu->ime = true;
                    break;

                case 2: /* JP HL */
                    cpu->registers.pc = cpu_get_hl(cpu);
                    break;

                default: /* LD SP,HL */
                    cpu->registers.sp = cpu_get_hl(cpu);
                    cpu_idle(cpu);
                    break;
            }
            break;

        case 2:
            switch (y) {
                case 4: /* LDH [C],A */
                    cpu_write8(
                        cpu,
                        (uint16_t)(0xFF00U + cpu->registers.c),
                        cpu->registers.a
                    );
                    break;

                case 5: /* LD [a16],A */
                    cpu_write8(cpu, cpu_fetch16(cpu), cpu->registers.a);
                    break;

                case 6: /* LDH A,[C] */
                    cpu->registers.a = cpu_read8(
                        cpu,
                        (uint16_t)(0xFF00U + cpu->registers.c)
                    );
                    break;

                case 7: /* LD A,[a16] */
                    cpu->registers.a = cpu_read8(cpu, cpu_fetch16(cpu));
                    break;

                default: /* JP cc,a16 */
                    cpu_jump_absolute(cpu, cpu_condition(cpu, y));
                    break;
            }
            break;

        case 3:
            switch (y) {
                case 0: /* JP a16 */
                    cpu_jump_absolute(cpu, true);
                    break;

                case 1: /* CB prefix */
                    cpu_execute_cb(cpu);
                    break;

                case 6: /* DI */
                    cpu->ime = false;
                    cpu->ime_enable_delay = 0;
                    break;

                default: /* EI: enable IME after the following instruction. */
                    /* An enable already pending is not pushed back by another EI. */
                    if (!cpu->ime && cpu->ime_enable_delay == 0) {
                        cpu->ime_enable_delay = 2;
                    }
                    break;
            }
            break;

        case 4: /* CALL cc,a16 */
            cpu_call(cpu, cpu_condition(cpu, y));
            break;

        default: /* z = 5 */
            if (!q) { /* PUSH r16 */
                uint16_t value = cpu_read_r16_stack(cpu, p);

                cpu_idle(cpu);
                cpu_push16(cpu, value);
            } else { /* CALL a16 */
                cpu_call(cpu, true);
            }
            break;
    }
}

void cpu_execute_opcode(CPU *cpu, uint8_t opcode)
{
    uint8_t x = (uint8_t)(opcode >> 6);
    uint8_t y = (uint8_t)((opcode >> 3) & 0x07);
    uint8_t z = (uint8_t)(opcode & 0x07);

    switch (x) {
        case 0:
            cpu_execute_block0(cpu, y, z);
            break;

        case 1:
            if (opcode == 0x76) { /* HALT */
                cpu->halted = true;
            } else { /* LD r8,r8 */
                cpu_write_r8(cpu, y, cpu_read_r8(cpu, z));
            }
            break;

        case 2: /* ALU A,r8 */
            cpu_alu_a(cpu, y, cpu_read_r8(cpu, z));
            break;

        default:
            if (z == 6) { /* ALU A,n8 */
                cpu_alu_a(cpu, y, cpu_fetch8(cpu));
            } else if (z == 7) { /* RST */
                cpu_idle(cpu);
                cpu_push16(cpu, cpu->registers.pc);
                cpu->registers.pc = (uint16_t)(y * 8);
            } else {
                cpu_execute_block3(cpu, y, z);
            }
            break;
    }
}
