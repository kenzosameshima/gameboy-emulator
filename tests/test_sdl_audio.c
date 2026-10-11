/* Exercise the front end with a deterministic audio device and clock. */
#include <assert.h>
#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "test_util.h"
#include "../src/emulator_internal.h"

static Uint32 queued;
static Uint32 ticks;
static bool draining;
static bool queue_failed;
static SDL_AudioStatus device_status;

static Uint32 fake_queued(SDL_AudioDeviceID device)
{
    (void)device;
    return queued;
}

static Uint32 fake_ticks(void)
{
    return ticks;
}

static void fake_delay(Uint32 milliseconds)
{
    ticks += milliseconds;
    if (draining && queued >= 4) {
        queued -= 4;
    }
}

static SDL_AudioStatus fake_status(SDL_AudioDeviceID device)
{
    (void)device;
    return device_status;
}

static int fake_queue(SDL_AudioDeviceID device, const void *data, Uint32 size)
{
    (void)device;
    (void)data;
    if (queue_failed) {
        return -1;
    }
    queued += size;
    return 0;
}

int sdl_frontend_main(int argc, char **argv);
#define main sdl_frontend_main
#define SDL_GetQueuedAudioSize fake_queued
#define SDL_GetTicks fake_ticks
#define SDL_Delay fake_delay
#define SDL_GetAudioDeviceStatus fake_status
#define SDL_QueueAudio fake_queue
#include "../frontend/sdl_main.c"
#undef main

static void test_stalled_cpu_produces_audio(void)
{
    static uint8_t rom[0x8000];
    rom[0x100] = 0x76; /* HALT with IE = 0 */
    const char *path = "test_sdl_audio.gb";
    test_write_file(path, rom, sizeof(rom));
    Emulator *emulator = emulator_create();
    assert(emulator != NULL);
    assert(emulator_load_rom(emulator, path) == EMULATOR_OK);
    emulator_set_audio_sample_rate(emulator, AUDIO_RATE);
    Audio audio = { .device = 1 };
    queued = 0;
    for (unsigned i = 0; i < 5; i++) {
        uint64_t before = emulator_cycles(emulator);
        assert(emulator_run_frame(emulator) == EMULATOR_STALLED);
        assert(emulator_is_stalled(emulator));
        if (i != 0) {
            assert(emulator_cycles(emulator) - before == 70224);
        }
        assert(audio_queue(&audio, emulator));
        /* More than 600 samples per frame, rather than a single M-cycle. */
        assert(queued >= 600 * AUDIO_FRAME_BYTES);
        queued = 0; /* device consumed the previous frame */
    }
    bus_write(&emulator->bus, PPU_LCDC_ADDRESS, 0);
    uint64_t before = emulator_cycles(emulator);
    assert(emulator_run_frame(emulator) == EMULATOR_STALLED);
    assert(emulator_cycles(emulator) - before == 70224);
    assert(audio_queue(&audio, emulator));
    assert(queued >= 800 * AUDIO_FRAME_BYTES);
    emulator_stop(emulator);
    before = emulator_cycles(emulator);
    assert(emulator_run_frame(emulator) == EMULATOR_OK);
    assert(emulator_cycles(emulator) == before);
    queue_failed = true;
    assert(!audio_queue(&audio, emulator));
    queue_failed = false;
    emulator_destroy(emulator);
    assert(remove(path) == 0);
}

static void test_audio_wait_is_bounded(void)
{
    Audio audio = { .device = 1 };
    Uint32 target = AUDIO_LATENCY_FRAMES * AUDIO_FRAME_BYTES;
    device_status = SDL_AUDIO_PLAYING;
    queued = target + 40;
    ticks = 0;
    draining = true;
    assert(audio_wait(&audio));
    assert(ticks == 10);

    draining = false;
    queued = target + 40;
    ticks = 0;
    assert(!audio_wait(&audio));
    assert(ticks == AUDIO_WAIT_TIMEOUT_MS);
    /* Unsigned elapsed time also handles SDL's millisecond clock wrapping. */
    ticks = UINT32_MAX - 10;
    assert(!audio_wait(&audio));
    assert(ticks == AUDIO_WAIT_TIMEOUT_MS - 11);

    device_status = SDL_AUDIO_STOPPED;
    ticks = 0;
    assert(!audio_wait(&audio));
    assert(ticks == 0);
    queued = target;
    assert(audio_wait(&audio));
}

int main(void)
{
    test_stalled_cpu_produces_audio();
    test_audio_wait_is_bounded();
    puts("SDL audio pacing tests passed!");
    return 0;
}
