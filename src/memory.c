#include <stdint.h>
#include <string.h>

#include <memory.h>

void memory_init(Memory *memory)
{
    memset(memory->wram, 0, sizeof(memory->wram));
    memset(memory->hram, 0, sizeof(memory->hram));
}

uint8_t memory_read(const Memory *memory, uint16_t address)
{
    if (address >= MEM_WRAM_START && address <= MEM_WRAM_END) {
        return memory->wram[address - MEM_WRAM_START];
    }

    if (address >= MEM_HRAM_START && address <= MEM_HRAM_END) {
        return memory->hram[address - MEM_HRAM_START];
    }

    return 0xFF;
}

void memory_write(Memory *memory, uint16_t address, uint8_t value)
{
    if (address >= MEM_WRAM_START && address <= MEM_WRAM_END) {
        memory->wram[address - MEM_WRAM_START] = value;
        return;
    }

    if (address >= MEM_HRAM_START && address <= MEM_HRAM_END) {
        memory->hram[address - MEM_HRAM_START] = value;
    }
}
