#ifndef PPU_H
#define PPU_H

#include <stdbool.h>
#include <stdint.h>

#include <cycles.h>
#include <interrupts.h>

enum {
    PPU_SCREEN_WIDTH = 160,
    PPU_SCREEN_HEIGHT = 144,

    PPU_VRAM_START = 0x8000,
    PPU_VRAM_END = 0x9FFF,
    PPU_VRAM_SIZE = 0x2000,

    PPU_OAM_START = 0xFE00,
    PPU_OAM_END = 0xFE9F,
    PPU_OAM_SIZE = 0xA0,

    PPU_LCDC_ADDRESS = 0xFF40,
    PPU_STAT_ADDRESS = 0xFF41,
    PPU_SCY_ADDRESS = 0xFF42,
    PPU_SCX_ADDRESS = 0xFF43,
    PPU_LY_ADDRESS = 0xFF44,
    PPU_LYC_ADDRESS = 0xFF45,
    /* FF46 is the OAM DMA register and does not belong to the PPU. */
    PPU_BGP_ADDRESS = 0xFF47,
    PPU_OBP0_ADDRESS = 0xFF48,
    PPU_OBP1_ADDRESS = 0xFF49,
    PPU_WY_ADDRESS = 0xFF4A,
    PPU_WX_ADDRESS = 0xFF4B,

    PPU_DOTS_PER_LINE = 456,
    PPU_LINES_PER_FRAME = 154
};

/* LCDC bits. */
enum {
    PPU_LCDC_BG_ENABLE = 0x01,
    PPU_LCDC_OBJ_ENABLE = 0x02,
    PPU_LCDC_OBJ_TALL = 0x04,
    PPU_LCDC_BG_MAP_HIGH = 0x08,
    PPU_LCDC_TILE_DATA_UNSIGNED = 0x10,
    PPU_LCDC_WINDOW_ENABLE = 0x20,
    PPU_LCDC_WINDOW_MAP_HIGH = 0x40,
    PPU_LCDC_LCD_ENABLE = 0x80
};

/* STAT bits. The low two bits are the mode; bit 2 is LY == LYC. */
enum {
    PPU_STAT_LYC_FLAG = 0x04,
    PPU_STAT_HBLANK_INTERRUPT = 0x08,
    PPU_STAT_VBLANK_INTERRUPT = 0x10,
    PPU_STAT_OAM_INTERRUPT = 0x20,
    PPU_STAT_LYC_INTERRUPT = 0x40,
    PPU_STAT_WRITE_MASK = 0x78
};

typedef enum {
    PPU_MODE_HBLANK = 0,
    PPU_MODE_VBLANK = 1,
    PPU_MODE_OAM_SCAN = 2,
    PPU_MODE_DRAWING = 3
} PpuMode;

typedef struct Ppu {
    uint8_t vram[PPU_VRAM_SIZE];
    uint8_t oam[PPU_OAM_SIZE];

    /* Shade 0 (lightest) to 3 (darkest) per pixel, row-major. */
    uint8_t framebuffer[PPU_SCREEN_WIDTH * PPU_SCREEN_HEIGHT];

    uint8_t lcdc;
    uint8_t stat;    /* only the writable bits; the rest is derived */
    uint8_t scy;
    uint8_t scx;
    uint8_t ly;
    uint8_t lyc;
    uint8_t bgp;
    uint8_t obp0;
    uint8_t obp1;
    uint8_t wy;
    uint8_t wx;

    PpuMode mode;
    uint16_t line_dot;      /* dots into the current line, 0-455 */
    uint8_t window_line;    /* next window row to draw */
    bool stat_line;         /* the ORed STAT interrupt condition */
    uint64_t frames;        /* completed frames since reset */

    InterruptRegisters *interrupts;
} Ppu;

/*
 * The picture processing unit: VRAM, OAM, the LCD registers, the mode
 * state machine (456 dots per line, 154 lines, OAM scan then drawing then
 * HBlank, then VBlank), the VBlank and STAT interrupts, and the scanline
 * renderer.
 *
 * Time comes from ppu_step() in T-cycles, which are dots. Each visible
 * line is rendered in one go when drawing starts, from the registers as
 * they are at that moment, so changes made during a line apply from the
 * next one. VRAM is unreadable while drawing and OAM while scanning or
 * drawing, as on hardware: reads give 0xFF and writes are dropped, unless
 * the LCD is off.
 *
 * Reset state is the DMG after its boot ROM: LCD on, BGP 0xFC, at the
 * start of line 0.
 */

void ppu_init(Ppu *ppu, InterruptRegisters *interrupts);

/* Back to the reset state; VRAM, OAM and the framebuffer are cleared. */
void ppu_reset(Ppu *ppu);

/*
 * VRAM, OAM and the registers FF40-FF45 and FF47-FF4B. `address` must be
 * one of those; the Bus routes only those.
 */
uint8_t ppu_read(const Ppu *ppu, uint16_t address);
void ppu_write(Ppu *ppu, uint16_t address, uint8_t value);

/* Writes an OAM byte regardless of the mode, as OAM DMA does. */
void ppu_oam_dma_write(Ppu *ppu, uint8_t index, uint8_t value);

void ppu_step(Ppu *ppu, CpuCycles cycles);

#endif
