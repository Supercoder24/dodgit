/*
 * dodgit - tunable settings shared by the ball and the pouch.
 * All distances/thresholds are placeholders: tune them on real hardware.
 */
#ifndef DODGIT_SETTINGS_H
#define DODGIT_SETTINGS_H

/* ---- Game ---------------------------------------------------------------- */
#define LIVES_MAX 3    /* starting lives, and the value after a reset */
#define MAX_POUCHES 8  /* timeslots in the roll-call table (the prototype uses <= 4) */

/* ---- Distances (millimeters) -------------------------------------------- */
#define PICKUP_DISTANCE_MM 1000   /* Disarmed -> Ready: ball this close to a pouch with lives */
#define LEAVE_DISTANCE_MM 700    /* Ready -> Armed: thrower pouch farther than this (or not heard) ... */
#define TOO_CLOSE_MM 300         /* Ready: ranging fails when pouch and ball nearly touch. If the thrower
                                  * goes silent and its last valid range was <= this, it is assumed to
                                  * still be right next to the ball (not thrown). Tune on hardware. */
#define TRANSITION_CONFIRM_ROUNDS 2 /* Disarmed -> Ready only after the distance condition held for this
                                     * many consecutive ranging rounds (filters out noisy measurements) */
#define ARM_CONFIRM_ROUNDS 1     /* Ready -> Armed: out-of-range rounds needed. Free fall is already strong
                                  * evidence of a release, so one round is enough (a short throw is over
                                  * in well under half a second). */
#define HIT_DISTANCE_MM 900     /* Classify: closest pouch must be within this to count */
#define SELF_HIT_IF_SINGLE_POUCH 1 /* with exactly one pouch registered, Classify may hit the thrower
                                    * (normally "thrower hit itself" goes back to Ready). For bench
                                    * tests with two boards; has no effect with 2+ pouches. */

/* ---- Accelerometer (ball only) ------------------------------------------ */
#define FREEFALL_MG 700          /* |a| below this (well under 1 g = 1000) means free fall */
#define FREEFALL_MIN_MS 30       /* |a| must stay below FREEFALL_MG this long to count */
#define FREEFALL_HOLD_MS 3000    /* Ready -> Armed: free fall must have been seen this recently */
#define IMPACT_MG 2500           /* |a| at or above this is an impact */
#define IMPACT_COOLDOWN_MS 300   /* ignore further impacts for this long after an ACCEPTED one (ringing /
                                  * double count). Kept short: a hit can follow a throw within a second. */
#define IMPACT_AFTER_FREEFALL_MS 150 /* an impact only counts if free fall was seen at most this long before it
                                      * (the ball was flying). Rejects the launch spike of a throw and bumps
                                      * while the ball is held, and a rejected spike starts no cooldown. */
#define STILL_DELTA_MG 100       /* |a(t) - a(t-100 ms)| above this = the ball is moving (not lying still) */
#define READY_STILL_TIMEOUT_MS 5000 /* Ready -> Disarmed: ball motionless this long = dropped / put down */
#define PICKUP_MOTION_MS 500     /* Disarmed -> Ready also needs motion within this long (ball being handled) */
#define ACCEL_STALL_MS 100       /* no fresh accelerometer sample for this long = sensor stalled; the ball
                                  * re-initializes it (samples normally arrive every few ms) */
#define ACCEL_RECOVER_RETRY_MS 250 /* while stalled, try the re-init at most this often */
#define ACCEL_SAMPLE_PERIOD_MS 5 /* 200 Hz polling of the sensor (sensor itself runs at 400 Hz) */

/* ---- Roll call and membership ---------------------------------------------- */
#define ROLLCALL_TIMEOUT_MS 15000 /* leave Roll Call this long after the last NEW pouch registered */
#define JOIN_RETRY_MIN_MS 100     /* pouch re-sends JOIN after a random delay in this range
                                   * (also the delay before its very first JOIN after power-up) */
#define JOIN_RETRY_MAX_MS 300
#define REMEMBER_PERIOD_MS 1000   /* ball: every pouch gets one REMEMBER per this long (sent as a
                                   * trickle, one pouch every REMEMBER_PERIOD_MS / pouches) */
#define LOST_TIMEOUT_MS 5000      /* pouch: no REMEMBER addressed to it for this long (Listening: no POLL
                                   * for this long) = the ball is gone or forgot us -> back to Joining */
#define REJOIN_DELAY_MAX_MS 1000  /* pouch: after hearing RESET, wait a random 0..this before sending
                                   * JOIN, so several late pouches do not collide */

/* ---- Ball state machine -------------------------------------------------- */
#define CLASSIFY_ATTEMPTS 3      /* ranging rounds tried in Classify if nobody answers */
#define INFORM_TIMEOUT_MS 1000   /* give up telling a pouch about a hit after this long */
#define RESET_RETRY_MS 200       /* ball re-sends RESET this often while blinking */
#define ARMED_TIMEOUT_MS 6000    /* Armed -> Disarmed: no impact for this long (wrong arm, or a throw that
                                  * never landed on anything) */
#define ARMED_RECHECK_MS 250     /* Armed: one ranging round this often, to notice the thrower is back */
#define REARM_DISTANCE_MM 700    /* Armed -> Ready: thrower within this for REARM_CONFIRM_ROUNDS re-checks
                                  * in a row means the throw was cancelled (wind-up, ball still held) */
#define REARM_CONFIRM_ROUNDS 2

/* ---- Ball state machine v2 (Unheld / Ready / Thrown / Hit) ----------------- */
#define THROW_RANGE_MM (2 * PICKUP_DISTANCE_MM) /* Ready -> Thrown: the THROWER is farther than this (and
                                                 * the ball is Spinning). Derived from the pickup range: a
                                                 * throw must travel at least this far before it lands. */
#define THROW_TIMEOUT_MS 3500    /* Thrown -> Disarm(): no hit for this long */
#define THROWN_RETURN_MS 1000    /* Thrown -> Ready: the closest alive pouch stayed within THROW_RANGE_MM this
                                  * long without a hit (throw cancelled, or the ball ended up near someone) */
#define READY_ORPHAN_MS 1000     /* Ready -> Unheld: no alive pouch within THROW_RANGE_MM for this long */
#define READY_NOT_HELD_MS 5000   /* Ready -> Unheld: accelerometer not Held for this long */
#define THROWER_SWITCH_MS 1000   /* Ready: another alive pouch must be closer than the thrower this long
                                  * (without a break) to become the new thrower */

/* ---- Logging (compile-time switches; 1 = on, 0 = compiled out) -------------- *
 * A disabled category costs nothing: no formatting, no console print, no Bluetooth queue slot.
 * Every line goes to the console and, on the ball, to Bluetooth (see ball/README.md). The pouch has
 * no Bluetooth: its flags only gate console output, and it only logs events (never periodically). */
#define LOG_STATE 1          /* State changes, Roll Call registrations, Reset summary, Classify, HIT lines */
#define LOG_STATUS 1         /* the STATUS line, STATUS_LOG_PERIOD_MS apart */
#define LOG_RANGE 0          /* RANGE lines: up to 5 rounds/s x one line per pouch */
#define LOG_MEMBERSHIP 1     /* JOIN, slot assigned, pouch deleted, pouch phase changes (events only) */
#define LOG_REMEMBER 0       /* every REMEMBER sent / received (up to 8/s on the ball) */
#define LOG_ACCEL_CLASS 1    /* classifier level changes and Hit events (on change only) */
#define LOG_ACCEL_BURST 0    /* raw |a| windows around every threshold crossing (~11 lines each) */
#define LOG_ACCEL_FEATURES 0 /* the classifier's window features, for threshold tuning */
#define LOG_ACCEL_RAW 0      /* raw x/y/z capture into RAM, dumped after a button press */
#define STATUS_LOG_PERIOD_MS 250
#define RANGE_LOG_PERIOD_MS 200

/* ---- UWB timing (UUS: 1 UUS = ~1.026 us = 65536 device time units) ---------- */
#define SLOT_BASE_UUS 2000       /* trigger frame received -> start of timeslot 0 */
#define SLOT_UUS 1000            /* length of one timeslot */
#define FINAL_MARGIN_UUS 1500    /* ball: end of response window -> FINAL transmit */
#define ANTENNA_DELAY 16385      /* generic default, not calibrated per board */
#define RANGE_OFFSET_MM 0        /* subtracted from every range; calibrate if biased */

#endif
