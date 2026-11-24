/* Host test of the ball's decision logic (ball/src/ball_logic.h): Disarm(), Classify(), Ready, Thrown.
 *   cc -std=c11 -Wall -Wextra -Werror -Ishared -Iball/src tests/ball_logic_test.c -o /tmp/ball_logic_test && /tmp/ball_logic_test */
#include <assert.h>
#include <stdio.h>
#include "ball_logic.h"

/* ranges for pouches 0..: {slot, uid, mm, lives} */
#define P(slot, mm, lives) {(slot), 0x1000 + (slot), (mm), (lives)}

static void test_disarm(void)
{
  struct pouch_range near_alive[] = {P(0, 600, 3), P(1, 1500, 3)};
  struct disarm_decision d = bl_disarm(near_alive, 2, true);
  assert(d.ready && d.thrower == 0 && d.mm == 600);

  /* not Held: a ball lying or bouncing next to a pouch (e.g. right after it hit it) stays Unheld */
  assert(!bl_disarm(near_alive, 2, false).ready);

  /* pickup range is inclusive; farther is not a pickup */
  struct pouch_range edge[] = {P(2, PICKUP_DISTANCE_MM, 1)};
  assert(bl_disarm(edge, 1, true).ready);
  struct pouch_range far1[] = {P(2, PICKUP_DISTANCE_MM + 1, 1)};
  assert(!bl_disarm(far1, 1, true).ready);

  /* a pouch without lives cannot pick up, and does not hide the alive one behind it */
  struct pouch_range dead_closest[] = {P(0, 200, 0), P(1, 800, 2)};
  d = bl_disarm(dead_closest, 2, true);
  assert(d.ready && d.thrower == 1);
  struct pouch_range only_dead[] = {P(0, 200, 0)};
  assert(!bl_disarm(only_dead, 1, true).ready);

  /* nobody answered */
  assert(!bl_disarm(NULL, 0, true).ready);
}

static void test_classify(void)
{
  struct pouch_range r[] = {P(0, 500, 3), P(1, 400, 3), P(2, 700, 3)};
  struct classify_decision d;

  /* the closest pouch is another one: hit */
  d = bl_classify(r, 3, 0, 3);
  assert(d.result == CLASSIFY_HIT && d.slot == 1 && d.mm == 400);

  /* the closest is the thrower: back to Ready */
  d = bl_classify(r, 3, 1, 3);
  assert(d.result == CLASSIFY_THROWER && d.slot == 1);

  /* hit range */
  struct pouch_range edge[] = {P(1, HIT_DISTANCE_MM, 3)};
  assert(bl_classify(edge, 1, 0, 2).result == CLASSIFY_HIT);
  struct pouch_range beyond[] = {P(1, HIT_DISTANCE_MM + 1, 3)};
  assert(bl_classify(beyond, 1, 0, 2).result == CLASSIFY_NO_POUCH);
  assert(bl_classify(NULL, 0, 0, 2).result == CLASSIFY_NO_POUCH);

  /* the closest pouch is OUT of lives: a miss, NOT the next-closest living one (nobody around an
   * eliminated player loses a life because of it) */
  struct pouch_range out[] = {P(1, 300, 0), P(2, 500, 3)};
  d = bl_classify(out, 2, 0, 3);
  assert(d.result == CLASSIFY_OUT_OF_LIVES && d.slot == 1);
  struct pouch_range out_closest[] = {P(1, 300, 2), P(2, 200, 0)};
  assert(bl_classify(out_closest, 2, 0, 3).result == CLASSIFY_OUT_OF_LIVES);
  /* ... and a living pouch that is the closest is hit even if another one is out */
  struct pouch_range alive_closest[] = {P(1, 200, 2), P(2, 300, 0)};
  assert(bl_classify(alive_closest, 2, 0, 3).result == CLASSIFY_HIT);

  /* single-pouch bench test: with exactly one pouch the thrower can be hit; with two it cannot */
  struct pouch_range self[] = {P(0, 300, 3)};
  assert(bl_classify(self, 1, 0, 1).result == CLASSIFY_SELF_HIT);
  assert(bl_classify(self, 1, 0, 2).result == CLASSIFY_THROWER);
}

static void test_merge_rounds(void)
{
  struct pouch_range acc[MAX_POUCHES];
  int n = 0;

  /* round 1: the pouch that was hit (slot 1, very close) is silent, the thrower and a bystander answer */
  struct pouch_range r1[] = {P(0, 3200, 3), P(2, 2500, 3)};
  bl_merge_round(acc, &n, MAX_POUCHES, r1, 2);
  assert(n == 2 && bl_classify(acc, n, 0, 3).result == CLASSIFY_NO_POUCH);

  /* round 2: it answers: it is the closest pouch now and gets the hit */
  struct pouch_range r2[] = {P(1, 150, 3), P(0, 3190, 3)};
  bl_merge_round(acc, &n, MAX_POUCHES, r2, 2);
  assert(n == 3);
  struct classify_decision d = bl_classify(acc, n, 0, 3);
  assert(d.result == CLASSIFY_HIT && d.slot == 1 && d.mm == 150);

  /* the smallest distance of the rounds is kept, the lives are the latest ones */
  struct pouch_range r3[] = {P(1, 400, 2)};
  bl_merge_round(acc, &n, MAX_POUCHES, r3, 1);
  assert(n == 3 && acc[bl_closest(acc, n, false)].mm == 150);
  assert(acc[bl_closest(acc, n, false)].lives == 2);

  /* a full table never overflows */
  n = 0;
  for (int slot = 0; slot < MAX_POUCHES; slot++)
  {
    struct pouch_range one[] = {P(slot, 100 + slot, 3)};
    bl_merge_round(acc, &n, MAX_POUCHES, one, 1);
  }
  struct pouch_range extra[] = {P(MAX_POUCHES, 50, 3)};
  bl_merge_round(acc, &n, MAX_POUCHES, extra, 1);
  assert(n == MAX_POUCHES);
}

/* One Ready round at time t. */
static enum ready_result ready(struct ready_ctx *c, int64_t t, const struct pouch_range *r, int n,
                               bool held, bool spinning)
{
  return bl_ready_update(c, t, r, n, held, spinning);
}

static void test_ready_throw(void)
{
  struct ready_ctx c;
  bl_ready_enter(&c, 0, 400);

  /* in the hand, ball Held, thrower close: nothing happens (also with a bystander near the ball) */
  struct pouch_range held[] = {P(0, 400, 3), P(1, 1500, 3)};
  for (int64_t t = 0; t < 2000; t += 20)
  {
    assert(ready(&c, t, held, 2, true, false) == READY_STAY);
  }

  /* Spinning but the thrower is still within throw range: not (yet) thrown */
  struct pouch_range flying_near[] = {P(0, THROW_RANGE_MM, 3)};
  assert(ready(&c, 2000, flying_near, 1, false, true) == READY_STAY);

  /* out of range without Spinning (somebody walks away with the ball): not thrown */
  struct pouch_range far0[] = {P(0, THROW_RANGE_MM + 1, 3)};
  assert(ready(&c, 2020, far0, 1, true, false) == READY_STAY);

  /* Spinning AND the thrower beyond the throw range -> Thrown, whatever the other pouches do */
  assert(ready(&c, 2040, far0, 1, false, true) == READY_THROWN);
  struct pouch_range bystander[] = {P(0, 3000, 3), P(1, 400, 3), P(2, 600, 3)}; /* standing next to the flight path */
  bl_ready_enter(&c, 0, 400);
  assert(ready(&c, 0, bystander, 3, false, true) == READY_THROWN);
  assert(c.thrower == 0);
}

static void test_ready_silent_thrower(void)
{
  struct ready_ctx c;

  /* ranging fails when ball and pouch nearly touch: silent after a very close reading = still there,
   * so a Spinning ball is NOT thrown (and a held one does not drop to Unheld as an orphan) */
  bl_ready_enter(&c, 0, TOO_CLOSE_MM);
  assert(ready(&c, 0, NULL, 0, false, true) == READY_STAY);
  assert(ready(&c, 10 * THROWER_SILENT_MS, NULL, 0, false, true) == READY_STAY);
  for (int64_t t = 20; t < 3 * READY_ORPHAN_MS; t += 20)
  {
    assert(ready(&c, t, NULL, 0, true, false) == READY_STAY);
  }

  /* silent after a farther reading: one dropped round while Spinning is NOT a throw (it keeps the last
   * range) ... */
  bl_ready_enter(&c, 0, 800);
  struct pouch_range answers[] = {P(0, 800, 3)};
  assert(ready(&c, 0, answers, 1, false, true) == READY_STAY);
  assert(ready(&c, 20, NULL, 0, false, true) == READY_STAY);
  assert(ready(&c, 40, answers, 1, false, true) == READY_STAY); /* the silence ended: the clock restarts */
  assert(ready(&c, 60, NULL, 0, false, true) == READY_STAY);
  assert(ready(&c, 60 + THROWER_SILENT_MS - 1, NULL, 0, false, true) == READY_STAY);
  /* ... but a silence of THROWER_SILENT_MS or more means far away (blocked or out of range): Spinning = thrown */
  assert(ready(&c, 60 + THROWER_SILENT_MS, NULL, 0, false, true) == READY_THROWN);

  /* the last range counts while it is silent: it was beyond the throw range already */
  bl_ready_enter(&c, 0, THROW_RANGE_MM + 500);
  assert(ready(&c, 0, NULL, 0, false, true) == READY_THROWN);

  /* the memory is refreshed by every valid range */
  bl_ready_enter(&c, 0, 800);
  struct pouch_range close_now[] = {P(0, 100, 3)};
  assert(ready(&c, 10, close_now, 1, true, false) == READY_STAY && c.last_thrower_mm == 100);
  assert(ready(&c, 20 + 10 * THROWER_SILENT_MS, NULL, 0, false, true) == READY_STAY);
}

static void test_ready_orphan_and_not_held(void)
{
  struct ready_ctx c;
  struct pouch_range gone[] = {P(0, THROW_RANGE_MM + 500, 3)};

  /* nobody alive within throw range and not Spinning for READY_ORPHAN_MS -> Unheld */
  bl_ready_enter(&c, 0, 400);
  assert(ready(&c, 0, gone, 1, true, false) == READY_STAY);
  assert(ready(&c, READY_ORPHAN_MS - 1, gone, 1, true, false) == READY_STAY);
  assert(ready(&c, READY_ORPHAN_MS, gone, 1, true, false) == READY_ORPHAN);

  /* any round with somebody in range restarts the count */
  bl_ready_enter(&c, 0, 400);
  struct pouch_range back[] = {P(0, 500, 3)};
  ready(&c, 0, gone, 1, true, false);
  ready(&c, 600, back, 1, true, false);
  assert(ready(&c, 1200, gone, 1, true, false) == READY_STAY);
  assert(ready(&c, 1200 + READY_ORPHAN_MS, gone, 1, true, false) == READY_ORPHAN);

  /* Spinning pauses the orphan count (the throw has priority) */
  bl_ready_enter(&c, 0, 400);
  ready(&c, 0, gone, 1, true, false);
  struct pouch_range alive_far[] = {P(0, THROW_RANGE_MM + 500, 3)};
  assert(ready(&c, 500, alive_far, 1, false, true) == READY_THROWN);

  /* a pouch without lives does not keep the ball in Ready */
  bl_ready_enter(&c, 0, 400);
  struct pouch_range dead_near[] = {P(0, THROW_RANGE_MM + 500, 3), P(1, 300, 0)};
  ready(&c, 0, dead_near, 2, true, false);
  assert(ready(&c, READY_ORPHAN_MS, dead_near, 2, true, false) == READY_ORPHAN);

  /* not Held for READY_NOT_HELD_MS -> Unheld; Held again restarts it */
  bl_ready_enter(&c, 0, 400);
  struct pouch_range here[] = {P(0, 300, 3)};
  assert(ready(&c, 0, here, 1, false, false) == READY_STAY);
  assert(ready(&c, READY_NOT_HELD_MS - 1, here, 1, false, false) == READY_STAY);
  assert(ready(&c, READY_NOT_HELD_MS, here, 1, false, false) == READY_NOT_HELD);
  bl_ready_enter(&c, 0, 400);
  ready(&c, 0, here, 1, false, false);
  ready(&c, 4000, here, 1, true, false);
  assert(ready(&c, 4000 + READY_NOT_HELD_MS - 1, here, 1, false, false) == READY_STAY);

  /* Spinning inside the throw range is "not Held" too: after READY_NOT_HELD_MS it is Unheld
   * (a point-blank throw is not a throw; its impact is ignored) */
  bl_ready_enter(&c, 0, 400);
  ready(&c, 0, here, 1, false, true);
  assert(ready(&c, READY_NOT_HELD_MS, here, 1, false, true) == READY_NOT_HELD);

  /* ... but a real throw wins when both apply */
  bl_ready_enter(&c, 0, 400);
  ready(&c, 0, here, 1, false, true);
  struct pouch_range far0[] = {P(0, THROW_RANGE_MM + 1, 3)};
  assert(ready(&c, READY_NOT_HELD_MS, far0, 1, false, true) == READY_THROWN);
}

static void test_ready_thrower_switch(void)
{
  struct ready_ctx c;

  /* two kids standing close together: pouch 1 is a bit closer than the thrower, but only for a moment */
  bl_ready_enter(&c, 0, 500);
  struct pouch_range b_closer[] = {P(0, 500, 3), P(1, 450, 3)};
  struct pouch_range a_closer[] = {P(0, 440, 3), P(1, 450, 3)};
  ready(&c, 0, b_closer, 2, true, false);
  ready(&c, 500, b_closer, 2, true, false);
  ready(&c, 900, a_closer, 2, true, false); /* a break: the count starts over */
  ready(&c, 1000, b_closer, 2, true, false);
  assert(ready(&c, 1000 + THROWER_SWITCH_MS - 1, b_closer, 2, true, false) == READY_STAY && c.thrower == 0);

  /* a hand-over: pouch 1 stays closer for THROWER_SWITCH_MS -> it is the thrower now */
  bl_ready_enter(&c, 0, 500);
  ready(&c, 0, b_closer, 2, true, false);
  assert(c.thrower == 0);
  ready(&c, THROWER_SWITCH_MS, b_closer, 2, true, false);
  assert(c.thrower == 1 && c.last_thrower_mm == 450);

  /* a third pouch becoming the closest restarts the count for that pouch */
  bl_ready_enter(&c, 0, 600);
  struct pouch_range b2[] = {P(0, 600, 3), P(1, 500, 3), P(2, 700, 3)};
  struct pouch_range c2[] = {P(0, 600, 3), P(1, 550, 3), P(2, 400, 3)};
  ready(&c, 0, b2, 3, true, false);
  ready(&c, 800, c2, 3, true, false); /* pouch 2 is the closest now */
  ready(&c, 1000, c2, 3, true, false);
  assert(c.thrower == 0);
  ready(&c, 800 + THROWER_SWITCH_MS, c2, 3, true, false);
  assert(c.thrower == 2);

  /* a closer pouch without lives never takes over */
  bl_ready_enter(&c, 0, 600);
  struct pouch_range dead[] = {P(0, 600, 3), P(1, 100, 0)};
  ready(&c, 0, dead, 2, true, false);
  ready(&c, 10 * THROWER_SWITCH_MS, dead, 2, true, false);
  assert(c.thrower == 0);

  /* a silent thrower that was very close keeps the ball: nobody else is "closer than its last 100 mm" */
  bl_ready_enter(&c, 0, 100);
  struct pouch_range other[] = {P(1, 300, 3)};
  ready(&c, 0, other, 1, true, false);
  ready(&c, 10 * THROWER_SWITCH_MS, other, 1, true, false);
  assert(c.thrower == 0);

  /* HAND-OVER WITH DROPPED ROUNDS (a pouch is missing from ~8% of the rounds at these distances): the
   * receiver is missing every other round, the thrower every third: the streak survives and the
   * thrower still changes after THROWER_SWITCH_MS */
  bl_ready_enter(&c, 0, 250);
  struct pouch_range both[] = {P(0, 250, 3), P(1, 150, 3)};
  struct pouch_range only_thrower[] = {P(0, 250, 3)};
  struct pouch_range only_receiver[] = {P(1, 150, 3)};
  int64_t t = 0;
  int round = 0;
  while (c.thrower == 0 && t < 3 * THROWER_SWITCH_MS)
  {
    if (round % 6 == 0)
    {
      ready(&c, t, NULL, 0, true, false); /* nobody answered at all */
    }
    else if (round % 2 == 0)
    {
      ready(&c, t, only_thrower, 1, true, false); /* the receiver is missing */
    }
    else if (round % 3 == 0)
    {
      ready(&c, t, only_receiver, 1, true, false); /* the thrower is missing */
    }
    else
    {
      ready(&c, t, both, 2, true, false);
    }
    t += 20;
    round++;
  }
  assert(c.thrower == 1 && t >= THROWER_SWITCH_MS && t <= THROWER_SWITCH_MS + 3 * 20 * 6);

  /* ... but a candidate that has not been heard for longer than the grace time loses its streak, and one that is
   * heard and NOT closer does at once */
  bl_ready_enter(&c, 0, 250);
  ready(&c, 0, both, 2, true, false);
  ready(&c, 100, only_thrower, 1, true, false);
  ready(&c, THROWER_SWITCH_GRACE_MS, only_thrower, 1, true, false);
  assert(c.candidate == 1);
  ready(&c, THROWER_SWITCH_GRACE_MS + 1, only_thrower, 1, true, false);
  assert(c.candidate == -1);
  struct pouch_range thrower_closer[] = {P(0, 100, 3), P(1, 150, 3)};
  ready(&c, 1000, both, 2, true, false);
  assert(c.candidate == 1);
  ready(&c, 1020, thrower_closer, 2, true, false);
  assert(c.candidate == -1);
}

static void test_thrown(void)
{
  struct thrown_ctx c;
  int slot = -1;
  int32_t mm = -1;

  /* timeout */
  bl_thrown_enter(&c, 1000);
  assert(bl_thrown_tick(&c, 1000 + THROW_TIMEOUT_MS - 1, false) == THROWN_STAY);
  assert(bl_thrown_tick(&c, 1000 + THROW_TIMEOUT_MS, false) == THROWN_TIMEOUT);

  /* landed and lies still: Disarm() early. Rest must last THROWN_REST_MS without a break. */
  bl_thrown_enter(&c, 0);
  assert(bl_thrown_tick(&c, 100, true) == THROWN_STAY);
  assert(bl_thrown_tick(&c, 100 + THROWN_REST_MS - 1, true) == THROWN_STAY);
  assert(bl_thrown_tick(&c, 100 + THROWN_REST_MS, true) == THROWN_REST);
  bl_thrown_enter(&c, 0);
  bl_thrown_tick(&c, 100, true);
  bl_thrown_tick(&c, 600, false); /* it moved again */
  assert(bl_thrown_tick(&c, 100 + THROWN_REST_MS, true) == THROWN_STAY);

  /* the thrower is back (throw cancelled): the same pouch within throw range for THROWN_RETURN_MS -> Ready */
  struct pouch_range back[] = {P(0, 900, 3), P(1, 5000, 3)};
  bl_thrown_enter(&c, 0);
  for (int64_t t = 250; t < THROWN_RETURN_MS + 250; t += THROWN_RECHECK_MS)
  {
    assert(!bl_thrown_round(&c, t, back, 2, &slot, &mm));
  }
  assert(bl_thrown_round(&c, 250 + THROWN_RETURN_MS, back, 2, &slot, &mm));
  assert(slot == 0 && mm == 900);

  /* the ball ended up nearer to ANOTHER pouch (possession changes); a different closest pouch starts over */
  struct pouch_range other[] = {P(0, 4000, 3), P(2, 1500, 3)};
  struct pouch_range out_of_range[] = {P(0, 4000, 3), P(2, 2500, 3)};
  bl_thrown_enter(&c, 0);
  assert(!bl_thrown_round(&c, 250, other, 2, &slot, &mm));
  assert(!bl_thrown_round(&c, 500, out_of_range, 2, &slot, &mm)); /* heard, but out of range: the count resets */
  assert(!bl_thrown_round(&c, 750, other, 2, &slot, &mm));
  assert(!bl_thrown_round(&c, 750 + THROWN_RETURN_MS - 1, other, 2, &slot, &mm));
  assert(bl_thrown_round(&c, 750 + THROWN_RETURN_MS, other, 2, &slot, &mm));
  assert(slot == 2 && mm == 1500);

  /* a re-check in which the pouch is missing (a dropped round, or nobody answered) does not break the streak ... */
  struct pouch_range others_only[] = {P(0, 4000, 3)};
  bl_thrown_enter(&c, 0);
  assert(!bl_thrown_round(&c, 250, other, 2, &slot, &mm));
  assert(!bl_thrown_round(&c, 500, others_only, 1, &slot, &mm));
  assert(!bl_thrown_round(&c, 750, NULL, 0, &slot, &mm));
  assert(bl_thrown_round(&c, 250 + THROWN_RETURN_MS, other, 2, &slot, &mm) && slot == 2);
  /* ... unless it stays missing for 3 re-checks */
  bl_thrown_enter(&c, 0);
  bl_thrown_round(&c, 250, other, 2, &slot, &mm);
  bl_thrown_round(&c, 500, others_only, 1, &slot, &mm);
  bl_thrown_round(&c, 750, others_only, 1, &slot, &mm);
  bl_thrown_round(&c, 1000, others_only, 1, &slot, &mm);
  bl_thrown_round(&c, 1100, others_only, 1, &slot, &mm); /* > 3 re-checks since it was last seen */
  assert(!bl_thrown_round(&c, 250 + THROWN_RETURN_MS + 500, other, 2, &slot, &mm)); /* a new streak starts */

  /* the CLOSEST pouch has to stay the same one for the whole time */
  struct pouch_range a_closest[] = {P(0, 1200, 3), P(1, 1900, 3)};
  struct pouch_range b_closest[] = {P(0, 1900, 3), P(1, 1200, 3)};
  bl_thrown_enter(&c, 0);
  bl_thrown_round(&c, 0, a_closest, 2, &slot, &mm);
  bl_thrown_round(&c, 600, b_closest, 2, &slot, &mm);
  assert(!bl_thrown_round(&c, THROWN_RETURN_MS, b_closest, 2, &slot, &mm));
  assert(bl_thrown_round(&c, 600 + THROWN_RETURN_MS, b_closest, 2, &slot, &mm) && slot == 1);

  /* a pouch without lives never gets the ball; nobody answering is not "back" */
  struct pouch_range dead[] = {P(0, 500, 0)};
  bl_thrown_enter(&c, 0);
  bl_thrown_round(&c, 0, dead, 1, &slot, &mm);
  assert(!bl_thrown_round(&c, 10 * THROWN_RETURN_MS, dead, 1, &slot, &mm));
  assert(!bl_thrown_round(&c, 10 * THROWN_RETURN_MS + 1, NULL, 0, &slot, &mm));
}

int main(void)
{
  test_disarm();
  test_classify();
  test_merge_rounds();
  test_ready_throw();
  test_ready_silent_thrower();
  test_ready_orphan_and_not_held();
  test_ready_thrower_switch();
  test_thrown();
  printf("ball_logic_test: OK\n");
  return 0;
}
