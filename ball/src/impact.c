/*
 * dodgit - ball accelerometer service: the sampling thread and its hand-over to the main loop.
 * The API and what the ball does with it are in impact.h; the pure classifier is accel_class.[ch]; the
 * log line formats and the burst / raw buffers (pure, host-tested) are accel_log.[ch]. This file is
 * only glue: sensor I/O, stall recovery, queues, and which of the LOG_ACCEL_* categories exist.
 *
 * The thread (K_THREAD_DEFINE below) polls the LIS2DH12 every ACCEL_SAMPLE_PERIOD_MS, computes |a|
 * in milli-g and feeds it to the classifier. It formats nothing: it hands small records to the main
 * loop, which turns them into text in impact_log_line() on its own (larger) stack.
 *
 * Who touches what (T = sampling thread, M = main loop):
 *   impact_event_q  k_msgq of struct impact_event. T puts Hits and stalls, M takes them (impact_get). When
 *                 it is full T drops the OLDEST event: the ball acts on a Hit only in Thrown and drains
 *                 the queue on every pass in every other state, so what is still queued when it overflows
 *                 is stale, while the newest events are the ones about the throw the ball is looking at.
 *   level_word    one atomic word: the level (2 bits) and the ms it began (30 bits). T writes it when the
 *                 level changes, M reads it with one atomic load per call, so one call never sees a torn
 *                 value and nobody ever blocks. (30 bits of ms = 12 days: a level older than that reads
 *                 wrapped.) impact_level() followed by impact_level_ms() is a consistent pair only because
 *                 M has the higher priority and does not sleep in between: T cannot change the level then.
 *   peak_mg, min_mg   atomics. T raises / lowers them, M swaps them to -1 (impact_take_*). A sample
 *                 landing between T's read and write of a swap is harmless: the value is still a real
 *                 sample of the new period.
 *   error_count, reset_count, running   atomics, written by T (running: by impact_init once, after the
 *                 thread-only variables below are set up), read by M.
 *   impact_class_q, impact_feat_q, impact_burst_q   k_msgq of log records, T -> M. Drop-when-full (the
 *                 newest is dropped; a gap in the BURST numbers shows it). Only the queues of enabled
 *                 categories exist, except impact_class_q, which always carries the stall warning. (They
 *                 are global symbols, hence the prefix.)
 *   raw_state     atomic handshake of the RAW capture, see raw_thread_poll(). raw_ring is written by T
 *                 only while the state is IDLE and read by M only while it is FROZEN: never both.
 *   everything else: private to T (classifier, burst recorder, stall state, ...) or to M (the burst /
 *                 raw line state in impact_log_line).
 * No lock is ever held across a sleep. T blocks in the k_msleep that paces it, in the I2C transfer of
 * the sensor (up to CONFIG_I2C_NRFX_TRANSFER_TIMEOUT = 500 ms if the bus hangs) and in the 5 ms
 * k_sleep of lis2dh_init_chip(). No printk in T.
 *
 * Stack: T has 2048 bytes. Its own frames are small (the burst window, the raw ring and the classifier
 * are static); the formatting (snprintf) happens only in M, whose stack is 4096 bytes.
 */
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/atomic.h>
#include "accel_log.h"
#include "ble_log.h"
#include "impact.h"
#include "settings.h"

/* The log keeps BLE_LOG_LINE_MAX characters of a line. The formatters cut at ACCEL_LOG_LINE_SIZE - 1: if
 * that were more, the last digits of a RAW line would be lost without anybody noticing. */
BUILD_ASSERT(ACCEL_LOG_LINE_SIZE - 1 <= BLE_LOG_LINE_MAX, "ble_log.c would cut accelerometer log lines");

/* Zephyr's LIS2DH driver: re-writes CTRL1..6 to the boot defaults and re-enables measurement.
 * Not declared in any public header, but it is a global function of the driver. */
extern int lis2dh_init_chip(const struct device *dev);

static const struct device *const accel = DEVICE_DT_GET(DT_ALIAS(accel0));

/* ---- Shared state (see the table at the top) -------------------------------------------------- */

#define EVENT_QUEUE_DEPTH 4 /* Hits are >= IMPACT_COOLDOWN_MS apart and the ball drains every pass */
K_MSGQ_DEFINE(impact_event_q, sizeof(struct impact_event), EVENT_QUEUE_DEPTH, 4);

#define LEVEL_TIME_MASK 0x3FFFFFFFu /* the ms part of level_word */
BUILD_ASSERT(ACCEL_SPINNING < 4, "enum accel_level must fit the 2 bits of level_word");
static atomic_t level_word; /* (ms the level began << 2) | enum accel_level; starts as UNKNOWN since 0 */

static atomic_t running;
static atomic_t peak_mg = ATOMIC_INIT(-1); /* max |a| since impact_take_peak(), -1 = no sample */
static atomic_t min_mg = ATOMIC_INIT(-1);  /* min |a| since impact_take_min(), -1 = no sample */
static atomic_t error_count;
static atomic_t reset_count;

/* ---- Log records (T -> M) ---------------------------------------------------------------------- */

enum class_rec_type
{
  REC_LEVEL, /* from -> level, value = ms spent in `from` */
  REC_HIT,   /* value = mg, level = the level at the Hit */
  REC_STALL, /* value = the number of the re-init (reset #n), time = when it was done, data_back = whether
              * data followed it */
};

struct class_rec
{
  uint32_t time; /* up= */
  uint32_t value;
  uint8_t type;  /* enum class_rec_type */
  uint8_t from;  /* enum accel_level */
  uint8_t level; /* enum accel_level */
  uint8_t data_back;
};
/* Level changes are at least ACCEL_MIN_DWELL_MS apart; with LOG_ACCEL_CLASS off only the stall
 * warning (at most one per ACCEL_STALL_MS) uses this queue. */
#define CLASS_QUEUE_DEPTH (LOG_ACCEL_CLASS ? 16 : 4)
K_MSGQ_DEFINE(impact_class_q, sizeof(struct class_rec), CLASS_QUEUE_DEPTH, 4);

#if LOG_ACCEL_FEATURES
struct feat_rec
{
  uint32_t time;
  struct accel_features f;
  uint8_t level;
};
K_MSGQ_DEFINE(impact_feat_q, sizeof(struct feat_rec), 4, 4);
#endif

#if LOG_ACCEL_BURST
K_MSGQ_DEFINE(impact_burst_q, sizeof(struct accel_burst), 3, 4); /* finished windows */
#endif

#if LOG_ACCEL_RAW
/* The capture handshake. The sampling thread owns the ring while IDLE, the main loop while FROZEN:
 *   IDLE      -> REQUESTED   main loop (impact_raw_capture, only from IDLE: a second request is ignored)
 *   REQUESTED -> FROZEN      thread, at the top of its next pass: the ring stops changing
 *   FROZEN    -> DONE        main loop, after the last line of the dump
 *   DONE      -> IDLE        thread: it empties the ring and records again (the capture starts afresh)
 * While FROZEN nothing is recorded (a dump takes about as long as the capture it shows). */
enum raw_capture_state
{
  RAW_STATE_IDLE,
  RAW_STATE_REQUESTED,
  RAW_STATE_FROZEN,
  RAW_STATE_DONE,
};
static atomic_t raw_state = ATOMIC_INIT(RAW_STATE_IDLE);
static struct accel_raw_ring raw_ring;
#endif

/* ---- Sampling thread only ---------------------------------------------------------------------- */

static struct accel_class classifier;
static enum accel_level cur_level;  /* the level last published */
static uint32_t cur_level_since;    /* ms (uptime) it began */
static uint32_t reinit_pending;     /* number of the re-init whose outcome is not known yet, 0 = none */
static uint32_t reinit_time;        /* ms (uptime) of that re-init */
static uint32_t episode_reinits;    /* re-inits so far in the current stall episode (the pending one too) */
#if LOG_ACCEL_BURST
static struct accel_burst_rec burst_rec;
#endif

/* Full scale +/-16 g (impacts are far above the +/-2 g default) and 400 Hz. */
static int accel_configure(void)
{
  struct sensor_value val;

  sensor_g_to_ms2(16, &val);
  int rc = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &val);
  if (rc)
  {
    return rc;
  }
  val.val1 = 400;
  val.val2 = 0;
  return sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &val);
}

/* Acceleration in milli-g: sensor_value is in m/s^2, 1 g = 9.80665 m/s^2. */
static int32_t to_mg(const struct sensor_value *v)
{
  return (int32_t)(sensor_value_to_micro(v) * 100 / 980665);
}

/* One reading, x/y/z in mg. False if there is no valid sample: -ENODATA (data-ready bit never set),
 * a read error (counted), or an all-zero sample, which is taken to be the stalled state. (A real free
 * fall can read all zeros now and then; that sample is lost too, so RAW and BURST can have a hole
 * there and min= never shows 0. Keeping such samples would feed a stalled sensor's zeros to the
 * classifier as free fall, and the classifier was tuned on data without them.) */
static bool read_sample(int32_t *x, int32_t *y, int32_t *z)
{
  struct sensor_value v[3];

  int rc = sensor_sample_fetch(accel);
  if (!rc)
  {
    rc = sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, v);
  }
  if (rc)
  {
    if (rc != -ENODATA)
    {
      atomic_inc(&error_count);
    }
    return false;
  }
  if (sensor_value_to_micro(&v[0]) == 0 && sensor_value_to_micro(&v[1]) == 0 &&
      sensor_value_to_micro(&v[2]) == 0)
  {
    return false;
  }
  *x = to_mg(&v[0]);
  *y = to_mg(&v[1]);
  *z = to_mg(&v[2]);
  return true;
}

static void class_put(enum class_rec_type type, uint32_t time, enum accel_level from, enum accel_level level,
                      uint32_t value, bool data_back)
{
  struct class_rec rec = {.time = time,
                          .value = value,
                          .type = (uint8_t)type,
                          .from = (uint8_t)from,
                          .level = (uint8_t)level,
                          .data_back = data_back};

  k_msgq_put(&impact_class_q, &rec, K_NO_WAIT); /* full: dropped */
}

/* Queue the newest event; if the queue is full the oldest goes (see the table at the top). The ball may
 * take an event between the two puts: then the get finds nothing and the second put succeeds anyway. */
static void event_put(int64_t time, int32_t mg, bool stall)
{
  struct impact_event ev = {.time = time, .mg = mg, .stall = stall};

  if (k_msgq_put(&impact_event_q, &ev, K_NO_WAIT))
  {
    struct impact_event stale;
    k_msgq_get(&impact_event_q, &stale, K_NO_WAIT);
    k_msgq_put(&impact_event_q, &ev, K_NO_WAIT);
  }
}

/* The classifier's level at time t (ms): publish it (and the time it began) and log the change. */
static void level_set(uint32_t t, enum accel_level to)
{
  if (to == cur_level)
  {
    return;
  }
  if (LOG_ACCEL_CLASS)
  {
    class_put(REC_LEVEL, t, cur_level, to, t - cur_level_since, false);
  }
  cur_level = to;
  cur_level_since = t;
  atomic_set(&level_word, (atomic_val_t)((t << 2) | (uint32_t)to));
}

#if LOG_ACCEL_BURST
static void burst_queue(void)
{
  k_msgq_put(&impact_burst_q, &burst_rec.win, K_NO_WAIT); /* queue full: dropped, visible as a gap in the numbers */
}
#endif

#if LOG_ACCEL_RAW
/* The thread's half of the capture handshake. Returns true if this pass should record the sample. */
static bool raw_thread_poll(void)
{
  switch (atomic_get(&raw_state))
  {
  case RAW_STATE_REQUESTED:
    atomic_set(&raw_state, RAW_STATE_FROZEN); /* from here on the main loop owns the ring */
    return false;
  case RAW_STATE_DONE:
    accel_raw_reset(&raw_ring);
    atomic_set(&raw_state, RAW_STATE_IDLE);
    return true;
  case RAW_STATE_IDLE:
    return true;
  default:
    return false; /* FROZEN: being dumped */
  }
}
#endif

/* The reads have been failing for ACCEL_STALL_MS: the first pass of a stall episode. Tell the ball (an
 * accelerometer that stops after a hard shock is itself a sign of an impact), and drop everything that
 * depended on the old stream: the classifier restarts (level UNKNOWN), the burst history is gone and a
 * window in progress ends with what it has. The raw ring keeps its samples: the gap shows in their
 * timestamps, and what happened just before a stall is what a capture is for. */
static void stall_begin(int64_t now)
{
  episode_reinits = 0;
  event_put(now, 0, true);
  accel_class_reset(&classifier);
  level_set((uint32_t)now, ACCEL_UNKNOWN);
#if LOG_ACCEL_BURST
  if (accel_burst_break(&burst_rec))
  {
    burst_queue();
  }
#endif
}

/* A sensor that never comes back is re-initialized every ACCEL_RECOVER_RETRY_MS. Its "still no data"
 * line is printed for the first STALL_LINES_FULL re-inits of an episode and then for every
 * STALL_LINES_EVERY-th, so a dead sensor does not bury the log (the numbers show what was skipped). */
#define STALL_LINES_FULL 3
#define STALL_LINES_EVERY 20

/* The stall warning of the re-init that is pending, once it is known whether data followed it. (Not at
 * the re-init itself: no sample can have arrived yet, so the line would nearly always say "still no
 * data", even when the sensor is back 5 ms later.) */
static void reinit_report(bool data_is_back)
{
  if (!reinit_pending)
  {
    return;
  }
  if (data_is_back || episode_reinits <= STALL_LINES_FULL || episode_reinits % STALL_LINES_EVERY == 0)
  {
    class_put(REC_STALL, reinit_time, ACCEL_UNKNOWN, ACCEL_UNKNOWN, reinit_pending, data_is_back);
  }
  reinit_pending = 0;
}

/* Re-initialize the sensor from software (the LIS2DH has no reset pin). Called at the start of every
 * stall episode, and again every ACCEL_RECOVER_RETRY_MS while there is still no data, which is also when
 * the previous attempt is known to have failed. A failed re-init counts as an error. */
static void sensor_recover(uint32_t now)
{
  reinit_report(false);
  int rc = lis2dh_init_chip(accel);

  if (!rc)
  {
    rc = accel_configure();
  }
  if (rc)
  {
    atomic_inc(&error_count);
  }
  reinit_pending = (uint32_t)atomic_inc(&reset_count) + 1;
  reinit_time = now;
  episode_reinits++;
}

/* One valid sample at time `now`. */
static void on_sample(int64_t now, int32_t x, int32_t y, int32_t z)
{
  uint32_t t = (uint32_t)now;
  int64_t sq = (int64_t)x * x + (int64_t)y * y + (int64_t)z * z;
  int32_t mg = (int32_t)sqrt((double)sq);

  /* Diagnostics for the STATUS line: peak and lowest |a| per period. */
  if (mg > atomic_get(&peak_mg))
  {
    atomic_set(&peak_mg, mg);
  }
  int32_t lowest = (int32_t)atomic_get(&min_mg);
  if (lowest < 0 || mg < lowest)
  {
    atomic_set(&min_mg, mg);
  }

#if LOG_ACCEL_BURST
  if (accel_burst_sample(&burst_rec, t, mg))
  {
    burst_queue();
  }
#endif

  struct accel_result r = accel_class_update(&classifier, t, mg);
  level_set(t, r.level);
  if (r.hit)
  {
    event_put(now, r.hit_mg, false);
    if (LOG_ACCEL_CLASS)
    {
      class_put(REC_HIT, t, ACCEL_UNKNOWN, r.level, (uint32_t)r.hit_mg, false);
    }
  }

#if LOG_ACCEL_BURST
  if (r.hit || r.hit_suppressed)
  {
    accel_burst_trigger(&burst_rec, t, r.hit_mg, r.hit ? ACCEL_BURST_HIT : ACCEL_BURST_COOLDOWN, r.level);
  }
#endif

#if LOG_ACCEL_FEATURES
  static uint32_t next_feat_ms;
  if ((int32_t)(t - next_feat_ms) >= 0)
  {
    struct feat_rec rec = {.time = t, .f = accel_class_features(&classifier), .level = (uint8_t)r.level};

    next_feat_ms = t + ACCEL_FEATURES_LOG_MS;
    k_msgq_put(&impact_feat_q, &rec, K_NO_WAIT); /* full: dropped */
  }
#endif
}

static void sample_thread(void *a, void *b, void *c)
{
  int64_t first_fail_ms = -1; /* when the current run of failed reads began, -1 = the last read was good */
  int64_t next_recover_ms = 0;
  bool stalled = false;

  while (1)
  {
    k_msleep(ACCEL_SAMPLE_PERIOD_MS);
    if (!atomic_get(&running))
    {
      continue;
    }
#if LOG_ACCEL_RAW
    bool record_raw = raw_thread_poll();
#endif

    int64_t now = k_uptime_get();
    int32_t x, y, z;

    if (!read_sample(&x, &y, &z))
    {
      /* A hard shock can leave the LIS2DH delivering nothing: re-initialize it once the reads have been
       * failing for a while, then again every ACCEL_RECOVER_RETRY_MS until data is back. The time runs
       * from the first failed read, not from the last good sample: if this thread was not scheduled
       * for a while (the main loop has the higher priority) that gap says nothing about the sensor. */
      if (first_fail_ms < 0)
      {
        first_fail_ms = now;
      }
      if (now - first_fail_ms > ACCEL_STALL_MS)
      {
        if (!stalled)
        {
          stalled = true;
          stall_begin(now);
          next_recover_ms = now; /* every episode gets its own re-init, so its own line */
        }
        if (now >= next_recover_ms)
        {
          next_recover_ms = now + ACCEL_RECOVER_RETRY_MS;
          sensor_recover((uint32_t)now);
        }
      }
      continue;
    }
    first_fail_ms = -1;
    stalled = false;
    reinit_report(true);

#if LOG_ACCEL_RAW
    if (record_raw)
    {
      accel_raw_push(&raw_ring, (uint32_t)now, x, y, z);
    }
#endif
    on_sample(now, x, y, z);
  }
}
K_THREAD_DEFINE(impact_thread, 2048, sample_thread, NULL, NULL, NULL, 5, 0, 0);

/* ---- Public API (main loop) -------------------------------------------------------------------- */

int impact_init(void)
{
  if (!device_is_ready(accel))
  {
    return -ENODEV;
  }
  int rc = accel_configure();
  if (rc)
  {
    return rc;
  }

  /* The thread-only variables are set up here, before `running` lets the thread loose (atomic_set is a
   * full barrier). */
  accel_class_reset(&classifier);
  cur_level = ACCEL_UNKNOWN;
  cur_level_since = k_uptime_get_32();
  atomic_set(&level_word, (atomic_val_t)((cur_level_since << 2) | (uint32_t)ACCEL_UNKNOWN));
  atomic_set(&running, 1);
  return 0;
}

enum accel_level impact_level(void)
{
  return (enum accel_level)((uint32_t)atomic_get(&level_word) & 3u);
}

uint32_t impact_level_ms(void)
{
  uint32_t word = (uint32_t)atomic_get(&level_word); /* the word first: the clock is never earlier than it */

  return (k_uptime_get_32() - (word >> 2)) & LEVEL_TIME_MASK;
}

bool impact_get(struct impact_event *event)
{
  return k_msgq_get(&impact_event_q, event, K_NO_WAIT) == 0;
}

int32_t impact_take_peak(void)
{
  return (int32_t)atomic_set(&peak_mg, -1); /* returns the old value */
}

int32_t impact_take_min(void)
{
  return (int32_t)atomic_set(&min_mg, -1); /* returns the old value */
}

uint32_t impact_errors(void)
{
  return (uint32_t)atomic_get(&error_count);
}

uint32_t impact_resets(void)
{
  return (uint32_t)atomic_get(&reset_count);
}

/* ---- Log lines (main loop only) ---------------------------------------------------------------- */

/* Stall warnings (always) and, with LOG_ACCEL_CLASS, level changes and Hits. */
static bool class_line(char *buf, size_t size)
{
  struct class_rec rec;

  if (k_msgq_get(&impact_class_q, &rec, K_NO_WAIT))
  {
    return false;
  }
  switch (rec.type)
  {
#if LOG_ACCEL_CLASS
  case REC_LEVEL:
    accel_log_level_line(buf, size, rec.time, (enum accel_level)rec.from, (enum accel_level)rec.level,
                         rec.value);
    break;
  case REC_HIT:
    accel_log_hit_line(buf, size, rec.time, (int32_t)rec.value, (enum accel_level)rec.level);
    break;
#endif
  default: /* REC_STALL */
    accel_log_stall_line(buf, size, rec.time, rec.value, rec.data_back);
    break;
  }
  return true;
}

#if LOG_ACCEL_FEATURES
static bool feat_line(char *buf, size_t size)
{
  struct feat_rec rec;

  if (k_msgq_get(&impact_feat_q, &rec, K_NO_WAIT))
  {
    return false;
  }
  accel_log_feat_line(buf, size, rec.time, (enum accel_level)rec.level, &rec.f);
  return true;
}
#endif

#if LOG_ACCEL_BURST
static struct accel_burst burst_out; /* the window being printed: a header line, then 16 values per line */
static bool burst_out_valid;
static uint16_t burst_out_pos;       /* 0 = header next, else index of the next value */

static bool burst_line(char *buf, size_t size)
{
  if (!burst_out_valid)
  {
    if (k_msgq_get(&impact_burst_q, &burst_out, K_NO_WAIT))
    {
      return false;
    }
    burst_out_valid = true;
    burst_out_pos = 0;
    accel_burst_header_line(buf, size, &burst_out);
    return true;
  }
  burst_out_pos = accel_burst_data_line(buf, size, &burst_out, burst_out_pos);
  burst_out_valid = burst_out_pos < burst_out.n;
  return true;
}
#endif

#if LOG_ACCEL_RAW
static struct accel_raw_dump raw_dump;
static bool raw_dumping;
static uint32_t raw_seq;
static int64_t raw_next_ms; /* earliest time of the next line of the dump */

/* One line of the dump of a frozen capture, at most one per ACCEL_RAW_LINE_MS (accel_log.h has the
 * arithmetic): the passes between are free for the other categories and for ranging. */
static bool raw_line(char *buf, size_t size)
{
  if (atomic_get(&raw_state) != RAW_STATE_FROZEN)
  {
    return false;
  }
  int64_t now = k_uptime_get();
  if (!raw_dumping)
  {
    raw_dumping = true;
    accel_raw_dump_begin(&raw_dump, ++raw_seq);
    raw_next_ms = now;
  }
  if (now < raw_next_ms)
  {
    return false;
  }
  raw_next_ms = now + ACCEL_RAW_LINE_MS;
  bool wrote = accel_raw_dump_line(&raw_dump, &raw_ring, buf, size);
  if (accel_raw_dump_done(&raw_dump))
  {
    raw_dumping = false;
    atomic_set(&raw_state, RAW_STATE_DONE); /* the thread takes the ring back */
  }
  return wrote;
}
#endif

bool impact_log_line(char *buf, size_t size)
{
  if (class_line(buf, size))
  {
    return true;
  }
#if LOG_ACCEL_FEATURES
  if (feat_line(buf, size))
  {
    return true;
  }
#endif
#if LOG_ACCEL_BURST
  if (burst_line(buf, size))
  {
    return true;
  }
#endif
#if LOG_ACCEL_RAW
  if (raw_line(buf, size))
  {
    return true;
  }
#endif
  return false;
}

void impact_raw_capture(void)
{
#if LOG_ACCEL_RAW
  atomic_cas(&raw_state, RAW_STATE_IDLE, RAW_STATE_REQUESTED); /* ignored unless idle */
#endif
}
