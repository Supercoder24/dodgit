/*
 * dodgit - UWB radio protocol shared by the ball and the pouches.
 * See shared/README.md for the message flow (join, REMEMBER, timeslots, ranging, hit, reset).
 *
 * The ball and the pouch each use only their own half of this API. One thread
 * (main) owns the radio; every wait has a bounded timeout.
 */
#ifndef DODGIT_UWB_H
#define DODGIT_UWB_H

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"
#include "members.h"
#include "pouch_fsm.h"

/* Probe and configure the DW3110. Returns 0 or -errno. Prints the device ID. */
int uwb_init(void);

/* ---------------------------------------------------------------------------
 * Ball side
 * ------------------------------------------------------------------------- */
struct pouch_range
{
  uint8_t slot;   /* timeslot of the pouch (0 .. MAX_POUCHES-1) */
  uint32_t uid;   /* the pouch's 32-bit hardware ID */
  int32_t mm;     /* measured distance */
  uint8_t lives;  /* lives the BALL has recorded for the pouch (the ball is the source of truth) */
};

/* Start listening for JOIN messages (call when entering Roll Call). */
void uwb_ball_rollcall_start(void);
/* Service the roll call; call often. On JOIN_NEW / JOIN_REPEAT / JOIN_FULL, *slot (-1 if full)
 * and *uid describe the pouch; a pouch that is registered is told its slot and lives (REMEMBER). */
enum join_result uwb_ball_rollcall_step(int *slot, uint32_t *uid);

int uwb_ball_pouch_count(void);          /* pouches in the table */
uint8_t uwb_ball_slot_mask(void);        /* bit n set = slot n in use */
uint32_t uwb_ball_slot_uid(int slot);    /* hardware ID in slot (0 if none) */
int uwb_ball_slot_lives(int slot);       /* lives the ball has recorded for slot, -1 if none */

/* The pouch in `slot` was hit: one life less in the ball's table (not below 0), done at once whether
 * or not the pouch ever acknowledges. Returns the lives remaining, -1 if the slot is empty. */
int uwb_ball_hit(int slot);

/* Send one REMEMBER (uid, slot, lives) if one is due. Call once per main-loop pass, BETWEEN radio
 * exchanges (it uses the radio). Each pouch gets one per REMEMBER_PERIOD_MS, as a trickle (one pouch
 * every REMEMBER_PERIOD_MS / pouches). A pouch uses it as proof that the ball still knows it, and a
 * rebooted pouch gets its slot and lives back from it. */
void uwb_ball_remember_tick(void);

/* One ranging round to every pouch in the table. Fills out[] (up to max) with the pouches
 * that completed the exchange and returns how many (0 = nobody answered), or -errno. */
int uwb_ball_range_round(struct pouch_range *out, int max);

/* Tell the pouch in `slot` how many lives it has left (the ball's table value, so call
 * uwb_ball_hit() first). Returns 0 once it acknowledged that value, else -errno. */
int uwb_ball_send_hit(int slot);

/* ---- Reset: the join window ------------------------------------------------------------------
 * A Reset is also how a pouch that was switched on late joins, so the ball listens for JOINs for the
 * whole Reset (not just until every pouch has acknowledged), and deletes the pouches that never
 * answered it. Usage: uwb_ball_reset_start() once, uwb_ball_reset_step() on every main-loop pass for
 * the whole Reset, then uwb_ball_reset_finish() once. */
struct reset_summary
{
  int joined;    /* NEW pouches registered during this Reset */
  int deleted;   /* silent pouches removed from the table (slot freed) */
  int remaining; /* pouches in the table now: all of them answered a RESET or joined */
};

/* Everybody's lives back to LIVES_MAX in the ball's table, nobody has acknowledged yet. */
void uwb_ball_reset_start(void);
/* Broadcast RESET every RESET_RETRY_MS until every pouch in the table has acknowledged it (a pouch
 * that joins counts as acknowledged), and keep listening for JOIN messages from pouches without a
 * slot: each is registered and told its slot (REMEMBER) at once. */
void uwb_ball_reset_step(void);
/* Close the window: delete every pouch that neither acknowledged nor joined (safe, because
 * everybody's lives were just reset, so nobody loses progress by being forgotten). */
struct reset_summary uwb_ball_reset_finish(void);

/* Switch the radio off (Armed state). */
void uwb_ball_idle(void);

/* ---------------------------------------------------------------------------
 * Pouch side
 * ------------------------------------------------------------------------- */
/* The pouch's phases and events are defined in pouch_fsm.h. */
struct pouch_event
{
  enum pouch_event_type type;
};

void uwb_pouch_start(void);            /* call once after uwb_init(): starts Joining */
uint32_t uwb_pouch_uid(void);          /* this pouch's 32-bit hardware ID */
enum pouch_phase uwb_pouch_phase(void);
int uwb_pouch_slot(void);              /* assigned timeslot, or -1 */
uint8_t uwb_pouch_lives(void);         /* lives as last told by the ball (HIT / REMEMBER / RESET) */

/* Run the pouch radio for one short step (<= ~40 ms). Call in a tight loop: replies are
 * timed to the radio's clock, but the pouch has to notice the incoming frame quickly. Does not print:
 * the caller logs phase changes and events after the step returns, so a slow console cannot make
 * a reply miss its timeslot. */
void uwb_pouch_step(struct pouch_event *ev);

#endif
