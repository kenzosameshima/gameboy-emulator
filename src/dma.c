#include <stdbool.h>
#include <stdint.h>

#include <bus.h>
#include <dma.h>
#include <ppu.h>

void dma_init(Dma *dma, Bus *bus, Ppu *ppu)
{
    dma->source_page = 0;
    dma->running = false;
    dma->startup = false;
    dma->copied = 0;
    dma->bus = bus;
    dma->ppu = ppu;
}

uint8_t dma_read(const Dma *dma)
{
    return dma->source_page;
}

void dma_write(Dma *dma, uint8_t page)
{
    dma->source_page = page;
    dma->running = true;
    dma->startup = true;
    dma->copied = 0;
}

bool dma_is_copying(const Dma *dma)
{
    return dma->running && !dma->startup;
}

void dma_step(Dma *dma, CpuCycles cycles)
{
    for (CpuCycles m_cycle = 0; m_cycle < cycles / CYCLES_PER_MCYCLE;
         m_cycle++) {
        if (!dma->running) {
            return;
        }

        if (dma->startup) {
            dma->startup = false;
            continue;
        }

        uint16_t source = (uint16_t)((dma->source_page << 8) | dma->copied);

        ppu_oam_dma_write(dma->ppu, dma->copied,
                          bus_dma_read(dma->bus, source));
        dma->copied++;

        if (dma->copied == DMA_BYTES) {
            dma->running = false;
        }
    }
}
