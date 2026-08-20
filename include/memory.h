#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

typedef struct Memory {
    uint8_t wram[0x2000];
    uint8_t hram[0x7F];
} Memory;

void memory_init(Memory *memory);

uint8_t memory_read(const Memory *memory, uint16_t address);
void memory_write(Memory *memory, uint16_t address, uint8_t value);

#endif
