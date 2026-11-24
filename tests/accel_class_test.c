/*
 * Host test of the accelerometer classifier (ball/src/accel_class.c), on recorded traces and
 * synthetic ones. Run from the repository root:
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Ishared -Iball/src tests/accel_class_test.c \
 *      ball/src/accel_class.c -o /tmp/accel_class_test && /tmp/accel_class_test [FIXTURE_DIR]
 *
 * FIXTURE_DIR defaults to tests/fixtures/accel (traces extracted from data/ by
 * `python3 -I tools/accel_trace.py fixtures tests/fixtures/accel`). The thresholds in
 * shared/settings.h were tuned on exactly these traces: when you change one, expect assertions on
 * the traces to need a look too (docs/Accelerometer.md, "Changing a threshold").
 *
 * Sections: A synthetic example trace, B recorded flights and landings (BURST windows), C resting,
 * D held / handled, E edge cases (reset, clipping, jitter, cooldown, gaps, timestamp wrap, garbage input).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "accel_class.h"
#include "settings.h"

static const char *fixture_dir = "tests/fixtures/accel";

/* ---- Recorded traces -------------------------------------------------------------- */

struct trace
{
  size_t n;
  uint32_t *t;
  int32_t *mg;
  long trigger_t; /* BURST windows: time of the threshold crossing, -1 otherwise */
};

static void load_trace(const char *name, struct trace *tr)
{
  char path[512], line[256];
  snprintf(path, sizeof path, "%s/%s.txt", fixture_dir, name);
  FILE *f = fopen(path, "r");
  if (!f)
  {
    fprintf(stderr, "cannot open %s (run from the repository root)\n", path);
    exit(2);
  }
  size_t cap = 1024;
  tr->n = 0;
  tr->trigger_t = -1;
  tr->t = malloc(cap * sizeof *tr->t);
  tr->mg = malloc(cap * sizeof *tr->mg);
  while (fgets(line, sizeof line, f))
  {
    long t, mg;
    if (line[0] == '#')
    {
      const char *p = strstr(line, "trigger_t=");
      if (p)
      {
        tr->trigger_t = atol(p + 10);
      }
    }
    else if (sscanf(line, "%ld %ld", &t, &mg) == 2)
    {
      if (tr->n == cap)
      {
        cap *= 2;
        tr->t = realloc(tr->t, cap * sizeof *tr->t);
        tr->mg = realloc(tr->mg, cap * sizeof *tr->mg);
      }
      tr->t[tr->n] = (uint32_t)t;
      tr->mg[tr->n] = (int32_t)mg;
      tr->n++;
    }
  }
  fclose(f);
  assert(tr->n > 100);
}

static void free_trace(struct trace *tr)
{
  free(tr->t);
  free(tr->mg);
}

/* What the classifier said after every sample of a trace. */
struct replay
{
  size_t n;
  const struct trace *tr;
  enum accel_level *level;
  bool *hit;
  bool *suppressed;
  int32_t *hit_mg;
  size_t changes;
  struct
  {
    size_t index;
    enum accel_level from, to;
    uint32_t lasted; /* ms the level we left had lasted */
  } change[128];
};

static void replay_trace(const struct trace *tr, struct replay *rp)
{
  struct accel_class ac;
  accel_class_reset(&ac);
  rp->n = tr->n;
  rp->tr = tr;
  rp->level = malloc(tr->n * sizeof *rp->level);
  rp->hit = calloc(tr->n, sizeof *rp->hit);
  rp->suppressed = calloc(tr->n, sizeof *rp->suppressed);
  rp->hit_mg = calloc(tr->n, sizeof *rp->hit_mg);
  rp->changes = 0;
  uint32_t since = tr->t[0];
  enum accel_level level = ACCEL_UNKNOWN;
  for (size_t i = 0; i < tr->n; i++)
  {
    struct accel_result r = accel_class_update(&ac, tr->t[i], tr->mg[i]);
    rp->level[i] = r.level;
    rp->hit[i] = r.hit;
    rp->suppressed[i] = r.hit_suppressed;
    rp->hit_mg[i] = r.hit_mg;
    assert(r.changed == (r.level != level));
    if (r.changed)
    {
      assert(rp->changes < sizeof rp->change / sizeof rp->change[0]);
      rp->change[rp->changes].index = i;
      rp->change[rp->changes].from = level;
      rp->change[rp->changes].to = r.level;
      rp->change[rp->changes].lasted = tr->t[i] - since;
      rp->changes++;
      level = r.level;
      since = tr->t[i];
    }
  }
}

static void free_replay(struct replay *rp)
{
  free(rp->level);
  free(rp->hit);
  free(rp->suppressed);
  free(rp->hit_mg);
}

/* Level after the last sample at or before time t (ms on the trace's clock). */
static enum accel_level level_at(const struct replay *rp, long t)
{
  enum accel_level level = ACCEL_UNKNOWN;
  for (size_t i = 0; i < rp->n && (long)rp->tr->t[i] <= t; i++)
  {
    level = rp->level[i];
  }
  return level;
}

/* Milliseconds in [t0, t1) the classifier spent in `level`. */
static long time_in(const struct replay *rp, enum accel_level level, long t0, long t1)
{
  long total = 0;
  for (size_t i = 1; i < rp->n; i++)
  {
    long a = (long)rp->tr->t[i - 1], b = (long)rp->tr->t[i];
    if (a >= t0 && b <= t1 && rp->level[i - 1] == level)
    {
      total += b - a;
    }
  }
  return total;
}

static int count_hits(const struct replay *rp, long t0, long t1)
{
  int n = 0;
  for (size_t i = 0; i < rp->n; i++)
  {
    n += rp->hit[i] && (long)rp->tr->t[i] >= t0 && (long)rp->tr->t[i] < t1;
  }
  return n;
}

/* First time at or after t0 the classifier changed to `to`; -1 if never. */
static long first_change_to(const struct replay *rp, enum accel_level to, long t0)
{
  for (size_t c = 0; c < rp->changes; c++)
  {
    long t = (long)rp->tr->t[rp->change[c].index];
    if (rp->change[c].to == to && t >= t0)
    {
      return t;
    }
  }
  return -1;
}

/* No level that was left again may have lasted less than ACCEL_MIN_DWELL_MS (a flicker). Changes
 * that start inside the warm-up of a trace (UNKNOWN, and the guess made when it ends) are skipped,
 * and so is the reset to UNKNOWN after a gap in the data. */
static void check_no_flicker(const struct replay *rp, long warmup_ms)
{
  for (size_t c = 0; c < rp->changes; c++)
  {
    long t = (long)rp->tr->t[rp->change[c].index] - (long)rp->tr->t[0];
    if (rp->change[c].from == ACCEL_UNKNOWN || rp->change[c].to == ACCEL_UNKNOWN || t < warmup_ms)
    {
      continue;
    }
    assert(rp->change[c].lasted >= ACCEL_MIN_DWELL_MS);
  }
}

/* ---- Synthetic traces ------------------------------------------------------------- */

struct gen
{
  struct accel_class ac;
  uint32_t t;
  uint32_t seed;
  /* what happened */
  int hits, suppressed;
  size_t changes;
  enum accel_level to[32];
  uint32_t change_t[32];
  int32_t last_hit_mg;
  uint32_t hit_t[16];
  struct accel_result last;
};

static uint32_t rnd(uint32_t *seed)
{
  *seed = *seed * 1664525u + 1013904223u;
  return *seed >> 8;
}

/* a is earlier than b, on a clock that wraps. */
static bool before(uint32_t a, uint32_t b)
{
  return (int32_t)(a - b) < 0;
}

static void gen_init(struct gen *g, uint32_t t0)
{
  memset(g, 0, sizeof *g);
  accel_class_reset(&g->ac);
  g->t = t0;
  g->seed = 12345;
}

static int32_t noise(struct gen *g, int32_t amp)
{
  return amp ? (int32_t)(rnd(&g->seed) % (uint32_t)(2 * amp + 1)) - amp : 0;
}

/* One sample at the current time, then advance by 5..8 ms (the sensor loop's real jitter). */
static void gen_sample(struct gen *g, int32_t mg)
{
  struct accel_result r = accel_class_update(&g->ac, g->t, mg);
  g->last = r;
  if (r.changed)
  {
    assert(g->changes < 32);
    g->to[g->changes] = r.level;
    g->change_t[g->changes++] = g->t;
  }
  if (r.hit)
  {
    assert(g->hits < 16);
    g->hit_t[g->hits++] = g->t;
    g->last_hit_mg = r.hit_mg;
  }
  g->suppressed += r.hit_suppressed;
  g->t += 5 + rnd(&g->seed) % 4;
}

/* Steady value with noise for ms milliseconds. */
static void gen_hold(struct gen *g, uint32_t ms, int32_t mg, int32_t amp)
{
  uint32_t end = g->t + ms;
  while (before(g->t, end))
  {
    gen_sample(g, mg + noise(g, amp));
  }
}

/* Sporadic values: a hand carrying the ball, every sample far from the previous one. */
static void gen_handled(struct gen *g, uint32_t ms)
{
  uint32_t end = g->t + ms;
  while (before(g->t, end))
  {
    gen_sample(g, 1000 + noise(g, 700));
  }
}

/* An impact: a short spike, then ringing that settles within ~100 ms. */
static void gen_impact(struct gen *g, int32_t peak_mg)
{
  gen_sample(g, peak_mg / 2);
  gen_sample(g, peak_mg);
  gen_sample(g, peak_mg / 3);
  uint32_t end = g->t + 100;
  while (before(g->t, end))
  {
    gen_sample(g, 600 + (int32_t)(rnd(&g->seed) % 3000));
  }
}

static void test_example_sequence(void)
{
  /* Rest -> Held -> (Hit) -> Spinning -> Hit -> Spinning -> Hit -> Rest, the sequence of
   * docs/Changes.md, as a trace shaped like a real pick-up, throw, bounce, bounce and landing. */
  struct gen g;
  gen_init(&g, 1000);
  gen_hold(&g, 3000, 930, 15);                     /* lying on the ground */
  uint32_t held_start = g.t;
  gen_handled(&g, 1500);                           /* picked up and carried */
  uint32_t launch_t = g.t;
  gen_impact(&g, 3500);                            /* launch spike: a Hit, not a level */
  uint32_t released_t = g.t;
  gen_hold(&g, 400, 3800, 25);                     /* flying: flat centripetal plateau */
  gen_impact(&g, 12000);                           /* first bounce */
  gen_hold(&g, 300, 2000, 25);                     /* spinning again */
  gen_impact(&g, 8000);                            /* second bounce, last impact */
  uint32_t landed_t = g.t;
  gen_hold(&g, 3000, 930, 15);                     /* settles */

  /* The levels, after the UNKNOWN warm-up: Held, Rest, Held, Spinning, Rest. */
  const enum accel_level expect[] = {ACCEL_HELD, ACCEL_REST, ACCEL_HELD, ACCEL_SPINNING, ACCEL_REST};
  assert(g.changes == 5);
  size_t k = 0;
  for (size_t i = 0; i < 5; i++)
  {
    assert(g.to[k + i] == expect[i]);
  }
  assert(g.change_t[k] < 1000 + 300);                              /* first guess after one window */
  assert(g.change_t[k + 1] < 1000 + 2000);                         /* Rest within 2 s of lying still */
  assert(g.change_t[k + 2] > held_start && g.change_t[k + 2] < held_start + 500); /* picked up */
  assert(g.change_t[k + 3] > released_t && g.change_t[k + 3] < released_t + 200); /* Spinning */
  assert(g.change_t[k + 4] > landed_t + ACCEL_REST_MS - 300);      /* Rest only after settling */
  assert(g.change_t[k + 4] < landed_t + ACCEL_REST_MS + 800);
  /* Three Hits: launch, first bounce, second bounce; Spinning never left in between. */
  assert(g.hits == 3);
  assert(g.hit_t[0] >= launch_t && g.hit_t[0] <= launch_t + 20);
  assert(g.hit_t[1] > g.hit_t[0] + IMPACT_COOLDOWN_MS && g.hit_t[2] > g.hit_t[1] + IMPACT_COOLDOWN_MS);
  assert(g.ac.level == ACCEL_REST);
  assert(accel_class_level_ms(&g.ac) > 500);
}

/* ---- B: recorded flights and landings (BURST windows) ------------------------------ */

static void burst(const char *name, struct trace *tr, struct replay *rp)
{
  load_trace(name, tr);
  assert(tr->trigger_t >= 0);
  replay_trace(tr, rp);
  /* Every window has its threshold crossing reported as one Hit, at the crossing, never inside the
   * cooldown of another one. */
  long t_trigger = (long)tr->t[0] + tr->trigger_t;
  assert(count_hits(rp, t_trigger - 40, t_trigger + 40) == 1);
  long last_hit = -1000000;
  for (size_t i = 0; i < tr->n; i++)
  {
    if (rp->hit[i])
    {
      assert(rp->hit_mg[i] >= IMPACT_MG);
      assert((long)tr->t[i] - last_hit >= IMPACT_COOLDOWN_MS);
      last_hit = (long)tr->t[i];
    }
  }
  check_no_flicker(rp, 300);
}

static void test_recorded_flights(void)
{
  struct trace tr;
  struct replay rp;

  /* data/ball_20261119_150548.csv BURST #8: a 10 g impact before the window, then ~600 ms of smooth
   * |a| drifting between 960 and 1340 mg (a ball spinning in flight; the old free-fall gate never
   * saw it), the 33 g crossing (clipped at 32767), then 3.8 g held flat for 160 ms. */
  burst("burst_20261119_150548_r1_s08", &tr, &rp);
  long t0 = (long)tr.t[0], trig = t0 + tr.trigger_t;
  assert(level_at(&rp, trig - 150) == ACCEL_SPINNING); /* the drift is recognized before the crossing */
  assert(level_at(&rp, trig - 1) == ACCEL_SPINNING);
  assert(time_in(&rp, ACCEL_SPINNING, trig - 150, trig + 250) >= 380); /* ... and never left */
  assert(time_in(&rp, ACCEL_SPINNING, trig + 120, trig + 240) >= 95); /* the plateau after the bounce */
  assert(count_hits(&rp, t0, t0 + 1000) == 1);
  free_trace(&tr);
  free_replay(&rp);

  /* #3: the same drift, a 35 g crossing and a 6.4 g plateau. */
  burst("burst_20261119_150548_r1_s03", &tr, &rp);
  trig = (long)tr.t[0] + tr.trigger_t;
  assert(level_at(&rp, trig - 100) == ACCEL_SPINNING);
  assert(time_in(&rp, ACCEL_SPINNING, trig + 30, trig + 200) >= 140);
  free_trace(&tr);
  free_replay(&rp);

  /* #14: 7.7 g plateau after the impact; #19: 3.4 g plateau. Flat, whatever the level. */
  burst("burst_20261119_150548_r1_s14", &tr, &rp);
  trig = (long)tr.t[0] + tr.trigger_t;
  assert(time_in(&rp, ACCEL_SPINNING, trig + 60, trig + 250) >= 160);
  free_trace(&tr);
  free_replay(&rp);
  burst("burst_20261119_150548_r1_s19", &tr, &rp);
  trig = (long)tr.t[0] + tr.trigger_t;
  assert(time_in(&rp, ACCEL_SPINNING, trig + 60, trig + 250) >= 160);
  free_trace(&tr);
  free_replay(&rp);

  /* #20 and #24: after the impact |a| sits flat near 1.26 g and 0.77 g for 200+ ms. Nearly 1 g, yet
   * not Rest: Rest needs ACCEL_REST_MS of calm and no impact in between. */
  burst("burst_20261119_150548_r1_s20", &tr, &rp);
  trig = (long)tr.t[0] + tr.trigger_t;
  assert(level_at(&rp, trig - 100) == ACCEL_SPINNING);
  assert(time_in(&rp, ACCEL_REST, trig - 600, trig + 250) == 0);
  free_trace(&tr);
  free_replay(&rp);
  burst("burst_20261119_150548_r1_s24", &tr, &rp);
  trig = (long)tr.t[0] + tr.trigger_t;
  assert(level_at(&rp, trig - 100) == ACCEL_SPINNING);
  assert(time_in(&rp, ACCEL_REST, trig - 600, trig + 250) == 0);
  free_trace(&tr);
  free_replay(&rp);

  /* 150548 #6: a ball lying still at 930 mg for 0.6 s (sd 16 mg), then kicked: a Hit, and smooth ~2 g
   * afterwards = Spinning, reached right after the impact. */
  burst("burst_20261119_150548_r1_s06", &tr, &rp);
  trig = (long)tr.t[0] + tr.trigger_t;
  assert(level_at(&rp, trig - 1) != ACCEL_SPINNING && level_at(&rp, trig - 1) != ACCEL_UNKNOWN);
  assert(level_at(&rp, trig + 280) == ACCEL_SPINNING);
  assert(first_change_to(&rp, ACCEL_SPINNING, trig) < trig + 300);
  assert(count_hits(&rp, trig - 40, trig + 300) == 1);
  assert(time_in(&rp, ACCEL_REST, trig - 700, trig + 300) == 0); /* only 0.6 s of calm: not settled */
  int suppressed = 0;
  for (size_t i = 0; i < tr.n; i++)
  {
    suppressed += rp.suppressed[i]; /* the 3.2 g crossing 20 ms after the 2.5 g one is the same impact */
  }
  assert(suppressed == 1);
  free_trace(&tr);
  free_replay(&rp);

  /* 150326 #1 (run 1): the ball in a hand (state Ready), 940 mg +-100 swinging with the arm, then a
   * 3 g bump. Held up to the bump: the 3-4 Hz swing has the trend of a spin drift but not for
   * ACCEL_SPIN_DRIFT_MS, and it must never be Rest or Spinning. */
  burst("burst_20261119_150326_r1_s01", &tr, &rp);
  trig = (long)tr.t[0] + tr.trigger_t;
  assert(time_in(&rp, ACCEL_SPINNING, trig - 500, trig - 1) < 50);
  assert(time_in(&rp, ACCEL_REST, trig - 500, trig + 250) == 0);
  free_trace(&tr);
  free_replay(&rp);

  /* Every complete window of the recordings: Hit at the crossing, no flicker (checked inside). */
  static const char *const all[] = {
      "burst_20261119_150326_r1_s02", "burst_20261119_150326_r1_s04", "burst_20261119_150326_r2_s02",
      "burst_20261119_150326_r2_s03", "burst_20261119_150326_r2_s04", "burst_20261119_150326_r2_s06",
      "burst_20261119_150326_r2_s07", "burst_20261119_150326_r2_s08", "burst_20261119_150326_r2_s09",
      "burst_20261119_150548_r1_s02", "burst_20261119_150548_r1_s04", "burst_20261119_150548_r1_s05",
      "burst_20261119_150548_r1_s07", "burst_20261119_150548_r1_s09", "burst_20261119_150548_r1_s10",
      "burst_20261119_150548_r1_s11", "burst_20261119_150548_r1_s12", "burst_20261119_150548_r1_s13",
      "burst_20261119_150548_r1_s18", "burst_20261119_150548_r1_s25", "burst_20261119_150548_r1_s26",
      "burst_20261119_150548_r1_s27"};
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
  {
    burst(all[i], &tr, &rp);
    free_trace(&tr);
    free_replay(&rp);
  }
}

/* The release of a toss: ~850 mg held steadily, then |a| falls to ~50 mg (free fall) within 60-100 ms.
 * The BURST windows hold only ~100 ms of the hold before it, so it is repeated (real samples,
 * looped) for 800 ms to let the classifier settle into Held first. Returns the time from the first
 * |a| < 300 mg to Spinning, in ms. */
static long release_latency(const char *name, long calm_end_ms)
{
  struct trace tr;
  load_trace(name, &tr);
  assert(tr.trigger_t >= 0);
  size_t pre = 0;
  while (pre < tr.n && (long)tr.t[pre] - (long)tr.t[0] <= tr.trigger_t + calm_end_ms)
  {
    pre++;
  }
  uint32_t span = tr.t[pre - 1] - tr.t[0] + 6;
  int loops = 8;
  struct accel_class ac;
  accel_class_reset(&ac);
  uint32_t base = 5000;
  for (int l = 0; l < loops; l++)
  {
    for (size_t i = 0; i < pre; i++)
    {
      (void)accel_class_update(&ac, base + (uint32_t)l * span + (tr.t[i] - tr.t[0]), tr.mg[i]);
    }
  }
  assert(ac.level == ACCEL_HELD); /* not Rest (0.8 s), not Spinning: nothing flying yet */
  uint32_t start = base + (uint32_t)loops * span;
  long low_t = -1, spin_t = -1;
  bool was_hit = false;
  for (size_t i = 0; i < tr.n; i++)
  {
    uint32_t t = start + (tr.t[i] - tr.t[0]);
    struct accel_result r = accel_class_update(&ac, t, tr.mg[i]);
    if (low_t < 0 && tr.mg[i] < 300 && (long)(tr.t[i] - tr.t[0]) > tr.trigger_t + calm_end_ms)
    {
      low_t = t;
    }
    if (spin_t < 0 && r.level == ACCEL_SPINNING)
    {
      spin_t = t;
    }
    was_hit |= r.hit;
  }
  assert(low_t > 0 && spin_t > 0 && was_hit);
  /* Spinning comes before the landing impact, which is ~500 ms later. */
  assert(spin_t < low_t + 300);
  free_trace(&tr);
  return spin_t - low_t;
}

static void test_release_latency(void)
{
  /* 150548 #25 and #27: tosses from a hand that was steady at 0.85 g, landing 0.5 s later. */
  long l25 = release_latency("burst_20261119_150548_r1_s25", -600);
  long l27 = release_latency("burst_20261119_150548_r1_s27", -610);
  printf("release -> Spinning latency (|a| < 300 mg to Spinning): #25 %ld ms, #27 %ld ms\n", l25, l27);
  assert(l25 <= 100 && l27 <= 100);
}

/* ---- C: resting ------------------------------------------------------------------- */

/* A lying-still stretch must reach Rest in about ACCEL_REST_MS and then stay there: no Hit, no
 * Spinning, no change at all. */
static void expect_rest(const struct replay *rp, long from_ms, long to_ms, long rest_by_ms)
{
  long t0 = (long)rp->tr->t[0];
  assert(count_hits(rp, t0 + from_ms, t0 + to_ms) == 0);
  assert(time_in(rp, ACCEL_SPINNING, t0 + from_ms, t0 + to_ms) == 0);
  assert(level_at(rp, t0 + rest_by_ms) == ACCEL_REST);
  assert(time_in(rp, ACCEL_REST, t0 + rest_by_ms, t0 + to_ms) >= to_ms - rest_by_ms - 25);
}

static void test_resting(void)
{
  struct trace tr;
  struct replay rp;

  /* accel_20261103_134755.csv: lying still for the first 5 s, the noisiest board (sd 33 mg). */
  load_trace("raw_134755", &tr);
  replay_trace(&tr, &rp);
  expect_rest(&rp, 0, 5000, 2500);
  assert(first_change_to(&rp, ACCEL_REST, 0) < 2500);
  /* ... and again from ~11 s on (settled by 14 s). The ball was put down at ~10.3 s. */
  assert(level_at(&rp, tr.t[tr.n - 1]) == ACCEL_REST);
  assert(count_hits(&rp, 10500, 20000) == 0);
  free_trace(&tr);
  free_replay(&rp);

  /* accel_20261103_135847.csv 24-32 s: 8 s lying still (sd 38 mg). */
  load_trace("raw_135847_rest", &tr);
  replay_trace(&tr, &rp);
  expect_rest(&rp, 0, 8000, 2500);
  assert(rp.changes == 2); /* UNKNOWN -> HELD -> REST and nothing after */
  free_trace(&tr);
  free_replay(&rp);

  /* accel_20261103_135212.csv: ball lying still at the end (from 8.5 s). */
  load_trace("raw_135212", &tr);
  replay_trace(&tr, &rp);
  assert(level_at(&rp, (long)tr.t[tr.n - 1]) == ACCEL_REST);
  free_trace(&tr);
  free_replay(&rp);

  /* STATUS lines (peak and min of every 250 ms, ball not moving for >= 1 s): noise drawn between
   * min and peak, 6.5 ms apart. The two stretches are 2.8 s and 5.9 s long. */
  char path[512], line[256];
  snprintf(path, sizeof path, "%s/status_rest.txt", fixture_dir);
  FILE *f = fopen(path, "r");
  assert(f);
  uint32_t seed = 777;
  struct accel_class ac;
  int stretches = 0;
  uint32_t t = 0;
  bool reached_rest = false;
  char file[128], last_file[128] = "";
  long ms, lo, hi;
  while (fgets(line, sizeof line, f))
  {
    if (line[0] == '#' || sscanf(line, "%127s %ld %ld %ld", file, &ms, &lo, &hi) != 4)
    {
      continue;
    }
    if (strcmp(file, last_file) != 0 || ms == 0)
    {
      if (stretches)
      {
        assert(reached_rest); /* the previous stretch settled */
      }
      stretches++;
      accel_class_reset(&ac);
      strcpy(last_file, file);
      t = 100000 * (uint32_t)stretches;
      reached_rest = false;
    }
    uint32_t end = 100000 * (uint32_t)stretches + (uint32_t)ms;
    while (t < end)
    {
      struct accel_result r = accel_class_update(&ac, t, lo + (int32_t)(rnd(&seed) % (uint32_t)(hi - lo + 1)));
      assert(!r.hit && ac.level != ACCEL_SPINNING);
      reached_rest |= r.level == ACCEL_REST;
      assert(!reached_rest || r.level == ACCEL_REST); /* and stays */
      t += 6 + rnd(&seed) % 2;
    }
  }
  assert(stretches == 2 && reached_rest);
  fclose(f);
}

/* ---- D: held / handled ------------------------------------------------------------- */

static void test_held(void)
{
  struct trace tr;
  struct replay rp;

  /* accel_20261103_134755.csv 5-10 s: shaken by hand (|a| 0.1-9 g at ~5 Hz). Held, never Rest, and
   * Hits (the 2.5-3.8 g peaks) are only events: they do not change the level. */
  load_trace("raw_134755", &tr);
  replay_trace(&tr, &rp);
  assert(time_in(&rp, ACCEL_REST, 5400, 10400) == 0);
  assert(time_in(&rp, ACCEL_HELD, 5400, 10400) >= 4900);
  assert(count_hits(&rp, 5000, 10000) >= 8);
  /* Rest -> Held within a quarter second of the first shake. */
  long held_t = first_change_to(&rp, ACCEL_HELD, 3000);
  assert(held_t > 5000 && held_t < 5600);
  check_no_flicker(&rp, 300);
  free_trace(&tr);
  free_replay(&rp);

  /* accel_20261103_140116.csv: lying still (Rest), picked up at ~4.7 s (Held, not Rest), tossed at
   * ~6.1 s (Spinning): the example sequence on a real recording. */
  load_trace("raw_140116", &tr);
  replay_trace(&tr, &rp);
  assert(level_at(&rp, 4500) == ACCEL_REST);
  assert(level_at(&rp, 5800) == ACCEL_HELD);
  assert(time_in(&rp, ACCEL_REST, 5300, 6100) == 0);
  assert(level_at(&rp, 7000) == ACCEL_SPINNING);
  long picked = first_change_to(&rp, ACCEL_HELD, 3000);
  long tossed = first_change_to(&rp, ACCEL_SPINNING, 3000);
  assert(picked > 4600 && picked < 5200);
  assert(tossed > 5800 && tossed < 6400);
  check_no_flicker(&rp, 300);
  free_trace(&tr);
  free_replay(&rp);

  /* accel_20261103_135646.csv: 1.7 s of samples were lost (BLE) at ~9 s. The classifier must not
   * pretend to know what happened inside a gap: back to UNKNOWN, then a fresh judgement. */
  load_trace("raw_135646", &tr);
  replay_trace(&tr, &rp);
  size_t gap = 0;
  for (size_t i = 1; i < tr.n; i++)
  {
    if (tr.t[i] - tr.t[i - 1] > tr.t[gap + 1] - tr.t[gap])
    {
      gap = i - 1;
    }
  }
  assert(tr.t[gap + 1] - tr.t[gap] > 1000);
  assert(rp.level[gap] != ACCEL_UNKNOWN && rp.level[gap + 1] == ACCEL_UNKNOWN);
  assert(level_at(&rp, (long)tr.t[gap + 1] + 300) != ACCEL_UNKNOWN);
  check_no_flicker(&rp, 300);
  free_trace(&tr);
  free_replay(&rp);
}

/* ---- E: edge cases ------------------------------------------------------------------ */

static void test_reset_and_unknown(void)
{
  struct gen g;
  gen_init(&g, 0);
  assert(g.ac.level == ACCEL_UNKNOWN);
  assert(accel_class_level_ms(&g.ac) == 0);
  assert(accel_class_features(&g.ac).shape == ACCEL_SHAPE_NONE);

  /* No judgement before one window: UNKNOWN, then Held (never Rest straight away). */
  gen_hold(&g, 90, 930, 15);
  assert(g.ac.level == ACCEL_UNKNOWN);
  gen_hold(&g, 150, 930, 15);
  assert(g.ac.level == ACCEL_HELD);
  assert(accel_class_features(&g.ac).shape == ACCEL_SHAPE_CALM);
  assert(accel_class_features(&g.ac).mean_mg > 900 && accel_class_features(&g.ac).mean_mg < 960);
  gen_hold(&g, 2500, 930, 15);
  assert(g.ac.level == ACCEL_REST);
  assert(accel_class_level_ms(&g.ac) > 500);

  /* Reset (sensor re-initialized): everything forgotten. */
  accel_class_reset(&g.ac);
  assert(g.ac.level == ACCEL_UNKNOWN && accel_class_level_ms(&g.ac) == 0);
  assert(accel_class_features(&g.ac).samples == 0);
  /* The first ACCEL_SETTLE_MS after it are blind to impacts (5000 mg for the first 100 ms is
   * ignored), then the first crossing counts. */
  g.hits = 0;
  uint32_t first = g.t;
  while (g.t < first + ACCEL_SETTLE_MS - 10)
  {
    gen_sample(&g, g.t < first + 100 ? 5000 : 900);
  }
  assert(g.hits == 0);
  gen_hold(&g, 40, 900, 5);
  gen_sample(&g, 5000); /* after the settling time a crossing counts */
  assert(g.hits == 1 && g.last_hit_mg == 5000);

  /* A gap longer than ACCEL_STALL_MS in the data is a reset by itself, and says so. */
  gen_hold(&g, 3000, 930, 15);
  assert(g.ac.level == ACCEL_REST);
  g.t += ACCEL_STALL_MS + 20;
  gen_sample(&g, 930);
  assert(g.last.level == ACCEL_UNKNOWN && g.last.changed);
  gen_hold(&g, 300, 930, 15);
  assert(g.ac.level == ACCEL_HELD);
}

static void test_clipping_and_range(void)
{
  struct gen g;
  gen_init(&g, 0);
  gen_hold(&g, 1000, 930, 15);
  g.hits = 0;
  /* The sensor saturates (|a| of the BURST data is clipped at 32767 mg): a plateau of 100 ms at the
   * clip value is ONE Hit, however long it lasts, and nothing overflows. */
  uint32_t end = g.t + 100;
  while (g.t < end)
  {
    gen_sample(&g, 32767);
  }
  assert(g.hits == 1 && g.last_hit_mg == 32767 && g.suppressed == 0);
  gen_hold(&g, 150, 32767, 0);
  assert(g.hits == 1);
  assert(g.ac.level == ACCEL_SPINNING);
  assert(accel_class_features(&g.ac).mean_mg == 32767 && accel_class_features(&g.ac).masd_mg == 0);
  /* Values beyond anything the sensor can say are clamped, negative ones too. */
  gen_hold(&g, 200, 2 * ACCEL_MG_MAX, 0);
  assert(accel_class_features(&g.ac).mean_mg == ACCEL_MG_MAX);
  gen_hold(&g, 200, -50, 0);
  assert(accel_class_features(&g.ac).mean_mg == 0);
}

static void test_jitter_and_timestamps(void)
{
  /* Irregular spacing, 1..12 ms, even two samples in the same millisecond: still Rest after the
   * settling time, still no Hit. */
  struct gen g;
  gen_init(&g, 50);
  while (g.t < 50 + 4000)
  {
    struct accel_result r = accel_class_update(&g.ac, g.t, 930 + noise(&g, 15));
    assert(!r.hit);
    g.t += rnd(&g.seed) % 13 == 0 ? 0 : 1 + rnd(&g.seed) % 12;
  }
  assert(g.ac.level == ACCEL_REST);

  /* The millisecond clock wraps (every 49 days): nothing special happens. */
  gen_init(&g, UINT32_MAX - 1500);
  gen_hold(&g, 4000, 930, 15);
  assert(g.ac.level == ACCEL_REST && g.changes == 2 && g.hits == 0);
  gen_impact(&g, 4000);
  assert(g.hits == 1);
}

static void test_cooldown(void)
{
  struct gen g;
  gen_init(&g, 0);
  gen_hold(&g, 1000, 930, 15);
  g.hits = 0;

  gen_sample(&g, 33000); /* the hit */
  assert(g.last.hit && !g.last.hit_suppressed && g.last.hit_mg == 33000);
  uint32_t hit_t = g.t - 7;
  gen_hold(&g, 40, 600, 20);
  assert(!g.last.hit);
  /* A second 33 g spike inside the cooldown (the ball ringing / bouncing on the spot): the same
   * physical impact. Flagged as suppressed, not reported. */
  gen_sample(&g, 33000);
  assert(!g.last.hit && g.last.hit_suppressed && g.last.hit_mg == 33000);
  assert(g.hits == 1 && g.suppressed == 1);
  /* While |a| stays above 3/4 of the threshold the crossing is used up: no repeat. */
  gen_hold(&g, 60, 3000, 10);
  assert(g.hits == 1 && g.suppressed == 1);
  /* After the cooldown a new crossing is a new Hit. */
  gen_hold(&g, IMPACT_COOLDOWN_MS, 700, 20);
  assert(g.t > hit_t + IMPACT_COOLDOWN_MS);
  gen_sample(&g, 2600);
  assert(g.last.hit && g.hits == 2);
  /* Staying under the threshold does not re-arm the gate halfway: 2400 mg is not a crossing, and
   * 2000 mg (> 3/4 of the threshold) between two crossings does not re-arm it. */
  gen_hold(&g, IMPACT_COOLDOWN_MS + 50, 2000, 10);
  gen_sample(&g, 2900);
  assert(!g.last.hit && !g.last.hit_suppressed && g.hits == 2);
  gen_hold(&g, 30, 1000, 10);
  gen_sample(&g, 2900);
  assert(g.last.hit && g.hits == 3);

  /* A bump while held is a Hit (an event) and does not change the level. */
  gen_init(&g, 0);
  gen_handled(&g, 1000);
  assert(g.ac.level == ACCEL_HELD);
  gen_impact(&g, 3500);
  gen_handled(&g, 300);
  assert(g.hits == 1 && g.ac.level == ACCEL_HELD);
}

/* Garbage in must not break anything: random values (negative, beyond the sensor's range), time
 * standing still or running backwards, gaps, resets at random. Mostly meant for the sanitizer build
 * (docs/Accelerometer.md); here it only checks the invariants the firmware relies on. */
static void test_garbage_input(void)
{
  struct accel_class ac;
  accel_class_reset(&ac);
  uint32_t seed = 4242, t = UINT32_MAX - 5000;
  long hits = 0;
  for (long i = 0; i < 300000; i++)
  {
    uint32_t kind = rnd(&seed) % 3000;
    t += kind == 0 ? rnd(&seed) % 400 : (kind == 1 ? (uint32_t)-(int32_t)(rnd(&seed) % 50) : rnd(&seed) % 10);
    int32_t mg;
    switch (rnd(&seed) % 5)
    {
    case 0:
      mg = (int32_t)rnd(&seed) - (1 << 22);
      break;
    case 1:
      mg = (rnd(&seed) & 1) ? INT32_MAX : INT32_MIN;
      break;
    case 2:
      mg = 930 + (int32_t)(rnd(&seed) % 40);
      break;
    case 3:
      mg = 32767;
      break;
    default:
      mg = (int32_t)(rnd(&seed) % 6000);
      break;
    }
    if (rnd(&seed) % 5000 == 0)
    {
      accel_class_reset(&ac);
    }
    struct accel_result r = accel_class_update(&ac, t, mg);
    struct accel_features f = accel_class_features(&ac);
    assert(r.level <= ACCEL_SPINNING && r.level == ac.level);
    assert(!r.hit || (r.hit_mg >= IMPACT_MG && !r.hit_suppressed));
    assert(f.mean_mg >= 0 && f.mean_mg <= ACCEL_MG_MAX && f.samples <= ACCEL_RING_SIZE);
    hits += r.hit;
  }
  assert(hits > 1000); /* the extreme values do produce Hits: the run was not all settling time */
}

int main(int argc, char **argv)
{
  if (argc > 1)
  {
    fixture_dir = argv[1];
  }
  test_example_sequence();
  test_recorded_flights();
  test_release_latency();
  test_resting();
  test_held();
  test_reset_and_unknown();
  test_clipping_and_range();
  test_jitter_and_timestamps();
  test_cooldown();
  test_garbage_input();
  printf("accel_class_test: OK\n");
  return 0;
}
