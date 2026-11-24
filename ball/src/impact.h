/*
 * dodgit - ball accelerometer service.
 *
 * A background thread reads the on-board LIS2DH12 every ACCEL_SAMPLE_PERIOD_MS, computes |a| in
 * milli-g and feeds it to the classifier (accel_class.h, docs/Accelerometer.md). The ball's state
 * machine (main.c) reads two things from here:
 *   - the LEVEL: Rest / Held / Spinning (or Unknown while the sensor is stalled), plus how long it has
 *     been that way;
 *   - EVENTS: every Hit (a sharp spike >= IMPACT_MG, one per physical impact) and every sensor stall
 *     (the LIS2DH often stops delivering data after a hard shock, which is itself a sign of one).
 *     The service reports them ALL, in every state; the state machine decides what they mean (only
 *     Thrown acts on them) and must drain the queue in the other states, or old events would pile up.
 *
 * Stall recovery: if the reads keep failing for ACCEL_STALL_MS (an all-zero reading counts as failed)
 * the thread re-initializes the sensor from software (the LIS2DH has no reset pin), resets the
 * classifier and goes on; a pause of the thread itself is not a failed read. Every stall episode
 * gets a re-init, and one more every ACCEL_RECOVER_RETRY_MS while there is still no data.
 *
 * Log lines (impact_log_line) exist only for the categories enabled in shared/settings.h
 * (LOG_ACCEL_*); a disabled category costs no code and no RAM.
 */
#ifndef DODGIT_IMPACT_H
#define DODGIT_IMPACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "accel_class.h"
#include "accel_log.h" /* ACCEL_LOG_LINE_SIZE */

struct impact_event
{
  int64_t time;  /* k_uptime_get() when it happened (ms) */
  int32_t mg;    /* Hit: magnitude of the spike, milli-g (the sensor saturates at about 41400, see
                  * accel_class.h); stall: 0 */
  bool stall;    /* false = Hit, true = the sensor stopped delivering data (and is being re-initialized) */
};

/* Configure the accelerometer (+/-16 g range, 400 Hz) and start the thread. Returns 0 or -errno. */
int impact_init(void);

/* The classifier's current level (ACCEL_UNKNOWN while the sensor is stalled or just re-initialized)
 * and how long (ms) it has been that level. Each call is one atomic load. Two calls are a consistent
 * pair only if the level cannot change in between: call them back to back from the main loop, which
 * has the higher priority than the sampling thread and does not sleep between them. */
enum accel_level impact_level(void);
uint32_t impact_level_ms(void);

/* Pop the oldest queued event. Returns false if there is none. The queue is small (4): drain it on every
 * main-loop pass. If it ever overflows the OLDEST events are dropped, so the newest always get through. */
bool impact_get(struct impact_event *event);

/* Diagnostics for the STATUS line. */
int32_t impact_take_peak(void); /* largest |a| (mg) since the previous call, -1 if there was no sample */
int32_t impact_take_min(void);  /* lowest |a| (mg) since the previous call, -1 if there was no sample */
uint32_t impact_errors(void);   /* failed sensor reads and failed re-initializations since boot */
uint32_t impact_resets(void);   /* times the sensor was re-initialized after a stall */

/* Next pending text line (no newline, at most size-1 characters; size >= ACCEL_LOG_LINE_SIZE = 160, which
 * is what the ball's log can carry) of the enabled LOG_ACCEL_* categories, or false if there is none. Call
 * it from ONE thread (the main loop), once per pass: long outputs (BURST windows, RAW dumps) are spread
 * over many calls, so one pass prints one line instead of a whole window (the console takes about 9 ms
 * for a 100 character line, and the sampling thread cannot run meanwhile), and a RAW dump is paced to
 * what the Bluetooth link can take. The exact formats are in accel_log.h. Line families:
 *   ACCEL level <A> -> <B> up=<ms> after=<ms in A>      LOG_ACCEL_CLASS  (level change)
 *   ACCEL hit up=<ms> mg=<n> level=<L>                  LOG_ACCEL_CLASS  (a Hit event was queued)
 *   ACCEL stalled up=<ms>, sensor re-initialized (reset #<n>, ...)   ALWAYS (a warning, whatever the
 *       flags; up= is the time of the re-init, the line itself comes up to ACCEL_RECOVER_RETRY_MS later;
 *       a sensor that does not come back gets the line for its first re-inits and then for every 20th)
 *   FEAT up=<ms> level=<L> shape=<S> mean=<mg> trend=<mg> masd=<mg> samples=<n>   LOG_ACCEL_FEATURES
 *   BURST #<n> trigger ... / BURST #<n> <t0>..<t1> ms: v v v ...                 LOG_ACCEL_BURST (header + data)
 *   RAW #<n> begin ... / RAW #<n> <t_offset>: x,y,z ... / RAW #<n> end           LOG_ACCEL_RAW
 * tools/ball_monitor.py colors lines by these prefixes. */
bool impact_log_line(char *buf, size_t size);

/* LOG_ACCEL_RAW only (no-op otherwise): snapshot the last ACCEL_RAW_CAPTURE_MS or more (the ring holds
 * ACCEL_RAW_CAPTURE_MS / ACCEL_SAMPLE_PERIOD_MS samples: about 12 s at the real sample spacing) of raw
 * x/y/z samples, kept in a RAM ring at the sample rate, and dump them through impact_log_line()
 * afterwards, paced to the link. The ball calls it on a short button press outside Roll Call, so a
 * labeled capture is: do the movement, press the button. A capture requested while a dump is running is
 * ignored. The ring is not recorded during a dump and starts empty after it, so a capture soon after a
 * dump is short. While a dump runs the main loop spends about a quarter of its time on the console and
 * the sampling thread cannot run then (holes of 10-15 ms in the sample stream): levels and Hits during
 * a dump are not representative. */
void impact_raw_capture(void);

#endif
