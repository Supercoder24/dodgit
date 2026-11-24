"""
Scenario 7 - the table is full: 8 pouches fit, the 9th is ignored and does not restart the Roll Call timeout.
"""
import re
import statistics

from simlib import BALL, CFG, LED_GREEN, scenario
from helpers import check_air_clean, check_consistent, check_remembers_heard

RING = {k: (2.5 * __import__("math").cos(k * 0.785), 2.5 * __import__("math").sin(k * 0.785)) for k in range(1, 10)}


@scenario("table_full_ninth_ignored")
def table_full(w):
    w.place(BALL, 0.0, 0.0)
    w.power_on(BALL, 0.0)
    for k in range(1, 9):
        w.place(k, *RING[k])
        w.power_on(k, 0.3 + 0.4 * k)
    w.place(9, *RING[9])
    t9 = 6.0
    w.power_on(9, t9)
    ok = w.until(lambda: len(w.ball_table()) == 8, 15.0)
    w.check(ok is not None, "8 pouches did not register: %d" % len(w.ball_table()), nodes=[BALL])
    regs = w.grep(BALL, r"ROLLCALL new pouch")
    w.check_eq(len(regs), 8, "ROLLCALL new pouch lines")
    t_last = regs[-1].t
    w.run_until(t_last + CFG.ROLLCALL_TIMEOUT_MS / 1000.0 + 1.5)
    w.check_eq(sorted(w.ball_table()), list(range(8)), "slots of the 8 pouches")
    ign = w.grep(BALL, r"MEMBER join uid=0x%08x ignored: all 8 timeslots taken" % w.uid(9))
    w.check(len(ign) >= 5, "the ball should have logged the ninth pouch's JOINs as ignored (%d lines)" % len(ign), nodes=[BALL, 9])
    w.check(not w.grep(BALL, r"uid=0x%08x -> slot" % w.uid(9)), "the ninth pouch got a slot", nodes=[BALL, 9])
    to = w.grep(BALL, r"ROLLCALL timeout, 8 pouch")
    w.check_eq(len(to), 1, "ROLLCALL timeout lines")
    dt = to[0].t - t_last
    w.check(abs(dt - CFG.ROLLCALL_TIMEOUT_MS / 1000.0) < 0.3,
            "Roll Call ended %.2f s after the last NEW pouch, expected %d s (the ninth pouch's JOINs must not restart the timeout)" %
            (dt, CFG.ROLLCALL_TIMEOUT_MS // 1000), nodes=[BALL, 9])
    # the ninth pouch is Joining until the ball ranges (then Listening); it never transmits again once Listening
    w.run_for(2.0)
    w.check_eq(w.phase(9), "Listening", "the ninth pouch after the game started")
    last_join = max([f.t for f in w.frames("JOIN", src=9)] or [0])
    w.check(last_join < to[0].t + 0.5, "the ninth pouch keeps sending JOINs after the game started")
    # the game with 8 pouches
    for k in range(1, 9):
        w.check_eq(w.phase(k), "Active", "pouch %d after the game started" % k)
    check_consistent(w, range(1, 9), "with 8 pouches")
    w.run_for(3.0)
    rounds = [r for r in w.ranges(t0=to[0].t + 1.0) if r["ret"] > 0]
    full = sum(1 for r in rounds if len(r["by_slot"]) == 8)
    w.note("%d ranging rounds with 8 pouches, %d heard all of them" % (len(rounds), full))
    w.check(len(rounds) > 40 and full >= 0.9 * len(rounds), "only %d of %d rounds heard all 8 pouches" % (full, len(rounds)))
    for k in (1, 4, 8):
        true_mm = w.dist(BALL, k) * 1000
        vals = [r["by_slot"][w.ball_slot_of(k)][1] for r in rounds if w.ball_slot_of(k) in r["by_slot"]]
        w.check(abs(statistics.median(vals) - true_mm) <= 15, "pouch %d range %.0f mm, true %.0f mm" % (k, statistics.median(vals), true_mm))
    check_air_clean(w, to[0].t + 1.0, w.now, "ranging with 8 pouches")
