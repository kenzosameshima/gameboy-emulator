/*
 * SDL2 front end: shows the LCD picture in a window, turns the keyboard
 * into Game Boy buttons and runs the machine at the real frame rate.
 *
 * Usage: gameboy-sdl [<rom>] [--scale N] [--gray] [--frames N] [--no-save]
 *                    [--mute]
 *
 * Without a ROM the window waits for one: drop a ROM file on it. A ROM
 * dropped while a game runs replaces it, after the old game's battery save
 * is written.
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
 * Errors go to stderr and to a message box: on Windows the front end is a
 * GUI program with no console, so stderr is lost there.
 *
 * Only the emulator.h interface is used; SDL stays out of the core.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
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

    AUDIO_WAIT_TIMEOUT_MS = 250,
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

    return 1;
}

static void report(SDL_Window *window, SDL_PRINTF_FORMAT_STRING const char *format,
                   ...) SDL_PRINTF_VARARG_FUNC(2);

/* Tells the player about an error, on stderr and in a message box that
 * waits to be closed. `window` is the box's parent, or NULL. */
static void report(SDL_Window *window, const char *format, ...)
{
    char message[1024];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    fprintf(stderr, "%s\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game Boy", message, window);
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

/*
 * Writes the battery save. A failure is shown in a box the first time, so
 * the player knows the game is not being kept, and for the last save of a
 * game, which nothing will retry. Later periodic failures go to stderr.
 */
static void save_battery(Emulator *emulator, const char *save_path,
                         SDL_Window *window, bool last)
{
    static bool warned = false;
    EmulatorStatus status = emulator_save_battery(
        emulator, save_path, (uint64_t)time(NULL)
    );

    if (status == EMULATOR_OK) {
        return;
    }

    if (last || !warned) {
        warned = true;
        report(window, "Could not write %s: %s", save_path,
               emulator_status_string(status));
    } else {
        fprintf(stderr, "Could not write %s: %s\n", save_path,
                emulator_status_string(status));
    }
}

/* The game in the machine and its battery save. */
typedef struct Game {
    bool loaded;
    bool saving;            /* save_path is loaded from and written to */
    char save_path[SAVE_PATH_CAPACITY];
} Game;

/*
 * Puts `rom` in the machine and picks up the battery save the last session
 * left. If the ROM does not load, the machine and `game` keep the game they
 * had.
 */
static bool load_game(Emulator *emulator, Game *game, const char *rom,
                      const Options *options, SDL_Window *window)
{
    EmulatorStatus status = emulator_load_rom(emulator, rom);

    if (status != EMULATOR_OK) {
        uint8_t type;

        if (emulator_get_unsupported_cartridge_type(emulator, &type)) {
            report(window, "Failed to load ROM %s: %s, header type 0x%02X",
                   rom, emulator_status_string(status), (unsigned)type);
        } else {
            report(window, "Failed to load ROM %s: %s", rom,
                   emulator_status_string(status));
        }

        return false;
    }

    game->loaded = true;
    game->saving = false;

    if (!options->save || !emulator_has_battery(emulator)) {
        return true;
    }

    if (!save_path_for(rom, game->save_path, sizeof(game->save_path))) {
        report(window, "ROM path too long for a save file: the game will "
               "not be saved");
        return true;
    }

    status = emulator_load_battery(emulator, game->save_path,
                                   (uint64_t)time(NULL));

    if (status != EMULATOR_OK && status != EMULATOR_NO_SAVE_FILE) {
        report(window, "Could not load %s: %s; not saving over it",
               game->save_path, emulator_status_string(status));
        return true;
    }

    game->saving = true;

    return true;
}

static void set_title(SDL_Window *window, const char *rom)
{
    char title[256];

    if (rom == NULL) {
        SDL_SetWindowTitle(window, "Game Boy - drop a ROM file here");
        return;
    }

    snprintf(title, sizeof(title), "Game Boy - %s", base_name(rom));
    SDL_SetWindowTitle(window, title);
}

/* A ROM dropped on the window. The old game's battery is written first, so
 * its progress is kept whether or not the new ROM loads. */
static void change_game(Emulator *emulator, Game *game, const char *rom,
                        const Options *options, SDL_Window *window)
{
    if (game->saving) {
        save_battery(emulator, game->save_path, window, true);
    }

    if (load_game(emulator, game, rom, options, window)) {
        set_title(window, rom);
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
        audio->device = 0;
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
static bool audio_queue(Audio *audio, Emulator *emulator)
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
        return false;
    }
    return true;
}

/* Waits until the speakers have played the queue down to the latency
 * target. This is what keeps the machine at the real speed. */
static bool audio_wait(const Audio *audio)
{
    uint32_t start = SDL_GetTicks();
    while (SDL_GetQueuedAudioSize(audio->device) >
           AUDIO_LATENCY_FRAMES * AUDIO_FRAME_BYTES) {
        if (SDL_GetAudioDeviceStatus(audio->device) != SDL_AUDIO_PLAYING ||
            SDL_GetTicks() - start >= AUDIO_WAIT_TIMEOUT_MS) {
            return false;
        }
        SDL_Delay(1);
    }
    return true;
}

static int run(
    Emulator *emulator,
    const Options *options,
    Audio *audio,
    Game *game
)
{
    SDL_Window *window = SDL_CreateWindow(
        "Game Boy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (int)(EMULATOR_SCREEN_WIDTH * options->scale),
        (int)(EMULATOR_SCREEN_HEIGHT * options->scale),
        SDL_WINDOW_RESIZABLE
    );

    if (window == NULL) {
        report(NULL, "Could not create a window: %s", SDL_GetError());
        return 1;
    }

    set_title(window, game->loaded ? options->rom : NULL);

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED
    );

    if (renderer == NULL) {
        /* No accelerated renderer (headless, remote): fall back. */
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (renderer == NULL) {
        report(window, "Could not create a renderer: %s", SDL_GetError());
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
        report(window, "Could not create a texture: %s", SDL_GetError());
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

        /* With no game there is nothing to run: sleep until an event. */
        if (!game->loaded) {
            SDL_WaitEvent(NULL);
        }

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_DROPFILE) {
                change_game(emulator, game, event.drop.file, options, window);
                SDL_free(event.drop.file);
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

        if (!game->loaded) {
            SDL_RenderClear(renderer);
            SDL_RenderPresent(renderer);
            continue;
        }

        emulator_set_buttons(emulator, pressed);

        EmulatorStatus status = emulator_run_frame(emulator);

        /* A CPU waiting in HALT forever just leaves the picture as it is. */
        if (status != EMULATOR_OK && status != EMULATOR_STALLED) {
            EmulatorFault fault;

            if (emulator_get_fault(emulator, &fault)) {
                report(window, "Emulation stopped: %s 0x%02X at PC=0x%04X",
                       emulator_status_string(status),
                       (unsigned)fault.opcode, (unsigned)fault.pc);
            } else {
                report(window, "Emulation stopped: %s",
                       emulator_status_string(status));
            }

            exit_code = 1;
            break;
        }

        if (game->saving &&
            SDL_GetTicks() - last_save >= SAVE_INTERVAL_MS) {
            save_battery(emulator, game->save_path, window, false);
            last_save = SDL_GetTicks();
        }

        if (audio->device != 0 && !audio_queue(audio, emulator)) {
            audio_close(audio);
            emulator_set_audio_sample_rate(emulator, 0);
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
            if (audio_wait(audio)) {
                next_frame = SDL_GetPerformanceCounter();
                continue;
            }
            fprintf(stderr, "Sound stopped draining; using clock pacing\n");
            audio_close(audio);
            emulator_set_audio_sample_rate(emulator, 0);
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

    if (game->saving) {
        save_battery(emulator, game->save_path, window, true);
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
        report(NULL, "Usage: %s [<rom>] [--scale N] [--gray] [--frames N] "
               "[--no-save] [--mute]", argv[0]);
        return 1;
    }

    Emulator *emulator = emulator_create();

    if (emulator == NULL) {
        report(NULL, "Failed to create emulator");
        return 1;
    }

    Game game = { 0 };

    if (options.rom != NULL &&
        !load_game(emulator, &game, options.rom, &options, NULL)) {
        emulator_destroy(emulator);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        report(NULL, "Could not start SDL: %s", SDL_GetError());
        emulator_destroy(emulator);
        return 1;
    }

    Audio audio = { 0 };

    if (options.audio) {
        audio_open(&audio, emulator);
    }

    int exit_code = run(emulator, &options, &audio, &game);

    audio_close(&audio);
    SDL_Quit();
    emulator_destroy(emulator);

    return exit_code;
}
