/*
 * dodgit - Ball firmware (DWM3001CDK)
 *
 * Startup ("Off" state in docs/FSMs.md):
 *   1. led3 (blue) is turned ON and stays on: it marks this board as the ball.
 *   2. Self-test: DW3110 UWB radio probe + accelerometer init.
 *      PASS: led0 (green) blinks 3 times, then the state machine starts in Roll Call.
 *      FAIL: led1 + led2 (red) blink forever (reason printed on the console).
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
 * The state number is shown in binary on led2 led1 led0 (led0 = 1s bit):
 *   Roll Call=0, Disarmed=1, Ready=2, Armed=3, Classify=4, Inform=5.
 * Reset blinks led0..led2 together 3 times slowly. In Roll Call led0 also flashes
 * once for every newly registered pouch.
 * There are no LED strips yet; the strip color of every state is noted in the comments.
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
// #define BLE_DEBUG

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/random/random.h>
#include "settings.h"
#include "uwb.h"
#include "impact.h"
#ifdef BLE_DEBUG
#include "ble_log.h"
#endif

#define LOOP_PERIOD_MS 10
#define HOLD_TIME_MS 3000
#define DEBOUNCE_SAMPLES 3 /* 3 consecutive loop passes with the same level */

#define RESET_BLINKS 3
#define RESET_BLINK_ON_MS 500
#define RESET_BLINK_OFF_MS 500

#define REGISTRATION_FLASH_MS 150
#define RANGE_LOG_PERIOD_MS 200
#define STATUS_LOG_PERIOD_MS 250

/* LOG(...) = printf-style log line (newline added): console always, Bluetooth too with BLE_DEBUG. */
#ifdef BLE_DEBUG
#define LOG(...) ble_log(__VA_ARGS__)
#else
#define LOG(fmt, ...) printk(fmt "\n", ##__VA_ARGS__)
#endif

/* ---------------------------------------------------------------------------
 * LEDs and button (aliases from the decawave_dwm3001cdk board DTS)
 * ------------------------------------------------------------------------- */
static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios), /* D9  green - 1s bit */
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios), /* D12 red   - 2s bit */
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios), /* D11 red   - 4s bit */
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios), /* D10 blue  - identity */
};
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static void led_set(int i, bool on)
{
  gpio_pin_set_dt(&leds[i], on);
}

/* Show the low 3 bits of value on led2 led1 led0. */
static void status_leds_show(uint8_t value)
{
  for (int i = 0; i < 3; i++)
  {
    led_set(i, (value >> i) & 1);
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
    led_set(0, true);
    k_msleep(100);
    led_set(0, false);
    k_msleep(150);
  }
}

static void self_test_fail_halt(const char *what, int rc)
{
  printk("Halting: %s failed (%d)\n", what, rc);
  while (1)
  {
    led_set(1, true);
    led_set(2, true);
    k_msleep(250);
    led_set(1, false);
    led_set(2, false);
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
  STATE_RESET = 6, /* not shown in binary; has its own blink pattern */
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
static uint16_t hit_seq;          /* number of the current hit; the pouch ignores repeats */
static int missed_rounds;         /* Ready: consecutive rounds the thrower did not answer */
static int64_t last_checkin_ms;   /* Roll Call: when the last NEW pouch registered */
static int64_t flash_until_ms;    /* Roll Call: led0 flash end */
static int64_t last_log_ms;       /* throttle for the range log */
static uint8_t reset_acked;       /* Reset: slots that acknowledged so far */
static int64_t reset_next_send_ms;

static struct pouch_range ranges[MAX_POUCHES];

static void enter_state(enum ball_state next)
{
  LOG("State: %s -> %s", state_name(state), state_name(next));
  state = next;
  state_entered_ms = k_uptime_get();

  switch (next)
  {
  case STATE_ROLL_CALL:
    status_leds_show(0);
    uwb_ball_rollcall_start();
    break;
  case STATE_ARMED:
    uwb_ball_idle();       /* Armed: no ranging at all */
    impact_flush();        /* forget bumps from before the throw */
    status_leds_show(next);
    break;
  case STATE_RESET:
    reset_acked = 0;
    reset_next_send_ms = 0;
    status_leds_show(0x7); /* first blink starts immediately */
    break;
  default:
    status_leds_show(next);
    break;
  }
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
    LOG("RANGE %lld none answered", (long long)now);
  }
  for (int i = 0; i < n; i++)
  {
    LOG("RANGE %lld slot=%u uid=0x%08x distance=%d mm lives=%u", (long long)now,
           ranges[i].slot, ranges[i].uid, ranges[i].mm, ranges[i].lives);
  }
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
  case RC_NEW:
    LOG("ROLLCALL new pouch uid=0x%08x -> slot %d (%d registered)", uid, slot,
           uwb_ball_pouch_count());
    last_checkin_ms = now; /* only NEW pouches restart the timeout */
    flash_until_ms = now + REGISTRATION_FLASH_MS;
    break;
  case RC_REPEAT:
    LOG("ROLLCALL uid=0x%08x asked again, same slot %d", uid, slot);
    break;
  case RC_FULL:
    LOG("ROLLCALL uid=0x%08x ignored: all %d timeslots taken", uid, MAX_POUCHES);
    break;
  case RC_NONE:
    break;
  }
  led_set(0, now < flash_until_ms);

  if (uwb_ball_pouch_count() > 0 && now - last_checkin_ms >= ROLLCALL_TIMEOUT_MS)
  {
    LOG("ROLLCALL timeout, %d pouch(es) registered", uwb_ball_pouch_count());
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

  int c = closest(n, true);
  if (c >= 0 && ranges[c].mm <= PICKUP_DISTANCE_MM)
  {
    thrower_slot = ranges[c].slot;
    missed_rounds = 0;
    LOG("Pickup by slot %d (%d mm)", thrower_slot, ranges[c].mm);
    enter_state(STATE_READY);
  }
}

/* Ready (Light Strip: Yellow)
 *   Constantly ranging. Leaving the thrower's proximity means the ball was thrown.
 *   (Accelerometer-based throw detection is intentionally not used yet.) */
static void run_ready(void)
{
  int n = uwb_ball_range_round(ranges, MAX_POUCHES);
  if (n < 0)
  {
    return;
  }
  log_ranges(n);

  bool near = false;
  for (int i = 0; i < n; i++)
  {
    if (ranges[i].slot == thrower_slot)
    {
      near = ranges[i].mm <= LEAVE_DISTANCE_MM;
      if (!near)
      {
        LOG("Thrower slot %d is %d mm away: thrown", thrower_slot, ranges[i].mm);
        enter_state(STATE_ARMED);
        return;
      }
      missed_rounds = 0;
      return;
    }
  }
  if (++missed_rounds >= LEAVE_MISS_ROUNDS) /* out of range counts as left */
  {
    LOG("Thrower slot %d out of range: thrown", thrower_slot);
    enter_state(STATE_ARMED);
  }
}

/* Armed (Light Strip: Red)
 *   No ranging. Watch the accelerometer; an impact moves on to Classify. */
static void run_armed(void)
{
  struct impact_event ev;
  if (impact_get(&ev))
  {
    LOG("IMPACT %lld ms, %d mg", (long long)ev.time, ev.mg);
    enter_state(STATE_CLASSIFY);
  }
}

/* With only one pouch registered the thrower is always the closest pouch, so the normal
 * rule "thrower hit itself -> back to Ready" would make a hit impossible to test.
 * In that case the thrower may be hit (SELF_HIT_IF_SINGLE_POUCH in settings.h). */
static bool single_pouch_test(void)
{
  return SELF_HIT_IF_SINGLE_POUCH && uwb_ball_pouch_count() == 1;
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
    LOG("Classify: no pouch within %d mm -> miss", HIT_DISTANCE_MM);
    enter_state(STATE_DISARMED);
  }
  else if (ranges[c].slot == thrower_slot && !single_pouch_test())
  {
    LOG("Classify: closest is the thrower (slot %d) -> back to Ready", thrower_slot);
    missed_rounds = 0;
    enter_state(STATE_READY);
  }
  else
  {
    if (ranges[c].slot == thrower_slot)
    {
      LOG("Classify: thrower is the only pouch -> treating it as hit (single-pouch test)");
    }
    hit_slot = ranges[c].slot;
    hit_seq++;
    LOG("Classify: HIT on slot %d (%d mm), hit #%u", hit_slot, ranges[c].mm, hit_seq);
    enter_state(STATE_INFORM);
  }
}

/* Inform (Light Strip: Magenta)
 *   Tell the hit pouch over UWB and wait for its acknowledgment; give up after
 *   INFORM_TIMEOUT_MS so a switched-off pouch cannot lock the ball. */
static void run_inform(void)
{
  if (uwb_ball_send_hit(hit_slot, hit_seq) == 0)
  {
    LOG("HIT_ACK slot %d hit #%u", hit_slot, hit_seq);
    enter_state(STATE_DISARMED);
  }
  else if (k_uptime_get() - state_entered_ms >= INFORM_TIMEOUT_MS)
  {
    LOG("HIT_UNACKED slot %d hit #%u", hit_slot, hit_seq);
    enter_state(STATE_DISARMED);
  }
}

/* Reset (Light Strip: Cyan, blinks 3 times slowly)
 *   Broadcast RESET every RESET_RETRY_MS while blinking, collecting acknowledgments
 *   (each pouch answers in its own timeslot). After the blinks go to Disarmed. We do not
 *   wait beyond the blinks for pouches that stay silent (e.g. switched off). */
static void run_reset(void)
{
  int64_t now = k_uptime_get();
  int64_t elapsed = now - state_entered_ms;
  int64_t period = RESET_BLINK_ON_MS + RESET_BLINK_OFF_MS;

  if (elapsed >= RESET_BLINKS * period)
  {
    LOG("Reset: %d of %d pouch(es) acknowledged", __builtin_popcount(reset_acked),
           uwb_ball_pouch_count());
    enter_state(STATE_DISARMED);
    return;
  }
  status_leds_show(elapsed % period < RESET_BLINK_ON_MS ? 0x7 : 0x0);

  if (now >= reset_next_send_ms && reset_acked != uwb_ball_slot_mask())
  {
    reset_acked |= uwb_ball_send_reset();
    reset_next_send_ms = now + RESET_RETRY_MS;
  }
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
  LOG("STATUS %lld state=%s pouches=%d peak=%d mg accel_errors=%u", (long long)now,
     state_name(state), uwb_ball_pouch_count(), impact_take_peak(), impact_errors());
}

int main(void)
{
  printk("\n===== dodgit BALL =====\n");

  if (!hw_init())
  {
    return 0;
  }

  /* Identity: blue LED steady on for the ball, from power-up onwards. */
  led_set(3, true);

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

  hit_seq = sys_rand32_get() & 0xFFFF; /* random start so a rebooted ball's hit numbers differ */
  state = STATE_ROLL_CALL;
  LOG("Roll Call: waiting for pouches (SW0 short press or %d s after the last one starts the game)",
         ROLLCALL_TIMEOUT_MS / 1000);
  enter_state(STATE_ROLL_CALL);

  while (1)
  {
    enum btn_event ev = button_poll();

    if (ev == BTN_HOLD && state != STATE_ROLL_CALL)
    {
      LOG("Button: hold");
      enter_state(STATE_RESET);
    }
    else if (ev == BTN_SHORT && state == STATE_ROLL_CALL)
    {
      if (uwb_ball_pouch_count() > 0)
      {
        LOG("Button: short press, ending Roll Call");
        enter_state(STATE_DISARMED);
      }
      else
      {
        LOG("Button: short press ignored, no pouch has registered yet");
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

    log_status();
    k_msleep(LOOP_PERIOD_MS);
  }
  return 0;
}
