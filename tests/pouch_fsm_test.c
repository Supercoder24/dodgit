/* Host test of the pouch's membership FSM (shared/pouch_fsm.h): join, reboot recovery, late joining,
 * hits, resets, loss detection.
 *   cc -std=c11 -Wall -Wextra -Werror -Ishared tests/pouch_fsm_test.c -o /tmp/pouch_fsm_test && /tmp/pouch_fsm_test */
#include <assert.h>
#include <stdio.h>
#include "pouch_fsm.h"

static struct pouch_fsm joined_pouch(int64_t now, int slot, uint8_t lives)
{
  struct pouch_fsm f;
  pouch_fsm_init(&f, now, 100);
  assert(pouch_fsm_on_remember(&f, now, slot, lives) == EV_REMEMBER);
  assert(f.phase == POUCH_JOINED);
  return f;
}

static void test_normal_start(void)
{
  struct pouch_fsm f;

  /* power-up: Joining, the first JOIN only after the delay */
  pouch_fsm_init(&f, 1000, 150);
  assert(f.phase == POUCH_JOINING && f.slot == -1 && f.lives == LIVES_MAX);
  assert(!pouch_fsm_join_due(&f, 1149) && pouch_fsm_join_due(&f, 1150));

  /* the ball answers with REMEMBER: Joined, slot and lives from the ball */
  assert(pouch_fsm_on_remember(&f, 1200, 2, LIVES_MAX) == EV_REMEMBER);
  assert(f.phase == POUCH_JOINED && f.slot == 2 && !pouch_fsm_join_due(&f, 9999));

  /* the first POLL makes it Active and it answers; a RESET or a HIT would do the same */
  assert(pouch_fsm_on_poll(&f, 1300) == POLL_ANSWER && f.phase == POUCH_ACTIVE);
  assert(pouch_fsm_on_poll(&f, 1310) == POLL_ANSWER);

  struct pouch_fsm g = joined_pouch(0, 0, LIVES_MAX);
  enum pouch_event_type ev = EV_RESET;
  assert(pouch_fsm_on_hit(&g, LIVES_MAX - 1, &ev) && ev == EV_HIT && g.phase == POUCH_ACTIVE);
  struct pouch_fsm h = joined_pouch(0, 0, LIVES_MAX);
  assert(pouch_fsm_on_reset(&h) == RESET_ACKNOWLEDGE && h.phase == POUCH_ACTIVE);
}

static void test_reboot_mid_game(void)
{
  struct pouch_fsm f;

  /* a pouch switched on while the ball is ranging: the POLL comes before its first JOIN is due ... */
  pouch_fsm_init(&f, 0, 200);
  assert(pouch_fsm_on_poll(&f, 50) == POLL_IGNORE && f.phase == POUCH_LISTENING);
  assert(!pouch_fsm_join_due(&f, 500)); /* ... so it never transmits, whatever the clock says */
  assert(pouch_fsm_on_poll(&f, 250) == POLL_IGNORE && f.phase == POUCH_LISTENING);

  /* a REMEMBER about somebody else is not ours (the caller filters by uid); ours restores the old
   * slot and lives, then the next POLL makes it Active again */
  assert(pouch_fsm_on_remember(&f, 900, 3, 1) == EV_REMEMBER);
  assert(f.phase == POUCH_JOINED && f.slot == 3 && f.lives == 1);
  assert(pouch_fsm_on_poll(&f, 950) == POLL_ANSWER && f.phase == POUCH_ACTIVE && f.lives == 1);
}

static void test_remember_is_authoritative(void)
{
  struct pouch_fsm f = joined_pouch(0, 1, LIVES_MAX);

  pouch_fsm_on_poll(&f, 10); /* Active */

  /* a HIT that never arrived: the REMEMBER shows fewer lives, reported as a hit */
  assert(pouch_fsm_on_remember(&f, 1000, 1, LIVES_MAX - 1) == EV_HIT);
  assert(f.lives == LIVES_MAX - 1 && f.slot == 1);
  /* the same value again is just a heartbeat */
  assert(pouch_fsm_on_remember(&f, 2000, 1, LIVES_MAX - 1) == EV_REMEMBER);
  /* the ball may also move us or give lives back (after a Reset we missed) */
  assert(pouch_fsm_on_remember(&f, 3000, 4, LIVES_MAX) == EV_REMEMBER);
  assert(f.slot == 4 && f.lives == LIVES_MAX && f.phase == POUCH_ACTIVE);
  /* garbage is ignored */
  assert(pouch_fsm_on_remember(&f, 4000, MAX_POUCHES, 1) == EV_NONE && f.slot == 4);
  assert(pouch_fsm_on_remember(&f, 4000, 0, LIVES_MAX + 1) == EV_NONE && f.lives == LIVES_MAX);
  assert(f.last_remember_ms == 3000);

  /* a pouch that is still Joining gets no hit effect from a lower value (a reboot is not a hit) */
  struct pouch_fsm g;
  pouch_fsm_init(&g, 0, 0);
  assert(pouch_fsm_on_remember(&g, 5, 0, 1) == EV_REMEMBER);
}

static void test_hit_is_idempotent(void)
{
  struct pouch_fsm f = joined_pouch(0, 0, LIVES_MAX);
  enum pouch_event_type ev;

  assert(pouch_fsm_on_hit(&f, 2, &ev) && ev == EV_HIT && f.lives == 2);
  assert(pouch_fsm_on_hit(&f, 2, &ev) && ev == EV_NONE && f.lives == 2); /* retry: acknowledged, no new hit */
  assert(pouch_fsm_on_hit(&f, 1, &ev) && ev == EV_HIT && f.lives == 1);
  assert(pouch_fsm_on_hit(&f, 0, &ev) && ev == EV_HIT && f.lives == 0);

  /* not in the game: not accepted, not acknowledged */
  struct pouch_fsm j;
  pouch_fsm_init(&j, 0, 0);
  assert(!pouch_fsm_on_hit(&j, 1, &ev) && j.phase == POUCH_JOINING);
  pouch_fsm_on_poll(&j, 1); /* Listening */
  assert(!pouch_fsm_on_hit(&j, 1, &ev) && j.phase == POUCH_LISTENING);
}

static void test_reset_and_late_join(void)
{
  /* a joined pouch: lives back to the maximum, acknowledge */
  struct pouch_fsm f = joined_pouch(0, 2, 1);
  pouch_fsm_on_poll(&f, 1);
  assert(pouch_fsm_on_reset(&f) == RESET_ACKNOWLEDGE && f.lives == LIVES_MAX && f.phase == POUCH_ACTIVE);

  /* a late pouch (Listening): a RESET sends it Joining after the caller's random delay, silent until then */
  struct pouch_fsm g;
  pouch_fsm_init(&g, 0, 100);
  pouch_fsm_on_poll(&g, 10);
  assert(pouch_fsm_on_reset(&g) == RESET_REJOIN && g.phase == POUCH_LISTENING);
  pouch_fsm_enter_joining(&g, 5000, 400);
  assert(g.phase == POUCH_JOINING && g.slot == -1);
  assert(!pouch_fsm_join_due(&g, 5399) && pouch_fsm_join_due(&g, 5400));
  assert(pouch_fsm_on_remember(&g, 5500, 5, LIVES_MAX) == EV_REMEMBER && g.phase == POUCH_JOINED);

  /* a Joining pouch just keeps sending JOINs */
  struct pouch_fsm h;
  pouch_fsm_init(&h, 0, 100);
  assert(pouch_fsm_on_reset(&h) == RESET_IGNORE && h.phase == POUCH_JOINING);
}

static void test_loss_detection(void)
{
  struct pouch_fsm f = joined_pouch(1000, 1, 2);

  /* heartbeat = REMEMBER with our ID, nothing else keeps it alive (POLLs do not) */
  pouch_fsm_on_poll(&f, 1100);
  assert(!pouch_fsm_lost(&f, 1000 + LOST_TIMEOUT_MS - 1));
  pouch_fsm_on_poll(&f, 1000 + LOST_TIMEOUT_MS - 1);
  assert(pouch_fsm_lost(&f, 1000 + LOST_TIMEOUT_MS)); /* a deleted pouch still hears POLLs: it must stop */
  pouch_fsm_enter_joining(&f, 7000, 100);
  assert(f.phase == POUCH_JOINING && f.slot == -1 && !pouch_fsm_lost(&f, 99999));

  /* a REMEMBER restarts the countdown */
  struct pouch_fsm g = joined_pouch(0, 0, LIVES_MAX);
  pouch_fsm_on_remember(&g, 4000, 0, LIVES_MAX);
  assert(!pouch_fsm_lost(&g, 4000 + LOST_TIMEOUT_MS - 1) && pouch_fsm_lost(&g, 4000 + LOST_TIMEOUT_MS));

  /* Listening: the ball ranges all the time, so no POLL for LOST_TIMEOUT_MS = it rebooted (Roll Call) */
  struct pouch_fsm l;
  pouch_fsm_init(&l, 0, 100);
  pouch_fsm_on_poll(&l, 10);
  assert(!pouch_fsm_lost(&l, 10 + LOST_TIMEOUT_MS - 1));
  pouch_fsm_on_poll(&l, 3000);
  assert(!pouch_fsm_lost(&l, 3000 + LOST_TIMEOUT_MS - 1) && pouch_fsm_lost(&l, 3000 + LOST_TIMEOUT_MS));

  /* Joining never times out: it keeps trying */
  struct pouch_fsm j;
  pouch_fsm_init(&j, 0, 100);
  assert(!pouch_fsm_lost(&j, 1000000));
}

int main(void)
{
  test_normal_start();
  test_reboot_mid_game();
  test_remember_is_authoritative();
  test_hit_is_idempotent();
  test_reset_and_late_join();
  test_loss_detection();
  printf("pouch_fsm_test: OK\n");
  return 0;
}
