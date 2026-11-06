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
    SPRITE_PALETTE_1 = 0x10,

    /* Where drawing starts, and the dot pixel 0 comes out at (before SCX). */
    DRAW_START_DOT = 80,
    FIRST_PIXEL_DOT = 92
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

static bool window_visible(const Ppu *ppu, uint8_t lcdc)
{
    return (lcdc & PPU_LCDC_WINDOW_ENABLE) != 0 &&
           ppu->ly >= ppu->wy &&
           ppu->wx <= WINDOW_MAX_WX;
}

typedef struct LineSprite {
    uint8_t index;   /* position in OAM, which breaks ties */
    int x;           /* leftmost screen column, may be negative */
    uint8_t tile;
    uint8_t flags;
    unsigned row;    /* row of the sprite on this line, before any flip */
} LineSprite;

/*
 * Picks the first ten sprites in OAM order that cover this line, then
 * orders them by priority: the lower X wins, and OAM order breaks ties.
 */
static unsigned select_sprites(
    const Ppu *ppu,
    uint8_t lcdc,
    LineSprite sprites[SPRITES_PER_LINE]
)
{
    unsigned height = (lcdc & PPU_LCDC_OBJ_TALL) != 0 ? 16 : 8;
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

/*
 * How many dots fetching each sprite stalls the pixels for: 6, and for the
 * first sprite over each 8-pixel column of the picture another
 * max(0, 5 - (X + SCX) mod 8). Sprites at X 168 and beyond are never fetched.
 * Returns the total.
 */
static unsigned sprite_stalls(
    const Ppu *ppu,
    const LineSprite sprites[SPRITES_PER_LINE],
    unsigned count,
    unsigned stalls[SPRITES_PER_LINE]
)
{
    enum {
        SPRITE_DOTS = 6,
        RIGHT_EDGE_X = 168
    };

    unsigned total = 0;
    unsigned previous_column = (unsigned)-1;

    for (unsigned i = 0; i < count; i++) {
        /* LineSprite.x is the screen column, OAM X minus 8. */
        unsigned oam_x = (unsigned)(sprites[i].x + SPRITE_X_OFFSET);

        stalls[i] = 0;

        if (oam_x >= RIGHT_EDGE_X) {
            continue;
        }

        unsigned offset = oam_x + ppu->scx;
        unsigned column = offset >> 3;

        stalls[i] = SPRITE_DOTS;

        /* The first sprite over a background tile waits for that tile's
         * fetch to line up with it. */
        if (column != previous_column) {
            unsigned alignment = offset & 7;

            stalls[i] += alignment < 5 ? 5 - alignment : 0;
            previous_column = column;
        }

        total += stalls[i];
    }

    return total;
}

/*
 * The background fetcher and pixel FIFO.
 *
 * Drawing starts at dot 80. The fetcher reads a background tile in three
 * steps of 2 dots: the tile number, then the low and the high byte of the
 * tile's row. It can push the 8 pixels into the FIFO 2 dots after that, when
 * the FIFO is empty. The first fetch of the line is thrown away and repeated,
 * so tile 0 is pushed at dot 92; every later tile is fetched while the
 * previous one is shifted out, and pushed in the dot after the last of its 8
 * pixels. The first SCX mod 8 pixels of tile 0 are shifted out without being
 * drawn. A sprite fetch freezes the fetcher and the shifter for its stall.
 *
 * Each step reads the registers as they are at its own dot: the map select
 * (LCDC bit 3), SCX and SCY for the tile number, the tile data select
 * (LCDC bit 4) and SCY for each byte.
 */
enum {
    FETCH_DOTS = 6,
    FIFO_PIXELS = 8,
    FETCH_NUMBER_AGE = 0,
    FETCH_LOW_AGE = 2,
    FETCH_HIGH_AGE = 4,

    /*
     * LCDC bit 3 (map select) is read this many dots after the tile number's
     * step, and bit 4 (tile data select) after each byte's. Fitted to the
     * mealybug m3_lcdc_bg_map_change and m3_lcdc_tile_sel_change pictures.
     */
    MAP_SELECT_DELAY = 1,
    TILE_SELECT_DELAY = 1,

    /*
     * Starting the window restarts the fetcher: its first pixel and all the
     * ones after come out this many dots later. The window's tile number and
     * bytes are read this many dots before the tile's first pixel. Fitted to
     * the mealybug m3_window_timing and m3_lcdc_*_win_change pictures.
     */
    /*
     * A sprite is fetched 6 dots before its first pixel (the 6 dots of its
     * stall). The OBJ layer must be on 6 dots after the pixel that reaches the
     * sprite for the fetch to happen, and the sprite size (LCDC bit 2) is read
     * 3 dots after the fetch begins. Fitted to the mealybug
     * m3_lcdc_obj_en_change* and m3_lcdc_obj_size_change* pictures.
     */
    SPRITE_FETCH_LEAD = 6,
    OBJ_ENABLE_DELAY = 6,
    SPRITE_SIZE_DELAY = 3,

    WINDOW_START_DOTS = 6,
    WINDOW_NUMBER_LEAD = 6,
    WINDOW_LOW_LEAD = 6,
    WINDOW_HIGH_LEAD = 4
};

typedef struct Fetcher {
    unsigned tile;      /* tile column being fetched, 0 at the line's start */
    unsigned age;       /* non-stalled dots since this fetch began */
    uint8_t number;     /* the tile number, once read */
    uint8_t low;
    uint8_t high;
} Fetcher;

/* VRAM offset of one row's byte of a background tile, for the given LCDC. */
static size_t background_row_address(
    uint8_t lcdc,
    uint8_t number,
    unsigned row,
    unsigned byte
)
{
    size_t tile =
        (lcdc & PPU_LCDC_TILE_DATA_UNSIGNED) != 0
            ? (size_t)number * TILE_BYTES
            : (size_t)(SIGNED_TILE_BASE + (int)(int8_t)number * TILE_BYTES);

    return tile + row * 2 + byte;
}

/* Does the step of the fetch in progress that falls on this dot. */
static void fetch_step(const Ppu *ppu, Fetcher *fetcher, unsigned dot)
{
    unsigned select_delay =
        fetcher->age == FETCH_NUMBER_AGE ? MAP_SELECT_DELAY : TILE_SELECT_DELAY;
    uint8_t lcdc = ppu_register_at(ppu, PPU_LOGGED_LCDC, dot + select_delay);
    unsigned y = (unsigned)(ppu->ly +
                            ppu_register_at(ppu, PPU_LOGGED_SCY, dot)) & 0xFF;

    if (fetcher->age == FETCH_NUMBER_AGE) {
        size_t map =
            (lcdc & PPU_LCDC_BG_MAP_HIGH) != 0
                ? TILEMAP_HIGH_OFFSET
                : TILEMAP_LOW_OFFSET;
        unsigned scx = ppu_register_at(ppu, PPU_LOGGED_SCX, dot);
        unsigned column = ((scx >> 3) + fetcher->tile) & (TILEMAP_COLUMNS - 1);

        fetcher->number = ppu->vram[map + (y / 8) * TILEMAP_COLUMNS + column];
    } else if (fetcher->age == FETCH_LOW_AGE) {
        fetcher->low = ppu->vram[
            background_row_address(lcdc, fetcher->number, y % 8, 0)
        ];
    } else if (fetcher->age == FETCH_HIGH_AGE) {
        fetcher->high = ppu->vram[
            background_row_address(lcdc, fetcher->number, y % 8, 1)
        ];
    }

    fetcher->age++;
}

/*
 * Reads one row of window tile `tile`, whose first pixel comes out at `dot`:
 * the number, the low byte and the high byte at their leads before it, each
 * with the registers as they are then.
 */
static void fetch_window_row(
    const Ppu *ppu,
    unsigned tile,
    unsigned dot,
    uint8_t *low,
    uint8_t *high
)
{
    unsigned row = ppu->window_line;
    uint8_t number_lcdc = ppu_register_at(
        ppu, PPU_LOGGED_LCDC, dot - WINDOW_NUMBER_LEAD + MAP_SELECT_DELAY);
    size_t map =
        (number_lcdc & PPU_LCDC_WINDOW_MAP_HIGH) != 0
            ? TILEMAP_HIGH_OFFSET
            : TILEMAP_LOW_OFFSET;
    uint8_t number =
        ppu->vram[map + (row / 8) * TILEMAP_COLUMNS + (tile & (TILEMAP_COLUMNS - 1))];

    *low = ppu->vram[background_row_address(
        ppu_register_at(ppu, PPU_LOGGED_LCDC, dot - WINDOW_LOW_LEAD + TILE_SELECT_DELAY),
        number, row % 8, 0)];
    *high = ppu->vram[background_row_address(
        ppu_register_at(ppu, PPU_LOGGED_LCDC, dot - WINDOW_HIGH_LEAD + TILE_SELECT_DELAY),
        number, row % 8, 1)];
}

/*
 * Runs the fetcher and the FIFO over the line. Fills `colors` with the
 * background (or window) color number of each pixel and `dots` with the dot
 * each one comes out at. Returns whether the window was drawn.
 */
static bool render_background(
    const Ppu *ppu,
    uint8_t lcdc,
    const LineSprite sprites[SPRITES_PER_LINE],
    const unsigned stalls[SPRITES_PER_LINE],
    unsigned sprite_count,
    bool fetched[SPRITES_PER_LINE],
    uint8_t colors[PPU_SCREEN_WIDTH],
    unsigned dots[PPU_SCREEN_WIDTH]
)
{
    uint8_t fifo[FIFO_PIXELS] = {0};
    unsigned head = 0;
    unsigned size = 0;
    Fetcher fetcher = {0};
    unsigned discard =
        ppu_register_at(ppu, PPU_LOGGED_SCX, DRAW_START_DOT) & 7;
    unsigned next_sprite = 0;
    unsigned stall = 0;
    unsigned x = 0;
    bool bg_on = (lcdc & PPU_LCDC_BG_ENABLE) != 0;
    bool window_started = false;
    bool window_drawn = false;
    int window_start = 0;
    unsigned window_tile = (unsigned)-1;
    uint8_t window_low = 0;
    uint8_t window_high = 0;

    for (unsigned dot = DRAW_START_DOT + FETCH_DOTS;; dot++) {
        if (stall > 0) {
            stall--;
            continue;
        }

        if (size == 0 && fetcher.age >= FETCH_DOTS) {
            for (unsigned i = 0; i < FIFO_PIXELS; i++) {
                unsigned bit = 7 - i;

                fifo[i] = (uint8_t)((((fetcher.high >> bit) & 1) << 1) |
                                    ((fetcher.low >> bit) & 1));
            }

            head = 0;
            size = FIFO_PIXELS;
            fetcher.tile++;
            fetcher.age = 0;
        }

        fetch_step(ppu, &fetcher, dot);

        if (size == 0) {
            continue;   /* the first tile is not ready yet */
        }

        if (discard > 0) {
            head++;
            size--;
            discard--;
            continue;
        }

        while (next_sprite < sprite_count &&
               sprites[next_sprite].x <= (int)x) {
            /* A sprite is fetched, and stalls the line, only if the OBJ
             * layer is on when its turn comes. */
            fetched[next_sprite] =
                (ppu_register_at(ppu, PPU_LOGGED_LCDC,
                                 dot + OBJ_ENABLE_DELAY) &
                 PPU_LCDC_OBJ_ENABLE) != 0;

            if (fetched[next_sprite]) {
                stall += stalls[next_sprite];
            }

            next_sprite++;
        }

        if (stall > 0) {
            stall--;
            continue;
        }

        dots[x] = dot;
        colors[x] = fifo[head++];
        size--;

        if (bg_on && ppu->ly >= ppu->wy) {
            uint8_t at_lcdc = ppu_register_at(ppu, PPU_LOGGED_LCDC, dot);

            /*
             * The window starts when WX equals the pixel counter plus 7, the
             * counter being 0 at the first pixel. WX below 7 matches in the
             * dots before it, so the comparison runs over those too.
             */
            unsigned first = x == 0 ? WINDOW_X_OFFSET : 0;

            for (unsigned back = first + 1; back-- > 0 && !window_started;) {
                unsigned at = dot - back;
                unsigned wx = ppu_register_at(ppu, PPU_LOGGED_WX, at);

                if ((ppu_register_at(ppu, PPU_LOGGED_LCDC, at) &
                     PPU_LCDC_WINDOW_ENABLE) != 0 &&
                    wx == x + WINDOW_X_OFFSET - back) {
                    window_started = true;
                    window_start = (int)wx - WINDOW_X_OFFSET;
                    dots[x] = dot + WINDOW_START_DOTS;
                    stall = WINDOW_START_DOTS;
                }
            }

            if (window_started && (at_lcdc & PPU_LCDC_WINDOW_ENABLE) != 0) {
                unsigned column = (unsigned)((int)x - window_start);

                if (column / 8 != window_tile) {
                    window_tile = column / 8;
                    fetch_window_row(ppu, window_tile, dots[x], &window_low,
                                     &window_high);
                }

                unsigned bit = 7 - column % 8;

                colors[x] = (uint8_t)((((window_high >> bit) & 1) << 1) |
                                      ((window_low >> bit) & 1));
                window_drawn = true;
            }
        }

        if (++x == PPU_SCREEN_WIDTH) {
            break;
        }
    }

    if ((lcdc & PPU_LCDC_BG_ENABLE) == 0) {
        /* With the background off, it and the window are blank. */
        for (x = 0; x < PPU_SCREEN_WIDTH; x++) {
            colors[x] = 0;
        }

        return false;
    }

    return window_drawn;
}

static void render_sprites(
    Ppu *ppu,
    const LineSprite sprites[SPRITES_PER_LINE],
    const bool fetched[SPRITES_PER_LINE],
    unsigned count,
    const unsigned dots[PPU_SCREEN_WIDTH],
    const uint8_t background_colors[PPU_SCREEN_WIDTH],
    uint8_t *line
)
{
    bool claimed[PPU_SCREEN_WIDTH] = {false};

    for (unsigned i = 0; i < count; i++) {
        const LineSprite *sprite = &sprites[i];

        if (!fetched[i]) {
            continue;
        }

        /*
         * The sprite's tile and row are worked out when it is fetched, with
         * the sprite size (LCDC bit 2) as it is then.
         */
        unsigned fetch_dot = dots[sprite->x < 0 ? 0 : (unsigned)sprite->x] -
                             SPRITE_FETCH_LEAD;
        unsigned height =
            (ppu_register_at(ppu, PPU_LOGGED_LCDC,
                             fetch_dot + SPRITE_SIZE_DELAY) &
             PPU_LCDC_OBJ_TALL) != 0 ? 16 : 8;
        unsigned row = sprite->row;
        uint8_t tile_number = sprite->tile;

        if ((sprite->flags & SPRITE_Y_FLIP) != 0) {
            row = (height - 1 - row) & (height - 1);
        }

        row &= height - 1;

        if (height == 16) {
            /* A tall sprite is two consecutive tiles; bit 0 is ignored. */
            tile_number = (uint8_t)((tile_number & 0xFE) | (row >= 8 ? 1 : 0));
        }

        const uint8_t *tile = &ppu->vram[(size_t)tile_number * TILE_BYTES];

        PpuLoggedRegister palette_register =
            (sprite->flags & SPRITE_PALETTE_1) != 0
                ? PPU_LOGGED_OBP1
                : PPU_LOGGED_OBP0;

        for (unsigned column = 0; column < 8; column++) {
            int x = sprite->x + (int)column;

            if (x < 0 || x >= PPU_SCREEN_WIDTH || claimed[x]) {
                continue;
            }

            /* The OBJ layer and the palette are those at the pixel's dot. */
            unsigned dot = dots[x];

            if ((ppu_register_at(ppu, PPU_LOGGED_LCDC, dot) &
                 PPU_LCDC_OBJ_ENABLE) == 0) {
                continue;
            }

            unsigned tile_column =
                (sprite->flags & SPRITE_X_FLIP) != 0 ? 7 - column : column;
            uint8_t color = tile_pixel(tile, row % 8, tile_column);

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

            line[x] = palette_shade(
                ppu_register_at(ppu, palette_register, dot),
                color
            );
        }
    }
}

void ppu_render_scanline(Ppu *ppu)
{
    if (ppu->ly >= PPU_SCREEN_HEIGHT) {
        return;
    }

    /*
     * The line's sprites are picked at the start of drawing; the background
     * and window tiles are read as the fetcher gets to them, and the BG and
     * OBJ enable bits, the palettes and what starts the window (WX, the
     * window enable bit) are those at each pixel's own dot.
     */
    uint8_t lcdc = ppu_register_at(ppu, PPU_LOGGED_LCDC, DRAW_START_DOT);
    uint8_t background_colors[PPU_SCREEN_WIDTH];
    uint8_t *line = &ppu->framebuffer[ppu->ly * PPU_SCREEN_WIDTH];

    /* The line's sprites are picked at the start of drawing; each is fetched,
     * and stalls the pixels, only if the OBJ layer is on when its turn comes. */
    LineSprite sprites[SPRITES_PER_LINE] = {{0}};
    unsigned stalls[SPRITES_PER_LINE] = {0};
    bool fetched[SPRITES_PER_LINE] = {false};
    unsigned count = select_sprites(ppu, lcdc, sprites);
    unsigned dots[PPU_SCREEN_WIDTH];

    sprite_stalls(ppu, sprites, count, stalls);

    bool window_drawn = render_background(
        ppu, lcdc, sprites, stalls, count, fetched, background_colors, dots
    );

    for (unsigned x = 0; x < PPU_SCREEN_WIDTH; x++) {
        unsigned dot = dots[x];

        line[x] = (ppu_register_at(ppu, PPU_LOGGED_LCDC, dot) &
                   PPU_LCDC_BG_ENABLE) != 0
            ? palette_shade(
                ppu_register_at(ppu, PPU_LOGGED_BGP, dot),
                background_colors[x]
            )
            : 0;
    }

    render_sprites(ppu, sprites, fetched, count, dots, background_colors, line);

    if (window_drawn) {
        ppu->window_line++;
    }
}

unsigned ppu_drawing_dots(const Ppu *ppu)
{
    enum {
        BASE_DOTS = 172,
        WINDOW_DOTS = 3,
        SPRITE_OVERLAP = 3
    };

    unsigned dots = BASE_DOTS + (ppu->scx & 7);

    /*
     * Starting the window restarts the fetcher. The gbmicrotest win ROMs put
     * the end of mode 3 2 to 4 dots later for every WX from 0 to 15; 3 is the
     * same 6 minus 3 a sprite costs.
     */
    if ((ppu->lcdc & PPU_LCDC_BG_ENABLE) != 0 &&
        window_visible(ppu, ppu->lcdc)) {
        dots += WINDOW_DOTS;
    }

    if ((ppu->lcdc & PPU_LCDC_OBJ_ENABLE) == 0) {
        return dots;
    }

    LineSprite sprites[SPRITES_PER_LINE];
    unsigned stalls[SPRITES_PER_LINE];
    unsigned count = select_sprites(ppu, ppu->lcdc, sprites);
    unsigned penalty = sprite_stalls(ppu, sprites, count, stalls);

    /*
     * Turning the OBJ layer on shortens mode 3 by 3 dots even with no sprite
     * on the line (the gbmicrotest ppu_sprite0 ROMs); each sprite then adds
     * its penalty.
     */
    return dots + penalty - SPRITE_OVERLAP;
}
