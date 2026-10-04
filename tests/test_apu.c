/*
 * The APU through the apu.h interface: registers and their unused bits,
 * power, status, the frame sequencer's length counters, and the mixed
 * output. Expected values come from documented hardware behaviour: the
 * register read masks, the duty patterns, the frame sequencer schedule
 * (length at steps 0, 2, 4 and 6, sweep at 2 and 6, envelope at 7) and the
 * DAC scale.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <apu.h>

enum {
    /* One output frame is 16 T-cycles, an exact 4 M-cycles. */
    SAMPLE_RATE = 262144,
    CYCLES_PER_SAMPLE = 16,

    /* The frame sequencer ticks every 8192 T-cycles of the system counter. */
    FRAME_TICK = 8192
};

static Apu apu;
static uint16_t divider;

/* The system counter the frame sequencer is clocked from. */
static void run(unsigned cycles)
{
    assert(cycles % 4 == 0);

    for (unsigned done = 0; done < cycles; done += 4) {
        divider = (uint16_t)(divider + 4);
        apu_step(&apu, 4, divider);
    }
}

static uint8_t read(uint16_t address)
{
    return apu_read(&apu, address);
}

static void write(uint16_t address, uint8_t value)
{
    apu_write(&apu, address, value);
}

static bool channel_on(unsigned channel)
{
    return (read(APU_NR52_ADDRESS) & (1u << channel)) != 0;
}

/* A powered-off and on APU with every register at 0 and the frame
 * sequencer at step 0, and a divider that starts a tick from zero. */
static void fresh(void)
{
    apu_init(&apu);
    divider = 0;
    write(APU_NR52_ADDRESS, 0x00);
    write(APU_NR52_ADDRESS, 0x80);
}

static void test_registers_after_the_boot_rom(void)
{
    apu_init(&apu);

    static const struct {
        uint16_t address;
        uint8_t value;
    } boot[] = {
        { APU_NR10_ADDRESS, 0x80 }, { APU_NR11_ADDRESS, 0xBF },
        { APU_NR12_ADDRESS, 0xF3 }, { APU_NR13_ADDRESS, 0xFF },
        { APU_NR14_ADDRESS, 0xBF }, { APU_NR21_ADDRESS, 0x3F },
        { APU_NR22_ADDRESS, 0x00 }, { APU_NR23_ADDRESS, 0xFF },
        { APU_NR24_ADDRESS, 0xBF }, { APU_NR30_ADDRESS, 0x7F },
        { APU_NR31_ADDRESS, 0xFF }, { APU_NR32_ADDRESS, 0x9F },
        { APU_NR33_ADDRESS, 0xFF }, { APU_NR34_ADDRESS, 0xBF },
        { APU_NR41_ADDRESS, 0xFF }, { APU_NR42_ADDRESS, 0x00 },
        { APU_NR43_ADDRESS, 0x00 }, { APU_NR44_ADDRESS, 0xBF },
        { APU_NR50_ADDRESS, 0x77 }, { APU_NR51_ADDRESS, 0xF3 },
        /* On, with channel 1 still playing the boot sound. */
        { APU_NR52_ADDRESS, 0xF1 },
        /* The unused addresses read as ones. */
        { 0xFF15, 0xFF }, { 0xFF1F, 0xFF }, { 0xFF27, 0xFF }, { 0xFF2F, 0xFF }
    };

    for (size_t i = 0; i < sizeof(boot) / sizeof(boot[0]); i++) {
        if (read(boot[i].address) != boot[i].value) {
            fprintf(stderr, "%04X: got %02X, want %02X\n", boot[i].address,
                    read(boot[i].address), boot[i].value);
        }

        assert(read(boot[i].address) == boot[i].value);
    }
}

/* Unused bits read as 1, per register. */
static void test_read_masks(void)
{
    static const struct {
        uint16_t address;
        uint8_t mask;
    } masks[] = {
        { APU_NR10_ADDRESS, 0x80 }, { APU_NR11_ADDRESS, 0x3F },
        { APU_NR12_ADDRESS, 0x00 }, { APU_NR13_ADDRESS, 0xFF },
        { APU_NR14_ADDRESS, 0xBF }, { APU_NR21_ADDRESS, 0x3F },
        { APU_NR22_ADDRESS, 0x00 }, { APU_NR23_ADDRESS, 0xFF },
        { APU_NR24_ADDRESS, 0xBF }, { APU_NR30_ADDRESS, 0x7F },
        { APU_NR31_ADDRESS, 0xFF }, { APU_NR32_ADDRESS, 0x9F },
        { APU_NR33_ADDRESS, 0xFF }, { APU_NR34_ADDRESS, 0xBF },
        { APU_NR41_ADDRESS, 0xFF }, { APU_NR42_ADDRESS, 0x00 },
        { APU_NR43_ADDRESS, 0x00 }, { APU_NR44_ADDRESS, 0xBF },
        { APU_NR50_ADDRESS, 0x00 }, { APU_NR51_ADDRESS, 0x00 }
    };

    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        /* Zeros: only the mask shows. */
        fresh();
        write(masks[i].address, 0x00);
        assert(read(masks[i].address) == masks[i].mask);

        /* Ones: write-only bits of NRx4 and the trigger do not read back,
         * but everything else does. */
        fresh();
        write(masks[i].address, 0xFF);
        assert(read(masks[i].address) == 0xFF);
    }

    /* A value in between shows the written bits and the mask. */
    fresh();
    write(APU_NR11_ADDRESS, 0x85);
    assert(read(APU_NR11_ADDRESS) == (0x80 | 0x3F));
    write(APU_NR12_ADDRESS, 0x5A);
    assert(read(APU_NR12_ADDRESS) == 0x5A);
    write(APU_NR50_ADDRESS, 0x3C);
    assert(read(APU_NR50_ADDRESS) == 0x3C);

    /* NR52 reads bit 7 and the status bits, with bits 4-6 set. */
    fresh();
    assert(read(APU_NR52_ADDRESS) == (0x80 | 0x70));
}

static void test_power(void)
{
    fresh();
    write(APU_NR50_ADDRESS, 0x55);
    write(APU_NR51_ADDRESS, 0xAA);
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR14_ADDRESS, 0x80);
    assert(channel_on(0));

    /* Writing NR52 bit 7 clear powers it off: the registers clear, the
     * channels stop, and only bit 7 and the unused bits read. */
    write(APU_NR52_ADDRESS, 0x00);
    assert(read(APU_NR52_ADDRESS) == 0x70);
    assert(read(APU_NR50_ADDRESS) == 0x00);
    assert(read(APU_NR51_ADDRESS) == 0x00);
    assert(read(APU_NR12_ADDRESS) == 0x00);
    assert(!channel_on(0));

    /* While off, writes to the registers are ignored. */
    write(APU_NR50_ADDRESS, 0x77);
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR14_ADDRESS, 0x80);
    assert(read(APU_NR50_ADDRESS) == 0x00);
    assert(read(APU_NR12_ADDRESS) == 0x00);
    assert(!channel_on(0));

    /* Writing the status bits of NR52 changes nothing but power. */
    write(APU_NR52_ADDRESS, 0x0F);
    assert(read(APU_NR52_ADDRESS) == 0x70);

    /* Back on, everything starts from zero. */
    write(APU_NR52_ADDRESS, 0x80);
    assert(read(APU_NR52_ADDRESS) == 0xF0);
    assert(read(APU_NR50_ADDRESS) == 0x00);
    write(APU_NR50_ADDRESS, 0x77);
    assert(read(APU_NR50_ADDRESS) == 0x77);
}

static void test_wave_ram(void)
{
    fresh();

    /* With the wave channel idle, wave RAM reads and writes like memory. */
    for (unsigned i = 0; i < APU_WAVE_RAM_SIZE; i++) {
        write((uint16_t)(APU_WAVE_RAM_START + i), (uint8_t)(0x10 * i + i));
    }

    for (unsigned i = 0; i < APU_WAVE_RAM_SIZE; i++) {
        assert(read((uint16_t)(APU_WAVE_RAM_START + i)) ==
               (uint8_t)(0x10 * i + i));
    }

    /* It survives powering off, and is writable while off. */
    write(APU_NR52_ADDRESS, 0x00);
    assert(read(APU_WAVE_RAM_START + 3) == 0x33);
    write(APU_WAVE_RAM_START + 3, 0xAB);
    assert(read(APU_WAVE_RAM_START + 3) == 0xAB);
}

/* A channel plays only with its DAC on, and triggering sets its status bit. */
static void test_status_and_dac(void)
{
    fresh();

    /* Triggering with the DAC off (NRx2 top five bits clear) does nothing. */
    write(APU_NR22_ADDRESS, 0x00);
    write(APU_NR24_ADDRESS, 0x80);
    assert(!channel_on(1));

    write(APU_NR22_ADDRESS, 0x08);   /* volume 0, increasing: DAC on */
    write(APU_NR24_ADDRESS, 0x80);
    assert(channel_on(1));

    /* Switching the DAC off stops the channel at once. */
    write(APU_NR22_ADDRESS, 0x07);
    assert(!channel_on(1));

    /* The same for each channel. */
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR14_ADDRESS, 0x80);
    write(APU_NR30_ADDRESS, 0x80);
    write(APU_NR34_ADDRESS, 0x80);
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR44_ADDRESS, 0x80);
    assert(read(APU_NR52_ADDRESS) == (0x80 | 0x70 | 0x0D));

    write(APU_NR30_ADDRESS, 0x00);
    assert(!channel_on(2));
}

/*
 * Length counters tick on frame sequencer steps 0, 2, 4 and 6, and a channel
 * with length enabled stops when its counter reaches 0. The first tick comes
 * when the system counter first falls through bit 12, 8192 cycles in.
 */
static void test_length_counter(void)
{
    fresh();
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR21_ADDRESS, 0x3E);   /* 64 - 62: two ticks */
    write(APU_NR24_ADDRESS, 0xC0);   /* trigger, length enabled */
    assert(channel_on(1));

    run(FRAME_TICK - 4);
    assert(channel_on(1));
    run(4);                          /* step 0: length 2 -> 1 */
    assert(channel_on(1));
    run(FRAME_TICK);                 /* step 1: no length tick */
    assert(channel_on(1));
    run(FRAME_TICK);                 /* step 2: length 1 -> 0, channel off */
    assert(!channel_on(1));

    /* Without length enabled it plays on. */
    fresh();
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR21_ADDRESS, 0x3F);   /* length 1 */
    write(APU_NR24_ADDRESS, 0x80);
    run(8 * FRAME_TICK);
    assert(channel_on(1));

    /* The wave channel counts 256, the others 64. */
    fresh();
    write(APU_NR30_ADDRESS, 0x80);
    write(APU_NR31_ADDRESS, 0xFE);   /* 256 - 254 = 2 */
    write(APU_NR34_ADDRESS, 0xC0);
    run(FRAME_TICK);                 /* step 0 */
    assert(channel_on(2));
    run(2 * FRAME_TICK);             /* steps 1 and 2 */
    assert(!channel_on(2));

    fresh();
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR41_ADDRESS, 0x3F);
    write(APU_NR44_ADDRESS, 0xC0);
    run(FRAME_TICK);
    assert(!channel_on(3));

    /* A counter of 0 reloads with the full length on a trigger: 64 ticks,
     * which is 64 length steps = 128 sequencer steps, so still playing
     * after 100 steps and off after 127. */
    fresh();
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR21_ADDRESS, 0x00);
    write(APU_NR24_ADDRESS, 0xC0);
    run(100 * FRAME_TICK);
    assert(channel_on(1));
    run(27 * FRAME_TICK);
    assert(!channel_on(1));
}

/* An initial frequency that overflows once swept stops channel 1 at once. */
static void test_sweep_overflow_on_trigger(void)
{
    fresh();
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR10_ADDRESS, 0x01);   /* shift 1, adding */
    write(APU_NR13_ADDRESS, 0xFF);
    write(APU_NR14_ADDRESS, 0x87);   /* frequency 0x7FF, trigger */
    assert(!channel_on(0));

    /* A lower frequency is fine: 0x3FF + 0x1FF is within 2047. */
    fresh();
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR10_ADDRESS, 0x01);
    write(APU_NR13_ADDRESS, 0xFF);
    write(APU_NR14_ADDRESS, 0x83);
    assert(channel_on(0));

    /* With the shift at 0 there is no calculation, so no overflow check. */
    fresh();
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR10_ADDRESS, 0x10);   /* period 1, shift 0 */
    write(APU_NR13_ADDRESS, 0xFF);
    write(APU_NR14_ADDRESS, 0x87);
    assert(channel_on(0));
}

/* Collects `frames` stereo frames, running the APU for them. */
static void collect(int16_t *left, int16_t *right, size_t frames)
{
    run((unsigned)(frames * CYCLES_PER_SAMPLE));

    int16_t buffer[2 * 2048];

    assert(frames <= 2048);
    assert(apu_take_samples(&apu, buffer, frames) == frames);

    for (size_t i = 0; i < frames; i++) {
        left[i] = buffer[2 * i];
        right[i] = buffer[2 * i + 1];
    }
}

static bool near(int actual, int expected)
{
    return abs(actual - expected) <= 4;
}

static int mean(const int16_t *samples, size_t count)
{
    long total = 0;

    for (size_t i = 0; i < count; i++) {
        total += samples[i];
    }

    return (int)(total / (long)count);
}

/*
 * A channel's output is its DAC: digital 0-15 maps to -1..+1, and the mix of
 * the four channels is divided by 4 and scaled by NR50, then by 32767. So a
 * single channel at digital 15 on both sides at full volume is +8191, and at
 * digital 0 it is -8191.
 */
static void test_pulse_output(void)
{
    static int16_t left[2048];
    static int16_t right[2048];

    fresh();
    apu_set_sample_rate(&apu, SAMPLE_RATE);
    write(APU_NR50_ADDRESS, 0x77);
    write(APU_NR51_ADDRESS, 0x22);   /* channel 2, both sides */
    write(APU_NR21_ADDRESS, 0x80);   /* duty 2: 10000111, 50% */
    write(APU_NR22_ADDRESS, 0xF0);   /* volume 15, no envelope */
    write(APU_NR23_ADDRESS, 0xC0);
    write(APU_NR24_ADDRESS, 0x87);   /* frequency 0x7C0: 256 cycles a step */

    /* Skip the first cycle, then look at the runs: one duty cycle is 2048
     * cycles = 128 frames, high for 64 (3 steps + 1 step wrapping round)
     * and low for 64. */
    collect(left, right, 256);
    collect(left, right, 512);

    unsigned run_length = 0;
    int level = 0;
    unsigned high_runs = 0;
    unsigned low_runs = 0;

    for (size_t i = 0; i < 512; i++) {
        int now = left[i] > 0 ? 1 : -1;

        if (now == level) {
            run_length++;
            continue;
        }

        /* A complete run between two transitions. */
        if (level != 0 && i > 128) {
            assert(run_length == 64);

            if (level > 0) {
                high_runs++;
            } else {
                low_runs++;
            }
        }

        level = now;
        run_length = 1;
    }

    assert(high_runs >= 2 && low_runs >= 2);

    /* The two levels, and the same on the right. */
    int high = 0;
    int low = 0;

    for (size_t i = 0; i < 512; i++) {
        assert(left[i] == right[i]);

        if (left[i] > 0) {
            high = left[i];
        } else {
            low = left[i];
        }
    }

    assert(near(high, 8191));
    assert(near(low, -8191));

    /* Panning: channel 2 on the left only. */
    write(APU_NR51_ADDRESS, 0x20);
    collect(left, right, 256);
    for (size_t i = 0; i < 256; i++) {
        assert(right[i] == 0);
        assert(near(abs(left[i]), 8191));
    }

    /* NR50: left volume 3 is 4/8, right volume 1 is 2/8. */
    write(APU_NR51_ADDRESS, 0x22);
    write(APU_NR50_ADDRESS, 0x31);
    collect(left, right, 256);
    for (size_t i = 0; i < 256; i++) {
        assert(near(abs(left[i]), 8191 / 2));
        assert(near(abs(right[i]), 8191 / 4));
    }

    /* Volume 8: digital 8 is (8 / 7.5 - 1) = +0.0667 of 8191, so 546. */
    write(APU_NR50_ADDRESS, 0x77);
    write(APU_NR22_ADDRESS, 0x80);
    write(APU_NR24_ADDRESS, 0x87);
    collect(left, right, 512);
    int volume_8 = 0;

    for (size_t i = 0; i < 512; i++) {
        if (left[i] > volume_8) {
            volume_8 = left[i];
        }
    }

    assert(near(volume_8, 546));
}

/* The envelope changes the volume once every `period` 64 Hz steps. */
static void test_envelope(void)
{
    static int16_t left[2048];
    static int16_t right[2048];

    fresh();
    apu_set_sample_rate(&apu, SAMPLE_RATE);
    write(APU_NR50_ADDRESS, 0x77);
    write(APU_NR51_ADDRESS, 0x22);
    write(APU_NR21_ADDRESS, 0x80);
    write(APU_NR22_ADDRESS, 0xF1);   /* volume 15, decreasing, period 1 */
    write(APU_NR23_ADDRESS, 0xC0);
    write(APU_NR24_ADDRESS, 0x87);

    /* The envelope is clocked on step 7, the 8th tick of the sequencer, at
     * 8 * 8192 cycles. Look at the 4096 cycles before it and after it. */
    run(7 * FRAME_TICK + FRAME_TICK / 2);
    apu_set_sample_rate(&apu, SAMPLE_RATE);   /* drop what has queued */
    collect(left, right, 256);

    int before = 0;

    for (size_t i = 0; i < 256; i++) {
        if (left[i] > before) {
            before = left[i];
        }
    }

    assert(near(before, 8191));

    run(4096);
    collect(left, right, 256);

    int after = 0;

    for (size_t i = 0; i < 256; i++) {
        if (left[i] > after) {
            after = left[i];
        }
    }

    /* Digital 14: (14 / 7.5 - 1) / 4 * 32767 = 7099. */
    assert(near(after, 7099));
}

/* With 15-bit width and the shortest period, the LFSR shifts every 8 cycles. */
static void test_noise_lfsr(void)
{
    fresh();
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR43_ADDRESS, 0x00);   /* divisor 8, shift 0, 15 bits */
    write(APU_NR44_ADDRESS, 0x80);
    assert(channel_on(3));

    /* A trigger fills the register with ones. Each shift moves it right
     * and feeds bit 0 XOR bit 1 into bit 14, so the ones drain out and
     * after 15 shifts it is 0x4000. */
    assert(apu.channel[3].lfsr == 0x7FFF);

    run(8 * 15);
    assert(apu.channel[3].lfsr == 0x4000);

    run(8);
    assert(apu.channel[3].lfsr == 0x2000);

    /* The 7-bit width also copies the new bit into bit 6. */
    fresh();
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR43_ADDRESS, 0x08);
    write(APU_NR44_ADDRESS, 0x80);
    run(8 * 7);
    assert((apu.channel[3].lfsr & 0x7F) == 0x40);
}

static void test_wave_output(void)
{
    static int16_t left[2048];
    static int16_t right[2048];

    fresh();
    apu_set_sample_rate(&apu, SAMPLE_RATE);

    /* Sample 0 is 15 and the other 31 are 0. */
    write(APU_WAVE_RAM_START, 0xF0);
    for (unsigned i = 1; i < APU_WAVE_RAM_SIZE; i++) {
        write((uint16_t)(APU_WAVE_RAM_START + i), 0x00);
    }

    write(APU_NR50_ADDRESS, 0x77);
    write(APU_NR51_ADDRESS, 0x44);   /* channel 3, both sides */
    write(APU_NR30_ADDRESS, 0x80);
    write(APU_NR32_ADDRESS, 0x20);   /* 100% */
    write(APU_NR33_ADDRESS, 0x80);
    write(APU_NR34_ADDRESS, 0x87);   /* frequency 0x780: 256 cycles a sample */

    /*
     * Over two whole periods (1024 frames) the mean does not depend on
     * where the window starts. One step in 32 at the sample's level and 31
     * at digital 0 (-8191.75): 100% is (8191.75 - 31 * 8191.75) / 32.
     */
    collect(left, right, 1024);
    collect(left, right, 1024);
    assert(near(mean(left, 1024), -7680));

    /* 50%: digital 7 is -1/15 of full scale, -546. 25%: digital 3 is -9/15,
     * -4915. */
    write(APU_NR32_ADDRESS, 0x40);
    write(APU_NR34_ADDRESS, 0x87);
    collect(left, right, 1024);
    collect(left, right, 1024);
    assert(near(mean(left, 1024), -7953));

    write(APU_NR32_ADDRESS, 0x60);
    write(APU_NR34_ADDRESS, 0x87);
    collect(left, right, 1024);
    collect(left, right, 1024);
    assert(near(mean(left, 1024), -8089));

    /* Volume code 0 mutes it: digital 0 throughout. */
    write(APU_NR32_ADDRESS, 0x00);
    write(APU_NR34_ADDRESS, 0x87);
    collect(left, right, 1024);
    collect(left, right, 1024);

    for (size_t i = 0; i < 1024; i++) {
        assert(near(left[i], -8191));
    }
}

static void test_sample_buffer(void)
{
    /* No rate, no samples. */
    fresh();
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR14_ADDRESS, 0x80);
    run(10000);

    int16_t out[16];

    assert(apu_take_samples(&apu, out, 8) == 0);

    /* With a rate, a frame every 16 cycles. */
    apu_set_sample_rate(&apu, SAMPLE_RATE);
    run(16 * 10);
    assert(apu_take_samples(&apu, out, 4) == 4);
    assert(apu_take_samples(&apu, out, 8) == 6);
    assert(apu_take_samples(&apu, out, 8) == 0);

    /* Turning it off clears what is queued. */
    run(16 * 5);
    apu_set_sample_rate(&apu, 0);
    assert(apu_take_samples(&apu, out, 8) == 0);

    /* A full buffer drops the oldest frames. */
    apu_set_sample_rate(&apu, SAMPLE_RATE);
    run(16 * (APU_SAMPLE_BUFFER_FRAMES + 100));
    assert(apu.sample_count == APU_SAMPLE_BUFFER_FRAMES);

    /* A rate that is not a divisor of the clock still averages evenly:
     * 48000 Hz is 87.38 cycles a frame, so 1 second gives 48000 frames
     * give or take one. */
    apu_set_sample_rate(&apu, 48000);
    size_t produced = 0;

    for (unsigned i = 0; i < 4194304 / 4096; i++) {
        run(4096);
        int16_t scratch[128];
        produced += apu_take_samples(&apu, scratch, 128);
    }

    assert(produced >= 48000 - 2 && produced <= 48000 + 2);
}

/*
 * The sweep runs on steps 2 and 6: it calculates the next frequency, applies
 * it, and calculates once more to check that one for overflow.
 */
static void test_sweep_runs_on_steps_2_and_6(void)
{
    /* 0x500 -> 0x780 at step 2, and 0x780 + 0x3C0 overflows right then. */
    fresh();
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR10_ADDRESS, 0x11);   /* period 1, adding, shift 1 */
    write(APU_NR13_ADDRESS, 0x00);
    write(APU_NR14_ADDRESS, 0x85);
    assert(channel_on(0));           /* 0x500 + 0x280 = 0x780 fits */
    run(2 * FRAME_TICK);             /* steps 0 and 1 */
    assert(channel_on(0));
    run(FRAME_TICK);                 /* step 2 */
    assert(!channel_on(0));

    /* 0x300 -> 0x480 at step 2 -> 0x6C0 at step 6, and 0x6C0 + 0x360
     * overflows then. */
    fresh();
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR10_ADDRESS, 0x11);
    write(APU_NR13_ADDRESS, 0x00);
    write(APU_NR14_ADDRESS, 0x83);
    run(3 * FRAME_TICK);             /* steps 0, 1 and 2 */
    assert(channel_on(0));
    run(3 * FRAME_TICK);             /* steps 3, 4 and 5 */
    assert(channel_on(0));
    run(FRAME_TICK);                 /* step 6 */
    assert(!channel_on(0));
}

/*
 * A counter that ran out reloads on a trigger: 256 for the wave channel. The
 * next step (1) does not clock lengths, so it comes up one short.
 */
static void test_length_reload_on_trigger(void)
{
    fresh();
    write(APU_NR30_ADDRESS, 0x80);
    write(APU_NR31_ADDRESS, 0xFF);   /* length 1 */
    write(APU_NR34_ADDRESS, 0xC0);
    run(FRAME_TICK);                 /* step 0 clocks it to 0 */
    assert(!channel_on(2));

    write(APU_NR34_ADDRESS, 0xC0);   /* length 255; ticks at steps 2, 4... */
    run(509 * FRAME_TICK);           /* steps 1 to 509: 254 length ticks */
    assert(channel_on(2));
    run(FRAME_TICK);                 /* step 510 is the 255th */
    assert(!channel_on(2));
}

static void test_length_counters_survive_power_off(void)
{
    /* Set before powering off. */
    fresh();
    write(APU_NR21_ADDRESS, 0x3E);   /* length 2 */
    write(APU_NR52_ADDRESS, 0x00);
    write(APU_NR52_ADDRESS, 0x80);
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR24_ADDRESS, 0xC0);
    run(FRAME_TICK);                 /* step 0 */
    assert(channel_on(1));
    run(2 * FRAME_TICK);             /* steps 1 and 2 */
    assert(!channel_on(1));

    /* Loaded while off. */
    fresh();
    write(APU_NR52_ADDRESS, 0x00);
    write(APU_NR31_ADDRESS, 0xFE);   /* length 2 */
    write(APU_NR52_ADDRESS, 0x80);
    write(APU_NR30_ADDRESS, 0x80);
    write(APU_NR34_ADDRESS, 0xC0);
    run(FRAME_TICK);
    assert(channel_on(2));
    run(2 * FRAME_TICK);
    assert(!channel_on(2));
}

/* The frame sequencer follows the system counter, so writing DIV (which sets
 * it to 0) clocks it if bit 12 was set. */
static void test_divider_reset_clocks_the_sequencer(void)
{
    fresh();
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR21_ADDRESS, 0x3F);   /* length 1 */
    write(APU_NR24_ADDRESS, 0xC0);
    run(0x1000);                     /* bit 12 now set, no edge yet */
    assert(channel_on(1));

    /* DIV was written: the counter drops and bit 12 falls. Any drop that
     * clears bit 12 does it, not only one to 0. */
    divider = 0x0100;
    apu_step(&apu, 4, divider);
    assert(!channel_on(1));
}

static void test_noise_period(void)
{
    /* Divisor code 1 is 16, shifted left by 1: 32 cycles. */
    fresh();
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR43_ADDRESS, 0x11);
    write(APU_NR44_ADDRESS, 0x80);
    run(28);
    assert(apu.channel[3].lfsr == 0x7FFF);
    run(4);
    assert(apu.channel[3].lfsr == 0x3FFF);

    /* Divisor code 7 is 112. */
    fresh();
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR43_ADDRESS, 0x07);
    write(APU_NR44_ADDRESS, 0x80);
    run(108);
    assert(apu.channel[3].lfsr == 0x7FFF);
    run(4);
    assert(apu.channel[3].lfsr == 0x3FFF);

    /* Shifts of 14 and 15 stop the generator. */
    fresh();
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR43_ADDRESS, 0xE0);   /* 8 << 14 = 131072 cycles */
    write(APU_NR44_ADDRESS, 0x80);
    run(140000);
    assert(apu.channel[3].lfsr == 0x7FFF);

    fresh();
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR43_ADDRESS, 0xF0);
    write(APU_NR44_ADDRESS, 0x80);
    run(270000);
    assert(apu.channel[3].lfsr == 0x7FFF);

    /* Shift 13 still runs: 8 << 13 = 65536 cycles. */
    fresh();
    write(APU_NR42_ADDRESS, 0xF0);
    write(APU_NR43_ADDRESS, 0xD0);
    write(APU_NR44_ADDRESS, 0x80);
    run(70000);
    assert(apu.channel[3].lfsr == 0x3FFF);
}

/* While the wave channel plays, the CPU can only get at wave RAM in the
 * cycle the channel fetches a sample. */
static void test_wave_ram_while_playing(void)
{
    fresh();
    write(APU_WAVE_RAM_START, 0x12);
    write(APU_NR30_ADDRESS, 0x80);
    write(APU_NR33_ADDRESS, 0x00);
    write(APU_NR34_ADDRESS, 0x80);   /* 4096 cycles a sample: no fetch yet */
    assert(channel_on(2));

    assert(read(APU_WAVE_RAM_START) == 0xFF);
    write(APU_WAVE_RAM_START, 0x55);

    write(APU_NR30_ADDRESS, 0x00);   /* DAC off stops it */
    assert(read(APU_WAVE_RAM_START) == 0x12);
}

/* A full buffer drops the oldest frames and keeps the order of the rest. */
static void test_full_buffer_keeps_the_newest(void)
{
    static int16_t out[2 * 4096];

    fresh();
    apu_set_sample_rate(&apu, SAMPLE_RATE);
    write(APU_NR22_ADDRESS, 0x08);   /* DAC on, nothing playing: digital 0 */
    write(APU_NR51_ADDRESS, 0x22);

    /* Quiet at 1/8 volume, then at full volume. 10000 frames in all, so
     * 1808 are lost and 3192 quiet ones remain. */
    write(APU_NR50_ADDRESS, 0x00);
    run(5000 * CYCLES_PER_SAMPLE);
    write(APU_NR50_ADDRESS, 0x77);
    run(5000 * CYCLES_PER_SAMPLE);
    assert(apu.sample_count == APU_SAMPLE_BUFFER_FRAMES);

    assert(apu_take_samples(&apu, out, 4096) == 4096);
    assert(near(out[0], -1023));
    assert(near(out[2 * 3191], -1023));
    assert(near(out[2 * 3192], -8191));

    assert(apu_take_samples(&apu, out, 4096) == 4096);
    assert(near(out[2 * 4095], -8191));
    assert(apu_take_samples(&apu, out, 4096) == 0);
}

/* Enabling a length counter when the next step does not clock lengths clocks
 * it once at once. */
static void test_length_enable_extra_clock(void)
{
    fresh();
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR21_ADDRESS, 0x3F);   /* length 1 */
    write(APU_NR24_ADDRESS, 0x80);   /* trigger, length not enabled */
    run(FRAME_TICK);                 /* step 0; the next, 1, does not clock */
    assert(channel_on(1));

    write(APU_NR24_ADDRESS, 0x40);
    assert(!channel_on(1));

    /* Before a step that does clock lengths there is no extra clock. */
    fresh();
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR21_ADDRESS, 0x3F);
    write(APU_NR24_ADDRESS, 0x80);
    write(APU_NR24_ADDRESS, 0x40);
    assert(channel_on(1));
}

/* An envelope period of 0 holds the volume. */
static void test_envelope_period_0_holds(void)
{
    static int16_t left[2048];
    static int16_t right[2048];

    fresh();
    apu_set_sample_rate(&apu, SAMPLE_RATE);
    write(APU_NR50_ADDRESS, 0x77);
    write(APU_NR51_ADDRESS, 0x22);
    write(APU_NR21_ADDRESS, 0x80);
    write(APU_NR22_ADDRESS, 0xF0);   /* volume 15, period 0 */
    write(APU_NR23_ADDRESS, 0xC0);
    write(APU_NR24_ADDRESS, 0x87);
    run(70 * FRAME_TICK);            /* past eight envelope clocks */
    apu_set_sample_rate(&apu, SAMPLE_RATE);
    collect(left, right, 256);

    int peak = 0;

    for (size_t i = 0; i < 256; i++) {
        if (left[i] > peak) {
            peak = left[i];
        }
    }

    assert(near(peak, 8191));
}

/* Negate mode subtracts, so 0x7FF does not overflow, and clearing the bit
 * afterwards stops the channel. */
static void test_sweep_negate(void)
{
    fresh();
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR10_ADDRESS, 0x19);   /* period 1, subtracting, shift 1 */
    write(APU_NR13_ADDRESS, 0xFF);
    write(APU_NR14_ADDRESS, 0x87);   /* 0x7FF - 0x3FF */
    assert(channel_on(0));

    write(APU_NR10_ADDRESS, 0x11);   /* back to adding after a negate */
    assert(!channel_on(0));

    /* Without a negated calculation there is nothing to stop. */
    fresh();
    write(APU_NR12_ADDRESS, 0xF0);
    write(APU_NR10_ADDRESS, 0x08);   /* shift 0: no calculation */
    write(APU_NR14_ADDRESS, 0x80);
    write(APU_NR10_ADDRESS, 0x00);
    assert(channel_on(0));
}

/*
 * On the DMG, triggering the wave channel as it is about to fetch a sample
 * damages wave RAM: if the byte it was about to read is one of the first
 * four it replaces byte 0, and otherwise its group of four replaces the
 * first four.
 */
static void test_retrigger_corrupts_wave_ram(void)
{
    static const struct {
        uint8_t position;
        int32_t timer;
        bool corrupted;
        uint8_t expected[4];
    } cases[] = {
        /* The byte after sample 5 is byte 3. */
        { 5, 2, true, { 0x33, 0x11, 0x22, 0x33 } },
        /* Sample 17 is followed by byte 9, in the group of bytes 8-11. */
        { 17, 2, true, { 0x88, 0x99, 0xAA, 0xBB } },
        /* Not about to fetch. */
        { 5, 3, false, { 0x00, 0x11, 0x22, 0x33 } }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fresh();

        for (unsigned byte = 0; byte < APU_WAVE_RAM_SIZE; byte++) {
            write((uint16_t)(APU_WAVE_RAM_START + byte), (uint8_t)(0x11 * byte));
        }

        write(APU_NR30_ADDRESS, 0x80);
        write(APU_NR34_ADDRESS, 0x80);
        apu.channel[2].position = cases[i].position;
        apu.channel[2].timer = cases[i].timer;
        write(APU_NR34_ADDRESS, 0x80);

        for (unsigned byte = 0; byte < 4; byte++) {
            assert(apu.wave_ram[byte] == cases[i].expected[byte]);
        }

        for (unsigned byte = 4; byte < APU_WAVE_RAM_SIZE; byte++) {
            assert(apu.wave_ram[byte] == (uint8_t)(0x11 * byte));
        }
    }
}

/* Over whole duty cycles the mean is the fraction high: with levels of
 * +/-8191, 1/8 high is (1 - 7) / 8 of that, and so on. A 128-frame window is
 * exactly one cycle wherever it starts. */
static void test_duty_cycles(void)
{
    static const struct {
        uint8_t nr21;
        int mean;
    } cases[] = {
        { 0x00, -6144 },    /* 12.5% */
        { 0x40, -4096 },    /* 25% */
        { 0x80, 0 },        /* 50% */
        { 0xC0, 4096 }      /* 75% */
    };
    static int16_t left[2048];
    static int16_t right[2048];

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fresh();
        apu_set_sample_rate(&apu, SAMPLE_RATE);
        write(APU_NR50_ADDRESS, 0x77);
        write(APU_NR51_ADDRESS, 0x22);
        write(APU_NR21_ADDRESS, cases[i].nr21);
        write(APU_NR22_ADDRESS, 0xF0);
        write(APU_NR23_ADDRESS, 0xC0);
        write(APU_NR24_ADDRESS, 0x87);
        collect(left, right, 256);
        collect(left, right, 512);
        assert(near(mean(left, 512), cases[i].mean));
    }
}

/* A pulse counter that ran out reloads with 64 on a trigger, one short as
 * in the wave channel's case. */
static void test_pulse_length_reload_on_trigger(void)
{
    fresh();
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR21_ADDRESS, 0x3F);   /* length 1 */
    write(APU_NR24_ADDRESS, 0xC0);
    run(FRAME_TICK);
    assert(!channel_on(1));

    write(APU_NR24_ADDRESS, 0xC0);   /* length 63 */
    run(125 * FRAME_TICK);           /* 62 length ticks */
    assert(channel_on(1));
    run(FRAME_TICK);
    assert(!channel_on(1));
}

/* A power cycle restarts the sequencer at step 0, wherever it had got to. */
static void test_power_on_restarts_the_sequencer(void)
{
    fresh();
    run(3 * FRAME_TICK);             /* the next step is now 3 */
    write(APU_NR52_ADDRESS, 0x00);
    write(APU_NR52_ADDRESS, 0x80);
    write(APU_NR22_ADDRESS, 0xF0);
    write(APU_NR21_ADDRESS, 0x3F);   /* length 1 */
    write(APU_NR24_ADDRESS, 0xC0);
    run(FRAME_TICK);                 /* step 0 clocks the length */
    assert(!channel_on(1));
}

/* A channel whose DAC is off adds nothing to the mix, not even its idle
 * level. */
static void test_dac_off_is_silent(void)
{
    static int16_t left[2048];
    static int16_t right[2048];

    fresh();
    apu_set_sample_rate(&apu, SAMPLE_RATE);
    write(APU_NR50_ADDRESS, 0x77);
    write(APU_NR51_ADDRESS, 0xFF);
    write(APU_NR12_ADDRESS, 0x00);
    write(APU_NR22_ADDRESS, 0x00);
    write(APU_NR30_ADDRESS, 0x00);
    write(APU_NR42_ADDRESS, 0x00);
    collect(left, right, 64);

    for (size_t i = 0; i < 64; i++) {
        assert(left[i] == 0);
        assert(right[i] == 0);
    }
}

/* Starts the wave channel at a 6-cycle sample period (frequency 0x7FD), so
 * with the 6-cycle start-up delay its fetches fall at cycles 12, 18, 24...
 * and those at 12, 24, 36... land on the end of a 4-cycle step. */
static void start_wave_with_period_6(void)
{
    fresh();

    for (unsigned byte = 0; byte < APU_WAVE_RAM_SIZE; byte++) {
        write((uint16_t)(APU_WAVE_RAM_START + byte),
              (uint8_t)(0x11 * byte + 1));
    }

    write(APU_NR30_ADDRESS, 0x80);
    write(APU_NR33_ADDRESS, 0xFD);
    write(APU_NR34_ADDRESS, 0x87);
}

static void test_wave_fetch_timing(void)
{
    /* Before the first fetch. */
    start_wave_with_period_6();
    run(8);
    assert(read(APU_WAVE_RAM_START) == 0xFF);

    /* The first fetch at cycle 12 reads sample 1, in byte 0. */
    start_wave_with_period_6();
    run(12);
    assert(read(APU_WAVE_RAM_START) == 0x01);

    /* One at 18 is not at the end of a step. */
    start_wave_with_period_6();
    run(20);
    assert(read(APU_WAVE_RAM_START) == 0xFF);

    /* The one at 24 reads sample 3, in byte 1, and a write then lands there
     * whatever the address. */
    start_wave_with_period_6();
    run(24);
    assert(read(APU_WAVE_RAM_START + 9) == 0x12);
    write(APU_WAVE_RAM_START + 7, 0xAB);
    write(APU_NR30_ADDRESS, 0x00);

    for (unsigned byte = 0; byte < APU_WAVE_RAM_SIZE; byte++) {
        assert(apu.wave_ram[byte] == (byte == 1 ? 0xAB
                                                : (uint8_t)(0x11 * byte + 1)));
    }

    /* Off the fetch, a write is lost. */
    start_wave_with_period_6();
    run(20);
    write(APU_WAVE_RAM_START + 7, 0xAB);
    write(APU_NR30_ADDRESS, 0x00);

    for (unsigned byte = 0; byte < APU_WAVE_RAM_SIZE; byte++) {
        assert(apu.wave_ram[byte] == (uint8_t)(0x11 * byte + 1));
    }
}

int main(void)
{
    test_registers_after_the_boot_rom();
    test_read_masks();
    test_power();
    test_wave_ram();
    test_status_and_dac();
    test_length_counter();
    test_sweep_overflow_on_trigger();
    test_pulse_output();
    test_envelope();
    test_noise_lfsr();
    test_wave_output();
    test_sample_buffer();
    test_sweep_runs_on_steps_2_and_6();
    test_length_reload_on_trigger();
    test_length_counters_survive_power_off();
    test_divider_reset_clocks_the_sequencer();
    test_noise_period();
    test_wave_ram_while_playing();
    test_full_buffer_keeps_the_newest();
    test_length_enable_extra_clock();
    test_envelope_period_0_holds();
    test_sweep_negate();
    test_retrigger_corrupts_wave_ram();
    test_duty_cycles();
    test_pulse_length_reload_on_trigger();
    test_power_on_restarts_the_sequencer();
    test_dac_off_is_silent();
    test_wave_fetch_timing();

    printf("APU tests passed!\n");

    return 0;
}
