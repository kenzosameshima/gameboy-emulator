/*
 * Runs a ROM headless for a number of frames and saves the LCD picture.
 *
 * Usage: frame_dump <rom> <out> [frames] [scale] [--press name@from-to]...
 *
 *   out     *.png  8-bit grayscale PNG (uncompressed), scaled by `scale`
 *           other  raw shades, one byte (0-3) per pixel, 160x144, no header
 *   frames  frames to run before saving (default 60)
 *   scale   integer pixel scale for PNG output (default 3)
 *   --press holds a button from one frame up to, but not including, another;
 *           names are right left up down a b select start, for example
 *           --press start@300-305. May be repeated.
 *
 * Exit code: 0 on success, 1 on any error.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emulator.h>

enum {
    DEFAULT_FRAMES = 60,
    DEFAULT_SCALE = 3,
    CYCLES_PER_SLICE = 4560,    /* ten lines, so input lands on frame edges */
    MAX_PRESSES = 32,
    STORED_BLOCK_MAX = 65535
};

/* Darkest shade 3 is black, lightest shade 0 is white. */
static uint8_t shade_to_gray(uint8_t shade)
{
    return (uint8_t)(255 - shade * 85);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];

        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }

    return crc;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static int write_chunk(FILE *file, const char *type, const uint8_t *data,
                       size_t size)
{
    uint8_t header[8];
    uint8_t trailer[4];
    uint32_t crc = 0xFFFFFFFFu;

    put_u32(header, (uint32_t)size);
    memcpy(header + 4, type, 4);

    crc = crc32_update(crc, (const uint8_t *)type, 4);
    crc = crc32_update(crc, data, size);
    put_u32(trailer, crc ^ 0xFFFFFFFFu);

    return fwrite(header, 1, 8, file) == 8 &&
           (size == 0 || fwrite(data, 1, size, file) == size) &&
           fwrite(trailer, 1, 4, file) == 4;
}

/* A zlib stream made of stored (uncompressed) deflate blocks. */
static uint8_t *zlib_store(const uint8_t *raw, size_t raw_size,
                           size_t *out_size)
{
    size_t blocks = (raw_size + STORED_BLOCK_MAX - 1) / STORED_BLOCK_MAX;
    size_t size = 2 + blocks * 5 + raw_size + 4;
    uint8_t *out = malloc(size);

    if (out == NULL) {
        return NULL;
    }

    size_t position = 0;
    uint32_t a = 1;
    uint32_t b = 0;

    out[position++] = 0x78;
    out[position++] = 0x01;

    for (size_t offset = 0; offset < raw_size; offset += STORED_BLOCK_MAX) {
        size_t length = raw_size - offset;

        if (length > STORED_BLOCK_MAX) {
            length = STORED_BLOCK_MAX;
        }

        out[position++] = offset + length == raw_size ? 1 : 0;
        out[position++] = (uint8_t)(length & 0xFF);
        out[position++] = (uint8_t)(length >> 8);
        out[position++] = (uint8_t)(~length & 0xFF);
        out[position++] = (uint8_t)((~length >> 8) & 0xFF);
        memcpy(out + position, raw + offset, length);
        position += length;
    }

    for (size_t i = 0; i < raw_size; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }

    put_u32(out + position, (b << 16) | a);
    position += 4;
    *out_size = position;

    return out;
}

static int write_png(const char *path, const uint8_t *shades, unsigned scale)
{
    unsigned width = EMULATOR_SCREEN_WIDTH * scale;
    unsigned height = EMULATOR_SCREEN_HEIGHT * scale;
    size_t raw_size = (size_t)(width + 1) * height;
    uint8_t *raw = malloc(raw_size);

    if (raw == NULL) {
        return 0;
    }

    for (unsigned y = 0; y < height; y++) {
        uint8_t *row = raw + (size_t)y * (width + 1);

        row[0] = 0; /* no filter */

        for (unsigned x = 0; x < width; x++) {
            row[1 + x] = shade_to_gray(
                shades[(y / scale) * EMULATOR_SCREEN_WIDTH + (x / scale)]
            );
        }
    }

    size_t compressed_size = 0;
    uint8_t *compressed = zlib_store(raw, raw_size, &compressed_size);

    free(raw);

    if (compressed == NULL) {
        return 0;
    }

    FILE *file = fopen(path, "wb");
    static const uint8_t signature[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    uint8_t ihdr[13];
    int ok = file != NULL;

    put_u32(ihdr, width);
    put_u32(ihdr + 4, height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 0;  /* grayscale */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    ok = ok && fwrite(signature, 1, 8, file) == 8 &&
         write_chunk(file, "IHDR", ihdr, sizeof(ihdr)) &&
         write_chunk(file, "IDAT", compressed, compressed_size) &&
         write_chunk(file, "IEND", NULL, 0);

    if (file != NULL) {
        ok = fclose(file) == 0 && ok;
    }

    free(compressed);

    return ok;
}

static int write_raw(const char *path, const uint8_t *shades)
{
    FILE *file = fopen(path, "wb");
    size_t size = (size_t)EMULATOR_SCREEN_WIDTH * EMULATOR_SCREEN_HEIGHT;

    if (file == NULL) {
        return 0;
    }

    int ok = fwrite(shades, 1, size, file) == size;

    return fclose(file) == 0 && ok;
}

static int parse_count(const char *text, unsigned long *value)
{
    char *end = NULL;

    errno = 0;
    *value = strtoul(text, &end, 10);

    return errno == 0 && end != text && *end == '\0' && *value > 0;
}

/* A button held from one frame up to, but not including, another. */
typedef struct Press {
    uint8_t mask;
    unsigned long from;
    unsigned long to;
} Press;

static const struct {
    const char *name;
    uint8_t mask;
} BUTTONS[] = {
    { "right", EMULATOR_BUTTON_RIGHT },
    { "left", EMULATOR_BUTTON_LEFT },
    { "up", EMULATOR_BUTTON_UP },
    { "down", EMULATOR_BUTTON_DOWN },
    { "a", EMULATOR_BUTTON_A },
    { "b", EMULATOR_BUTTON_B },
    { "select", EMULATOR_BUTTON_SELECT },
    { "start", EMULATOR_BUTTON_START }
};

/* Parses "name@from-to", for example "start@300-305". */
static int parse_press(const char *text, Press *press)
{
    const char *at = strchr(text, '@');
    char *end = NULL;

    if (at == NULL) {
        return 0;
    }

    press->mask = 0;

    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
        if (strlen(BUTTONS[i].name) == (size_t)(at - text) &&
            strncmp(BUTTONS[i].name, text, (size_t)(at - text)) == 0) {
            press->mask = BUTTONS[i].mask;
        }
    }

    errno = 0;
    press->from = strtoul(at + 1, &end, 10);

    if (press->mask == 0 || errno != 0 || end == at + 1 || *end != '-') {
        return 0;
    }

    press->to = strtoul(end + 1, &end, 10);

    return errno == 0 && *end == '\0' && press->to > press->from;
}

static uint8_t held_at(const Press *presses, size_t count,
                       unsigned long frame)
{
    uint8_t mask = 0;

    for (size_t i = 0; i < count; i++) {
        if (frame >= presses[i].from && frame < presses[i].to) {
            mask |= presses[i].mask;
        }
    }

    return mask;
}

int main(int argc, char **argv)
{
    const char *positional[4] = { NULL, NULL, NULL, NULL };
    size_t positional_count = 0;
    Press presses[MAX_PRESSES];
    size_t press_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--press") == 0) {
            if (i + 1 >= argc || press_count == MAX_PRESSES ||
                !parse_press(argv[i + 1], &presses[press_count])) {
                fprintf(stderr, "Invalid --press (use name@from-to)\n");
                return 1;
            }

            press_count++;
            i++;
        } else if (positional_count < 4) {
            positional[positional_count++] = argv[i];
        } else {
            positional_count = 5;
        }
    }

    if (positional_count < 2 || positional_count > 4) {
        fprintf(stderr,
                "Usage: %s <rom> <out.png|out.raw> [frames] [scale] "
                "[--press name@from-to]...\n", argv[0]);
        return 1;
    }

    unsigned long frames = DEFAULT_FRAMES;
    unsigned long scale = DEFAULT_SCALE;

    if ((positional_count > 2 && !parse_count(positional[2], &frames)) ||
        (positional_count > 3 && !parse_count(positional[3], &scale))) {
        fprintf(stderr, "Invalid frame count or scale\n");
        return 1;
    }

    Emulator *emulator = emulator_create();

    if (emulator == NULL) {
        fprintf(stderr, "Failed to create emulator\n");
        return 1;
    }

    EmulatorStatus status = emulator_load_rom(emulator, positional[0]);
    unsigned long applied_frame = (unsigned long)-1;

    while (status == EMULATOR_OK && emulator_frame_count(emulator) < frames) {
        unsigned long frame = (unsigned long)emulator_frame_count(emulator);

        if (frame != applied_frame) {
            emulator_set_buttons(emulator,
                                 held_at(presses, press_count, frame));
            applied_frame = frame;
        }

        status = emulator_run_cycles(emulator, CYCLES_PER_SLICE);
    }

    /* A CPU that stopped for good just leaves the picture as it is. */
    if (status != EMULATOR_OK && status != EMULATOR_STALLED) {
        fprintf(stderr, "%s: %s\n", positional[0],
                emulator_status_string(status));
        emulator_destroy(emulator);
        return 1;
    }

    const char *out = positional[1];
    size_t name_length = strlen(out);
    int is_png = name_length > 4 &&
                 strcmp(out + name_length - 4, ".png") == 0;
    const uint8_t *shades = emulator_framebuffer(emulator);
    int ok = is_png
        ? write_png(out, shades, (unsigned)scale)
        : write_raw(out, shades);

    if (!ok) {
        fprintf(stderr, "Could not write %s\n", out);
    }

    printf("%s: %llu frames, %llu cycles%s\n", positional[0],
           (unsigned long long)emulator_frame_count(emulator),
           (unsigned long long)emulator_cycles(emulator),
           status == EMULATOR_STALLED ? " (CPU stalled)" : "");

    emulator_destroy(emulator);

    return ok ? 0 : 1;
}
