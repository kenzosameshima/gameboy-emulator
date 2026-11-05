/*
 * Runs a test ROM headless and compares the LCD picture with a reference
 * screenshot, for the ROMs that report through the screen: the mealybug
 * tearoom PPU tests, and Blargg ROMs such as halt_bug.
 *
 * Usage: screen_test <rom> <reference.png> [--frames N] [--max-cycles N]
 *
 * Without --frames the ROM is run until it executes LD B,B (opcode 0x40),
 * which the mealybug tests do when they are finished, and the picture is
 * compared as it is at that moment. With --frames the ROM runs for that many
 * frames first, for ROMs that just stay on a final screen.
 *
 * The reference is a 160 x 144 PNG using the four DMG shades #000000,
 * #555555, #AAAAAA and #FFFFFF.
 *
 * Exit code: 0 if the picture matches, 1 if it differs or the run fails.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emulator.h>

#include "../src/emulator_internal.h"
#include "png_read.h"

enum {
    DEFAULT_MAX_CYCLES = 40000000,
    LD_B_B = 0x40
};

/* Shade 0 is the lightest and 3 the darkest. */
static unsigned shade_to_gray(uint8_t shade)
{
    return 255u - (unsigned)shade * 85u;
}

static int parse_number(const char *text, uint64_t *value)
{
    char *end = NULL;

    errno = 0;
    *value = strtoull(text, &end, 10);

    return errno == 0 && end != text && *end == '\0';
}

/* Runs until LD B,B is about to execute. Returns 0 if it was reached. */
static int run_to_ld_b_b(Emulator *emulator, uint64_t max_cycles)
{
    while (emulator_cycles(emulator) < max_cycles) {
        if (bus_read(&emulator->bus, emulator->cpu.registers.pc) == LD_B_B) {
            return 0;
        }

        EmulatorStatus status = emulator_step(emulator);

        if (status != EMULATOR_OK) {
            return 1;
        }
    }

    return 1;
}

int main(int argc, char **argv)
{
    const char *rom = NULL;
    const char *reference = NULL;
    uint64_t frames = 0;
    uint64_t max_cycles = DEFAULT_MAX_CYCLES;
    int use_frames = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            use_frames = parse_number(argv[++i], &frames);

            if (!use_frames) {
                rom = NULL;
                break;
            }
        } else if (strcmp(argv[i], "--max-cycles") == 0 && i + 1 < argc) {
            if (!parse_number(argv[++i], &max_cycles)) {
                rom = NULL;
                break;
            }
        } else if (rom == NULL) {
            rom = argv[i];
        } else if (reference == NULL) {
            reference = argv[i];
        } else {
            rom = NULL;
            break;
        }
    }

    if (rom == NULL || reference == NULL) {
        fprintf(
            stderr,
            "Usage: %s <rom> <reference.png> [--frames N] [--max-cycles N]\n",
            argv[0]
        );
        return 1;
    }

    uint8_t *expected = NULL;
    unsigned width = 0;
    unsigned height = 0;

    if (png_read_rgb(reference, &expected, &width, &height) != 0 ||
        width != EMULATOR_SCREEN_WIDTH || height != EMULATOR_SCREEN_HEIGHT) {
        fprintf(stderr, "Cannot use reference %s (need a 160x144 PNG)\n", reference);
        free(expected);
        return 1;
    }

    Emulator *emulator = emulator_create();

    if (emulator == NULL) {
        fprintf(stderr, "Out of memory\n");
        free(expected);
        return 1;
    }

    EmulatorStatus status = emulator_load_rom(emulator, rom);
    int run_failed = 0;

    if (status != EMULATOR_OK) {
        run_failed = 1;
    } else if (use_frames) {
        for (uint64_t frame = 0; frame < frames && !run_failed; frame++) {
            status = emulator_run_frame(emulator);
            run_failed = status != EMULATOR_OK && status != EMULATOR_STALLED;
        }
    } else {
        run_failed = run_to_ld_b_b(emulator, max_cycles);
    }

    unsigned differing = 0;
    unsigned first_x = 0;
    unsigned first_y = 0;
    unsigned first_expected = 0;
    unsigned first_actual = 0;

    if (!run_failed) {
        const uint8_t *picture = emulator_framebuffer(emulator);

        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                const uint8_t *pixel = expected + ((size_t)y * width + x) * 3;
                unsigned want = shade_to_gray(picture[y * width + x]);
                int same = pixel[0] == want && pixel[1] == want && pixel[2] == want;

                if (!same && differing++ == 0) {
                    first_x = x;
                    first_y = y;
                    first_expected = pixel[0];
                    first_actual = want;
                }
            }
        }
    }

    if (run_failed) {
        printf("[ERROR] %s (did not reach the end of the test)\n", rom);
    } else if (differing == 0) {
        printf("[PASS] %s\n", rom);
    } else {
        printf(
            "[FAIL] %s (%u pixels differ, first at x=%u y=%u: want %02X got %02X)\n",
            rom, differing, first_x, first_y, first_expected, first_actual
        );
    }

    emulator_destroy(emulator);
    free(expected);

    return !run_failed && differing == 0 ? 0 : 1;
}
