/*
 * dodgit - UWB radio protocol shared by the ball and the pouches.
 * See shared/README.md for the message flow (roll call, timeslots, ranging, hit, reset).
 *
 * The ball and the pouch each use only their own half of this API. One thread
 * (main) owns the radio; every wait has a bounded timeout.
 */
#ifndef DODGIT_UWB_H
#define DODGIT_UWB_H

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

/* Probe and configure the DW3110. Returns 0 or -errno. Prints the device ID. */
int uwb_init(void);

/* ---------------------------------------------------------------------------
 * Ball side
 * ------------------------------------------------------------------------- */
struct pouch_range
{
  uint8_t slot;   /* timeslot assigned during roll call (0 .. MAX_POUCHES-1) */
  uint32_t uid;   /* the pouch's 32-bit hardware ID */
  int32_t mm;     /* measured distance */
  uint8_t lives;  /* lives the pouch reported in its reply */
};

enum rollcall_result
{
  RC_NONE,   /* nothing (valid) heard */
  RC_NEW,    /* new pouch registered and told its timeslot */
  RC_REPEAT, /* known pouch asked again (lost ACK): told the same timeslot again */
  RC_FULL,   /* table full: ignored, no reply */
};

/* Start listening for JOIN messages (call when entering Roll Call). */
void uwb_ball_rollcall_start(void);
/* Service the roll call; call often. On RC_NEW / RC_REPEAT / RC_FULL, *slot (-1 if full)
 * and *uid describe the pouch. */
enum rollcall_result uwb_ball_rollcall_step(int *slot, uint32_t *uid);

int uwb_ball_pouch_count(void);          /* pouches registered so far */
uint8_t uwb_ball_slot_mask(void);        /* bit n set = slot n registered */
uint32_t uwb_ball_slot_uid(int slot);    /* hardware ID registered in slot (0 if none) */

/* One ranging round to every registered pouch. Fills out[] (up to max) with the pouches
 * that completed the exchange and returns how many (0 = nobody answered), or -errno. */
int uwb_ball_range_round(struct pouch_range *out, int max);

/* Tell the pouch in `slot` it was hit. Returns 0 once it acknowledged, else -errno. */
int uwb_ball_send_hit(int slot, uint16_t hit_seq);

/* Broadcast RESET. Returns a bitmask of the slots that acknowledged. */
uint8_t uwb_ball_send_reset(void);

/* Switch the radio off (Armed state). */
void uwb_ball_idle(void);

/* ---------------------------------------------------------------------------
 * Pouch side
 * ------------------------------------------------------------------------- */
enum pouch_phase
{
  POUCH_JOINING,     /* sending JOIN, no timeslot yet */
  POUCH_JOINED,      /* timeslot assigned, ball has not started ranging yet */
  POUCH_ACTIVE,      /* ball is ranging: the game has started */
  POUCH_JOIN_FAILED, /* ball was already ranging before we got a timeslot (terminal) */
};

enum pouch_event_type
{
  EV_NONE,
  EV_HIT,   /* ball says this pouch was hit (already acknowledged) */
  EV_RESET, /* ball says reset lives (already acknowledged) */
};

struct pouch_event
{
  enum pouch_event_type type;
  uint16_t hit_seq; /* EV_HIT: ball's hit number; retries repeat it, so de-duplicate */
};

uint32_t uwb_pouch_uid(void);          /* this pouch's 32-bit hardware ID */
enum pouch_phase uwb_pouch_phase(void);
int uwb_pouch_slot(void);              /* assigned timeslot, or -1 */

/* Run the pouch radio for one short step (<= ~40 ms). Call in a tight loop: replies are
 * timed to the radio's clock, but the pouch has to notice the incoming frame quickly.
 * `lives` is reported to the ball in ranging replies. */
void uwb_pouch_step(uint8_t lives, struct pouch_event *ev);

#endif
