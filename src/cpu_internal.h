#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <cpu.h>

/*
 * Machine access and timing (cpu.c).
 *
 * Every access below consumes one M-cycle: the bus is accessed first and
 * the machine is then ticked for 4 T-cycles.
 */
void cpu_tick(CPU *cpu, CpuCycles cycles);
void cpu_idle(CPU *cpu);
uint8_t cpu_read8(CPU *cpu, uint16_t address);
void cpu_write8(CPU *cpu, uint16_t address, uint8_t value);
uint8_t cpu_fetch8(CPU *cpu);
uint16_t cpu_fetch16(CPU *cpu);
void cpu_push16(CPU *cpu, uint16_t value);
uint16_t cpu_pop16(CPU *cpu);

/* Registers and flags (cpu.c). */
uint16_t cpu_get_hl(const CPU *cpu);
void cpu_set_hl(CPU *cpu, uint16_t value);

/* r8 index: 0=B 1=C 2=D 3=E 4=H 5=L 6=[HL] 7=A. Index 6 accesses memory. */
uint8_t cpu_read_r8(CPU *cpu, uint8_t index);
void cpu_write_r8(CPU *cpu, uint8_t index, uint8_t value);

/* r16 index: 0=BC 1=DE 2=HL 3=SP. */
uint16_t cpu_read_r16(const CPU *cpu, uint8_t index);
void cpu_write_r16(CPU *cpu, uint8_t index, uint16_t value);

/* Stack r16 index: 0=BC 1=DE 2=HL 3=AF. */
uint16_t cpu_read_r16_stack(const CPU *cpu, uint8_t index);
void cpu_write_r16_stack(CPU *cpu, uint8_t index, uint16_t value);

bool cpu_get_flag(const CPU *cpu, Flag flag);
void cpu_set_flag(CPU *cpu, Flag flag, bool value);
void cpu_set_flags(CPU *cpu, bool z, bool n, bool h, bool c);

/* Condition index: 0=NZ 1=Z 2=NC 3=C. */
bool cpu_condition(const CPU *cpu, uint8_t index);

/* ALU (cpu_alu.c). Flag behavior follows the SM83. */
uint8_t cpu_inc8(CPU *cpu, uint8_t value);
uint8_t cpu_dec8(CPU *cpu, uint8_t value);

/* op: 0=ADD 1=ADC 2=SUB 3=SBC 4=AND 5=XOR 6=OR 7=CP, on register A. */
void cpu_alu_a(CPU *cpu, uint8_t op, uint8_t value);

/* op: 0=RLC 1=RRC 2=RL 3=RR 4=SLA 5=SRA 6=SWAP 7=SRL. Sets Z N H C. */
uint8_t cpu_rotate_shift(CPU *cpu, uint8_t op, uint8_t value);

uint16_t cpu_add_hl(CPU *cpu, uint16_t hl, uint16_t value);

/* SP plus a signed offset byte; sets Z N H C as ADD SP,e8 does. */
uint16_t cpu_sp_plus_offset(CPU *cpu, uint8_t offset);

void cpu_daa(CPU *cpu);

/* Instruction decoders (cpu_ops.c, cpu_cb.c). */
bool cpu_opcode_is_illegal(uint8_t opcode);
void cpu_execute_opcode(CPU *cpu, uint8_t opcode);
void cpu_execute_cb(CPU *cpu);

#endif
