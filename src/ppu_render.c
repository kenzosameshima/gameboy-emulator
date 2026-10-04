#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ppu.h>

#include "ppu_internal.h"

/*
 * Scanline renderer.
 *
 * Tiles are 8x8 pixels of 2 bits, stored as 16 bytes: each row is a low
 * bitplane byte followed by a high bitplane byte, with the leftmost pixel
 * in bit 7. A pixel's color number (0-3) goes through a palette register
 * to become a shade (0 lightest to 3 darkest).
 */

enum {
    TILE_BYTES = 16,
    TILEMAP_COLUMNS = 32,
    TILEMAP_LOW_OFFSET = 0x1800,   /* 9800 */
    TILEMAP_HIGH_OFFSET = 0x1C00,  /* 9C00 */
    SIGNED_TILE_BASE = 0x1000,     /* 9000, where signed tile 0 lives */

    SPRITE_BYTES = 4,
    SPRITE_COUNT = 40,
    SPRITES_PER_LINE = 10,
    SPRITE_Y_OFFSET = 16,
    SPRITE_X_OFFSET = 8,
    WINDOW_X_OFFSET = 7,
    WINDOW_MAX_WX = 166,

    SPRITE_BEHIND_BG = 0x80,
    SPRITE_Y_FLIP = 0x40,
    SPRITE_X_FLIP = 0x20,
    SPRITE_PALETTE_1 = 0x10
};

static uint8_t tile_pixel(const uint8_t *tile, unsigned row, unsigned column)
{
    unsigned bit = 7 - column;
    unsigned low = (tile[row * 2] >> bit) & 1;
    unsigned high = (tile[row * 2 + 1] >> bit) & 1;

    return (uint8_t)((high << 1) | low);
}

static uint8_t palette_shade(uint8_t palette, uint8_t color)
{
    return (uint8_t)((palette >> (color * 2)) & 0x03);
}

/* Tile data for a background or window tile number. */
static const uint8_t *background_tile(const Ppu *ppu, uint8_t number)
{
    if ((ppu->lcdc & PPU_LCDC_TILE_DATA_UNSIGNED) != 0) {
        return &ppu->vram[(size_t)number * TILE_BYTES];
    }

    return &ppu->vram[SIGNED_TILE_BASE + (int)(int8_t)number * TILE_BYTES];
}

static uint8_t tilemap_pixel(
    const Ppu *ppu,
    size_t map_offset,
    unsigned x,
    unsigned y
)
{
    uint8_t number = ppu->vram[
        map_offset + (y / 8) * TILEMAP_COLUMNS + (x / 8)
    ];

    return tile_pixel(background_tile(ppu, number), y % 8, x % 8);
}

static bool window_visible(const Ppu *ppu)
{
    return (ppu->lcdc & PPU_LCDC_WINDOW_ENABLE) != 0 &&
           ppu->ly >= ppu->wy &&
           ppu->wx <= WINDOW_MAX_WX;
}

/*
 * Fills `colors` with the background and window color number of each
 * pixel on the line, and returns whether the window was drawn.
 */
static bool render_background(
    const Ppu *ppu,
    uint8_t colors[PPU_SCREEN_WIDTH]
)
{
    if ((ppu->lcdc & PPU_LCDC_BG_ENABLE) == 0) {
        /* With the background off, it and the window are blank. */
        for (unsigned x = 0; x < PPU_SCREEN_WIDTH; x++) {
            colors[x] = 0;
        }

        return false;
    }

    size_t background_map =
        (ppu->lcdc & PPU_LCDC_BG_MAP_HIGH) != 0
            ? TILEMAP_HIGH_OFFSET
            : TILEMAP_LOW_OFFSET;
    size_t window_map =
        (ppu->lcdc & PPU_LCDC_WINDOW_MAP_HIGH) != 0
            ? TILEMAP_HIGH_OFFSET
            : TILEMAP_LOW_OFFSET;
    bool window = window_visible(ppu);
    int window_start = (int)ppu->wx - WINDOW_X_OFFSET;
    unsigned background_y = (unsigned)(ppu->ly + ppu->scy) & 0xFF;

    for (unsigned x = 0; x < PPU_SCREEN_WIDTH; x++) {
        if (window && (int)x >= window_start) {
            colors[x] = tilemap_pixel(
                ppu,
                window_map,
                (unsigned)((int)x - window_start),
                ppu->window_line
            );
        } else {
            colors[x] = tilemap_pixel(
                ppu,
                background_map,
                (x + ppu->scx) & 0xFF,
                background_y
            );
        }
    }

    return window;
}

typedef struct LineSprite {
    uint8_t index;   /* position in OAM, which breaks ties */
    int x;           /* leftmost screen column, may be negative */
    uint8_t tile;
    uint8_t flags;
    unsigned row;    /* row of the sprite on this line, after any flip */
} LineSprite;

/*
 * Picks the first ten sprites in OAM order that cover this line, then
 * orders them by priority: the lower X wins, and OAM order breaks ties.
 */
static unsigned select_sprites(
    const Ppu *ppu,
    LineSprite sprites[SPRITES_PER_LINE]
)
{
    unsigned height = (ppu->lcdc & PPU_LCDC_OBJ_TALL) != 0 ? 16 : 8;
    unsigned count = 0;

    for (unsigned i = 0; i < SPRITE_COUNT && count < SPRITES_PER_LINE; i++) {
        const uint8_t *entry = &ppu->oam[i * SPRITE_BYTES];
        int top = (int)entry[0] - SPRITE_Y_OFFSET;
        int row = (int)ppu->ly - top;

        if (row < 0 || row >= (int)height) {
            continue;
        }

        LineSprite sprite = {
            .index = (uint8_t)i,
            .x = (int)entry[1] - SPRITE_X_OFFSET,
            .tile = entry[2],
            .flags = entry[3],
            .row = (unsigned)row
        };

        if ((sprite.flags & SPRITE_Y_FLIP) != 0) {
            sprite.row = height - 1 - sprite.row;
        }

        if (height == 16) {
            /* A tall sprite is two consecutive tiles; bit 0 is ignored. */
            sprite.tile = (uint8_t)((sprite.tile & 0xFE) |
                                    (sprite.row >= 8 ? 1 : 0));
        }

        /* Insertion sort keeps equal X in OAM order. */
        unsigned position = count;

        while (position > 0 && sprites[position - 1].x > sprite.x) {
            sprites[position] = sprites[position - 1];
            position--;
        }

        sprites[position] = sprite;
        count++;
    }

    return count;
}

static void render_sprites(
    Ppu *ppu,
    const uint8_t background_colors[PPU_SCREEN_WIDTH],
    uint8_t *line
)
{
    LineSprite sprites[SPRITES_PER_LINE];
    unsigned count = select_sprites(ppu, sprites);
    bool claimed[PPU_SCREEN_WIDTH] = {false};

    for (unsigned i = 0; i < count; i++) {
        const LineSprite *sprite = &sprites[i];
        const uint8_t *tile = &ppu->vram[(size_t)sprite->tile * TILE_BYTES];
        uint8_t palette = (sprite->flags & SPRITE_PALETTE_1) != 0
            ? ppu->obp1
            : ppu->obp0;

        for (unsigned column = 0; column < 8; column++) {
            int x = sprite->x + (int)column;

            if (x < 0 || x >= PPU_SCREEN_WIDTH || claimed[x]) {
                continue;
            }

            unsigned tile_column =
                (sprite->flags & SPRITE_X_FLIP) != 0 ? 7 - column : column;
            uint8_t color = tile_pixel(tile, sprite->row % 8, tile_column);

            if (color == 0) {
                continue;
            }

            /* The highest-priority opaque sprite pixel decides, even if
             * the background hides it. */
            claimed[x] = true;

            if ((sprite->flags & SPRITE_BEHIND_BG) != 0 &&
                background_colors[x] != 0) {
                continue;
            }

            line[x] = palette_shade(palette, color);
        }
    }
}

void ppu_render_scanline(Ppu *ppu)
{
    if (ppu->ly >= PPU_SCREEN_HEIGHT) {
        return;
    }

    uint8_t background_colors[PPU_SCREEN_WIDTH];
    uint8_t *line = &ppu->framebuffer[ppu->ly * PPU_SCREEN_WIDTH];
    bool window_drawn = render_background(ppu, background_colors);

    for (unsigned x = 0; x < PPU_SCREEN_WIDTH; x++) {
        line[x] = (ppu->lcdc & PPU_LCDC_BG_ENABLE) != 0
            ? palette_shade(ppu->bgp, background_colors[x])
            : 0;
    }

    if ((ppu->lcdc & PPU_LCDC_OBJ_ENABLE) != 0) {
        render_sprites(ppu, background_colors, line);
    }

    if (window_drawn) {
        ppu->window_line++;
    }
}
