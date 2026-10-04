#ifndef PPU_INTERNAL_H
#define PPU_INTERNAL_H

#include <ppu.h>

/*
 * Draws line `ppu->ly` into the framebuffer from the registers, VRAM and
 * OAM as they are now: background, window, then sprites (ppu_render.c).
 * Advances the window's own line counter when the window was drawn.
 */
void ppu_render_scanline(Ppu *ppu);

#endif
