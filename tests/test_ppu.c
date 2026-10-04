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
    run(200);
    assert(mode() == PPU_MODE_HBLANK);
    assert(read(PPU_LY_ADDRESS) == 0);
    run(4);
    assert(read(PPU_LY_ADDRESS) == 1);
    assert(mode() == PPU_MODE_OAM_SCAN);
    assert(ppu.line_dot == 0);
}

static void test_frame_timing_and_vblank(void)
{
    setup();

    run(143 * LINE + 452);
    assert(read(PPU_LY_ADDRESS) == 143);
    assert(!irq(INTERRUPT_VBLANK));
    assert(ppu.frames == 0);

    run(4);
    assert(read(PPU_LY_ADDRESS) == 144);
    assert(mode() == PPU_MODE_VBLANK);
    assert(irq(INTERRUPT_VBLANK));
    assert(ppu.frames == 1);

    /* VBlank lasts 10 lines and covers all of them. */
    clear_irqs();
    run(9 * LINE + 452);
    assert(read(PPU_LY_ADDRESS) == 153);
    assert(mode() == PPU_MODE_VBLANK);

    run(4);
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
    run(5 * LINE - 4);
    assert(!irq(INTERRUPT_LCD_STAT));
    run(4);
    assert(irq(INTERRUPT_LCD_STAT));

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

    /* Turning it on starts line 0 with an OAM scan. */
    ppu_write(&ppu, PPU_LCDC_ADDRESS, 0x91);
    assert(read(PPU_LY_ADDRESS) == 0);
    assert(mode() == PPU_MODE_OAM_SCAN);
    run(80);
    assert(mode() == PPU_MODE_DRAWING);
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
    test_lcd_off_and_on();
    test_register_access();

    printf("PPU timing tests passed!\n");

    return 0;
}
