#ifndef CYCLES_H
#define CYCLES_H

#include <stdint.h>

/*
 * Machine time is measured in T-cycles (the 4194304 Hz clock).
 * One M-cycle (one bus access or one internal delay) is 4 T-cycles.
 * A single instruction never takes more than 24 T-cycles.
 */
typedef uint32_t CpuCycles;

/*
 * Falling edges of the system counter (the timer's divider): bit n set means
 * counter bit n went from 1 to 0. Serial and the APU are clocked by them.
 */
typedef uint16_t CounterEdges;

enum {
    CYCLES_PER_MCYCLE = 4,
    CYCLES_PER_FRAME = 70224
};

#endif
