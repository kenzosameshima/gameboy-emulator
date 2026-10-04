#ifndef DMA_H
#define DMA_H

#include <stdbool.h>
#include <stdint.h>

#include <cycles.h>

typedef struct Bus Bus;
typedef struct Ppu Ppu;

enum {
    DMA_ADDRESS = 0xFF46,
    DMA_BYTES = 160
};

typedef struct Dma {
    uint8_t source_page;     /* last value written to FF46 */
    bool running;
    bool startup;            /* the M-cycle after the write, before the bus is taken */
    uint8_t copied;          /* bytes copied so far */

    Bus *bus;
    Ppu *ppu;
} Dma;

/*
 * OAM DMA. Writing a page number XX to FF46 copies the 160 bytes at XX00
 * into OAM, one per M-cycle, starting one M-cycle after the write. While
 * it copies it owns the bus the CPU uses for ROM, RAM and OAM, so the CPU
 * reaches only high RAM and the I/O registers (the Bus enforces that
 * through dma_is_copying()). Sources from E000 up read the work RAM mirror.
 * A new write while copying restarts the transfer.
 */

void dma_init(Dma *dma, Bus *bus, Ppu *ppu);

/* FF46 reads back the last page written. */
uint8_t dma_read(const Dma *dma);
void dma_write(Dma *dma, uint8_t page);

/* True while the transfer has the CPU's bus. */
bool dma_is_copying(const Dma *dma);

void dma_step(Dma *dma, CpuCycles cycles);

#endif
