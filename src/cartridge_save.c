#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cartridge.h>

#include "mapper.h"

/*
 * Battery saves: the cartridge RAM as a raw dump, then whatever extra state
 * the mapper keeps alive (the MBC3 clock). See cartridge.h for the format.
 */

static size_t extra_size(const Cartridge *cartridge)
{
    const MapperOps *ops = cartridge_mapper_ops(cartridge->mapper);

    return ops->save_extra_size != NULL ? ops->save_extra_size(cartridge) : 0;
}

/* Writes a whole file, or leaves the old one alone. */
static bool write_file_atomically(
    const char *path,
    const uint8_t *data,
    size_t size
)
{
    size_t name_length = strlen(path);
    char *temporary = malloc(name_length + sizeof(".tmp"));

    if (temporary == NULL) {
        return false;
    }

    memcpy(temporary, path, name_length);
    memcpy(temporary + name_length, ".tmp", sizeof(".tmp"));

    FILE *file = fopen(temporary, "wb");
    bool ok = file != NULL;

    if (file != NULL) {
        ok = (size == 0 || fwrite(data, 1, size, file) == size);
        ok = (fclose(file) == 0) && ok;
    }

#ifdef _WIN32
    /* rename() does not replace an existing file on Windows. */
    if (ok) {
        remove(path);
    }
#endif

    ok = ok && rename(temporary, path) == 0;

    if (!ok) {
        remove(temporary);
    }

    free(temporary);

    return ok;
}

CartridgeSaveStatus cartridge_save_battery(
    const Cartridge *cartridge,
    const char *path,
    uint64_t unix_time
)
{
    if (!cartridge->has_battery) {
        return CARTRIDGE_SAVE_NOT_BATTERY_BACKED;
    }

    size_t extra = extra_size(cartridge);
    size_t total = cartridge->ram_size + extra;
    uint8_t *data = malloc(total == 0 ? 1 : total);

    if (data == NULL) {
        return CARTRIDGE_SAVE_IO_ERROR;
    }

    if (cartridge->ram_size != 0) {
        memcpy(data, cartridge->ram, cartridge->ram_size);
    }

    if (extra != 0) {
        cartridge_mapper_ops(cartridge->mapper)->save_extra(
            cartridge,
            data + cartridge->ram_size,
            unix_time
        );
    }

    bool ok = write_file_atomically(path, data, total);

    free(data);

    return ok ? CARTRIDGE_SAVE_OK : CARTRIDGE_SAVE_IO_ERROR;
}

CartridgeSaveStatus cartridge_load_battery(
    Cartridge *cartridge,
    const char *path,
    uint64_t unix_time
)
{
    if (!cartridge->has_battery) {
        return CARTRIDGE_SAVE_NOT_BATTERY_BACKED;
    }

    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return errno == ENOENT ? CARTRIDGE_SAVE_NO_FILE
                               : CARTRIDGE_SAVE_IO_ERROR;
    }

    size_t extra = extra_size(cartridge);
    size_t full = cartridge->ram_size + extra;
    uint8_t *data = malloc(full == 0 ? 1 : full + 1);

    if (data == NULL) {
        fclose(file);
        return CARTRIDGE_SAVE_IO_ERROR;
    }

    /* One byte more than the largest valid file tells a too-big one. */
    size_t read = fread(data, 1, full + 1, file);
    bool read_error = ferror(file) != 0;

    fclose(file);

    CartridgeSaveStatus status = CARTRIDGE_SAVE_OK;

    if (read_error) {
        status = CARTRIDGE_SAVE_IO_ERROR;
    } else if (read != cartridge->ram_size && read != full) {
        /* Either the RAM alone or the RAM and the extra state. */
        status = CARTRIDGE_SAVE_BAD_SIZE;
    } else {
        if (cartridge->ram_size != 0) {
            memcpy(cartridge->ram, data, cartridge->ram_size);
        }

        if (read == full && extra != 0) {
            cartridge_mapper_ops(cartridge->mapper)->load_extra(
                cartridge,
                data + cartridge->ram_size,
                unix_time
            );
        }
    }

    free(data);

    return status;
}
