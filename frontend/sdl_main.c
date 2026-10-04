/*
 * SDL2 front end: shows the LCD picture in a window, turns the keyboard
 * into Game Boy buttons and runs the machine at the real frame rate.
 *
 * Usage: gameboy-sdl <rom> [--scale N] [--gray] [--frames N] [--no-save]
 *                    [--mute]
 *
 *   --scale N   window size as a multiple of 160x144 (default 4)
 *   --gray      grayscale instead of the classic green palette
 *   --frames N  exit after N frames (for smoke tests)
 *   --no-save   do not load or write the battery save
 *   --mute      no sound
 *
 * Sound is played through SDL's audio queue, and the machine is paced by how
 * much is queued, so picture and sound stay together. Without an audio device
 * it falls back to pacing by the clock. The emulator's raw mix is high-pass
 * filtered here, as the hardware does, to take the DC offset out.
 *
 * A cartridge with a battery keeps its RAM (and an MBC3 clock) in a .sav file
 * next to the ROM, with the same name: it is loaded at start, written every
 * 30 seconds and when the program ends.
 *
 * Keys: arrows = D-pad, Z = A, X = B, Enter = Start, Right Shift or
 * Backspace = Select, Escape = quit.
 *
 * Only the emulator.h interface is used; SDL stays out of the core.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include <emulator.h>

enum {
    DEFAULT_SCALE = 4,
    SAVE_INTERVAL_MS = 30000,
    SAVE_PATH_CAPACITY = 1024,
    CYCLES_PER_FRAME = 70224,

    AUDIO_RATE = 48000,
    AUDIO_BUFFER_FRAMES = 1024,     /* SDL's device buffer */
    AUDIO_CHUNK_FRAMES = 2048,      /* taken from the emulator per video frame */
    AUDIO_LATENCY_FRAMES = 2400,    /* 50 ms queued ahead of the speakers */
    AUDIO_FRAME_BYTES = 2 * sizeof(int16_t)
};

/*
 * The DMG's output capacitor keeps 0.999958 of its charge each clock. At 48 kHz
 * a sample is 4194304 / 48000 = 87.38 clocks, so it keeps 0.999958^87.38.
 */
#define HIGH_PASS_CHARGE 0.996337f

/* 4194304 Hz / 70224 cycles per frame, as nanoseconds per frame. */
#define FRAME_NANOSECONDS 16742706ull

static const uint32_t PALETTE_GREEN[4] = {
    0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820
};

static const uint32_t PALETTE_GRAY[4] = {
    0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000
};

static uint8_t button_for_key(SDL_Keycode key)
{
    switch (key) {
        case SDLK_RIGHT:
            return EMULATOR_BUTTON_RIGHT;

        case SDLK_LEFT:
            return EMULATOR_BUTTON_LEFT;

        case SDLK_UP:
            return EMULATOR_BUTTON_UP;

        case SDLK_DOWN:
            return EMULATOR_BUTTON_DOWN;

        case SDLK_z:
            return EMULATOR_BUTTON_A;

        case SDLK_x:
            return EMULATOR_BUTTON_B;

        case SDLK_RETURN:
            return EMULATOR_BUTTON_START;

        case SDLK_RSHIFT:
        case SDLK_BACKSPACE:
            return EMULATOR_BUTTON_SELECT;

        default:
            return 0;
    }
}

static int parse_count(const char *text, unsigned long *value)
{
    char *end = NULL;

    errno = 0;
    *value = strtoul(text, &end, 10);

    return errno == 0 && end != text && *end == '\0' && *value > 0;
}

typedef struct Options {
    const char *rom;
    unsigned long scale;
    unsigned long frames;   /* 0 means run until the window is closed */
    const uint32_t *palette;
    int save;               /* load and write the battery save */
    int audio;              /* play sound */
} Options;

static int parse_options(int argc, char **argv, Options *options)
{
    options->rom = NULL;
    options->scale = DEFAULT_SCALE;
    options->frames = 0;
    options->palette = PALETTE_GREEN;
    options->save = 1;
    options->audio = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            if (!parse_count(argv[++i], &options->scale)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (!parse_count(argv[++i], &options->frames)) {
                return 0;
            }
        } else if (strcmp(argv[i], "--gray") == 0) {
            options->palette = PALETTE_GRAY;
        } else if (strcmp(argv[i], "--no-save") == 0) {
            options->save = 0;
        } else if (strcmp(argv[i], "--mute") == 0) {
            options->audio = 0;
        } else if (argv[i][0] != '-' && options->rom == NULL) {
            options->rom = argv[i];
        } else {
            return 0;
        }
    }

    return options->rom != NULL;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');

    if (backslash != NULL && (slash == NULL || backslash > slash)) {
        slash = backslash;
    }

    return slash == NULL ? path : slash + 1;
}

/* Sleeps until `deadline` (in performance counter ticks). */
static void wait_until(uint64_t deadline)
{
    uint64_t frequency = SDL_GetPerformanceFrequency();

    for (;;) {
        uint64_t now = SDL_GetPerformanceCounter();

        if (now >= deadline) {
            return;
        }

        uint64_t remaining_ms = (deadline - now) * 1000 / frequency;

        /* Sleep most of the wait, then spin for accuracy. */
        if (remaining_ms > 2) {
            SDL_Delay((uint32_t)(remaining_ms - 1));
        }
    }
}

/* "game.gb" -> "game.sav": the extension, if any, is replaced. */
static int save_path_for(const char *rom, char *out, size_t capacity)
{
    const char *name = base_name(rom);
    const char *dot = strrchr(name, '.');
    size_t stem = dot != NULL ? (size_t)(dot - rom) : strlen(rom);

    if (stem + sizeof(".sav") > capacity) {
        return 0;
    }

    memcpy(out, rom, stem);
    memcpy(out + stem, ".sav", sizeof(".sav"));

    return 1;
}

static void save_battery(Emulator *emulator, const char *save_path)
{
    EmulatorStatus status = emulator_save_battery(
        emulator, save_path, (uint64_t)time(NULL)
    );

    if (status != EMULATOR_OK) {
        fprintf(stderr, "Could not write %s: %s\n", save_path,
                emulator_status_string(status));
    }
}

static void draw_frame(const Emulator *emulator, const uint32_t *palette,
                       uint32_t *pixels)
{
    const uint8_t *shades = emulator_framebuffer(emulator);

    for (unsigned i = 0;
         i < EMULATOR_SCREEN_WIDTH * EMULATOR_SCREEN_HEIGHT; i++) {
        pixels[i] = palette[shades[i] & 3];
    }
}

/* The speakers: SDL's audio queue and the filter state that goes with it. */
typedef struct Audio {
    SDL_AudioDeviceID device;   /* 0 when there is no sound */
    float capacitor_left;
    float capacitor_right;
} Audio;

/* Opens the default device and tells the emulator to produce for it. */
static void audio_open(Audio *audio, Emulator *emulator)
{
    SDL_AudioSpec want;

    memset(audio, 0, sizeof(*audio));
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_BUFFER_FRAMES;

    /* Sound is optional: with no audio driver the machine runs silent. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "No sound: %s\n", SDL_GetError());
        return;
    }

    /* No changes allowed: SDL converts if the device wants something else. */
    audio->device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);

    if (audio->device == 0) {
        fprintf(stderr, "No sound: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }

    emulator_set_audio_sample_rate(emulator, AUDIO_RATE);
    SDL_PauseAudioDevice(audio->device, 0);
}

static void audio_close(Audio *audio)
{
    if (audio->device != 0) {
        SDL_CloseAudioDevice(audio->device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

/* One side of the hardware's high-pass filter: what the capacitor has not
 * yet charged to. */
static int16_t high_pass(float *capacitor, int16_t sample)
{
    float out = (float)sample - *capacitor;

    *capacitor = (float)sample - out * HIGH_PASS_CHARGE;

    if (out > INT16_MAX) {
        return INT16_MAX;
    }

    return out < INT16_MIN ? INT16_MIN : (int16_t)out;
}

/* Sends what the machine has produced since the last call to the speakers. */
static void audio_queue(Audio *audio, Emulator *emulator)
{
    static int16_t chunk[2 * AUDIO_CHUNK_FRAMES];
    size_t frames = emulator_take_audio(emulator, chunk, AUDIO_CHUNK_FRAMES);

    for (size_t i = 0; i < frames; i++) {
        chunk[2 * i] = high_pass(&audio->capacitor_left, chunk[2 * i]);
        chunk[2 * i + 1] = high_pass(&audio->capacitor_right,
                                     chunk[2 * i + 1]);
    }

    if (SDL_QueueAudio(audio->device, chunk,
                       (uint32_t)(frames * AUDIO_FRAME_BYTES)) != 0) {
        fprintf(stderr, "Could not queue sound: %s\n", SDL_GetError());
    }
}

/* Waits until the speakers have played the queue down to the latency
 * target. This is what keeps the machine at the real speed. */
static void audio_wait(const Audio *audio)
{
    while (SDL_GetQueuedAudioSize(audio->device) >
           AUDIO_LATENCY_FRAMES * AUDIO_FRAME_BYTES) {
        SDL_Delay(1);
    }
}

static int run(
    Emulator *emulator,
    const Options *options,
    Audio *audio,
    const char *save_path
)
{
    char title[256];

    snprintf(title, sizeof(title), "Game Boy - %s", base_name(options->rom));

    SDL_Window *window = SDL_CreateWindow(
        title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (int)(EMULATOR_SCREEN_WIDTH * options->scale),
        (int)(EMULATOR_SCREEN_HEIGHT * options->scale),
        SDL_WINDOW_RESIZABLE
    );

    if (window == NULL) {
        fprintf(stderr, "Could not create a window: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED
    );

    if (renderer == NULL) {
        /* No accelerated renderer (headless, remote): fall back. */
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (renderer == NULL) {
        fprintf(stderr, "Could not create a renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return 1;
    }

    /* Sharp pixels, and the picture keeps its shape when the window is
     * resized. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(renderer, EMULATOR_SCREEN_WIDTH,
                             EMULATOR_SCREEN_HEIGHT);

    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        EMULATOR_SCREEN_WIDTH, EMULATOR_SCREEN_HEIGHT
    );

    if (texture == NULL) {
        fprintf(stderr, "Could not create a texture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return 1;
    }

    static uint32_t pixels[EMULATOR_SCREEN_WIDTH * EMULATOR_SCREEN_HEIGHT];
    uint8_t pressed = 0;
    int exit_code = 0;
    int running = 1;
    unsigned long frame = 0;
    uint64_t frame_ticks =
        SDL_GetPerformanceFrequency() * FRAME_NANOSECONDS / 1000000000ull;
    uint64_t next_frame = SDL_GetPerformanceCounter();
    uint32_t last_save = SDL_GetTicks();

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    running = 0;
                    continue;
                }

                uint8_t button = button_for_key(event.key.keysym.sym);

                if (event.type == SDL_KEYDOWN) {
                    pressed |= button;
                } else {
                    pressed = (uint8_t)(pressed & ~button);
                }
            }
        }

        emulator_set_buttons(emulator, pressed);

        EmulatorStatus status = emulator_run_cycles(emulator, CYCLES_PER_FRAME);

        /* A CPU waiting in HALT forever just leaves the picture as it is. */
        if (status != EMULATOR_OK && status != EMULATOR_STALLED) {
            EmulatorFault fault;

            fprintf(stderr, "Emulation stopped: %s",
                    emulator_status_string(status));

            if (emulator_get_fault(emulator, &fault)) {
                fprintf(stderr, " 0x%02X at PC=0x%04X",
                        (unsigned)fault.opcode, (unsigned)fault.pc);
            }

            fputc('\n', stderr);
            exit_code = 1;
            break;
        }

        if (save_path != NULL &&
            SDL_GetTicks() - last_save >= SAVE_INTERVAL_MS) {
            save_battery(emulator, save_path);
            last_save = SDL_GetTicks();
        }

        if (audio->device != 0) {
            audio_queue(audio, emulator);
        }

        draw_frame(emulator, options->palette, pixels);
        SDL_UpdateTexture(texture, NULL, pixels,
                          EMULATOR_SCREEN_WIDTH * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        frame++;

        if (options->frames != 0 && frame >= options->frames) {
            break;
        }

        if (audio->device != 0) {
            audio_wait(audio);
            continue;
        }

        /* No sound to pace by: keep the real frame rate with the clock. If we
         * fall behind, do not try to catch up with a burst. */
        next_frame += frame_ticks;

        uint64_t now = SDL_GetPerformanceCounter();

        if (now > next_frame + frame_ticks * 5) {
            next_frame = now;
        }

        wait_until(next_frame);
    }

    if (save_path != NULL) {
        save_battery(emulator, save_path);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    return exit_code;
}

int main(int argc, char **argv)
{
    Options options;

    if (!parse_options(argc, argv, &options)) {
        fprintf(stderr,
                "Usage: %s <rom> [--scale N] [--gray] [--frames N] [--no-save]\n",
                argv[0]);
        return 1;
    }

    Emulator *emulator = emulator_create();

    if (emulator == NULL) {
        fprintf(stderr, "Failed to create emulator\n");
        return 1;
    }

    EmulatorStatus status = emulator_load_rom(emulator, options.rom);

    if (status != EMULATOR_OK) {
        uint8_t cartridge_type;

        fprintf(stderr, "Failed to load ROM %s: %s", options.rom,
                emulator_status_string(status));

        if (emulator_get_unsupported_cartridge_type(emulator,
                                                    &cartridge_type)) {
            fprintf(stderr, ", header type 0x%02X", (unsigned)cartridge_type);
        }

        fputc('\n', stderr);
        emulator_destroy(emulator);
        return 1;
    }

    /* Pick up the battery save left by the last session, if any. */
    char save_path[SAVE_PATH_CAPACITY];
    const char *save = NULL;

    if (options.save && emulator_has_battery(emulator)) {
        if (!save_path_for(options.rom, save_path, sizeof(save_path))) {
            fprintf(stderr, "ROM path too long for a save file\n");
        } else {
            save = save_path;
            status = emulator_load_battery(emulator, save, (uint64_t)time(NULL));

            if (status != EMULATOR_OK && status != EMULATOR_NO_SAVE_FILE) {
                fprintf(stderr, "Could not load %s: %s; not saving over it\n",
                        save, emulator_status_string(status));
                save = NULL;
            }
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "Could not start SDL: %s\n", SDL_GetError());
        emulator_destroy(emulator);
        return 1;
    }

    Audio audio = { 0 };

    if (options.audio) {
        audio_open(&audio, emulator);
    }

    int exit_code = run(emulator, &options, &audio, save);

    audio_close(&audio);
    SDL_Quit();
    emulator_destroy(emulator);

    return exit_code;
}
