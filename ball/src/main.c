/*
 * dodgit - Ball firmware (DWM3001CDK)
 *
 * Startup ("Off" state in docs/FSMs.md):
 *   Self-test: DW3110 UWB radio probe + accelerometer init.
 *     PASS: led0 (green) blinks 3 times, then the state machine starts in Roll Call.
 *     FAIL: led1 + led2 (red) blink forever (reason printed on the console).
 *
 * State machine (docs/FSMs.md "Ball Main FSM"); the decision rules are pure functions in ball_logic.h:
 *   Roll Call -> Disarm() -> Unheld | Ready
 *   Unheld    nobody is holding the ball: hits cannot register.   -> Ready when picked up
 *   Ready     a player holds the ball.                            -> Thrown | Unheld
 *   Thrown    in flight, waiting for the accelerometer's Hit.     -> Classify() | Disarm() | Ready
 *   Hit       tell the pouch it lost a life.                      -> Disarm()
 *   Reset     (button held >= 3 s, any state except Roll Call)    -> Roll Call | Disarm()
 * Disarm() and Classify() are functions (one ranging round + a decision), not states:
 *   Disarm()   an alive pouch within pickup range AND the ball Held -> Ready, else Unheld
 *   Classify() the closest pouch decides: miss -> Unheld, the thrower -> Ready, another pouch -> Hit
 *
 * The ball owns the lives (table in shared/uwb.c): a Hit is counted at once, the pouch is told with
 * HIT and kept in sync by the REMEMBER trickle (uwb_ball_remember_tick, every pass in every state).
 *
 * The ball's button (the sw0 alias = the external button on header J10, see hw.c):
 *   short press         : leave Roll Call (only once at least one pouch has registered)
 *   hold (BUTTON_HOLD_MS): Reset - everybody's lives back to LIVES_MAX; also the join window for
 *                         pouches switched on late (they join without turning anything else off)
 * (It is unrelated to the board's hardware RESET button, which reboots the MCU.)
 *
 * What the LEDs show is data in shared/led_patterns.h (one color at a time, steady vs. blinking);
 * the pattern of each state is chosen in leds_update() below.
 *
 * Everything the ball does is also printed on the console (J-Link USB serial, 115200 baud), sorted into
 * categories with compile-time switches in shared/settings.h (shared/log.h). With BLE_DEBUG (below)
 * the same log is also sent over Bluetooth: tools/ball_monitor.py.
 */

/* ---------------------------------------------------------------------------
 * DEBUG FLAG
 *   BLE_DEBUG defined : the ball advertises as "dodgit-ball" (Nordic UART Service) and
 *                       sends every log line to a connected laptop (tools/ball_monitor.py).
 *                       Nothing depends on the connection: the ball behaves exactly the
 *                       same with nobody connected, and the console log is unchanged.
 *   BLE_DEBUG removed : no Bluetooth at all (log goes to the console only).
 * (The Bluetooth stack is configured in prj.conf; it is simply never started when the
 * flag is off.)
 * ------------------------------------------------------------------------- */
#define BLE_DEBUG

#include <zephyr/kernel.h>
#include "settings.h"
#include "log.h"
#include "led_patterns.h"
#include "uwb.h"
#include "impact.h"
#include "ball_logic.h"
#include "hw.h"
#ifdef BLE_DEBUG
#include "ble_log.h"
#endif

#define LOOP_PERIOD_MS 10

enum ball_state
{
  STATE_OFF, /* before the state machine starts (self-test): only ever the "from" of the first transition */
  STATE_ROLL_CALL,
  STATE_UNHELD,
  STATE_READY,
  STATE_THROWN,
  STATE_HIT,
  STATE_RESET,
};

static const char *state_name(enum ball_state s)
{
  switch (s)
  {
  case STATE_OFF: return "Off";
  case STATE_ROLL_CALL: return "Roll Call";
  case STATE_UNHELD: return "Unheld";
  case STATE_READY: return "Ready";
  case STATE_THROWN: return "Thrown";
  case STATE_HIT: return "Hit";
  case STATE_RESET: return "Reset";
  }
  return "?";
}

static enum ball_state state = STATE_OFF;
static int64_t state_entered_ms;

/* Data carried between states */
static struct ready_ctx ready;    /* Ready: thrower and the timers of the Ready rules (the thrower is kept in Thrown) */
static struct thrown_ctx thrown;  /* Thrown: timers */
static int64_t thrown_next_recheck_ms; /* Thrown: when the next ranging re-check is due */
static int hit_slot = -1;         /* pouch that was hit (set in Classify, used in Hit) */
static uint16_t hit_count;        /* hits since boot (log only) */
static int confirm_rounds;        /* Unheld: consecutive rounds the pickup condition held */
static int pickup_candidate = -1; /* Unheld: slot being confirmed as the picker-up */
static int64_t last_checkin_ms;   /* Roll Call: when the last NEW pouch registered (or Roll Call began) */
static int64_t flash_until_ms;    /* Roll Call: end of the green / red flash */
static uint8_t flash_mask;        /* Roll Call: LED of that flash */
static int64_t last_log_ms;       /* throttle for the range log */

static struct pouch_range ranges[MAX_POUCHES];

static void enter_state(enum ball_state next)
{
  /* up= is the ball's uptime in ms, the same clock as STATUS/RANGE/ACCEL/BURST: the laptop's
   * receive times are bunched by Bluetooth batching and cannot be used to order events. */
  LOG_CAT(LOG_STATE, "State: %s -> %s (up=%lld)", state_name(state), state_name(next),
          (long long)k_uptime_get());
  state = next;
  state_entered_ms = k_uptime_get();
  confirm_rounds = 0; /* every state starts its own confirmation count */
  pickup_candidate = -1;

  switch (next)
  {
  case STATE_ROLL_CALL:
    last_checkin_ms = state_entered_ms; /* re-entered after a Reset: the timeout starts over */
    flash_until_ms = 0;
    uwb_ball_rollcall_start();
    break;
  case STATE_THROWN:
    uwb_ball_idle(); /* radio off; run_thrown() re-ranges only every THROWN_RECHECK_MS */
    bl_thrown_enter(&thrown, state_entered_ms);
    thrown_next_recheck_ms = state_entered_ms + THROWN_RECHECK_MS;
    break;
  case STATE_RESET:
    uwb_ball_reset_start();
    break;
  case STATE_OFF:
  case STATE_UNHELD:
  case STATE_READY:
  case STATE_HIT:
    break;
  }
}

/* The LEDs of the current state (docs/FSMs.md); called once per loop pass. */
static void leds_update(int64_t now)
{
  const struct led_pattern *p = &PAT_BALL_UNHELD;
  int64_t t = now; /* free-running pattern */

  switch (state)
  {
  case STATE_OFF:
  case STATE_ROLL_CALL:
    if (now < flash_until_ms)
    {
      leds_show(flash_mask); /* a flash REPLACES the steady blue: one color at a time */
      return;
    }
    p = &PAT_BALL_ROLL_CALL;
    break;
  case STATE_UNHELD: p = &PAT_BALL_UNHELD; break;
  case STATE_READY: p = &PAT_BALL_READY; break;
  case STATE_THROWN: p = &PAT_BALL_THROWN; break;
  case STATE_HIT: p = &PAT_BALL_HIT; break;
  case STATE_RESET:
    p = &PAT_BALL_RESET;
    t = now - state_entered_ms; /* the first blink starts immediately */
    break;
  }
  leds_show(led_pattern_mask(p, t));
}

/* Print every pouch of the latest round, at most RANGE_LOG_PERIOD_MS apart. */
static void log_ranges(int n)
{
  if (!LOG_RANGE)
  {
    return;
  }
  int64_t now = k_uptime_get();
  if (now - last_log_ms < RANGE_LOG_PERIOD_MS)
  {
    return;
  }
  last_log_ms = now;
  if (n == 0)
  {
    LOG_CAT(LOG_RANGE, "RANGE %lld none answered", (long long)now);
  }
  for (int i = 0; i < n; i++)
  {
    LOG_CAT(LOG_RANGE, "RANGE %lld slot=%u uid=0x%08x distance=%d mm lives=%u", (long long)now,
            ranges[i].slot, ranges[i].uid, ranges[i].mm, ranges[i].lives);
  }
}

/* One ranging round into ranges[], logged. Returns the number of pouches that answered (0 = nobody), or
 * -errno if the round itself failed. */
static int range_once(void)
{
  int n = uwb_ball_range_round(ranges, MAX_POUCHES);

  if (n >= 0)
  {
    log_ranges(n);
  }
  return n;
}

/* Ranging for a decision (Disarm(), Classify()): up to max_rounds rounds, merged per pouch into
 * ranges[] (a pouch heard in any round counts, at its smallest distance), stopping early once
 * stop_after rounds have heard somebody. Returns the number of pouches in ranges[] (0 = nobody). */
static int range_decision(int max_rounds, int stop_after)
{
  struct pouch_range round[MAX_POUCHES];
  int n = 0;
  int answered = 0;

  for (int attempt = 0; attempt < max_rounds && answered < stop_after; attempt++)
  {
    int m = uwb_ball_range_round(round, MAX_POUCHES);
    if (m > 0)
    {
      bl_merge_round(ranges, &n, MAX_POUCHES, round, m);
      answered++;
    }
  }
  last_log_ms = 0; /* always show what a decision is based on */
  log_ranges(n);
  return n;
}

static void enter_ready(int thrower, int32_t mm)
{
  enter_state(STATE_READY);
  bl_ready_enter(&ready, thrower, mm);
}

/* Disarm(): range once (more rounds only if nobody answers) and decide Ready or Unheld. */
static void disarm(void)
{
  int n = range_decision(RANGE_ATTEMPTS, 1);
  enum accel_level level = impact_level();
  struct disarm_decision d = bl_disarm(ranges, n, level == ACCEL_HELD);

  if (d.ready)
  {
    LOG_CAT(LOG_STATE, "Disarm: slot %d within pickup range (%d mm) and the ball is Held -> Ready",
            d.thrower, d.mm);
    enter_ready(d.thrower, d.mm);
  }
  else
  {
    LOG_CAT(LOG_STATE,
            "Disarm: no alive pouch within %d mm, or the ball is not Held (%s) -> Unheld",
            PICKUP_DISTANCE_MM, accel_level_name(level));
    enter_state(STATE_UNHELD);
  }
}

/* Classify(): a few ranging rounds, merged; the closest pouch (alive or not) decides what the impact was. */
static void classify(void)
{
  int n = range_decision(CLASSIFY_ROUNDS, CLASSIFY_ROUNDS);
  struct classify_decision d = bl_classify(ranges, n, ready.thrower, uwb_ball_pouch_count());

  switch (d.result)
  {
  case CLASSIFY_NO_POUCH:
    LOG_CAT(LOG_STATE, "Classify: no pouch within %d mm -> miss", HIT_DISTANCE_MM);
    enter_state(STATE_UNHELD);
    break;
  case CLASSIFY_OUT_OF_LIVES:
    LOG_CAT(LOG_STATE, "Classify: closest pouch (slot %d, %d mm) is out of lives -> miss", d.slot,
            d.mm);
    enter_state(STATE_UNHELD);
    break;
  case CLASSIFY_THROWER:
    LOG_CAT(LOG_STATE, "Classify: closest is the thrower (slot %d) -> back to Ready", d.slot);
    enter_ready(d.slot, d.mm);
    break;
  case CLASSIFY_SELF_HIT:
    LOG_CAT(LOG_STATE, "Classify: thrower is the only pouch -> treating it as hit (single-pouch test)");
    /* fall through */
  case CLASSIFY_HIT:
  {
    hit_slot = d.slot;
    hit_count++;
    /* The ball owns the lives: count the hit now, whether or not the pouch ever acknowledges. */
    int lives_left = uwb_ball_hit(hit_slot);
    LOG_CAT(LOG_STATE, "Classify: HIT on slot %d (%d mm), hit #%u, lives left %d", hit_slot, d.mm,
            hit_count, lives_left);
    enter_state(STATE_HIT);
    break;
  }
  }
}

/* Roll Call
 *   Pouches announce themselves; each new one gets the next free timeslot.
 *   Leave when SW0 is pressed (handled in main) or ROLLCALL_TIMEOUT_MS after the last
 *   NEW registration (or after Roll Call began, if it was re-entered). Never leave before at least
 *   one pouch has registered. */
static void run_roll_call(void)
{
  int slot;
  uint32_t uid;
  int64_t now = k_uptime_get();

  switch (uwb_ball_rollcall_step(&slot, &uid))
  {
  case JOIN_NEW:
    LOG_CAT(LOG_STATE, "ROLLCALL new pouch uid=0x%08x -> slot %d (%d registered)", uid, slot,
            uwb_ball_pouch_count());
    last_checkin_ms = now; /* only NEW pouches restart the timeout */
    flash_mask = LED_BALL_FLASH_NEW_POUCH;
    flash_until_ms = now + LED_BALL_FLASH_MS;
    break;
  case JOIN_REPEAT: /* the pouch asked again: it got the same slot (logged as MEMBER join) */
  case JOIN_FULL:   /* ninth pouch: ignored, does not restart the timeout */
  case JOIN_NONE:
    break;
  }

  if (uwb_ball_pouch_count() > 0 && now - last_checkin_ms >= ROLLCALL_TIMEOUT_MS)
  {
    LOG_CAT(LOG_STATE, "ROLLCALL timeout, %d pouch(es) registered", uwb_ball_pouch_count());
    disarm();
  }
}

/* Unheld
 *   Constantly ranging. An alive pouch within PICKUP_DISTANCE_MM AND the accelerometer reads Held
 *   (a player picked the ball up) for TRANSITION_CONFIRM_ROUNDS rounds in a row -> Ready. A ball
 *   lying on the floor next to a pouch is not "picked up". */
static void run_unheld(void)
{
  int n = range_once();
  if (n < 0)
  {
    return;
  }

  struct disarm_decision d = bl_disarm(ranges, n, impact_level() == ACCEL_HELD);
  if (!d.ready)
  {
    confirm_rounds = 0; /* a single bad round restarts the count */
    pickup_candidate = -1;
    return;
  }
  if (d.thrower != pickup_candidate)
  {
    pickup_candidate = d.thrower;
    confirm_rounds = 0;
  }
  if (++confirm_rounds >= TRANSITION_CONFIRM_ROUNDS)
  {
    LOG_CAT(LOG_STATE, "Pickup by slot %d (%d mm)", d.thrower, d.mm);
    enter_ready(d.thrower, d.mm);
  }
}

/* Ready
 *   A player holds the ball. Constantly ranging; Hit events are ignored (the launch spike, bumps in
 *   the hand, and point-blank throws are not hits: see docs/FSMs.md "Gameplay assumptions").
 *   Thrown when the ball is Spinning AND the thrower is beyond THROW_RANGE_MM; Unheld when nobody is
 *   in range or the ball is not Held for a while. Possession changes after THROWER_SWITCH_MS. */
static void run_ready(void)
{
  int n = range_once();
  if (n < 0)
  {
    return;
  }

  int before = ready.thrower;
  enum accel_level level = impact_level();
  switch (bl_ready_update(&ready, k_uptime_get(), ranges, n, level == ACCEL_HELD,
                          level == ACCEL_SPINNING))
  {
  case READY_THROWN:
    LOG_CAT(LOG_STATE, "Thrower slot %d beyond %d mm and the ball is Spinning (for %u ms): thrown",
            ready.thrower, THROW_RANGE_MM, impact_level_ms());
    enter_state(STATE_THROWN);
    return;
  case READY_ORPHAN:
    LOG_CAT(LOG_STATE, "Ready: no alive pouch within %d mm for %d ms (carried away?) -> Unheld",
            THROW_RANGE_MM, READY_ORPHAN_MS);
    enter_state(STATE_UNHELD);
    return;
  case READY_NOT_HELD:
    LOG_CAT(LOG_STATE, "Ready: the ball was not Held for %d ms (put down or dropped) -> Unheld",
            READY_NOT_HELD_MS);
    enter_state(STATE_UNHELD);
    return;
  case READY_STAY:
    break;
  }
  if (ready.thrower != before)
  {
    LOG_CAT(LOG_STATE, "Ready: possession changes, thrower slot %d -> %d", before, ready.thrower);
  }
}

/* Thrown
 *   In flight. Ranging only every THROWN_RECHECK_MS, otherwise the radio is idle.
 *   A Hit event, or the accelerometer going silent (it often stalls after a hard shock) -> Classify().
 *   No hit for THROW_TIMEOUT_MS, or the ball lies at Rest for THROWN_REST_MS -> Disarm().
 *   The same alive pouch the closest within THROW_RANGE_MM for THROWN_RETURN_MS -> Ready with that pouch
 *   as thrower: the throw was cancelled, or the ball ended up nearer to somebody else (the only way
 *   possession changes in flight: there are no catches or passes). */
static void run_thrown(void)
{
  struct impact_event ev;
  if (impact_get(&ev))
  {
    if (ev.stall)
    {
      LOG_CAT(LOG_STATE, "Thrown: accelerometer stalled (hard shock?) -> Classify");
    }
    else
    {
      LOG_CAT(LOG_STATE, "Thrown: Hit %d mg -> Classify", ev.mg);
    }
    classify();
    return;
  }

  int64_t now = k_uptime_get();
  switch (bl_thrown_tick(&thrown, now, impact_level() == ACCEL_REST))
  {
  case THROWN_TIMEOUT:
    LOG_CAT(LOG_STATE, "Thrown: no hit for %d ms -> Disarm", THROW_TIMEOUT_MS);
    disarm();
    return;
  case THROWN_REST:
    LOG_CAT(LOG_STATE, "Thrown: the ball lies at Rest -> Disarm");
    disarm();
    return;
  case THROWN_STAY:
    break;
  }
  if (now < thrown_next_recheck_ms)
  {
    return;
  }
  thrown_next_recheck_ms = now + THROWN_RECHECK_MS;

  int n = range_once(); /* logged, so the ball's position during a flight can be read from the log */
  if (n < 0)
  {
    return;
  }

  int slot;
  int32_t mm;
  if (bl_thrown_round(&thrown, now, ranges, n, &slot, &mm))
  {
    LOG_CAT(LOG_STATE, "Thrown: slot %d within %d mm for %d ms (%d mm) -> Ready%s", slot,
            THROW_RANGE_MM, THROWN_RETURN_MS, mm, slot == ready.thrower ? "" : " (possession changes)");
    enter_ready(slot, mm);
  }
}

/* Hit
 *   Tell the pouch (HIT carries the lives it has left) and wait for its acknowledgment; give up after
 *   HIT_ACK_TIMEOUT_MS so a switched-off pouch cannot lock the ball. The ball's table already has the
 *   new lives either way; a pouch that missed the HIT learns them from the next REMEMBER. */
static void run_hit(void)
{
  if (uwb_ball_send_hit(hit_slot) == 0)
  {
    LOG_CAT(LOG_STATE, "HIT_ACK slot %d hit #%u", hit_slot, hit_count);
    disarm();
  }
  else if (k_uptime_get() - state_entered_ms >= HIT_ACK_TIMEOUT_MS)
  {
    LOG_CAT(LOG_STATE, "HIT_UNACKED slot %d hit #%u", hit_slot, hit_count);
    disarm();
  }
}

/* Reset (all four LEDs blink together, slowly, for the whole window)
 *   The join window. For the whole RESET_WINDOW_MS (3 s), not only until every pouch has acknowledged:
 *   the ball broadcasts RESET until each pouch in its table has answered (in its own timeslot), then
 *   keeps repeating it more slowly for late pouches, and keeps listening for JOINs from pouches
 *   without a slot. Afterwards pouches that never answered are
 *   deleted (everybody's lives were just reset, so nobody loses progress by being forgotten).
 *   Next: Roll Call if the table is empty or somebody new joined (gives several late players time to
 *   join; a short press or the timeout ends it), else straight back into the game with Disarm(). */
static void run_reset(void)
{
  if (k_uptime_get() - state_entered_ms < RESET_WINDOW_MS)
  {
    uwb_ball_reset_step();
    return;
  }
  struct reset_summary sum = uwb_ball_reset_finish();
  LOG_CAT(LOG_STATE, "Reset: %d joined, %d deleted (no answer), %d pouch(es) in the game", sum.joined,
          sum.deleted, sum.remaining);
  if (sum.remaining == 0 || sum.joined > 0)
  {
    enter_state(STATE_ROLL_CALL);
  }
  else
  {
    disarm();
  }
}

static uint32_t log_dropped(void)
{
#ifdef BLE_DEBUG
  return ble_log_dropped();
#else
  return 0;
#endif
}

/* One status line every STATUS_LOG_PERIOD_MS in every state: lets you check the
 * accelerometer (peak |a| since the last line; ~1000 mg when sitting still) and the
 * connection without doing anything. */
static void log_status(void)
{
  static int64_t next_ms;
  int64_t now = k_uptime_get();

  if (!LOG_STATUS || now < next_ms)
  {
    return;
  }
  next_ms = now + STATUS_LOG_PERIOD_MS;
  /* log_dropped = lines the Bluetooth log lost (full queue / link): gaps in the log are explained.
   * accel_errors stays the last field: tools/ball_monitor.py colors the line by its value. */
  LOG_CAT(LOG_STATUS,
          "STATUS %lld state=%s pouches=%d peak=%d mg min=%d mg level=%s level_ms=%u resets=%u "
          "log_dropped=%u accel_errors=%u",
          (long long)now, state_name(state), uwb_ball_pouch_count(), impact_take_peak(),
          impact_take_min(), accel_level_name(impact_level()), impact_level_ms(), impact_resets(),
          log_dropped(), impact_errors());
}

/* Forward the next accelerometer log line (levels, hits, bursts, features, raw dumps), one per pass,
 * so printing a long burst never stalls ranging. Only the enabled LOG_ACCEL_* categories produce any. */
static void log_accel(void)
{
  char line[160];

  if (impact_log_line(line, sizeof(line)))
  {
    dodgit_log("%s", line);
  }
}

/* Hit events matter in Thrown only; everywhere else they are drained so old ones never pile up in the
 * queue (and a real hit in Thrown is never dropped because the queue is full of stale ones). */
static void drain_impact_events(void)
{
  struct impact_event ev;

  while (impact_get(&ev))
  {
  }
}

int main(void)
{
  printk("\n===== dodgit BALL =====\n");

  if (!hw_init())
  {
    return 0;
  }

#ifdef BLE_DEBUG
  int ble_rc = ble_log_init(); /* failure is not fatal: the ball just logs to the console */
  if (ble_rc)
  {
    printk("Bluetooth debug log unavailable (%d)\n", ble_rc);
  }
#endif

  int rc = uwb_init();
  if (rc)
  {
    self_test_fail_halt("UWB radio", rc);
  }
  rc = impact_init();
  if (rc)
  {
    self_test_fail_halt("accelerometer", rc);
  }
  self_test_pass_blink();

  state = STATE_ROLL_CALL;
  LOG_CAT(LOG_STATE, "Roll Call: waiting for pouches (SW0 short press or %d s after the last one starts the game)",
          ROLLCALL_TIMEOUT_MS / 1000);
  enter_state(STATE_ROLL_CALL);

  while (1)
  {
    enum btn_event ev = button_poll();

    if (ev == BTN_HOLD && state != STATE_ROLL_CALL)
    {
      LOG_CAT(LOG_STATE, "Button: hold");
      enter_state(STATE_RESET);
    }
    else if (ev == BTN_SHORT && state == STATE_ROLL_CALL)
    {
      if (uwb_ball_pouch_count() > 0)
      {
        LOG_CAT(LOG_STATE, "Button: short press, ending Roll Call");
        disarm();
      }
      else
      {
        LOG_CAT(LOG_STATE, "Button: short press ignored, no pouch has registered yet");
        flash_mask = LED_BALL_FLASH_IGNORED;
        flash_until_ms = k_uptime_get() + LED_BALL_FLASH_MS;
      }
    }
    else if (ev == BTN_SHORT && LOG_ACCEL_RAW)
    {
      impact_raw_capture(); /* development builds only: dump the last seconds of raw samples */
    }

    if (state != STATE_THROWN)
    {
      drain_impact_events();
    }
    switch (state)
    {
    case STATE_OFF: break; /* only before the first transition */
    case STATE_ROLL_CALL: run_roll_call(); break;
    case STATE_UNHELD: run_unheld(); break;
    case STATE_READY: run_ready(); break;
    case STATE_THROWN: run_thrown(); break;
    case STATE_HIT: run_hit(); break;
    case STATE_RESET: run_reset(); break;
    }

    uwb_ball_remember_tick(); /* between exchanges, in every state: keeps the pouches' slot and lives in sync */
    leds_update(k_uptime_get());
    log_accel();
    log_status();
    /* Ready ranges on every pass and reacts to a throw within a few rounds: do not add the full idle
     * sleep to the ~20 ms a round already takes. */
    k_msleep(state == STATE_READY ? 1 : LOOP_PERIOD_MS);
  }
  return 0;
}
