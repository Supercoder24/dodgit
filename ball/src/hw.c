/*
 * dodgit - ball hardware: LEDs, button, self-test results. See hw.h.
 * Aliases come from the decawave_dwm3001cdk board DTS and the overlay in ball/boards/: `sw0` is the external
 * button (header J10 pin 15 = nRF P0.28 to GND), unrelated to the board's hardware RESET button.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "hw.h"
#include "led_patterns.h"
#include "settings.h"

#define DEBOUNCE_SAMPLES 3 /* 3 consecutive loop passes with the same level */

static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios), /* D9  green */
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios), /* D12 red   */
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios), /* D11 red   */
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios), /* D10 blue  */
};
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static uint8_t leds_shown = LED_NONE;

void leds_show(uint8_t mask)
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

bool hw_init(void)
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

enum btn_event button_poll(void)
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

  if (stable && !hold_fired && now - press_start >= BUTTON_HOLD_MS)
  {
    hold_fired = true;
    return BTN_HOLD;
  }
  return BTN_NONE;
}

void self_test_pass_blink(void)
{
  for (int i = 0; i < 3; i++)
  {
    leds_show(LED_GREEN);
    k_msleep(100);
    leds_show(LED_NONE);
    k_msleep(150);
  }
}

void self_test_fail_halt(const char *what, int rc)
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
