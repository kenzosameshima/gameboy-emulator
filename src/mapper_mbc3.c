#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cycles.h>
#include <memory_map.h>

#include "mapper.h"

/*
 * MBC3: up to 2 MiB of ROM in 16 KiB banks, 32 KiB of RAM in 8 KiB banks
 * and, on some cartridges, a real-time clock. Register writes land in
 * 0000-7FFF:
 *
 *   0000-1FFF  RAM and clock enable (low nibble 0xA enables)
 *   2000-3FFF  ROM bank, 7 bits (written 0 is stored as 1)
 *   4000-5FFF  selects what A000-BFFF shows: RAM bank 00-03, or a clock
 *              register 08-0C
 *   6000-7FFF  clock latch: writing 00 then 01 copies the running clock
 *              into the registers the game reads
 *
 * Clock registers: 08 seconds, 09 minutes, 0A hours, 0B day (low 8 bits),
 * 0C bit 0 day (bit 8), bit 6 halt, bit 7 day carry.
 *
 * The clock runs off the CPU clock, so one second is 4194304 T-cycles and
 * it is deterministic.
 */

enum {
    MBC3_RAM_ENABLE_END = 0x1FFF,
    MBC3_ROM_BANK_END = 0x3FFF,
    MBC3_SELECT_END = 0x5FFF,
    MBC3_ENABLE_VALUE = 0x0A,

    MBC3_LAST_RAM_BANK = 0x03,
    MBC3_RTC_SECONDS = 0x08,
    MBC3_RTC_MINUTES = 0x09,
    MBC3_RTC_HOURS = 0x0A,
    MBC3_RTC_DAY_LOW = 0x0B,
    MBC3_RTC_DAY_HIGH = 0x0C,

    MBC3_DAY_HIGH_BIT8 = 0x01,
    MBC3_DAY_HIGH_HALT = 0x40,
    MBC3_DAY_HIGH_CARRY = 0x80,

    MBC3_CYCLES_PER_SECOND = 4194304,
    MBC3_HEADER_TYPE_TIMER_BATTERY = 0x0F,
    MBC3_HEADER_TYPE_TIMER_RAM_BATTERY = 0x10
};

static void mbc3_reset(Cartridge *cartridge)
{
    uint8_t type = cartridge->rom[CARTRIDGE_HEADER_TYPE];

    cartridge->state.mbc3 = (Mbc3State){
        .ram_and_clock_enabled = false,
        .rom_bank = 1,
        .select = 0,
        /* Not 0, so a lone write of 1 cannot latch. */
        .latch_write = 0xFF,
        .has_clock = type == MBC3_HEADER_TYPE_TIMER_BATTERY ||
                     type == MBC3_HEADER_TYPE_TIMER_RAM_BATTERY
    };
}

static uint8_t mbc3_read_rom(const Cartridge *cartridge, uint16_t address)
{
    size_t bank = 0;

    if (address >= CARTRIDGE_ROM_BANK_SIZE) {
        bank = cartridge->state.mbc3.rom_bank & cartridge->rom_bank_mask;
    }

    return cartridge_rom_byte(
        cartridge,
        bank * CARTRIDGE_ROM_BANK_SIZE +
            (address & (CARTRIDGE_ROM_BANK_SIZE - 1))
    );
}

static void mbc3_write_rom(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    Mbc3State *mbc3 = &cartridge->state.mbc3;

    if (address <= MBC3_RAM_ENABLE_END) {
        mbc3->ram_and_clock_enabled =
            (value & 0x0F) == MBC3_ENABLE_VALUE;
    } else if (address <= MBC3_ROM_BANK_END) {
        uint8_t bank = (uint8_t)(value & 0x7F);

        mbc3->rom_bank = bank == 0 ? 1 : bank;
    } else if (address <= MBC3_SELECT_END) {
        mbc3->select = value;
    } else {
        if (mbc3->latch_write == 0x00 && value == 0x01) {
            mbc3->latched = mbc3->live;
        }

        mbc3->latch_write = value;
    }
}

static bool mbc3_clock_selected(const Mbc3State *mbc3)
{
    return mbc3->has_clock &&
           mbc3->select >= MBC3_RTC_SECONDS &&
           mbc3->select <= MBC3_RTC_DAY_HIGH;
}

static uint8_t mbc3_day_high(const RtcRegisters *clock)
{
    return (uint8_t)(
        ((clock->days >> 8) & MBC3_DAY_HIGH_BIT8) |
        (clock->halted ? MBC3_DAY_HIGH_HALT : 0) |
        (clock->day_carry ? MBC3_DAY_HIGH_CARRY : 0)
    );
}

static uint8_t mbc3_read_clock(const Mbc3State *mbc3)
{
    const RtcRegisters *clock = &mbc3->latched;

    switch (mbc3->select) {
        case MBC3_RTC_SECONDS:
            return clock->seconds;

        case MBC3_RTC_MINUTES:
            return clock->minutes;

        case MBC3_RTC_HOURS:
            return clock->hours;

        case MBC3_RTC_DAY_LOW:
            return (uint8_t)(clock->days & 0xFF);

        default:
            return mbc3_day_high(clock);
    }
}

static void mbc3_write_clock(Mbc3State *mbc3, uint8_t value)
{
    RtcRegisters *clock = &mbc3->live;

    switch (mbc3->select) {
        case MBC3_RTC_SECONDS:
            clock->seconds = (uint8_t)(value & 0x3F);
            /* Writing the seconds restarts the current second. */
            mbc3->subsecond_cycles = 0;
            break;

        case MBC3_RTC_MINUTES:
            clock->minutes = (uint8_t)(value & 0x3F);
            break;

        case MBC3_RTC_HOURS:
            clock->hours = (uint8_t)(value & 0x1F);
            break;

        case MBC3_RTC_DAY_LOW:
            clock->days = (uint16_t)((clock->days & 0x100) | value);
            break;

        default:
            clock->days = (uint16_t)(
                (clock->days & 0xFF) |
                ((value & MBC3_DAY_HIGH_BIT8) != 0 ? 0x100 : 0)
            );
            clock->halted = (value & MBC3_DAY_HIGH_HALT) != 0;
            clock->day_carry = (value & MBC3_DAY_HIGH_CARRY) != 0;
            break;
    }
}

static size_t mbc3_ram_offset(const Cartridge *cartridge, uint16_t address)
{
    size_t offset = (size_t)cartridge->state.mbc3.select *
                        CARTRIDGE_RAM_BANK_SIZE +
                    (size_t)(address - MEM_CART_RAM_START);

    return offset % cartridge->ram_size;
}

static bool mbc3_ram_selected(const Cartridge *cartridge)
{
    return cartridge->ram != NULL &&
           cartridge->ram_size != 0 &&
           cartridge->state.mbc3.select <= MBC3_LAST_RAM_BANK;
}

static uint8_t mbc3_read_ram(const Cartridge *cartridge, uint16_t address)
{
    const Mbc3State *mbc3 = &cartridge->state.mbc3;

    if (!mbc3->ram_and_clock_enabled) {
        return 0xFF;
    }

    if (mbc3_ram_selected(cartridge)) {
        return cartridge->ram[mbc3_ram_offset(cartridge, address)];
    }

    if (mbc3_clock_selected(mbc3)) {
        return mbc3_read_clock(mbc3);
    }

    return 0xFF;
}

static void mbc3_write_ram(
    Cartridge *cartridge,
    uint16_t address,
    uint8_t value
)
{
    Mbc3State *mbc3 = &cartridge->state.mbc3;

    if (!mbc3->ram_and_clock_enabled) {
        return;
    }

    if (mbc3_ram_selected(cartridge)) {
        cartridge->ram[mbc3_ram_offset(cartridge, address)] = value;
    } else if (mbc3_clock_selected(mbc3)) {
        mbc3_write_clock(mbc3, value);
    }
}

/* Advances the running clock by one second. */
static void mbc3_tick_second(RtcRegisters *clock)
{
    /*
     * Each field is as wide as its register, so a value written past its
     * range counts up and wraps at the register width without carrying.
     */
    clock->seconds = (uint8_t)((clock->seconds + 1) & 0x3F);

    if (clock->seconds != 60) {
        return;
    }

    clock->seconds = 0;
    clock->minutes = (uint8_t)((clock->minutes + 1) & 0x3F);

    if (clock->minutes != 60) {
        return;
    }

    clock->minutes = 0;
    clock->hours = (uint8_t)((clock->hours + 1) & 0x1F);

    if (clock->hours != 24) {
        return;
    }

    clock->hours = 0;
    clock->days = (uint16_t)((clock->days + 1) & 0x1FF);

    if (clock->days == 0) {
        clock->day_carry = true;
    }
}

static void mbc3_step(Cartridge *cartridge, CpuCycles cycles)
{
    Mbc3State *mbc3 = &cartridge->state.mbc3;

    if (!mbc3->has_clock || mbc3->live.halted) {
        return;
    }

    mbc3->subsecond_cycles += cycles;

    while (mbc3->subsecond_cycles >= MBC3_CYCLES_PER_SECOND) {
        mbc3->subsecond_cycles -= MBC3_CYCLES_PER_SECOND;
        mbc3_tick_second(&mbc3->live);
    }
}

const MapperOps MAPPER_MBC3 = {
    .reset = mbc3_reset,
    .read_rom = mbc3_read_rom,
    .write_rom = mbc3_write_rom,
    .read_ram = mbc3_read_ram,
    .write_ram = mbc3_write_ram,
    .step = mbc3_step
};
