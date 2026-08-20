#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

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
    CPU_STEP_UNIMPLEMENTED_OPCODE
} CpuStepStatus;

typedef uint8_t CpuCycles;

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

typedef struct {
    Registers registers;

    Bus *bus;

    bool halted;
    bool stopped;
    bool ime; /* Interrupt Master Enable. */
    uint8_t ime_enable_delay;
    CpuStepStatus step_status;
} CPU;

void cpu_init(CPU *cpu, Bus *bus);

/* A halted CPU consumes a step without fetching a new instruction. */
CpuCycles cpu_step(CPU *cpu);

#endif
