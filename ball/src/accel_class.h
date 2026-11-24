/*
 * dodgit - ball accelerometer classifier (pure C11: no Zephyr, no malloc, no globals, integer math).
 *
 * Turns the stream of |a| samples into
 *   - a LEVEL, the persistent state of the ball:
 *       ACCEL_UNKNOWN   no data yet, or the sensor was just re-initialized (accel_class_reset)
 *       ACCEL_REST      lying still on the ground: ~1 g with nothing but sensor noise, for a while
 *       ACCEL_HELD      carried / handled by a player: irregular values (also the default guess)
 *       ACCEL_SPINNING  flying or rolling: |a| is smooth, either far from 1 g (free fall, or the flat
 *                       centripetal plateau of a spinning ball) or slowly drifting near 1 g
 *   - a HIT, an edge event (not a level): |a| reached IMPACT_MG. One physical impact = one event: the
 *     crossing must fall below 3/4 of the threshold to re-arm and IMPACT_COOLDOWN_MS separates two
 *     reports. The classifier reports every Hit; the ball FSM decides whether it matters in its
 *     state (Thrown: yes; Ready, Unheld: no). There is no free-fall gate any more.
 *
 * A trace shaped like Rest -> Held -> (Hit) -> Spinning -> Hit -> Spinning -> Hit -> Rest (the
 * sequence of docs/Changes.md) produces exactly that; Spinning is reached from Held, or from Rest
 * when the ball is clearly falling or was just hit, and Rest only after ACCEL_REST_MS of calm. The
 * features, thresholds, the data behind them and the known failure modes are in
 * docs/Accelerometer.md; the tunables are the ACCEL_* macros in shared/settings.h.
 *
 * Usage (one instance, owned by the caller):
 *     struct accel_class ac;
 *     accel_class_reset(&ac);                                        // at boot and after a stall re-init
 *     struct accel_result r = accel_class_update(&ac, now_ms, mg);   // every sample, 4-8 ms apart
 *     if (r.hit) ...;  if (r.changed) log(accel_level_name(r.level));
 * Samples are the magnitude |a| in milli-g and a millisecond clock (wraps are fine, spacing may
 * jitter). The thresholds were tuned for 5-7 ms spacing; a faster stream (400 Hz) must be decimated.
 * Host tests: tests/accel_class_test.c; replay of a trace: tests/accel_replay.c.
 */
#ifndef DODGIT_ACCEL_CLASS_H
#define DODGIT_ACCEL_CLASS_H

#include <stdbool.h>
#include <stdint.h>

enum accel_level
{
  ACCEL_UNKNOWN = 0, /* zero, so that a zeroed struct is a freshly reset one */
  ACCEL_REST,
  ACCEL_HELD,
  ACCEL_SPINNING,
};

/* What the latest window of samples looks like (see ACCEL_* in shared/settings.h). The level comes
 * from how long each shape has lasted, never from a single window. */
enum accel_shape
{
  ACCEL_SHAPE_NONE,   /* too few samples in the window to say */
  ACCEL_SHAPE_CALM,   /* about 1 g, no trend, only sensor noise: rest-like */
  ACCEL_SHAPE_ROUGH,  /* irregular: large sample-to-sample changes (carried, shaken, an impact) */
  ACCEL_SHAPE_SMOOTH, /* smooth and far from 1 g: free fall, or the flat plateau of a spinning ball */
  ACCEL_SHAPE_DRIFT,  /* smooth, about 1 g, but clearly drifting: a spinning ball's slow oscillation */
  ACCEL_SHAPE_MIXED,  /* in between: no evidence either way */
};

/* Features of the latest window (the last ACCEL_WINDOW_MS of samples), for LOG_ACCEL_FEATURES and
 * for tuning. */
struct accel_features
{
  int32_t mean_mg;
  int32_t trend_mg; /* |mean of the newer half - mean of the older half|: slow drift, noise averaged out */
  int32_t masd_mg;  /* mean absolute difference of consecutive samples: roughness, ignores slow drift */
  uint16_t samples; /* samples in the window */
  enum accel_shape shape;
};

/* Result of one sample. */
struct accel_result
{
  enum accel_level level; /* the level after this sample */
  bool changed;           /* it differs from the level returned for the previous sample */
  bool hit;               /* a Hit happened at this sample; hit_mg is its magnitude */
  bool hit_suppressed;    /* a crossing inside IMPACT_COOLDOWN_MS: the same impact, not reported;
                           * hit_mg is its magnitude (diagnostics) */
  int32_t hit_mg;
};

/* Classifier state. Treat as opaque; it is public only so that the caller can allocate it
 * (about 620 bytes). */
struct accel_run
{
  bool on;           /* the condition has held since `since` (a short gap does not end the run) */
  bool off;          /* ... but it is false right now (inside the gap) */
  uint32_t since;
  uint32_t off_since;
};

#define ACCEL_RING_SIZE 64 /* at least ACCEL_WINDOW_MS / smallest sample spacing; a fuller ring just
                            * shortens the window */

struct accel_class
{
  uint32_t ring_t[ACCEL_RING_SIZE];
  int32_t ring_mg[ACCEL_RING_SIZE];
  uint16_t head;  /* next write position */
  uint16_t count; /* valid samples in the ring */
  uint32_t last_t;
  uint32_t first_t; /* time of the first sample after a reset (no Hit for ACCEL_SETTLE_MS from then) */
  enum accel_level level;
  uint32_t level_since;
  struct accel_features feat;
  struct accel_run calm, notcalm, smooth, fly; /* how long each shape has lasted */
  uint32_t last_alive_t; /* last sample that showed a flying or landing ball: smooth, calm, or an impact */
  bool gate;             /* hit hysteresis: |a| has been below 3/4 of IMPACT_MG since the last crossing */
  bool crossed;          /* at least one crossing since the reset (last_cross_t is valid) */
  uint32_t last_cross_t;
  bool hit_seen;         /* at least one reported Hit since the reset (last_hit_t is valid) */
  uint32_t last_hit_t;
};

/* Forget everything: level UNKNOWN, empty window, no Hit reported for ACCEL_SETTLE_MS from the next
 * sample on (the sensor settles). Call at boot and after every sensor stall + re-init ("keep the
 * existing stall recovery"). A gap of more than ACCEL_STALL_MS between two samples does the same by
 * itself, so replaying a recording with holes in it is safe. */
void accel_class_reset(struct accel_class *c);

/* Feed one sample: t_ms from any monotonic millisecond clock, mg = |a| in milli-g (clamped to
 * 0..ACCEL_MG_MAX; the sensor saturates at about 41400, BURST logs at 32767). */
struct accel_result accel_class_update(struct accel_class *c, uint32_t t_ms, int32_t mg);

/* Milliseconds since the level last changed, as of the latest sample; 0 right after a reset. */
uint32_t accel_class_level_ms(const struct accel_class *c);

/* Features and shape of the latest window (shape NONE and zeros before the first samples). */
struct accel_features accel_class_features(const struct accel_class *c);

/* "UNKNOWN" / "REST" / "HELD" / "SPINNING" and "NONE" / "CALM" / "ROUGH" / "SMOOTH" / "DRIFT" / "MIXED". */
const char *accel_level_name(enum accel_level level);
const char *accel_shape_name(enum accel_shape shape);

#endif
