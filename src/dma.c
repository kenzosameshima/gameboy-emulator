#include <stdbool.h>
#include <stdint.h>

#include <bus.h>
#include <dma.h>
#include <ppu.h>

void dma_init(Dma *dma, Bus *bus, Ppu *ppu)
{
    dma->register_value = 0;
    dma->copying = false;
    dma->active_page = 0;
    dma->copied = 0;
    dma->startup = 0;
    dma->requested_page = 0;
    dma->bus = bus;
    dma->ppu = ppu;
}

uint8_t dma_read(const Dma *dma)
{
    return dma->register_value;
}

void dma_write(Dma *dma, uint8_t page)
{
    dma->register_value = page;
    dma->requested_page = page;
    dma->startup = DMA_STARTUP_CYCLES;
}

bool dma_is_copying(const Dma *dma)
{
    return dma->copying;
}

bool dma_source_is_vram(const Dma *dma)
{
    return dma->active_page >= 0x80 && dma->active_page <= 0x9F;
}

/* One M-cycle. */
static void dma_tick(Dma *dma)
{
    bool starting = false;

    if (dma->startup != 0) {
        dma->startup--;
        starting = dma->startup == 0;
    }

    /* A transfer that is already running carries on until it is replaced. */
    if (dma->copying && !starting) {
        uint16_t source = (uint16_t)((dma->active_page << 8) | dma->copied);

        ppu_oam_dma_write(dma->ppu, dma->copied,
                          bus_dma_read(dma->bus, source));
        dma->copied++;

        if (dma->copied == DMA_BYTES) {
            dma->copying = false;
        }
    }

    if (starting) {
        dma->copying = true;
        dma->active_page = dma->requested_page;
        dma->copied = 0;
    }
}

void dma_step(Dma *dma, CpuCycles cycles)
{
    for (CpuCycles m_cycle = 0; m_cycle < cycles / CYCLES_PER_MCYCLE;
         m_cycle++) {
        dma_tick(dma);
    }
}
