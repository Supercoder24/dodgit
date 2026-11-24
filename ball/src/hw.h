/*
 * dodgit - ball hardware: the four LEDs, the user button and the startup self-test results.
 * (The state machine in main.c decides WHAT to show; patterns are data in shared/led_patterns.h.)
 */
#ifndef DODGIT_BALL_HW_H
#define DODGIT_BALL_HW_H

#include <stdbool.h>
#include <stdint.h>

/* Configure the LEDs (all off) and the button. Returns false if a GPIO is not ready. */
bool hw_init(void);

/* Light exactly the LEDs in mask (bit i = ledi: led0 green, led1 + led2 red, led3 blue); only touches
 * the GPIOs when the mask changes. */
void leds_show(uint8_t mask);

/* Button, polled every main-loop pass with a small debounce.
 *   BTN_SHORT - released before BUTTON_HOLD_MS
 *   BTN_HOLD  - fired once when BUTTON_HOLD_MS is reached while still pressed; the following
 *               release is swallowed (no BTN_SHORT). */
/* (BUTTON_HOLD_MS is in shared/settings.h.) */
enum btn_event
{
  BTN_NONE,
  BTN_SHORT,
  BTN_HOLD,
};
enum btn_event button_poll(void);

/* Self-test results. Pass: green blinks 3 times. Fail: both reds blink forever, `what` and rc go to
 * the console. */
void self_test_pass_blink(void);
void self_test_fail_halt(const char *what, int rc) __attribute__((noreturn));

#endif
