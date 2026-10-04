/*
 * PPU timing, interrupts and CPU access rules, through the ppu.h interface:
 * 456 dots per line, 154 lines, mode order, the VBlank and STAT interrupts
 * and when VRAM and OAM can be reached.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <interrupts.h>
#include <ppu.h>

enum {
    LINE = 456,
    FRAME = 456 * 154
};

static Ppu ppu;
static InterruptRegisters interrupts;

static void setup(void)
{
    interrupts_init(&interrupts);
    ppu_init(&ppu, &interrupts);
}

/* Steps in 4-dot M-cycles, like the machine does. */
static void run(unsigned dots)
{
    assert(dots % 4 == 0);

    for (unsigned i = 0; i < dots; i += 4) {
        ppu_step(&ppu, 4);
    }
}

static uint8_t read(uint16_t address)
{
    return ppu_read(&ppu, address);
}

static unsigned mode(void)
{
    return read(PPU_STAT_ADDRESS) & 0x03;
}

static bool irq(uint8_t source)
{
    return (interrupts.interrupt_flag & source) != 0;
}

static void clear_irqs(void)
{
    interrupts.interrupt_flag = 0;
}

static void test_reset_state(void)
{
    setup();

    assert(read(PPU_LCDC_ADDRESS) == 0x91);
    assert(read(PPU_BGP_ADDRESS) == 0xFC);
    assert(read(PPU_OBP0_ADDRESS) == 0xFF);
    assert(read(PPU_OBP1_ADDRESS) == 0xFF);
    assert(read(PPU_LY_ADDRESS) == 0);

    /* Line 0, OAM scan, and LY == LYC because both are 0. */
    assert(read(PPU_STAT_ADDRESS) == (0x80 | 0x04 | 0x02));
    assert(ppu.frames == 0);
}

static void test_line_timing(void)
{
    setup();

    assert(mode() == PPU_MODE_OAM_SCAN);
    run(76);
    assert(mode() == PPU_MODE_OAM_SCAN);
    run(4);
    assert(mode() == PPU_MODE_DRAWING);
    run(168);
    assert(mode() == PPU_MODE_DRAWING);
    run(4);
    assert(mode() == PPU_MODE_HBLANK);
    run(196);
    assert(mode() == PPU_MODE_HBLANK);
    assert(read(PPU_LY_ADDRESS) == 0);

    /* LY advances 4 dots before the line ends, while the mode is still 0. */
    run(4);
    assert(ppu.line_dot == 452);
    assert(read(PPU_LY_ADDRESS) == 1);
    assert(mode() == PPU_MODE_HBLANK);

    run(4);
    assert(read(PPU_LY_ADDRESS) == 1);
    assert(mode() == PPU_MODE_OAM_SCAN);
    assert(ppu.line_dot == 0);
}

static void test_frame_timing_and_vblank(void)
{
    setup();

    run(143 * LINE + 448);
    assert(read(PPU_LY_ADDRESS) == 143);
    assert(!irq(INTERRUPT_VBLANK));
    assert(ppu.frames == 0);

    /* LY shows 144 from dot 452, but VBlank starts with the line. */
    run(4);
    assert(read(PPU_LY_ADDRESS) == 144);
    assert(!irq(INTERRUPT_VBLANK));

    run(4);
    assert(read(PPU_LY_ADDRESS) == 144);
    assert(mode() == PPU_MODE_VBLANK);
    assert(irq(INTERRUPT_VBLANK));
    assert(ppu.frames == 1);

    /* VBlank lasts 10 lines and covers all of them. */
    clear_irqs();
    run(9 * LINE + 448);
    assert(read(PPU_LY_ADDRESS) == 153);
    assert(mode() == PPU_MODE_VBLANK);

    run(8);
    assert(read(PPU_LY_ADDRESS) == 0);
    assert(mode() == PPU_MODE_OAM_SCAN);
    assert(!irq(INTERRUPT_VBLANK));

    /* The next frame raises it again, once. */
    run(FRAME);
    assert(irq(INTERRUPT_VBLANK));
    assert(ppu.frames == 2);
}

static void test_lyc_flag_and_interrupt(void)
{
    setup();

    ppu_write(&ppu, PPU_LYC_ADDRESS, 5);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);

    run(5 * LINE);
    assert(read(PPU_LY_ADDRESS) == 5);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) != 0);
    assert(!irq(INTERRUPT_LCD_STAT));

    run(LINE);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);

    /* With the interrupt enabled it fires when LY reaches LYC, once. */
    setup();
    ppu_write(&ppu, PPU_LYC_ADDRESS, 5);
    ppu_write(&ppu, PPU_STAT_ADDRESS, PPU_STAT_LYC_INTERRUPT);
    run(5 * LINE - 4);   /* dot 452 of line 4: LY already reads 5 */
    assert(!irq(INTERRUPT_LCD_STAT));
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);
    run(4);
    assert(irq(INTERRUPT_LCD_STAT));
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) != 0);

    clear_irqs();
    run(LINE - 4);
    assert(!irq(INTERRUPT_LCD_STAT));
}

static void test_stat_mode_interrupts(void)
{
    /* HBlank: at the end of drawing on every visible line. */
    setup();
    ppu_write(&ppu, PPU_STAT_ADDRESS, PPU_STAT_HBLANK_INTERRUPT);
    run(248);
    assert(!irq(INTERRUPT_LCD_STAT));
    run(4);
    assert(irq(INTERRUPT_LCD_STAT));
    clear_irqs();
    run(LINE);
    assert(irq(INTERRUPT_LCD_STAT));

    /* OAM scan: at the start of every visible line. */
    setup();
    ppu_write(&ppu, PPU_STAT_ADDRESS, PPU_STAT_OAM_INTERRUPT);
    clear_irqs();
    run(LINE - 4);
    assert(!irq(INTERRUPT_LCD_STAT));
    run(4);
    assert(irq(INTERRUPT_LCD_STAT));

    /* VBlank: only when line 144 starts. */
    setup();
    ppu_write(&ppu, PPU_STAT_ADDRESS, PPU_STAT_VBLANK_INTERRUPT);
    run(144 * LINE - 4);
    assert(!irq(INTERRUPT_LCD_STAT));
    run(4);
    assert(irq(INTERRUPT_LCD_STAT));
}

/*
 * The interrupt fires when the ORed condition rises. At the end of HBlank
 * the next line's OAM scan keeps it high, so with both enabled the OAM
 * condition does not raise it again.
 */
static void test_stat_interrupt_blocking(void)
{
    setup();
    ppu_write(&ppu, PPU_STAT_ADDRESS,
              PPU_STAT_HBLANK_INTERRUPT | PPU_STAT_OAM_INTERRUPT);

    /* Line 0 already raised the OAM condition when STAT was written. */
    run(252);
    clear_irqs();
    run(LINE - 252);
    assert(read(PPU_LY_ADDRESS) == 1);
    assert(mode() == PPU_MODE_OAM_SCAN);
    assert(!irq(INTERRUPT_LCD_STAT));
}

static void test_access_rules(void)
{
    setup();

    /* OAM scan: VRAM is open, OAM is not. */
    assert(mode() == PPU_MODE_OAM_SCAN);
    ppu_write(&ppu, PPU_VRAM_START, 0x11);
    assert(ppu.vram[0] == 0x11);
    assert(read(PPU_VRAM_START) == 0x11);
    ppu_write(&ppu, PPU_OAM_START, 0x22);
    assert(ppu.oam[0] == 0x00);
    assert(read(PPU_OAM_START) == 0xFF);

    /* Drawing: neither. */
    run(80);
    assert(mode() == PPU_MODE_DRAWING);
    ppu_write(&ppu, PPU_VRAM_START, 0x33);
    assert(ppu.vram[0] == 0x11);
    assert(read(PPU_VRAM_START) == 0xFF);
    ppu_write(&ppu, PPU_OAM_START, 0x44);
    assert(ppu.oam[0] == 0x00);
    assert(read(PPU_OAM_START) == 0xFF);

    /* HBlank: both. */
    run(172);
    assert(mode() == PPU_MODE_HBLANK);
    ppu_write(&ppu, PPU_VRAM_START + 1, 0x55);
    ppu_write(&ppu, PPU_OAM_START + 1, 0x66);
    assert(read(PPU_VRAM_START + 1) == 0x55);
    assert(read(PPU_OAM_START + 1) == 0x66);

    /* VBlank: both. */
    run(144 * LINE - 252);
    assert(mode() == PPU_MODE_VBLANK);
    assert(read(PPU_VRAM_START + 1) == 0x55);
    assert(read(PPU_OAM_START + 1) == 0x66);

    /* DMA writes OAM whatever the mode. */
    run(LINE * 10);
    run(80);
    assert(mode() == PPU_MODE_DRAWING);
    ppu_oam_dma_write(&ppu, 5, 0x77);
    assert(ppu.oam[5] == 0x77);
}

/* Reads and writes of VRAM and OAM at one dot of the current line. */
typedef struct Access {
    bool oam_read, oam_write, vram_read, vram_write;
} Access;

static Access access_now(void)
{
    Access a;

    ppu.vram[0x10] = 0x11;
    ppu.oam[0x10] = 0x22;

    a.vram_read = read(PPU_VRAM_START + 0x10) == 0x11;
    a.oam_read = read(PPU_OAM_START + 0x10) == 0x22;

    ppu_write(&ppu, PPU_VRAM_START + 0x10, 0x33);
    a.vram_write = ppu.vram[0x10] == 0x33;
    ppu_write(&ppu, PPU_OAM_START + 0x10, 0x44);
    a.oam_write = ppu.oam[0x10] == 0x44;

    return a;
}

/*
 * When the CPU can reach VRAM and OAM, dot by dot, from the Mooneye
 * lcdon_timing and lcdon_write_timing tables. On an ordinary line (OAM scan
 * until dot 80, drawing until 252):
 *
 *   OAM reads    blocked from dot 452 of the line before until drawing ends
 *   OAM writes   blocked from dot 0 to 75 and from 80 until drawing ends
 *   VRAM reads   blocked from dot 76 until drawing ends
 *   VRAM writes  blocked from dot 80 until drawing ends
 */
static void test_access_windows(void)
{
    setup();
    run(LINE);   /* line 1, dot 0 */

    Access a = access_now();
    assert(!a.oam_read && !a.oam_write && a.vram_read && a.vram_write);

    run(72);
    a = access_now();
    assert(!a.oam_read && !a.oam_write && a.vram_read && a.vram_write);

    /* The last cycle of the scan: OAM can be written, VRAM cannot be read. */
    run(4);
    assert(ppu.line_dot == 76);
    a = access_now();
    assert(!a.oam_read && a.oam_write && !a.vram_read && a.vram_write);

    run(4);
    a = access_now();
    assert(!a.oam_read && !a.oam_write && !a.vram_read && !a.vram_write);

    run(168);
    assert(ppu.line_dot == 248);
    a = access_now();
    assert(!a.oam_read && !a.oam_write && !a.vram_read && !a.vram_write);

    run(4);
    assert(ppu.line_dot == 252);
    a = access_now();
    assert(a.oam_read && a.oam_write && a.vram_read && a.vram_write);

    run(196);
    assert(ppu.line_dot == 448);
    a = access_now();
    assert(a.oam_read && a.oam_write && a.vram_read && a.vram_write);

    /* OAM reads are blocked 4 dots before the next scan; writes are not. */
    run(4);
    assert(ppu.line_dot == 452);
    a = access_now();
    assert(!a.oam_read && a.oam_write && a.vram_read && a.vram_write);

    /* After the last visible line OAM is not scanned again until the frame
     * restarts, so it stays reachable through VBlank. */
    run(142 * LINE - 452 + 448);
    assert(read(PPU_LY_ADDRESS) == 143);
    a = access_now();
    assert(a.oam_read && a.oam_write);
    run(4);
    a = access_now();
    assert(a.oam_read && a.oam_write && a.vram_read && a.vram_write);
}

/*
 * The first line after the LCD is switched on is different: it starts in
 * mode 0 with no OAM scan, so nothing is blocked until drawing begins.
 */
static void test_first_line_access(void)
{
    setup();
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x11);
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x91);

    run(76);
    Access a = access_now();
    assert(a.oam_read && a.oam_write && a.vram_read && a.vram_write);

    run(4);
    assert(mode() == PPU_MODE_DRAWING);
    a = access_now();
    assert(!a.oam_read && !a.oam_write && !a.vram_read && !a.vram_write);

    run(172);
    assert(mode() == PPU_MODE_HBLANK);
    a = access_now();
    assert(a.oam_read && a.oam_write && a.vram_read && a.vram_write);

    /* Line 1 is an ordinary line again. */
    run(LINE - 252);
    assert(read(PPU_LY_ADDRESS) == 1);
    assert(mode() == PPU_MODE_OAM_SCAN);
    a = access_now();
    assert(!a.oam_read && !a.oam_write && a.vram_read && a.vram_write);
}

static void test_lcd_off_and_on(void)
{
    setup();

    run(3 * LINE + 100);
    ppu.framebuffer[0] = 3;

    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x11); /* LCD enable off */

    /* Rewound, blank and stopped. */
    assert(read(PPU_LY_ADDRESS) == 0);
    assert((read(PPU_STAT_ADDRESS) & 0x03) == PPU_MODE_HBLANK);
    assert(ppu.framebuffer[0] == 0);

    run(10 * LINE);
    assert(read(PPU_LY_ADDRESS) == 0);
    assert(ppu.frames == 0);
    assert(!irq(INTERRUPT_VBLANK));

    /* VRAM and OAM are open while it is off. */
    ppu_write(&ppu, PPU_VRAM_START, 0x12);
    ppu_write(&ppu, PPU_OAM_START, 0x34);
    assert(read(PPU_VRAM_START) == 0x12);
    assert(read(PPU_OAM_START) == 0x34);

    /* Turning it on starts line 0 in mode 0, with no OAM scan, and goes
     * straight to drawing. */
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x91);
    assert(read(PPU_LY_ADDRESS) == 0);
    assert(mode() == PPU_MODE_HBLANK);
    run(76);
    assert(mode() == PPU_MODE_HBLANK);
    run(4);
    assert(mode() == PPU_MODE_DRAWING);
}

/*
 * The LY == LYC flag is latched state: it is recomputed when LY or LYC
 * change while the LCD is on and when the LCD turns on, and it freezes while
 * the LCD is off. The STAT interrupt line also keeps its value across the
 * LCD being off, so turning it on does not look like a new rising edge
 * unless the line really rises.
 */
static void test_lyc_flag_across_lcd_off(void)
{
    /* Off while the flag is set: it is retained and LYC changes are ignored. */
    setup();
    ppu_write(&ppu, PPU_STAT_ADDRESS, PPU_STAT_LYC_INTERRUPT);
    ppu_write(&ppu, PPU_LYC_ADDRESS, 5);
    run(5 * LINE);
    assert((read(PPU_STAT_ADDRESS) & 0x07) == PPU_STAT_LYC_FLAG + PPU_MODE_OAM_SCAN);
    clear_irqs();

    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x11);
    assert((read(PPU_STAT_ADDRESS) & 0x07) == PPU_STAT_LYC_FLAG);

    ppu_write(&ppu, PPU_LYC_ADDRESS, 1);
    assert((read(PPU_STAT_ADDRESS) & 0x07) == PPU_STAT_LYC_FLAG);

    /* Turning it on compares LY = 0 with LYC = 1: the flag drops, and
     * falling is not an interrupt. */
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x91);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);
    assert(!irq(INTERRUPT_LCD_STAT));

    /* The flag stays set across off and on (LY = 0 and LYC = 0 at the end):
     * the line never fell, so there is no new interrupt. */
    setup();
    ppu_write(&ppu, PPU_STAT_ADDRESS, PPU_STAT_LYC_INTERRUPT);
    ppu_write(&ppu, PPU_LYC_ADDRESS, 5);
    run(5 * LINE);
    clear_irqs();
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x11);
    ppu_write(&ppu, PPU_LYC_ADDRESS, 0);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) != 0);
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x91);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) != 0);
    assert(!irq(INTERRUPT_LCD_STAT));

    /* Off with the flag clear, then on with LY = LYC: it rises, and the
     * interrupt is requested. */
    setup();
    ppu_write(&ppu, PPU_LYC_ADDRESS, 5);
    ppu_write(&ppu, PPU_STAT_ADDRESS, PPU_STAT_LYC_INTERRUPT);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);
    clear_irqs();
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x11);
    ppu_write(&ppu, PPU_LYC_ADDRESS, 0);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x91);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) != 0);
    assert(irq(INTERRUPT_LCD_STAT));
}


/*
 * LY moves on at dot 452 but the LY == LYC flag is recomputed only when the
 * new line starts, so it reads 0 for those 4 dots, whatever LYC is.
 */
static void test_lyc_flag_gap_at_line_change(void)
{
    setup();
    ppu_write(&ppu, PPU_LYC_ADDRESS, 0);
    run(LINE - 8);
    assert(read(PPU_LY_ADDRESS) == 0);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) != 0);

    run(4);
    assert(read(PPU_LY_ADDRESS) == 1);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);

    run(4);
    assert(read(PPU_LY_ADDRESS) == 1);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);

    setup();
    ppu_write(&ppu, PPU_LYC_ADDRESS, 1);
    run(LINE - 4);
    assert(read(PPU_LY_ADDRESS) == 1);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) == 0);
    run(4);
    assert((read(PPU_STAT_ADDRESS) & PPU_STAT_LYC_FLAG) != 0);
}

/* With the OAM interrupt enabled, one is also requested when line 144 starts. */
static void test_mode2_interrupt_at_vblank_start(void)
{
    setup();
    ppu_write(&ppu, PPU_STAT_ADDRESS, PPU_STAT_OAM_INTERRUPT);

    run(143 * LINE + 100);
    clear_irqs();
    run(LINE - 112);
    assert(read(PPU_LY_ADDRESS) == 143);
    assert(!irq(INTERRUPT_LCD_STAT));
    assert(!irq(INTERRUPT_VBLANK));

    /* Both arrive together, when the line starts. */
    run(12);
    assert(read(PPU_LY_ADDRESS) == 144);
    assert(irq(INTERRUPT_VBLANK));
    assert(irq(INTERRUPT_LCD_STAT));

    /* Line 145 has no mode 2, so nothing more. */
    clear_irqs();
    run(LINE);
    assert(!irq(INTERRUPT_LCD_STAT));
}

static void put_oam_sprite(unsigned index, uint8_t x)
{
    ppu_write(&ppu, (uint16_t)(PPU_OAM_START + index * 4), 16);   /* line 0 */
    ppu_write(&ppu, (uint16_t)(PPU_OAM_START + index * 4 + 1), x);
}

/* Dot of the first tick at which line 0 is in HBlank. */
static unsigned hblank_start(uint8_t lcdc, uint8_t scx, const uint8_t *sprites,
                             size_t count)
{
    setup();
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x00);
    ppu_write(&ppu, PPU_SCX_ADDRESS, scx);

    for (size_t i = 0; i < count; i++) {
        put_oam_sprite((unsigned)i, sprites[i]);
    }

    ppu_write(&ppu, PPU_LCDC_ADDRESS, lcdc);

    /* Line 0 starts in mode 0; the first HBlank is the one after drawing. */
    while (mode() != PPU_MODE_DRAWING) {
        run(4);
    }

    while (mode() != PPU_MODE_HBLANK) {
        run(4);
        assert(ppu.line_dot < 456);
    }

    return ppu.line_dot;
}

/*
 * Mode 3 lasts 172 dots, plus SCX mod 8, plus, when sprites are on the line,
 * the sum of their penalties minus 3: 6 per sprite with X < 168, and for the
 * first sprite in each 8-pixel column of the picture another max(0, 5 -
 * (X + SCX) mod 8). The figures below are rows of the Mooneye
 * intr_2_mode0_timing_sprites table, worked out by hand.
 */
static void test_mode3_length(void)
{
    enum { OBJ = 0x93, NO_OBJ = 0x91 };

    /* SCX: 0 adds nothing, 1-4 one M-cycle, 5-7 two. */
    assert(hblank_start(NO_OBJ, 0, NULL, 0) == 252);

    for (uint8_t scx = 1; scx <= 4; scx++) {
        assert(hblank_start(NO_OBJ, scx, NULL, 0) == 256);
    }

    for (uint8_t scx = 5; scx <= 7; scx++) {
        assert(hblank_start(NO_OBJ, scx, NULL, 0) == 260);
    }

    /* One sprite: 11 dots at X = 0 down to 6 dots from X = 5, minus 3. */
    static const uint8_t x0[] = { 0 };
    static const uint8_t x3[] = { 3 };
    static const uint8_t x4[] = { 4 };
    static const uint8_t x7[] = { 7 };

    assert(hblank_start(OBJ, 0, x0, 1) == 260);
    assert(hblank_start(OBJ, 0, x3, 1) == 260);
    assert(hblank_start(OBJ, 0, x4, 1) == 256);
    assert(hblank_start(OBJ, 0, x7, 1) == 256);

    /* Ten sprites at the same X share one alignment penalty. */
    static const uint8_t ten_at_0[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t ten_at_2[10] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };

    assert(hblank_start(OBJ, 0, ten_at_0, 10) == 316);
    assert(hblank_start(OBJ, 0, ten_at_2, 10) == 312);

    /* Sprites a tile apart each pay their own. */
    static const uint8_t apart_0[10] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 72 };
    static const uint8_t apart_4[10] = { 4, 12, 20, 28, 36, 44, 52, 60, 68, 76 };
    static const uint8_t two_apart[2] = { 0, 8 };

    assert(hblank_start(OBJ, 0, apart_0, 10) == 360);
    assert(hblank_start(OBJ, 0, apart_4, 10) == 320);
    assert(hblank_start(OBJ, 0, two_apart, 2) == 272);

    /* The alignment uses X + SCX. */
    assert(hblank_start(OBJ, 3, x0, 1) == 260);

    /* Sprites past the right edge cost nothing, and a disabled OBJ layer
     * none either. */
    static const uint8_t off_right[] = { 168 };

    assert(hblank_start(OBJ, 0, off_right, 1) == 252);
    assert(hblank_start(NO_OBJ, 0, x0, 1) == 252);

    /* Only ten sprites are looked at per line. */
    static const uint8_t eleven[11] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    assert(hblank_start(OBJ, 0, eleven, 11) == 316);
}

static void test_register_access(void)
{
    setup();

    /* Only STAT bits 3-6 are writable; bit 7 reads as 1. */
    ppu_write(&ppu, PPU_STAT_ADDRESS, 0xFF);
    assert((read(PPU_STAT_ADDRESS) & 0xF8) == (0x80 | 0x78));
    ppu_write(&ppu, PPU_STAT_ADDRESS, 0x00);
    assert((read(PPU_STAT_ADDRESS) & 0xF8) == 0x80);

    /* LY cannot be written. */
    ppu_write(&ppu, PPU_LY_ADDRESS, 77);
    assert(read(PPU_LY_ADDRESS) == 0);

    static const uint16_t plain[] = {
        PPU_SCY_ADDRESS, PPU_SCX_ADDRESS, PPU_LYC_ADDRESS, PPU_BGP_ADDRESS,
        PPU_OBP0_ADDRESS, PPU_OBP1_ADDRESS, PPU_WY_ADDRESS, PPU_WX_ADDRESS
    };

    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        ppu_write(&ppu, plain[i], (uint8_t)(0x40 + i));
        assert(read(plain[i]) == 0x40 + i);
    }
}

int main(void)
{
    test_reset_state();
    test_line_timing();
    test_frame_timing_and_vblank();
    test_lyc_flag_and_interrupt();
    test_stat_mode_interrupts();
    test_stat_interrupt_blocking();
    test_access_rules();
    test_access_windows();
    test_first_line_access();
    test_lcd_off_and_on();
    test_lyc_flag_across_lcd_off();
    test_lyc_flag_gap_at_line_change();
    test_mode2_interrupt_at_vblank_start();
    test_mode3_length();
    test_register_access();

    printf("PPU timing tests passed!\n");

    return 0;
}
