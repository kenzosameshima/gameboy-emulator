#ifndef PPU_INTERNAL_H
#define PPU_INTERNAL_H

#include <ppu.h>

/*
 * Draws line `ppu->ly` into the framebuffer from the registers, VRAM and
 * OAM as they are now: background, window, then sprites (ppu_render.c).
 * Advances the window's own line counter when the window was drawn.
 */
void ppu_render_scanline(Ppu *ppu);

/*
 * How many dots mode 3 lasts on line `ppu->ly`: 172, plus SCX mod 8, plus, with
 * sprites on the line, the sum of their penalties minus 3. A sprite with X
 * below 168 costs 6 dots, and the first one in each 8-pixel column of the
 * picture costs another max(0, 5 - (X + SCX) mod 8). Only the first ten
 * sprites of the line count, and none do while the OBJ layer is off.
 */
unsigned ppu_drawing_dots(const Ppu *ppu);

#endif
