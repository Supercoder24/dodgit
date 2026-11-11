/* dodgit - ball accelerometer impact detector (background thread). */
#ifndef DODGIT_IMPACT_H
#define DODGIT_IMPACT_H

#include <stdbool.h>
#include <stdint.h>

struct impact_event {
    int64_t time; /* k_uptime_get() when the impact was seen (ms) */
    int32_t mg;   /* magnitude, milli-g */
};

/* Configure the accelerometer (+/-16 g, 400 Hz) and start detecting. Returns 0 or -errno.
 * Impacts are ignored for the first 500 ms (sensor settling). */
int impact_init(int32_t threshold_mg, int cooldown_ms);

/* Pop the oldest queued impact. Returns false if there is none. */
bool impact_get(struct impact_event *event);

/* Largest |a| (milli-g) seen since the previous call, then starts over. ~1000 when the ball
 * sits still: a quick check that the accelerometer is alive. */
int32_t impact_take_peak(void);

/* Number of failed sensor reads since boot. */
uint32_t impact_errors(void);

/* Drop all queued impacts (call when entering Armed so old bumps don't count). */
void impact_flush(void);

#endif
