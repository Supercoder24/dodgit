/*
 * dodgit - Pouch firmware (DWM3001CDK)
 *
 * Startup:
 *   Self-test: DW3110 UWB radio probe.
 *     PASS: led0 (green) blinks 3 times, then the pouch starts Joining.
 *     FAIL: led1 + led2 (red) blink together, slowly, forever.
 *
 * The pouch FSM rules are in shared/pouch_fsm.h; shared/uwb.c is the radio glue around them (see
 * shared/README.md and docs/FSMs.md). The ball owns the lives and the timeslots; the pouch only displays
 * them and answers the ball's messages:
 *
 *   JOINING    sends JOIN (its 32-bit hardware ID) at random intervals until the ball answers with
 *              REMEMBER; hearing the ball's POLL first means the game is already running -> LISTENING
 *   LISTENING  silent, until a REMEMBER with our ID arrives (we rebooted mid-game: same slot and lives
 *              as before) or a RESET (the ball accepts new pouches); no POLL for LOST_TIMEOUT_MS = the
 *              ball rebooted -> JOINING
 *   JOINED     slot and lives known, waiting for the ball's first POLL / HIT / RESET
 *   ACTIVE     answering POLLs, HITs and RESETs; a REMEMBER keeps slot and lives in sync
 * No REMEMBER for LOST_TIMEOUT_MS sends a Joined / Active pouch back to Joining. Nothing is terminal:
 * a pouch can be switched on or rebooted at any time.
 *
 * What the LEDs show is data in shared/led_patterns.h, driven by display.c (the LED strips will
 * replace that backend). This file only feeds the display layer and logs events to the console
 * (events only, never periodically, so a slow console cannot make a reply miss its timeslot).
 *
 * The pouch does not use its button or accelerometer.
 */

#include <stdarg.h>
#include <zephyr/kernel.h>
#include "settings.h"
#include "log.h"
#include "uwb.h"
#include "display.h"

/* Log sink for LOG_CAT (shared/log.h): the pouch has no Bluetooth, console only. */
void dodgit_log(const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  vprintk(fmt, args);
  va_end(args);
  printk("\n");
}

#define RESET_REPEAT_IGNORE_MS 1500 /* RESETs closer together than this belong to one Reset */

static const char *phase_name(enum pouch_phase phase)
{
  switch (phase)
  {
  case POUCH_JOINING: return "Joining";
  case POUCH_LISTENING: return "Listening";
  case POUCH_JOINED: return "Joined";
  case POUCH_ACTIVE: return "Active";
  }
  return "?";
}

int main(void)
{
  printk("\n===== dodgit POUCH =====\n");

  if (!display_init())
  {
    return 0;
  }

  int rc = uwb_init();
  if (rc)
  {
    printk("Halting: DW3110 init failed (%d)\n", rc);
    display_selftest_fail();
  }
  display_selftest_pass();

  printk("Pouch hardware ID: 0x%08x\n", uwb_pouch_uid());
  uwb_pouch_start();

  enum pouch_phase last_phase = uwb_pouch_phase();
  int64_t last_reset_ms = -RESET_REPEAT_IGNORE_MS;
  LOG_CAT(LOG_MEMBERSHIP, "MEMBER phase %s: sending JOIN until the ball answers", phase_name(last_phase));

  while (1)
  {
    /* One short radio step. This is also where ranging replies, hit and reset messages are handled
     * (and acknowledged) in the pouch's timeslot. Everything below runs after the reply was sent. */
    struct pouch_event ev;
    uwb_pouch_step(&ev);

    int64_t now = k_uptime_get();
    enum pouch_phase phase = uwb_pouch_phase();
    uint8_t lives = uwb_pouch_lives();

    if (phase != last_phase)
    {
      LOG_CAT(LOG_MEMBERSHIP, "MEMBER phase %s -> %s (slot %d, lives %u)", phase_name(last_phase),
              phase_name(phase), uwb_pouch_slot(), lives);
      last_phase = phase;
    }

    switch (ev.type)
    {
    case EV_HIT:
      display_hit(now);
      LOG_CAT(LOG_STATE, "HIT, lives = %u", lives);
      break;
    case EV_RESET:
      /* During a Reset the ball repeats RESET (and every pouch acknowledges each one): show and log the
       * first, not every repeat. */
      if (now - last_reset_ms >= RESET_REPEAT_IGNORE_MS)
      {
        display_reset(now);
        LOG_CAT(LOG_STATE, "RESET, lives = %u", lives);
      }
      last_reset_ms = now;
      break;
    case EV_REMEMBER:
      LOG_CAT(LOG_REMEMBER, "REMEMBER slot %d, lives %u", uwb_pouch_slot(), lives);
      break;
    case EV_NONE:
      break;
    }

    display_update(phase, lives, now);
  }
  return 0;
}
