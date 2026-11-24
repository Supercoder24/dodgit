/*
 * dodgit - accelerometer log lines and the buffers behind them (pure C: no Zephyr, no malloc, no
 * globals, so tests/accel_log_test.c runs them on the host). ball/src/impact.c owns the instances, the
 * threads and the queues; this file only formats lines and does the index bookkeeping.
 *
 * Every formatter writes at most size-1 characters plus the terminating NUL and no newline. The
 * ball's log keeps up to BLE_LOG_LINE_MAX = 159 characters of a line (ble_log.h), so a buffer of
 * ACCEL_LOG_LINE_SIZE = 160 bytes holds every line the ball can print and nothing is cut. Times are
 * the ball's uptime in ms (the same clock as STATUS), mg values are milli-g.
 *
 * Line families (tools/ball_monitor.py colors them by their prefix):
 *
 *   ACCEL level <A> -> <B> up=<ms> after=<ms in A>       the classifier's level changed (A, B: UNKNOWN /
 *                                                         REST / HELD / SPINNING)
 *   ACCEL hit up=<ms> mg=<n> level=<L>                    a Hit event
 *   ACCEL stalled up=<ms>, sensor re-initialized (reset #<n>, data is back|still no data)
 *                                                         the sensor stopped delivering data and was
 *                                                         re-initialized (always printed, whatever the
 *                                                         flags; up= is the time of the re-init). Printed
 *                                                         once the outcome is known: "data is back" when
 *                                                         the first valid sample followed, "still no
 *                                                         data" when the next re-init (ACCEL_RECOVER_
 *                                                         RETRY_MS later) was due
 *   FEAT up=<ms> level=<L> shape=<S> mean=<mg> trend=<mg> masd=<mg> samples=<n>
 *                                                         the classifier's window features (see
 *                                                         accel_class.h for mean / trend / masd)
 *
 *   BURST #<n> trigger up=<ms> mg=<mg> hit|cooldown level=<L> also=<k> samples=<n> pre=<n>
 *   BURST #<n> <t0>..<t1> ms: v v v ...                  (16 values per line)
 *       The |a| samples around a threshold crossing. "hit": the crossing was reported as a Hit;
 *       "cooldown": it was inside IMPACT_COOLDOWN_MS of the previous Hit and was not reported.
 *       also= further crossings inside the same window; samples= values in the window, pre= how many
 *       of them are up to and including the crossing. <t0>..<t1> are the times (ms, relative to the
 *       trigger) of the first and last value of the line; the ones between are evenly spread. Burst
 *       numbers are consecutive: a gap means the ball dropped a burst (its queue was full).
 *
 *   RAW #<n> begin up=<ms> rate=<Hz> samples=<n>          one capture of the last ACCEL_RAW_CAPTURE_MS
 *   RAW #<n> <t_offset_ms>: x,y,z x,y,z ...               up to ACCEL_RAW_PER_LINE samples per line
 *   RAW #<n> end
 *       up= is the time of the first (oldest) sample, rate= the average rate over the capture, in Hz.
 *       Each data line starts with the offset (ms after up=) of ITS first sample; the samples after
 *       it are 1000/rate ms apart (the real spacing jitters by a millisecond or so; the offset of the
 *       next line corrects the drift). x, y, z are milli-g per axis (clipped to the int16 range).
 *       A line holds fewer than ACCEL_RAW_PER_LINE samples if they would not fit in the line.
 *       A gap in the offsets (much more than the usual 6-7 ms between samples) is a sensor stall, or
 *       samples the ball discarded as failed reads (impact.c: an all-zero reading counts as one, and a
 *       real free fall can read all zeros now and then), or a line the ball's log dropped (about 8
 *       samples, 50 ms; the begin line's samples= is then larger than what arrived).
 *
 * Paced output (RAW): the Bluetooth link takes only a few KB/s and the ball's log queue holds 48
 * lines, so impact.c emits one RAW line per ACCEL_RAW_LINE_MS. A line is at most 159 characters
 * (about 110 for typical values): at 40 ms that is at most 25 lines/s = 4 KB/s (about 2.8 KB/s
 * typical), which the link sustains. The console (115200 baud, about 11 KB/s) is busy for about
 * a quarter of the time while it dumps, and the main loop prints on it itself (immediate log mode):
 * while it does, the sampling thread, which has the lower priority, does not run. During a dump the
 * sample stream therefore has a hole of 10-15 ms about every 46 ms, and levels and Hits seen then are
 * not representative of normal play.
 */
#ifndef DODGIT_ACCEL_LOG_H
#define DODGIT_ACCEL_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "accel_class.h"
#include "settings.h"

#define ACCEL_LOG_LINE_SIZE 160 /* the buffer size every line is designed for: 159 characters and the NUL */

/* ---- Class / feature lines ---------------------------------------------------------------- */

/* Each returns the length of the line written to buf. */
size_t accel_log_level_line(char *buf, size_t size, uint32_t up_ms, enum accel_level from,
                            enum accel_level to, uint32_t after_ms);
size_t accel_log_hit_line(char *buf, size_t size, uint32_t up_ms, int32_t mg, enum accel_level level);
size_t accel_log_stall_line(char *buf, size_t size, uint32_t up_ms, uint32_t reset_no, bool data_is_back);
size_t accel_log_feat_line(char *buf, size_t size, uint32_t up_ms, enum accel_level level,
                           const struct accel_features *f);

/* ---- BURST: |a| samples around a threshold crossing ----------------------------------------- */

#define ACCEL_BURST_MAX (BURST_PRE_SAMPLES + BURST_POST_SAMPLES)
#define ACCEL_BURST_LINE_SAMPLES 16

enum accel_burst_kind
{
  ACCEL_BURST_HIT,      /* the crossing was reported as a Hit */
  ACCEL_BURST_COOLDOWN, /* the crossing was inside IMPACT_COOLDOWN_MS of the previous Hit: not reported */
};

/* One window of samples. */
struct accel_burst
{
  uint32_t seq;
  uint32_t first_ms;    /* time of mg[0] */
  uint32_t trigger_ms;
  int32_t trigger_mg;
  uint8_t kind;         /* enum accel_burst_kind */
  uint8_t level;        /* enum accel_level at the trigger */
  uint8_t extra;        /* further crossings inside this window (the "also=" of the header) */
  uint16_t n;           /* samples stored */
  uint16_t n_pre;       /* samples up to and including the crossing */
  uint16_t t_off[ACCEL_BURST_MAX]; /* ms since first_ms */
  int16_t mg[ACCEL_BURST_MAX];
};

/* Recorder: keeps the last BURST_PRE_SAMPLES samples and collects the window around a crossing. A
 * zeroed struct is ready to use. Single thread (the sampling thread). */
struct accel_burst_rec
{
  struct
  {
    uint32_t t;
    int16_t mg;
  } pre[BURST_PRE_SAMPLES]; /* ring of the newest samples */
  uint16_t next;            /* next write position */
  uint16_t len;             /* valid entries */
  struct accel_burst win;   /* the window being collected; complete once a call returned true */
  bool active;
  uint16_t post_left;
  uint32_t seq;             /* number of the latest window started */
};

/* Feed every valid sample (before accel_burst_trigger for the same sample). Returns true when this
 * sample completed the window in r->win; the caller queues it (a new window may start right away). */
bool accel_burst_sample(struct accel_burst_rec *r, uint32_t t_ms, int32_t mg);

/* A crossing at the sample just fed. Starts a window (returns true) unless one is being collected,
 * in which case it only counts in "also=". */
bool accel_burst_trigger(struct accel_burst_rec *r, uint32_t t_ms, int32_t mg, enum accel_burst_kind kind,
                         enum accel_level level);

/* The sample stream broke (sensor stall): forget the history, and end the window being collected
 * with what it has. Returns true if there was one (r->win is complete, as after accel_burst_sample). */
bool accel_burst_break(struct accel_burst_rec *r);

size_t accel_burst_header_line(char *buf, size_t size, const struct accel_burst *b);

/* The data line for the samples from index `first` (up to ACCEL_BURST_LINE_SAMPLES); returns the
 * index of the first sample the next line starts with (== b->n when done). */
uint16_t accel_burst_data_line(char *buf, size_t size, const struct accel_burst *b, uint16_t first);

/* ---- RAW: x/y/z history ------------------------------------------------------------------------ */

#define ACCEL_RAW_PER_LINE 8
/* The sensor is polled every ACCEL_SAMPLE_PERIOD_MS at the very fastest (really about 1 ms slower),
 * so this many samples always cover at least ACCEL_RAW_CAPTURE_MS. */
#define ACCEL_RAW_RING_SIZE (ACCEL_RAW_CAPTURE_MS / ACCEL_SAMPLE_PERIOD_MS)

struct accel_raw_sample
{
  int16_t x, y, z; /* mg */
  uint16_t dt_ms;  /* time since the previous sample (saturates at 65535: a long stall) */
};

/* Ring of the newest ACCEL_RAW_RING_SIZE samples, 8 bytes each. A zeroed struct is an empty ring.
 * The writer (sampling thread) and the dump reader (main thread) must never run at the same time;
 * impact.c freezes the writer for the length of a dump. */
struct accel_raw_ring
{
  struct accel_raw_sample s[ACCEL_RAW_RING_SIZE];
  uint16_t head;      /* next write position */
  uint16_t count;     /* valid samples */
  uint32_t t_newest;  /* time of the newest sample */
};

void accel_raw_reset(struct accel_raw_ring *r);
/* x, y, z are clipped to the int16 range. */
void accel_raw_push(struct accel_raw_ring *r, uint32_t t_ms, int32_t x, int32_t y, int32_t z);

/* A dump in progress: header, data lines, end line. */
struct accel_raw_dump
{
  uint32_t seq;
  uint16_t next;   /* index of the next sample to print (0 = the oldest) */
  uint32_t t_off;  /* ms from the first sample to sample `next` */
  uint8_t stage;
};

void accel_raw_dump_begin(struct accel_raw_dump *d, uint32_t seq);
/* Writes the next line of the dump of ring r (which must not change meanwhile). Returns false if
 * the dump was already finished before this call. */
bool accel_raw_dump_line(struct accel_raw_dump *d, const struct accel_raw_ring *r, char *buf, size_t size);
/* True once the end line has been written. */
bool accel_raw_dump_done(const struct accel_raw_dump *d);

#endif
