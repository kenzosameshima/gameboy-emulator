#include <assert.h>
#include <stdio.h>

#include <cartridge.h>

int main(void)
{
    Cartridge cartridge;

    cartridge_init(&cartridge);

    assert(cartridge.rom == NULL);
    assert(cartridge.rom_size == 0);

    cartridge_destroy(&cartridge);

    assert(
        cartridge_load(
            &cartridge,
            "roms/01-special.gb"
        )
    );

    printf(
        "ROM size: %zu bytes\n",
        cartridge.rom_size
    );

    assert(
        cartridge_read(&cartridge, 0x0000) ==
        cartridge.rom[0x0000]
    );

    assert(
        cartridge_read(&cartridge, 0x0100) ==
        cartridge.rom[0x0100]
    );

    assert(cartridge_read(&cartridge, 0xFFFF) == 0xFF);

    uint8_t *first_rom = cartridge.rom;
    size_t first_size = cartridge.rom_size;

    assert(
        cartridge_load(
            &cartridge,
            "roms/02-interrupts.gb"
        )
    );

    assert(cartridge.rom != first_rom);
    assert(cartridge.rom_size > 0);

    uint8_t *second_rom = cartridge.rom;
    size_t second_size = cartridge.rom_size;

    assert(
        !cartridge_load(
            &cartridge,
            "roms/does-not-exist.gb"
        )
    );

    assert(cartridge.rom == second_rom);
    assert(cartridge.rom_size == second_size);
    assert(first_size > 0);

    cartridge_destroy(&cartridge);

    assert(cartridge.rom == NULL);
    assert(cartridge.rom_size == 0);

    printf("Cartridge tests passed!\n");

    return 0;
}
