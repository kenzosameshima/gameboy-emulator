#include <stdbool.h>
#include <stdint.h>

#include "cpu_internal.h"

/*
 * All arithmetic is done on int so that carries and borrows are visible
 * in the intermediate value; results are narrowed with explicit casts.
 */

/*
 * INC r8
 *
 * Flags:
 *
 * Z = set if result is zero
 * N = reset
 * H = set if carry from bit 3
 * C = unchanged
 */

uint8_t cpu_inc8(CPU *cpu, uint8_t value)
{
    uint8_t result = (uint8_t)(value + 1);

    cpu_set_flag(cpu, FLAG_Z, result == 0);
    cpu_set_flag(cpu, FLAG_N, false);
    cpu_set_flag(
        cpu,
        FLAG_H,
        (value & 0x0F) == 0x0F
    );

    return result;
}

/*
 * DEC r8
 *
 * Flags:
 *
 * Z = set if result is zero
 * N = set
 * H = set if borrow from bit 4
 * C = unchanged
 */

uint8_t cpu_dec8(CPU *cpu, uint8_t value)
{
    uint8_t result = (uint8_t)(value - 1);

    cpu_set_flag(cpu, FLAG_Z, result == 0);
    cpu_set_flag(cpu, FLAG_N, true);
    cpu_set_flag(
        cpu,
        FLAG_H,
        (value & 0x0F) == 0
    );

    return result;
}

static uint8_t cpu_add8(CPU *cpu, uint8_t a, uint8_t value, int carry)
{
    int sum = a + value + carry;
    uint8_t result = (uint8_t)sum;

    cpu_set_flags(
        cpu,
        result == 0,
        false,
        ((a & 0x0F) + (value & 0x0F) + carry) > 0x0F,
        sum > 0xFF
    );

    return result;
}

static uint8_t cpu_sub8(CPU *cpu, uint8_t a, uint8_t value, int carry)
{
    int difference = a - value - carry;
    uint8_t result = (uint8_t)difference;

    cpu_set_flags(
        cpu,
        result == 0,
        true,
        ((a & 0x0F) - (value & 0x0F) - carry) < 0,
        difference < 0
    );

    return result;
}

void cpu_alu_a(CPU *cpu, uint8_t op, uint8_t value)
{
    uint8_t a = cpu->registers.a;
    int carry = cpu_get_flag(cpu, FLAG_C) ? 1 : 0;

    switch (op) {
        case 0:
            cpu->registers.a = cpu_add8(cpu, a, value, 0);
            break;

        case 1:
            cpu->registers.a = cpu_add8(cpu, a, value, carry);
            break;

        case 2:
            cpu->registers.a = cpu_sub8(cpu, a, value, 0);
            break;

        case 3:
            cpu->registers.a = cpu_sub8(cpu, a, value, carry);
            break;

        case 4:
            cpu->registers.a = (uint8_t)(a & value);
            cpu_set_flags(cpu, cpu->registers.a == 0, false, true, false);
            break;

        case 5:
            cpu->registers.a = (uint8_t)(a ^ value);
            cpu_set_flags(cpu, cpu->registers.a == 0, false, false, false);
            break;

        case 6:
            cpu->registers.a = (uint8_t)(a | value);
            cpu_set_flags(cpu, cpu->registers.a == 0, false, false, false);
            break;

        default:
            /* CP: SUB without storing the result. */
            cpu_sub8(cpu, a, value, 0);
            break;
    }
}

uint8_t cpu_rotate_shift(CPU *cpu, uint8_t op, uint8_t value)
{
    int carry_in = cpu_get_flag(cpu, FLAG_C) ? 1 : 0;
    int result;
    bool carry_out;

    switch (op) {
        case 0: /* RLC */
            carry_out = (value & 0x80) != 0;
            result = (value << 1) | (carry_out ? 1 : 0);
            break;

        case 1: /* RRC */
            carry_out = (value & 0x01) != 0;
            result = (value >> 1) | (carry_out ? 0x80 : 0);
            break;

        case 2: /* RL */
            carry_out = (value & 0x80) != 0;
            result = (value << 1) | carry_in;
            break;

        case 3: /* RR */
            carry_out = (value & 0x01) != 0;
            result = (value >> 1) | (carry_in << 7);
            break;

        case 4: /* SLA */
            carry_out = (value & 0x80) != 0;
            result = value << 1;
            break;

        case 5: /* SRA */
            carry_out = (value & 0x01) != 0;
            result = (value >> 1) | (value & 0x80);
            break;

        case 6: /* SWAP */
            carry_out = false;
            result = (value << 4) | (value >> 4);
            break;

        default: /* SRL */
            carry_out = (value & 0x01) != 0;
            result = value >> 1;
            break;
    }

    uint8_t narrowed = (uint8_t)result;

    cpu_set_flags(cpu, narrowed == 0, false, false, carry_out);

    return narrowed;
}

/*
 * ADD HL,r16
 *
 * Flags:
 *
 * Z = unchanged
 * N = reset
 * H = set if carry from bit 11
 * C = set if carry from bit 15
 */

uint16_t cpu_add_hl(CPU *cpu, uint16_t hl, uint16_t value)
{
    int sum = hl + value;

    cpu_set_flag(cpu, FLAG_N, false);
    cpu_set_flag(cpu, FLAG_H, ((hl & 0x0FFF) + (value & 0x0FFF)) > 0x0FFF);
    cpu_set_flag(cpu, FLAG_C, sum > 0xFFFF);

    return (uint16_t)sum;
}

/*
 * ADD SP,e8 and LD HL,SP+e8
 *
 * Flags:
 *
 * Z = reset
 * N = reset
 * H = set if carry from bit 3 of the low byte
 * C = set if carry from bit 7 of the low byte
 */

uint16_t cpu_sp_plus_offset(CPU *cpu, uint8_t offset)
{
    uint16_t sp = cpu->registers.sp;

    cpu_set_flags(
        cpu,
        false,
        false,
        ((sp & 0x0F) + (offset & 0x0F)) > 0x0F,
        ((sp & 0xFF) + offset) > 0xFF
    );

    return (uint16_t)(sp + (int8_t)offset);
}

void cpu_daa(CPU *cpu)
{
    int a = cpu->registers.a;
    bool n = cpu_get_flag(cpu, FLAG_N);
    bool h = cpu_get_flag(cpu, FLAG_H);
    bool c = cpu_get_flag(cpu, FLAG_C);

    if (!n) {
        if (c || a > 0x99) {
            a += 0x60;
            c = true;
        }

        if (h || (a & 0x0F) > 0x09) {
            a += 0x06;
        }
    } else {
        if (c) {
            a -= 0x60;
        }

        if (h) {
            a -= 0x06;
        }
    }

    cpu->registers.a = (uint8_t)a;
    cpu_set_flags(cpu, cpu->registers.a == 0, n, false, c);
}
