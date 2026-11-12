/*
 * dodgit - ball accelerometer impact detector.
 *
 * A background thread reads the on-board LIS2DH12 every ACCEL_SAMPLE_PERIOD_MS and
 * computes |a| = sqrt(x^2 + y^2 + z^2) in milli-g. An impact is |a| >= threshold,
 * with hysteresis (|a| must first fall below 3/4 of the threshold) and a cooldown so
 * one shock is not counted twice. Impacts are queued; the ball's state machine only
 * looks at the queue while Armed.
 */
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/atomic.h>
#include "impact.h"
#include "settings.h"

static const struct device *const accel = DEVICE_DT_GET(DT_ALIAS(accel0));
K_MSGQ_DEFINE(impact_q, sizeof(struct impact_event), 8, 4);

static atomic_t running;
static int32_t threshold_mg;
static int cooldown_ms;
static int64_t warm_until_ms;
static atomic_t peak_mg;   /* max |a| since impact_take_peak() */
static atomic_t error_count;

int impact_init(int32_t threshold, int cooldown)
{
  struct sensor_value val;

  if (!device_is_ready(accel))
  {
    return -ENODEV;
  }
  /* Impacts are far above the +/-2 g default range. */
  sensor_g_to_ms2(16, &val);
  int rc = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &val);
  if (rc)
  {
    return rc;
  }
  val.val1 = 400;
  val.val2 = 0;
  rc = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &val);
  if (rc)
  {
    return rc;
  }

  threshold_mg = threshold;
  cooldown_ms = cooldown;
  warm_until_ms = k_uptime_get() + 500;
  atomic_set(&running, 1);
  return 0;
}

bool impact_get(struct impact_event *event)
{
  return k_msgq_get(&impact_q, event, K_NO_WAIT) == 0;
}

int32_t impact_take_peak(void)
{
  return (int32_t)atomic_set(&peak_mg, 0); /* returns the old value */
}

uint32_t impact_errors(void)
{
  return (uint32_t)atomic_get(&error_count);
}

void impact_flush(void)
{
  k_msgq_purge(&impact_q);
}

static void sample_thread(void *a, void *b, void *c)
{
  bool armed_gate = true; /* hysteresis: true once |a| has dropped below 3/4 threshold */
  int64_t cooldown_until = 0;

  while (1)
  {
    k_msleep(ACCEL_SAMPLE_PERIOD_MS);
    if (!atomic_get(&running))
    {
      continue;
    }

    struct sensor_value xyz[3];
    int rc = sensor_sample_fetch(accel);
    if (!rc)
    {
      rc = sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, xyz);
    }
    if (rc)
    {
      if (rc != -ENODATA)
      {
        atomic_inc(&error_count);
      }
      continue; /* no new data / read error: try again next period */
    }
    double x = sensor_value_to_double(&xyz[0]) * 1000.0 / 9.80665;
    double y = sensor_value_to_double(&xyz[1]) * 1000.0 / 9.80665;
    double z = sensor_value_to_double(&xyz[2]) * 1000.0 / 9.80665;
    int32_t mg = (int32_t)sqrt(x * x + y * y + z * z);
    int64_t now = k_uptime_get();

    if (mg > atomic_get(&peak_mg))
    {
      atomic_set(&peak_mg, mg);
    }
    if (mg < threshold_mg * 3 / 4)
    {
      armed_gate = true;
    }
    if (now >= warm_until_ms && armed_gate && now >= cooldown_until && mg >= threshold_mg)
    {
      armed_gate = false;
      cooldown_until = now + cooldown_ms;
      struct impact_event ev = {.time = now, .mg = mg};
      k_msgq_put(&impact_q, &ev, K_NO_WAIT); /* queue full: drop */
    }
  }
}
K_THREAD_DEFINE(impact_thread, 2048, sample_thread, NULL, NULL, NULL, 5, 0, 0);
