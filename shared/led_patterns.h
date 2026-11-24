/*
 * dodgit - LED patterns of the ball and the pouch, defined as data (pure C, no Zephyr: host test in
 * tests/led_patterns_test.c). Remapping a color or a timing is a one-line change here.
 *
 * Hardware: four plain GPIO LEDs per DWM3001CDK board, no dimming:
 *   led0 green (D9)   led1 red (D12)   led2 red (D11)   led3 blue (D10)
 * so there are effectively three colors. "Red" always lights BOTH red LEDs (twice the light, closer
 * to green's brightness).
 *
 * Principles (docs/Plan.md Part E):
 *  - One color at a time. Only the explicit effects (pouch hit effect, ball Reset, ball Hit) mix colors.
 *  - Color is never the only cue (red-green color blindness): steady vs. blinking carries meaning too.
 *    Steady = a lasting condition, blinking = needs attention / in progress.
 *  - No binary codes, no identity LED.
 */
#ifndef DODGIT_LED_PATTERNS_H
#define DODGIT_LED_PATTERNS_H

#include <stdint.h>
#include "settings.h"
#include "pouch_fsm.h" /* enum pouch_phase */

/* Bit i = ledi of the board (the Zephyr aliases led0..led3). */
#define LED_NONE 0x0
#define LED_GREEN 0x1
#define LED_RED 0x6 /* both red LEDs */
#define LED_BLUE 0x8
#define LED_ALL 0xF

/* Timing (ms). Slow = 1 Hz, fast = 4 Hz, "5 Hz" is only used by the pouch hit effect. */
#define LED_SLOW_MS 500
#define LED_FAST_MS 125
#define LED_5HZ_MS 100
#define LED_FLASH_MS 100
#define LED_LISTEN_PERIOD_MS 2000 /* Listening: one flash per this long */
#define LED_HIT_EFFECT_MS 1000    /* pouch: all four LEDs at 5 Hz for this long after losing a life */
#define LED_RESET_EFFECT_MS 500   /* pouch: all four LEDs on this long after a RESET */

/* The LEDs in `mask` are on for on_ms, then all off for off_ms, repeating. off_ms == 0: steady. */
struct led_pattern
{
  uint8_t mask;
  uint16_t on_ms;
  uint16_t off_ms;
};

/* Mask of the LEDs that are lit at time t (ms): now for a free-running pattern, or the time since an
 * event / state entry for one that should start with its "on" phase. */
static inline uint8_t led_pattern_mask(const struct led_pattern *p, int64_t t)
{
  if (p->off_ms == 0)
  {
    return p->mask;
  }
  int64_t period = (int64_t)p->on_ms + p->off_ms;
  return (t % period) < p->on_ms ? p->mask : LED_NONE;
}

/* ---- Pouch (temporary until the LED strips arrive) -------------------------------------------- */
static const struct led_pattern PAT_POUCH_JOINING = {LED_BLUE, LED_FAST_MS, LED_FAST_MS};
static const struct led_pattern PAT_POUCH_JOINED = {LED_BLUE, LED_SLOW_MS, LED_SLOW_MS};
static const struct led_pattern PAT_POUCH_LISTENING = {LED_BLUE, LED_FLASH_MS,
                                                       LED_LISTEN_PERIOD_MS - LED_FLASH_MS};
static const struct led_pattern PAT_POUCH_LIVES_3 = {LED_GREEN, 1, 0};
static const struct led_pattern PAT_POUCH_LIVES_2 = {LED_BLUE, 1, 0};
static const struct led_pattern PAT_POUCH_LIVES_1 = {LED_RED, LED_FAST_MS, LED_FAST_MS};
static const struct led_pattern PAT_POUCH_LIVES_0 = {LED_RED, 1, 0}; /* out of lives: red, steady */
static const struct led_pattern PAT_POUCH_HIT = {LED_ALL, LED_5HZ_MS, LED_5HZ_MS};
static const struct led_pattern PAT_POUCH_RESET = {LED_ALL, 1, 0};

/* The lives patterns are written for LIVES_MAX = 3 (three distinguishable looks). The LED strips
 * will show one pixel per life and lift this limit. */
_Static_assert(LIVES_MAX <= 3, "pouch lives patterns (led_patterns.h) cover at most 3 lives");

enum pouch_effect
{
  POUCH_EFFECT_NONE,
  POUCH_EFFECT_HIT,   /* lost a life */
  POUCH_EFFECT_RESET, /* the ball reset everybody's lives */
};

static inline const struct led_pattern *pouch_pattern(enum pouch_phase phase, uint8_t lives)
{
  switch (phase)
  {
  case POUCH_JOINING: return &PAT_POUCH_JOINING;
  case POUCH_LISTENING: return &PAT_POUCH_LISTENING;
  case POUCH_JOINED: return &PAT_POUCH_JOINED;
  case POUCH_ACTIVE:
    break;
  }
  if (lives >= 3)
  {
    return &PAT_POUCH_LIVES_3;
  }
  switch (lives)
  {
  case 2: return &PAT_POUCH_LIVES_2;
  case 1: return &PAT_POUCH_LIVES_1;
  default: return &PAT_POUCH_LIVES_0;
  }
}

/* LEDs the pouch shows now. `effect` started effect_age_ms ago; once it is over (or if there is
 * none) the pattern of the phase / lives is shown. */
static inline uint8_t pouch_led_mask(enum pouch_phase phase, uint8_t lives,
                                     enum pouch_effect effect, int64_t effect_age_ms, int64_t now_ms)
{
  if (effect == POUCH_EFFECT_HIT && effect_age_ms < LED_HIT_EFFECT_MS)
  {
    return led_pattern_mask(&PAT_POUCH_HIT, effect_age_ms);
  }
  if (effect == POUCH_EFFECT_RESET && effect_age_ms < LED_RESET_EFFECT_MS)
  {
    return led_pattern_mask(&PAT_POUCH_RESET, effect_age_ms);
  }
  return led_pattern_mask(pouch_pattern(phase, lives), now_ms);
}

/* ---- Ball (nobody reads Thrown or Hit; the other four states are looked at from inside the ball) --- */
static const struct led_pattern PAT_BALL_ROLL_CALL = {LED_BLUE, 1, 0};
static const struct led_pattern PAT_BALL_UNHELD = {LED_GREEN, 1, 0};
static const struct led_pattern PAT_BALL_READY = {LED_RED, 1, 0};
static const struct led_pattern PAT_BALL_THROWN = {LED_RED, LED_FAST_MS, LED_FAST_MS};
static const struct led_pattern PAT_BALL_HIT = {LED_RED | LED_BLUE, 1, 0};
static const struct led_pattern PAT_BALL_RESET = {LED_ALL, LED_SLOW_MS, LED_SLOW_MS}; /* x RESET_BLINKS */
/* Roll Call flashes: a new pouch registered (green), or a short press was ignored because nobody has
 * registered yet (red). A flash REPLACES the steady blue for LED_BALL_FLASH_MS (one color at a time). */
#define LED_BALL_FLASH_MS 150
#define LED_BALL_FLASH_NEW_POUCH LED_GREEN
#define LED_BALL_FLASH_IGNORED LED_RED

#endif
