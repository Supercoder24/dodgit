/*
 * dodgit - pouch display layer.
 *
 * The pouch FSM (shared/uwb.c, main.c) only tells this layer WHAT to show (phase, lives, a hit or
 * reset event); the patterns are data in shared/led_patterns.h and the LED GPIOs are driven here.
 * When the WS2812 strips replace the plain LEDs only this file's backend has to change.
 */
#ifndef DODGIT_DISPLAY_H
#define DODGIT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>
#include "uwb.h"

/* Configure the LEDs (all off). Returns false if a GPIO is not ready. */
bool display_init(void);

/* Self-test results (blocking). Pass: green blinks 3 times. Fail: both reds blink slowly, forever. */
void display_selftest_pass(void);
void display_selftest_fail(void) __attribute__((noreturn));

/* The pouch lost a life / the ball reset the lives: play the effect, then fall back to the pattern. */
void display_hit(int64_t now_ms);
void display_reset(int64_t now_ms);

/* Show what the phase and lives call for. Call every loop pass (it only touches the LEDs when
 * something changes). */
void display_update(enum pouch_phase phase, uint8_t lives, int64_t now_ms);

#endif
