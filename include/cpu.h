#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

#include <cycles.h>

typedef struct Bus Bus;

typedef enum {
    FLAG_Z = 0x80,
    FLAG_N = 0x40,
    FLAG_H = 0x20,
    FLAG_C = 0x10
} Flag;

typedef enum {
    CPU_STEP_EXECUTED,
    CPU_STEP_HALTED,
    /* HALT ended because IF & IE has a valid pending bit. */
    CPU_STEP_WOKE_FROM_HALT,
    CPU_STEP_INTERRUPT_SERVICED,
    /* STOP is active and no joypad interrupt has been requested. */
    CPU_STEP_STOPPED,
    /*
     * The opcode is one of the 11 undefined ones. The step consumed no
     * time and PC still points at the opcode; see fault_pc/fault_opcode.
     */
    CPU_STEP_UNIMPLEMENTED_OPCODE
} CpuStepStatus;

typedef struct {
    uint8_t a;
    uint8_t f;

    uint8_t b;
    uint8_t c;

    uint8_t d;
    uint8_t e;

    uint8_t h;
    uint8_t l;

    uint16_t sp;
    uint16_t pc;
} Registers;

/*
 * Called after every M-cycle (4 T-cycles) the CPU consumes, right after
 * the bus access of that cycle, so the machine components advance in
 * lockstep with the instruction.
 */
typedef void (*CpuTickFn)(void *context, CpuCycles cycles);

typedef struct {
    Registers registers;

    Bus *bus;

    bool halted;
    bool stopped;
    bool ime; /* Interrupt Master Enable. */
    uint8_t ime_enable_delay;
    CpuStepStatus step_status;

    /* T-cycles consumed by the current cpu_step(). */
    CpuCycles step_cycles;

    CpuTickFn tick;
    void *tick_context;

    uint16_t fault_pc;
    uint8_t fault_opcode;
} CPU;

/* Resets the CPU to the post-boot state. Clears the tick handler. */
void cpu_init(CPU *cpu, Bus *bus);

void cpu_set_tick_handler(CPU *cpu, CpuTickFn tick, void *context);

/*
 * Executes one instruction, one interrupt dispatch, or one halted cycle
 * and returns the T-cycles it took. Timing is modelled per M-cycle: each
 * bus access is followed by a 4 T-cycle tick, and internal delays are
 * explicit idle ticks, so a bus access observes the machine as it is
 * after all the earlier M-cycles of the same instruction.
 */
CpuCycles cpu_step(CPU *cpu);

#endif
