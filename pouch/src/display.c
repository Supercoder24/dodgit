/*
 * dodgit - pouch display layer (plain LEDs). See display.h and shared/led_patterns.h.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "display.h"
#include "led_patterns.h"

/* aliases from the decawave_dwm3001cdk board DTS; bit i of a pattern mask = leds[i] */
static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios), /* D9  green */
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios), /* D12 red */
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios), /* D11 red */
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios), /* D10 blue */
};

static uint8_t shown = 0xFF; /* mask currently on the LEDs; 0xFF = unknown, forces the first write */
static enum pouch_effect effect = POUCH_EFFECT_NONE;
static int64_t effect_start_ms;

static void leds_show(uint8_t mask)
{
  if (mask == shown)
  {
    return;
  }
  shown = mask;
  for (int i = 0; i < ARRAY_SIZE(leds); i++)
  {
    gpio_pin_set_dt(&leds[i], (mask >> i) & 1);
  }
}

bool display_init(void)
{
  for (int i = 0; i < ARRAY_SIZE(leds); i++)
  {
    if (!gpio_is_ready_dt(&leds[i]) || gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) < 0)
    {
      printk("LED %d init failed\n", i);
      return false;
    }
  }
  shown = LED_NONE;
  return true;
}

void display_selftest_pass(void)
{
  for (int i = 0; i < 3; i++)
  {
    leds_show(LED_GREEN);
    k_msleep(100);
    leds_show(LED_NONE);
    k_msleep(150);
  }
}

void display_selftest_fail(void)
{
  while (1)
  {
    leds_show(LED_RED);
    k_msleep(250);
    leds_show(LED_NONE);
    k_msleep(250);
  }
}

void display_hit(int64_t now_ms)
{
  effect = POUCH_EFFECT_HIT;
  effect_start_ms = now_ms;
}

void display_reset(int64_t now_ms)
{
  effect = POUCH_EFFECT_RESET;
  effect_start_ms = now_ms;
}

void display_update(enum pouch_phase phase, uint8_t lives, int64_t now_ms)
{
  leds_show(pouch_led_mask(phase, lives, effect, now_ms - effect_start_ms, now_ms));
}
