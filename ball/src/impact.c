/*
 * dodgit - ball accelerometer impact detector.
 *
 * A background thread reads the on-board LIS2DH12 every ACCEL_SAMPLE_PERIOD_MS and
 * computes |a| = sqrt(x^2 + y^2 + z^2) in milli-g. An impact is |a| >= threshold,
 * with hysteresis (|a| must first fall below 3/4 of the threshold). Impacts are queued;
 * the ball's state machine looks at the queue in Ready and Armed.
 *
 * Three things beyond plain thresholding:
 *  - Free-fall gate: an impact only counts if free fall (|a| near 0) was seen just before it,
 *    i.e. the ball was flying. The launch spike of a throw and bumps while the ball is held are
 *    counted (impact_rejected) but ignored, and they start no cooldown, so they cannot hide a
 *    hit that follows soon after.
 *  - Motion tracking: impact_still_ms() tells for how long the ball has been lying still.
 *  - Stall recovery: after a hard shock the sensor sometimes stops delivering data (status
 *    "no new data" forever, so the reading looks like 0). If no fresh sample arrives for
 *    ACCEL_STALL_MS the thread re-initializes the sensor from software (the LIS2DH has no
 *    reset pin).
 */
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/atomic.h>
#include "impact.h"
#include "settings.h"

/* Zephyr's LIS2DH driver: re-writes CTRL1..6 to the boot defaults and re-enables measurement.
 * Not declared in any public header, but it is a global function of the driver. */
extern int lis2dh_init_chip(const struct device *dev);

static const struct device *const accel = DEVICE_DT_GET(DT_ALIAS(accel0));
K_MSGQ_DEFINE(impact_q, sizeof(struct impact_event), 8, 4);

#define MOTION_WINDOW_MS 100 /* the reference sample for motion detection is this old at most */
#define SETTLE_MS 500        /* impacts ignored this long after (re-)initializing the sensor */

static atomic_t running;
static int32_t threshold_mg;
static int cooldown_ms;
static int64_t warm_until_ms;
static int64_t last_good_ms; /* time of the last valid sample (written by init and the thread) */
static atomic_t peak_mg = ATOMIC_INIT(-1); /* max |a| since impact_take_peak(), -1 = no sample */
static atomic_t error_count;
static atomic_t reset_count;
static atomic_t healthy = ATOMIC_INIT(1);
static atomic_t rejected_count;
static atomic_t rejected_mg;
static atomic_t freefall_ms;       /* k_uptime_get_32() of the latest free-fall sample, 0 = none */
static atomic_t freefall_start;    /* k_uptime_get_32() when that free fall began, 0 = none */
static atomic_t last_motion_ms;    /* k_uptime_get_32() of the latest motion */

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

int impact_init(int32_t threshold, int cooldown)
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

  threshold_mg = threshold;
  cooldown_ms = cooldown;
  warm_until_ms = k_uptime_get() + SETTLE_MS;
  last_good_ms = k_uptime_get();
  atomic_set(&last_motion_ms, (atomic_val_t)k_uptime_get_32());
  atomic_set(&running, 1);
  return 0;
}

bool impact_get(struct impact_event *event)
{
  return k_msgq_get(&impact_q, event, K_NO_WAIT) == 0;
}

int32_t impact_take_peak(void)
{
  return (int32_t)atomic_set(&peak_mg, -1); /* returns the old value */
}

uint32_t impact_errors(void)
{
  return (uint32_t)atomic_get(&error_count);
}

uint32_t impact_resets(void)
{
  return (uint32_t)atomic_get(&reset_count);
}

bool impact_healthy(void)
{
  return atomic_get(&healthy) != 0;
}

uint32_t impact_rejected(int32_t *last_mg)
{
  if (last_mg)
  {
    *last_mg = (int32_t)atomic_get(&rejected_mg);
  }
  return (uint32_t)atomic_get(&rejected_count);
}

bool impact_freefall_recent(int hold_ms)
{
  uint32_t t = (uint32_t)atomic_get(&freefall_ms);
  return t != 0 && (uint32_t)(k_uptime_get_32() - t) <= (uint32_t)hold_ms;
}

uint32_t impact_freefall_start_ms(void)
{
  return (uint32_t)atomic_get(&freefall_start);
}

void impact_freefall_clear(void)
{
  atomic_set(&freefall_ms, 0);
  atomic_set(&freefall_start, 0);
}

uint32_t impact_still_ms(void)
{
  if (!impact_healthy())
  {
    return 0;
  }
  return k_uptime_get_32() - (uint32_t)atomic_get(&last_motion_ms);
}

bool impact_moved_recent(int ms)
{
  return impact_still_ms() <= (uint32_t)ms; /* unhealthy -> 0 -> true */
}

void impact_flush(void)
{
  k_msgq_purge(&impact_q);
}

static void sample_thread(void *a, void *b, void *c)
{
  bool armed_gate = true; /* hysteresis: true once |a| has dropped below 3/4 threshold */
  int64_t cooldown_until = 0;
  int64_t freefall_since = 0; /* start of the current run of |a| < FREEFALL_MG, 0 = none */
  int64_t next_recover_ms = 0;
  bool ref_valid = false;     /* motion reference sample (taken at most MOTION_WINDOW_MS ago) */
  int64_t ref_ms = 0;
  double rx = 0, ry = 0, rz = 0;

  while (1)
  {
    k_msleep(ACCEL_SAMPLE_PERIOD_MS);
    if (!atomic_get(&running))
    {
      continue;
    }

    struct sensor_value xyz[3];
    int64_t now = k_uptime_get();
    bool good = false;
    double x = 0, y = 0, z = 0;

    int rc = sensor_sample_fetch(accel);
    if (!rc)
    {
      rc = sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, xyz);
    }
    if (!rc)
    {
      x = sensor_value_to_double(&xyz[0]) * 1000.0 / 9.80665;
      y = sensor_value_to_double(&xyz[1]) * 1000.0 / 9.80665;
      z = sensor_value_to_double(&xyz[2]) * 1000.0 / 9.80665;
      /* A real sensor never reads exactly 0 on all three axes: that is the stalled state. */
      good = !(x == 0.0 && y == 0.0 && z == 0.0);
    }
    else if (rc != -ENODATA)
    {
      atomic_inc(&error_count);
    }

    if (!good)
    {
      /* -ENODATA (data-ready bit never set), read error, or all-zero data. A hard shock can
       * leave the LIS2DH like this; re-initialize it once nothing valid has arrived for a while. */
      if (now - last_good_ms > ACCEL_STALL_MS)
      {
        atomic_set(&healthy, 0);
        if (now >= next_recover_ms)
        {
          next_recover_ms = now + ACCEL_RECOVER_RETRY_MS;
          int rrc = lis2dh_init_chip(accel);
          if (!rrc)
          {
            rrc = accel_configure();
          }
          atomic_inc(&reset_count);
          printk("ACCEL stalled %lld ms, re-init rc=%d (reset #%u)\n", now - last_good_ms, rrc,
                 (uint32_t)atomic_get(&reset_count));
          /* Forget everything that depended on the old data stream. */
          armed_gate = true;
          cooldown_until = 0;
          freefall_since = 0;
          ref_valid = false;
          warm_until_ms = k_uptime_get() + SETTLE_MS;
        }
      }
      continue;
    }
    last_good_ms = now;
    atomic_set(&healthy, 1);

    int32_t mg = (int32_t)sqrt(x * x + y * y + z * z);

    /* Motion: compare with a reference sample that is refreshed every MOTION_WINDOW_MS. */
    if (!ref_valid)
    {
      ref_valid = true;
      ref_ms = now;
      rx = x;
      ry = y;
      rz = z;
      atomic_set(&last_motion_ms, (atomic_val_t)k_uptime_get_32()); /* unknown: assume moving */
    }
    else
    {
      double dx = x - rx, dy = y - ry, dz = z - rz;
      if (sqrt(dx * dx + dy * dy + dz * dz) > STILL_DELTA_MG)
      {
        atomic_set(&last_motion_ms, (atomic_val_t)k_uptime_get_32());
      }
      if (now - ref_ms >= MOTION_WINDOW_MS)
      {
        ref_ms = now;
        rx = x;
        ry = y;
        rz = z;
      }
    }

    if (mg > atomic_get(&peak_mg))
    {
      atomic_set(&peak_mg, mg);
    }
    if (mg < FREEFALL_MG)
    {
      if (freefall_since == 0)
      {
        freefall_since = now;
      }
      if (now - freefall_since >= FREEFALL_MIN_MS)
      {
        if (atomic_get(&freefall_start) == 0)
        {
          atomic_set(&freefall_start, (atomic_val_t)((uint32_t)freefall_since | 1));
        }
        atomic_set(&freefall_ms, (atomic_val_t)(k_uptime_get_32() | 1)); /* never 0 */
      }
    }
    else
    {
      freefall_since = 0;
    }
    if (mg < threshold_mg * 3 / 4)
    {
      armed_gate = true;
    }
    if (now >= warm_until_ms && armed_gate && mg >= threshold_mg)
    {
      armed_gate = false; /* this crossing is used up until |a| falls again */
      if (now >= cooldown_until)
      {
        if (impact_freefall_recent(IMPACT_AFTER_FREEFALL_MS))
        {
          cooldown_until = now + cooldown_ms; /* only a real (accepted) impact starts it */
          struct impact_event ev = {.time = now, .mg = mg};
          k_msgq_put(&impact_q, &ev, K_NO_WAIT); /* queue full: drop */
        }
        else
        {
          /* Not flying just before: launch spike, or a bump in the hand. */
          atomic_set(&rejected_mg, mg);
          atomic_inc(&rejected_count);
        }
      }
    }
  }
}
K_THREAD_DEFINE(impact_thread, 2048, sample_thread, NULL, NULL, NULL, 5, 0, 0);
