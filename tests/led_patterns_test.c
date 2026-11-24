/* Host test of the LED pattern tables (shared/led_patterns.h):
 *   cc -std=c11 -Wall -Wextra -Werror -Ishared tests/led_patterns_test.c -o /tmp/led_patterns_test && /tmp/led_patterns_test */
#include <assert.h>
#include <stdio.h>
#include "led_patterns.h"

/* The pattern the pouch shows (no effect running) at time t. */
static uint8_t pouch_at(enum pouch_phase phase, uint8_t lives, int64_t t)
{
  return pouch_led_mask(phase, lives, POUCH_EFFECT_NONE, 0, t);
}

int main(void)
{
  /* steady, slow blink, fast blink, flash */
  assert(led_pattern_mask(&PAT_BALL_UNHELD, 0) == LED_GREEN);
  assert(led_pattern_mask(&PAT_BALL_UNHELD, 123456) == LED_GREEN);
  assert(led_pattern_mask(&PAT_POUCH_JOINED, 0) == LED_BLUE);
  assert(led_pattern_mask(&PAT_POUCH_JOINED, LED_SLOW_MS - 1) == LED_BLUE);
  assert(led_pattern_mask(&PAT_POUCH_JOINED, LED_SLOW_MS) == LED_NONE);
  assert(led_pattern_mask(&PAT_POUCH_JOINED, 2 * LED_SLOW_MS) == LED_BLUE);
  assert(led_pattern_mask(&PAT_POUCH_JOINING, LED_FAST_MS) == LED_NONE);
  assert(led_pattern_mask(&PAT_POUCH_LISTENING, 0) == LED_BLUE);
  assert(led_pattern_mask(&PAT_POUCH_LISTENING, LED_FLASH_MS) == LED_NONE);
  assert(led_pattern_mask(&PAT_POUCH_LISTENING, LED_LISTEN_PERIOD_MS - 1) == LED_NONE);
  assert(led_pattern_mask(&PAT_POUCH_LISTENING, LED_LISTEN_PERIOD_MS) == LED_BLUE);

  /* "red" lights both red LEDs, one color at a time */
  assert(LED_RED == 0x6 && (LED_RED & (LED_GREEN | LED_BLUE)) == 0);

  /* pouch lives: green / blue / red blinking / red steady (never mixed) */
  assert(pouch_at(POUCH_ACTIVE, 3, 777) == LED_GREEN);
  assert(pouch_at(POUCH_ACTIVE, 2, 777) == LED_BLUE);
  assert(pouch_at(POUCH_ACTIVE, 1, 0) == LED_RED);
  assert(pouch_at(POUCH_ACTIVE, 1, LED_FAST_MS) == LED_NONE);
  assert(pouch_at(POUCH_ACTIVE, 0, 0) == LED_RED);
  assert(pouch_at(POUCH_ACTIVE, 0, LED_FAST_MS) == LED_RED); /* out of lives: steady, not the blinking 1-life look */

  /* blue steady (2 lives) is never confused with blue blinking (Joined) */
  assert(pouch_at(POUCH_ACTIVE, 2, LED_SLOW_MS) == LED_BLUE && pouch_at(POUCH_JOINED, 2, LED_SLOW_MS) == LED_NONE);

  /* the phase decides before the lives do */
  assert(pouch_at(POUCH_JOINED, 3, 0) == LED_BLUE);
  assert(pouch_at(POUCH_JOINING, 0, 0) == LED_BLUE);

  /* hit effect: all four LEDs at 5 Hz for LED_HIT_EFFECT_MS, then the lives pattern */
  assert(pouch_led_mask(POUCH_ACTIVE, 2, POUCH_EFFECT_HIT, 0, 5000) == LED_ALL);
  assert(pouch_led_mask(POUCH_ACTIVE, 2, POUCH_EFFECT_HIT, LED_5HZ_MS, 5000) == LED_NONE);
  assert(pouch_led_mask(POUCH_ACTIVE, 2, POUCH_EFFECT_HIT, LED_HIT_EFFECT_MS - 1, 5000) != LED_BLUE);
  assert(pouch_led_mask(POUCH_ACTIVE, 2, POUCH_EFFECT_HIT, LED_HIT_EFFECT_MS, 5000) == LED_BLUE);
  /* reset effect: all four on for LED_RESET_EFFECT_MS, then the 3-lives pattern */
  assert(pouch_led_mask(POUCH_ACTIVE, 3, POUCH_EFFECT_RESET, LED_RESET_EFFECT_MS - 1, 0) == LED_ALL);
  assert(pouch_led_mask(POUCH_ACTIVE, 3, POUCH_EFFECT_RESET, LED_RESET_EFFECT_MS, 0) == LED_GREEN);

  /* ball: the Reset blink is the only pattern that uses all four LEDs steady-on-off */
  assert(PAT_BALL_RESET.mask == LED_ALL);
  assert(led_pattern_mask(&PAT_BALL_RESET, 0) == LED_ALL && led_pattern_mask(&PAT_BALL_RESET, LED_SLOW_MS) == LED_NONE);
  assert(PAT_BALL_READY.off_ms == 0 && PAT_BALL_THROWN.off_ms != 0 && PAT_BALL_READY.mask == PAT_BALL_THROWN.mask);

  printf("led_patterns_test: OK\n");
  return 0;
}
