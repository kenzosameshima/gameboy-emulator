/*
 * dmg-acid2 conformance: runs roms/dmg-acid2.gb and compares the LCD
 * picture with the reference screenshot, pixel by pixel.
 *
 * dmg-acid2 (MIT licence, Matt Currie, github.com/mattcurrie/dmg-acid2)
 * draws a face that is only right if sprite priority, 8x16 sprites, the
 * ten-sprites-per-line limit, window and background addressing, the LCDC
 * bits and the palettes all behave as on a DMG.
 *
 * tests/data/dmg-acid2-reference.txt is img/reference-dmg.png from that
 * repository with each pixel written as a digit, 0 (lightest) to 3
 * (darkest): 144 lines of 160 digits.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <emulator.h>

enum {
    CYCLES_PER_FRAME_RUN = 70224,
    FRAMES_TO_RUN = 30
};

static void load_reference(uint8_t *shades)
{
    FILE *file = fopen("tests/data/dmg-acid2-reference.txt", "r");

    assert(file != NULL);

    size_t count = 0;
    int c;

    while ((c = fgetc(file)) != EOF) {
        if (c >= '0' && c <= '3') {
            assert(count < (size_t)EMULATOR_SCREEN_WIDTH *
                           EMULATOR_SCREEN_HEIGHT);
            shades[count++] = (uint8_t)(c - '0');
        } else {
            /* Line endings (LF or CRLF) are the only other thing allowed. */
            assert(c == '\n' || c == '\r');
        }
    }

    fclose(file);
    assert(count == (size_t)EMULATOR_SCREEN_WIDTH * EMULATOR_SCREEN_HEIGHT);
}

int main(void)
{
    uint8_t reference[EMULATOR_SCREEN_WIDTH * EMULATOR_SCREEN_HEIGHT];

    load_reference(reference);

    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    assert(emulator_load_rom(emulator, "roms/dmg-acid2.gb") == EMULATOR_OK);

    while (emulator_frame_count(emulator) < FRAMES_TO_RUN) {
        EmulatorStatus status =
            emulator_run_cycles(emulator, CYCLES_PER_FRAME_RUN);

        /* The ROM ends by halting, which is fine. */
        assert(status == EMULATOR_OK || status == EMULATOR_STALLED);

        if (status == EMULATOR_STALLED) {
            break;
        }
    }

    const uint8_t *shades = emulator_framebuffer(emulator);
    unsigned differing = 0;

    for (unsigned y = 0; y < EMULATOR_SCREEN_HEIGHT; y++) {
        for (unsigned x = 0; x < EMULATOR_SCREEN_WIDTH; x++) {
            uint8_t want = reference[y * EMULATOR_SCREEN_WIDTH + x];
            uint8_t got = shades[y * EMULATOR_SCREEN_WIDTH + x];

            if (want != got) {
                if (differing < 10) {
                    fprintf(stderr, "(x=%u,y=%u): want shade %u, got %u\n",
                            x, y, want, got);
                }

                differing++;
            }
        }
    }

    if (differing != 0) {
        fprintf(stderr, "%u pixels differ from the reference\n", differing);
    }

    assert(differing == 0);

    emulator_destroy(emulator);

    printf("dmg-acid2 matches the reference picture!\n");

    return 0;
}
