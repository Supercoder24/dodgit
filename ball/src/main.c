/*
 * dodgit - Ball firmware (DWM3001CDK)
 *
 * Startup ("Off" state in docs/FSMs.md):
 *   Self-test: DW3110 UWB radio probe + accelerometer init.
 *     PASS: led0 (green) blinks 3 times, then the state machine starts in Roll Call.
 *     FAIL: led1 + led2 (red) blink forever (reason printed on the console).
 *
 * State machine (docs/FSMs.md "Ball Main FSM"):
 *   Roll Call -> Disarmed -> Ready -> Armed -> Classify -> (Ready | Disarmed | Inform)
 *   Inform -> Disarmed;   Reset (button held >= 3 s, any state except Roll Call) -> Disarmed
 *
 * The user button SW0 simulates the future external button:
 *   short press (< 3 s) : leave Roll Call (only once at least one pouch has registered)
 *   hold (>= 3 s)       : Reset - tell all pouches to restore their lives
 * (SW0 is unrelated to the board's hardware RESET button, which reboots the MCU.)
 *
 * Ready also ends when the ball lies still for READY_STILL_TIMEOUT_MS (dropped), Armed when the
 * thrower comes back close (wind-up cancelled) or after ARMED_TIMEOUT_MS, and an impact only counts
 * if free fall came just before it (see impact.c).
 *
 * Reset (button held >= 3 s) is also the join window: for its whole length the ball listens for
 * JOINs (a pouch switched on late joins without turning anything else off), and pouches that never
 * answer it are deleted. If anybody joined, or nobody is left, it continues in Roll Call.
 *
 * What the LEDs show is data in shared/led_patterns.h (one color at a time, steady vs. blinking);
 * the pattern of each state is chosen in leds_update() below.
 *
 * Everything the ball does is also printed on the console (J-Link USB serial, 115200 baud).
 * With BLE_DEBUG (below) the same log is also sent over Bluetooth: tools/ball_monitor.py.
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
#include <zephyr/drivers/gpio.h>
#include "settings.h"
#include "log.h"
#include "led_patterns.h"
#include "uwb.h"
#include "impact.h"
#ifdef BLE_DEBUG
#include "ble_log.h"
#endif

#define LOOP_PERIOD_MS 10
#define HOLD_TIME_MS 3000
#define DEBOUNCE_SAMPLES 3 /* 3 consecutive loop passes with the same level */

#define RESET_BLINKS 3 /* the Reset state lasts RESET_BLINKS x (PAT_BALL_RESET on + off) = 3 s */

/* Log lines: LOG_CAT(<category flag from settings.h>, ...) (shared/log.h). dodgit_log() prints on the
 * console and, with BLE_DEBUG and a laptop connected, sends the line over Bluetooth too. */

/* ---------------------------------------------------------------------------
 * LEDs and button (aliases from the decawave_dwm3001cdk board DTS)
 * ------------------------------------------------------------------------- */
static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios), /* D9  green */
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios), /* D12 red   */
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios), /* D11 red   */
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios), /* D10 blue  */
};
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static uint8_t leds_shown = LED_NONE;

/* Light exactly the LEDs in mask (bit i = leds[i]); only touches the GPIOs when it changes. */
static void leds_show(uint8_t mask)
{
  if (mask == leds_shown)
  {
    return;
  }
  leds_shown = mask;
  for (int i = 0; i < ARRAY_SIZE(leds); i++)
  {
    gpio_pin_set_dt(&leds[i], (mask >> i) & 1);
  }
}

static bool hw_init(void)
{
  for (int i = 0; i < ARRAY_SIZE(leds); i++)
  {
    if (!gpio_is_ready_dt(&leds[i]) || gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) < 0)
    {
      printk("LED %d init failed\n", i);
      return false;
    }
  }
  if (!gpio_is_ready_dt(&button) || gpio_pin_configure_dt(&button, GPIO_INPUT) < 0)
  {
    printk("Button init failed\n");
    return false;
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * Button: polled every loop pass with a small debounce.
 *   BTN_SHORT - released before HOLD_TIME_MS
 *   BTN_HOLD  - fired once when HOLD_TIME_MS is reached while still pressed;
 *               the following release is swallowed (no BTN_SHORT).
 * ------------------------------------------------------------------------- */
enum btn_event
{
  BTN_NONE,
  BTN_SHORT,
  BTN_HOLD,
};

static enum btn_event button_poll(void)
{
  static bool stable;     /* debounced level, true = pressed */
  static int same_count;  /* consecutive raw samples differing from stable */
  static int64_t press_start;
  static bool hold_fired;

  bool raw = gpio_pin_get_dt(&button) > 0;
  int64_t now = k_uptime_get();

  if (raw != stable)
  {
    if (++same_count >= DEBOUNCE_SAMPLES)
    {
      stable = raw;
      same_count = 0;
      if (stable)
      {
        press_start = now;
        hold_fired = false;
      }
      else if (!hold_fired)
      {
        return BTN_SHORT;
      }
    }
  }
  else
  {
    same_count = 0;
  }

  if (stable && !hold_fired && now - press_start >= HOLD_TIME_MS)
  {
    hold_fired = true;
    return BTN_HOLD;
  }
  return BTN_NONE;
}

/* ---------------------------------------------------------------------------
 * Startup self-test
 * ------------------------------------------------------------------------- */
static void self_test_pass_blink(void)
{
  for (int i = 0; i < 3; i++)
  {
    leds_show(LED_GREEN);
    k_msleep(100);
    leds_show(LED_NONE);
    k_msleep(150);
  }
}

static void self_test_fail_halt(const char *what, int rc)
{
  printk("Halting: %s failed (%d)\n", what, rc);
  while (1)
  {
    leds_show(LED_RED);
    k_msleep(250);
    leds_show(LED_NONE);
    k_msleep(250);
  }
}

/* ---------------------------------------------------------------------------
 * Ball state machine (docs/FSMs.md)
 * ------------------------------------------------------------------------- */
enum ball_state
{
  STATE_ROLL_CALL = 0,
  STATE_DISARMED = 1,
  STATE_READY = 2,
  STATE_ARMED = 3,
  STATE_CLASSIFY = 4,
  STATE_INFORM = 5,
  STATE_RESET = 6,
};

static const char *state_name(enum ball_state s)
{
  switch (s)
  {
  case STATE_ROLL_CALL: return "Roll Call";
  case STATE_DISARMED: return "Disarmed";
  case STATE_READY: return "Ready";
  case STATE_ARMED: return "Armed";
  case STATE_CLASSIFY: return "Classify";
  case STATE_INFORM: return "Inform";
  case STATE_RESET: return "Reset";
  }
  return "?";
}

static enum ball_state state;
static int64_t state_entered_ms;

/* Data carried between states */
static int thrower_slot = -1;     /* pouch that was holding the ball (set on Disarmed -> Ready) */
static int hit_slot = -1;         /* pouch that was hit (set in Classify, used in Inform) */
static uint16_t hit_count;        /* hits since boot (log only) */
static int confirm_rounds;        /* Disarmed/Ready: consecutive rounds the transition condition held */
static int last_thrower_mm;       /* Ready: last valid range to the thrower (0 = assume touching) */
static int pickup_candidate = -1; /* Disarmed: slot being confirmed as the picker-up */
static int64_t last_checkin_ms;   /* Roll Call: when the last NEW pouch registered (or Roll Call began) */
static int64_t flash_until_ms;    /* Roll Call: end of the green / red flash */
static uint8_t flash_mask;        /* Roll Call: LED of that flash */
static int64_t last_log_ms;       /* throttle for the range log */
static int64_t armed_next_recheck_ms; /* Armed: when the next thrower re-check is due */

static struct pouch_range ranges[MAX_POUCHES];

static void enter_state(enum ball_state next)
{
  /* up= is the ball's uptime in ms, the same clock as STATUS/RANGE/IMPACT/BURST: the laptop's
   * receive times are bunched by Bluetooth batching and cannot be used to order events. */
  LOG_CAT(LOG_STATE, "State: %s -> %s (up=%lld)", state_name(state), state_name(next),
          (long long)k_uptime_get());
  state = next;
  state_entered_ms = k_uptime_get();
  confirm_rounds = 0; /* every state starts its own confirmation count */
  pickup_candidate = -1;
  last_thrower_mm = 0;
  if (next == STATE_READY)
  {
    impact_freefall_clear(); /* only free fall after this point counts as a throw */
    impact_flush();          /* forget bumps from before */
  }

  switch (next)
  {
  case STATE_ROLL_CALL:
    last_checkin_ms = state_entered_ms; /* re-entered after a Reset: the timeout starts over */
    flash_until_ms = 0;
    uwb_ball_rollcall_start();
    break;
  case STATE_ARMED:
    uwb_ball_idle();       /* radio off; run_armed() re-ranges only every ARMED_RECHECK_MS */
    armed_next_recheck_ms = state_entered_ms + ARMED_RECHECK_MS;
    /* No flush here: impacts are only queued after free fall, so one that happened between the
     * release and this transition is a real hit and must not be lost. */
    break;
  case STATE_RESET:
    uwb_ball_reset_start();
    break;
  default:
    break;
  }
}

/* The LEDs of the current state (docs/FSMs.md); called once per loop pass. */
static void leds_update(int64_t now)
{
  const struct led_pattern *p;
  int64_t t = now; /* free-running pattern */

  switch (state)
  {
  case STATE_ROLL_CALL:
    if (now < flash_until_ms)
    {
      leds_show(flash_mask); /* a flash REPLACES the steady blue: one color at a time */
      return;
    }
    p = &PAT_BALL_ROLL_CALL;
    break;
  case STATE_DISARMED: p = &PAT_BALL_UNHELD; break;
  case STATE_READY: p = &PAT_BALL_READY; break;
  case STATE_ARMED:
  case STATE_CLASSIFY: p = &PAT_BALL_THROWN; break;
  case STATE_INFORM: p = &PAT_BALL_HIT; break;
  case STATE_RESET:
    p = &PAT_BALL_RESET;
    t = now - state_entered_ms; /* the first blink starts immediately */
    break;
  default: p = &PAT_BALL_UNHELD; break;
  }
  leds_show(led_pattern_mask(p, t));
}

/* Index of the closest pouch in ranges[0..n), or -1. With need_lives, skip dead pouches. */
static int closest(int n, bool need_lives)
{
  int best = -1;
  for (int i = 0; i < n; i++)
  {
    if (need_lives && ranges[i].lives == 0)
    {
      continue;
    }
    if (best < 0 || ranges[i].mm < ranges[best].mm)
    {
      best = i;
    }
  }
  return best;
}

/* Print every pouch of the latest round, at most RANGE_LOG_PERIOD_MS apart. */
static void log_ranges(int n)
{
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

/* With only one pouch registered the thrower is always the closest pouch, so the normal
 * rule "thrower hit itself -> back to Ready" would make a hit impossible to test.
 * In that case the thrower may be hit (SELF_HIT_IF_SINGLE_POUCH in settings.h). */
static bool single_pouch_test(void)
{
  return SELF_HIT_IF_SINGLE_POUCH && uwb_ball_pouch_count() == 1;
}

/* Roll Call (Light Strip: White)
 *   Pouches announce themselves; each new one gets the next free timeslot.
 *   Leave when SW0 is pressed (handled in main) or ROLLCALL_TIMEOUT_MS after the last
 *   NEW registration. Never leave before at least one pouch has registered. */
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
    enter_state(STATE_DISARMED);
  }
}

/* Disarmed (Light Strip: Green)
 *   Constantly ranging, accelerometer not used. A pouch with lives left that is
 *   within PICKUP_DISTANCE_MM means a player picked up the ball. */
static void run_disarmed(void)
{
  int n = uwb_ball_range_round(ranges, MAX_POUCHES);
  if (n < 0)
  {
    return;
  }
  log_ranges(n);

  /* Close to a pouch is not enough: a ball lying on the floor next to it is not "picked up".
   * The ball has to be moving (impact_moved_recent is true if the sensor is down). */
  int c = closest(n, true);
  if (c < 0 || ranges[c].mm > PICKUP_DISTANCE_MM || !impact_moved_recent(PICKUP_MOTION_MS))
  {
    confirm_rounds = 0; /* a single bad round restarts the count */
    pickup_candidate = -1;
    return;
  }
  if (ranges[c].slot != pickup_candidate)
  {
    pickup_candidate = ranges[c].slot;
    confirm_rounds = 0;
  }
  if (++confirm_rounds >= TRANSITION_CONFIRM_ROUNDS)
  {
    thrower_slot = pickup_candidate;
    confirm_rounds = 0;
    LOG_CAT(LOG_STATE, "Pickup by slot %d (%d mm)", thrower_slot, ranges[c].mm);
    enter_state(STATE_READY);
    last_thrower_mm = ranges[c].mm; /* after enter_state, which clears it */
  }
}

/* Ready (Light Strip: Yellow)
 *   Constantly ranging. The ball counts as thrown only when the thrower is out of range
 *   AND the accelerometer has seen free fall (|a| well below 1 g) recently.
 *   Other exits: an impact after free fall (hit before Armed was reached), and lying still
 *   for READY_STILL_TIMEOUT_MS (dropped / put down). */
static void run_ready(void)
{
  struct impact_event ev;
  while (impact_get(&ev))
  {
    /* Only impacts that follow free fall get here. With a single pouch (bench test) a drop
     * would be classified as a hit on the thrower, so those are ignored in Ready. */
    if (!single_pouch_test())
    {
      LOG_CAT(LOG_STATE, "IMPACT %lld ms, %d mg, ffage=%d ms min150=%d mg (still Ready: hit right after release)",
              (long long)ev.time, ev.mg, ev.ff_age_ms, ev.min_prev_mg);
      enter_state(STATE_CLASSIFY);
      return;
    }
  }
  if (impact_still_ms() >= READY_STILL_TIMEOUT_MS)
  {
    LOG_CAT(LOG_STATE, "Ready: ball lay still for %d ms (dropped?) -> Disarmed", READY_STILL_TIMEOUT_MS);
    enter_state(STATE_DISARMED);
    return;
  }

  int n = uwb_ball_range_round(ranges, MAX_POUCHES);
  if (n < 0)
  {
    return;
  }
  log_ranges(n);

  /* A round counts as "out" if the thrower is too far, or silent after a last valid range
   * beyond TOO_CLOSE_MM. Silence right after a very close range is a ranging failure from
   * the ball touching the pouch, so the ball is still held. */
  bool out = last_thrower_mm > TOO_CLOSE_MM;
  for (int i = 0; i < n; i++)
  {
    if (ranges[i].slot == thrower_slot)
    {
      last_thrower_mm = ranges[i].mm;
      out = ranges[i].mm > LEAVE_DISTANCE_MM;
      break;
    }
  }
  if (!out)
  {
    confirm_rounds = 0; /* a single in-range round restarts the count */
    return;
  }
  if (confirm_rounds < ARM_CONFIRM_ROUNDS)
  {
    confirm_rounds++;
  }
  /* Out of range alone is not enough: the accelerometer must also have seen free fall. */
  if (confirm_rounds >= ARM_CONFIRM_ROUNDS && impact_freefall_recent(FREEFALL_HOLD_MS))
  {
    uint32_t ff = impact_freefall_start_ms();
    LOG_CAT(LOG_STATE, "Thrower slot %d out of range for %d round(s) + free fall (first since Ready began %d ms "
            "ago, latest %d ms ago): thrown",
            thrower_slot, confirm_rounds, ff ? (int)(k_uptime_get_32() - ff) : -1,
            impact_freefall_age_ms());
    enter_state(STATE_ARMED);
  }
}

/* Armed (Light Strip: Red)
 *   Watch the accelerometer; an impact (only counted after free fall) moves on to Classify.
 *   Armed must not be a dead end, so also:
 *   - every ARMED_RECHECK_MS one ranging round: the thrower back within REARM_DISTANCE_MM for
 *     REARM_CONFIRM_ROUNDS in a row means the throw was cancelled (e.g. wind-up) -> Ready;
 *   - after ARMED_TIMEOUT_MS without an impact -> Disarmed. */
static void run_armed(void)
{
  struct impact_event ev;
  if (impact_get(&ev))
  {
    LOG_CAT(LOG_STATE, "IMPACT %lld ms, %d mg, ffage=%d ms min150=%d mg", (long long)ev.time, ev.mg,
            ev.ff_age_ms, ev.min_prev_mg);
    enter_state(STATE_CLASSIFY);
    return;
  }

  int64_t now = k_uptime_get();
  if (now - state_entered_ms >= ARMED_TIMEOUT_MS)
  {
    LOG_CAT(LOG_STATE, "Armed: no impact for %d ms -> Disarmed", ARMED_TIMEOUT_MS);
    enter_state(STATE_DISARMED);
    return;
  }
  if (now < armed_next_recheck_ms)
  {
    return;
  }
  armed_next_recheck_ms = now + ARMED_RECHECK_MS;

  int n = uwb_ball_range_round(ranges, MAX_POUCHES);
  if (n < 0)
  {
    return;
  }
  log_ranges(n); /* so the ball's position during a flight can be read from the log */
  int close_mm = -1;
  for (int i = 0; i < n; i++)
  {
    if (ranges[i].slot == thrower_slot && ranges[i].mm <= REARM_DISTANCE_MM)
    {
      close_mm = ranges[i].mm;
    }
  }
  if (close_mm < 0)
  {
    confirm_rounds = 0;
    return;
  }
  if (++confirm_rounds >= REARM_CONFIRM_ROUNDS)
  {
    LOG_CAT(LOG_STATE, "Armed: thrower slot %d is back (%d mm), throw cancelled -> Ready", thrower_slot,
            close_mm);
    enter_state(STATE_READY);
    last_thrower_mm = close_mm; /* after enter_state, which clears it */
  }
}

/* Classify (Light Strip: Blue)
 *   One quick ranging poll (repeated a few times only if nobody answered), pick the
 *   closest pouch and decide what the impact was. */
static void run_classify(void)
{
  int n = 0;
  for (int attempt = 0; attempt < CLASSIFY_ATTEMPTS && n <= 0; attempt++)
  {
    n = uwb_ball_range_round(ranges, MAX_POUCHES);
  }
  last_log_ms = 0;
  log_ranges(n > 0 ? n : 0);

  int c = n > 0 ? closest(n, false) : -1;
  if (c < 0 || ranges[c].mm > HIT_DISTANCE_MM)
  {
    LOG_CAT(LOG_STATE, "Classify: no pouch within %d mm -> miss", HIT_DISTANCE_MM);
    enter_state(STATE_DISARMED);
  }
  else if (ranges[c].slot == thrower_slot && !single_pouch_test())
  {
    LOG_CAT(LOG_STATE, "Classify: closest is the thrower (slot %d) -> back to Ready", thrower_slot);
    confirm_rounds = 0;
    enter_state(STATE_READY);
  }
  else
  {
    if (ranges[c].slot == thrower_slot)
    {
      LOG_CAT(LOG_STATE, "Classify: thrower is the only pouch -> treating it as hit (single-pouch test)");
    }
    hit_slot = ranges[c].slot;
    hit_count++;
    /* The ball owns the lives: count the hit now, whether or not the pouch ever acknowledges. */
    int lives_left = uwb_ball_hit(hit_slot);
    LOG_CAT(LOG_STATE, "Classify: HIT on slot %d (%d mm), hit #%u, lives left %d", hit_slot,
            ranges[c].mm, hit_count, lives_left);
    enter_state(STATE_INFORM);
  }
}

/* Inform (Light Strip: Magenta)
 *   Tell the hit pouch over UWB and wait for its acknowledgment; give up after
 *   INFORM_TIMEOUT_MS so a switched-off pouch cannot lock the ball. */
static void run_inform(void)
{
  if (uwb_ball_send_hit(hit_slot) == 0)
  {
    LOG_CAT(LOG_STATE, "HIT_ACK slot %d hit #%u", hit_slot, hit_count);
    enter_state(STATE_DISARMED);
  }
  else if (k_uptime_get() - state_entered_ms >= INFORM_TIMEOUT_MS)
  {
    /* The ball's table already has the new lives: the pouch learns them from the next REMEMBER. */
    LOG_CAT(LOG_STATE, "HIT_UNACKED slot %d hit #%u", hit_slot, hit_count);
    enter_state(STATE_DISARMED);
  }
}

/* Reset (all four LEDs blink together 3 times, slowly)
 *   The join window. For the whole 3 seconds, not only until every pouch has acknowledged: the ball
 *   broadcasts RESET until each pouch in its table has answered (in its own timeslot) and keeps
 *   listening for JOINs from pouches without a slot. Afterwards pouches that never answered are
 *   deleted (everybody's lives were just reset, so nobody loses progress by being forgotten).
 *   Next: Roll Call if the table is empty or somebody new joined (gives several late players time to
 *   join; a short press or the timeout ends it), else straight back into the game. */
static void run_reset(void)
{
  int64_t elapsed = k_uptime_get() - state_entered_ms;
  int64_t period = PAT_BALL_RESET.on_ms + PAT_BALL_RESET.off_ms;

  if (elapsed < RESET_BLINKS * period)
  {
    uwb_ball_reset_step();
    return;
  }
  struct reset_summary sum = uwb_ball_reset_finish();
  LOG_CAT(LOG_STATE, "Reset: %d joined, %d deleted (no answer), %d pouch(es) in the game", sum.joined,
          sum.deleted, sum.remaining);
  enter_state(sum.remaining == 0 || sum.joined > 0 ? STATE_ROLL_CALL : STATE_DISARMED);
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

  if (now < next_ms)
  {
    return;
  }
  next_ms = now + STATUS_LOG_PERIOD_MS;
  /* log_dropped = lines the Bluetooth log lost (full queue / link): gaps in the log are explained.
   * accel_errors stays the last field: tools/ball_monitor.py colors the line by its value. */
  LOG_CAT(LOG_STATUS,
          "STATUS %lld state=%s pouches=%d peak=%d mg min=%d mg still=%u ms resets=%u rejected=%u "
          "log_dropped=%u accel_errors=%u",
          (long long)now, state_name(state), uwb_ball_pouch_count(), impact_take_peak(),
          impact_take_min(), impact_still_ms(), impact_resets(), impact_rejected(NULL),
          log_dropped(), impact_errors());
}

/* One line whenever the accelerometer was re-initialized or an impact was ignored
 * (no free fall just before it), so both can be checked in the log. */
static void log_accel_events(void)
{
  static uint32_t seen_resets, seen_rejected;
  uint32_t resets = impact_resets();
  int32_t rejected_mg;
  uint32_t rejected = impact_rejected(&rejected_mg);

  if (resets != seen_resets)
  {
    seen_resets = resets;
    LOG_CAT(LOG_ACCEL_CLASS, "ACCEL stalled, sensor re-initialized (reset #%u, %s)", resets,
            impact_healthy() ? "data is back" : "still no data");
  }
  if (rejected != seen_rejected)
  {
    seen_rejected = rejected;
    int32_t ff_age, min_prev;
    impact_rejected_detail(&ff_age, &min_prev);
    LOG_CAT(LOG_ACCEL_CLASS, "IMPACT ignored: %d mg without free fall just before (#%u), ffage=%d ms min150=%d mg",
            rejected_mg, rejected, ff_age, min_prev);
  }
}

/* Raw accelerometer burst around every threshold crossing (see impact.h). One line per loop
 * pass, so a burst of ~10 lines is spread out and never stalls ranging. */
static void log_bursts(void)
{
  char line[160];
  if (impact_burst_line(line, sizeof(line)))
  {
    LOG_CAT(LOG_ACCEL_BURST, "%s", line);
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
  rc = impact_init(IMPACT_MG, IMPACT_COOLDOWN_MS);
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
        enter_state(STATE_DISARMED);
      }
      else
      {
        LOG_CAT(LOG_STATE, "Button: short press ignored, no pouch has registered yet");
        flash_mask = LED_BALL_FLASH_IGNORED;
        flash_until_ms = k_uptime_get() + LED_BALL_FLASH_MS;
      }
    }

    switch (state)
    {
    case STATE_ROLL_CALL: run_roll_call(); break;
    case STATE_DISARMED: run_disarmed(); break;
    case STATE_READY: run_ready(); break;
    case STATE_ARMED: run_armed(); break;
    case STATE_CLASSIFY: run_classify(); break;
    case STATE_INFORM: run_inform(); break;
    case STATE_RESET: run_reset(); break;
    }

    uwb_ball_remember_tick(); /* between exchanges, in every state: keeps the pouches' slot and lives in sync */
    leds_update(k_uptime_get());
    log_accel_events();
    log_bursts();
    log_status();
    /* A ranging round in Ready already takes ~20 ms; do not add the full idle sleep there. */
    k_msleep(state == STATE_READY ? 1 : LOOP_PERIOD_MS);
  }
  return 0;
}
