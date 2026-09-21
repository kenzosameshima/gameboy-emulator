#include <stdbool.h>
#include <stdint.h>

#include "cpu_internal.h"

/*
 * CB-prefixed instructions
 *
 * The byte after the prefix is split into bit fields:
 *
 *   x = opcode >> 6       0=rotate/shift 1=BIT 2=RES 3=SET
 *   y = (opcode >> 3) & 7 operation for x=0, bit number otherwise
 *   z = opcode & 7        r8 operand (6 = [HL])
 *
 * BIT on [HL] only reads it, so it is one M-cycle shorter than the
 * other [HL] forms, which read and then write.
 */

void cpu_execute_cb(CPU *cpu)
{
    uint8_t opcode = cpu_fetch8(cpu);

    uint8_t x = (uint8_t)(opcode >> 6);
    uint8_t y = (uint8_t)((opcode >> 3) & 0x07);
    uint8_t z = (uint8_t)(opcode & 0x07);

    uint8_t value = cpu_read_r8(cpu, z);
    uint8_t mask = (uint8_t)(1U << y);

    switch (x) {
        case 0:
            cpu_write_r8(cpu, z, cpu_rotate_shift(cpu, y, value));
            break;

        case 1: /* BIT: Z = !bit, N = 0, H = 1, C unchanged */
            cpu_set_flag(cpu, FLAG_Z, (value & mask) == 0);
            cpu_set_flag(cpu, FLAG_N, false);
            cpu_set_flag(cpu, FLAG_H, true);
            break;

        case 2: /* RES */
            cpu_write_r8(cpu, z, (uint8_t)(value & ~mask));
            break;

        default: /* SET */
            cpu_write_r8(cpu, z, (uint8_t)(value | mask));
            break;
    }
}
