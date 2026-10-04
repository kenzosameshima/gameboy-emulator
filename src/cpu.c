#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bus.h>
#include <cpu.h>
#include <interrupts.h>

#include "cpu_internal.h"

/*
 * Private helpers
 */

static void cpu_normalize_flags(CPU *cpu);
static void cpu_advance_ime_delay(CPU *cpu);
static void cpu_service_interrupt(CPU *cpu, uint8_t pending);


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
    cpu->step_cycles = 0;

    cpu->tick = NULL;
    cpu->tick_context = NULL;

    cpu->fault_pc = 0;
    cpu->fault_opcode = 0;
}

void cpu_set_tick_handler(CPU *cpu, CpuTickFn tick, void *context)
{
    cpu->tick = tick;
    cpu->tick_context = context;
}


/*
 * Machine timing and memory access
 */

void cpu_tick(CPU *cpu, CpuCycles cycles)
{
    cpu->step_cycles = (CpuCycles)(cpu->step_cycles + cycles);

    if (cpu->tick != NULL) {
        cpu->tick(cpu->tick_context, cycles);
    }
}

void cpu_idle(CPU *cpu)
{
    cpu_tick(cpu, CYCLES_PER_MCYCLE);
}

uint8_t cpu_read8(CPU *cpu, uint16_t address)
{
    uint8_t value = bus_read(cpu->bus, address);

    cpu_tick(cpu, CYCLES_PER_MCYCLE);

    return value;
}

void cpu_write8(CPU *cpu, uint16_t address, uint8_t value)
{
    bus_write(cpu->bus, address, value);

    cpu_tick(cpu, CYCLES_PER_MCYCLE);
}

uint8_t cpu_fetch8(CPU *cpu)
{
    uint8_t value = cpu_read8(cpu, cpu->registers.pc);

    cpu->registers.pc++;

    return value;
}

uint16_t cpu_fetch16(CPU *cpu)
{
    uint8_t low = cpu_fetch8(cpu);
    uint8_t high = cpu_fetch8(cpu);

    return (uint16_t)(((uint16_t)high << 8) | low);
}

void cpu_push16(CPU *cpu, uint16_t value)
{
    cpu->registers.sp--;
    cpu_write8(cpu, cpu->registers.sp, (uint8_t)(value >> 8));

    cpu->registers.sp--;
    cpu_write8(cpu, cpu->registers.sp, (uint8_t)(value & 0xFF));
}

uint16_t cpu_pop16(CPU *cpu)
{
    uint8_t low = cpu_read8(cpu, cpu->registers.sp);
    cpu->registers.sp++;

    uint8_t high = cpu_read8(cpu, cpu->registers.sp);
    cpu->registers.sp++;

    return (uint16_t)(((uint16_t)high << 8) | low);
}


/*
 * Instruction execution
 */

CpuCycles cpu_step(CPU *cpu)
{
    cpu_normalize_flags(cpu);

    cpu->step_cycles = 0;

    uint8_t pending = interrupts_pending(cpu->bus->interrupts);

    if (cpu->ime && pending != 0) {
        cpu_service_interrupt(cpu, pending);
        return cpu->step_cycles;
    }

    if (cpu->halted) {
        if (pending != 0) {
            cpu->halted = false;
            cpu->step_status = CPU_STEP_WOKE_FROM_HALT;
        } else {
            cpu->step_status = CPU_STEP_HALTED;
        }

        cpu_idle(cpu);
        return cpu->step_cycles;
    }

    if (cpu->stopped) {
        /* Only a joypad line going low ends STOP. */
        if ((cpu->bus->interrupts->interrupt_flag & INTERRUPT_JOYPAD) != 0) {
            cpu->stopped = false;
        } else {
            cpu->step_status = CPU_STEP_STOPPED;
            cpu_idle(cpu);
            return cpu->step_cycles;
        }
    }

    cpu->step_status = CPU_STEP_EXECUTED;

    /*
     * Undefined opcodes are detected before any time is consumed, so PC
     * keeps pointing at the faulting instruction.
     */
    uint8_t opcode = bus_read(cpu->bus, cpu->registers.pc);

    if (cpu_opcode_is_illegal(opcode)) {
        cpu->fault_pc = cpu->registers.pc;
        cpu->fault_opcode = opcode;
        cpu->step_status = CPU_STEP_UNIMPLEMENTED_OPCODE;
        return 0;
    }

    cpu->registers.pc++;
    cpu_idle(cpu);

    cpu_execute_opcode(cpu, opcode);

    if (cpu->step_status == CPU_STEP_EXECUTED) {
        cpu_advance_ime_delay(cpu);
    }

    return cpu->step_cycles;
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

/*
 * Interrupt dispatch takes 5 M-cycles: two internal delays, the two
 * stack writes, and the load of the vector into PC.
 */
static void cpu_service_interrupt(CPU *cpu, uint8_t pending)
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

    cpu->ime = false;
    cpu->ime_enable_delay = 0;
    cpu->halted = false;

    cpu_idle(cpu);
    cpu_idle(cpu);
    cpu_push16(cpu, cpu->registers.pc);

    interrupts_acknowledge(cpu->bus->interrupts, interrupt_mask);

    cpu->registers.pc = vector;
    cpu_idle(cpu);

    cpu->step_status = CPU_STEP_INTERRUPT_SERVICED;
}


/*
 * Register pairs
 */

static uint16_t cpu_get_bc(const CPU *cpu)
{
    return (uint16_t)(
        ((uint16_t)cpu->registers.b << 8) | cpu->registers.c
    );
}

static uint16_t cpu_get_de(const CPU *cpu)
{
    return (uint16_t)(
        ((uint16_t)cpu->registers.d << 8) | cpu->registers.e
    );
}

static uint16_t cpu_get_af(const CPU *cpu)
{
    return (uint16_t)(
        ((uint16_t)cpu->registers.a << 8) | cpu->registers.f
    );
}

uint16_t cpu_get_hl(const CPU *cpu)
{
    return (uint16_t)(
        ((uint16_t)cpu->registers.h << 8) | cpu->registers.l
    );
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

/* The low nibble of F is always zero. */
static void cpu_set_af(CPU *cpu, uint16_t value)
{
    cpu->registers.a = (uint8_t)(value >> 8);
    cpu->registers.f = (uint8_t)(value & 0xF0);
}

void cpu_set_hl(CPU *cpu, uint16_t value)
{
    cpu->registers.h = (uint8_t)(value >> 8);
    cpu->registers.l = (uint8_t)(value & 0xFF);
}

uint16_t cpu_read_r16(const CPU *cpu, uint8_t index)
{
    switch (index) {
        case 0:
            return cpu_get_bc(cpu);

        case 1:
            return cpu_get_de(cpu);

        case 2:
            return cpu_get_hl(cpu);

        default:
            return cpu->registers.sp;
    }
}

void cpu_write_r16(CPU *cpu, uint8_t index, uint16_t value)
{
    switch (index) {
        case 0:
            cpu_set_bc(cpu, value);
            break;

        case 1:
            cpu_set_de(cpu, value);
            break;

        case 2:
            cpu_set_hl(cpu, value);
            break;

        default:
            cpu->registers.sp = value;
            break;
    }
}

uint16_t cpu_read_r16_stack(const CPU *cpu, uint8_t index)
{
    return index == 3 ? cpu_get_af(cpu) : cpu_read_r16(cpu, index);
}

void cpu_write_r16_stack(CPU *cpu, uint8_t index, uint16_t value)
{
    if (index == 3) {
        cpu_set_af(cpu, value);
    } else {
        cpu_write_r16(cpu, index, value);
    }
}


/*
 * Flags
 */

bool cpu_get_flag(const CPU *cpu, Flag flag)
{
    return (cpu->registers.f & flag) != 0;
}

void cpu_set_flag(CPU *cpu, Flag flag, bool value)
{
    if (value) {
        cpu->registers.f |= (uint8_t)flag;
    } else {
        cpu->registers.f &= (uint8_t)~flag;
    }
}

void cpu_set_flags(CPU *cpu, bool z, bool n, bool h, bool c)
{
    cpu->registers.f = (uint8_t)(
        (z ? FLAG_Z : 0) |
        (n ? FLAG_N : 0) |
        (h ? FLAG_H : 0) |
        (c ? FLAG_C : 0)
    );
}

/* The low nibble of F is always zero. */
static void cpu_normalize_flags(CPU *cpu)
{
    cpu->registers.f &= 0xF0;
}

bool cpu_condition(const CPU *cpu, uint8_t index)
{
    switch (index) {
        case 0:
            return !cpu_get_flag(cpu, FLAG_Z);

        case 1:
            return cpu_get_flag(cpu, FLAG_Z);

        case 2:
            return !cpu_get_flag(cpu, FLAG_C);

        default:
            return cpu_get_flag(cpu, FLAG_C);
    }
}


/*
 * Generic 8-bit register access
 */

uint8_t cpu_read_r8(CPU *cpu, uint8_t index)
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
            return cpu_read8(cpu, cpu_get_hl(cpu));

        default:
            return cpu->registers.a;
    }
}

void cpu_write_r8(CPU *cpu, uint8_t index, uint8_t value)
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
            cpu_write8(cpu, cpu_get_hl(cpu), value);
            break;

        default:
            cpu->registers.a = value;
            break;
    }
}
