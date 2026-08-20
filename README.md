# Game Boy Emulator Core

An incremental C23 Game Boy emulator core focused on establishing a correct CPU, Bus, Memory, Cartridge, interrupt, and Timer architecture before adding larger hardware subsystems.

This project is intentionally developed in small, testable stages. It does not yet represent a complete Game Boy emulator.

## Current Architecture

```text
main.c
  |
  v
emulator.h
  |
  v
emulator.c
  |
  +-- CPU
  +-- Bus
  +-- Memory
  +-- Cartridge
  +-- InterruptRegisters
  +-- Timer
```

`main.c` only uses the opaque `Emulator` API. `Emulator` owns the machine components by value. The Bus routes accesses to Cartridge, Memory, Timer, and interrupt registers; CPU does not know the Timer directly.

The main stepping flow is:

```text
emulator_step()
    |
    +--> cpu_step()
    |       |
    |       +--> CpuCycles
    |
    +--> timer_step(cycles)
```

## Implemented Features

### Emulator

- Opaque `Emulator` type.
- Create, ROM load, step, run, stop, and destroy operations.
- Loading a ROM resets CPU, Memory, Timer, and interrupt state.
- Failed ROM loads preserve the previously loaded Cartridge.

### Cartridge

- ROM file loading.
- Transactional replacement when loading a new ROM.
- Safe initialization and destruction.
- Read access with `0xFF` returned outside the loaded ROM size.

MBCs and cartridge RAM banking are not implemented.

### CPU

Implemented instruction groups include:

- `NOP`.
- `HALT`.
- `DI`, `EI`, and `RETI`.
- `JP a16`.
- `JR e8`.
- `JR NZ,e8`, `JR Z,e8`, `JR NC,e8`, and `JR C,e8`.
- `LD r8,n8`.
- `LD r16,d16`.
- Generic `LD r8,r8` block.
- `LD [BC],A`, `LD [DE],A`.
- `LD A,[BC]`, `LD A,[DE]`.
- `LD [HL+],A`, `LD [HL-],A`.
- `LD A,[HL+]`, `LD A,[HL-]`.
- `LDH [a8],A` and `LDH A,[a8]`.
- `XOR A`.
- `INC r8`, `DEC r8`.
- `INC r16`, `DEC r16`.

The CPU currently does not implement PUSH, POP, CALL, RET, RST, CB-prefixed instructions, or the remaining ALU and load families.

`CpuCycles` remains a `uint8_t`-based type. Tests verify instruction results, PC updates, flags, memory effects, and timing.

### Interrupts

Implemented interrupt registers:

- `IF` at `0xFF0F`.
- `IE` at `0xFFFF`.

Implemented interrupt sources and vectors:

| Source | Bit | Vector |
|---|---:|---:|
| VBlank | 0 | `0x0040` |
| LCD STAT | 1 | `0x0048` |
| Timer | 2 | `0x0050` |
| Serial | 3 | `0x0058` |
| Joypad | 4 | `0x0060` |

Interrupt service includes priority selection, selective IF clearing, IME clearing, PC push, vector transfer, and 20-cycle timing.

HALT behavior distinguishes:

- CPU halted without pending interrupt.
- HALT wake-up when an interrupt is pending but IME is disabled.
- Direct interrupt service when IME is enabled.

`EI` uses delayed IME enable semantics. `DI` cancels IME and pending enable. `RETI` restores PC from the stack and enables IME.

### Timer

Implemented registers:

| Register | Address |
|---|---:|
| DIV | `0xFF04` |
| TIMA | `0xFF05` |
| TMA | `0xFF06` |
| TAC | `0xFF07` |

The Timer includes:

- Internal 16-bit divider.
- DIV reads exposing the high byte.
- DIV reset on write.
- TAC frequency selection.
- Falling-edge-based TIMA increments.
- Falling-edge behavior for DIV and TAC writes.
- Delayed TIMA reload after overflow.
- TIMA write cancellation during the reload window.
- TMA write behavior during reload.
- Timer interrupt requests through `IF.TIMER`.
- Advancement during normal CPU execution, HALT, and interrupt service.

The Timer currently models the DMG-style normal-speed timing path. STOP mode and CGB double-speed behavior are not implemented.

## Memory Map Currently Used

```text
0000-7FFF   Cartridge ROM
C000-DFFF   Work RAM
FF04-FF07   Timer registers
FF0F        Interrupt Flag (IF)
FF80-FFFE   High RAM
FFFF        Interrupt Enable (IE)
```

Other regions are currently unimplemented or return `0xFF` according to the component being accessed.

## Project Layout

```text
include/                Public and internal module headers
src/main.c              CLI/application entry point
src/emulator.c          Emulator composition and lifecycle
src/emulator_internal.h Private Emulator definition
src/cpu.c               CPU decoder and instruction implementation
src/bus.c               Address routing
src/memory.c            WRAM and HRAM
src/cartridge.c         ROM ownership and loading
src/timer.c             Timer implementation
tests/                  Unit and integration tests
roms/                   Local test ROMs
opcodes.json            Unprefixed and CB opcode reference data
makefile                Build and test rules
```

## Requirements

- GCC or another C23-capable compiler.
- GNU Make.
- A POSIX-like shell for the current Makefile commands.

The Makefile enables strict diagnostics, including:

- `-Wall`
- `-Wextra`
- `-Wpedantic`
- `-Werror`
- `-fanalyzer`
- `-Wconversion`
- `-Wsign-conversion`
- `-Wshadow`
- `-Wformat=2`
- `-Wundef`
- `-Wcast-qual`
- `-Wcast-align`
- `-Wwrite-strings`
- `-Wstrict-prototypes`
- `-Wmissing-prototypes`

## Build

From this directory:

```sh
make
```

This builds the `gameboy` executable.

## Run

Pass a ROM path to the executable:

```sh
./gameboy roms/01-special.gb
```

The emulator currently runs a headless CPU/machine loop. A ROM that reaches HALT can stop the current execution loop behavior; future hardware integration will separate CPU HALT from complete machine shutdown more fully.

## Tests

Run the complete test suite:

```sh
make test
```

Run a clean rebuild and test:

```sh
make clean
make
make test
```

The test suite includes:

- Cartridge lifecycle and transactional loading.
- CPU instruction behavior and timing.
- Memory and Bus boundaries.
- Interrupt priority, service, HALT wake-up, EI, DI, and RETI.
- Timer registers, frequencies, falling edges, overflow reload, and IF requests.
- Emulator integration for interrupts and Timer-to-CPU service.

Individual test targets include:

```sh
make test_cpu
make test_cpu_instructions
make test_interrupts
make test_timer
make test_emulator
make test_emulator_interrupts
make test_emulator_timer
make test_memory_bus
make test_cartridge
```

## Coverage and Limitations

The CPU is being expanded incrementally. Current limitations include:

- PUSH/POP are not implemented as instructions.
- CALL/RET and RST are not implemented.
- CB-prefixed instructions are not implemented.
- Most ALU instructions are not implemented.
- STOP is not implemented.
- MBCs and cartridge RAM banking are not implemented.
- PPU, DMA, Joypad, and Serial hardware are not implemented in this project.
- No Mooneye test ROM harness is included.
- External ROM compatibility has not been established by this repository alone.

The presence of a test or module does not imply complete Game Boy hardware compatibility; timing and hardware behavior remain under incremental validation.

## Development Direction

The current architectural sequence is:

1. Stabilize CPU, Bus, Memory, Cartridge, interrupts, and Timer.
2. Expand CPU coverage in small instruction families.
3. Validate against external CPU/Timer/interrupt test ROMs when the required instruction coverage and harness are available.
4. Add larger hardware subsystems only after the core timing contracts are validated.
