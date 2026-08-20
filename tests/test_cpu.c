#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <bus.h>
#include <cartridge.h>
#include <cpu.h>
#include <memory.h>

static InterruptRegisters interrupt_registers;


/*
 * Test helpers
 */

static void cartridge_init_test(Cartridge *cartridge)
{
    cartridge->rom_size = 0x8000;
    cartridge->rom = calloc(
        cartridge->rom_size,
        sizeof(uint8_t)
    );

    assert(cartridge->rom != NULL);
}

static void setup_cpu(
    CPU *cpu,
    Bus *bus,
    Memory *memory,
    Cartridge *cartridge
)
{
    cartridge_init_test(cartridge);
    memory_init(memory);

    bus_init(
        bus,
        cartridge,
        memory,
        &interrupt_registers
    );

    interrupt_registers.interrupt_flag = 0;
    interrupt_registers.interrupt_enable = 0;

    cpu_init(cpu, bus);
}

static void cleanup_cpu(
    Cartridge *cartridge
)
{
    cartridge_destroy(cartridge);
}

static void write_program(
    Cartridge *cartridge,
    const uint8_t *program,
    size_t size
)
{
    assert(size <= cartridge->rom_size - 0x0100);

    for (size_t i = 0; i < size; i++) {
        cartridge->rom[0x0100 + i] = program[i];
    }
}


/*
 * Generic r8 test helpers
 *
 * Encoding:
 *
 * 000 = B
 * 001 = C
 * 010 = D
 * 011 = E
 * 100 = H
 * 101 = L
 * 110 = [HL]
 * 111 = A
 */

static uint8_t test_value_for_register(uint8_t index)
{
    switch (index) {
        case 0:
            return 0x10; /* B */

        case 1:
            return 0x21; /* C */

        case 2:
            return 0x32; /* D */

        case 3:
            return 0x43; /* E */

        case 4:
            return 0x54; /* H */

        case 5:
            return 0x65; /* L */

        case 7:
            return 0x87; /* A */

        default:
            return 0x00;
    }
}

static uint8_t test_read_register(
    const CPU *cpu,
    uint8_t index
)
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

        case 7:
            return cpu->registers.a;

        default:
            return 0x00;
    }
}

static void test_write_register(
    CPU *cpu,
    uint8_t index,
    uint8_t value
)
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

        case 7:
            cpu->registers.a = value;
            break;

        default:
            break;
    }
}


/*
 * INC r8
 */

static void test_inc_registers(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge = {0};

    setup_cpu(
        &cpu,
        &bus,
        &memory,
        &cartridge
    );

    /*
     * INC B
     *
     * 0x0F -> 0x10
     *
     * Z = 0
     * N = 0
     * H = 1
     * C = preserved
     */
    {
        const uint8_t program[] = {
            0x04
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.b = 0x0F;
        cpu.registers.f = FLAG_C;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 4);
        assert(cpu.registers.b == 0x10);

        assert(
            cpu.registers.f ==
            (uint8_t)(FLAG_H | FLAG_C)
        );
    }

    /*
     * INC C
     *
     * 0xFF -> 0x00
     *
     * Z = 1
     * N = 0
     * H = 1
     * C = preserved
     */
    {
        const uint8_t program[] = {
            0x0C
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.c = 0xFF;
        cpu.registers.f = FLAG_C;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 4);
        assert(cpu.registers.c == 0x00);

        assert(
            cpu.registers.f ==
            (uint8_t)(FLAG_Z | FLAG_H | FLAG_C)
        );
    }

    /*
     * INC A
     *
     * 0x01 -> 0x02
     *
     * No Z/N/H.
     * C = preserved.
     */
    {
        const uint8_t program[] = {
            0x3C
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.a = 0x01;
        cpu.registers.f = FLAG_C;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 4);
        assert(cpu.registers.a == 0x02);
        assert(cpu.registers.f == FLAG_C);
    }

    cleanup_cpu(&cartridge);
}


/*
 * INC [HL]
 */

static void test_inc_hl_memory(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge = {0};

    setup_cpu(
        &cpu,
        &bus,
        &memory,
        &cartridge
    );

    const uint8_t program[] = {
        0x34
    };

    write_program(
        &cartridge,
        program,
        sizeof(program)
    );

    cpu.registers.h = 0xC0;
    cpu.registers.l = 0x00;
    cpu.registers.f = FLAG_C;

    memory_write(
        &memory,
        0xC000,
        0x0F
    );

    uint8_t cycles = cpu_step(&cpu);

    assert(cycles == 12);

    assert(
        memory_read(&memory, 0xC000) == 0x10
    );

    assert(
        cpu.registers.f ==
        (uint8_t)(FLAG_H | FLAG_C)
    );

    cleanup_cpu(&cartridge);
}


/*
 * DEC r8
 */

static void test_dec_registers(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge = {0};

    setup_cpu(
        &cpu,
        &bus,
        &memory,
        &cartridge
    );

    /*
     * DEC B
     *
     * 0x10 -> 0x0F
     *
     * Z = 0
     * N = 1
     * H = 1
     * C = preserved
     */
    {
        const uint8_t program[] = {
            0x05
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.b = 0x10;
        cpu.registers.f = FLAG_C;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 4);
        assert(cpu.registers.b == 0x0F);

        assert(
            cpu.registers.f ==
            (uint8_t)(FLAG_N | FLAG_H | FLAG_C)
        );
    }

    /*
     * DEC C
     *
     * 0x01 -> 0x00
     *
     * Z = 1
     * N = 1
     * H = 0
     * C = preserved
     */
    {
        const uint8_t program[] = {
            0x0D
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.c = 0x01;
        cpu.registers.f = FLAG_C;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 4);
        assert(cpu.registers.c == 0x00);

        assert(
            cpu.registers.f ==
            (uint8_t)(FLAG_Z | FLAG_N | FLAG_C)
        );
    }

    /*
     * DEC A
     *
     * 0x02 -> 0x01
     *
     * Z = 0
     * N = 1
     * H = 0
     * C = preserved
     */
    {
        const uint8_t program[] = {
            0x3D
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.a = 0x02;
        cpu.registers.f = FLAG_C;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 4);
        assert(cpu.registers.a == 0x01);

        assert(
            cpu.registers.f ==
            (uint8_t)(FLAG_N | FLAG_C)
        );
    }

    cleanup_cpu(&cartridge);
}


/*
 * DEC [HL]
 */

static void test_dec_hl_memory(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge = {0};

    setup_cpu(
        &cpu,
        &bus,
        &memory,
        &cartridge
    );

    const uint8_t program[] = {
        0x35
    };

    write_program(
        &cartridge,
        program,
        sizeof(program)
    );

    cpu.registers.h = 0xC0;
    cpu.registers.l = 0x00;
    cpu.registers.f = FLAG_C;

    memory_write(
        &memory,
        0xC000,
        0x10
    );

    uint8_t cycles = cpu_step(&cpu);

    assert(cycles == 12);

    assert(
        memory_read(&memory, 0xC000) == 0x0F
    );

    assert(
        cpu.registers.f ==
        (uint8_t)(FLAG_N | FLAG_H | FLAG_C)
    );

    cleanup_cpu(&cartridge);
}


/*
 * INC r16
 *
 * INC r16 does not modify flags.
 */

static void test_inc_register_pairs(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge = {0};

    setup_cpu(
        &cpu,
        &bus,
        &memory,
        &cartridge
    );

    /*
     * INC BC
     *
     * 0x12FF -> 0x1300
     */
    {
        const uint8_t program[] = {
            0x03
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.b = 0x12;
        cpu.registers.c = 0xFF;
        cpu.registers.f = 0xF0;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 8);
        assert(cpu.registers.b == 0x13);
        assert(cpu.registers.c == 0x00);
        assert(cpu.registers.f == 0xF0);
    }

    /*
     * INC DE
     *
     * 0xFFFF -> 0x0000
     */
    {
        const uint8_t program[] = {
            0x13
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.d = 0xFF;
        cpu.registers.e = 0xFF;
        cpu.registers.f = 0xA0;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 8);
        assert(cpu.registers.d == 0x00);
        assert(cpu.registers.e == 0x00);
        assert(cpu.registers.f == 0xA0);
    }

    /*
     * INC HL
     *
     * 0x12FF -> 0x1300
     */
    {
        const uint8_t program[] = {
            0x23
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.h = 0x12;
        cpu.registers.l = 0xFF;
        cpu.registers.f = 0x50;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 8);
        assert(cpu.registers.h == 0x13);
        assert(cpu.registers.l == 0x00);
        assert(cpu.registers.f == 0x50);
    }

    /*
     * INC SP
     *
     * 0xFFFF -> 0x0000
     */
    {
        const uint8_t program[] = {
            0x33
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.sp = 0xFFFF;
        cpu.registers.f = 0xF0;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 8);
        assert(cpu.registers.sp == 0x0000);
        assert(cpu.registers.f == 0xF0);
    }

    cleanup_cpu(&cartridge);
}


/*
 * DEC r16
 *
 * DEC r16 does not modify flags.
 */

static void test_dec_register_pairs(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge = {0};

    setup_cpu(
        &cpu,
        &bus,
        &memory,
        &cartridge
    );

    /*
     * DEC BC
     *
     * 0x1200 -> 0x11FF
     */
    {
        const uint8_t program[] = {
            0x0B
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.b = 0x12;
        cpu.registers.c = 0x00;
        cpu.registers.f = 0xF0;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 8);
        assert(cpu.registers.b == 0x11);
        assert(cpu.registers.c == 0xFF);
        assert(cpu.registers.f == 0xF0);
    }

    /*
     * DEC DE
     *
     * 0x0000 -> 0xFFFF
     */
    {
        const uint8_t program[] = {
            0x1B
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.d = 0x00;
        cpu.registers.e = 0x00;
        cpu.registers.f = 0xA0;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 8);
        assert(cpu.registers.d == 0xFF);
        assert(cpu.registers.e == 0xFF);
        assert(cpu.registers.f == 0xA0);
    }

    /*
     * DEC HL
     *
     * 0x1200 -> 0x11FF
     */
    {
        const uint8_t program[] = {
            0x2B
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.h = 0x12;
        cpu.registers.l = 0x00;
        cpu.registers.f = 0x50;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 8);
        assert(cpu.registers.h == 0x11);
        assert(cpu.registers.l == 0xFF);
        assert(cpu.registers.f == 0x50);
    }

    /*
     * DEC SP
     *
     * 0x0000 -> 0xFFFF
     */
    {
        const uint8_t program[] = {
            0x3B
        };

        write_program(
            &cartridge,
            program,
            sizeof(program)
        );

        cpu.registers.pc = 0x0100;
        cpu.registers.sp = 0x0000;
        cpu.registers.f = 0xF0;

        uint8_t cycles = cpu_step(&cpu);

        assert(cycles == 8);
        assert(cpu.registers.sp == 0xFFFF);
        assert(cpu.registers.f == 0xF0);
    }

    cleanup_cpu(&cartridge);
}


/*
 * LD r8,r8
 *
 * Encoding:
 *
 * 01DDDSSS
 *
 * DDD = destination
 * SSS = source
 *
 * 000 = B
 * 001 = C
 * 010 = D
 * 011 = E
 * 100 = H
 * 101 = L
 * 110 = [HL]
 * 111 = A
 *
 * 0x76 is HALT, not LD H,[HL].
 */

static void test_ld_r8_r8(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge = {0};

    setup_cpu(
        &cpu,
        &bus,
        &memory,
        &cartridge
    );

    for (uint8_t destination = 0;
         destination < 8;
         destination++) {

        for (uint8_t source = 0;
             source < 8;
             source++) {

            /*
             * 0x76 = HALT, não LD [HL],[HL].
             */
            if (destination == 6 && source == 6) {
                continue;
            }

            /*
             * Estado inicial conhecido.
             */
            cpu.registers.pc = 0x0100;
            cpu.halted = false;
            cpu.stopped = false;

            cpu.registers.b = 0x10;
            cpu.registers.c = 0x21;
            cpu.registers.d = 0x32;
            cpu.registers.e = 0x43;
            cpu.registers.h = 0xC0;
            cpu.registers.l = 0x00;
            cpu.registers.a = 0x87;

            /*
             * LD não altera flags.
             */
            cpu.registers.f = 0xF0;

            /*
             * H e L não são alterados artificialmente.
             *
             * Isso mantém HL em C000 mesmo quando H ou L
             * são utilizados como registrador fonte.
             */
            if (source != 4 &&
                source != 5 &&
                source != 6) {

                test_write_register(
                    &cpu,
                    source,
                    test_value_for_register(source)
                );
            }

            /*
             * HL agora representa o endereço real que
             * será utilizado por [HL].
             */
            uint16_t hl_address =
                (uint16_t)(
                    ((uint16_t)cpu.registers.h << 8) |
                    cpu.registers.l
                );

            /*
             * Coloca um valor conhecido em [HL].
             */
            memory_write(
                &memory,
                hl_address,
                0xAB
            );

            /*
             * Captura o valor esperado ANTES da execução.
             */
            uint8_t expected;

            if (source == 6) {
                expected = 0xAB;
            } else {
                expected = test_read_register(
                    &cpu,
                    source
                );
            }

            /*
             * 01DDDSSS
             */
            uint8_t opcode = (uint8_t)(
                0x40U |
                ((uint8_t)(destination << 3U)) |
                source
            );

            /*
             * O opcode está na ROM.
             *
             * Não podemos usar cpu_write8(), pois a ROM
             * é somente leitura através do Bus.
             */
            cartridge.rom[0x0100] = opcode;

            /*
             * Executa.
             */
            uint8_t cycles = cpu_step(&cpu);

            /*
             * LD r8,r8 possui um byte.
             */
            assert(cpu.registers.pc == 0x0101);

            /*
             * LD não modifica flags.
             */
            assert(cpu.registers.f == 0xF0);

            /*
             * LD não deve colocar a CPU em HALT.
             */
            assert(!cpu.halted);

            /*
             * Registrador -> registrador:
             *     4 ciclos
             *
             * Registrador <-> [HL]:
             *     8 ciclos
             */
            if (destination == 6 || source == 6) {
                assert(cycles == 8);
            } else {
                assert(cycles == 4);
            }

            /*
             * Verifica o destino.
             */
            if (destination == 6) {
                /*
                 * LD [HL],r8
                 *
                 * Usa o endereço HL que existia antes
                 * da execução.
                 */
                assert(
                    memory_read(
                        &memory,
                        hl_address
                    ) == expected
                );
            } else {
                /*
                 * LD r8,r8
                 * LD r8,[HL]
                 */
                assert(
                    test_read_register(
                        &cpu,
                        destination
                    ) == expected
                );
            }
        }
    }

    cleanup_cpu(&cartridge);

    printf("LD r8,r8 tests passed!\n");
}

static void test_cpu_control_states_and_flags(void)
{
    CPU cpu;
    Bus bus;
    Memory memory;
    Cartridge cartridge = {0};

    setup_cpu(
        &cpu,
        &bus,
        &memory,
        &cartridge
    );

    cartridge.rom[0x0100] = 0x76;

    assert(cpu.registers.f == 0xB0);

    uint8_t cycles = cpu_step(&cpu);

    assert(cycles == 4);
    assert(cpu.halted);
    assert(cpu.step_status == CPU_STEP_EXECUTED);
    assert(cpu.registers.pc == 0x0101);

    cycles = cpu_step(&cpu);

    assert(cycles == 4);
    assert(cpu.halted);
    assert(cpu.step_status == CPU_STEP_HALTED);
    assert(cpu.registers.pc == 0x0101);

    cpu.halted = false;
    cpu.registers.pc = 0x0100;
    cartridge.rom[0x0100] = 0xD3;

    cycles = cpu_step(&cpu);

    assert(cycles == 0);
    assert(!cpu.halted);
    assert(cpu.step_status == CPU_STEP_UNIMPLEMENTED_OPCODE);

    cpu.registers.pc = 0x0100;
    cartridge.rom[0x0100] = 0x04;
    cpu.registers.b = 0x00;
    cpu.registers.f = 0x1F;

    cycles = cpu_step(&cpu);

    assert(cycles == 4);
    assert(cpu.registers.f == FLAG_C);
    assert((cpu.registers.f & 0x0F) == 0);

    cleanup_cpu(&cartridge);
}


/*
 * Main
 */

int main(void)
{
    test_inc_registers();
    test_inc_hl_memory();

    test_dec_registers();
    test_dec_hl_memory();

    test_inc_register_pairs();
    test_dec_register_pairs();

    test_ld_r8_r8();
    test_cpu_control_states_and_flags();

    printf("All CPU tests passed!\n");

    return 0;
}
