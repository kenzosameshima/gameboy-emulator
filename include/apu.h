#ifndef APU_H
#define APU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cycles.h>

enum {
    APU_REGISTERS_START = 0xFF10,   /* NR10 */
    APU_REGISTERS_END = 0xFF26,     /* NR52 */
    APU_WAVE_RAM_START = 0xFF30,
    APU_WAVE_RAM_END = 0xFF3F,

    APU_NR10_ADDRESS = 0xFF10,
    APU_NR11_ADDRESS = 0xFF11,
    APU_NR12_ADDRESS = 0xFF12,
    APU_NR13_ADDRESS = 0xFF13,
    APU_NR14_ADDRESS = 0xFF14,
    APU_NR21_ADDRESS = 0xFF16,
    APU_NR22_ADDRESS = 0xFF17,
    APU_NR23_ADDRESS = 0xFF18,
    APU_NR24_ADDRESS = 0xFF19,
    APU_NR30_ADDRESS = 0xFF1A,
    APU_NR31_ADDRESS = 0xFF1B,
    APU_NR32_ADDRESS = 0xFF1C,
    APU_NR33_ADDRESS = 0xFF1D,
    APU_NR34_ADDRESS = 0xFF1E,
    APU_NR41_ADDRESS = 0xFF20,
    APU_NR42_ADDRESS = 0xFF21,
    APU_NR43_ADDRESS = 0xFF22,
    APU_NR44_ADDRESS = 0xFF23,
    APU_NR50_ADDRESS = 0xFF24,
    APU_NR51_ADDRESS = 0xFF25,
    APU_NR52_ADDRESS = 0xFF26,

    APU_WAVE_RAM_SIZE = 16,

    /* Room for about 0.17 s of audio at 48 kHz before the oldest is lost. */
    APU_SAMPLE_BUFFER_FRAMES = 8192
};

/* One of the four sound channels. Fields not used by a channel stay 0. */
typedef struct ApuChannel {
    bool enabled;               /* the status bit in NR52 */
    uint16_t length;            /* length counter, counting down */
    int32_t timer;              /* T-cycles until the frequency timer fires */
    uint8_t position;           /* duty step (pulse) or sample index (wave) */
    uint8_t volume;             /* envelope volume, 0-15 */
    uint8_t envelope_timer;     /* frame sequencer steps until it changes */
    uint16_t lfsr;              /* noise generator */
    uint16_t shadow_frequency;  /* channel 1 sweep */
    uint8_t sweep_timer;
    bool sweep_enabled;
    bool sweep_negated;         /* a negate calculation has been done */
    uint8_t wave_buffer;        /* sample (0-15) the wave channel last read */
    bool wave_just_read;        /* it read a sample in the last M-cycle */
} ApuChannel;

typedef struct Apu {
    uint8_t registers[APU_REGISTERS_END - APU_REGISTERS_START + 1];
    uint8_t wave_ram[APU_WAVE_RAM_SIZE];

    bool powered;
    uint16_t divider;           /* the system counter at the last step */
    uint8_t frame_step;        /* the step the next frame sequencer tick runs */

    ApuChannel channel[4];

    /* Sample output. */
    unsigned sample_rate;       /* frames per second; 0 means no output */
    uint32_t cycles_per_sample_x256;
    uint32_t sample_phase_x256;
    int64_t accumulated_left;   /* mixer output times x256 cycles so far */
    int64_t accumulated_right;
    int16_t samples[APU_SAMPLE_BUFFER_FRAMES * 2];
    size_t sample_head;         /* next frame to read */
    size_t sample_count;        /* frames waiting */
} Apu;

/*
 * The audio processing unit: two pulse channels (one with a frequency
 * sweep), a wave channel and a noise channel, each with its own length
 * counter, and envelopes for all but the wave channel, driven by a 512 Hz
 * frame sequencer, and the mixer with NR50 and NR51.
 *
 * Time comes from apu_step() in T-cycles. The frame sequencer is clocked by
 * the falling edge of bit 12 of the system counter (the timer's divider), so
 * apu_step() is given the divider after the step, as serial_step() is.
 *
 * Registers read with their unused bits set, as on hardware. While the APU
 * is off (NR52 bit 7 clear) the registers are cleared and writes to them are
 * ignored, except the length counters, and wave RAM stays accessible.
 *
 * Reset state is the DMG after its boot ROM: the APU on, with the registers
 * the boot ROM's start-up sound leaves behind.
 */

void apu_init(Apu *apu);

/*
 * NR10-NR52 (FF10-FF26) and wave RAM (FF30-FF3F). The unused addresses
 * between them read 0xFF; the Bus does not route them here.
 */
uint8_t apu_read(const Apu *apu, uint16_t address);
void apu_write(Apu *apu, uint16_t address, uint8_t value);

void apu_step(Apu *apu, CpuCycles cycles, uint16_t divider);

/*
 * Audio output. With a sample rate set, the mixer produces one stereo frame
 * every 4194304 / rate T-cycles, averaging the output over that time. The
 * frames are the plain mix of the four channel DACs scaled by NR50, with no
 * filtering: real hardware also removes the DC offset with a high-pass
 * filter, which a front end can apply. A rate of 0, the default, produces
 * nothing and costs nothing.
 *
 * Setting the rate, even to the same value, discards the queued frames.
 * apu_take_samples() copies up to `max_frames` interleaved left, right
 * frames into `out` and returns how many it copied. If the buffer fills up
 * the oldest frames are dropped.
 */
void apu_set_sample_rate(Apu *apu, unsigned rate);
size_t apu_take_samples(Apu *apu, int16_t *out, size_t max_frames);

#endif
