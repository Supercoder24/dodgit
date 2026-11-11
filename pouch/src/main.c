/*
 * dodgit - Pouch firmware (DWM3001CDK)
 *
 * Startup:
 *   1. led3 (blue) stays OFF while playing: it marks this board as a pouch (the ball has it on).
 *   2. Self-test: DW3110 UWB radio probe.
 *      PASS: led0 (green) blinks 3 times, then roll call starts.
 *      FAIL: led1 + led2 (red) blink together, slowly, forever.
 *
 * Roll call (see shared/README.md): the pouch announces its 32-bit hardware ID to the
 * ball until the ball answers with a timeslot. All later ranging replies and hit/reset
 * acknowledgments are sent in that timeslot. What the LEDs show:
 *
 *   JOINING      blue blinks (2 Hz), everything else off     - no timeslot yet
 *   JOINED       blue steady + assigned timeslot in binary   - timeslot received, waiting
 *                on led2 led1 led0 (led0 = 1s bit)             for the ball to start the game
 *   ACTIVE       blue off, lives shown (below)               - ball is ranging: game running
 *   JOIN FAILED  ALL FOUR LEDs flash fast (5 Hz), forever    - the ball was already ranging
 *                                                               before we got a timeslot
 *
 * A pouch switched on after the ball left Roll Call (a "late pouch") cannot join: it ends
 * up in JOIN FAILED. Turn off all pouches and the ball, then start again.
 *
 * Lives (ACTIVE): that many of led2..led0 lit, steady (the green led0 goes out first).
 * At 0 lives led0..led2 blink together once per second. A HIT message from the ball
 * costs one life (ignored at 0 lives); a RESET message restores LIVES_MAX.
 *
 * The pouch does not use its button or accelerometer.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "settings.h"
#include "uwb.h"

#define JOINING_BLINK_PERIOD_MS 500 /* blue blink while waiting for a timeslot */
#define FAIL_BLINK_PERIOD_MS 200    /* roll-call failure: all LEDs, 5 Hz */
#define DEAD_BLINK_PERIOD_MS 1000
#define DEAD_BLINK_ON_MS 500

static int lives = LIVES_MAX;

/* ---------------------------------------------------------------------------
 * LEDs (aliases from the decawave_dwm3001cdk board DTS)
 * ------------------------------------------------------------------------- */
static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios), /* D9  green */
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios), /* D12 red */
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios), /* D11 red */
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios), /* D10 blue - identity */
};

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
  return true;
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

/* Radio failure: only led1 + led2 (red) blink, slowly. Deliberately calmer than the
 * roll-call failure pattern below. */
static void self_test_fail_halt(int rc)
{
  printk("Halting: DW3110 init failed (%d)\n", rc);
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
 * LED display for each phase
 * ------------------------------------------------------------------------- */
static void show_leds(enum pouch_phase phase)
{
  int64_t now = k_uptime_get();

  switch (phase)
  {
  case POUCH_JOINING:
    led_set(0, false);
    led_set(1, false);
    led_set(2, false);
    led_set(3, now % JOINING_BLINK_PERIOD_MS < JOINING_BLINK_PERIOD_MS / 2);
    break;

  case POUCH_JOINED:
  {
    int slot = uwb_pouch_slot();
    led_set(0, slot & 1);
    led_set(1, (slot >> 1) & 1);
    led_set(2, (slot >> 2) & 1);
    led_set(3, true);
    break;
  }

  case POUCH_ACTIVE:
    led_set(3, false);
    if (lives > 0)
    {
      /* N lives = last N of led0..led2 lit, steady (green drops first) */
      for (int i = 0; i < 3; i++)
      {
        led_set(i, i >= 3 - lives);
      }
    }
    else
    {
      /* Out of lives: blink all three once per second to show we are alive */
      bool on = now % DEAD_BLINK_PERIOD_MS < DEAD_BLINK_ON_MS;
      for (int i = 0; i < 3; i++)
      {
        led_set(i, on);
      }
    }
    break;

  case POUCH_JOIN_FAILED:
  {
    bool on = now % FAIL_BLINK_PERIOD_MS < FAIL_BLINK_PERIOD_MS / 2;
    for (int i = 0; i < 4; i++)
    {
      led_set(i, on);
    }
    break;
  }
  }
}

int main(void)
{
  printk("\n===== dodgit POUCH =====\n");

  if (!hw_init())
  {
    return 0;
  }
  led_set(3, false); /* identity: blue off on a pouch */

  int rc = uwb_init();
  if (rc)
  {
    self_test_fail_halt(rc);
  }
  self_test_pass_blink();

  printk("Pouch hardware ID: 0x%08x\n", uwb_pouch_uid());
  printk("Roll call: sending JOIN until the ball assigns a timeslot\n");

  enum pouch_phase last_phase = uwb_pouch_phase();
  bool have_hit = false;   /* have we counted a hit yet? (for de-duplicating retries) */
  uint16_t last_hit_seq = 0;

  while (1)
  {
    /* One short radio step. This is also where ranging replies, hit and reset
     * messages are handled (and acknowledged) in the pouch's timeslot. */
    struct pouch_event ev;
    uwb_pouch_step(lives, &ev);

    enum pouch_phase phase = uwb_pouch_phase();
    if (phase != last_phase)
    {
      switch (phase)
      {
      case POUCH_JOINED:
        printk("Roll call: assigned timeslot %d, waiting for the game to start\n", uwb_pouch_slot());
        break;
      case POUCH_ACTIVE:
        printk("Ball is ranging: game started, lives = %d\n", lives);
        break;
      case POUCH_JOIN_FAILED:
        printk("Roll call FAILED: ball is already ranging and never gave us a timeslot "
               "(joined too late?). Turn everything off and start again.\n");
        break;
      default:
        break;
      }
      last_phase = phase;
    }

    if (ev.type == EV_HIT)
    {
      if (have_hit && ev.hit_seq == last_hit_seq)
      {
        printk("Hit #%u repeated, ignored\n", ev.hit_seq);
      }
      else
      {
        have_hit = true;
        last_hit_seq = ev.hit_seq;
        if (lives > 0)
        {
          lives--;
        }
        printk("HIT #%u, lives = %d\n", ev.hit_seq, lives);
      }
    }
    else if (ev.type == EV_RESET)
    {
      lives = LIVES_MAX;
      have_hit = false;
      printk("RESET, lives = %d\n", lives);
    }

    show_leds(phase);
  }
  return 0;
}
