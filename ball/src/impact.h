/* dodgit - ball accelerometer impact detector (background thread). */
#ifndef DODGIT_IMPACT_H
#define DODGIT_IMPACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct impact_event {
    int64_t time;       /* k_uptime_get() when the impact was seen (ms) */
    int32_t mg;         /* magnitude, milli-g */
    int32_t ff_age_ms;  /* diagnostics: ms since the latest free-fall sample, -1 if none */
    int32_t min_prev_mg;/* diagnostics: lowest |a| in the IMPACT_AFTER_FREEFALL_MS before it, -1 if no sample */
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

/* Lowest |a| (milli-g) seen since the previous call, then starts over; -1 if there was no sample.
 * Diagnostics for the free-fall gate: during a real flight this should drop below FREEFALL_MG. */
int32_t impact_take_min(void);

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

/* Diagnostics of the latest rejected crossing (same snapshot as impact_rejected()): ms since the
 * latest free-fall sample (-1 = none since the last clear) and the lowest |a| in the
 * IMPACT_AFTER_FREEFALL_MS before the crossing (-1 = no sample). Either pointer may be NULL. */
void impact_rejected_detail(int32_t *ff_age_ms, int32_t *min_prev_mg);

/* ms since the latest free-fall sample, -1 if none since the last impact_freefall_clear(). */
int32_t impact_freefall_age_ms(void);

/* Raw accelerometer burst around every threshold crossing (accepted, rejected or inside the
 * cooldown): the BURST_PRE_SAMPLES |a| samples before it, the crossing itself and
 * BURST_POST_SAMPLES after it (~5 ms apart). Finished bursts are queued (a few at most; if the
 * queue is full the burst is dropped, which shows as a gap in the burst numbers).
 *
 * impact_burst_line() formats the next text line of the oldest queued burst into buf (no
 * newline): first a header line, then data lines of 16 samples each. Returns false when nothing
 * is queued. Call it from ONE thread only (the main loop), one line per pass, so printing a
 * burst never stalls ranging. */
#define BURST_PRE_SAMPLES 100
#define BURST_POST_SAMPLES 40
bool impact_burst_line(char *buf, size_t size);

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
