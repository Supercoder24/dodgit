/*
 * dodgit - decision logic of the ball's state machine, as pure functions (no Zephyr, no radio, no
 * accelerometer driver: host test in tests/ball_logic_test.c). ball/src/main.c does the ranging, reads
 * the accelerometer and acts on what these functions decide. Rules: docs/FSMs.md.
 *
 *   Disarm()    one ranging round: an alive pouch within pickup range and the ball Held -> Ready, else Unheld
 *   Classify()  one ranging round after a Hit: the closest pouch decides: miss / back to Ready / Hit
 *   Ready       thrower tracking, throw detection (Spinning + the thrower beyond THROW_RANGE_MM)
 *   Thrown      timeout, "the ball came back / landed near somebody" return rule
 *
 * Time is passed in (ms, any steady clock). "Alive" = lives > 0 as recorded in the ball's table
 * (struct pouch_range.lives). Distances are millimeters.
 *
 * A pouch is regularly missing from a ranging round (measured on the recorded flights: about 8% of the
 * rounds when it is within 300 mm, about 1% farther away). Every rule below therefore treats "did not
 * answer" as missing information, never as evidence: a single silent round must not start, break or
 * decide anything on its own.
 */
#ifndef DODGIT_BALL_LOGIC_H
#define DODGIT_BALL_LOGIC_H

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"
#include "uwb.h" /* struct pouch_range */

/* Distance of a pouch that did not answer and was not known to be close. */
#define BL_FAR_MM INT32_MAX

/* Index in r[0..n) of the closest pouch, or -1. With need_lives, pouches without lives are skipped. */
static inline int bl_closest(const struct pouch_range *r, int n, bool need_lives)
{
  int best = -1;

  for (int i = 0; i < n; i++)
  {
    if (need_lives && r[i].lives == 0)
    {
      continue;
    }
    if (best < 0 || r[i].mm < r[best].mm)
    {
      best = i;
    }
  }
  return best;
}

/* Merge the ranging round r[0..m) into acc[0..*n) (capacity max): a pouch heard in ANY round counts, at its
 * smallest distance of the rounds, with the lives of the latest one. Classify() ranges a few times and
 * merges, because the pouch that was just hit (very close to the ball) is often silent in one round. */
static inline void bl_merge_round(struct pouch_range *acc, int *n, int max, const struct pouch_range *r,
                                  int m)
{
  for (int i = 0; i < m; i++)
  {
    int j = 0;

    while (j < *n && acc[j].slot != r[i].slot)
    {
      j++;
    }
    if (j < *n)
    {
      int32_t best = acc[j].mm < r[i].mm ? acc[j].mm : r[i].mm;
      acc[j] = r[i];
      acc[j].mm = best;
    }
    else if (*n < max)
    {
      acc[(*n)++] = r[i];
    }
  }
}

/* ---------------------------------------------------------------------------
 * Disarm()
 * ------------------------------------------------------------------------- */
struct disarm_decision
{
  bool ready;      /* true: go to Ready with `thrower`; false: Unheld */
  int thrower;     /* slot of the closest alive pouch (valid if ready) */
  int32_t mm;      /* its distance (valid if ready) */
};

/* An alive pouch within PICKUP_DISTANCE_MM AND the accelerometer reads Held -> Ready. Without the
 * Held requirement a ball still bouncing next to the pouch it just hit would go straight back to
 * Ready, and a second bounce could count as a second hit: it waits in Unheld until somebody
 * actually picks it up. */
static inline struct disarm_decision bl_disarm(const struct pouch_range *r, int n, bool held)
{
  struct disarm_decision d = {false, -1, 0};
  int c = bl_closest(r, n, true);

  if (held && c >= 0 && r[c].mm <= PICKUP_DISTANCE_MM)
  {
    d = (struct disarm_decision){true, r[c].slot, r[c].mm};
  }
  return d;
}

/* ---------------------------------------------------------------------------
 * Classify()
 * ------------------------------------------------------------------------- */
enum classify_result
{
  CLASSIFY_NO_POUCH,     /* miss -> Unheld: nobody within HIT_DISTANCE_MM (or nobody answered) */
  CLASSIFY_OUT_OF_LIVES, /* miss -> Unheld: the closest pouch has no lives left */
  CLASSIFY_THROWER,      /* closest is the thrower -> Ready */
  CLASSIFY_HIT,          /* closest is another pouch with lives -> Hit */
  CLASSIFY_SELF_HIT,     /* -> Hit: the thrower is the only pouch (SELF_HIT_IF_SINGLE_POUCH bench test) */
};

struct classify_decision
{
  enum classify_result result;
  int slot;      /* the closest pouch (valid unless CLASSIFY_NO_POUCH) */
  int32_t mm;
};

/* The closest pouch, alive or not, decides. A closest pouch that is OUT of lives is a miss: it is
 * deliberately not skipped in favour of the next-closest living one, because hitting a player who is
 * already out must not cost the players around them a life. */
static inline struct classify_decision bl_classify(const struct pouch_range *r, int n, int thrower,
                                                   int pouch_count)
{
  struct classify_decision d = {CLASSIFY_NO_POUCH, -1, 0};
  int c = bl_closest(r, n, false);

  if (c < 0 || r[c].mm > HIT_DISTANCE_MM)
  {
    return d;
  }
  d.slot = r[c].slot;
  d.mm = r[c].mm;
  if (r[c].lives == 0)
  {
    d.result = CLASSIFY_OUT_OF_LIVES;
  }
  else if (r[c].slot != thrower)
  {
    d.result = CLASSIFY_HIT;
  }
  else if (SELF_HIT_IF_SINGLE_POUCH && pouch_count == 1)
  {
    d.result = CLASSIFY_SELF_HIT; /* with one pouch "the thrower hit itself" would make a hit impossible */
  }
  else
  {
    d.result = CLASSIFY_THROWER;
  }
  return d;
}

/* ---------------------------------------------------------------------------
 * Ready
 * ------------------------------------------------------------------------- */
struct ready_ctx
{
  int thrower;              /* slot of the pouch that holds the ball */
  int32_t last_thrower_mm;  /* last valid range to the thrower (0 = assume touching) */
  int64_t silent_since;     /* the thrower has not answered since, or -1 */
  int candidate;            /* slot of a pouch that has been closer than the thrower, or -1 */
  int64_t candidate_since;  /* ... since (the start of the streak) */
  int64_t candidate_seen;   /* ... and the last round that showed it closer */
  int64_t orphan_since;     /* nobody alive within THROW_RANGE_MM (and not Spinning) since, or -1 */
  int64_t not_held_since;   /* the accelerometer has not read Held since, or -1 */
};

enum ready_result
{
  READY_STAY,
  READY_THROWN,    /* Spinning and the thrower beyond THROW_RANGE_MM -> Thrown */
  READY_ORPHAN,    /* nobody alive in range for READY_ORPHAN_MS: somebody without a pouch carries the ball -> Unheld */
  READY_NOT_HELD,  /* not Held for READY_NOT_HELD_MS: put down or dropped -> Unheld */
};

static inline void bl_ready_enter(struct ready_ctx *c, int thrower, int32_t mm)
{
  *c = (struct ready_ctx){.thrower = thrower, .last_thrower_mm = mm, .silent_since = -1, .candidate = -1,
                          .orphan_since = -1, .not_held_since = -1};
}

/* Distance to the thrower for this round. A pouch that does not answer after a very close reading
 * (<= TOO_CLOSE_MM; ranging fails when ball and pouch nearly touch) is still right there: 0 mm. After a
 * farther reading a short silence keeps the last distance (it has not moved far in a few rounds); only
 * a silence of THROWER_SILENT_MS or more counts as far away (blocked, or out of range). *present
 * tells whether the thrower answered this round. */
static inline int32_t bl_thrower_mm(struct ready_ctx *c, int64_t now, const struct pouch_range *r, int n,
                                    bool *present)
{
  for (int i = 0; i < n; i++)
  {
    if (r[i].slot == c->thrower)
    {
      c->last_thrower_mm = r[i].mm;
      c->silent_since = -1;
      *present = true;
      return r[i].mm;
    }
  }
  *present = false;
  if (c->silent_since < 0)
  {
    c->silent_since = now;
  }
  if (c->last_thrower_mm <= TOO_CLOSE_MM)
  {
    return 0;
  }
  return now - c->silent_since >= THROWER_SILENT_MS ? BL_FAR_MM : c->last_thrower_mm;
}

/* One call per ranging round (n = pouches that answered, may be 0), with the accelerometer level.
 *  - Throw: only the THROWER's range matters, so a player standing near the flight path cannot block
 *    a throw. Spinning AND the thrower farther than THROW_RANGE_MM -> READY_THROWN (highest priority).
 *    There are no point-blank throws: a ball thrown less than THROW_RANGE_MM never gets here.
 *  - Possession changes by timeout, not by a flip: another alive pouch must be closer to the ball than
 *    the thrower for THROWER_SWITCH_MS (two kids standing close together do not swap the thrower every
 *    round; a hand-over does). Only evidence interrupts the streak: the candidate was heard and is not
 *    closer, or another pouch is, or it was not heard for THROWER_SWITCH_GRACE_MS. A dropped round of
 *    the candidate or of the thrower does not. The caller sees it as a changed ctx->thrower.
 *  - Unheld: nobody alive within THROW_RANGE_MM for READY_ORPHAN_MS without Spinning, or not Held for
 *    READY_NOT_HELD_MS (Spinning is "not Held" too, but Thrown wins when both apply). */
static inline enum ready_result bl_ready_update(struct ready_ctx *c, int64_t now,
                                                const struct pouch_range *r, int n, bool held,
                                                bool spinning)
{
  bool thrower_present;
  int32_t thrower_mm = bl_thrower_mm(c, now, r, n, &thrower_present);
  /* what the other pouches are compared with: the real last reading, not the 0 mm "too close" stand-in */
  int32_t compare_mm = thrower_present ? thrower_mm : c->last_thrower_mm;

  /* possession: the closest alive pouch other than the thrower, closer than the thrower */
  int b = bl_closest(r, n, true);
  if (b >= 0 && r[b].slot != c->thrower && r[b].mm < compare_mm)
  {
    if (c->candidate != r[b].slot)
    {
      c->candidate = r[b].slot;
      c->candidate_since = now;
    }
    c->candidate_seen = now;
    if (now - c->candidate_since >= THROWER_SWITCH_MS)
    {
      c->thrower = r[b].slot;
      c->last_thrower_mm = r[b].mm;
      c->silent_since = -1;
      c->candidate = -1;
      thrower_mm = r[b].mm;
    }
  }
  else if (c->candidate >= 0)
  {
    bool heard = false;
    for (int i = 0; i < n; i++)
    {
      heard = heard || r[i].slot == c->candidate;
    }
    if (heard || now - c->candidate_seen > THROWER_SWITCH_GRACE_MS)
    {
      c->candidate = -1; /* heard and not closer, or gone for too long */
    }
  }

  /* is anybody alive within throw range? (the thrower counts through the "too close" memory) */
  bool in_range = thrower_mm <= THROW_RANGE_MM;
  for (int i = 0; i < n && !in_range; i++)
  {
    in_range = r[i].lives > 0 && r[i].mm <= THROW_RANGE_MM;
  }
  if (in_range || spinning)
  {
    c->orphan_since = -1;
  }
  else if (c->orphan_since < 0)
  {
    c->orphan_since = now;
  }
  if (held)
  {
    c->not_held_since = -1;
  }
  else if (c->not_held_since < 0)
  {
    c->not_held_since = now;
  }

  if (spinning && thrower_mm > THROW_RANGE_MM)
  {
    return READY_THROWN;
  }
  if (c->orphan_since >= 0 && now - c->orphan_since >= READY_ORPHAN_MS)
  {
    return READY_ORPHAN;
  }
  if (c->not_held_since >= 0 && now - c->not_held_since >= READY_NOT_HELD_MS)
  {
    return READY_NOT_HELD;
  }
  return READY_STAY;
}

/* ---------------------------------------------------------------------------
 * Thrown
 * ------------------------------------------------------------------------- */
struct thrown_ctx
{
  int64_t entered_ms;
  int return_slot;          /* closest alive pouch within THROW_RANGE_MM, or -1 */
  int64_t return_since_ms;  /* ... since (the start of the streak) */
  int64_t return_seen_ms;   /* ... and the last re-check that showed it */
  int64_t rest_since_ms;    /* the accelerometer has read Rest since, or -1 */
};

enum thrown_result
{
  THROWN_STAY,
  THROWN_TIMEOUT, /* no hit for THROW_TIMEOUT_MS -> Disarm() */
  THROWN_REST,    /* the ball landed and lies still (Rest for THROWN_REST_MS) -> Disarm() */
};

static inline void bl_thrown_enter(struct thrown_ctx *c, int64_t now)
{
  *c = (struct thrown_ctx){.entered_ms = now, .return_slot = -1, .rest_since_ms = -1};
}

/* Every main-loop pass (not only when ranging): the timeout and the "landed and lies still" shortcut. */
static inline enum thrown_result bl_thrown_tick(struct thrown_ctx *c, int64_t now, bool rest)
{
  if (rest)
  {
    if (c->rest_since_ms < 0)
    {
      c->rest_since_ms = now;
    }
  }
  else
  {
    c->rest_since_ms = -1;
  }
  if (c->rest_since_ms >= 0 && now - c->rest_since_ms >= THROWN_REST_MS)
  {
    return THROWN_REST;
  }
  return now - c->entered_ms >= THROW_TIMEOUT_MS ? THROWN_TIMEOUT : THROWN_STAY;
}

/* One call per re-check round (THROWN_RECHECK_MS apart; n may be 0). True when the ball goes back to
 * Ready: the same alive pouch has been the closest one within THROW_RANGE_MM for THROWN_RETURN_MS. Either
 * the thrower is back (the throw was cancelled) or the ball ended up nearer to somebody else, which
 * changes possession: *slot / *mm are the new thrower. There are no catches or passes in the game, so
 * the only way the ball changes hands in flight is by lying closer to another player.
 * A re-check in which that pouch is missing does not break the streak (a pouch is missing from some
 * rounds, and re-checks are 250 ms apart). The streak ends when a different pouch is the closest one in
 * range, when the pouch was heard but is out of range, or when it has not been seen for 3 re-checks. */
static inline bool bl_thrown_round(struct thrown_ctx *c, int64_t now, const struct pouch_range *r,
                                   int n, int *slot, int32_t *mm)
{
  int b = bl_closest(r, n, true);

  if (b >= 0 && r[b].mm <= THROW_RANGE_MM)
  {
    if (c->return_slot != r[b].slot)
    {
      c->return_slot = r[b].slot;
      c->return_since_ms = now;
    }
    c->return_seen_ms = now;
    if (now - c->return_since_ms >= THROWN_RETURN_MS)
    {
      *slot = r[b].slot;
      *mm = r[b].mm;
      return true;
    }
    return false;
  }
  if (c->return_slot >= 0)
  {
    bool heard = false;
    for (int i = 0; i < n; i++)
    {
      heard = heard || r[i].slot == c->return_slot;
    }
    if (heard || now - c->return_seen_ms > 3 * THROWN_RECHECK_MS)
    {
      c->return_slot = -1;
    }
  }
  return false;
}

#endif
