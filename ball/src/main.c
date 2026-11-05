/*
 * dodgit - Ball firmware (DWM3001CDK)
 *
 * Startup ("Off" state in docs/FSMs.md):
 *   1. led3 (blue) is turned ON and stays on: it marks this board as the ball.
 *   2. Quick DW3110 ID check (dwt_probe + device ID upper half 0xDECA).
 *      PASS: led0 (green) blinks 3 times, then the state machine starts.
 *      FAIL: led1 + led2 (red) blink forever.
 *
 * State machine (docs/FSMs.md "Ball Main FSM"), currently driven only by the
 * user button so it can be tested on a single board without radio or
 * accelerometer:
 *   short press (< 3 s) : Disarmed -> Ready -> Armed -> Classify -> Inform -> Disarmed
 *   hold (>= 3 s)       : any state -> Reset -> Disarmed
 *
 * The state number is shown in binary on led2 led1 led0 (led0 = 1s bit):
 *   Disarmed=1, Ready=2, Armed=3, Classify=4, Inform=5.
 * Reset blinks led0..led2 together 3 times slowly, then goes to Disarmed.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <deca_device_api.h>
#include <deca_probe_interface.h>
#include <dw3000_hw.h>

#define LOOP_PERIOD_MS 10
#define HOLD_TIME_MS 3000
#define DEBOUNCE_SAMPLES 3 /* 3 x 10 ms of stable level */

#define RESET_BLINKS 3
#define RESET_BLINK_ON_MS 500
#define RESET_BLINK_OFF_MS 500

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
 * Button: polled every LOOP_PERIOD_MS with a small debounce.
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
 * Startup self-test: quick DW3110 ID check only
 * ------------------------------------------------------------------------- */
static bool dw3110_id_check(void)
{
  dw3000_hw_init();
  dw3000_hw_reset();

  /* Wait for DW3110 to finish reset */
  k_msleep(2000);

  int32_t rc = dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf);
  uint32_t dev_id = dwt_readdevid();
  bool ok = rc == DWT_SUCCESS && (dev_id & 0xFFFF0000) == 0xDECA0000;

  printk("DW3110 probe rc = %d, device ID = 0x%08X\n", rc, dev_id);
  printk("DW3110 ID check: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

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

static void self_test_fail_halt(void)
{
  printk("Halting: DW3110 not detected\n");
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

static void enter_state(enum ball_state next)
{
  printk("State: %s -> %s\n", state_name(state), state_name(next));
  state = next;
  state_entered_ms = k_uptime_get();

  if (next == STATE_RESET)
  {
    status_leds_show(0x7); /* first blink starts immediately */
  }
  else
  {
    status_leds_show(next);
  }
}

/* Short press stands in for the real transition condition of each state. */
static enum ball_state next_on_short_press(enum ball_state s)
{
  switch (s)
  {
  case STATE_DISARMED: return STATE_READY;
  case STATE_READY: return STATE_ARMED;
  case STATE_ARMED: return STATE_CLASSIFY;
  case STATE_CLASSIFY: return STATE_INFORM;
  case STATE_INFORM: return STATE_DISARMED;
  case STATE_RESET: return STATE_RESET; /* ignored while resetting */
  }
  return s;
}

static void run_disarmed(void)
{
  /* Future behavior (Light Strip green):
   *   range_all_pouches();                        // constant ranging
   *   accelerometer: not used
   *   for each pouch p in ranging results:
   *     if (p.lives > 0 && p.distance < PICKUP_DISTANCE)
   *       thrower_id = p.id;                      // remember who holds the ball
   *       enter_state(STATE_READY);
   */
}

static void run_ready(void)
{
  /* Future behavior (Light Strip yellow):
   *   range_to(thrower_id);                       // constant ranging
   *   accel = read_accelerometer();               // constant monitoring
   *   if (distance_to(thrower_id) > PICKUP_DISTANCE || is_throw(accel))
   *     enter_state(STATE_ARMED);
   *   // Maybe: left proximity without throw acceleration -> STATE_DISARMED
   */
}

static void run_armed(void)
{
  /* Future behavior (Light Strip red):
   *   ranging: off
   *   accel = read_accelerometer();               // monitor closely
   *   if (is_impact(accel))
   *     enter_state(STATE_CLASSIFY);
   */
}

static void run_classify(void)
{
  /* Future behavior (Light Strip blue):
   *   results = single_quick_ranging_poll();
   *   closest = closest_pouch(results);
   *   if (closest == NONE || closest.distance > HIT_DISTANCE)
   *     enter_state(STATE_DISARMED);              // missed everyone
   *   else if (closest.id == thrower_id)
   *     enter_state(STATE_READY);                 // thrower caught it / self-hit
   *   else
   *     hit_id = closest.id;
   *     enter_state(STATE_INFORM);
   */
}

static void run_inform(void)
{
  /* Future behavior (Light Strip magenta):
   *   send_hit_message(hit_id);                   // over UWB radio
   *   if (receive_ack(hit_id, ACK_TIMEOUT))
   *     enter_state(STATE_DISARMED);
   *   // else retry send_hit_message(hit_id)
   */
}

static void run_reset(void)
{
  /* Future behavior (Lights blink cyan 3 times slowly):
   *   broadcast_reset_message();                  // all pouches: lives = LIVES_MAX
   *   wait for ack from at least one pouch (or all known pouches, or a fixed
   *   time - still to be decided, see docs/FSMs.md), then STATE_DISARMED.
   */

  /* Current behavior: blink led0..led2 together RESET_BLINKS times, then
   * go to Disarmed. */
  int64_t elapsed = k_uptime_get() - state_entered_ms;
  int64_t period = RESET_BLINK_ON_MS + RESET_BLINK_OFF_MS;

  if (elapsed >= RESET_BLINKS * period)
  {
    enter_state(STATE_DISARMED);
    return;
  }
  status_leds_show(elapsed % period < RESET_BLINK_ON_MS ? 0x7 : 0x0);
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

  if (!dw3110_id_check())
  {
    self_test_fail_halt();
  }
  self_test_pass_blink();

  state = STATE_DISARMED;
  state_entered_ms = k_uptime_get();
  status_leds_show(state);
  printk("State: %s\n", state_name(state));

  while (1)
  {
    enum btn_event ev = button_poll();

    if (ev == BTN_HOLD)
    {
      printk("Button: hold\n");
      enter_state(STATE_RESET);
    }
    else if (ev == BTN_SHORT)
    {
      printk("Button: short press\n");
      enum ball_state next = next_on_short_press(state);
      if (next != state)
      {
        enter_state(next);
      }
    }

    switch (state)
    {
    case STATE_DISARMED: run_disarmed(); break;
    case STATE_READY: run_ready(); break;
    case STATE_ARMED: run_armed(); break;
    case STATE_CLASSIFY: run_classify(); break;
    case STATE_INFORM: run_inform(); break;
    case STATE_RESET: run_reset(); break;
    }

    k_msleep(LOOP_PERIOD_MS);
  }
  return 0;
}
