/*
 * File names are UTF-8 on every platform. A ROM and its battery save named
 * outside the Windows ANSI code pages must load and save: SDL hands the
 * front end its arguments in UTF-8, and Japanese ROM names are common.
 *
 * The files are made and checked by their real names, through the UTF-16
 * API on Windows, so a core that read the names in the ANSI code page would
 * miss them.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include <emulator.h>

enum {
    ROM_SIZE = 0x8000,
    RAM_SIZE = 0x2000,
    HEADER_TYPE = 0x0147,
    HEADER_RAM_SIZE = 0x0149,

    TYPE_MBC1_RAM_BATTERY = 0x03,
    RAM_CODE_8K = 0x02
};

/* "é" and the katakana "ポ", in UTF-8 and in UTF-16. */
#define ROM_NAME "test_utf8_paths_\xC3\xA9\xE3\x83\x9D.gb"
#define SAVE_NAME "test_utf8_paths_\xC3\xA9\xE3\x83\x9D.sav"
#define ROM_WIDE L"test_utf8_paths_éポ.gb"
#define SAVE_WIDE L"test_utf8_paths_éポ.sav"

/* Opens and removes files by their real names, not through the core. */
#ifdef _WIN32
#define open_real(name, wide, mode) _wfopen(wide, L"" mode)
#define remove_real(name, wide) _wremove(wide)
#else
#define open_real(name, wide, mode) fopen(name, mode)
#define remove_real(name, wide) remove(name)
#endif

int main(void)
{
    static uint8_t rom[ROM_SIZE];

    /* A jump to itself at the entry point: the program only has to load. */
    rom[0x0100] = 0x18;
    rom[0x0101] = 0xFE;
    rom[HEADER_TYPE] = TYPE_MBC1_RAM_BATTERY;
    rom[HEADER_RAM_SIZE] = RAM_CODE_8K;

    FILE *file = open_real(ROM_NAME, ROM_WIDE, "wb");

    assert(file != NULL);
    assert(fwrite(rom, 1, sizeof(rom), file) == sizeof(rom));
    assert(fclose(file) == 0);

    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    assert(emulator_load_rom(emulator, ROM_NAME) == EMULATOR_OK);
    assert(emulator_save_battery(emulator, SAVE_NAME, 0) == EMULATOR_OK);

    /* The save is there under its real name, whole. */
    file = open_real(SAVE_NAME, SAVE_WIDE, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    assert(ftell(file) == RAM_SIZE);
    assert(fclose(file) == 0);

    assert(emulator_load_battery(emulator, SAVE_NAME, 0) == EMULATOR_OK);

    emulator_destroy(emulator);

    assert(remove_real(ROM_NAME, ROM_WIDE) == 0);
    assert(remove_real(SAVE_NAME, SAVE_WIDE) == 0);

    printf("UTF-8 path tests passed!\n");

    return 0;
}
