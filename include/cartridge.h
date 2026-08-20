#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <stdint.h>
#include <stddef.h>

typedef struct Cartridge {
    uint8_t *rom;
    size_t rom_size;
} Cartridge;

void cartridge_init(Cartridge *cartridge);

int cartridge_load(Cartridge *cartridge, const char *path);
void cartridge_destroy(Cartridge *cartridge);

uint8_t cartridge_read(const Cartridge *cartridge, uint16_t address);

#endif
