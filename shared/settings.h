/*
 * dodgit - tunable settings shared by the ball and the pouch.
 * All distances/thresholds are placeholders: tune them on real hardware.
 */
#ifndef DODGIT_SETTINGS_H
#define DODGIT_SETTINGS_H

/* ---- Game ---------------------------------------------------------------- */
#define LIVES_MAX 3    /* starting lives, and the value after a reset */
#define MAX_POUCHES 8  /* timeslots in the ball's table of pouches (the prototype uses <= 4). At most 8: slot
                        * sets are one byte on the radio (FINAL mask) and in members.h */

/* ---- Distances (millimeters) -------------------------------------------- */
#define PICKUP_DISTANCE_MM 1000   /* Unheld -> Ready, Disarm(): ball this close to a pouch with lives (and Held) */
#define TOO_CLOSE_MM 300         /* Ready: ranging fails when pouch and ball nearly touch. If the thrower
                                  * goes silent and its last valid range was <= this, it is assumed to
                                  * still be right next to the ball (not thrown). Tune on hardware. */
#define TRANSITION_CONFIRM_ROUNDS 2 /* Unheld -> Ready only after the pickup condition held for this
                                     * many consecutive ranging rounds (filters out noisy measurements) */
#define HIT_DISTANCE_MM 900     /* Classify(): closest pouch must be within this to count */
#define SELF_HIT_IF_SINGLE_POUCH 1 /* with exactly one pouch registered, Classify may hit the thrower
                                    * (normally "thrower hit itself" goes back to Ready). For bench
                                    * tests with two boards; has no effect with 2+ pouches. */

/* ---- Accelerometer (ball only) ------------------------------------------ */
#define IMPACT_MG 2500           /* a Hit: |a| at or above this (one Hit per crossing; it must fall below 3/4
                                  * of it before the next one). The classifier reports every one of them; the
                                  * ball's state machine decides whether it matters (only in Thrown). */
#define IMPACT_COOLDOWN_MS 300   /* a crossing this soon after a reported Hit is part of the same impact
                                  * (ringing, double bounce) and is not reported again. Kept short: a hit can
                                  * follow a throw within a second. */
#define ACCEL_STALL_MS 100       /* the sensor read keeps failing (no data, an error, all zeros) for this long
                                  * = sensor stalled; the ball re-initializes it (samples normally arrive
                                  * every few ms). A pause of the sampling thread itself does not count. */
#define ACCEL_RECOVER_RETRY_MS 250 /* while stalled, re-init again this often (the first re-init comes at once) */
#define ACCEL_SAMPLE_PERIOD_MS 5 /* 200 Hz polling of the sensor (sensor itself runs at 400 Hz) */

/* Accelerometer diagnostics (ball/src/accel_log.h, used only by the LOG_ACCEL_* categories below). */
#define ACCEL_FEATURES_LOG_MS 100 /* LOG_ACCEL_FEATURES: one FEAT line at most this often */
#define BURST_PRE_SAMPLES 100     /* LOG_ACCEL_BURST: |a| samples kept before a crossing (~0.5 s) ... */
#define BURST_POST_SAMPLES 40     /* ... and recorded after it (~0.2 s) */
#define ACCEL_RAW_CAPTURE_MS 10000 /* LOG_ACCEL_RAW: the RAM history of raw x/y/z samples holds this much time at the
                                    * polling period (2000 samples of 8 bytes = 16 KB). The real spacing is
                                    * 6-7 ms, so the capture is about 12 s long. */
#define ACCEL_RAW_LINE_MS 40      /* LOG_ACCEL_RAW: one dump line (<= 8 samples) this often, so the dump
                                   * of a full capture takes about as long as the capture itself and the
                                   * Bluetooth link (a few KB/s) is never flooded (ball/src/accel_log.h) */

/* -- Accelerometer classifier (ball/src/accel_class.c) --------------------------------------- *
 * Rest / Held / Spinning from the shape of |a| over a short window, plus the Hit event (IMPACT_MG and
 * IMPACT_COOLDOWN_MS above). Every number below was derived from the recordings in data/ and is
 * justified, with the measured distributions, in docs/Accelerometer.md ("Evidence"); the tests in
 * tests/accel_class_test.c lock the behaviour. They assume samples 4-8 ms apart (the ball polls every
 * ACCEL_SAMPLE_PERIOD_MS plus read time: 5.8-7.5 ms measured); at 400 Hz decimate by 2 first.
 * Window features (mg): mean, MASD = mean absolute difference of consecutive samples (roughness:
 * sensor noise adds 15-50, a hand adds 50-900), TREND = |mean of the newer half - mean of the older
 * half| of the window (slow drift; averaging suppresses noise). */
#define ACCEL_WINDOW_MS 100      /* features are taken over this long: ~15 samples. Long enough to average
                                  * sensor noise, short enough for the 3.5-4 Hz drift of a spinning ball (150548
                                  * #3, #8: period 250-290 ms) to show as a trend */
#define ACCEL_FAST_WINDOW_MS 50  /* a smooth stretch far from 1 g is judged over this shorter window, so a
                                  * release (|a| falls to ~0 within 60-100 ms) is seen ~50 ms earlier */
#define ACCEL_MIN_SAMPLES 4      /* fewer samples in a window = no judgement (right after a reset). Must be >= 2 */
#define ACCEL_G_MIN_MG 600       /* lower end of "about 1 g" (window mean). Lying still: window means p10-p90 878-992 mg
                                  * on five stretches. Free fall is 47-162 mg (p10-p90), far below */
#define ACCEL_G_MAX_MG 1500      /* upper end of "about 1 g". Smooth plateaus seen after impacts: 770, 1260, 3800, 6400,
                                  * 7700 mg. Wide on purpose, so that ordinary hand motion (0.6-1.5 g) never counts
                                  * as "smooth and far from 1 g" flight evidence; the 770 and 1260 mg plateaus
                                  * rely on ACCEL_AFTER_HIT_MS instead */
#define ACCEL_CALM_TREND_MG 60   /* CALM (rest-like) needs a window mean near 1 g, TREND <= this ...
                                  * Lying still: trend p90 18-51 mg on the five stretches (noisiest board = 51) */
#define ACCEL_CALM_MASD_MG 60    /* ... and MASD <= this. Lying still: MASD p90 23-54 mg (noisiest board = 54) */
#define ACCEL_DRIFT_TREND_MG 100 /* DRIFT (smooth near 1 g, moving) needs TREND >= this ...
                                  * A ball spinning in flight at ~1 g (150548 #3, #8): trend p50 100-139 mg. A hand
                                  * gives the same trend (p50 58-88 mg), so only the duration separates them
                                  * (ACCEL_SPIN_DRIFT_MS) */
#define ACCEL_DRIFT_MASD_MG 40   /* ... and MASD <= this. Flights at ~1 g: MASD p50 22-25, p90 29-38 mg; hand
                                  * motion p50 33-49 mg */
#define ACCEL_ROUGH_MASD_MG 80   /* ROUGH: MASD above this. A hand shaking the ball: MASD p10 118, p50 261 mg. Smooth
                                  * flight, flat plateaus and free fall: MASD p99 16-44 mg (one window of 110) */
#define ACCEL_REST_MS 1500       /* Held/Spinning -> Rest: CALM for this long (settled). Lying-still stretches
                                  * (4 raw stretches) are CALM for 2.2-14 s, shaking never. A hand holding the ball
                                  * still looks the same as the ground: longer = fewer false Rest in a hand,
                                  * later Rest on the ground */
#define ACCEL_REST_GRACE_MS 150  /* a non-CALM stretch shorter than this does not restart the Rest timer. On the four
                                  * lying-still stretches 0-7% of the windows are not CALM (noise spikes) and the
                                  * longest such run is 95-100 ms */
#define ACCEL_LEAVE_REST_MS 250  /* Rest -> Held: not CALM for this long. Longer than the 100 ms noise runs above
                                  * (and than ACCEL_REST_GRACE_MS), short enough to follow a pick-up quickly */
#define ACCEL_SPIN_MS 45         /* flight evidence (smooth far from 1 g, or smooth right after an impact) must
                                  * last this long for Spinning. The longest such run in a hand-shaken ball
                                  * (134755, 5.3-9.9 s, ~23 shakes) is 40 ms: 30-40 here called it Spinning for 2 s.
                                  * A thin margin: flights give 100+ ms (docs/Accelerometer.md). With the 50 ms
                                  * window this is 95 ms of smoothness: 67-84 ms from |a| < 300 mg to Spinning
                                  * on the two recorded releases; each +5 ms here adds about as much */
#define ACCEL_SPIN_DRIFT_MS 350  /* Held -> Spinning without free fall or an impact (spin drift near 1 g): DRIFT
                                  * or SMOOTH windows for this long. The hand-swung ball of 150326 #1 (state Ready,
                                  * swing) has such windows for 275-300 ms: 250 and 275 ms called it Spinning, 300 does
                                  * not; 350 keeps 50 ms of margin. The price: a drift flight at ~1 g (150548 #3, #8)
                                  * is recognized only after 350 ms of drift */
#define ACCEL_SPIN_GRACE_MS 100  /* a gap shorter than this does not restart that timer (drift flights alternate
                                  * between DRIFT and CALM windows at the turning points of the oscillation) */
#define ACCEL_SPIN_HOLD_MS 500   /* Spinning -> Held when this long passes without any sign of life (a smooth or
                                  * calm window, or an impact). After a Hit the ball rings rough until the first
                                  * smooth window: 185 ms median, 415 ms p90, 926 ms max over the 68 impacts of the
                                  * raw recordings, so 94% are covered; the rest fall back to Held and are
                                  * Spinning again once the ball is smooth far from 1 g (or after 350 ms of drift) */
#define ACCEL_AFTER_HIT_MS 500   /* Spinning is "reachable right after a Hit": within this long after an impact
                                  * any smooth stretch (not only one far from 1 g) counts as flight evidence */
#define ACCEL_MIN_DWELL_MS 50    /* no level lasts shorter than this (flicker guard, whatever the evidence) */
#define ACCEL_SETTLE_MS 500      /* no Hit is reported this long after accel_class_reset() (the sensor settles
                                  * after the re-init; value of the old SETTLE_MS in impact.c) */
#define ACCEL_MG_MAX 65535       /* samples are clamped to 0..this. The sensor saturates at about 41400 mg (+-23.9 g
                                  * per axis), BURST logs clip at 32767; this only keeps the sums far from overflow */

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

/* ---- Ball state machine (Roll Call / Unheld / Ready / Thrown / Hit / Reset) ------ */
#define RANGE_ATTEMPTS 3         /* Disarm() and Classify(): ranging rounds tried if nobody answers */
#define HIT_ACK_TIMEOUT_MS 1000  /* Hit: give up telling a pouch about a hit after this long */
#define RESET_WINDOW_MS 3000     /* Reset: how long the ball listens for JOINs (and re-sends RESET). Must leave a late
                                  * pouch time to hear a RESET, wait its random REJOIN delay and get a JOIN through */
#define RESET_RETRY_MS 200       /* Reset: the ball re-sends RESET this often until every pouch acknowledged */
#define RESET_KEEPALIVE_MS 500   /* Reset: ... and this often afterwards, so a late pouch that missed one RESET
                                  * (it only reacts to RESET while Listening) gets another chance */
#define BUTTON_HOLD_MS 3000      /* a button press this long is a Reset (a shorter one is a "short press") */

#define THROW_RANGE_MM (2 * PICKUP_DISTANCE_MM) /* Ready -> Thrown: the THROWER is farther than this (and
                                                 * the ball is Spinning). Derived from the pickup range: a
                                                 * throw must travel at least this far before it lands. */
#define THROW_TIMEOUT_MS 3500    /* Thrown -> Disarm(): no hit for this long */
#define THROWN_RETURN_MS 1000    /* Thrown -> Ready: the closest alive pouch stayed within THROW_RANGE_MM this
                                  * long without a hit (throw cancelled, or the ball ended up near someone) */
#define THROWN_REST_MS 1000      /* Thrown -> Disarm(): the accelerometer reads Rest this long (the ball landed
                                  * and lies still) - no need to wait out THROW_TIMEOUT_MS */
#define THROWN_RECHECK_MS 250    /* Thrown: one ranging round this often, otherwise the radio is idle */
#define READY_ORPHAN_MS 1000     /* Ready -> Unheld: no alive pouch within THROW_RANGE_MM for this long */
#define READY_NOT_HELD_MS 5000   /* Ready -> Unheld: accelerometer not Held for this long */
#define THROWER_SWITCH_MS 1000   /* Ready: another alive pouch must be closer than the thrower this long
                                  * to become the new thrower. Only evidence breaks the streak (the pouch was
                                  * heard and is not closer): a pouch is missing from ~8% of the rounds when it
                                  * is within 300 mm, and hand-overs happen at that distance. */
#define THROWER_SWITCH_GRACE_MS 300 /* ... or it was not heard at all for this long */
#define THROWER_SILENT_MS 100    /* Ready: a thrower that stops answering (last range > TOO_CLOSE_MM) counts as
                                  * beyond THROW_RANGE_MM only after this long; a shorter silence keeps the last
                                  * range, so one dropped round while the ball spins cannot start Thrown */
#define CLASSIFY_ROUNDS 3        /* Classify(): ranging rounds merged per pouch (a pouch heard in any round
                                  * counts, at its smallest distance): the pouch that was just hit is very
                                  * close and often silent in a single round */

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
