#ifndef PNG_READ_H
#define PNG_READ_H

#include <stddef.h>
#include <stdint.h>

/*
 * A small PNG reader for the test ROM screenshots: non-interlaced images,
 * every colour type, bit depths 1 to 8 (16 keeps the high byte).
 *
 * On success returns 0 and gives `*rgb`, a malloc'ed buffer of
 * width * height * 3 bytes, to be freed by the caller. Returns non-zero for
 * a file that cannot be read or is not a PNG this reader supports.
 */
int png_read_rgb(
    const char *path,
    uint8_t **rgb,
    unsigned *width,
    unsigned *height
);

#endif
