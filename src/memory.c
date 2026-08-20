#include <stdint.h>

#include <memory.h>

void memory_init(Memory *memory)
{
    for (int i = 0; i < 0x2000; i++) {
        memory->wram[i] = 0;
    }

    for (int i = 0; i < 0x7F; i++) {
        memory->hram[i] = 0;
    }
}

uint8_t memory_read(const Memory *memory, uint16_t address)
{
    if (address >= 0xC000 && address <= 0xDFFF) {
        return memory->wram[address - 0xC000];
    }

    if (address >= 0xFF80 && address <= 0xFFFE) {
        return memory->hram[address - 0xFF80];
    }

    return 0xFF;
}

void memory_write(Memory *memory, uint16_t address, uint8_t value)
{
    if (address >= 0xC000 && address <= 0xDFFF) {
        memory->wram[address - 0xC000] = value;
        return;
    }

    if (address >= 0xFF80 && address <= 0xFFFE) {
        memory->hram[address - 0xFF80] = value;
    }
}
