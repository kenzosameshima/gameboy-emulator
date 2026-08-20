CC = gcc

CPPFLAGS = -Iinclude

CFLAGS = -std=c23 \
	     -Wall \
	     -Wextra \
	     -Wpedantic \
	     -Werror \
	     -fanalyzer \
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
	     -g

LDFLAGS =

GAMEBOY = gameboy

GAMEBOY_SRC = src/main.c \
	          src/emulator.c \
	          src/cpu.c \
	          src/bus.c \
			  src/timer.c \
	          src/cartridge.c \
	          src/memory.c

GAMEBOY_OBJ = $(GAMEBOY_SRC:.c=.o)

TEST_CARTRIDGE = test_cartridge

TEST_CARTRIDGE_SRC = tests/test_cartridge.c \
	                 src/cartridge.c

TEST_CARTRIDGE_OBJ = $(TEST_CARTRIDGE_SRC:.c=.o)

TEST_CPU = test_cpu

TEST_CPU_SRC = tests/test_cpu.c \
	           src/cpu.c \
	           src/bus.c \
			   src/timer.c \
	           src/cartridge.c \
	           src/memory.c

TEST_CPU_OBJ = $(TEST_CPU_SRC:.c=.o)

TEST_EMULATOR = test_emulator

TEST_EMULATOR_SRC = tests/test_emulator.c \
	                src/emulator.c \
	                src/cpu.c \
	                src/bus.c \
					 src/timer.c \
	                src/cartridge.c \
	                src/memory.c

TEST_EMULATOR_OBJ = $(TEST_EMULATOR_SRC:.c=.o)

TEST_MEMORY_BUS = test_memory_bus

TEST_MEMORY_BUS_SRC = tests/test_memory_bus.c \
					  src/bus.c \
					  src/timer.c \
					  src/cartridge.c \
					  src/memory.c

TEST_MEMORY_BUS_OBJ = $(TEST_MEMORY_BUS_SRC:.c=.o)

TEST_CPU_INSTRUCTIONS = test_cpu_instructions

TEST_CPU_INSTRUCTIONS_SRC = tests/test_cpu_instructions.c \
							 src/cpu.c \
							 src/bus.c \
							 src/timer.c \
							 src/cartridge.c \
							 src/memory.c

TEST_CPU_INSTRUCTIONS_OBJ = $(TEST_CPU_INSTRUCTIONS_SRC:.c=.o)

TEST_INTERRUPTS = test_interrupts

TEST_INTERRUPTS_SRC = tests/test_interrupts.c \
					  src/cpu.c \
					  src/bus.c \
					  src/timer.c \
					  src/cartridge.c \
					  src/memory.c

TEST_INTERRUPTS_OBJ = $(TEST_INTERRUPTS_SRC:.c=.o)

TEST_EMULATOR_INTERRUPTS = test_emulator_interrupts

TEST_EMULATOR_INTERRUPTS_SRC = tests/test_emulator_interrupts.c \
							   src/emulator.c \
							   src/cpu.c \
							   src/bus.c \
							   src/timer.c \
							   src/cartridge.c \
							   src/memory.c

TEST_EMULATOR_INTERRUPTS_OBJ = $(TEST_EMULATOR_INTERRUPTS_SRC:.c=.o)

TEST_TIMER = test_timer

TEST_TIMER_SRC = tests/test_timer.c \
				 src/cpu.c \
				 src/timer.c \
				 src/bus.c \
				 src/cartridge.c \
				 src/memory.c

TEST_TIMER_OBJ = $(TEST_TIMER_SRC:.c=.o)

TEST_EMULATOR_TIMER = test_emulator_timer

TEST_EMULATOR_TIMER_SRC = tests/test_emulator_timer.c \
						  src/emulator.c \
						  src/cpu.c \
						  src/timer.c \
						  src/bus.c \
						  src/cartridge.c \
						  src/memory.c

TEST_EMULATOR_TIMER_OBJ = $(TEST_EMULATOR_TIMER_SRC:.c=.o)


.PHONY: all clean test test_emulator_run test_memory_bus_run test_cpu_instructions_run test_interrupts_run test_emulator_interrupts_run test_timer_run test_emulator_timer_run


all: $(GAMEBOY)


$(GAMEBOY): $(GAMEBOY_OBJ)
	$(CC) $(LDFLAGS) $(GAMEBOY_OBJ) -o $@


$(TEST_CARTRIDGE): $(TEST_CARTRIDGE_OBJ)
	$(CC) $(LDFLAGS) $(TEST_CARTRIDGE_OBJ) -o $@


$(TEST_CPU): $(TEST_CPU_OBJ)
	$(CC) $(LDFLAGS) $(TEST_CPU_OBJ) -o $@


$(TEST_EMULATOR): $(TEST_EMULATOR_OBJ)
	$(CC) $(LDFLAGS) $(TEST_EMULATOR_OBJ) -o $@


test_emulator_run: $(TEST_EMULATOR)
	./$(TEST_EMULATOR)


$(TEST_MEMORY_BUS): $(TEST_MEMORY_BUS_OBJ)
	$(CC) $(LDFLAGS) $(TEST_MEMORY_BUS_OBJ) -o $@


test_memory_bus_run: $(TEST_MEMORY_BUS)
	./$(TEST_MEMORY_BUS)


$(TEST_CPU_INSTRUCTIONS): $(TEST_CPU_INSTRUCTIONS_OBJ)
	$(CC) $(LDFLAGS) $(TEST_CPU_INSTRUCTIONS_OBJ) -o $@


test_cpu_instructions_run: $(TEST_CPU_INSTRUCTIONS)
	./$(TEST_CPU_INSTRUCTIONS)


$(TEST_INTERRUPTS): $(TEST_INTERRUPTS_OBJ)
	$(CC) $(LDFLAGS) $(TEST_INTERRUPTS_OBJ) -o $@


test_interrupts_run: $(TEST_INTERRUPTS)
	./$(TEST_INTERRUPTS)


$(TEST_EMULATOR_INTERRUPTS): $(TEST_EMULATOR_INTERRUPTS_OBJ)
	$(CC) $(LDFLAGS) $(TEST_EMULATOR_INTERRUPTS_OBJ) -o $@


test_emulator_interrupts_run: $(TEST_EMULATOR_INTERRUPTS)
	./$(TEST_EMULATOR_INTERRUPTS)


$(TEST_TIMER): $(TEST_TIMER_OBJ)
	$(CC) $(LDFLAGS) $(TEST_TIMER_OBJ) -o $@


test_timer_run: $(TEST_TIMER)
	./$(TEST_TIMER)


$(TEST_EMULATOR_TIMER): $(TEST_EMULATOR_TIMER_OBJ)
	$(CC) $(LDFLAGS) $(TEST_EMULATOR_TIMER_OBJ) -o $@


test_emulator_timer_run: $(TEST_EMULATOR_TIMER)
	./$(TEST_EMULATOR_TIMER)


test: $(TEST_CARTRIDGE) $(TEST_CPU) test_emulator_run test_memory_bus_run test_cpu_instructions_run test_interrupts_run test_emulator_interrupts_run test_timer_run test_emulator_timer_run
	./$(TEST_CARTRIDGE)
	./$(TEST_CPU)


%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@


-include $(GAMEBOY_OBJ:.o=.d)
-include $(TEST_CARTRIDGE_OBJ:.o=.d)
-include $(TEST_CPU_OBJ:.o=.d)
-include $(TEST_EMULATOR_OBJ:.o=.d)
-include $(TEST_MEMORY_BUS_OBJ:.o=.d)
-include $(TEST_CPU_INSTRUCTIONS_OBJ:.o=.d)
-include $(TEST_INTERRUPTS_OBJ:.o=.d)
-include $(TEST_EMULATOR_INTERRUPTS_OBJ:.o=.d)
-include $(TEST_TIMER_OBJ:.o=.d)
-include $(TEST_EMULATOR_TIMER_OBJ:.o=.d)


clean:
	rm -f $(GAMEBOY)
	rm -f $(GAMEBOY_OBJ)
	rm -f $(GAMEBOY_OBJ:.o=.d)
	rm -f $(TEST_CARTRIDGE)
	rm -f $(TEST_CARTRIDGE_OBJ)
	rm -f $(TEST_CARTRIDGE_OBJ:.o=.d)
	rm -f $(TEST_CPU)
	rm -f $(TEST_CPU_OBJ)
	rm -f $(TEST_CPU_OBJ:.o=.d)
	rm -f $(TEST_EMULATOR)
	rm -f $(TEST_EMULATOR_OBJ)
	rm -f $(TEST_EMULATOR_OBJ:.o=.d)
	rm -f $(TEST_MEMORY_BUS)
	rm -f $(TEST_MEMORY_BUS_OBJ)
	rm -f $(TEST_MEMORY_BUS_OBJ:.o=.d)
	rm -f $(TEST_CPU_INSTRUCTIONS)
	rm -f $(TEST_CPU_INSTRUCTIONS_OBJ)
	rm -f $(TEST_CPU_INSTRUCTIONS_OBJ:.o=.d)
	rm -f $(TEST_INTERRUPTS)
	rm -f $(TEST_INTERRUPTS_OBJ)
	rm -f $(TEST_INTERRUPTS_OBJ:.o=.d)
	rm -f $(TEST_EMULATOR_INTERRUPTS)
	rm -f $(TEST_EMULATOR_INTERRUPTS_OBJ)
	rm -f $(TEST_EMULATOR_INTERRUPTS_OBJ:.o=.d)
	rm -f $(TEST_TIMER)
	rm -f $(TEST_TIMER_OBJ)
	rm -f $(TEST_TIMER_OBJ:.o=.d)
	rm -f $(TEST_EMULATOR_TIMER)
	rm -f $(TEST_EMULATOR_TIMER_OBJ)
	rm -f $(TEST_EMULATOR_TIMER_OBJ:.o=.d)
