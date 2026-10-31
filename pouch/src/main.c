/*
 * dodgit - Pouch firmware (DWM3001CDK)
 *
 * Startup:
 *   1. led3 (blue) stays OFF: it marks this board as a pouch (the ball has it on).
 *   2. Quick DW3110 ID check (dwt_probe + device ID upper half 0xDECA).
 *      PASS: led0 (green) blinks 3 times, then the main loop starts.
 *      FAIL: led1 + led2 (red) blink forever.
 *
 * Main loop, currently driven only by the user button so it can be tested on
 * a single board without radio:
 *   short press (< 3 s) : lose one life (ignored at 0 lives)
 *   hold (>= 3 s)       : simulated reset from the ball, lives = LIVES_MAX
 *
 * Lives are shown as that many LEDs lit on led0..led2 (steady). At 0 lives
 * led0..led2 blink together once per second.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <deca_device_api.h>
#include <deca_probe_interface.h>
#include <dw3000_hw.h>

#define LIVES_MAX 3 /* initial lives and the value after a reset */

#define LOOP_PERIOD_MS 10
#define HOLD_TIME_MS 3000
#define DEBOUNCE_SAMPLES 3 /* 3 x 10 ms of stable level */

#define DEAD_BLINK_PERIOD_MS 1000
#define DEAD_BLINK_ON_MS 500

static int lives = LIVES_MAX;

/* ---------------------------------------------------------------------------
 * LEDs and button (aliases from the decawave_dwm3001cdk board DTS)
 * ------------------------------------------------------------------------- */
static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios), /* D9  green */
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios), /* D12 red */
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios), /* D11 red */
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios), /* D10 blue - identity, off on pouch */
};
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static void led_set(int i, bool on)
{
  gpio_pin_set_dt(&leds[i], on);
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
 * Lives display
 * ------------------------------------------------------------------------- */
static void lives_show(void)
{
  if (lives > 0)
  {
    /* N lives = led0..led(N-1) lit, steady */
    for (int i = 0; i < 3; i++)
    {
      led_set(i, i < lives);
    }
  }
  else
  {
    /* Out of lives: blink all three once per second to show we are alive */
    bool on = k_uptime_get() % DEAD_BLINK_PERIOD_MS < DEAD_BLINK_ON_MS;
    for (int i = 0; i < 3; i++)
    {
      led_set(i, on);
    }
  }
}

/* ---------------------------------------------------------------------------
 * Future radio behavior (not implemented yet):
 *
 *   on ranging request from the ball:
 *     reply immediately (near-instant response so the ball's distance
 *     estimate is accurate); include lives so the ball can skip pouches
 *     with 0 lives when looking for the thrower
 *
 *   on HIT message addressed to this pouch:
 *     if (lives > 0) lives--;
 *     send_ack(ball, HIT);
 *
 *   on RESET message (broadcast from the ball):
 *     lives = LIVES_MAX;
 *     send_ack(ball, RESET);
 *
 * Until then the button stands in for these messages: short press = HIT,
 * hold = RESET.
 * ------------------------------------------------------------------------- */

int main(void)
{
  printk("\n===== dodgit POUCH =====\n");

  if (!hw_init())
  {
    return 0;
  }

  /* Identity: blue LED stays off on a pouch. */
  led_set(3, false);

  if (!dw3110_id_check())
  {
    self_test_fail_halt();
  }
  self_test_pass_blink();

  printk("Lives: %d\n", lives);

  while (1)
  {
    /* Future: poll the radio here for ranging requests, HIT and RESET
     * messages (see pseudocode above). */

    enum btn_event ev = button_poll();

    if (ev == BTN_HOLD)
    {
      lives = LIVES_MAX;
      printk("Button: hold -> reset, lives = %d\n", lives);
    }
    else if (ev == BTN_SHORT)
    {
      if (lives > 0)
      {
        lives--;
        printk("Button: short press -> hit, lives = %d\n", lives);
      }
      else
      {
        printk("Button: short press ignored, no lives left\n");
      }
    }

    lives_show();
    k_msleep(LOOP_PERIOD_MS);
  }
  return 0;
}
