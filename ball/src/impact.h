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

/* Pop the oldest queued impact. Returns false if there is none.
 * Only impacts preceded by free fall (IMPACT_AFTER_FREEFALL_MS) are queued: the ball was
 * flying. Threshold crossings without free fall (launch spike, bumps in the hand) are only
 * counted, see impact_rejected(). */
bool impact_get(struct impact_event *event);

/* Largest |a| (milli-g) seen since the previous call, then starts over. ~1000 when the ball
 * sits still: a quick check that the accelerometer is alive. -1 if there was no sample at all
 * (sensor stalled). */
int32_t impact_take_peak(void);

/* Number of failed sensor reads since boot. */
uint32_t impact_errors(void);

/* Number of times the sensor stopped delivering data and was re-initialized. */
uint32_t impact_resets(void);

/* False while the sensor is stalled (no data) and being re-initialized. The motion helpers
 * below fail safe in that case. */
bool impact_healthy(void);

/* Number of threshold crossings ignored because no free fall preceded them. If last_mg is
 * not NULL it receives the magnitude of the latest one. */
uint32_t impact_rejected(int32_t *last_mg);

/* True if free fall (|a| < FREEFALL_MG for >= FREEFALL_MIN_MS) ended or is ongoing within the
 * last hold_ms milliseconds. */
bool impact_freefall_recent(int hold_ms);

/* k_uptime_get_32() when the current free fall began, 0 if none since the last clear. */
uint32_t impact_freefall_start_ms(void);

/* Forget any detected free fall (call when entering Ready). */
void impact_freefall_clear(void);

/* Milliseconds since the ball last moved (|a(t) - a(t-100 ms)| > STILL_DELTA_MG). 0 while
 * the sensor is unhealthy, so a dead sensor never makes the ball look idle. */
uint32_t impact_still_ms(void);

/* True if the ball moved within the last ms milliseconds; always true while unhealthy. */
bool impact_moved_recent(int ms);

/* Drop all queued impacts. */
void impact_flush(void);

#endif
