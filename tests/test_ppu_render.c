/*
 * Scanline rendering through the ppu.h interface: background, scrolling,
 * tile data and map selection, window, palettes and sprites.
 *
 * Expected pixels are worked out by hand from the tile bytes below, not
 * computed with the renderer's own steps.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <interrupts.h>
#include <ppu.h>

enum {
    FRAME_DOTS = 456 * 154,

    /* VRAM offsets (address - 0x8000). */
    MAP_LOW = 0x1800,   /* 9800 */
    MAP_HIGH = 0x1C00,  /* 9C00 */

    /* LCDC building blocks. */
    LCD_ON = PPU_LCDC_LCD_ENABLE,
    BG_ON = PPU_LCDC_BG_ENABLE,
    OBJ_ON = PPU_LCDC_OBJ_ENABLE,
    OBJ_TALL = PPU_LCDC_OBJ_TALL,
    BG_MAP_HIGH = PPU_LCDC_BG_MAP_HIGH,
    TILES_UNSIGNED = PPU_LCDC_TILE_DATA_UNSIGNED,
    WINDOW_ON = PPU_LCDC_WINDOW_ENABLE,
    WINDOW_MAP_HIGH = PPU_LCDC_WINDOW_MAP_HIGH,

    IDENTITY = 0xE4
};

/*
 * Tile A, one row per two bytes (low plane, high plane). Colors per row:
 *   0: 0 2 3 3 3 3 2 0     4: 1 0 1 0 1 0 1 0
 *   1: 1 1 1 1 1 1 1 1     5: 2 0 2 0 2 0 2 0
 *   2: 2 2 2 2 2 2 2 2     6: 1 0 0 0 0 0 0 2
 *   3: 3 3 3 3 3 3 3 3     7: 0 0 0 0 0 0 0 0
 */
static const uint8_t TILE_A[16] = {
    0x3C, 0x7E, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xAA, 0x00, 0x00, 0xAA, 0x80, 0x01, 0x00, 0x00
};

static const uint8_t TILE_A_COLORS[8][8] = {
    { 0, 2, 3, 3, 3, 3, 2, 0 },
    { 1, 1, 1, 1, 1, 1, 1, 1 },
    { 2, 2, 2, 2, 2, 2, 2, 2 },
    { 3, 3, 3, 3, 3, 3, 3, 3 },
    { 1, 0, 1, 0, 1, 0, 1, 0 },
    { 2, 0, 2, 0, 2, 0, 2, 0 },
    { 1, 0, 0, 0, 0, 0, 0, 2 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }
};

static uint8_t solid_tile(uint8_t color, uint8_t out[16])
{
    for (unsigned row = 0; row < 8; row++) {
        out[row * 2] = (color & 1) != 0 ? 0xFF : 0x00;
        out[row * 2 + 1] = (color & 2) != 0 ? 0xFF : 0x00;
    }

    return color;
}

static Ppu ppu;
static InterruptRegisters interrupts;

/* A blank machine with the LCD off, so VRAM and OAM can be filled freely. */
static void begin(void)
{
    interrupts_init(&interrupts);
    ppu_init(&ppu, &interrupts);
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x00);
    ppu_write(&ppu, PPU_BGP_ADDRESS, IDENTITY);
    ppu_write(&ppu, PPU_OBP0_ADDRESS, IDENTITY);
    ppu_write(&ppu, PPU_OBP1_ADDRESS, IDENTITY);
}

static void put_tile(unsigned number, const uint8_t bytes[16])
{
    memcpy(&ppu.vram[number * 16], bytes, 16);
}

static void put_tile_at(size_t vram_offset, const uint8_t bytes[16])
{
    memcpy(&ppu.vram[vram_offset], bytes, 16);
}

static void put_solid_tile(unsigned number, uint8_t color)
{
    uint8_t bytes[16];

    solid_tile(color, bytes);
    put_tile(number, bytes);
}

static void put_sprite(
    unsigned index,
    uint8_t y,
    uint8_t x,
    uint8_t tile,
    uint8_t flags
)
{
    uint8_t entry[4] = { y, x, tile, flags };

    for (unsigned i = 0; i < 4; i++) {
        ppu_write(&ppu, (uint16_t)(PPU_OAM_START + index * 4 + i), entry[i]);
    }
}

/* Turns the LCD on with `lcdc` and runs one whole frame. */
static void draw(uint8_t lcdc)
{
    ppu_write(&ppu, PPU_LCDC_ADDRESS, (uint8_t)(lcdc | LCD_ON));

    for (unsigned dots = 0; dots < FRAME_DOTS; dots += 4) {
        ppu_step(&ppu, 4);
    }
}

static uint8_t pixel(unsigned x, unsigned y)
{
    return ppu.framebuffer[y * PPU_SCREEN_WIDTH + x];
}

/* Asserts that the 8 pixels from (x, y) are the given colors under the
 * identity palette. */
static void expect_row(unsigned x, unsigned y, const uint8_t colors[8])
{
    for (unsigned i = 0; i < 8; i++) {
        if (pixel(x + i, y) != colors[i]) {
            fprintf(stderr, "pixel (%u,%u): got %u, want %u\n", x + i, y,
                    pixel(x + i, y), colors[i]);
        }

        assert(pixel(x + i, y) == colors[i]);
    }
}

static void expect_tile(unsigned x, unsigned y)
{
    for (unsigned row = 0; row < 8; row++) {
        expect_row(x, y + row, TILE_A_COLORS[row]);
    }
}

static void expect_blank(unsigned x, unsigned y, unsigned width,
                         unsigned height, uint8_t shade)
{
    for (unsigned dy = 0; dy < height; dy++) {
        for (unsigned dx = 0; dx < width; dx++) {
            assert(pixel(x + dx, y + dy) == shade);
        }
    }
}

static void test_background_tile(void)
{
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW + 0] = 1;
    ppu.vram[MAP_LOW + 1] = 1;
    ppu.vram[MAP_LOW + 32] = 1;   /* the tile below the first one */

    draw(BG_ON | TILES_UNSIGNED);

    expect_tile(0, 0);
    expect_tile(8, 0);
    expect_tile(0, 8);

    /* Everything else is tile 0, which is all color 0. */
    expect_blank(16, 0, 144, 8, 0);
    expect_blank(8, 8, 152, 8, 0);
    expect_blank(0, 16, 160, 128, 0);
    assert(ppu.frames == 1);
}

static void test_background_palette(void)
{
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;

    /* 0x1B maps colors 0 1 2 3 to shades 3 2 1 0. */
    ppu_write(&ppu, PPU_BGP_ADDRESS, 0x1B);
    draw(BG_ON | TILES_UNSIGNED);

    static const uint8_t row0[8] = { 3, 1, 0, 0, 0, 0, 1, 3 };

    expect_row(0, 0, row0);
    /* The empty tiles around it are color 0, which is now shade 3. */
    assert(pixel(8, 0) == 3);
    assert(pixel(0, 100) == 3);

    /* A palette that sends every color to shade 2. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;
    ppu_write(&ppu, PPU_BGP_ADDRESS, 0xAA);
    draw(BG_ON | TILES_UNSIGNED);
    expect_blank(0, 0, 8, 8, 2);
}

static void test_scrolling(void)
{
    /* SCX moves the picture left; tile at map column 1 slides to x = 0. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW + 1] = 1;
    ppu_write(&ppu, PPU_SCX_ADDRESS, 8);
    draw(BG_ON | TILES_UNSIGNED);
    expect_tile(0, 0);
    expect_blank(8, 0, 152, 8, 0);

    /* A scroll that is not a multiple of 8 shifts it by that many pixels. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW + 1] = 1;
    ppu_write(&ppu, PPU_SCX_ADDRESS, 3);
    draw(BG_ON | TILES_UNSIGNED);
    expect_tile(5, 0);
    expect_blank(0, 0, 5, 8, 0);

    /* SCY moves it up: screen line 0 shows tile row 2. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;
    ppu_write(&ppu, PPU_SCY_ADDRESS, 2);
    draw(BG_ON | TILES_UNSIGNED);
    expect_row(0, 0, TILE_A_COLORS[2]);
    expect_row(0, 1, TILE_A_COLORS[3]);
    expect_row(0, 5, TILE_A_COLORS[7]);

    /* The 256x256 map wraps: with SCX = 252 the tile at column 0 sits 4
     * pixels in, and the last four columns of the map fill the left. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;
    put_solid_tile(2, 3);
    ppu.vram[MAP_LOW + 31] = 2;
    ppu_write(&ppu, PPU_SCX_ADDRESS, 252);
    draw(BG_ON | TILES_UNSIGNED);
    expect_blank(0, 0, 4, 8, 3);
    expect_row(4, 0, TILE_A_COLORS[0]);

    /* And vertically: SCY = 252 shows the last map row first. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;
    put_solid_tile(2, 2);
    ppu.vram[MAP_LOW + 31 * 32] = 2;
    ppu_write(&ppu, PPU_SCY_ADDRESS, 252);
    draw(BG_ON | TILES_UNSIGNED);
    expect_blank(0, 0, 4, 4, 0 + 2);
    expect_row(0, 4, TILE_A_COLORS[0]);
}

static void test_tile_data_addressing(void)
{
    /* Bit 4 clear: signed numbers around 9000, so 0x00 is at 9000. */
    begin();
    put_tile_at(0x1000, TILE_A);
    uint8_t solid3[16];
    uint8_t solid1[16];

    solid_tile(3, solid3);
    solid_tile(1, solid1);

    put_tile_at(0x0800, solid3);        /* number 0x80 = -128 -> 8800 */
    put_tile_at(0x0FF0, solid1);        /* number 0xFF = -1   -> 8FF0 */
    ppu.vram[MAP_LOW + 0] = 0x00;
    ppu.vram[MAP_LOW + 1] = 0x80;
    ppu.vram[MAP_LOW + 2] = 0xFF;

    draw(BG_ON);

    expect_tile(0, 0);
    expect_blank(8, 0, 8, 8, 3);
    expect_blank(16, 0, 8, 8, 1);

    /* Bit 4 set: unsigned numbers from 8000, so 0x80 is at 8800 too but
     * 0x00 is at 8000 and no longer at 9000. */
    begin();
    put_tile_at(0x1000, TILE_A);
    put_tile_at(0x0800, solid3);
    ppu.vram[MAP_LOW + 0] = 0x00;
    ppu.vram[MAP_LOW + 1] = 0x80;
    draw(BG_ON | TILES_UNSIGNED);
    expect_blank(0, 0, 8, 8, 0);
    expect_blank(8, 0, 8, 8, 3);
}

static void test_background_map_select(void)
{
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;
    ppu.vram[MAP_HIGH] = 0;

    draw(BG_ON | TILES_UNSIGNED);
    expect_tile(0, 0);

    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;
    ppu.vram[MAP_HIGH] = 0;
    draw(BG_ON | TILES_UNSIGNED | BG_MAP_HIGH);
    expect_blank(0, 0, 8, 8, 0);

    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_HIGH] = 1;
    draw(BG_ON | TILES_UNSIGNED | BG_MAP_HIGH);
    expect_tile(0, 0);
}

static void test_background_disabled(void)
{
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;
    ppu_write(&ppu, PPU_BGP_ADDRESS, 0xFF);

    /* With bit 0 clear the background is blank, whatever the palette. */
    draw(TILES_UNSIGNED);
    expect_blank(0, 0, 160, 144, 0);

    /* Color numbers under a disabled background count as 0, so a sprite that
     * asks to be behind BG colors 1-3 still shows. */
    begin();
    put_tile(1, TILE_A);
    put_solid_tile(2, 3);
    ppu.vram[MAP_LOW] = 2;
    put_sprite(0, 16, 8, 1, 0x80);
    draw(OBJ_ON | TILES_UNSIGNED);
    expect_tile(0, 0);
}

static void test_window(void)
{
    /* Window at x = 80 (WX = 87) from line 8, with its own map at 9C00. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_HIGH] = 1;
    ppu.vram[MAP_HIGH + 32] = 1;   /* second window row */
    ppu_write(&ppu, PPU_WX_ADDRESS, 87);
    ppu_write(&ppu, PPU_WY_ADDRESS, 8);

    draw(BG_ON | TILES_UNSIGNED | WINDOW_ON | WINDOW_MAP_HIGH);

    expect_tile(80, 8);
    expect_tile(80, 16);   /* continues from the window's own line counter */
    expect_blank(0, 0, 160, 8, 0);
    expect_blank(0, 8, 80, 136, 0);
    expect_blank(88, 8, 72, 136, 0);

    /* Window disabled: nothing is drawn. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_HIGH] = 1;
    ppu_write(&ppu, PPU_WX_ADDRESS, 87);
    ppu_write(&ppu, PPU_WY_ADDRESS, 8);
    draw(BG_ON | TILES_UNSIGNED | WINDOW_MAP_HIGH);
    expect_blank(0, 0, 160, 144, 0);

    /* WX below 7 pushes the window's left edge off screen: with WX = 3 the
     * first visible column is column 4 of the window's first tile. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_HIGH] = 1;
    ppu_write(&ppu, PPU_WX_ADDRESS, 3);
    draw(BG_ON | TILES_UNSIGNED | WINDOW_ON | WINDOW_MAP_HIGH);
    static const uint8_t cut[4] = { 3, 3, 2, 0 };

    for (unsigned i = 0; i < 4; i++) {
        assert(pixel(i, 0) == cut[i]);
    }

    /* The window needs the background enabled too. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_HIGH] = 1;
    ppu_write(&ppu, PPU_WX_ADDRESS, 7);
    draw(TILES_UNSIGNED | WINDOW_ON | WINDOW_MAP_HIGH);
    expect_blank(0, 0, 160, 144, 0);

    /* WX above 166 hides it. */
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_HIGH] = 1;
    ppu_write(&ppu, PPU_WX_ADDRESS, 167);
    draw(BG_ON | TILES_UNSIGNED | WINDOW_ON | WINDOW_MAP_HIGH);
    expect_blank(0, 0, 160, 144, 0);
}

static void test_sprite_basics(void)
{
    begin();
    put_tile(1, TILE_A);
    put_sprite(0, 16, 8, 1, 0);   /* top-left corner of the screen */

    draw(BG_ON | OBJ_ON | TILES_UNSIGNED);
    expect_tile(0, 0);

    /* Color 0 is transparent: the background shows through. */
    begin();
    put_tile(1, TILE_A);
    put_solid_tile(2, 2);
    ppu.vram[MAP_LOW] = 2;
    put_sprite(0, 16, 8, 1, 0);
    draw(BG_ON | OBJ_ON | TILES_UNSIGNED);
    assert(pixel(0, 0) == 2);   /* sprite color 0 over BG shade 2 */
    assert(pixel(1, 0) == 2);   /* sprite color 2 */
    assert(pixel(2, 0) == 3);   /* sprite color 3 */

    /* Disabled sprites are not drawn. */
    begin();
    put_tile(1, TILE_A);
    put_sprite(0, 16, 8, 1, 0);
    draw(BG_ON | TILES_UNSIGNED);
    expect_blank(0, 0, 8, 8, 0);

    /* They show with the background off. */
    begin();
    put_tile(1, TILE_A);
    put_sprite(0, 16, 8, 1, 0);
    draw(OBJ_ON | TILES_UNSIGNED);
    expect_tile(0, 0);

    /* A sprite partly off the left edge shows its right columns. */
    begin();
    put_tile(1, TILE_A);
    put_sprite(0, 16, 4, 1, 0);   /* screen x = -4 */
    draw(BG_ON | OBJ_ON | TILES_UNSIGNED);
    assert(pixel(0, 0) == 3);     /* tile column 4 of row 0 */
    assert(pixel(2, 0) == 2);     /* column 6 */
    assert(pixel(3, 0) == 0);     /* column 7 */
    assert(pixel(4, 0) == 0);
}

static void test_sprite_flips_and_palettes(void)
{
    /* X flip mirrors a row: tile row 6 is 1 0 0 0 0 0 0 2. */
    begin();
    put_tile(1, TILE_A);
    put_sprite(0, 16, 8, 1, 0x20);
    draw(BG_ON | OBJ_ON | TILES_UNSIGNED);
    static const uint8_t flipped[8] = { 2, 0, 0, 0, 0, 0, 0, 1 };

    expect_row(0, 6, flipped);

    /* Y flip: screen line 0 shows tile row 7 and line 7 shows row 0. */
    begin();
    put_tile(1, TILE_A);
    put_sprite(0, 16, 8, 1, 0x40);
    draw(BG_ON | OBJ_ON | TILES_UNSIGNED);
    expect_row(0, 0, TILE_A_COLORS[7]);
    expect_row(0, 1, TILE_A_COLORS[6]);
    expect_row(0, 7, TILE_A_COLORS[0]);

    /* Bit 4 picks OBP1. Here OBP1 inverts the colors and OBP0 does not. */
    begin();
    put_tile(1, TILE_A);
    ppu_write(&ppu, PPU_OBP1_ADDRESS, 0x1B);
    put_sprite(0, 16, 8, 1, 0x00);
    put_sprite(1, 16, 24, 1, 0x10);
    draw(BG_ON | OBJ_ON | TILES_UNSIGNED);
    expect_row(0, 1, TILE_A_COLORS[1]);
    for (unsigned i = 0; i < 8; i++) {
        /* Color 1 under OBP1 = 0x1B is shade 2. */
        assert(pixel(16 + i, 1) == 2);
    }
    /* Transparent color 0 stays transparent even though OBP1 maps it. */
    assert(pixel(16, 0) == 0);
}

static void test_sprite_priority_flag(void)
{
    /* BG color 2 under the sprite, and BG color 0 further right. */
    begin();
    put_tile(1, TILE_A);
    put_solid_tile(2, 2);
    ppu.vram[MAP_LOW] = 2;
    put_sprite(0, 16, 8, 1, 0x80);    /* behind BG colors 1-3 */
    put_sprite(1, 16, 24, 1, 0x80);   /* over BG color 0 */
    draw(BG_ON | OBJ_ON | TILES_UNSIGNED);

    expect_blank(0, 0, 8, 8, 2);      /* hidden by the BG */
    expect_tile(16, 0);               /* nothing to hide behind */
}

static void test_sprite_priority_between_sprites(void)
{
    uint8_t solid3[16];
    uint8_t solid1[16];

    solid_tile(3, solid3);
    solid_tile(1, solid1);

    /* Lower X wins: sprite 0 (x = 8..15) sits under sprite 1 (x = 4..11)
     * where they overlap. */
    begin();
    put_tile(2, solid3);
    put_tile(3, solid1);
    put_sprite(0, 16, 16, 2, 0);
    put_sprite(1, 16, 12, 3, 0);
    draw(OBJ_ON | TILES_UNSIGNED);
    expect_blank(4, 0, 8, 8, 1);      /* sprite 1: x = 4..11 */
    expect_blank(12, 0, 4, 8, 3);     /* sprite 0 where sprite 1 ends */

    /* The same X: the lower OAM index wins. */
    begin();
    put_tile(2, solid3);
    put_tile(3, solid1);
    put_sprite(0, 16, 8, 2, 0);
    put_sprite(1, 16, 8, 3, 0);
    draw(OBJ_ON | TILES_UNSIGNED);
    expect_blank(0, 0, 8, 8, 3);

    /* An opaque sprite hidden behind the background still blocks a lower
     * priority sprite below it. */
    begin();
    put_solid_tile(4, 2);
    ppu.vram[MAP_LOW] = 4;
    put_tile(2, solid3);
    put_tile(3, solid1);
    put_sprite(0, 16, 8, 2, 0x80);    /* wins, but is behind BG color 2 */
    put_sprite(1, 16, 8, 3, 0);       /* would show, but loses priority */
    draw(BG_ON | OBJ_ON | TILES_UNSIGNED);
    expect_blank(0, 0, 8, 8, 2);
}

static void test_sprite_8x16(void)
{
    uint8_t solid1[16];

    solid_tile(1, solid1);

    /* Tile number 3 in 8x16 mode means tiles 2 (top) and 3 (bottom). */
    begin();
    put_tile(2, solid1);
    put_tile(3, TILE_A);
    put_sprite(0, 16, 8, 3, 0);
    draw(OBJ_ON | OBJ_TALL | TILES_UNSIGNED);
    expect_blank(0, 0, 8, 8, 1);
    expect_tile(0, 8);
    expect_blank(0, 16, 8, 8, 0);

    /* Y flip swaps the halves and flips each. */
    begin();
    put_tile(2, solid1);
    put_tile(3, TILE_A);
    put_sprite(0, 16, 8, 2, 0x40);
    draw(OBJ_ON | OBJ_TALL | TILES_UNSIGNED);
    expect_row(0, 0, TILE_A_COLORS[7]);
    expect_row(0, 7, TILE_A_COLORS[0]);
    expect_blank(0, 8, 8, 8, 1);
}

static void test_sprites_per_line_limit(void)
{
    /* Eleven sprites on one line: only the first ten in OAM order show. */
    begin();
    put_solid_tile(1, 3);

    for (unsigned i = 0; i < 11; i++) {
        put_sprite(i, 16, (uint8_t)(8 + i * 8), 1, 0);
    }

    draw(OBJ_ON | TILES_UNSIGNED);

    for (unsigned i = 0; i < 10; i++) {
        assert(pixel(i * 8, 0) == 3);
    }

    assert(pixel(80, 0) == 0);

    /* The limit is per line: the next line has room again. */
    begin();
    put_solid_tile(1, 3);

    for (unsigned i = 0; i < 11; i++) {
        put_sprite(i, (uint8_t)(16 + (i == 10 ? 8 : 0)), (uint8_t)(8 + i * 8),
                   1, 0);
    }

    draw(OBJ_ON | TILES_UNSIGNED);
    assert(pixel(80, 0) == 0);
    assert(pixel(80, 8) == 3);
}

static void test_lcd_off_blanks_the_screen(void)
{
    begin();
    put_tile(1, TILE_A);
    ppu.vram[MAP_LOW] = 1;
    draw(BG_ON | TILES_UNSIGNED);
    assert(pixel(2, 0) == 3);

    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x00);
    expect_blank(0, 0, 160, 144, 0);
}

int main(void)
{
    test_background_tile();
    test_background_palette();
    test_scrolling();
    test_tile_data_addressing();
    test_background_map_select();
    test_background_disabled();
    test_window();
    test_sprite_basics();
    test_sprite_flips_and_palettes();
    test_sprite_priority_flag();
    test_sprite_priority_between_sprites();
    test_sprite_8x16();
    test_sprites_per_line_limit();
    test_lcd_off_blanks_the_screen();

    printf("PPU rendering tests passed!\n");

    return 0;
}
