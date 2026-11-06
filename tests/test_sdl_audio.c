/* Exercise the front end with a deterministic audio device and clock, and
 * with message boxes that only count what they would show. */
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

static int message_boxes;
static char last_message[1024];

static int fake_message_box(Uint32 flags, const char *title,
                            const char *message, SDL_Window *window)
{
    (void)flags;
    (void)title;
    (void)window;
    message_boxes++;
    snprintf(last_message, sizeof(last_message), "%s", message);
    return 0;
}

int sdl_frontend_main(int argc, char **argv);
/* SDL's flags define main as SDL_main on Windows. */
#undef main
#define main sdl_frontend_main
#define SDL_GetQueuedAudioSize fake_queued
#define SDL_GetTicks fake_ticks
#define SDL_Delay fake_delay
#define SDL_GetAudioDeviceStatus fake_status
#define SDL_QueueAudio fake_queue
#define SDL_ShowSimpleMessageBox fake_message_box
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

/* A ROM dropped on the window replaces the game after the old game's
 * battery is written. One that does not load is shown in a box and leaves
 * the game as it was. */
static void test_dropped_rom_replaces_the_game(void)
{
    static uint8_t rom[0x8000];
    rom[0x100] = 0x18; /* JR -2 */
    rom[0x101] = 0xFE;
    rom[0x147] = 0x03; /* MBC1 + RAM + battery */
    rom[0x149] = 0x02; /* 8 KiB of RAM */
    test_write_file("test_sdl_drop_a.gb", rom, sizeof(rom));
    test_write_file("test_sdl_drop_b.gb", rom, sizeof(rom));

    Options options = { .save = 1 };
    Game game = { 0 };
    Emulator *emulator = emulator_create();
    assert(emulator != NULL);
    message_boxes = 0;

    assert(load_game(emulator, &game, "test_sdl_drop_a.gb", &options, NULL));
    assert(game.loaded && game.saving);

    change_game(emulator, &game, "test_sdl_drop_b.gb", &options, NULL);
    assert(strcmp(game.save_path, "test_sdl_drop_b.sav") == 0);
    assert(message_boxes == 0);
    /* The old game's save was written on the way out. */
    assert(remove("test_sdl_drop_a.sav") == 0);

    change_game(emulator, &game, "test_sdl_drop_missing.gb", &options, NULL);
    assert(message_boxes == 1);
    assert(strstr(last_message, "Failed to load ROM") != NULL);
    assert(game.saving);
    assert(strcmp(game.save_path, "test_sdl_drop_b.sav") == 0);

    emulator_destroy(emulator);
    assert(remove("test_sdl_drop_b.sav") == 0);
    assert(remove("test_sdl_drop_a.gb") == 0);
    assert(remove("test_sdl_drop_b.gb") == 0);
}

int main(void)
{
    test_stalled_cpu_produces_audio();
    test_audio_wait_is_bounded();
    test_dropped_rom_replaces_the_game();
    puts("SDL audio pacing tests passed!");
    return 0;
}
