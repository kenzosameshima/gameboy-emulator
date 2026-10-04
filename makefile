CC = gcc
AR = ar

CPPFLAGS = -Iinclude

# C23 needs GCC 14 or newer; older compilers (GCC 13 on Ubuntu 24.04) only
# know it as c2x, which is enough for this code.
STD := $(shell echo 'int main(void) { return 0; }' | $(CC) -std=c23 -x c -fsyntax-only - 2>/dev/null && echo c23 || echo c2x)

# The static analyzer is GCC-only; clang rejects the flag.
ANALYZER := $(shell echo 'int main(void) { return 0; }' | $(CC) -fanalyzer -x c -fsyntax-only - 2>/dev/null && echo -fanalyzer)

CFLAGS = -std=$(STD) \
	     -Wall \
	     -Wextra \
	     -Wpedantic \
	     -Werror \
	     $(ANALYZER) \
	     -Wconversion \
	     -Wsign-conversion \
	     -Wshadow \
	     -Wformat=2 \
	     -Wundef \
	     -Wcast-qual \
	     -Wcast-align \
	     -Wwrite-strings \
	     -Wstrict-prototypes \
	     -Wmissing-prototypes \
	     $(OPT) \
	     -g

# Optimised by default so the test ROMs run quickly; override with OPT=-O0.
OPT ?= -O2

LDFLAGS =

# Sanitizer build: make clean && make SANITIZE=1 test
ifdef SANITIZE
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

# MinGW and MSYS2 append .exe to executables.
ifeq ($(OS),Windows_NT)
EXE = .exe
endif

BUILD = build

# Everything except main.c: the machine core, shared by the game, the
# tools and every test.
CORE_SRC = src/emulator.c \
	       src/cpu.c \
	       src/cpu_alu.c \
	       src/cpu_ops.c \
	       src/cpu_cb.c \
	       src/bus.c \
	       src/interrupts.c \
	       src/timer.c \
	       src/serial.c \
	       src/ppu.c \
	       src/ppu_render.c \
	       src/dma.c \
	       src/joypad.c \
	       src/apu.c \
	       src/cartridge.c \
	       src/cartridge_save.c \
	       src/mapper_rom_only.c \
	       src/mapper_mbc1.c \
	       src/mapper_mbc2.c \
	       src/mapper_mbc3.c \
	       src/mapper_mbc5.c \
	       src/memory.c

CORE_OBJ = $(CORE_SRC:%.c=$(BUILD)/%.o)
CORE_LIB = $(BUILD)/libgbcore.a

GAMEBOY = gameboy$(EXE)
ROM_TEST = $(BUILD)/rom_test$(EXE)
FRAME_DUMP = $(BUILD)/frame_dump$(EXE)
SDL_FRONTEND = $(BUILD)/gameboy-sdl$(EXE)

# The SDL front end is built on request (make sdl) and needs SDL2 and
# pkg-config. SDL headers are included as system headers so the strict
# warnings apply to our code only.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null | sed "s/-I/-isystem /g")
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)

TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:tests/%.c=$(BUILD)/tests/%$(EXE))

ALL_OBJ = $(CORE_OBJ) \
	      $(BUILD)/src/main.o \
	      $(BUILD)/tools/rom_test.o \
	      $(BUILD)/tools/frame_dump.o \
	      $(BUILD)/frontend/sdl_main.o \
	      $(TEST_SRC:%.c=$(BUILD)/%.o)

# Test ROMs that run headless and report through the serial port.
# dmg-acid2 reports through the screen, not the serial port, and is checked
# by tests/test_acid2.c instead. The dmg_sound ROMs report through cartridge
# RAM, which rom_test reads back through the battery save.
ROM_TESTS = roms/[0-9]*.gb roms/cpu_instrs.gb roms/mem_timing.gb \
	    roms/dmg_sound/*.gb


.PHONY: all clean test rom-test check sdl mooneye

# Keep object files between runs so unchanged tests are not rebuilt.
.SECONDARY:


all: $(GAMEBOY)


$(GAMEBOY): $(BUILD)/src/main.o $(CORE_LIB)
	$(CC) $(LDFLAGS) $^ -o $@


$(ROM_TEST): $(BUILD)/tools/rom_test.o $(CORE_LIB)
	$(CC) $(LDFLAGS) $^ -o $@


$(FRAME_DUMP): $(BUILD)/tools/frame_dump.o $(CORE_LIB)
	$(CC) $(LDFLAGS) $^ -o $@


# SDL include flags apply to the front end only.
$(BUILD)/frontend/sdl_main.o: CPPFLAGS += $(SDL_CFLAGS)


$(SDL_FRONTEND): $(BUILD)/frontend/sdl_main.o $(CORE_LIB)
	$(CC) $(LDFLAGS) $^ $(SDL_LIBS) -o $@


sdl:
ifeq ($(SDL_LIBS),)
	@echo "SDL2 not found: install libsdl2-dev and pkg-config" >&2; exit 1
else
	@$(MAKE) --no-print-directory $(SDL_FRONTEND)
endif


$(CORE_LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^


# Tests run from the repository root: they read opcodes.json and roms/.
$(BUILD)/tests/%$(EXE): $(BUILD)/tests/%.o $(CORE_LIB)
	$(CC) $(LDFLAGS) $^ -o $@


$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@


# Tests rely on assert(), so they are always built with assertions on, even
# if NDEBUG sneaks in through CFLAGS.
$(BUILD)/tests/%.o: CFLAGS += -UNDEBUG


test: $(TEST_BIN)
	@for test in $(TEST_BIN); do \
		echo "== $$test"; \
		$$test || exit 1; \
	done


# Runs every test ROM under a cycle budget and checks for "Passed".
rom-test: $(ROM_TEST)
	@status=0; \
	for rom in $(ROM_TESTS); do \
		$(ROM_TEST) "$$rom" || status=1; \
	done; \
	exit $$status


# Everything: the unit tests and the Blargg test ROMs.
check: test rom-test


# The Mooneye test suite, DMG ROMs only. Downloads it into roms/mooneye on
# first use; see tools/mooneye.sh. Not part of check because it needs the
# network and not every ROM passes yet.
mooneye: $(ROM_TEST)
	tools/mooneye.sh $(ROM_TEST)


-include $(ALL_OBJ:.o=.d)


clean:
	rm -rf $(BUILD)
	rm -f $(GAMEBOY)


