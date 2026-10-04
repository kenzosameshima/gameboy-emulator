#include <apu.h>

#include <string.h>

enum {
    CLOCK_HZ = 4194304,

    CHANNEL_PULSE_1 = 0,
    CHANNEL_PULSE_2 = 1,
    CHANNEL_WAVE = 2,
    CHANNEL_NOISE = 3,
    CHANNEL_COUNT = 4,

    /* The frame sequencer is clocked by the falling edge of this bit of the
     * system counter: 512 Hz. */
    FRAME_SEQUENCER_BIT = 0x1000,

    /* A register's offset from NRx0 of its channel. */
    REGISTER_SWEEP = 0,         /* NR10, and NR30's DAC enable */
    REGISTER_LENGTH = 1,
    REGISTER_ENVELOPE = 2,      /* NR32 holds the wave volume */
    REGISTER_FREQUENCY_LOW = 3,
    REGISTER_CONTROL = 4,       /* NRx4: trigger, length enable, frequency */

    NR50_INDEX = APU_NR50_ADDRESS - APU_REGISTERS_START,
    NR51_INDEX = APU_NR51_ADDRESS - APU_REGISTERS_START,

    LENGTH_ENABLE = 0x40,
    TRIGGER = 0x80,
    MAX_FREQUENCY = 2047,

    /* The mixer scales a channel's -15..15 DAC value by NR50's 1-8 volume,
     * for at most 4 * 15 * 8 = 480 of full scale at 32767. */
    MIX_FULL_SCALE = 4 * 15 * 8,
    SAMPLE_FULL_SCALE = 32767
};

/* NR10 (FF10) to NR52 (FF26): the bits that always read as 1. */
static const uint8_t read_masks[APU_REGISTERS_END - APU_REGISTERS_START + 1] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,   /* NR10-NR14 */
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,   /* unused, NR21-NR24 */
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,   /* NR30-NR34 */
    0xFF, 0xFF, 0x00, 0x00, 0xBF,   /* unused, NR41-NR44 */
    0x00, 0x00, 0x70                /* NR50-NR52 */
};

/* What the boot ROM leaves in NR10-NR51 when it hands over to the cartridge. */
static const uint8_t boot_registers[] = {
    0x80, 0xBF, 0xF3, 0xFF, 0xBF,
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,
    0xFF, 0xFF, 0x00, 0x00, 0xBF,
    0x77, 0xF3
};

/* Duty cycles 12.5%, 25%, 50% and 75%, one bit a step, step 0 first. */
static const uint8_t duty_patterns[4] = { 0x01, 0x81, 0x87, 0x7E };

static const uint8_t noise_divisors[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

/* The first address of each channel's registers. Channel 2's NRx0 is NR30. */
static const uint16_t channel_base[CHANNEL_COUNT] = {
    APU_NR10_ADDRESS, APU_NR21_ADDRESS - 1, APU_NR30_ADDRESS,
    APU_NR41_ADDRESS - 1
};

static uint8_t channel_register(
    const Apu *apu,
    unsigned channel,
    unsigned offset
)
{
    return apu->registers[channel_base[channel] + offset - APU_REGISTERS_START];
}

static uint16_t channel_frequency(const Apu *apu, unsigned channel)
{
    return (uint16_t)(((channel_register(apu, channel, REGISTER_CONTROL) & 7u)
                       << 8) |
                      channel_register(apu, channel, REGISTER_FREQUENCY_LOW));
}

static bool length_enabled(const Apu *apu, unsigned channel)
{
    return (channel_register(apu, channel, REGISTER_CONTROL) & LENGTH_ENABLE)
           != 0;
}

static unsigned max_length(unsigned channel)
{
    return channel == CHANNEL_WAVE ? 256 : 64;
}

/* A channel plays only while its DAC is on: the top five bits of NRx2, or
 * NR30's bit 7 for the wave channel. */
static bool dac_enabled(const Apu *apu, unsigned channel)
{
    if (channel == CHANNEL_WAVE) {
        return (channel_register(apu, channel, REGISTER_SWEEP) & 0x80) != 0;
    }

    return (channel_register(apu, channel, REGISTER_ENVELOPE) & 0xF8) != 0;
}

/* T-cycles between steps of the channel's waveform. */
static int32_t channel_period(const Apu *apu, unsigned channel)
{
    if (channel == CHANNEL_NOISE) {
        uint8_t polynomial = channel_register(apu, channel,
                                              REGISTER_FREQUENCY_LOW);

        return (int32_t)noise_divisors[polynomial & 7u] << (polynomial >> 4);
    }

    int32_t steps = 2048 - channel_frequency(apu, channel);

    return channel == CHANNEL_WAVE ? steps * 2 : steps * 4;
}

/* ---- Triggering and the sweep ---- */

/* The next sweep frequency. Overflowing 2047 stops the channel. */
static uint16_t sweep_calculate(Apu *apu)
{
    ApuChannel *channel = &apu->channel[CHANNEL_PULSE_1];
    uint8_t sweep = channel_register(apu, CHANNEL_PULSE_1, REGISTER_SWEEP);
    uint16_t delta = (uint16_t)(channel->shadow_frequency >> (sweep & 7u));
    uint16_t next;

    if ((sweep & 0x08) != 0) {
        next = (uint16_t)(channel->shadow_frequency - delta);
        channel->sweep_negated = true;
    } else {
        next = (uint16_t)(channel->shadow_frequency + delta);
    }

    if (next > MAX_FREQUENCY) {
        channel->enabled = false;
    }

    return next;
}

static void set_frequency(Apu *apu, unsigned channel, uint16_t frequency)
{
    uint16_t base = (uint16_t)(channel_base[channel] - APU_REGISTERS_START);
    uint8_t *control = &apu->registers[base + REGISTER_CONTROL];

    apu->registers[base + REGISTER_FREQUENCY_LOW] = (uint8_t)frequency;
    *control = (uint8_t)((*control & ~7u) | (frequency >> 8));
}

static void start_sweep(Apu *apu)
{
    ApuChannel *channel = &apu->channel[CHANNEL_PULSE_1];
    uint8_t sweep = channel_register(apu, CHANNEL_PULSE_1, REGISTER_SWEEP);
    uint8_t period = (sweep >> 4) & 7u;
    uint8_t shift = sweep & 7u;

    channel->shadow_frequency = channel_frequency(apu, CHANNEL_PULSE_1);
    channel->sweep_timer = period != 0 ? period : 8;
    channel->sweep_negated = false;
    channel->sweep_enabled = period != 0 || shift != 0;

    if (shift != 0) {
        (void)sweep_calculate(apu);
    }
}

/* The envelope timer counts 8 when the period is 0, but then never runs. */
static void start_envelope(Apu *apu, unsigned channel)
{
    uint8_t envelope = channel_register(apu, channel, REGISTER_ENVELOPE);
    uint8_t period = envelope & 7u;

    apu->channel[channel].volume = envelope >> 4;
    apu->channel[channel].envelope_timer = period != 0 ? period : 8;
}

/*
 * Retriggering the wave channel just as it fetches a sample damages wave RAM
 * on the DMG: the byte it was about to read, if that is one of the first
 * four, replaces byte 0, and otherwise the aligned group of four bytes it is
 * in replaces the first four.
 */
static void corrupt_wave_ram(Apu *apu)
{
    unsigned next = ((apu->channel[CHANNEL_WAVE].position + 1u) >> 1) & 0xFu;

    if (next < 4) {
        apu->wave_ram[0] = apu->wave_ram[next];
    } else {
        memcpy(apu->wave_ram, apu->wave_ram + (next & ~3u), 4);
    }
}

static void trigger(Apu *apu, unsigned channel)
{
    ApuChannel *state = &apu->channel[channel];

    /* The channel is about to fetch its next sample. */
    bool about_to_read = state->enabled && state->timer <= 2;

    state->enabled = dac_enabled(apu, channel);

    if (state->length == 0) {
        state->length = (uint16_t)max_length(channel);

        /* Reloading a counter that is enabled when the next sequencer step
         * does not clock lengths leaves it one short. */
        if (length_enabled(apu, channel) && (apu->frame_step & 1u) != 0) {
            state->length--;
        }
    }

    state->timer = channel_period(apu, channel);

    switch (channel) {
    case CHANNEL_PULSE_1:
        start_sweep(apu);
        start_envelope(apu, channel);
        break;
    case CHANNEL_PULSE_2:
        start_envelope(apu, channel);
        break;
    case CHANNEL_WAVE:
        if (about_to_read) {
            corrupt_wave_ram(apu);
        }

        /* The first sample is read a little after the channel starts. */
        state->timer += 6;
        state->position = 0;
        break;
    default:
        start_envelope(apu, channel);
        state->lfsr = 0x7FFF;
        break;
    }
}

/* ---- Frame sequencer ---- */

static void clock_lengths(Apu *apu)
{
    for (unsigned channel = 0; channel < CHANNEL_COUNT; channel++) {
        ApuChannel *state = &apu->channel[channel];

        if (length_enabled(apu, channel) && state->length != 0) {
            state->length--;

            if (state->length == 0) {
                state->enabled = false;
            }
        }
    }
}

static void clock_sweep(Apu *apu)
{
    ApuChannel *channel = &apu->channel[CHANNEL_PULSE_1];

    if (channel->sweep_timer == 0 || --channel->sweep_timer != 0) {
        return;
    }

    uint8_t sweep = channel_register(apu, CHANNEL_PULSE_1, REGISTER_SWEEP);
    uint8_t period = (sweep >> 4) & 7u;

    channel->sweep_timer = period != 0 ? period : 8;

    if (!channel->sweep_enabled || period == 0) {
        return;
    }

    uint16_t next = sweep_calculate(apu);

    if (next <= MAX_FREQUENCY && (sweep & 7u) != 0) {
        channel->shadow_frequency = next;
        set_frequency(apu, CHANNEL_PULSE_1, next);

        /* The sweep checks the following step for overflow at once. */
        (void)sweep_calculate(apu);
    }
}

static void clock_envelopes(Apu *apu)
{
    static const uint8_t envelope_channels[] = {
        CHANNEL_PULSE_1, CHANNEL_PULSE_2, CHANNEL_NOISE
    };

    for (size_t i = 0; i < sizeof(envelope_channels); i++) {
        unsigned channel = envelope_channels[i];
        ApuChannel *state = &apu->channel[channel];
        uint8_t envelope = channel_register(apu, channel, REGISTER_ENVELOPE);
        uint8_t period = envelope & 7u;

        if (period == 0 || state->envelope_timer == 0 ||
            --state->envelope_timer != 0) {
            continue;
        }

        state->envelope_timer = period;

        if ((envelope & 0x08) != 0) {
            if (state->volume < 15) {
                state->volume++;
            }
        } else if (state->volume > 0) {
            state->volume--;
        }
    }
}

static void clock_frame_sequencer(Apu *apu)
{
    uint8_t step = apu->frame_step;

    if ((step & 1u) == 0) {
        clock_lengths(apu);
    }

    if (step == 2 || step == 6) {
        clock_sweep(apu);
    }

    if (step == 7) {
        clock_envelopes(apu);
    }

    apu->frame_step = (step + 1u) & 7u;
}

/* ---- Channel waveforms ---- */

/* Runs the channel's frequency timer for `cycles` and returns how many times
 * it expired. */
static unsigned run_timer(ApuChannel *channel, uint32_t cycles, int32_t period)
{
    unsigned expirations = 0;

    channel->timer -= (int32_t)cycles;

    while (channel->timer <= 0) {
        channel->timer += period;
        expirations++;
    }

    return expirations;
}

static void clock_noise(Apu *apu, unsigned times)
{
    ApuChannel *channel = &apu->channel[CHANNEL_NOISE];
    uint8_t polynomial = channel_register(apu, CHANNEL_NOISE,
                                          REGISTER_FREQUENCY_LOW);

    for (unsigned i = 0; i < times; i++) {
        unsigned feedback = (channel->lfsr ^ (channel->lfsr >> 1)) & 1u;

        channel->lfsr = (uint16_t)((channel->lfsr >> 1) | (feedback << 14));

        if ((polynomial & 0x08) != 0) {
            channel->lfsr = (uint16_t)((channel->lfsr & ~0x40u) |
                                       (feedback << 6));
        }
    }
}

static void advance_channels(Apu *apu, uint32_t cycles)
{
    ApuChannel *wave = &apu->channel[CHANNEL_WAVE];

    wave->wave_just_read = false;

    for (unsigned channel = 0; channel < CHANNEL_COUNT; channel++) {
        ApuChannel *state = &apu->channel[channel];

        if (!state->enabled) {
            continue;
        }

        int32_t period = channel_period(apu, channel);
        unsigned expirations = run_timer(state, cycles,
                                         period);

        switch (channel) {
        case CHANNEL_WAVE:
            if (expirations != 0) {
                state->position = (uint8_t)((state->position + expirations) &
                                            31u);

                uint8_t byte = apu->wave_ram[state->position >> 1];

                state->wave_buffer = (state->position & 1u) == 0
                                         ? (uint8_t)(byte >> 4)
                                         : (uint8_t)(byte & 0x0F);

                /* The CPU only gets at wave RAM if the fetch landed on the
                 * very end of the step. */
                state->wave_just_read = state->timer == period;
            }
            break;
        case CHANNEL_NOISE:
            /* Shifts of 14 and 15 stop the generator. */
            if (channel_register(apu, channel, REGISTER_FREQUENCY_LOW) <
                0xE0) {
                clock_noise(apu, expirations);
            }
            break;
        default:
            state->position = (uint8_t)((state->position + expirations) & 7u);
            break;
        }
    }
}

/* The channel's digital output, 0-15. */
static unsigned digital_output(const Apu *apu, unsigned channel)
{
    const ApuChannel *state = &apu->channel[channel];

    if (!state->enabled) {
        return 0;
    }

    switch (channel) {
    case CHANNEL_WAVE: {
        static const uint8_t volume_shifts[4] = { 4, 0, 1, 2 };
        uint8_t code = (channel_register(apu, channel, REGISTER_ENVELOPE) >> 5)
                       & 3u;

        return (unsigned)state->wave_buffer >> volume_shifts[code];
    }
    case CHANNEL_NOISE:
        return (state->lfsr & 1u) == 0 ? state->volume : 0u;
    default: {
        uint8_t duty = channel_register(apu, channel, REGISTER_LENGTH) >> 6;

        return ((duty_patterns[duty] << state->position) & 0x80) != 0
                   ? state->volume
                   : 0u;
    }
    }
}

/* ---- Mixer ---- */

/* Each DAC maps 0-15 to -1..+1, so in fifteenths a channel is -15..15, and
 * one with its DAC off adds nothing. The sides are scaled by NR50's volume. */
static void mix(const Apu *apu, int32_t *left, int32_t *right)
{
    uint8_t panning = apu->registers[NR51_INDEX];
    uint8_t volume = apu->registers[NR50_INDEX];
    int32_t sum_left = 0;
    int32_t sum_right = 0;

    for (unsigned channel = 0; channel < CHANNEL_COUNT; channel++) {
        if (!dac_enabled(apu, channel)) {
            continue;
        }

        int32_t level = 2 * (int32_t)digital_output(apu, channel) - 15;

        if ((panning & (0x10u << channel)) != 0) {
            sum_left += level;
        }

        if ((panning & (1u << channel)) != 0) {
            sum_right += level;
        }
    }

    *left = sum_left * (int32_t)(((volume >> 4) & 7u) + 1u);
    *right = sum_right * (int32_t)((volume & 7u) + 1u);
}

static int16_t scale_sample(int64_t accumulated, uint32_t cycles_x256)
{
    return (int16_t)(accumulated * SAMPLE_FULL_SCALE /
                     ((int64_t)MIX_FULL_SCALE * cycles_x256));
}

static void push_frame(Apu *apu)
{
    size_t index;

    if (apu->sample_count < APU_SAMPLE_BUFFER_FRAMES) {
        index = (apu->sample_head + apu->sample_count) %
                APU_SAMPLE_BUFFER_FRAMES;
        apu->sample_count++;
    } else {
        /* Full: drop the oldest frame. */
        index = apu->sample_head;
        apu->sample_head = (apu->sample_head + 1) % APU_SAMPLE_BUFFER_FRAMES;
    }

    apu->samples[2 * index] = scale_sample(apu->accumulated_left,
                                           apu->cycles_per_sample_x256);
    apu->samples[2 * index + 1] = scale_sample(apu->accumulated_right,
                                               apu->cycles_per_sample_x256);
    apu->accumulated_left = 0;
    apu->accumulated_right = 0;
    apu->sample_phase_x256 = 0;
}

/* Adds `cycles` of the current mix to the frame being averaged. */
static void accumulate_output(Apu *apu, uint32_t cycles)
{
    if (apu->sample_rate == 0) {
        return;
    }

    int32_t left;
    int32_t right;

    mix(apu, &left, &right);

    uint64_t remaining = (uint64_t)cycles * 256;

    while (remaining > 0) {
        uint32_t needed = apu->cycles_per_sample_x256 - apu->sample_phase_x256;
        uint32_t chunk = remaining < needed ? (uint32_t)remaining : needed;

        apu->accumulated_left += (int64_t)left * chunk;
        apu->accumulated_right += (int64_t)right * chunk;
        apu->sample_phase_x256 += chunk;
        remaining -= chunk;

        if (apu->sample_phase_x256 == apu->cycles_per_sample_x256) {
            push_frame(apu);
        }
    }
}

/* ---- Power ---- */

/* Powering off clears the registers and stops the channels. The length
 * counters are the one thing that survives, as on the DMG. */
static void power_off(Apu *apu)
{
    for (unsigned channel = 0; channel < CHANNEL_COUNT; channel++) {
        uint16_t length = apu->channel[channel].length;

        apu->channel[channel] = (ApuChannel){ .length = length };
    }

    memset(apu->registers, 0, sizeof(apu->registers));
    apu->powered = false;
}

static void power_on(Apu *apu)
{
    apu->powered = true;
    apu->frame_step = 0;
}

void apu_init(Apu *apu)
{
    memset(apu, 0, sizeof(*apu));
    memcpy(apu->registers, boot_registers, sizeof(boot_registers));
    apu->powered = true;

    /* The boot ROM's start-up sound is still playing on channel 1, though
     * its envelope has run down. */
    apu->channel[CHANNEL_PULSE_1].enabled = true;
    apu->channel[CHANNEL_PULSE_1].length = 1;
}

/* ---- Registers ---- */

uint8_t apu_read(const Apu *apu, uint16_t address)
{
    if (address >= APU_WAVE_RAM_START && address <= APU_WAVE_RAM_END) {
        const ApuChannel *wave = &apu->channel[CHANNEL_WAVE];

        /* While the channel plays, the CPU sees the byte it is reading, and
         * only in the cycle it reads it. */
        if (wave->enabled) {
            return wave->wave_just_read ? apu->wave_ram[wave->position >> 1]
                                        : 0xFF;
        }

        return apu->wave_ram[address - APU_WAVE_RAM_START];
    }

    if (address < APU_REGISTERS_START || address > APU_REGISTERS_END) {
        return 0xFF;
    }

    if (address == APU_NR52_ADDRESS) {
        uint8_t status = apu->powered ? 0x80 : 0x00;

        for (unsigned channel = 0; channel < CHANNEL_COUNT; channel++) {
            if (apu->channel[channel].enabled) {
                status |= (uint8_t)(1u << channel);
            }
        }

        return status | read_masks[address - APU_REGISTERS_START];
    }

    return apu->registers[address - APU_REGISTERS_START] |
           read_masks[address - APU_REGISTERS_START];
}

/* NRx1 (NR31 for the wave channel) loads a length counter. These are the
 * only registers that take writes while the APU is off. */
static void load_length(Apu *apu, uint16_t address, uint8_t value)
{
    switch (address) {
    case APU_NR11_ADDRESS:
        apu->channel[CHANNEL_PULSE_1].length = 64u - (value & 0x3Fu);
        break;
    case APU_NR21_ADDRESS:
        apu->channel[CHANNEL_PULSE_2].length = 64u - (value & 0x3Fu);
        break;
    case APU_NR31_ADDRESS:
        apu->channel[CHANNEL_WAVE].length = 256u - value;
        break;
    case APU_NR41_ADDRESS:
        apu->channel[CHANNEL_NOISE].length = 64u - (value & 0x3Fu);
        break;
    default:
        break;
    }
}

/* Writing NRx4 can enable the length counter and trigger the channel. */
static void write_control(Apu *apu, unsigned channel, uint8_t value)
{
    ApuChannel *state = &apu->channel[channel];
    bool was_enabled = length_enabled(apu, channel);

    apu->registers[channel_base[channel] + REGISTER_CONTROL -
                   APU_REGISTERS_START] = value;

    /* Enabling the length counter when the next sequencer step does not
     * clock lengths clocks it once now. */
    if (!was_enabled && (value & LENGTH_ENABLE) != 0 && state->length != 0 &&
        (apu->frame_step & 1u) != 0) {
        state->length--;

        if (state->length == 0 && (value & TRIGGER) == 0) {
            state->enabled = false;
        }
    }

    if ((value & TRIGGER) != 0) {
        trigger(apu, channel);
    }
}

static void write_wave_ram(Apu *apu, uint16_t address, uint8_t value)
{
    const ApuChannel *wave = &apu->channel[CHANNEL_WAVE];

    if (!wave->enabled) {
        apu->wave_ram[address - APU_WAVE_RAM_START] = value;
    } else if (wave->wave_just_read) {
        apu->wave_ram[wave->position >> 1] = value;
    }
}

void apu_write(Apu *apu, uint16_t address, uint8_t value)
{
    if (address >= APU_WAVE_RAM_START && address <= APU_WAVE_RAM_END) {
        write_wave_ram(apu, address, value);
        return;
    }

    if (address < APU_REGISTERS_START || address > APU_REGISTERS_END) {
        return;
    }

    if (address == APU_NR52_ADDRESS) {
        if ((value & 0x80) == 0) {
            power_off(apu);
        } else if (!apu->powered) {
            power_on(apu);
        }

        return;
    }

    if (!apu->powered) {
        load_length(apu, address, value);
        return;
    }

    uint16_t index = address - APU_REGISTERS_START;
    unsigned channel = 0;

    while (channel + 1 < CHANNEL_COUNT && address >= channel_base[channel + 1]) {
        channel++;
    }

    unsigned offset = address - channel_base[channel];

    if (address >= APU_NR50_ADDRESS) {
        apu->registers[index] = value;
        return;
    }

    if (offset == REGISTER_CONTROL) {
        write_control(apu, channel, value);
        return;
    }

    uint8_t previous = apu->registers[index];

    apu->registers[index] = value;

    switch (offset) {
    case REGISTER_SWEEP:
        if (channel == CHANNEL_PULSE_1) {
            /* Leaving negate mode after a negated calculation stops it. */
            if ((value & 0x08) == 0 && (previous & 0x08) != 0 &&
                apu->channel[channel].sweep_negated) {
                apu->channel[channel].enabled = false;
            }
        } else if (channel == CHANNEL_WAVE && !dac_enabled(apu, channel)) {
            apu->channel[channel].enabled = false;
        }
        break;
    case REGISTER_LENGTH:
        load_length(apu, address, value);
        break;
    case REGISTER_ENVELOPE:
        if (channel != CHANNEL_WAVE && !dac_enabled(apu, channel)) {
            apu->channel[channel].enabled = false;
        }
        break;
    default:
        break;
    }
}

/* ---- Time ---- */

void apu_step(Apu *apu, CpuCycles cycles, uint16_t divider)
{
    bool tick = (apu->divider & FRAME_SEQUENCER_BIT) != 0 &&
                (divider & FRAME_SEQUENCER_BIT) == 0;

    apu->divider = divider;

    /* A timer that runs out at the end of the step changes the output for
     * the next one, so the step is mixed from the state it started in. */
    accumulate_output(apu, cycles);

    if (apu->powered) {
        advance_channels(apu, cycles);

        if (tick) {
            clock_frame_sequencer(apu);
        }
    }
}

/* ---- Output ---- */

void apu_set_sample_rate(Apu *apu, unsigned rate)
{
    if (rate > CLOCK_HZ) {
        rate = CLOCK_HZ;
    }

    apu->sample_rate = rate;
    apu->cycles_per_sample_x256 =
        rate == 0 ? 0 : (uint32_t)(((uint64_t)CLOCK_HZ * 256) / rate);
    apu->sample_phase_x256 = 0;
    apu->accumulated_left = 0;
    apu->accumulated_right = 0;
    apu->sample_head = 0;
    apu->sample_count = 0;
}

size_t apu_take_samples(Apu *apu, int16_t *out, size_t max_frames)
{
    size_t frames = apu->sample_count < max_frames ? apu->sample_count
                                                    : max_frames;

    for (size_t i = 0; i < frames; i++) {
        size_t index = (apu->sample_head + i) % APU_SAMPLE_BUFFER_FRAMES;

        out[2 * i] = apu->samples[2 * index];
        out[2 * i + 1] = apu->samples[2 * index + 1];
    }

    apu->sample_head = (apu->sample_head + frames) % APU_SAMPLE_BUFFER_FRAMES;
    apu->sample_count -= frames;

    return frames;
}
