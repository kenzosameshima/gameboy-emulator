# Game Boy Emulator Core

An incremental C23 Game Boy emulator core. It has a complete SM83 CPU, a Bus, Memory, a Cartridge with ROM-only, MBC1, MBC2, MBC3 and MBC5 mappers, interrupts, a Timer, a Serial port, a PPU, OAM DMA, a joypad and an APU (sound), and it passes Blargg's CPU instruction, memory timing and sound test ROMs, the dmg-acid2 picture test and all 94 DMG Mooneye acceptance ROMs.

The core is headless: the PPU draws into a framebuffer that a front end reads with `emulator_framebuffer()`, and the front end reports held buttons with `emulator_set_buttons()`. `frontend/sdl_main.c` is such a front end (SDL2, built with `make sdl`), so Tetris and Pokémon Red can be played with sound. The APU queues mixed stereo frames at a rate the front end chooses (`emulator_set_audio_sample_rate()`, `emulator_take_audio()`). Test ROMs run because they report their results through the serial port, and `tools/frame_dump` saves the screen as a PNG, optionally with scripted button presses.

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
- ROM-only and MBC1 (`0x00`-`0x03`): ROM bank switching (5 + 2 bits, bank 0 remapped to 1), banking mode, RAM enable, and RAM banking. Bank numbers wrap to the ROM size. A 1 MiB ROM with the Nintendo logo at the start of at least two of its 256 KiB sections is treated as a multicart: 4 bank bits inside a game and the game picker shifted by 4.
- MBC3 (`0x0F`-`0x13`): 7-bit ROM banking (bank 0 remapped to 1), RAM banking, and the real-time clock on the timer variants (`0x0F`, `0x10`). The clock registers (seconds, minutes, hours, 9-bit day, halt, day carry) are read through the latch (write 0 then 1 to `6000-7FFF`), writing the seconds restarts the current second, and the clock runs off the CPU clock (4194304 T-cycles per second), so it is deterministic. Pokémon Red/Blue (`0x13`) loads and runs. The clock is part of the battery save.
- MBC2 (`0x05`, `0x06`): a 4-bit ROM bank register and 512 half-bytes of built-in RAM, both programmed through `0000-3FFF` where address bit 8 picks the register; the RAM reads with the top nibble set and repeats through `A000-BFFF`.
- MBC5 (`0x19`-`0x1E`): 9-bit ROM banking up to 8 MiB where bank 0 is a real choice for the switchable window, and RAM banking up to 128 KiB. Rumble is ignored.
- Unsupported cartridge types are rejected at load time instead of running with the wrong mapping. `cartridge_load()` reports why through `CartridgeLoadStatus` (unreadable file, out of memory, unsupported type), `emulator_load_rom()` maps that to distinct `EmulatorStatus` values, and `emulator_get_unsupported_cartridge_type()` returns the header type byte, so an MBC6 game (`0x20`) is reported as an unsupported header type `0x20`.
- Battery saves: `emulator_save_battery()` and `emulator_load_battery()` keep the cartridge RAM of a battery cartridge (types `0x03`, `0x06`, `0x0F`, `0x10`, `0x13`, `0x1B` and `0x1E`) in a raw dump that other emulators read too, plus the 48-byte clock footer that BGB and VBA-M use for MBC3 timer cartridges. Loading adds the time that passed since the save to a running clock. The core has no clock of its own, so the caller passes the time in. A save is written to a temporary file and renamed into place, and a load that fails changes nothing.

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

`IF` is at `0xFF0F` and `IE` at `0xFFFF`. Hardware components raise interrupts with `interrupts_request()`. Dispatch takes 5 M-cycles (20 T-cycles): IME clearing, the PC push, priority selection, selective IF clearing, and the vector load. The interrupt is chosen after the high byte of PC has been pushed, so a push that lands on `IE` (SP at 0000 or 0001) can cancel the dispatch (PC becomes 0 and IF is left alone) or redirect it to a lower priority interrupt. `IF` reads with its upper three bits set, and `IE` keeps all eight bits. Priority and vectors are pure functions in `interrupts.c` (`interrupts_highest_priority()`, `interrupts_vector()`).

HALT behavior distinguishes:

- CPU halted without pending interrupt.
- HALT wake-up when an interrupt is pending but IME is disabled: HALT ends with no delay of its own and the next instruction runs at once.
- Direct interrupt service when IME is enabled.

`EI` uses delayed IME enable semantics, and an `EI` executed while an enable is already pending does not push it back. `DI` cancels IME and a pending enable. `RETI` restores PC from the stack and enables IME.

### Timer

Registers: DIV `0xFF04`, TIMA `0xFF05`, TMA `0xFF06`, TAC `0xFF07`.

- Internal 16-bit divider, DIV exposing its high byte, DIV reset on write.
- TAC frequency selection.
- Falling-edge-based TIMA increments, including the falling edges caused by DIV and TAC writes.
- Delayed TIMA reload after overflow, TIMA write cancellation in the cycle where TIMA reads 0, and the cycle after the reload where writes to TIMA are ignored and a write to TMA also reaches TIMA.
- Timer interrupt requests through `IF.TIMER`.

The Timer models the DMG normal-speed path. CGB double-speed behavior is not implemented.

### Serial

Registers: SB `0xFF01` and SC `0xFF02`. With no link partner, a transfer started with the internal clock (`SC = 0x81`) reports its byte to the output callback, completes with `SB = 0xFF`, clears `SC` bit 7, and requests the serial interrupt. The serial clock is the system counter divided down: one bit is shifted on each falling edge of counter bit 8 (every 512 T-cycles, at multiples of 512 of the timer divider) and the transfer completes on the eighth edge after the `SC` write, so it takes between 3584 and 4096 cycles depending on where the divider is. Writing `SC = 0x81` again restarts the transfer. The external clock never completes.

### PPU

The picture processing unit (`ppu.c`, `ppu_render.c`) owns VRAM, OAM, the LCD registers and the framebuffer.

- Mode state machine on the T-cycle clock: 456 dots per line, 154 lines, OAM scan (80 dots), drawing, HBlank, then VBlank on lines 144-153. Drawing lasts 172 dots plus `SCX mod 8` plus, when sprites are on the line, the sum of their penalties minus 3 (6 dots per sprite with X below 168, and `max(0, 5 - (X + SCX) mod 8)` more for the first sprite over each 8-pixel background tile), which moves when HBlank starts; the rule was fitted to and verified by the Mooneye sprite timing ROM. `emulator_frame_count()` counts frames at VBlank entry.
- VBlank interrupt at line 144, and the STAT interrupt with LYC, HBlank, VBlank and OAM sources ORed into one line that requests the interrupt only when it rises (STAT blocking). The OAM source also fires when line 144 starts. `LY` reads the next line from dot 452, while the `LY = LYC` flag reads 0 for those 4 dots and is recomputed when the line starts; the flag and the STAT line keep their values while the LCD is off.
- VRAM and OAM are blocked to the CPU at slightly different dots for reads and writes (reads give `0xFF`, writes are dropped): OAM reads from dot 452 of the previous line until drawing ends, OAM writes during the scan except its last cycle and during drawing, VRAM reads from the scan's last cycle (dot 76) through drawing, and VRAM writes during drawing only. Both are open while the LCD is off. Turning the LCD off blanks the screen and rewinds to line 0; turning it on starts line 0 in mode 0 with no OAM scan.
- Scanline renderer: background with SCX/SCY wrap, both tile data addressing modes and both tile maps, window with its own line counter and WX < 7 handling, 8x8 and 8x16 sprites with flips, both palettes and the behind-background flag, DMG sprite priority (lower X first, then OAM order) and the ten-sprites-per-line limit. Shades 0 (lightest) to 3 (darkest) are available through `emulator_framebuffer()`.
- `OAM DMA` writes use `ppu_oam_dma_write()`, which ignores the access lock like the hardware does.

### OAM DMA

Writing a page number XX to `FF46` copies the 160 bytes at `XX00` into OAM, one per M-cycle; `FF46` reads back the last page. Counting the write as M-cycle 0, the CPU can still use every bus at M = 1, the transfer takes over from M = 2 and copies during M = 2 to M = 161, and everything is free again at M = 162. While it copies, the CPU shares a bus with it: OAM is always taken, and so is the bus the source is on, the external bus (ROM, cartridge RAM, work RAM and its echo) or the video bus (VRAM). Reads of a taken bus give `0xFF` and writes are dropped; the I/O registers and high RAM stay free (games run their wait loop from HRAM, or from work RAM when the source is VRAM). Sources from `E000` up read the work RAM mirror. Writing again while copying does not stop the transfer at once: it keeps the bus through the two start-up cycles and the new transfer begins after them. The DMA reads VRAM and OAM even when the PPU is using them.

### Joypad

`FF00` (P1) selects the direction keys (bit 4 low) or the buttons (bit 5 low) and reads the selected group in bits 0-3, where 0 means pressed; bits 6-7 read 1. The front end reports the held set with `emulator_set_buttons()` using the `EMULATOR_BUTTON_*` bits. A key pressed in a selected group, or a group selected while a key is held, pulls a line low and requests the joypad interrupt, which also ends `STOP`. Because input can end `STOP` at any time, `STOP` is never reported as stalled.

### APU

The audio processing unit (`apu.c`) has the four channels (two pulse channels, the first with a frequency sweep, a wave channel and a noise channel), each with a length counter, envelopes on all but the wave channel, and the mixer with `NR50` and `NR51`. The 512 Hz frame sequencer is clocked by the falling edge of bit 12 of the timer's divider, so writing `DIV` clocks it as on hardware. Registers read with their unused bits set, powering off (`NR52` bit 7) clears them and ignores writes except to the length counters, which survive as on the DMG, and wave RAM stays accessible. The DMG's quirks are modelled: the extra length clock when a length counter is enabled before a step that does not clock lengths, the counter reloading one short on a trigger, the sweep's negate and overflow rules, and wave RAM, which while the channel plays can only be read or written in the cycle the channel fetches a sample (it reads `0xFF` otherwise) and is damaged by a retrigger just as it fetches.

The channels advance in whole M-cycles, and the mixer averages the output over each output frame, so an edge that falls inside an M-cycle takes effect on the next. With a sample rate set, the core produces the plain mix of the four DACs; it does not high-pass filter it, which the SDL front end does with the hardware's capacitor constants. A rate of 0, the default, produces nothing.

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
FF10-FF26   Sound registers (NR10-NR52)
FF30-FF3F   Wave RAM
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
src/cartridge_save.c    Battery saves: the RAM dump and the mapper's extra state
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
src/apu.c               Sound channels, frame sequencer, mixer and sample queue
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

Options: `--scale N` (window size as a multiple of 160x144, default 4), `--gray` (grayscale instead of the classic green) and `--frames N` (exit after N frames, used for smoke tests; with `SDL_VIDEODRIVER=dummy` it needs no display), `--no-save` and `--mute`. The window can be resized and keeps its shape. There is no pause key yet. Sound plays through SDL's audio queue at 48 kHz, and the machine is paced by how much is queued, so picture and sound stay together; without an audio device it says so and paces by the clock. A cartridge with a battery keeps its RAM (and an MBC3 clock) in a `.sav` file next to the ROM, with the same name: it is loaded at start, written every 30 seconds and when the program ends. `--no-save` turns that off.

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
- `test_cpu_edge_cases`: the cases the Mooneye ROMs found, as unit tests: an `EI` while an enable is pending, HALT waking with no extra cycle, interrupt dispatch when the PC push lands on `IE`, and the `IF`/`IE` register bits.
- `test_timer_reload`: the M-cycles after a TIMA overflow, where TIMA reads 0, is reloaded, and then ignores writes while a TMA write also reaches it.
- `test_emulator_boot_state`: what a ROM sees at its entry point, checked with real programs; so far the divider the boot ROM leaves (DIV reads 0xAB).
- `test_timer`, `test_emulator_timer`: registers, frequencies, falling edges, overflow reload, and IF requests.
- `test_serial`: register masks, transfer timing, restart, and the callback. Also the transfer's alignment to the system counter, checked for several divider phases including the boot ROM's.
- `test_cartridge`, `test_cartridge_mbc1`: loading, transactional replacement, ROM and RAM banking, and rejection of unsupported types.
- `test_cartridge_mbc2`: the ROM bank and RAM enable registers selected by address bit 8, and the 4-bit built-in RAM with its echo.
- `test_cartridge_mbc5`: 9-bit ROM banking up to 512 banks including bank 0, wrapping to the ROM size, and RAM banking up to 128 KiB.
- `test_cartridge_save`: which cartridges have a battery, the RAM dump and the clock footer byte for byte, failed loads that change nothing, and the clock catching up for the time the console was off (including the day counter wrapping and a halted clock).
- `test_emulator_battery`: a program writes cartridge RAM, the emulator saves it, and a fresh emulator loads it and a second program reads it back.
- `test_cartridge_mbc3`: MBC3 ROM and RAM banking up to 2 MiB, and the clock latch, halt, rollover, day carry and seconds-write behaviour.
- `test_ppu`: line and frame timing, the mode order, VBlank and STAT interrupts (including STAT blocking), LY = LYC, VRAM and OAM access rules, and LCD on/off. Also the dot-level windows when VRAM and OAM can be read and written, `LY` advancing at dot 452 with the `LY = LYC` flag lagging, the mode 3 length with `SCX` and sprites, and the LCD-on first line.
- `test_ppu_render`: backgrounds, scrolling and wrap, both tile addressing modes and maps, window, palettes, sprites (flips, priority, 8x16, the ten-per-line limit); expected pixels are worked out by hand from the tile bytes.
- `test_dma`: what OAM DMA copies and from where (ROM, VRAM, the work RAM mirror), one byte per M-cycle after a two-cycle start-up, which bus the CPU loses for a work RAM source and for a VRAM source, and restarts.
- `test_apu`: the register read masks and boot values, power on and off, status and DAC rules, the frame sequencer's length, sweep and envelope steps (including a DIV write clocking it), the length quirks, the noise generator, the wave RAM access rules and retrigger damage, and the mixed output (duty cycles, panning, NR50, the sample queue); expected values come from the documented hardware behaviour, not from the code.
- `test_emulator_audio`: a program plays a note and the frames come out of `emulator_take_audio()` at the requested rate, which survives loading a ROM.
- `test_joypad`: the P1 groups and active-low lines, which bit each key lands on, and when the joypad interrupt fires.
- `test_emulator_input_dma`: real programs through the whole machine that poll the joypad, wake from `STOP` on a button, and start a DMA from a routine in high RAM.
- `test_acid2`: runs `roms/dmg-acid2.gb` and compares the picture with the reference screenshot pixel by pixel (`tests/data/dmg-acid2-reference.txt`, from the dmg-acid2 repository, MIT licence).
- `test_memory_bus`: Memory and Bus boundaries, echo RAM.
- `test_emulator`, `test_emulator_run`: cycle budgets, stall detection, fault reporting, and serial output through the whole machine.

### Test ROMs

```sh
make rom-test
```

Runs Blargg's 11 individual `cpu_instrs` ROMs, the combined `cpu_instrs.gb`, `mem_timing.gb` and the 12 `dmg_sound` ROMs (`roms/dmg_sound/`) headless under a cycle budget. A ROM passes when it prints `Passed` over the serial port. The sound ROMs only print on screen; they also leave their result in cartridge RAM, which `rom_test` reads back through the battery save. All of them pass.

`roms/dmg-acid2.gb` reports through the screen, not the serial port, so `test_acid2` checks it instead.

Run the Mooneye test suite (the ROMs that apply to a DMG; it is downloaded once into the ignored `roms/mooneye/`, from a fixed release checked against a SHA-256):

```sh
make mooneye
```

Mooneye ROMs report through the serial port as six bytes: the Fibonacci numbers `3 5 8 13 21 34` for a pass, or `0x42` six times for a failure; `rom_test` recognises both that and Blargg's text. All 94 pass. Any ROM known to fail would be listed in `tools/mooneye-expected-failures.txt`, which is empty now: `make mooneye` fails only for a ROM that is not on the list and reports listed ROMs that now pass. CI runs it.

To run one ROM with a custom budget:

```sh
make build/rom_test
./build/rom_test roms/mem_timing.gb 10000000
```

## Coverage and Limitations

- The PPU draws each line in one go when drawing starts, so register changes during a line apply from the next line, and the window adds no dots to drawing. The LY = 153 early-zero quirk, the STAT write quirk and the OAM bug are not modelled.
- Audio is DMG only, advances in whole M-cycles, and the core's output is unfiltered (the front end applies the high-pass filter). The zombie-mode envelope quirks and other behaviour that only the CGB shows are not modelled.
- A blocked read gives `0xFF`; real hardware can return the byte the DMA is transferring on a conflicting read. The cartridge and VRAM source rules follow the DMG.
- Only ROM-only, MBC1, MBC2, MBC3 and MBC5 cartridges. Others (MBC6, MBC7, HuC1, the camera and so on) are rejected at load time.
- The HALT bug (HALT with IME off and an interrupt already pending) is not modelled.
- Register power-on values are the DMG post-boot ones for the CPU registers, the divider (DIV reads 0xAB at the entry point), `P1` (0xCF), `IF` (0xE1) and the sound registers; most other I/O registers start at zero, and the unused bits of some registers other than `IF`, `TAC`, `P1`, `SC` and `STAT` read as zero rather than one.
- Timing beyond what the Blargg and Mooneye ROMs cover is unvalidated.

Passing the tests above does not imply complete Game Boy hardware compatibility.

## Development Direction

1. Accuracy the ROMs do not yet check: the HALT bug, the window's effect on mode 3 length, mid-line PPU register changes and the OAM bug.
2. Colour: the CGB.
