#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ppu.h>

#include "ppu_internal.h"

enum {
    OAM_SCAN_END_DOT = 80,
    DRAWING_END_DOT = 252,
    FIRST_VBLANK_LINE = 144
};

static bool ppu_lcd_on(const Ppu *ppu)
{
    return (ppu->lcdc & PPU_LCDC_LCD_ENABLE) != 0;
}

/* The CPU cannot see VRAM while the LCD is reading it, or OAM while it scans. */
static bool ppu_vram_accessible(const Ppu *ppu)
{
    return !ppu_lcd_on(ppu) || ppu->mode != PPU_MODE_DRAWING;
}

static bool ppu_oam_accessible(const Ppu *ppu)
{
    return !ppu_lcd_on(ppu) ||
           (ppu->mode != PPU_MODE_OAM_SCAN && ppu->mode != PPU_MODE_DRAWING);
}

static bool ppu_lyc_match(const Ppu *ppu)
{
    return ppu_lcd_on(ppu) && ppu->ly == ppu->lyc;
}

/*
 * The STAT interrupt line is the OR of every enabled condition, and the
 * interrupt is requested when the line rises, so a condition that stays
 * true across several events requests it only once.
 */
static void ppu_update_stat_line(Ppu *ppu)
{
    bool line = false;

    if (ppu_lcd_on(ppu)) {
        line = (ppu_lyc_match(ppu) &&
                (ppu->stat & PPU_STAT_LYC_INTERRUPT) != 0) ||
               (ppu->mode == PPU_MODE_HBLANK &&
                (ppu->stat & PPU_STAT_HBLANK_INTERRUPT) != 0) ||
               (ppu->mode == PPU_MODE_VBLANK &&
                (ppu->stat & PPU_STAT_VBLANK_INTERRUPT) != 0) ||
               (ppu->mode == PPU_MODE_OAM_SCAN &&
                (ppu->stat & PPU_STAT_OAM_INTERRUPT) != 0);
    }

    if (line && !ppu->stat_line) {
        interrupts_request(ppu->interrupts, INTERRUPT_LCD_STAT);
    }

    ppu->stat_line = line;
}

static void ppu_start_frame(Ppu *ppu)
{
    ppu->ly = 0;
    ppu->line_dot = 0;
    ppu->window_line = 0;
    ppu->mode = PPU_MODE_OAM_SCAN;
}

void ppu_init(Ppu *ppu, InterruptRegisters *interrupts)
{
    ppu->interrupts = interrupts;
    ppu_reset(ppu);
}

void ppu_reset(Ppu *ppu)
{
    memset(ppu->vram, 0, sizeof(ppu->vram));
    memset(ppu->oam, 0, sizeof(ppu->oam));
    memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));

    ppu->lcdc = 0x91;
    ppu->stat = 0;
    ppu->scy = 0;
    ppu->scx = 0;
    ppu->lyc = 0;
    ppu->bgp = 0xFC;
    ppu->obp0 = 0xFF;
    ppu->obp1 = 0xFF;
    ppu->wy = 0;
    ppu->wx = 0;
    ppu->stat_line = false;
    ppu->frames = 0;

    ppu_start_frame(ppu);
}

uint8_t ppu_read(const Ppu *ppu, uint16_t address)
{
    if (address >= PPU_VRAM_START && address <= PPU_VRAM_END) {
        return ppu_vram_accessible(ppu)
            ? ppu->vram[address - PPU_VRAM_START]
            : 0xFF;
    }

    if (address >= PPU_OAM_START && address <= PPU_OAM_END) {
        return ppu_oam_accessible(ppu)
            ? ppu->oam[address - PPU_OAM_START]
            : 0xFF;
    }

    switch (address) {
        case PPU_LCDC_ADDRESS:
            return ppu->lcdc;

        case PPU_STAT_ADDRESS:
            return (uint8_t)(
                0x80 |
                (ppu->stat & PPU_STAT_WRITE_MASK) |
                (ppu_lyc_match(ppu) ? PPU_STAT_LYC_FLAG : 0) |
                (ppu_lcd_on(ppu) ? ppu->mode : PPU_MODE_HBLANK)
            );

        case PPU_SCY_ADDRESS:
            return ppu->scy;

        case PPU_SCX_ADDRESS:
            return ppu->scx;

        case PPU_LY_ADDRESS:
            return ppu->ly;

        case PPU_LYC_ADDRESS:
            return ppu->lyc;

        case PPU_BGP_ADDRESS:
            return ppu->bgp;

        case PPU_OBP0_ADDRESS:
            return ppu->obp0;

        case PPU_OBP1_ADDRESS:
            return ppu->obp1;

        case PPU_WY_ADDRESS:
            return ppu->wy;

        case PPU_WX_ADDRESS:
            return ppu->wx;

        default:
            return 0xFF;
    }
}

static void ppu_write_lcdc(Ppu *ppu, uint8_t value)
{
    bool was_on = ppu_lcd_on(ppu);

    ppu->lcdc = value;

    if (was_on && !ppu_lcd_on(ppu)) {
        /* Turning the LCD off blanks the screen and rewinds to line 0. */
        memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));
        ppu_start_frame(ppu);
        ppu->mode = PPU_MODE_HBLANK;
        ppu->stat_line = false;
    } else if (!was_on && ppu_lcd_on(ppu)) {
        ppu_start_frame(ppu);
        ppu->stat_line = false;
        ppu_update_stat_line(ppu);
    }
}

void ppu_write(Ppu *ppu, uint16_t address, uint8_t value)
{
    if (address >= PPU_VRAM_START && address <= PPU_VRAM_END) {
        if (ppu_vram_accessible(ppu)) {
            ppu->vram[address - PPU_VRAM_START] = value;
        }

        return;
    }

    if (address >= PPU_OAM_START && address <= PPU_OAM_END) {
        if (ppu_oam_accessible(ppu)) {
            ppu->oam[address - PPU_OAM_START] = value;
        }

        return;
    }

    switch (address) {
        case PPU_LCDC_ADDRESS:
            ppu_write_lcdc(ppu, value);
            break;

        case PPU_STAT_ADDRESS:
            ppu->stat = (uint8_t)(value & PPU_STAT_WRITE_MASK);
            ppu_update_stat_line(ppu);
            break;

        case PPU_SCY_ADDRESS:
            ppu->scy = value;
            break;

        case PPU_SCX_ADDRESS:
            ppu->scx = value;
            break;

        case PPU_LYC_ADDRESS:
            ppu->lyc = value;
            ppu_update_stat_line(ppu);
            break;

        case PPU_BGP_ADDRESS:
            ppu->bgp = value;
            break;

        case PPU_OBP0_ADDRESS:
            ppu->obp0 = value;
            break;

        case PPU_OBP1_ADDRESS:
            ppu->obp1 = value;
            break;

        case PPU_WY_ADDRESS:
            ppu->wy = value;
            break;

        case PPU_WX_ADDRESS:
            ppu->wx = value;
            break;

        default:
            /* LY is read-only. */
            break;
    }
}

void ppu_oam_dma_write(Ppu *ppu, uint8_t index, uint8_t value)
{
    if (index < PPU_OAM_SIZE) {
        ppu->oam[index] = value;
    }
}

/* Moves to the next line (or the next frame), picking the new mode. */
static void ppu_finish_line(Ppu *ppu)
{
    ppu->ly++;

    if (ppu->ly == FIRST_VBLANK_LINE) {
        ppu->mode = PPU_MODE_VBLANK;
        ppu->frames++;
        interrupts_request(ppu->interrupts, INTERRUPT_VBLANK);
    } else if (ppu->ly == PPU_LINES_PER_FRAME) {
        ppu_start_frame(ppu);
    } else if (ppu->ly < FIRST_VBLANK_LINE) {
        ppu->mode = PPU_MODE_OAM_SCAN;
    }

    ppu_update_stat_line(ppu);
}

void ppu_step(Ppu *ppu, CpuCycles cycles)
{
    if (!ppu_lcd_on(ppu)) {
        return;
    }

    ppu->line_dot = (uint16_t)(ppu->line_dot + cycles);

    for (;;) {
        if (ppu->mode == PPU_MODE_OAM_SCAN) {
            if (ppu->line_dot < OAM_SCAN_END_DOT) {
                return;
            }

            ppu->mode = PPU_MODE_DRAWING;
            ppu_render_scanline(ppu);
            ppu_update_stat_line(ppu);
        } else if (ppu->mode == PPU_MODE_DRAWING) {
            if (ppu->line_dot < DRAWING_END_DOT) {
                return;
            }

            ppu->mode = PPU_MODE_HBLANK;
            ppu_update_stat_line(ppu);
        } else {
            /* HBlank and VBlank both last until the end of the line. */
            if (ppu->line_dot < PPU_DOTS_PER_LINE) {
                return;
            }

            uint16_t carried = (uint16_t)(ppu->line_dot - PPU_DOTS_PER_LINE);

            ppu_finish_line(ppu);
            ppu->line_dot = carried;
        }
    }
}
