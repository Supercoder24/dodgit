"""
Scenario 1 - normal start: ball + 3 pouches switched on in different orders; Roll Call ends by short press or by
the timeout; everybody is registered with a distinct slot and ranging reports the real distances.
"""
import statistics

from simlib import ACTIVE, BALL, CFG, LED_BLUE, LED_GREEN, LED_NONE, scenario
from helpers import check_air_clean, check_consistent, check_remembers_heard, power_times, start_game

# distances ball - pouch: 0.50 m, 0.90 m, 2.12 m
POS = {1: (0.5, 0.0), 2: (0.0, 0.9), 3: (1.5, 1.5)}


def normal_start(w, order, leave):
    left = start_game(w, 3, positions=POS, ball_pos=(0.0, 0.0), leave=leave, order=order)
    pouches = (1, 2, 3)

    # everybody registered, distinct slots 0..2, the pouches agree with the ball's table
    tbl = w.ball_table()
    w.check_eq(sorted(tbl), [0, 1, 2], "slots in the ball's table")
    w.check_eq(sorted(u for u, _ in tbl.values()), sorted(w.uid(k) for k in pouches), "uids in the table")
    w.check(all(l == CFG.LIVES_MAX for _, l in tbl.values()), "all pouches start with full lives: %r" % tbl)
    check_consistent(w, pouches, "after Roll Call")
    for k in pouches:
        w.check_eq(w.phase(k), "Active", "pouch %d phase after the first POLL" % k)

    # first come, first served: the slot numbers follow the order of the "new pouch" lines
    regs = w.grep(BALL, r"ROLLCALL new pouch uid=0x([0-9a-f]+) -> slot (\d+)")
    w.check_eq(len(regs), 3, "ROLLCALL new pouch lines")
    import re
    for i, ln in enumerate(regs):
        m = re.search(r"slot (\d+)", ln.text)
        w.check_eq(int(m.group(1)), i, "slot of registration #%d" % i)

    # how Roll Call ended
    if leave == "press":
        w.check(w.grep(BALL, r"Button: short press, ending Roll Call"), "no short-press line in the ball's log")
    else:
        t_last = regs[-1].t
        to = w.grep(BALL, r"ROLLCALL timeout, 3 pouch")
        w.check(len(to) == 1, "expected one ROLLCALL timeout line, got %d" % len(to))
        dt = to[0].t - t_last
        w.check(abs(dt - CFG.ROLLCALL_TIMEOUT_MS / 1000.0) < 0.3,
                "Roll Call ended %.2f s after the last new pouch, expected %d s" % (dt, CFG.ROLLCALL_TIMEOUT_MS // 1000))
    w.check_eq(w.ball_path(), ["Roll Call", "Unheld"], "ball states (a resting ball is Unheld, not Ready)")

    # LEDs: ball Roll Call = blue (green flash per new pouch), Unheld = green; pouches: Joined blue blink, Active green
    w.check_eq(w.led_masks(BALL, regs[0].t, left - 0.05) <= {LED_BLUE, LED_GREEN}, True,
               "ball LEDs in Roll Call: %r" % sorted(w.led_masks(BALL, regs[0].t, left - 0.05)))
    w.check_eq(w.led_at(BALL, w.now), LED_GREEN, "ball LED in Unheld")
    for k in pouches:
        w.check_eq(w.led_at(k, w.now), LED_GREEN, "pouch %d LED with full lives (green steady)" % k)

    # ranging: the measured distances are the placed ones (DS-TWR math, clock offsets, 32-bit wrap)
    w.run_for(2.0)
    rounds = [r for r in w.ranges(t0=left + 0.8) if r["ret"] > 0]
    w.check(len(rounds) >= 60, "only %d ranging rounds in the Unheld phase" % len(rounds))
    for k in pouches:
        slot = w.ball_slot_of(k)
        true_mm = w.dist(BALL, k) * 1000
        vals = [r["by_slot"][slot][1] for r in rounds if slot in r["by_slot"]]
        w.check(len(vals) >= 0.95 * len(rounds), "pouch %d (slot %d) answered only %d of %d rounds" % (k, slot, len(vals), len(rounds)))
        med = statistics.median(vals)
        dev = max(abs(v - true_mm) for v in vals)
        w.note("pouch %d: true %.0f mm, median %.0f mm, worst error %.0f mm over %d rounds" % (k, true_mm, med, dev, len(vals)))
        w.check(abs(med - true_mm) <= 15, "pouch %d: median range %.0f mm, true distance %.0f mm" % (k, med, true_mm))
        w.check(dev <= 50, "pouch %d: worst range error %.0f mm (true %.0f mm)" % (k, dev, true_mm))
    check_air_clean(w, left, w.now, "ranging in Unheld")
    check_remembers_heard(w, left, w.now, "REMEMBER during ranging")


for _order in ("ball_first", "pouches_first", "interleaved", "together"):
    for _leave in ("press", "timeout"):
        scenario("start_%s_%s" % (_order, _leave))(
            (lambda o, l: lambda w: normal_start(w, o, l))(_order, _leave))
