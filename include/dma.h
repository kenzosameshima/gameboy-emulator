#ifndef DMA_H
#define DMA_H

#include <stdbool.h>
#include <stdint.h>

#include <cycles.h>

typedef struct Bus Bus;
typedef struct Ppu Ppu;

enum {
    DMA_ADDRESS = 0xFF46,
    DMA_BYTES = 160,
    /* M-cycles from the write until the transfer takes the bus. */
    DMA_STARTUP_CYCLES = 2
};

typedef struct Dma {
    uint8_t register_value;   /* last value written to FF46 */

    /* The transfer in progress, which owns its source bus. */
    bool copying;
    uint8_t active_page;
    uint8_t copied;           /* bytes copied so far */

    /* A requested transfer waiting for its start-up cycles to pass. */
    uint8_t startup;          /* 0 when none is waiting */
    uint8_t requested_page;

    Bus *bus;
    Ppu *ppu;
} Dma;

/*
 * OAM DMA. Writing a page number XX to FF46 copies the 160 bytes at XX00
 * into OAM, one per M-cycle. Counting the write as M-cycle 0, the CPU can
 * still use every bus at M = 1, the transfer takes over from M = 2 and
 * copies during M = 2 to M = 161, and everything is free again at M = 162.
 * Sources from E000 up read the work RAM mirror.
 *
 * While it copies, the CPU shares a bus with it: the external bus (ROM,
 * cartridge RAM, work RAM and its echo) when the source is there, or the
 * video bus (VRAM) when the source is VRAM. Reads of the bus it holds give
 * 0xFF and writes are dropped, and OAM is unreachable either way. The I/O
 * registers and high RAM are always free. The Bus enforces this using
 * dma_is_copying() and dma_source_is_vram().
 *
 * A write while a transfer is running does not stop it at once: it keeps
 * the bus through the start-up cycles and the new transfer begins after
 * them.
 */

void dma_init(Dma *dma, Bus *bus, Ppu *ppu);

/* FF46 reads back the last page written. */
uint8_t dma_read(const Dma *dma);
void dma_write(Dma *dma, uint8_t page);

/* True while a transfer holds its source bus and OAM. */
bool dma_is_copying(const Dma *dma);

/* True if the transfer in progress reads from VRAM rather than the
 * external bus. Only meaningful while dma_is_copying(). */
bool dma_source_is_vram(const Dma *dma);

void dma_step(Dma *dma, CpuCycles cycles);

#endif
