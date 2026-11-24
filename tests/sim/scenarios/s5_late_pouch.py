"""
Scenario 5 - late pouches: the game is running, a new pouch is switched on and ends up Listening (silent). A 3 s
button hold is the Reset: all lives back to the maximum, the late pouches join, Roll Call follows, a short press
resumes the game.
"""
from simlib import BALL, CFG, LED_ALL, LED_GREEN, LED_NONE, scenario
from helpers import (COURT, BOOT_S, WINDOW, begin_reset, check_air_clean, check_consistent, hit_pouch,
                     start_game)

def late_setup(w, n_known, n_late, hits=1):
    """A running game with n_known pouches (pouch 2 hit `hits` times) and n_late pouches switched on afterwards."""
    start_game(w, n_known)
    if hits:
        hit_pouch(w, 1, 2, hits)
    for i in range(n_late):
        k = n_known + 1 + i
        w.place(k, *COURT[k])
        w.power_on(k, w.now + 0.2 + 0.1 * i)
    late = [n_known + 1 + i for i in range(n_late)]
    t = w.until(lambda: all(w.booted(k) == 1 and w.phase(k) == "Listening" for k in late), 6.0)
    w.check(t is not None, "late pouches did not end up Listening: %r" % {k: w.pouch(k) for k in late}, nodes=[BALL] + late)
    # silent: they send nothing while Listening (JOINs before the first POLL are fine: count them afterwards)
    t_l = w.now
    w.run_for(3.0)
    for k in late:
        w.check(not w.frames("JOIN", t0=t_l, src=k), "pouch %d (Listening) is transmitting" % k)
        w.check_eq(w.led_masks(k, t_l, t_l + 2.0) <= {0, 8}, True, "LED of a Listening pouch (a blue flash every 2 s)")
    w.check_eq(len(w.ball_table()), n_known, "the late pouches must not be in the table before a Reset")
    return late


def after_reset_checks(w, everybody, n_new, t_reset, t_end, summary):
    n = len(everybody)
    w.check("%d joined" % n_new in summary and "0 deleted" in summary, "Reset summary: %r" % summary, nodes=[BALL])
    w.check_eq(w.ball_state(), "Roll Call", "ball state after a Reset in which somebody joined")
    w.check_eq(len(w.ball_table()), n, "pouches in the table after the Reset")
    w.check(all(l == CFG.LIVES_MAX for _, l in w.ball_table().values()), "Reset must restore every life: %r" % w.ball_table())
    w.check_eq(sorted(w.ball_table()), list(range(n)), "slots after the Reset")
    for k in everybody:
        w.check(w.phase(k) in ("Joined", "Active"), "pouch %d did not join: %r" % (k, w.pouch(k)), nodes=[BALL, k])
        w.check_eq(w.lives(k), CFG.LIVES_MAX, "lives of pouch %d after the Reset" % k)
    check_consistent(w, everybody, "after the Reset")
    # the known pouches showed the reset effect once (each RESET is acknowledged, only the first one is shown)
    for k in everybody:
        if k in everybody[: n - n_new]:
            lines = w.grep(k, r"^RESET, lives", t0=t_reset - 0.1, t1=t_end + 0.5)
            w.check_eq(len(lines), 1, "'RESET, lives' lines logged by pouch %d during the Reset" % k)
    # Roll Call then a short press resumes the game
    w.short_press(w.now + 0.3)
    w.until(lambda: w.ball_state() != "Roll Call", 5.0)
    w.check(w.ball_state() in ("Unheld", "Ready"), "the short press did not end Roll Call: %s" % w.ball_state(), nodes=[BALL])
    w.run_for(2.0)
    for k in everybody:
        w.check_eq(w.pouch(k)[0], "Active", "pouch %d after the game resumed" % k)
        w.check_eq(w.led_at(k, w.now), LED_GREEN, "pouch %d LED (full lives)" % k)
    check_consistent(w, everybody, "after the game resumed")
    rounds = [r for r in w.ranges(t0=w.now - 1.5) if r["ret"] > 0]
    w.check(len(rounds) > 20 and all(len(r["by_slot"]) == n for r in rounds), "ranging rounds do not hear all %d pouches" % n)
    check_air_clean(w, w.now - 1.5, w.now, "ranging after the Reset")


@scenario("late_pouch_joins_at_reset")
def one_late(w):
    late = late_setup(w, 2, 1, hits=1)
    w.check_eq(w.ball_lives(2), 2, "pouch 2 was hit before the Reset")
    t0, t1, summary = begin_reset(w)
    w.note("Reset %.2f s long; %s" % (t1 - t0, summary))
    w.check(abs((t1 - t0) - WINDOW) < 0.15, "the Reset lasted %.2f s instead of RESET_WINDOW_MS = %.1f s" % (t1 - t0, WINDOW))
    after_reset_checks(w, [1, 2, 3], 1, t0, t1, summary)
    # the late pouch joined with the first RESET it heard + its random rejoin delay
    jn = w.grep(BALL, r"MEMBER join uid=0x%08x" % w.uid(3), t0=t0)
    w.check(len(jn) >= 1 and jn[0].t < t1, "the late pouch joined after the Reset window", nodes=[BALL, 3])


@scenario("late_pouches_three_at_once")
def three_late(w):
    late = late_setup(w, 2, 3, hits=2)
    t0, t1, summary = begin_reset(w)
    w.note("Reset %.2f s long; %s" % (t1 - t0, summary))
    after_reset_checks(w, [1, 2, 3, 4, 5], 3, t0, t1, summary)
    joins = [ln.t - t0 for ln in w.grep(BALL, r"MEMBER join .*\(new\)", t0=t0, t1=t1)]
    w.note("late pouches joined %s s after the start of the Reset" % ", ".join("%.2f" % j for j in joins))


@scenario("late_pouch_first_reset_lost")
def first_reset_lost(w):
    """(a) The very first RESET broadcast does not reach the late pouch: it only reacts to RESET, so the repeats
    (RESET_KEEPALIVE_MS) must get it into the game within the window."""
    late = late_setup(w, 2, 1, hits=1)
    t_press = w.now + 0.2
    rule = w.drop("RESET", src=BALL, dst=3, count=1, t0=t_press)
    w.long_press(t_press)
    t0 = w.run_until_log(BALL, "-> Reset", CFG.BUTTON_HOLD_MS / 1000.0 + 2.0)
    t1 = w.run_until_log(BALL, "Reset: ", WINDOW + 1.5)
    summary = w.grep(BALL, r"Reset: \d+ joined")[-1].text
    w.check_eq(w.dropped(rule), 1, "RESET frames dropped for the late pouch")
    resets = w.frames("RESET", t0=t0 - 0.1, t1=t1)
    w.note("%d RESET broadcasts in the window: at %s s" % (len(resets), ", ".join("%.2f" % (f.t - t0) for f in resets)))
    w.check(len(resets) >= 3, "RESET is not repeated for the late pouch (%d broadcasts)" % len(resets))
    after_reset_checks(w, [1, 2, 3], 1, t0, t1, summary)


@scenario("late_pouch_first_two_resets_lost")
def first_two_lost(w):
    late = late_setup(w, 2, 1, hits=0)
    t_press = w.now + 0.2
    rule = w.drop("RESET", src=BALL, dst=3, count=2, t0=t_press)
    w.long_press(t_press)
    t0 = w.run_until_log(BALL, "-> Reset", CFG.BUTTON_HOLD_MS / 1000.0 + 2.0)
    t1 = w.run_until_log(BALL, "Reset: ", WINDOW + 1.5)
    summary = w.grep(BALL, r"Reset: \d+ joined")[-1].text
    w.check_eq(w.dropped(rule), 2, "RESET frames dropped for the late pouch")
    after_reset_checks(w, [1, 2, 3], 1, t0, t1, summary)


@scenario("late_pouch_joins_in_last_200ms")
def join_in_last_200ms(w):
    """A pouch that joins in the last 200 ms of the Reset is not deleted (a JOIN counts as the acknowledgment)."""
    start_game(w, 2)
    hit_pouch(w, 1, 2, 1)
    w.place(3, *COURT[3])
    t_press = w.now + 0.2
    w.long_press(t_press)
    t0 = w.run_until_log(BALL, "-> Reset", CFG.BUTTON_HOLD_MS / 1000.0 + 2.0)
    # switch pouch 3 on so that its (forced) first JOIN comes WINDOW - 0.1 s into the Reset
    first_join_delay = 0.15  # forced: JOIN_RETRY_MIN_MS + 50
    w.force_random(3, [50])
    t_on = t0 + WINDOW - 0.10 - BOOT_S + 0.0 - first_join_delay + 0.0
    w.power_on(3, t_on)
    t1 = w.run_until_log(BALL, "Reset: ", WINDOW + 1.5)
    summary = w.grep(BALL, r"Reset: \d+ joined")[-1].text
    jn = w.grep(BALL, r"MEMBER join uid=0x%08x" % w.uid(3), t0=t0)
    w.check(len(jn) >= 1, "pouch 3 never sent a JOIN in the window", nodes=[BALL, 3], air=True, types=("JOIN",))
    left = t1 - jn[0].t
    w.note("pouch 3 joined %.0f ms before the end of the Reset; %s" % (left * 1000, summary))
    w.check(0.0 < left <= 0.25, "the late JOIN came %.0f ms before the end of the Reset, wanted <= 200 ms (the test setup drifted)" % (left * 1000))
    w.check("1 joined" in summary and "0 deleted" in summary and "3 pouch" in summary, "Reset summary: %r" % summary, nodes=[BALL, 3])
    w.check(not w.grep(BALL, r"MEMBER deleted"), "the pouch that joined last was deleted", nodes=[BALL, 3])
    after_reset_checks(w, [1, 2, 3], 1, t0, t1, summary)
