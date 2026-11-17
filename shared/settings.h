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
#define LEAVE_DISTANCE_MM 1000   /* Ready -> Armed: thrower pouch farther than this (or not heard) ... */
#define TOO_CLOSE_MM 300         /* Ready: ranging fails when pouch and ball nearly touch. If the thrower
                                  * goes silent and its last valid range was <= this, it is assumed to
                                  * still be right next to the ball (not thrown). Tune on hardware. */
#define TRANSITION_CONFIRM_ROUNDS 2 /* Disarmed -> Ready and Ready -> Armed only after the distance
                                     * condition held for this many consecutive ranging rounds
                                     * (filters out occasional noisy measurements) */
#define HIT_DISTANCE_MM 900     /* Classify: closest pouch must be within this to count */
#define SELF_HIT_IF_SINGLE_POUCH 1 /* with exactly one pouch registered, Classify may hit the thrower
                                    * (normally "thrower hit itself" goes back to Ready). For bench
                                    * tests with two boards; has no effect with 2+ pouches. */

/* ---- Accelerometer (ball only) ------------------------------------------ */
#define FREEFALL_MG 500          /* |a| below this (well under 1 g = 1000) means free fall */
#define FREEFALL_MIN_MS 30       /* |a| must stay below FREEFALL_MG this long to count */
#define FREEFALL_HOLD_MS 3000    /* Ready -> Armed: free fall must have been seen this recently */
#define IMPACT_MG 2500           /* |a| at or above this is an impact */
#define IMPACT_COOLDOWN_MS 1000  /* ignore further impacts for this long after one */
#define ACCEL_SAMPLE_PERIOD_MS 5 /* 200 Hz polling of the sensor (sensor itself runs at 400 Hz) */

/* ---- Roll call ------------------------------------------------------------ */
#define ROLLCALL_TIMEOUT_MS 5000 /* leave Roll Call this long after the last NEW pouch registered */
#define JOIN_RETRY_MIN_MS 100     /* pouch re-sends JOIN after a random delay in this range */
#define JOIN_RETRY_MAX_MS 300

/* ---- Ball state machine -------------------------------------------------- */
#define CLASSIFY_ATTEMPTS 3      /* ranging rounds tried in Classify if nobody answers */
#define INFORM_TIMEOUT_MS 1000   /* give up telling a pouch about a hit after this long */
#define RESET_RETRY_MS 200       /* ball re-sends RESET this often while blinking */

/* ---- UWB timing (UUS: 1 UUS = ~1.026 us = 65536 device time units) ---------- */
#define SLOT_BASE_UUS 2000       /* trigger frame received -> start of timeslot 0 */
#define SLOT_UUS 1000            /* length of one timeslot */
#define FINAL_MARGIN_UUS 1500    /* ball: end of response window -> FINAL transmit */
#define ANTENNA_DELAY 16385      /* generic default, not calibrated per board */
#define RANGE_OFFSET_MM 0        /* subtracted from every range; calibrate if biased */

#endif
