/*
 * dodgit - double-sided two-way ranging (DS-TWR) distance calculation.
 * Pure C (no Zephyr), so it can be unit tested on the host: tests/twr_test.c
 */
#ifndef DODGIT_TWR_H
#define DODGIT_TWR_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * All timestamps are the low 32 bits of the radio's 40-bit time (device time
 * units, ~15.65 ps). Unsigned subtraction gives the right interval even across
 * the 32-bit rollover (every ~67 ms) as long as each interval is shorter than that.
 *
 *   Ra = resp_rx  - poll_tx   ball:  POLL sent -> RESPONSE received
 *   Da = final_tx - resp_rx   ball:  RESPONSE received -> FINAL sent
 *   Rb = final_rx - resp_tx   pouch: RESPONSE sent -> FINAL received
 *   Db = resp_tx  - poll_rx   pouch: POLL received -> RESPONSE sent
 *
 *   time of flight = (Ra*Rb - Da*Db) / (Ra + Rb + Da + Db)
 *
 * Returns false (and leaves *mm alone) for impossible inputs instead of
 * clamping them to "close".
 */
static inline bool twr_distance(uint32_t poll_tx, uint32_t resp_rx, uint32_t final_tx,
                                uint32_t poll_rx, uint32_t resp_tx, uint32_t final_rx,
                                int32_t *mm)
{
    double ra = (uint32_t)(resp_rx - poll_tx);
    double da = (uint32_t)(final_tx - resp_rx);
    double rb = (uint32_t)(final_rx - resp_tx);
    double db = (uint32_t)(resp_tx - poll_rx);

    /* Every interval must be positive and below 30 ms. */
    const double max_interval = 30000.0 * 65536.0;
    if (ra <= 0 || rb <= 0 || da <= 0 || db <= 0 ||
        ra > max_interval || rb > max_interval || da > max_interval || db > max_interval) {
        return false;
    }

    double tof = (ra * rb - da * db) / (ra + rb + da + db);
    /* 1 time unit = 1 / (499.2 MHz * 128); speed of light in air 299702547 m/s. */
    double distance = tof * (299702547.0 * 1000.0 / (499200000.0 * 128.0));
    if (!isfinite(distance) || distance < 0 || distance > 50000) {
        return false;
    }
    *mm = (int32_t)(distance + 0.5);
    return true;
}

#endif
