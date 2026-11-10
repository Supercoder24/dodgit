#ifndef DODGIT_HIT_LOGIC_H
#define DODGIT_HIT_LOGIC_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "settings.h"

/* Unsigned subtraction handles the low-32-bit DW timestamp rollover. All
 * exchange intervals are bounded below 30 ms, less than the 67 ms rollover. */
static inline bool twr_distance(uint32_t poll_tx, uint32_t resp_rx,
                               uint32_t final_tx, uint32_t poll_rx,
                               uint32_t resp_tx, uint32_t final_rx, int32_t *mm)
{
    double ra = (uint32_t)(resp_rx - poll_tx);
    double rb = (uint32_t)(final_rx - resp_tx);
    double da = (uint32_t)(final_tx - resp_rx);
    double db = (uint32_t)(resp_tx - poll_rx);
    const double max_interval = 30000.0 * 65536.0;
    if (ra <= 0 || rb <= 0 || da <= 0 || db <= 0 ||
        ra > max_interval || rb > max_interval || da > max_interval || db > max_interval) {
        return false;
    }
    double tof = (ra * rb - da * db) / (ra + rb + da + db);
    double distance = tof * (299702547.0 * 1000.0 / (499200000.0 * 128.0));
    /* Reject negative/implausible results instead of clamping them to "close". */
    if (!isfinite(distance) || distance < 0 || distance > 50000) {
        return false;
    }
    *mm = (int32_t)(distance + 0.5);
    return true;
}

static inline bool range_matches(int64_t impact_ms, int64_t range_ms, int32_t mm)
{
    int64_t age = range_ms - impact_ms;
    return age >= -RANGE_WINDOW_MS && age <= RANGE_WINDOW_MS &&
           mm >= 0 && mm <= HIT_DISTANCE_MM;
}

/* Always active, with hysteresis and cooldown to avoid counting one impact twice. */
struct impact_gate { bool ready; int64_t cooldown_until; };

static inline bool impact_sample(struct impact_gate *d, int64_t now, int32_t mg,
                                 int32_t threshold, int cooldown_ms)
{
    if (mg < threshold * 3 / 4) d->ready = true;
    if (d->ready && now >= d->cooldown_until && mg >= threshold) {
        d->ready = false;
        d->cooldown_until = now + cooldown_ms;
        return true;
    }
    return false;
}

#endif
