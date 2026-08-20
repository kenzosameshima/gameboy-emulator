#include <stdbool.h>
#include <stdint.h>

#include <cpu.h>
#include <bus.h>

/*
 * Private helpers
 */

/* Register pairs */
static uint16_t cpu_get_bc(const CPU *cpu);
static uint16_t cpu_get_de(const CPU *cpu);
static uint16_t cpu_get_hl(const CPU *cpu);

static void cpu_set_bc(CPU *cpu, uint16_t value);
static void cpu_set_de(CPU *cpu, uint16_t value);
static void cpu_set_hl(CPU *cpu, uint16_t value);

/* Flags */
static void cpu_set_flag(CPU *cpu, Flag flag, bool value);

/* Generic 8-bit register access */
static uint8_t cpu_read_r8(CPU *cpu, uint8_t index);
static void cpu_write_r8(CPU *cpu, uint8_t index, uint8_t value);

/* 8-bit arithmetic */
static uint8_t cpu_inc8(CPU *cpu, uint8_t value);
static uint8_t cpu_dec8(CPU *cpu, uint8_t value);

/* Instruction decoding */
static uint8_t cpu_read8(CPU *cpu, uint16_t address);
static void cpu_write8(CPU *cpu, uint16_t address, uint8_t value);
static uint8_t cpu_fetch8(CPU *cpu);
static uint16_t cpu_fetch16(CPU *cpu);
static uint16_t cpu_pop16(CPU *cpu);
static uint8_t cpu_pending_interrupts(const CPU *cpu);
static CpuCycles cpu_service_interrupt(CPU *cpu, uint8_t pending);
static void cpu_advance_ime_delay(CPU *cpu);
static CpuCycles cpu_jump_relative(CPU *cpu, bool condition);

static CpuCycles cpu_execute_opcode(CPU *cpu, uint8_t opcode);

/* Flag invariant */
static void cpu_normalize_flags(CPU *cpu);


/*
 * CPU
 */

void cpu_init(CPU *cpu, Bus *bus)
{
    cpu->registers.a = 0x01;
    cpu->registers.f = 0xB0;

    cpu->registers.b = 0x00;
    cpu->registers.c = 0x13;

    cpu->registers.d = 0x00;
    cpu->registers.e = 0xD8;

    cpu->registers.h = 0x01;
    cpu->registers.l = 0x4D;

    cpu->registers.sp = 0xFFFE;
    cpu->registers.pc = 0x0100;

    cpu->bus = bus;

    cpu->halted = false;
    cpu->stopped = false;
    cpu->ime = false;
    cpu->ime_enable_delay = 0;
    cpu->step_status = CPU_STEP_EXECUTED;
}


/*
 * Memory access
 */

static uint8_t cpu_read8(CPU *cpu, uint16_t address)
{
    return bus_read(cpu->bus, address);
}

static void cpu_write8(CPU *cpu, uint16_t address, uint8_t value)
{
    bus_write(cpu->bus, address, value);
}

/*
 * Instruction fetch
 */

static uint8_t cpu_fetch8(CPU *cpu)
{
    uint8_t value = cpu_read8(cpu, cpu->registers.pc);

    cpu->registers.pc++;

    return value;
}

static uint16_t cpu_fetch16(CPU *cpu)
{
    uint8_t low = cpu_fetch8(cpu);
    uint8_t high = cpu_fetch8(cpu);

    return ((uint16_t)high << 8) | low;
}


/*
 * Instruction execution
 */

CpuCycles cpu_step(CPU *cpu)
{
    cpu_normalize_flags(cpu);

    uint8_t pending = cpu_pending_interrupts(cpu);

    if (cpu->ime && pending != 0) {
        return cpu_service_interrupt(cpu, pending);
    }

    if (cpu->halted) {
        if (pending != 0) {
            cpu->halted = false;
            cpu->step_status = CPU_STEP_WOKE_FROM_HALT;
            return 4;
        }

        cpu->step_status = CPU_STEP_HALTED;
        return 4;
    }

    cpu->step_status = CPU_STEP_EXECUTED;

    uint8_t opcode = cpu_fetch8(cpu);
    CpuCycles cycles = cpu_execute_opcode(cpu, opcode);

    if (cpu->step_status == CPU_STEP_EXECUTED) {
        cpu_advance_ime_delay(cpu);
    }

    return cycles;
}

static void cpu_advance_ime_delay(CPU *cpu)
{
    if (cpu->ime_enable_delay == 0) {
        return;
    }

    cpu->ime_enable_delay--;

    if (cpu->ime_enable_delay == 0) {
        cpu->ime = true;
    }
}

static CpuCycles cpu_service_interrupt(CPU *cpu, uint8_t pending)
{
    uint8_t interrupt_mask;
    uint16_t vector;

    if ((pending & INTERRUPT_VBLANK) != 0) {
        interrupt_mask = INTERRUPT_VBLANK;
        vector = 0x0040;
    } else if ((pending & INTERRUPT_LCD_STAT) != 0) {
        interrupt_mask = INTERRUPT_LCD_STAT;
        vector = 0x0048;
    } else if ((pending & INTERRUPT_TIMER) != 0) {
        interrupt_mask = INTERRUPT_TIMER;
        vector = 0x0050;
    } else if ((pending & INTERRUPT_SERIAL) != 0) {
        interrupt_mask = INTERRUPT_SERIAL;
        vector = 0x0058;
    } else {
        interrupt_mask = INTERRUPT_JOYPAD;
        vector = 0x0060;
    }

    uint8_t interrupt_flag =
        bus_read(cpu->bus, INTERRUPT_FLAG_ADDRESS);
    bus_write(
        cpu->bus,
        INTERRUPT_FLAG_ADDRESS,
        (uint8_t)(interrupt_flag & (uint8_t)~interrupt_mask)
    );

    uint16_t return_pc = cpu->registers.pc;
    cpu->registers.sp = (uint16_t)(cpu->registers.sp - 2);
    cpu_write8(
        cpu,
        cpu->registers.sp,
        (uint8_t)(return_pc & 0xFF)
    );
    cpu_write8(
        cpu,
        (uint16_t)(cpu->registers.sp + 1),
        (uint8_t)(return_pc >> 8)
    );

    cpu->registers.pc = vector;
    cpu->ime = false;
    cpu->ime_enable_delay = 0;
    cpu->halted = false;
    cpu->step_status = CPU_STEP_INTERRUPT_SERVICED;

    return 20;
}

static uint16_t cpu_pop16(CPU *cpu)
{
    uint8_t low = cpu_read8(cpu, cpu->registers.sp);
    uint8_t high = cpu_read8(
        cpu,
        (uint16_t)(cpu->registers.sp + 1)
    );

    cpu->registers.sp = (uint16_t)(cpu->registers.sp + 2);

    return ((uint16_t)high << 8) | low;
}

static uint8_t cpu_pending_interrupts(const CPU *cpu)
{
    uint8_t interrupt_flag =
        bus_read(cpu->bus, INTERRUPT_FLAG_ADDRESS);
    uint8_t interrupt_enable =
        bus_read(cpu->bus, INTERRUPT_ENABLE_ADDRESS);

    return (uint8_t)(
        interrupt_flag & interrupt_enable & INTERRUPT_VALID_MASK
    );
}


/*
 * Opcode decoder
 */

static CpuCycles cpu_execute_opcode(CPU *cpu, uint8_t opcode)
{
    switch (opcode) {

        /*
         * Miscellaneous
         */

        case 0x00:
            /*
             * NOP
             */
            return 4;

        case 0x76:
            /*
             * HALT
             */
            cpu->halted = true;
            return 4;

        case 0xF3:
            /*
             * DI
             */
            cpu->ime = false;
            cpu->ime_enable_delay = 0;
            return 4;

        case 0xFB:
            /*
             * EI: enable IME after the following instruction.
             */
            cpu->ime_enable_delay = 2;
            return 4;

        case 0xAF:
            /*
             * XOR A
             */
            cpu->registers.a = 0;
            cpu->registers.f = FLAG_Z;
            return 4;


        /*
         * 8-bit loads
         *
         * LD r8,n8
         */

        case 0x06:
            /*
             * LD B,n8
             */
            cpu->registers.b = cpu_fetch8(cpu);
            return 8;

        case 0x0E:
            /*
             * LD C,n8
             */
            cpu->registers.c = cpu_fetch8(cpu);
            return 8;

        case 0x16:
            /*
             * LD D,n8
             */
            cpu->registers.d = cpu_fetch8(cpu);
            return 8;

        case 0x1E:
            /*
             * LD E,n8
             */
            cpu->registers.e = cpu_fetch8(cpu);
            return 8;

        case 0x26:
            /*
             * LD H,n8
             */
            cpu->registers.h = cpu_fetch8(cpu);
            return 8;

        case 0x2E:
            /*
             * LD L,n8
             */
            cpu->registers.l = cpu_fetch8(cpu);
            return 8;

        case 0x36:
            /*
             * LD [HL],n8
             */
            cpu_write8(
                cpu,
                cpu_get_hl(cpu),
                cpu_fetch8(cpu)
            );
            return 12;

        case 0x3E:
            /*
             * LD A,n8
             */
            cpu->registers.a = cpu_fetch8(cpu);
            return 8;

        case 0xE0:
            /*
             * LDH [a8],A
             */
            {
                uint8_t offset = cpu_fetch8(cpu);
                uint16_t address = (uint16_t)(0xFF00U + offset);

                cpu_write8(cpu, address, cpu->registers.a);
            }
            return 12;

        case 0xF0:
            /*
             * LDH A,[a8]
             */
            {
                uint8_t offset = cpu_fetch8(cpu);
                uint16_t address = (uint16_t)(0xFF00U + offset);

                cpu->registers.a = cpu_read8(cpu, address);
            }
            return 12;


        /*
         * 16-bit loads
         *
         * LD r16,n16
         */

        case 0x01:
            /*
             * LD BC,d16
             */
            cpu_set_bc(cpu, cpu_fetch16(cpu));
            return 12;

        case 0x11:
            /*
             * LD DE,d16
             */
            cpu_set_de(cpu, cpu_fetch16(cpu));
            return 12;

        case 0x21:
            /*
             * LD HL,d16
             */
            cpu_set_hl(cpu, cpu_fetch16(cpu));
            return 12;

        case 0x31:
            /*
             * LD SP,d16
             */
            cpu->registers.sp = cpu_fetch16(cpu);
            return 12;


        /*
         * LD [r16],A
         */

        case 0x02:
            /*
             * LD [BC],A
             */
            cpu_write8(
                cpu,
                cpu_get_bc(cpu),
                cpu->registers.a
            );
            return 8;

        case 0x12:
            /*
             * LD [DE],A
             */
            cpu_write8(
                cpu,
                cpu_get_de(cpu),
                cpu->registers.a
            );
            return 8;


        /*
         * LD A,[r16]
         */

        case 0x0A:
            /*
             * LD A,[BC]
             */
            cpu->registers.a = cpu_read8(
                cpu,
                cpu_get_bc(cpu)
            );
            return 8;

        case 0x1A:
            /*
             * LD A,[DE]
             */
            cpu->registers.a = cpu_read8(
                cpu,
                cpu_get_de(cpu)
            );
            return 8;


        /*
         * LD [HL+],A
         */

        case 0x22:
            /*
             * LD [HLI],A
             */
            cpu_write8(
                cpu,
                cpu_get_hl(cpu),
                cpu->registers.a
            );

            cpu_set_hl(
                cpu,
                (uint16_t)(cpu_get_hl(cpu) + 1)
            );

            return 8;


        /*
         * LD [HL-],A
         */

        case 0x32:
            /*
             * LD [HLD],A
             */
            cpu_write8(
                cpu,
                cpu_get_hl(cpu),
                cpu->registers.a
            );

            cpu_set_hl(
                cpu,
                (uint16_t)(cpu_get_hl(cpu) - 1)
            );

            return 8;


        /*
         * LD A,[HL+]
         */

        case 0x2A:
            /*
             * LD A,[HLI]
             */
            cpu->registers.a = cpu_read8(
                cpu,
                cpu_get_hl(cpu)
            );

            cpu_set_hl(
                cpu,
                (uint16_t)(cpu_get_hl(cpu) + 1)
            );

            return 8;


        /*
         * LD A,[HL-]
         */

        case 0x3A:
            /*
             * LD A,[HLD]
             */
            cpu->registers.a = cpu_read8(
                cpu,
                cpu_get_hl(cpu)
            );

            cpu_set_hl(
                cpu,
                (uint16_t)(cpu_get_hl(cpu) - 1)
            );

            return 8;


        /*
         * INC r8
         *
         * Encoding:
         *
         * 00RRR100
         *
         * RRR:
         *
         * 000 = B
         * 001 = C
         * 010 = D
         * 011 = E
         * 100 = H
         * 101 = L
         * 110 = [HL]
         * 111 = A
         */

        case 0x04:
            /*
             * INC B
             */
            cpu->registers.b = cpu_inc8(
                cpu,
                cpu->registers.b
            );
            return 4;

        case 0x0C:
            /*
             * INC C
             */
            cpu->registers.c = cpu_inc8(
                cpu,
                cpu->registers.c
            );
            return 4;

        case 0x14:
            /*
             * INC D
             */
            cpu->registers.d = cpu_inc8(
                cpu,
                cpu->registers.d
            );
            return 4;

        case 0x1C:
            /*
             * INC E
             */
            cpu->registers.e = cpu_inc8(
                cpu,
                cpu->registers.e
            );
            return 4;

        case 0x24:
            /*
             * INC H
             */
            cpu->registers.h = cpu_inc8(
                cpu,
                cpu->registers.h
            );
            return 4;

        case 0x2C:
            /*
             * INC L
             */
            cpu->registers.l = cpu_inc8(
                cpu,
                cpu->registers.l
            );
            return 4;

        case 0x34:
            /*
             * INC [HL]
             */
            {
                uint16_t address = cpu_get_hl(cpu);
                uint8_t value = cpu_read8(cpu, address);

                value = cpu_inc8(cpu, value);

                cpu_write8(cpu, address, value);
            }

            return 12;

        case 0x3C:
            /*
             * INC A
             */
            cpu->registers.a = cpu_inc8(
                cpu,
                cpu->registers.a
            );
            return 4;


        /*
         * DEC r8
         */

        case 0x05:
            /*
             * DEC B
             */
            cpu->registers.b = cpu_dec8(
                cpu,
                cpu->registers.b
            );
            return 4;

        case 0x0D:
            /*
             * DEC C
             */
            cpu->registers.c = cpu_dec8(
                cpu,
                cpu->registers.c
            );
            return 4;

        case 0x15:
            /*
             * DEC D
             */
            cpu->registers.d = cpu_dec8(
                cpu,
                cpu->registers.d
            );
            return 4;

        case 0x1D:
            /*
             * DEC E
             */
            cpu->registers.e = cpu_dec8(
                cpu,
                cpu->registers.e
            );
            return 4;

        case 0x25:
            /*
             * DEC H
             */
            cpu->registers.h = cpu_dec8(
                cpu,
                cpu->registers.h
            );
            return 4;

        case 0x2D:
            /*
             * DEC L
             */
            cpu->registers.l = cpu_dec8(
                cpu,
                cpu->registers.l
            );
            return 4;

        case 0x35:
            /*
             * DEC [HL]
             */
            {
                uint16_t address = cpu_get_hl(cpu);
                uint8_t value = cpu_read8(cpu, address);

                value = cpu_dec8(cpu, value);

                cpu_write8(cpu, address, value);
            }

            return 12;

        case 0x3D:
            /*
             * DEC A
             */
            cpu->registers.a = cpu_dec8(
                cpu,
                cpu->registers.a
            );
            return 4;


        /*
         * INC r16
         *
         * These instructions do not modify flags.
         */

        case 0x03:
            /*
             * INC BC
             */
            cpu_set_bc(
                cpu,
                (uint16_t)(cpu_get_bc(cpu) + 1)
            );
            return 8;

        case 0x13:
            /*
             * INC DE
             */
            cpu_set_de(
                cpu,
                (uint16_t)(cpu_get_de(cpu) + 1)
            );
            return 8;

        case 0x23:
            /*
             * INC HL
             */
            cpu_set_hl(
                cpu,
                (uint16_t)(cpu_get_hl(cpu) + 1)
            );
            return 8;

        case 0x33:
            /*
             * INC SP
             */
            cpu->registers.sp =
                (uint16_t)(cpu->registers.sp + 1);

            return 8;


        /*
         * DEC r16
         *
         * These instructions do not modify flags.
         */

        case 0x0B:
            /*
             * DEC BC
             */
            cpu_set_bc(
                cpu,
                (uint16_t)(cpu_get_bc(cpu) - 1)
            );
            return 8;

        case 0x1B:
            /*
             * DEC DE
             */
            cpu_set_de(
                cpu,
                (uint16_t)(cpu_get_de(cpu) - 1)
            );
            return 8;

        case 0x2B:
            /*
             * DEC HL
             */
            cpu_set_hl(
                cpu,
                (uint16_t)(cpu_get_hl(cpu) - 1)
            );
            return 8;

        case 0x3B:
            /*
             * DEC SP
             */
            cpu->registers.sp =
                (uint16_t)(cpu->registers.sp - 1);

            return 8;


        /*
         * Control flow
         */

        case 0xC3:
            /*
             * JP a16
             */
            cpu->registers.pc = cpu_fetch16(cpu);
            return 16;

        case 0xD9:
            /*
             * RETI
             */
            cpu->registers.pc = cpu_pop16(cpu);
            cpu->ime = true;
            cpu->ime_enable_delay = 0;
            return 16;

        case 0x18:
            /*
             * JR e8
             */
            return cpu_jump_relative(cpu, true);

        case 0x20:
            /*
             * JR NZ,e8
             */
            return cpu_jump_relative(
                cpu,
                (cpu->registers.f & FLAG_Z) == 0
            );

        case 0x28:
            /*
             * JR Z,e8
             */
            return cpu_jump_relative(
                cpu,
                (cpu->registers.f & FLAG_Z) != 0
            );

        case 0x30:
            /*
             * JR NC,e8
             */
            return cpu_jump_relative(
                cpu,
                (cpu->registers.f & FLAG_C) == 0
            );

        case 0x38:
            /*
             * JR C,e8
             */
            return cpu_jump_relative(
                cpu,
                (cpu->registers.f & FLAG_C) != 0
            );

        default:
            break;
    }


    /*
     * LD r8,r8
     *
     * Encoding:
     *
     * 01DDDSSS
     *
     * DDD = destination
     * SSS = source
     *
     * Register encoding:
     *
     * 000 = B
     * 001 = C
     * 010 = D
     * 011 = E
     * 100 = H
     * 101 = L
     * 110 = [HL]
     * 111 = A
     *
     * 0x76 is HALT and was handled above.
     */

    if (opcode >= 0x40 && opcode <= 0x7F) {
        uint8_t destination =
            (uint8_t)((opcode >> 3) & 0x07);

        uint8_t source =
            (uint8_t)(opcode & 0x07);

        uint8_t value = cpu_read_r8(
            cpu,
            source
        );

        cpu_write_r8(
            cpu,
            destination,
            value
        );

        if (source == 6 || destination == 6) {
            return 8;
        }

        return 4;
    }


    /*
     * Opcode not implemented.
     */

    cpu->step_status = CPU_STEP_UNIMPLEMENTED_OPCODE;

    return 0;
}

static CpuCycles cpu_jump_relative(CPU *cpu, bool condition)
{
    int8_t offset = (int8_t)cpu_fetch8(cpu);

    if (condition) {
        int32_t target =
            (int32_t)cpu->registers.pc + (int32_t)offset;

        cpu->registers.pc = (uint16_t)target;
        return 12;
    }

    return 8;
}


/*
 * Register pair helpers
 */

static uint16_t cpu_get_bc(const CPU *cpu)
{
    return ((uint16_t)cpu->registers.b << 8) |
           cpu->registers.c;
}

static uint16_t cpu_get_de(const CPU *cpu)
{
    return ((uint16_t)cpu->registers.d << 8) |
           cpu->registers.e;
}

static uint16_t cpu_get_hl(const CPU *cpu)
{
    return ((uint16_t)cpu->registers.h << 8) |
           cpu->registers.l;
}

static void cpu_set_bc(CPU *cpu, uint16_t value)
{
    cpu->registers.b = (uint8_t)(value >> 8);
    cpu->registers.c = (uint8_t)(value & 0xFF);
}

static void cpu_set_de(CPU *cpu, uint16_t value)
{
    cpu->registers.d = (uint8_t)(value >> 8);
    cpu->registers.e = (uint8_t)(value & 0xFF);
}

static void cpu_set_hl(CPU *cpu, uint16_t value)
{
    cpu->registers.h = (uint8_t)(value >> 8);
    cpu->registers.l = (uint8_t)(value & 0xFF);
}


/*
 * Flag helpers
 */

static void cpu_set_flag(CPU *cpu, Flag flag, bool value)
{
    if (value) {
        cpu->registers.f |= (uint8_t)flag;
    } else {
        cpu->registers.f &= (uint8_t)~(uint8_t)flag;
    }

    cpu_normalize_flags(cpu);
}

static void cpu_normalize_flags(CPU *cpu)
{
    cpu->registers.f &= 0xF0U;
}


/*
 * 8-bit arithmetic helpers
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

static uint8_t cpu_inc8(CPU *cpu, uint8_t value)
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

static uint8_t cpu_dec8(CPU *cpu, uint8_t value)
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


/*
 * Generic 8-bit register access
 *
 * Index:
 *
 *   0 = B
 *   1 = C
 *   2 = D
 *   3 = E
 *   4 = H
 *   5 = L
 *   6 = [HL]
 *   7 = A
 */

static uint8_t cpu_read_r8(CPU *cpu, uint8_t index)
{
    switch (index) {
        case 0:
            return cpu->registers.b;

        case 1:
            return cpu->registers.c;

        case 2:
            return cpu->registers.d;

        case 3:
            return cpu->registers.e;

        case 4:
            return cpu->registers.h;

        case 5:
            return cpu->registers.l;

        case 6:
            return cpu_read8(
                cpu,
                cpu_get_hl(cpu)
            );

        case 7:
            return cpu->registers.a;

        default:
            return 0;
    }
}

static void cpu_write_r8(
    CPU *cpu,
    uint8_t index,
    uint8_t value
)
{
    switch (index) {
        case 0:
            cpu->registers.b = value;
            break;

        case 1:
            cpu->registers.c = value;
            break;

        case 2:
            cpu->registers.d = value;
            break;

        case 3:
            cpu->registers.e = value;
            break;

        case 4:
            cpu->registers.h = value;
            break;

        case 5:
            cpu->registers.l = value;
            break;

        case 6:
            cpu_write8(
                cpu,
                cpu_get_hl(cpu),
                value
            );
            break;

        case 7:
            cpu->registers.a = value;
            break;

        default:
            break;
    }
}
