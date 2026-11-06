#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ppu.h>

#include "ppu_internal.h"

enum {
    OAM_SCAN_END_DOT = 80,
    FIRST_VBLANK_LINE = 144,

    /* LY already shows the next line this many dots before a line ends. */
    LY_ADVANCE_DOT = 452,

    /* The scan's last cycle: OAM can be written and VRAM cannot be read. */
    OAM_SCAN_LAST_CYCLE_DOT = 76,

    /* A register write reaches the pixels this many dots into its M-cycle. */
    PALETTE_WRITE_DOT_OFFSET = 1,
    LCDC_WRITE_DOT_OFFSET = 2,

    /* Fitted to the mealybug m3_scx_low_3_bits and m3_scy_change pictures. */
    SCX_WRITE_DOT_OFFSET = 0,
    SCY_WRITE_DOT_OFFSET = 1
};

static bool ppu_lcd_on(const Ppu *ppu)
{
    return (ppu->lcdc & PPU_LCDC_LCD_ENABLE) != 0;
}

/* False after the last visible line, until the frame restarts. */
static bool ppu_next_line_is_visible(const Ppu *ppu)
{
    return ppu->ly + 1 < FIRST_VBLANK_LINE ||
           ppu->ly == PPU_LINES_PER_FRAME - 1;
}

/*
 * Whether the line after this one is announced by a mode 2 pulse in the last
 * M-cycle of this one: every visible line is, and so is line 144, where
 * VBlank begins. Line 0 is not: its pulse comes with the line itself.
 */
static bool ppu_next_line_has_oam_interrupt(const Ppu *ppu)
{
    return ppu->ly + 1 <= FIRST_VBLANK_LINE;
}

/*
 * LY as the CPU sees it: it moves on 4 dots before the line ends. On the last
 * line it reads 0 from its first dot, so 153 is seen only in those 4 dots at
 * the end of line 152.
 */
static uint8_t ppu_visible_ly(const Ppu *ppu)
{
    if (ppu->ly == PPU_LINES_PER_FRAME - 1) {
        return 0;
    }

    if (!ppu->ly_advanced) {
        return ppu->ly;
    }

    return (uint8_t)((ppu->ly + 1) % PPU_LINES_PER_FRAME);
}

/*
 * The LY the LYC comparison uses. It is LY, except on line 153: the first 4
 * dots still compare 153, then there is a gap, and from dot 8 it compares 0.
 */
static uint8_t ppu_compared_ly(const Ppu *ppu)
{
    if (ppu->ly == PPU_LINES_PER_FRAME - 1 && ppu->ly153_stage == 0) {
        return ppu->ly;
    }

    return ppu_visible_ly(ppu);
}

/*
 * The mode bits STAT reads. They are the mode, except in the last M-cycle of
 * line 153, which reads as HBlank before line 0 starts in the scan.
 */
static unsigned ppu_stat_mode(const Ppu *ppu)
{
    if (!ppu_lcd_on(ppu)) {
        return PPU_MODE_HBLANK;
    }

    if (ppu->ly == PPU_LINES_PER_FRAME - 1 && ppu->line_dot >= LY_ADVANCE_DOT) {
        return PPU_MODE_HBLANK;
    }

    return ppu->mode;
}

/*
 * When the CPU can reach VRAM and OAM, dot by dot, as the Mooneye lcdon
 * timing tables show it. Reads and writes differ slightly.
 *
 *   OAM reads    blocked from dot 452 of the line before, through the scan
 *                and drawing
 *   OAM writes   blocked during the scan except its last cycle, and
 *                during drawing
 *   VRAM reads   blocked from the scan's last cycle through drawing
 *   VRAM writes  blocked during drawing
 *
 * Everything is open while the LCD is off, and on the first line after it
 * is switched on, which has no scan, until drawing starts.
 */
static bool ppu_vram_read_blocked(const Ppu *ppu)
{
    if (!ppu_lcd_on(ppu)) {
        return false;
    }

    return ppu->mode == PPU_MODE_DRAWING ||
           (ppu->mode == PPU_MODE_OAM_SCAN &&
            ppu->line_dot >= OAM_SCAN_LAST_CYCLE_DOT);
}

static bool ppu_vram_write_blocked(const Ppu *ppu)
{
    return ppu_lcd_on(ppu) && ppu->mode == PPU_MODE_DRAWING;
}

static bool ppu_oam_read_blocked(const Ppu *ppu)
{
    if (!ppu_lcd_on(ppu)) {
        return false;
    }

    if (ppu->mode == PPU_MODE_OAM_SCAN || ppu->mode == PPU_MODE_DRAWING) {
        return true;
    }

    return ppu->line_dot >= LY_ADVANCE_DOT && ppu_next_line_is_visible(ppu);
}

static bool ppu_oam_write_blocked(const Ppu *ppu)
{
    if (!ppu_lcd_on(ppu)) {
        return false;
    }

    return ppu->mode == PPU_MODE_DRAWING ||
           (ppu->mode == PPU_MODE_OAM_SCAN &&
            ppu->line_dot < OAM_SCAN_LAST_CYCLE_DOT);
}

/*
 * The comparison runs while the LCD is on, against the LY the CPU sees; the
 * flag keeps its value while the LCD is off.
 */
static void ppu_update_lyc_flag(Ppu *ppu)
{
    ppu->lyc_flag = ppu_compared_ly(ppu) == ppu->lyc;
}

/*
 * The STAT interrupt line is the OR of every enabled condition, and the
 * interrupt is requested when the line rises, so a condition that stays
 * true across several events requests it only once.
 */
static void ppu_update_stat_line(Ppu *ppu)
{
    /* The line keeps its value while the LCD is off. */
    if (!ppu_lcd_on(ppu)) {
        return;
    }

    bool lyc = ppu->lyc_flag && (ppu->stat & PPU_STAT_LYC_INTERRUPT) != 0;

    /* The first line after the LCD is switched on reads as mode 0 but has had
     * no HBlank. */
    bool hblank = ppu->mode == PPU_MODE_HBLANK && !ppu->pre_draw &&
                  (ppu->stat & PPU_STAT_HBLANK_INTERRUPT) != 0;
    bool vblank = ppu->mode == PPU_MODE_VBLANK &&
                  (ppu->stat & PPU_STAT_VBLANK_INTERRUPT) != 0;

    /*
     * The mode 2 condition is a pulse, not a level held through the scan: it
     * is high only for the 4 dots before the line it announces starts, so
     * enabling it during the scan does nothing. Line 144 gets one too.
     */
    bool pulse = ppu->oam_pulse && (ppu->stat & PPU_STAT_OAM_INTERRUPT) != 0;
    bool line = lyc || hblank || vblank || pulse;

    if (line && !ppu->stat_line) {
        /* The pulse edge falls late in its M-cycle; the others do not. */
        if (hblank && !lyc && !vblank && ppu->hblank_late) {
            interrupts_request_late(ppu->interrupts, INTERRUPT_LCD_STAT);
        } else if (lyc || hblank || vblank) {
            interrupts_request(ppu->interrupts, INTERRUPT_LCD_STAT);
        } else {
            interrupts_request_late(ppu->interrupts, INTERRUPT_LCD_STAT);
        }
    }

    ppu->stat_line = line;
}

/* Starts the log of register writes for the line that begins now. */
static void ppu_begin_line_log(Ppu *ppu)
{
    ppu->line_registers[PPU_LOGGED_LCDC] = ppu->lcdc;
    ppu->line_registers[PPU_LOGGED_BGP] = ppu->bgp;
    ppu->line_registers[PPU_LOGGED_OBP0] = ppu->obp0;
    ppu->line_registers[PPU_LOGGED_OBP1] = ppu->obp1;
    ppu->line_registers[PPU_LOGGED_SCX] = ppu->scx;
    ppu->line_registers[PPU_LOGGED_SCY] = ppu->scy;
    ppu->write_count = 0;
}

static void ppu_log_entry(
    Ppu *ppu,
    unsigned dot,
    PpuLoggedRegister reg,
    uint8_t value
)
{
    if (ppu->write_count < PPU_WRITE_LOG_SIZE) {
        PpuRegisterWrite *write = &ppu->writes[ppu->write_count++];

        write->dot = (uint16_t)dot;
        write->reg = (uint8_t)reg;
        write->value = value;
    }
}

/*
 * Records a write for the pixels still to come. A palette write glitches for
 * one dot: it reads as the old and the new value ORed together, then as the
 * new one, which is why the pixel where a palette change starts and the one
 * where its undoing starts are not the same number of dots apart as the
 * writes.
 */
static void ppu_log_write(
    Ppu *ppu,
    PpuLoggedRegister reg,
    uint8_t old_value,
    uint8_t value,
    bool palette
)
{
    unsigned plain = reg == PPU_LOGGED_SCX ? SCX_WRITE_DOT_OFFSET :
                     reg == PPU_LOGGED_SCY ? SCY_WRITE_DOT_OFFSET
                                           : LCDC_WRITE_DOT_OFFSET;

    if (palette) {
        unsigned dot = ppu->line_dot + PALETTE_WRITE_DOT_OFFSET;

        ppu_log_entry(ppu, dot, reg, (uint8_t)(old_value | value));
        ppu_log_entry(ppu, dot + 1, reg, value);
    } else {
        ppu_log_entry(ppu, ppu->line_dot + plain, reg, value);
    }
}

uint8_t ppu_register_at(
    const Ppu *ppu,
    PpuLoggedRegister reg,
    unsigned dot
)
{
    uint8_t value = ppu->line_registers[reg];

    for (unsigned i = 0; i < ppu->write_count; i++) {
        if (ppu->writes[i].reg == reg && ppu->writes[i].dot <= dot) {
            value = ppu->writes[i].value;
        }
    }

    return value;
}

static void ppu_start_frame(Ppu *ppu)
{
    ppu->ly = 0;
    ppu->line_dot = 0;
    ppu->window_line = 0;
    ppu->mode = PPU_MODE_OAM_SCAN;
    ppu->pre_draw = false;
    ppu->ly_advanced = false;
    ppu->oam_pulse = false;
    ppu->ly153_stage = 0;
    ppu_begin_line_log(ppu);
}

void ppu_enter_boot_phase(Ppu *ppu)
{
    ppu->ly = PPU_LINES_PER_FRAME - 1;
    ppu->line_dot = 398;
    ppu->mode = PPU_MODE_VBLANK;
    ppu->ly_advanced = false;
    ppu->ly153_stage = 2;
    ppu->oam_pulse = false;
    ppu->window_line = 0;
    ppu_begin_line_log(ppu);

    ppu_update_lyc_flag(ppu);
    ppu_update_stat_line(ppu);
}

void ppu_load_boot_vram(Ppu *ppu, const uint8_t logo[PPU_LOGO_SIZE])
{
    /* The (R) symbol, one row per byte, in the boot ROM at 00D8. */
    static const uint8_t registered[8] = {
        0x3C, 0x42, 0xB9, 0xA5, 0xB9, 0xA5, 0x42, 0x3C
    };

    /*
     * The boot ROM expands each nibble of the logo to a byte by doubling
     * every bit, writes it as two identical rows of a tile, and leaves the
     * second bit plane of those rows at 0.
     */
    unsigned tile_byte = 0x10;

    for (unsigned i = 0; i < PPU_LOGO_SIZE; i++) {
        for (unsigned half = 0; half < 2; half++) {
            unsigned nibble = half == 0 ? logo[i] >> 4 : logo[i] & 0x0F;
            uint8_t row = 0;

            for (unsigned bit = 0; bit < 4; bit++) {
                unsigned set = (nibble >> (3 - bit)) & 1;

                row = (uint8_t)((row << 2) | (set != 0 ? 0x03 : 0x00));
            }

            for (unsigned copy = 0; copy < 2; copy++) {
                ppu->vram[tile_byte] = row;
                tile_byte += 2;
            }
        }
    }

    for (unsigned i = 0; i < sizeof(registered); i++) {
        ppu->vram[tile_byte] = registered[i];
        tile_byte += 2;
    }

    /* Tiles 1 to 12, then the (R), on map row 8 and 13 to 24 on row 9. */
    for (unsigned i = 0; i < 12; i++) {
        ppu->vram[0x1904 + i] = (uint8_t)(1 + i);
        ppu->vram[0x1924 + i] = (uint8_t)(13 + i);
    }

    ppu->vram[0x1910] = 0x19;
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
    ppu_update_lyc_flag(ppu);
}

uint8_t ppu_read(const Ppu *ppu, uint16_t address)
{
    if (address >= PPU_VRAM_START && address <= PPU_VRAM_END) {
        return ppu_vram_read_blocked(ppu)
            ? 0xFF
            : ppu->vram[address - PPU_VRAM_START];
    }

    if (address >= PPU_OAM_START && address <= PPU_OAM_END) {
        return ppu_oam_read_blocked(ppu)
            ? 0xFF
            : ppu->oam[address - PPU_OAM_START];
    }

    switch (address) {
        case PPU_LCDC_ADDRESS:
            return ppu->lcdc;

        case PPU_STAT_ADDRESS:
            return (uint8_t)(
                0x80 |
                (ppu->stat & PPU_STAT_WRITE_MASK) |
                (ppu->lyc_flag ? PPU_STAT_LYC_FLAG : 0) |
                ppu_stat_mode(ppu)
            );

        case PPU_SCY_ADDRESS:
            return ppu->scy;

        case PPU_SCX_ADDRESS:
            return ppu->scx;

        case PPU_LY_ADDRESS:
            return ppu_visible_ly(ppu);

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
    } else if (!was_on && ppu_lcd_on(ppu)) {
        /*
         * The first line starts in mode 0 with no OAM scan and goes straight
         * to drawing at dot 80.
         */
        ppu_start_frame(ppu);
        ppu->mode = PPU_MODE_HBLANK;
        ppu->pre_draw = true;
        ppu_update_lyc_flag(ppu);
        ppu_update_stat_line(ppu);
    }
}

void ppu_write(Ppu *ppu, uint16_t address, uint8_t value)
{
    if (address >= PPU_VRAM_START && address <= PPU_VRAM_END) {
        if (!ppu_vram_write_blocked(ppu)) {
            ppu->vram[address - PPU_VRAM_START] = value;
        }

        return;
    }

    if (address >= PPU_OAM_START && address <= PPU_OAM_END) {
        if (!ppu_oam_write_blocked(ppu)) {
            ppu->oam[address - PPU_OAM_START] = value;
        }

        return;
    }

    switch (address) {
        case PPU_LCDC_ADDRESS:
            ppu_log_write(ppu, PPU_LOGGED_LCDC, ppu->lcdc, value, false);
            ppu_write_lcdc(ppu, value);
            break;

        case PPU_STAT_ADDRESS:
            /*
             * The DMG's STAT write glitch: while the write happens every
             * enable bit reads as set, so the line is high in any mode but
             * drawing, or whenever LY = LYC, and the interrupt is requested
             * if that raises a line that was low. The new enables then
             * apply as usual.
             */
            ppu->stat = PPU_STAT_WRITE_MASK;
            ppu_update_stat_line(ppu);

            ppu->stat = (uint8_t)(value & PPU_STAT_WRITE_MASK);
            ppu_update_stat_line(ppu);
            break;

        case PPU_SCY_ADDRESS:
            ppu_log_write(ppu, PPU_LOGGED_SCY, ppu->scy, value, false);
            ppu->scy = value;
            break;

        case PPU_SCX_ADDRESS:
            ppu_log_write(ppu, PPU_LOGGED_SCX, ppu->scx, value, false);
            ppu->scx = value;
            break;

        case PPU_LYC_ADDRESS:
            ppu->lyc = value;

            if (ppu_lcd_on(ppu)) {
                ppu_update_lyc_flag(ppu);
                ppu_update_stat_line(ppu);
            }
            break;

        case PPU_BGP_ADDRESS:
            ppu_log_write(ppu, PPU_LOGGED_BGP, ppu->bgp, value, true);
            ppu->bgp = value;
            break;

        case PPU_OBP0_ADDRESS:
            ppu_log_write(ppu, PPU_LOGGED_OBP0, ppu->obp0, value, true);
            ppu->obp0 = value;
            break;

        case PPU_OBP1_ADDRESS:
            ppu_log_write(ppu, PPU_LOGGED_OBP1, ppu->obp1, value, true);
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
    ppu->ly_advanced = false;
    ppu->oam_pulse = false;
    ppu->ly153_stage = 0;
    ppu_begin_line_log(ppu);

    if (ppu->ly == FIRST_VBLANK_LINE) {
        ppu->mode = PPU_MODE_VBLANK;
        ppu->frames++;
        interrupts_request(ppu->interrupts, INTERRUPT_VBLANK);
    } else if (ppu->ly == PPU_LINES_PER_FRAME) {
        ppu_start_frame(ppu);
        ppu->oam_pulse = true;
    } else if (ppu->ly < FIRST_VBLANK_LINE) {
        ppu->mode = PPU_MODE_OAM_SCAN;
    }

    ppu_update_lyc_flag(ppu);
    ppu_update_stat_line(ppu);
}

/* Drawing starts: renders the line and works out when it will end. */
static void ppu_start_drawing(Ppu *ppu)
{
    ppu->mode = PPU_MODE_DRAWING;
    ppu->drawing_end_dot =
        (uint16_t)(OAM_SCAN_END_DOT + ppu_drawing_dots(ppu));
    ppu_update_stat_line(ppu);
}

void ppu_step(Ppu *ppu, CpuCycles cycles)
{
    if (!ppu_lcd_on(ppu)) {
        return;
    }

    ppu->line_dot = (uint16_t)(ppu->line_dot + cycles);

    for (;;) {
        if (ppu->pre_draw) {
            if (ppu->line_dot < OAM_SCAN_END_DOT) {
                return;
            }

            ppu->pre_draw = false;
            ppu_start_drawing(ppu);
        } else if (ppu->mode == PPU_MODE_OAM_SCAN) {
            /* Line 0's pulse lasts its first M-cycle. */
            if (ppu->oam_pulse && ppu->line_dot >= CYCLES_PER_MCYCLE) {
                ppu->oam_pulse = false;
                ppu_update_stat_line(ppu);
            }

            if (ppu->line_dot < OAM_SCAN_END_DOT) {
                return;
            }

            ppu_start_drawing(ppu);
        } else if (ppu->mode == PPU_MODE_DRAWING) {
            if (ppu->line_dot < ppu->drawing_end_dot) {
                return;
            }

            /*
             * The mode changes at drawing_end_dot, which can be anywhere in
             * the M-cycle that ends at line_dot; a request in its last 2 dots
             * is met late by a halted CPU.
             */
            ppu_render_scanline(ppu);

            ppu->mode = PPU_MODE_HBLANK;
            ppu->hblank_late = ppu->line_dot - ppu->drawing_end_dot < 2;
            ppu_update_stat_line(ppu);
        } else {
            /* HBlank and VBlank both last until the end of the line. */
            if (ppu->ly == PPU_LINES_PER_FRAME - 1) {
                /* LY already reads 0 here, so the comparison changes twice
                 * early in the line instead of at its end. */
                if (ppu->ly153_stage == 0 && ppu->line_dot >= 4) {
                    ppu->ly153_stage = 1;
                    ppu->lyc_flag = false;
                    ppu_update_stat_line(ppu);
                }

                if (ppu->ly153_stage == 1 && ppu->line_dot >= 8) {
                    ppu->ly153_stage = 2;
                    ppu_update_lyc_flag(ppu);
                    ppu_update_stat_line(ppu);
                }
            }

            if (!ppu->ly_advanced && ppu->line_dot >= LY_ADVANCE_DOT) {
                ppu->ly_advanced = true;

                /* LY has moved on but the comparison has not caught up: the
                 * flag reads 0 until the new line starts. Not on line 153,
                 * where LY stays 0. */
                if (ppu->ly != PPU_LINES_PER_FRAME - 1) {
                    ppu->lyc_flag = false;
                }

                ppu->oam_pulse = ppu_next_line_has_oam_interrupt(ppu);
                ppu_update_stat_line(ppu);
            }

            if (ppu->line_dot < PPU_DOTS_PER_LINE) {
                return;
            }

            uint16_t carried = (uint16_t)(ppu->line_dot - PPU_DOTS_PER_LINE);

            ppu_finish_line(ppu);
            ppu->line_dot = carried;
        }
    }
}
