# Game Boy Emulator Core

An incremental C23 Game Boy emulator core. It has a complete SM83 CPU, a Bus, Memory, a Cartridge with MBC1, interrupts, a Timer and a Serial port, and it passes Blargg's CPU instruction and memory timing test ROMs.

The core is headless: there is no PPU, joypad, DMA or audio yet, so games cannot be played. Test ROMs run because they report their results through the serial port.

## Current Architecture

```text
main.c            tools/rom_test.c
    \                /
     v              v
          emulator.h
              |
              v
          emulator.c  ---- owns and steps every component
              |
              +-- CPU  (cpu.c, cpu_ops.c, cpu_cb.c, cpu_alu.c)
              +-- Bus
              +-- Memory
              +-- Cartridge (ROM-only, MBC1)
              +-- InterruptRegisters (interrupts.c)
              +-- Timer
              +-- Serial
```

`main.c` and `tools/rom_test.c` only use the opaque `Emulator` API. `Emulator` owns the machine components by value. The Bus routes accesses to Cartridge, Memory, Timer, Serial, and the interrupt registers; the CPU only knows the Bus.

### Timing model

Time is modelled per M-cycle (4 T-cycles). The CPU spends time only through a few primitives: a bus access followed by a 4 T-cycle tick, or an explicit idle tick for an internal delay. After each tick the CPU calls a tick handler, which is how the Emulator advances the Timer and Serial in lockstep with the instruction:

```text
emulator_step()
    |
    +--> cpu_step()
            |
            +--> cpu_read8 / cpu_write8 / cpu_idle     (one M-cycle each)
                    |
                    +--> bus access, then tick handler
                            |
                            +--> emulator_tick(): timer_step, serial_step
```

A bus access therefore sees the machine as it is after all the earlier M-cycles of the same instruction. `cpu_step()` returns the total T-cycles of the step, which is exactly what was ticked. Any future component that runs off the CPU clock (PPU, DMA, APU) is added in `emulator_tick()`.

## Implemented Features

### Emulator

- Opaque `Emulator` type.
- Create, ROM load, step, run, stop, and destroy operations.
- `emulator_run_cycles(n)` runs for a T-cycle budget. `emulator_run()` runs until stopped, stalled, or an error occurs. `emulator_stop()` is async-signal-safe and can be called from the serial callback. A request made before the loop starts is not lost: the next run returns immediately and consumes it.
- Typed results (`EmulatorStatus`) instead of bare integers. `EMULATOR_OK` is zero.
- An undefined opcode reports the exact instruction through `emulator_get_fault()` (PC of the opcode and the opcode itself).
- `EMULATOR_STALLED` reports a CPU halted with nothing able to wake it (HALT with `IE = 0`, or STOP), instead of spinning forever.
- `emulator_set_serial_output()` receives every byte the program sends over the serial port.
- Loading a ROM resets CPU, Memory, Timer, Serial, interrupt state, and the cycle counter. A failed load preserves the previously loaded Cartridge.

### Cartridge

- ROM file loading with transactional replacement.
- Header parsing for the cartridge type and RAM size.
- ROM-only and MBC1 (`0x00`-`0x03`): ROM bank switching (5 + 2 bits, bank 0 remapped to 1), banking mode, RAM enable, and RAM banking. Bank numbers wrap to the ROM size.
- Unsupported cartridge types are rejected at load time instead of running with the wrong mapping. `cartridge_load()` reports why through `CartridgeLoadStatus` (unreadable file, out of memory, unsupported type), `emulator_load_rom()` maps that to distinct `EmulatorStatus` values, and `emulator_get_unsupported_cartridge_type()` returns the header type byte, so `./gameboy "roms/Pokemon Red.gb"` says it is type `0x13` (MBC3).
- Cartridge RAM is not persisted to disk.

### CPU

The full SM83 instruction set is implemented: all 245 defined unprefixed opcodes and all 256 CB-prefixed opcodes. The 11 undefined opcodes (`D3 DB DD E3 E4 EB EC ED F4 FC FD`) stop the emulator without consuming time and leave PC on the opcode.

The decoder is algorithmic. Opcodes are split into `x`, `y`, `z` bit fields, so the 3-bit register operand, the ALU operation, and the CB operations are each handled in one place rather than one `case` per opcode.

`STOP` skips its second byte and stops the CPU until a joypad interrupt is requested. `HALT` and `EI` delay behave as described in the next section.

### Interrupts

Implemented interrupt registers and sources:

| Source | Bit | Vector |
|---|---:|---:|
| VBlank | 0 | `0x0040` |
| LCD STAT | 1 | `0x0048` |
| Timer | 2 | `0x0050` |
| Serial | 3 | `0x0058` |
| Joypad | 4 | `0x0060` |

`IF` is at `0xFF0F` and `IE` at `0xFFFF`. Hardware components raise interrupts with `interrupts_request()`. Dispatch takes 5 M-cycles (20 T-cycles): priority selection, IME clearing, PC push, selective IF clearing, and the vector load.

HALT behavior distinguishes:

- CPU halted without pending interrupt.
- HALT wake-up when an interrupt is pending but IME is disabled.
- Direct interrupt service when IME is enabled.

`EI` uses delayed IME enable semantics. `DI` cancels IME and a pending enable. `RETI` restores PC from the stack and enables IME.

### Timer

Registers: DIV `0xFF04`, TIMA `0xFF05`, TMA `0xFF06`, TAC `0xFF07`.

- Internal 16-bit divider, DIV exposing its high byte, DIV reset on write.
- TAC frequency selection.
- Falling-edge-based TIMA increments, including the falling edges caused by DIV and TAC writes.
- Delayed TIMA reload after overflow, TIMA write cancellation, and TMA write behavior during the reload window.
- Timer interrupt requests through `IF.TIMER`.

The Timer models the DMG normal-speed path. CGB double-speed behavior is not implemented.

### Serial

Registers: SB `0xFF01` and SC `0xFF02`. With no link partner, a transfer started with the internal clock (`SC = 0x81`) reports its byte to the output callback, completes after 4096 T-cycles with `SB = 0xFF`, clears `SC` bit 7, and requests the serial interrupt. Writing `SC = 0x81` again restarts the transfer. The external clock never completes.

## Memory Map Currently Used

```text
0000-7FFF   Cartridge ROM (writes program the mapper)
A000-BFFF   Cartridge RAM
C000-DFFF   Work RAM
E000-FDFF   Echo of C000-DDFF
FF01-FF02   Serial
FF04-FF07   Timer registers
FF0F        Interrupt Flag (IF)
FF80-FFFE   High RAM
FFFF        Interrupt Enable (IE)
```

The constants live in `include/memory_map.h`. Other regions are currently unimplemented and read `0xFF`.

## Project Layout

```text
include/                Module headers (public API is emulator.h)
src/main.c              CLI entry point
src/emulator.c          Emulator composition, lifecycle, run loop, tick handler
src/emulator_internal.h Private Emulator definition
src/cpu.c               CPU step, M-cycle timing, stack, interrupt dispatch
src/cpu_ops.c           Unprefixed opcode decoder
src/cpu_cb.c            CB-prefixed opcode decoder
src/cpu_alu.c           ALU, rotate/shift, DAA, 16-bit arithmetic
src/cpu_internal.h      Private CPU helpers shared by the cpu_*.c files
src/bus.c               Address routing
src/interrupts.c        IF/IE registers and interrupt requests
src/memory.c            WRAM and HRAM
src/cartridge.c         ROM/RAM ownership, loading, MBC1
src/timer.c             Timer implementation
src/serial.c            Serial port
tools/rom_test.c        Headless test ROM runner
tests/                  Unit and integration tests
roms/                   Local test ROMs
opcodes.json            Opcode reference data, used by test_opcode_timing
makefile                Build and test rules
```

## Requirements

- GCC 14 or newer builds with `-std=c23`. Older GCC, such as 13 on Ubuntu 24.04, is detected by the Makefile and builds with `-std=c2x`. Clang works with `make CC=clang`.
- GNU Make (`mingw32-make` from MSYS2 UCRT64 on Windows).
- A POSIX-like shell for the Makefile recipes.

The Makefile enables strict diagnostics: `-Wall -Wextra -Wpedantic -Werror -fanalyzer -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wundef -Wcast-qual -Wcast-align -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes`.

## Build

```sh
make
```

This builds the `gameboy` executable. Objects and the core library `libgbcore.a` go to `build/`. Every test and tool links against that library.

Objects from one platform cannot be reused on another, so when the same checkout is built from both Windows and WSL, give each its own directory. Building under `/tmp` in WSL is also much faster than under `/mnt/c`:

```sh
make BUILD=/tmp/gb-build test
```

Build with AddressSanitizer and UndefinedBehaviorSanitizer (needs a compiler with the sanitizer runtime, which MinGW lacks) in a directory of its own:

```sh
make SANITIZE=1 BUILD=/tmp/gb-san test rom-test
```

## Run

Pass a ROM path to the executable. Bytes the ROM sends over the serial port are printed to standard output:

```sh
./gameboy "roms/06-ld r,r.gb"
```

```text
06-ld r,r


Passed
```

Without a limit the emulator runs until it is interrupted or the CPU stalls. Use `-c` to stop after a number of T-cycles:

```sh
./gameboy roms/cpu_instrs.gb -c 250000000
```

## Tests

Run the unit and integration tests:

```sh
make test
```

`make check` runs the unit tests and the test ROMs below. Tests are always built with assertions enabled (`-UNDEBUG`). The default build uses `-O2`; pass `OPT=-O0` for a debugging build.

GitHub Actions (`.github/workflows/ci.yml`) runs `make check` on every push to `main` and every pull request, with gcc and clang, each with and without `SANITIZE=1`.

Each `tests/test_*.c` file is built and run automatically. Run a single one with `make build/tests/test_timer` (`.exe` suffix on Windows) and execute it from the repository root, since tests read `opcodes.json` and `roms/`.

The test suite includes:

- `test_opcode_timing`: every one of the 512 opcodes against `opcodes.json` for cycle count and byte length, plus the ordering of bus accesses and ticks within an instruction.
- `test_cpu_alu`: flags and results for ALU, rotate/shift, CB, stack, and 16-bit arithmetic instructions.
- `test_cpu`, `test_cpu_instructions`: register, load, jump, and control behavior.
- `test_interrupts`, `test_emulator_interrupts`: priority, service, HALT wake-up, EI, DI, RETI, and Timer-to-CPU service.
- `test_timer`, `test_emulator_timer`: registers, frequencies, falling edges, overflow reload, and IF requests.
- `test_serial`: register masks, transfer timing, restart, and the callback.
- `test_cartridge`, `test_cartridge_mbc1`: loading, transactional replacement, ROM and RAM banking, and rejection of unsupported types.
- `test_memory_bus`: Memory and Bus boundaries, echo RAM.
- `test_emulator`, `test_emulator_run`: cycle budgets, stall detection, fault reporting, and serial output through the whole machine.

### Test ROMs

```sh
make rom-test
```

Runs Blargg's 11 individual `cpu_instrs` ROMs, the combined `cpu_instrs.gb`, and `mem_timing.gb` headless under a cycle budget. A ROM passes when it prints `Passed` over the serial port. All of them pass.

`roms/dmg-acid2.gb` needs a PPU and is not part of this run.

To run one ROM with a custom budget:

```sh
make build/rom_test
./build/rom_test roms/mem_timing.gb 10000000
```

## Coverage and Limitations

- No PPU, so no video output and no VBlank or LCD STAT interrupts. `dmg-acid2` runs but shows nothing.
- No joypad, OAM DMA, or audio.
- Only ROM-only and MBC1 cartridges. MBC2, MBC3, MBC5 and others are rejected at load time.
- Cartridge RAM is not saved to disk.
- The HALT bug (HALT with IME off and an interrupt already pending) is not modelled.
- Interrupt dispatch does not model the `IE` write during the high-byte push.
- Register power-on values are the DMG post-boot CPU registers only. DIV and the hardware I/O registers start at zero, and `IF`/`TAC` unused bits read as zero rather than one.
- No Mooneye test ROM harness is included, so timing beyond what `mem_timing` covers is unvalidated.

Passing the tests above does not imply complete Game Boy hardware compatibility.

## Development Direction

1. Validate against Mooneye acceptance ROMs. They signal a pass with `LD B,B` and the Fibonacci values in the registers, so the runner needs to detect that instead of serial text. Along the way: power-on DIV, `IF`/`TAC` upper bits, and the HALT bug.
2. PPU: keep the core headless, expose a framebuffer and a button-state setter through `emulator.h`, and put any SDL front end outside the core. Start with LCD registers and mode timing (456 dots per line, 154 lines), raise VBlank and STAT through `interrupts_request()`, and render per scanline. `dmg-acid2.gb` is the target.
3. Joypad register and OAM DMA alongside the PPU.
4. Audio last.
