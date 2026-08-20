#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <cartridge.h>

void cartridge_init(Cartridge *cartridge)
{
    if (cartridge == NULL) {
        return;
    }

    cartridge->rom = NULL;
    cartridge->rom_size = 0;
}

int cartridge_load(Cartridge *cartridge, const char *path)
{
    if (cartridge == NULL || path == NULL) {
        return 0;
    }

    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }

    long size = ftell(file);

    if (size <= 0) {
        fclose(file);
        return 0;
    }

    rewind(file);

    size_t rom_size = (size_t)size;

    uint8_t *rom = malloc(rom_size);

    if (rom == NULL) {
        fclose(file);
        return 0;
    }

    size_t read = fread(
        rom,
        1,
        rom_size,
        file
    );

    fclose(file);

    if (read != rom_size) {
        free(rom);

        return 0;
    }

    free(cartridge->rom);
    cartridge->rom = rom;
    cartridge->rom_size = rom_size;

    return 1;
}

void cartridge_destroy(Cartridge *cartridge)
{
    if (cartridge == NULL) {
        return;
    }

    free(cartridge->rom);

    cartridge->rom = NULL;
    cartridge->rom_size = 0;
}

uint8_t cartridge_read(
    const Cartridge *cartridge,
    uint16_t address
)
{
    if (cartridge == NULL || address >= cartridge->rom_size) {
        return 0xFF;
    }

    return cartridge->rom[address];
}
