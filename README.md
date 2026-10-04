# Game Boy Emulator Core

An incremental C23 Game Boy emulator core. It has a complete SM83 CPU, a Bus, Memory, a Cartridge with ROM-only, MBC1, MBC2, MBC3 and MBC5 mappers, interrupts, a Timer, a Serial port, a PPU, OAM DMA and a joypad, and it passes Blargg's CPU instruction and memory timing test ROMs and the dmg-acid2 picture test.

The core is headless: the PPU draws into a framebuffer that a front end reads with `emulator_framebuffer()`, and the front end reports held buttons with `emulator_set_buttons()`. `frontend/sdl_main.c` is such a front end (SDL2, built with `make sdl`), so Tetris and Pokémon Red can be played; there is no audio yet. Test ROMs run because they report their results through the serial port, and `tools/frame_dump` saves the screen as a PNG, optionally with scripted button presses.

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
              +-- Cartridge (ROM-only, MBC1, MBC2, MBC3, MBC5)   [mapper seam: src/mapper.h]
              +-- InterruptRegisters (interrupts.c)
              +-- Timer
              +-- Serial
              +-- Ppu (ppu.c, ppu_render.c)
              +-- Dma (dma.c)
              +-- Joypad (joypad.c)
```

`main.c` and `tools/rom_test.c` only use the opaque `Emulator` API. `Emulator` owns the machine components by value. The Bus routes accesses to Cartridge, Memory, Timer, Serial, and the interrupt registers; the CPU uses the Bus for memory and takes the interrupt registers as its own dependency (`cpu_init(cpu, bus, interrupts)`), so it never reaches through the Bus.

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
- `EMULATOR_STALLED` reports a CPU halted with nothing able to wake it (HALT with `IE = 0`, or STOP), instead of spinning forever. The rule lives in `cpu_is_stalled()`, next to the wake rules in `cpu_step()`.
- `emulator_set_serial_output()` receives every byte the program sends over the serial port.
- Loading a ROM resets CPU, Memory, Timer, Serial, interrupt state, and the cycle counter. A failed load preserves the previously loaded Cartridge.

### Cartridge

- ROM file loading with transactional replacement.
- Header parsing for the cartridge type and RAM size.
- A mapper seam (`MapperOps` in `src/mapper.h`): `Cartridge` keeps loading, header parsing and the ROM and RAM buffers, and each mapper (ROM-only, MBC1, MBC2, MBC3, MBC5) is an adapter in its own file that decides how addresses map into them and holds its own registers. A new mapper is one new file plus a header-type entry.
- ROM-only and MBC1 (`0x00`-`0x03`): ROM bank switching (5 + 2 bits, bank 0 remapped to 1), banking mode, RAM enable, and RAM banking. Bank numbers wrap to the ROM size.
- MBC3 (`0x0F`-`0x13`): 7-bit ROM banking (bank 0 remapped to 1), RAM banking, and the real-time clock on the timer variants (`0x0F`, `0x10`). The clock registers (seconds, minutes, hours, 9-bit day, halt, day carry) are read through the latch (write 0 then 1 to `6000-7FFF`), writing the seconds restarts the current second, and the clock runs off the CPU clock (4194304 T-cycles per second), so it is deterministic. Pokémon Red/Blue (`0x13`) loads and runs. The clock is not persisted.
- MBC2 (`0x05`, `0x06`): a 4-bit ROM bank register and 512 half-bytes of built-in RAM, both programmed through `0000-3FFF` where address bit 8 picks the register; the RAM reads with the top nibble set and repeats through `A000-BFFF`.
- MBC5 (`0x19`-`0x1E`): 9-bit ROM banking up to 8 MiB where bank 0 is a real choice for the switchable window, and RAM banking up to 128 KiB. Rumble is ignored.
- Unsupported cartridge types are rejected at load time instead of running with the wrong mapping. `cartridge_load()` reports why through `CartridgeLoadStatus` (unreadable file, out of memory, unsupported type), `emulator_load_rom()` maps that to distinct `EmulatorStatus` values, and `emulator_get_unsupported_cartridge_type()` returns the header type byte, so an MBC6 game (`0x20`) is reported as an unsupported header type `0x20`.
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

`IF` is at `0xFF0F` and `IE` at `0xFFFF`. Hardware components raise interrupts with `interrupts_request()`. Dispatch takes 5 M-cycles (20 T-cycles): priority selection, IME clearing, PC push, selective IF clearing, and the vector load. Priority and vectors are pure functions in `interrupts.c` (`interrupts_highest_priority()`, `interrupts_vector()`).

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

### PPU

The picture processing unit (`ppu.c`, `ppu_render.c`) owns VRAM, OAM, the LCD registers and the framebuffer.

- Mode state machine on the T-cycle clock: 456 dots per line, 154 lines, OAM scan (80 dots), drawing (172), HBlank, then VBlank on lines 144-153. `emulator_frame_count()` counts frames at VBlank entry.
- VBlank interrupt at line 144, and the STAT interrupt with LYC, HBlank, VBlank and OAM sources ORed into one line that requests the interrupt only when it rises (STAT blocking).
- VRAM is unreadable while drawing and OAM while scanning or drawing (reads give `0xFF`, writes are dropped), and both are open while the LCD is off. Turning the LCD off blanks the screen and rewinds to line 0.
- Scanline renderer: background with SCX/SCY wrap, both tile data addressing modes and both tile maps, window with its own line counter and WX < 7 handling, 8x8 and 8x16 sprites with flips, both palettes and the behind-background flag, DMG sprite priority (lower X first, then OAM order) and the ten-sprites-per-line limit. Shades 0 (lightest) to 3 (darkest) are available through `emulator_framebuffer()`.
- `OAM DMA` writes use `ppu_oam_dma_write()`, which ignores the access lock like the hardware does.

### OAM DMA

Writing a page number XX to `FF46` copies the 160 bytes at `XX00` into OAM, one per M-cycle; `FF46` reads back the last page. Counting the write as M-cycle 0, the CPU can still use every bus at M = 1, the transfer takes over from M = 2 and copies during M = 2 to M = 161, and everything is free again at M = 162. While it copies, the CPU shares a bus with it: OAM is always taken, and so is the bus the source is on, the external bus (ROM, cartridge RAM, work RAM and its echo) or the video bus (VRAM). Reads of a taken bus give `0xFF` and writes are dropped; the I/O registers and high RAM stay free (games run their wait loop from HRAM, or from work RAM when the source is VRAM). Sources from `E000` up read the work RAM mirror. Writing again while copying does not stop the transfer at once: it keeps the bus through the two start-up cycles and the new transfer begins after them. The DMA reads VRAM and OAM even when the PPU is using them.

### Joypad

`FF00` (P1) selects the direction keys (bit 4 low) or the buttons (bit 5 low) and reads the selected group in bits 0-3, where 0 means pressed; bits 6-7 read 1. The front end reports the held set with `emulator_set_buttons()` using the `EMULATOR_BUTTON_*` bits. A key pressed in a selected group, or a group selected while a key is held, pulls a line low and requests the joypad interrupt, which also ends `STOP`. Because input can end `STOP` at any time, `STOP` is never reported as stalled.

## Memory Map Currently Used

```text
0000-7FFF   Cartridge ROM (writes program the mapper)
A000-BFFF   Cartridge RAM
C000-DFFF   Work RAM
8000-9FFF   Video RAM
E000-FDFF   Echo of C000-DDFF
FE00-FE9F   OAM (sprite attributes)
FF01-FF02   Serial
FF00        Joypad (P1)
FF04-FF07   Timer registers
FF40-FF45   LCD registers (LCDC, STAT, SCY, SCX, LY, LYC)
FF46        OAM DMA
FF47-FF4B   LCD palettes and window (BGP, OBP0, OBP1, WY, WX)
FF0F        Interrupt Flag (IF)
FF80-FFFE   High RAM
FFFF        Interrupt Enable (IE)
```

The constants live in `include/memory_map.h`. Other regions are currently unimplemented and read `0xFF`.

## Project Layout

```text
include/                Module headers (public API is emulator.h)
src/main.c              CLI entry point
frontend/sdl_main.c      SDL2 front end: window, keyboard, frame pacing
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
src/cartridge.c         ROM/RAM ownership, loading, header parsing, mapper dispatch
src/mapper.h            MapperOps: the seam between Cartridge and a mapper
src/mapper_rom_only.c   ROM-only mapper
src/mapper_mbc1.c       MBC1 mapper
src/mapper_mbc2.c       MBC2 mapper
src/mapper_mbc3.c       MBC3 mapper and real-time clock
src/mapper_mbc5.c       MBC5 mapper
src/timer.c             Timer implementation
src/serial.c            Serial port
src/ppu.c               PPU state machine, registers, interrupts, VRAM/OAM access
src/ppu_render.c        PPU scanline renderer
src/dma.c               OAM DMA
src/joypad.c            Joypad register and interrupt
tools/rom_test.c        Headless test ROM runner
tools/frame_dump.c      Runs a ROM and saves the LCD picture as a PNG
tests/                  Unit and integration tests
roms/                   Local test ROMs
opcodes.json            Opcode reference data, used by test_opcode_timing
makefile                Build and test rules
```

## Requirements

- GCC 14 or newer builds with `-std=c23`. Older GCC, such as 13 on Ubuntu 24.04, is detected by the Makefile and builds with `-std=c2x`. Clang works with `make CC=clang`.
- GNU Make (`mingw32-make` from MSYS2 UCRT64 on Windows).
- A POSIX-like shell for the Makefile recipes.
- SDL2 and pkg-config, only for the optional front end (`libsdl2-dev` on Debian and Ubuntu). The core, the tests and the other tools do not need them.

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

### Play

The SDL front end shows the picture in a window, turns the keyboard into buttons and runs at the real frame rate (about 59.7 frames per second):

```sh
make sdl
./build/gameboy-sdl "roms/Tetris.gb"
```

| Key | Button |
|---|---|
| Arrow keys | D-pad |
| `Z` | A |
| `X` | B |
| `Enter` | Start |
| Right `Shift` or `Backspace` | Select |
| `Esc` | Quit |

Options: `--scale N` (window size as a multiple of 160x144, default 4), `--gray` (grayscale instead of the classic green) and `--frames N` (exit after N frames, used for smoke tests; with `SDL_VIDEODRIVER=dummy` it needs no display). The window can be resized and keeps its shape. There is no audio, no save file and no pause key yet.

### Command line

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

Save the screen after a number of frames as a PNG (default 60 frames, 3x scale), for a ROM that draws something:

```sh
make build/frame_dump
./build/frame_dump roms/dmg-acid2.gb acid2.png 30
```

Hold buttons by frame number to get past title screens (a button is held from the first frame up to, but not including, the second):

```sh
./build/frame_dump roms/Tetris.gb game.png 1100 3 --press start@400-405 --press start@480-485
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
- `test_cpu_alu`: flags and results for ALU, rotate/shift, CB, stack, and 16-bit arithmetic instructions, using known reference values.
- `test_cpu_alu_exhaustive`: every 8-bit ALU, INC/DEC, rotate/shift and DAA input (plus ADD HL and SP+e8 samples) against an independent model; DAA is checked against decimal arithmetic on BCD operands.
- `test_cpu`, `test_cpu_instructions`: register and memory wiring of INC/DEC and LD, jumps, and control behavior.
- `test_interrupts`, `test_emulator_interrupts`: priority, service, HALT wake-up, EI, DI, RETI, and Timer-to-CPU service.
- `test_timer`, `test_emulator_timer`: registers, frequencies, falling edges, overflow reload, and IF requests.
- `test_serial`: register masks, transfer timing, restart, and the callback.
- `test_cartridge`, `test_cartridge_mbc1`: loading, transactional replacement, ROM and RAM banking, and rejection of unsupported types.
- `test_cartridge_mbc2`: the ROM bank and RAM enable registers selected by address bit 8, and the 4-bit built-in RAM with its echo.
- `test_cartridge_mbc5`: 9-bit ROM banking up to 512 banks including bank 0, wrapping to the ROM size, and RAM banking up to 128 KiB.
- `test_cartridge_mbc3`: MBC3 ROM and RAM banking up to 2 MiB, and the clock latch, halt, rollover, day carry and seconds-write behaviour.
- `test_ppu`: line and frame timing, the mode order, VBlank and STAT interrupts (including STAT blocking), LY = LYC, VRAM and OAM access rules, and LCD on/off.
- `test_ppu_render`: backgrounds, scrolling and wrap, both tile addressing modes and maps, window, palettes, sprites (flips, priority, 8x16, the ten-per-line limit); expected pixels are worked out by hand from the tile bytes.
- `test_dma`: what OAM DMA copies and from where (ROM, VRAM, the work RAM mirror), one byte per M-cycle after a two-cycle start-up, which bus the CPU loses for a work RAM source and for a VRAM source, and restarts.
- `test_joypad`: the P1 groups and active-low lines, which bit each key lands on, and when the joypad interrupt fires.
- `test_emulator_input_dma`: real programs through the whole machine that poll the joypad, wake from `STOP` on a button, and start a DMA from a routine in high RAM.
- `test_acid2`: runs `roms/dmg-acid2.gb` and compares the picture with the reference screenshot pixel by pixel (`tests/data/dmg-acid2-reference.txt`, from the dmg-acid2 repository, MIT licence).
- `test_memory_bus`: Memory and Bus boundaries, echo RAM.
- `test_emulator`, `test_emulator_run`: cycle budgets, stall detection, fault reporting, and serial output through the whole machine.

### Test ROMs

```sh
make rom-test
```

Runs Blargg's 11 individual `cpu_instrs` ROMs, the combined `cpu_instrs.gb`, and `mem_timing.gb` headless under a cycle budget. A ROM passes when it prints `Passed` over the serial port. All of them pass.

`roms/dmg-acid2.gb` reports through the screen, not the serial port, so `test_acid2` checks it instead.

Run the Mooneye test suite (the ROMs that apply to a DMG; it is downloaded once into the ignored `roms/mooneye/`, from a fixed release checked against a SHA-256):

```sh
make mooneye
```

Mooneye ROMs report through the serial port as six bytes: the Fibonacci numbers `3 5 8 13 21 34` for a pass, or `0x42` six times for a failure; `rom_test` recognises both that and Blargg's text. The ROMs that are known to fail are listed in `tools/mooneye-expected-failures.txt`: `make mooneye` fails only for a ROM that is not on the list and reports listed ROMs that now pass. CI runs it.

To run one ROM with a custom budget:

```sh
make build/rom_test
./build/rom_test roms/mem_timing.gb 10000000
```

## Coverage and Limitations

- The PPU draws each line in one go when drawing starts, so register changes during a line apply from the next line, and drawing always lasts 172 dots (no sprite or scroll penalties). The LY = 153 early-zero quirk, the STAT write quirk, the OAM bug and the extra mode 2 interrupt at line 144 are not modelled.
- No audio. A blocked read gives `0xFF`; real hardware can return the byte the DMA is transferring on a conflicting read. The cartridge and VRAM source rules follow the DMG.
- Only ROM-only, MBC1, MBC2, MBC3 and MBC5 cartridges. Others (MBC6, MBC7, HuC1, the camera and so on) are rejected at load time. MBC1 multicart wiring is not detected.
- Cartridge RAM is not saved to disk.
- The HALT bug (HALT with IME off and an interrupt already pending) is not modelled.
- Interrupt dispatch does not model the `IE` write during the high-byte push.
- Register power-on values are the DMG post-boot CPU registers only. DIV and the hardware I/O registers start at zero, and `IF`/`TAC` unused bits read as zero rather than one.
- No Mooneye test ROM harness is included, so timing beyond what `mem_timing` covers is unvalidated.

Passing the tests above does not imply complete Game Boy hardware compatibility.

## Development Direction

1. Validate against Mooneye acceptance ROMs. They signal a pass with `LD B,B` and the Fibonacci values in the registers, so the runner needs to detect that instead of serial text. Along the way: power-on DIV, `IF`/`TAC` upper bits, and the HALT bug.
2. Battery saves, then audio.
