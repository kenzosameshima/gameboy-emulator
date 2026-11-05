#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_read.h"

/*
 * Inflate, after zlib's puff.c: the canonical Huffman codes are decoded one
 * bit at a time from per-length counts, which is slow and small, and plenty
 * for images of 160 x 144 pixels.
 */

enum {
    MAX_BITS = 15,
    MAX_L_CODES = 286,
    MAX_D_CODES = 30,
    MAX_CODES = MAX_L_CODES + MAX_D_CODES,
    FIX_L_CODES = 288
};

typedef struct Huffman {
    short count[MAX_BITS + 1];
    short symbol[MAX_CODES];
} Huffman;

typedef struct Inflate {
    const uint8_t *in;
    size_t in_size;
    size_t in_count;
    unsigned bit_buffer;
    unsigned bit_count;

    uint8_t *out;
    size_t out_size;
    size_t out_count;
} Inflate;

/* Reads `need` bits, or returns -1 at the end of the input. */
static int get_bits(Inflate *s, unsigned need)
{
    unsigned long value = s->bit_buffer;

    while (s->bit_count < need) {
        if (s->in_count == s->in_size) {
            return -1;
        }

        value |= (unsigned long)s->in[s->in_count++] << s->bit_count;
        s->bit_count += 8;
    }

    s->bit_buffer = (unsigned)(value >> need);
    s->bit_count -= need;

    return (int)(value & ((1UL << need) - 1));
}

static int decode(Inflate *s, const Huffman *h)
{
    int code = 0;
    int first = 0;
    int index = 0;

    for (unsigned len = 1; len <= MAX_BITS; len++) {
        int bit = get_bits(s, 1);

        if (bit < 0) {
            return -1;
        }

        code |= bit;

        int count = h->count[len];

        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }

        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }

    return -1;
}

/* Builds the decoding tables; returns 0 for a complete code. */
static int construct(Huffman *h, const short *length, unsigned n)
{
    short offsets[MAX_BITS + 1];

    for (unsigned len = 0; len <= MAX_BITS; len++) {
        h->count[len] = 0;
    }

    for (unsigned symbol = 0; symbol < n; symbol++) {
        h->count[length[symbol]]++;
    }

    if (h->count[0] == (short)n) {
        return 0;
    }

    int left = 1;

    for (unsigned len = 1; len <= MAX_BITS; len++) {
        left <<= 1;
        left -= h->count[len];

        if (left < 0) {
            return left;
        }
    }

    offsets[1] = 0;

    for (unsigned len = 1; len < MAX_BITS; len++) {
        offsets[len + 1] = (short)(offsets[len] + h->count[len]);
    }

    for (unsigned symbol = 0; symbol < n; symbol++) {
        if (length[symbol] != 0) {
            h->symbol[offsets[length[symbol]]++] = (short)symbol;
        }
    }

    return left;
}

static int inflate_codes(Inflate *s, const Huffman *lengths, const Huffman *distances)
{
    static const short length_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const short length_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const short distance_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const short distance_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;) {
        int symbol = decode(s, lengths);

        if (symbol < 0) {
            return 1;
        }

        if (symbol < 256) {
            if (s->out_count == s->out_size) {
                return 1;
            }

            s->out[s->out_count++] = (uint8_t)symbol;
            continue;
        }

        if (symbol == 256) {
            return 0;
        }

        symbol -= 257;

        if (symbol >= 29) {
            return 1;
        }

        int extra = get_bits(s, (unsigned)length_extra[symbol]);

        if (extra < 0) {
            return 1;
        }

        size_t length = (size_t)(length_base[symbol] + extra);

        symbol = decode(s, distances);

        if (symbol < 0 || symbol >= 30) {
            return 1;
        }

        extra = get_bits(s, (unsigned)distance_extra[symbol]);

        if (extra < 0) {
            return 1;
        }

        size_t distance = (size_t)(distance_base[symbol] + extra);

        if (distance > s->out_count || length > s->out_size - s->out_count) {
            return 1;
        }

        for (size_t i = 0; i < length; i++) {
            s->out[s->out_count] = s->out[s->out_count - distance];
            s->out_count++;
        }
    }
}

static int inflate_stored(Inflate *s)
{
    s->bit_buffer = 0;
    s->bit_count = 0;

    if (s->in_size - s->in_count < 4) {
        return 1;
    }

    unsigned length = s->in[s->in_count] | (unsigned)(s->in[s->in_count + 1] << 8);
    unsigned inverse = s->in[s->in_count + 2] | (unsigned)(s->in[s->in_count + 3] << 8);

    s->in_count += 4;

    if (length != (~inverse & 0xFFFFu) || length > s->in_size - s->in_count ||
        length > s->out_size - s->out_count) {
        return 1;
    }

    memcpy(s->out + s->out_count, s->in + s->in_count, length);
    s->in_count += length;
    s->out_count += length;

    return 0;
}

static int inflate_fixed(Inflate *s)
{
    Huffman lengths;
    Huffman distances;
    short table[FIX_L_CODES];
    unsigned symbol = 0;

    for (; symbol < 144; symbol++) {
        table[symbol] = 8;
    }

    for (; symbol < 256; symbol++) {
        table[symbol] = 9;
    }

    for (; symbol < 280; symbol++) {
        table[symbol] = 7;
    }

    for (; symbol < FIX_L_CODES; symbol++) {
        table[symbol] = 8;
    }

    construct(&lengths, table, FIX_L_CODES);

    for (symbol = 0; symbol < MAX_D_CODES; symbol++) {
        table[symbol] = 5;
    }

    construct(&distances, table, MAX_D_CODES);

    return inflate_codes(s, &lengths, &distances);
}

static int inflate_dynamic(Inflate *s)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    short lengths[MAX_CODES];
    Huffman length_code;
    Huffman distance_code;

    int nlen = get_bits(s, 5);
    int ndist = get_bits(s, 5);
    int ncode = get_bits(s, 4);

    if (nlen < 0 || ndist < 0 || ncode < 0) {
        return 1;
    }

    nlen += 257;
    ndist += 1;
    ncode += 4;

    if (nlen > MAX_L_CODES || ndist > MAX_D_CODES) {
        return 1;
    }

    int index = 0;

    for (; index < ncode; index++) {
        int value = get_bits(s, 3);

        if (value < 0) {
            return 1;
        }

        lengths[order[index]] = (short)value;
    }

    for (; index < 19; index++) {
        lengths[order[index]] = 0;
    }

    if (construct(&length_code, lengths, 19) != 0) {
        return 1;
    }

    index = 0;

    while (index < nlen + ndist) {
        int symbol = decode(s, &length_code);

        if (symbol < 0) {
            return 1;
        }

        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }

        short previous = 0;
        int repeat;

        if (symbol == 16) {
            if (index == 0) {
                return 1;
            }

            previous = lengths[index - 1];
            repeat = get_bits(s, 2);
            repeat += 3;
        } else if (symbol == 17) {
            repeat = get_bits(s, 3);
            repeat += 3;
        } else {
            repeat = get_bits(s, 7);
            repeat += 11;
        }

        if (repeat < 3 || index + repeat > nlen + ndist) {
            return 1;
        }

        while (repeat-- > 0) {
            lengths[index++] = previous;
        }
    }

    if (lengths[256] == 0) {
        return 1;
    }

    /* An incomplete code is only valid when it has a single code of length 1. */
    int err = construct(&length_code, lengths, (unsigned)nlen);

    if (err != 0 &&
        (err < 0 || nlen != length_code.count[0] + length_code.count[1])) {
        return 1;
    }

    err = construct(&distance_code, lengths + nlen, (unsigned)ndist);

    if (err != 0 &&
        (err < 0 || ndist != distance_code.count[0] + distance_code.count[1])) {
        return 1;
    }

    return inflate_codes(s, &length_code, &distance_code);
}

/* Inflates a raw deflate stream into `out`, which must be exactly sized. */
static int inflate_stream(
    const uint8_t *in,
    size_t in_size,
    uint8_t *out,
    size_t out_size
)
{
    Inflate s = { in, in_size, 0, 0, 0, out, out_size, 0 };
    int last;

    do {
        last = get_bits(&s, 1);

        int type = get_bits(&s, 2);

        if (last < 0 || type < 0) {
            return 1;
        }

        int err;

        switch (type) {
            case 0:
                err = inflate_stored(&s);
                break;

            case 1:
                err = inflate_fixed(&s);
                break;

            case 2:
                err = inflate_dynamic(&s);
                break;

            default:
                err = 1;
                break;
        }

        if (err != 0) {
            return 1;
        }
    } while (last == 0);

    return s.out_count == out_size ? 0 : 1;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) {
        return a;
    }

    return pb <= pc ? b : c;
}

/* Undoes the per-row filters in place; `row_bytes` excludes the filter byte. */
static int unfilter(uint8_t *data, unsigned rows, size_t row_bytes, unsigned bpp)
{
    for (unsigned y = 0; y < rows; y++) {
        uint8_t *row = data + (size_t)y * (row_bytes + 1);
        uint8_t filter = row[0];
        uint8_t *cur = row + 1;
        const uint8_t *prev = y == 0 ? NULL : row - row_bytes;

        for (size_t x = 0; x < row_bytes; x++) {
            uint8_t a = x >= bpp ? cur[x - bpp] : 0;
            uint8_t b = prev != NULL ? prev[x] : 0;
            uint8_t c = (prev != NULL && x >= bpp) ? prev[x - bpp] : 0;

            switch (filter) {
                case 0:
                    break;

                case 1:
                    cur[x] = (uint8_t)(cur[x] + a);
                    break;

                case 2:
                    cur[x] = (uint8_t)(cur[x] + b);
                    break;

                case 3:
                    cur[x] = (uint8_t)(cur[x] + ((a + b) >> 1));
                    break;

                case 4:
                    cur[x] = (uint8_t)(cur[x] + paeth(a, b, c));
                    break;

                default:
                    return 1;
            }
        }
    }

    return 0;
}

/* The sample at index `i` of a packed row. */
static unsigned sample(const uint8_t *row, unsigned i, unsigned depth)
{
    if (depth == 8) {
        return row[i];
    }

    if (depth == 16) {
        return row[i * 2];
    }

    unsigned per_byte = 8 / depth;
    unsigned shift = 8 - depth * (1 + i % per_byte);

    return (row[i / per_byte] >> shift) & ((1u << depth) - 1);
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return NULL;
    }

    uint8_t *data = NULL;

    if (fseek(file, 0, SEEK_END) == 0) {
        long length = ftell(file);

        if (length > 0 && length < (1L << 26) && fseek(file, 0, SEEK_SET) == 0) {
            data = malloc((size_t)length);

            if (data != NULL && fread(data, 1, (size_t)length, file) != (size_t)length) {
                free(data);
                data = NULL;
            }

            *size = (size_t)length;
        }
    }

    fclose(file);

    return data;
}

int png_read_rgb(const char *path, uint8_t **rgb, unsigned *width, unsigned *height)
{
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    size_t size = 0;
    uint8_t *file = read_file(path, &size);

    if (file == NULL || size < 8 || memcmp(file, signature, 8) != 0) {
        free(file);
        return 1;
    }

    unsigned w = 0;
    unsigned h = 0;
    unsigned depth = 0;
    unsigned type = 0;
    uint8_t palette[256 * 3] = {0};
    uint8_t *idat = NULL;
    size_t idat_size = 0;
    int ok = 0;

    for (size_t pos = 8; pos + 12 <= size;) {
        uint32_t length = be32(file + pos);
        const uint8_t *tag = file + pos + 4;
        const uint8_t *body = file + pos + 8;

        if (length > size - pos - 12) {
            break;
        }

        if (memcmp(tag, "IHDR", 4) == 0 && length >= 13) {
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            type = body[9];

            if (body[12] != 0 || body[10] != 0 || body[11] != 0) {
                break;
            }
        } else if (memcmp(tag, "PLTE", 4) == 0 && length <= sizeof(palette)) {
            memcpy(palette, body, length);
        } else if (memcmp(tag, "IDAT", 4) == 0) {
            uint8_t *grown = realloc(idat, idat_size + length);

            if (grown == NULL) {
                break;
            }

            idat = grown;
            memcpy(idat + idat_size, body, length);
            idat_size += length;
        } else if (memcmp(tag, "IEND", 4) == 0) {
            ok = 1;
            break;
        }

        pos += 12 + length;
    }

    unsigned channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : 4;
    int valid = ok && w > 0 && h > 0 && w <= 4096 && h <= 4096 && idat_size > 6 &&
                (type == 0 || type == 2 || type == 3 || type == 4 || type == 6) &&
                (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16);

    if (!valid) {
        free(idat);
        free(file);
        return 1;
    }

    unsigned bits_per_pixel = channels * depth;
    size_t row_bytes = ((size_t)w * bits_per_pixel + 7) / 8;
    unsigned bpp = (bits_per_pixel + 7) / 8;
    size_t raw_size = (row_bytes + 1) * h;
    uint8_t *raw = malloc(raw_size);
    uint8_t *pixels = malloc((size_t)w * h * 3);
    int status = 1;

    /* Skip the 2-byte zlib header; the checksum after the stream is unused. */
    if (raw != NULL && pixels != NULL &&
        inflate_stream(idat + 2, idat_size - 2, raw, raw_size) == 0 &&
        unfilter(raw, h, row_bytes, bpp) == 0) {
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *row = raw + (size_t)y * (row_bytes + 1) + 1;

            for (unsigned x = 0; x < w; x++) {
                uint8_t *out = pixels + ((size_t)y * w + x) * 3;
                unsigned r;
                unsigned g;
                unsigned b;

                if (type == 3) {
                    unsigned index = sample(row, x, depth);

                    r = palette[index * 3];
                    g = palette[index * 3 + 1];
                    b = palette[index * 3 + 2];
                } else if (type == 0 || type == 4) {
                    unsigned value = sample(row, x * channels, depth);

                    if (depth < 8) {
                        value = value * 255 / ((1u << depth) - 1);
                    }

                    r = g = b = value;
                } else {
                    r = sample(row, x * channels, depth);
                    g = sample(row, x * channels + 1, depth);
                    b = sample(row, x * channels + 2, depth);
                }

                out[0] = (uint8_t)r;
                out[1] = (uint8_t)g;
                out[2] = (uint8_t)b;
            }
        }

        *rgb = pixels;
        *width = w;
        *height = h;
        pixels = NULL;
        status = 0;
    }

    free(pixels);
    free(raw);
    free(idat);
    free(file);

    return status;
}
