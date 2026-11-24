/*
 * dodgit - the pouch's membership state machine as pure logic (no Zephyr, no radio: host test in
 * tests/pouch_fsm_test.c). shared/uwb.c feeds it what the radio hears and does what it says.
 *
 *   Joining    sends JOIN at random intervals; listens
 *   Listening  silent: the ball is already ranging, so it will not answer a JOIN
 *   Joined     slot and lives known (told by a REMEMBER), waiting for the ball's first POLL / HIT / RESET
 *   Active     answering the ball
 *
 *   power-up                         -> Joining        (first JOIN after a random 100-300 ms)
 *   Joining   --POLL heard-->           Listening
 *   Joining / Listening
 *             --REMEMBER with our ID--> Joined         (also: we rebooted mid-game, same slot and lives)
 *   Listening --RESET heard-->          Joining        (after a random delay: the ball accepts new pouches)
 *   Joined    --POLL / HIT / RESET-->   Active
 *   Joined / Active --no REMEMBER for LOST_TIMEOUT_MS--> Joining    (ball rebooted, or it deleted us)
 *   Listening --no POLL for LOST_TIMEOUT_MS-->           Joining    (the ball rebooted into Roll Call)
 *
 * A REMEMBER with our ID overwrites slot and lives in ANY phase: the ball is the source of truth.
 */
#ifndef DODGIT_POUCH_FSM_H
#define DODGIT_POUCH_FSM_H

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

enum pouch_phase
{
  POUCH_JOINING,
  POUCH_LISTENING,
  POUCH_JOINED,
  POUCH_ACTIVE,
};

enum pouch_event_type
{
  EV_NONE,
  EV_HIT,      /* lives went down (HIT message, or a REMEMBER that revealed a lost HIT) */
  EV_RESET,    /* the ball reset the lives */
  EV_REMEMBER, /* a REMEMBER with our ID arrived (slot and lives were overwritten with the ball's values) */
};

enum pouch_poll_action
{
  POLL_IGNORE,
  POLL_ANSWER, /* run our half of the ranging exchange */
};

enum pouch_reset_action
{
  RESET_IGNORE,
  RESET_ACKNOWLEDGE, /* send RESET_ACK in our slot (lives are already back to LIVES_MAX) */
  RESET_REJOIN,      /* start Joining after a random delay (the caller draws it) */
};

struct pouch_fsm
{
  enum pouch_phase phase;
  int slot;                 /* -1 until a REMEMBER told us */
  uint8_t lives;
  int64_t next_join_ms;     /* Joining: when to send the next JOIN */
  int64_t last_remember_ms; /* Joined / Active: time of the last REMEMBER with our ID (the heartbeat) */
  int64_t last_poll_ms;     /* Listening: time of the last POLL */
};

/* Joining, first JOIN `delay_ms` from now. Used at power-up, after a RESET heard in Listening and after
 * the ball stopped talking to us. The slot is forgotten: it may belong to somebody else by now. */
static inline void pouch_fsm_enter_joining(struct pouch_fsm *f, int64_t now, int64_t delay_ms)
{
  f->phase = POUCH_JOINING;
  f->slot = -1;
  f->next_join_ms = now + delay_ms;
}

static inline void pouch_fsm_init(struct pouch_fsm *f, int64_t now, int64_t first_join_delay_ms)
{
  f->lives = LIVES_MAX;
  pouch_fsm_enter_joining(f, now, first_join_delay_ms);
}

/* Time-based loss detection. True means: call pouch_fsm_enter_joining(). Joined / Active: the heartbeat
 * is a REMEMBER with OUR ID (any frame from the ball would not do: a pouch the ball deleted still hears
 * its POLLs). Listening: the ball ranges constantly during a game, so no POLL means it rebooted. */
static inline bool pouch_fsm_lost(const struct pouch_fsm *f, int64_t now)
{
  if (f->phase == POUCH_JOINED || f->phase == POUCH_ACTIVE)
  {
    return now - f->last_remember_ms >= LOST_TIMEOUT_MS;
  }
  return f->phase == POUCH_LISTENING && now - f->last_poll_ms >= LOST_TIMEOUT_MS;
}

/* True when a JOIN is due (Joining only). The caller sends it and schedules the next one. */
static inline bool pouch_fsm_join_due(const struct pouch_fsm *f, int64_t now)
{
  return f->phase == POUCH_JOINING && now >= f->next_join_ms;
}

/* A REMEMBER with our ID (the caller compared it) arrived. A slot or lives out of range is ignored
 * (EV_NONE). Otherwise the ball's slot and lives overwrite ours; a drop in lives while Active means a
 * HIT never reached us (the ball counts it at once), reported as EV_HIT like the real thing. */
static inline enum pouch_event_type pouch_fsm_on_remember(struct pouch_fsm *f, int64_t now, int slot,
                                                           uint8_t lives)
{
  if (slot < 0 || slot >= MAX_POUCHES || lives > LIVES_MAX)
  {
    return EV_NONE;
  }
  enum pouch_event_type ev = (f->phase == POUCH_ACTIVE && lives < f->lives) ? EV_HIT : EV_REMEMBER;

  f->last_remember_ms = now;
  f->slot = slot;
  f->lives = lives;
  if (f->phase == POUCH_JOINING || f->phase == POUCH_LISTENING)
  {
    f->phase = POUCH_JOINED;
  }
  return ev;
}

/* A broadcast POLL was heard. */
static inline enum pouch_poll_action pouch_fsm_on_poll(struct pouch_fsm *f, int64_t now)
{
  switch (f->phase)
  {
  case POUCH_JOINING:
    /* The ball is already ranging, so it is not in Roll Call and will not answer a JOIN. Stay silent and
     * wait for a REMEMBER with our ID (we rebooted) or a RESET (someone is being added). */
    f->phase = POUCH_LISTENING;
    f->last_poll_ms = now;
    return POLL_IGNORE;
  case POUCH_LISTENING:
    f->last_poll_ms = now;
    return POLL_IGNORE;
  case POUCH_JOINED:
  case POUCH_ACTIVE:
    f->phase = POUCH_ACTIVE;
    return POLL_ANSWER;
  }
  return POLL_IGNORE;
}

/* A HIT addressed to our slot, carrying the lives we have left. Returns false if we are not in the
 * game (Joining / Listening: the caller must not acknowledge). HIT carries the absolute value, so a
 * retry sets the same number again: only a DROP is a (new) hit, reported in *ev. Acknowledge with
 * the same value. */
static inline bool pouch_fsm_on_hit(struct pouch_fsm *f, uint8_t lives, enum pouch_event_type *ev)
{
  if (f->phase != POUCH_JOINED && f->phase != POUCH_ACTIVE)
  {
    return false;
  }
  f->phase = POUCH_ACTIVE;
  *ev = lives < f->lives ? EV_HIT : EV_NONE;
  f->lives = lives <= LIVES_MAX ? lives : LIVES_MAX;
  return true;
}

/* A broadcast RESET was heard. */
static inline enum pouch_reset_action pouch_fsm_on_reset(struct pouch_fsm *f)
{
  switch (f->phase)
  {
  case POUCH_JOINED:
  case POUCH_ACTIVE:
    f->phase = POUCH_ACTIVE;
    f->lives = LIVES_MAX;
    return RESET_ACKNOWLEDGE;
  case POUCH_LISTENING:
    return RESET_REJOIN;
  case POUCH_JOINING:
    break; /* already sending JOINs */
  }
  return RESET_IGNORE;
}

#endif
