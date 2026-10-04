/*
 * Audio through the emulator.h interface: a program plays a note on channel
 * 2, and the frames come out of emulator_take_audio().
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emulator.h>

#include "test_util.h"

enum {
    ROM_SIZE = 0x8000,
    RATE = 48000,

    /* A frame of video is 70224 cycles: 803.4 audio frames at 48 kHz. */
    CYCLES_PER_VIDEO_FRAME = 70224
};

static const char *rom_path = "test_emulator_audio_rom.gb";

/* Plays channel 2 at full volume on both sides, then spins. */
static const uint8_t note[] = {
    0x3E, 0x80, 0xE0, 0x26,     /* NR52: power on */
    0x3E, 0x77, 0xE0, 0x24,     /* NR50: full volume */
    0x3E, 0x22, 0xE0, 0x25,     /* NR51: channel 2 to both sides */
    0x3E, 0x80, 0xE0, 0x16,     /* NR21: 50% duty */
    0x3E, 0xF0, 0xE0, 0x17,     /* NR22: volume 15, no envelope */
    0x3E, 0x00, 0xE0, 0x18,     /* NR23 */
    0x3E, 0x84, 0xE0, 0x19,     /* NR24: trigger, frequency 0x400 */
    0x18, 0xFE                  /* spin */
};

static Emulator *boot(void)
{
    static uint8_t rom[ROM_SIZE];

    memset(rom, 0, sizeof(rom));
    memcpy(rom + 0x0100, note, sizeof(note));
    test_write_file(rom_path, rom, ROM_SIZE);

    Emulator *emulator = emulator_create();

    assert(emulator != NULL);
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);

    return emulator;
}

static void test_no_rate_no_audio(void)
{
    Emulator *emulator = boot();
    int16_t out[64];

    assert(emulator_run_cycles(emulator, CYCLES_PER_VIDEO_FRAME) ==
           EMULATOR_OK);
    assert(emulator_take_audio(emulator, out, 32) == 0);
    emulator_destroy(emulator);
}

static void test_frames_come_out_at_the_rate(void)
{
    static int16_t out[2 * 2048];
    Emulator *emulator = boot();

    emulator_set_audio_sample_rate(emulator, RATE);
    assert(emulator_run_cycles(emulator, CYCLES_PER_VIDEO_FRAME) ==
           EMULATOR_OK);

    size_t frames = emulator_take_audio(emulator, out, 2048);

    /* 70224 cycles at 48 kHz is 803.4 frames; the run overshoots by less
     * than one instruction. */
    assert(frames >= 803 && frames <= 806);

    /* The note is on: both sides carry the pulse, swinging well past 0 in
     * both directions, and equally. */
    int high = 0;
    int low = 0;

    for (size_t i = 0; i < frames; i++) {
        assert(out[2 * i] == out[2 * i + 1]);

        if (out[2 * i] > high) {
            high = out[2 * i];
        }

        if (out[2 * i] < low) {
            low = out[2 * i];
        }
    }

    assert(high > 8000);
    assert(low < -8000);

    /* Everything was taken. */
    assert(emulator_take_audio(emulator, out, 2048) == 0);
    emulator_destroy(emulator);
}

/* The rate is a setting of the front end, not of the machine: loading
 * another ROM keeps it. */
static void test_rate_survives_loading_a_rom(void)
{
    static int16_t out[2 * 2048];
    Emulator *emulator = boot();

    emulator_set_audio_sample_rate(emulator, RATE);
    assert(emulator_load_rom(emulator, rom_path) == EMULATOR_OK);
    assert(emulator_run_cycles(emulator, CYCLES_PER_VIDEO_FRAME) ==
           EMULATOR_OK);
    assert(emulator_take_audio(emulator, out, 2048) >= 803);
    emulator_destroy(emulator);
}

static void test_arguments(void)
{
    int16_t out[8];

    emulator_set_audio_sample_rate(NULL, RATE);
    assert(emulator_take_audio(NULL, out, 4) == 0);

    Emulator *emulator = boot();

    assert(emulator_take_audio(emulator, NULL, 4) == 0);
    emulator_destroy(emulator);
}

int main(void)
{
    test_no_rate_no_audio();
    test_frames_come_out_at_the_rate();
    test_rate_survives_loading_a_rom();
    test_arguments();

    assert(remove(rom_path) == 0);

    printf("Emulator audio tests passed!\n");

    return 0;
}
