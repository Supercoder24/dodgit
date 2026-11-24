/*
 * dodgit - ball accelerometer classifier. See accel_class.h for the interface and
 * docs/Accelerometer.md for the data behind every threshold.
 *
 * Per sample:
 *  1. Hit: |a| >= IMPACT_MG, with hysteresis and a cooldown, reported as an edge event.
 *  2. Shape of the last ACCEL_WINDOW_MS of |a| (mean, mean absolute sample-to-sample difference,
 *     slow trend): ROUGH / CALM / SMOOTH / DRIFT / MIXED. A smooth stretch far from 1 g is judged on
 *     the shorter ACCEL_FAST_WINDOW_MS so that a release is seen quickly.
 *  3. Level, from how long each shape has lasted (debouncing is built in), gated by the level:
 *       Rest only after CALM for ACCEL_REST_MS (settled);
 *       Spinning from Held / Unknown on flight evidence, and from Rest only when the ball is clearly
 *       falling or was just hit (a calm ball never drifts into Spinning);
 *       Held when Spinning loses all signs of life, or Rest is disturbed for ACCEL_LEAVE_REST_MS.
 *     No level lasts shorter than ACCEL_MIN_DWELL_MS.
 */
#include "accel_class.h"
#include "settings.h"

_Static_assert(ACCEL_MIN_SAMPLES >= 2, "window_stats() divides by n / 2 and by n - 1");

static uint32_t since_ms(uint32_t now, uint32_t then)
{
  return (uint32_t)(now - then); /* wrap-safe */
}

static int32_t abs32(int32_t v)
{
  return v < 0 ? -v : v;
}

/* ---- Runs: how long a condition has held ------------------------------------ */

/* Update a run with this sample's condition. A run survives a stretch of "false" shorter than
 * grace_ms (0 = it ends at the first false sample). */
static void run_update(struct accel_run *r, bool cond, uint32_t t, uint32_t grace_ms)
{
  if (cond)
  {
    if (!r->on)
    {
      r->on = true;
      r->since = t;
    }
    r->off = false;
  }
  else if (r->on)
  {
    if (!r->off)
    {
      r->off = true;
      r->off_since = t;
    }
    if (since_ms(t, r->off_since) >= grace_ms)
    {
      r->on = false;
      r->off = false;
    }
  }
}

/* Length of the run; 0 unless the condition is true at this very sample. */
static uint32_t run_ms(const struct accel_run *r, uint32_t t)
{
  return (r->on && !r->off) ? since_ms(t, r->since) : 0;
}

/* ---- Window features --------------------------------------------------------- */

/* Sample `age` steps back from the newest one (age 0 = newest). */
static uint16_t ring_index(const struct accel_class *c, uint16_t age)
{
  return (uint16_t)((c->head + ACCEL_RING_SIZE - 1 - age) % ACCEL_RING_SIZE);
}

/* Statistics of the newest samples no older than span_ms. Returns false if there are too few of them
 * (ACCEL_MIN_SAMPLES, which must be >= 2) or they cover less than half the span: then nothing can be
 * said. A ring that is full before span_ms is over just gives a shorter window. */
static bool window_stats(const struct accel_class *c, uint32_t span_ms, struct accel_features *f)
{
  uint16_t n = 0;
  while (n < c->count && since_ms(c->last_t, c->ring_t[ring_index(c, n)]) <= span_ms)
  {
    n++;
  }
  f->samples = n;
  if (n < ACCEL_MIN_SAMPLES ||
      since_ms(c->last_t, c->ring_t[ring_index(c, (uint16_t)(n - 1))]) < span_ms / 2)
  {
    return false;
  }

  uint16_t half = n / 2;
  int32_t sum = 0, newer = 0, older = 0, diffs = 0;
  for (uint16_t age = 0; age < n; age++)
  {
    int32_t v = c->ring_mg[ring_index(c, age)];
    sum += v;
    if (age < half)
    {
      newer += v;
    }
    else if (age >= n - half)
    {
      older += v;
    }
    if (age > 0)
    {
      diffs += abs32(c->ring_mg[ring_index(c, (uint16_t)(age - 1))] - v);
    }
  }
  f->mean_mg = sum / n;
  f->trend_mg = abs32(newer - older) / half;
  f->masd_mg = diffs / (n - 1);
  return true;
}

static bool near_g(int32_t mean_mg)
{
  return mean_mg >= ACCEL_G_MIN_MG && mean_mg <= ACCEL_G_MAX_MG;
}

/* Features of the long window, and the shape of the ball's motion. */
static struct accel_features classify_window(const struct accel_class *c)
{
  struct accel_features f = {0};
  struct accel_features fast = {0};
  bool valid = window_stats(c, ACCEL_WINDOW_MS, &f);
  bool fast_valid = window_stats(c, ACCEL_FAST_WINDOW_MS, &fast);

  if (!valid)
  {
    f.shape = ACCEL_SHAPE_NONE; /* right after a reset, or a stream too sparse to judge */
  }
  else if (fast_valid && !near_g(fast.mean_mg) && fast.masd_mg <= ACCEL_ROUGH_MASD_MG)
  {
    /* Judged on the short window: the long one would still contain the launch / the impact. */
    f.shape = ACCEL_SHAPE_SMOOTH;
  }
  else if (f.masd_mg > ACCEL_ROUGH_MASD_MG)
  {
    f.shape = ACCEL_SHAPE_ROUGH;
  }
  else if (!near_g(f.mean_mg))
  {
    f.shape = ACCEL_SHAPE_MIXED; /* far from 1 g on the long window only: the ball just changed course */
  }
  else if (f.trend_mg <= ACCEL_CALM_TREND_MG && f.masd_mg <= ACCEL_CALM_MASD_MG)
  {
    f.shape = ACCEL_SHAPE_CALM;
  }
  else if (f.trend_mg >= ACCEL_DRIFT_TREND_MG && f.masd_mg <= ACCEL_DRIFT_MASD_MG)
  {
    f.shape = ACCEL_SHAPE_DRIFT;
  }
  else
  {
    f.shape = ACCEL_SHAPE_MIXED;
  }
  return f;
}

/* ---- Level -------------------------------------------------------------------- */

static enum accel_level next_level(const struct accel_class *c, uint32_t t)
{
  if (since_ms(t, c->level_since) < ACCEL_MIN_DWELL_MS)
  {
    return c->level;
  }

  bool after_hit = c->crossed && since_ms(t, c->last_cross_t) <= ACCEL_AFTER_HIT_MS;
  uint32_t fly_ms = run_ms(&c->fly, t);
  /* Evidence of flight: smooth and far from 1 g (free fall, spin plateau), or any smooth stretch
   * right after an impact. */
  bool flying = run_ms(&c->smooth, t) >= ACCEL_SPIN_MS || (after_hit && fly_ms >= ACCEL_SPIN_MS);
  bool settled = run_ms(&c->calm, t) >= ACCEL_REST_MS;

  switch (c->level)
  {
  case ACCEL_UNKNOWN:
    /* After one full window plus the time the shortest evidence needs: Spinning if the ball is
     * clearly flying, else assume it is being handled until it proves otherwise (Rest needs
     * ACCEL_REST_MS of calm). */
    if (since_ms(t, c->first_t) < ACCEL_WINDOW_MS + ACCEL_SPIN_MS)
    {
      return ACCEL_UNKNOWN;
    }
    return (flying || fly_ms >= ACCEL_SPIN_DRIFT_MS) ? ACCEL_SPINNING : ACCEL_HELD;
  case ACCEL_HELD:
    if (flying || fly_ms >= ACCEL_SPIN_DRIFT_MS) /* the long run also catches near-1 g drift */
    {
      return ACCEL_SPINNING;
    }
    return settled ? ACCEL_REST : ACCEL_HELD;
  case ACCEL_SPINNING:
    if (settled)
    {
      return ACCEL_REST;
    }
    /* Flight is smooth, the ringing after an impact is not: Spinning survives roughness for
     * ACCEL_SPIN_HOLD_MS after the last sign of life, then the ball is just being handled. */
    return since_ms(t, c->last_alive_t) > ACCEL_SPIN_HOLD_MS ? ACCEL_HELD : ACCEL_SPINNING;
  case ACCEL_REST:
    if (flying)
    {
      return ACCEL_SPINNING;
    }
    return run_ms(&c->notcalm, t) >= ACCEL_LEAVE_REST_MS ? ACCEL_HELD : ACCEL_REST;
  }
  return c->level;
}

/* ---- Interface ---------------------------------------------------------------- */

void accel_class_reset(struct accel_class *c)
{
  *c = (struct accel_class){0};
  c->gate = true;
}

struct accel_result accel_class_update(struct accel_class *c, uint32_t t, int32_t mg)
{
  struct accel_result r = {0};
  enum accel_level before = c->level;

  if (c->count > 0 && since_ms(t, c->last_t) > ACCEL_STALL_MS)
  {
    accel_class_reset(c); /* a gap this long means the sensor stalled: the window would span it */
  }
  if (c->count == 0)
  {
    c->first_t = t;
    c->level_since = t;
  }
  mg = mg < 0 ? 0 : (mg > ACCEL_MG_MAX ? ACCEL_MG_MAX : mg);

  c->ring_t[c->head] = t;
  c->ring_mg[c->head] = mg;
  c->head = (uint16_t)((c->head + 1) % ACCEL_RING_SIZE);
  if (c->count < ACCEL_RING_SIZE)
  {
    c->count++;
  }
  c->last_t = t;

  /* Hit: |a| >= IMPACT_MG once per crossing (it must fall below 3/4 of it to re-arm), and at most
   * one reported per IMPACT_COOLDOWN_MS (ringing, double bounce). A crossing inside the cooldown is
   * only flagged, but it still counts as "an impact is happening" for the level logic. No crossing
   * counts while the sensor settles after a reset. */
  bool crossing = false;
  if (mg < IMPACT_MG * 3 / 4)
  {
    c->gate = true;
  }
  if (c->gate && mg >= IMPACT_MG && since_ms(t, c->first_t) >= ACCEL_SETTLE_MS)
  {
    crossing = true;
    c->gate = false;
    c->crossed = true;
    c->last_cross_t = t;
    r.hit_mg = mg;
    if (c->hit_seen && since_ms(t, c->last_hit_t) < IMPACT_COOLDOWN_MS)
    {
      r.hit_suppressed = true;
    }
    else
    {
      r.hit = true;
      c->hit_seen = true;
      c->last_hit_t = t;
    }
  }

  c->feat = classify_window(c);
  enum accel_shape s = c->feat.shape;
  run_update(&c->calm, s == ACCEL_SHAPE_CALM, t, ACCEL_REST_GRACE_MS);
  run_update(&c->notcalm, s != ACCEL_SHAPE_CALM && s != ACCEL_SHAPE_NONE, t, 0);
  run_update(&c->smooth, s == ACCEL_SHAPE_SMOOTH, t, 0);
  run_update(&c->fly, s == ACCEL_SHAPE_SMOOTH || s == ACCEL_SHAPE_DRIFT, t, ACCEL_SPIN_GRACE_MS);
  if (crossing || s == ACCEL_SHAPE_CALM || s == ACCEL_SHAPE_SMOOTH || s == ACCEL_SHAPE_DRIFT)
  {
    c->last_alive_t = t;
  }

  enum accel_level next = next_level(c, t);
  if (next != c->level)
  {
    c->level = next;
    c->level_since = t;
  }
  r.level = c->level;
  r.changed = c->level != before;
  return r;
}

uint32_t accel_class_level_ms(const struct accel_class *c)
{
  return c->count ? since_ms(c->last_t, c->level_since) : 0;
}

struct accel_features accel_class_features(const struct accel_class *c)
{
  return c->feat;
}

const char *accel_level_name(enum accel_level level)
{
  switch (level)
  {
  case ACCEL_REST:
    return "REST";
  case ACCEL_HELD:
    return "HELD";
  case ACCEL_SPINNING:
    return "SPINNING";
  default:
    return "UNKNOWN";
  }
}

const char *accel_shape_name(enum accel_shape shape)
{
  switch (shape)
  {
  case ACCEL_SHAPE_CALM:
    return "CALM";
  case ACCEL_SHAPE_ROUGH:
    return "ROUGH";
  case ACCEL_SHAPE_SMOOTH:
    return "SMOOTH";
  case ACCEL_SHAPE_DRIFT:
    return "DRIFT";
  case ACCEL_SHAPE_MIXED:
    return "MIXED";
  default:
    return "NONE";
  }
}
