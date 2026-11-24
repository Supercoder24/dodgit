"""
Scenario 3 - the ball is switched off and on again in the middle of a game. Its table is gone (RAM only), so
the pouches fall back to Joining after LOST_TIMEOUT_MS and register again in the new Roll Call. The two orders
(pouches keep running / pouches off meanwhile) work and leave consistent slots.
"""
import re

from simlib import BALL, CFG, LED_BLUE, LED_GREEN, LED_NONE, scenario
from helpers import check_air_clean, check_consistent, hit_pouch, start_game


def finish_and_check(w, pouches, t_from):
    """Short press, then ranging with all pouches under their new slots."""
    n = len(pouches)
    w.check_eq(len(w.ball_table()), n, "pouches in the new table")
    check_consistent(w, pouches, "in the new Roll Call")
    w.check_eq(sorted(w.ball_table()), list(range(n)), "slots of the new game")
    w.check(all(l == CFG.LIVES_MAX for _, l in w.ball_table().values()), "a new game starts with full lives: %r" % w.ball_table())
    w.short_press(w.now + 0.3)
    w.until(lambda: w.ball_state() != "Roll Call", 5.0)
    w.check(w.ball_state() in ("Unheld", "Ready"), "Roll Call did not end: %s" % w.ball_state())
    w.run_for(3.0)
    for k in pouches:
        w.check_eq(w.phase(k), "Active", "pouch %d after the new game started" % k)
        w.check_eq(w.lives(k), CFG.LIVES_MAX, "lives of pouch %d" % k)
    check_consistent(w, pouches, "in the new game")
    t_game = [t for t, _, to in w.ball_transitions() if to in ("Unheld", "Ready") and t > t_from][0]
    rounds = [r for r in w.ranges(t0=t_game + 0.5) if r["ret"] > 0]
    w.check(len(rounds) > 50, "the ball is not ranging in the new game")
    full = sum(1 for r in rounds if len(r["by_slot"]) == n)
    w.check(full >= 0.95 * len(rounds), "only %d of %d rounds heard all %d pouches (a stale slot?)" % (full, len(rounds), n))
    check_air_clean(w, t_game, w.now, "ranging after the ball reboot")


@scenario("ball_reboot_pouches_running")
def pouches_running(w):
    start_game(w, 3)
    hit_pouch(w, 1, 3, 1)  # lives differ: 3, 3, 2
    t_off = w.now + 0.5
    w.reboot(BALL, t_off, off_for=0.4)
    t_back = t_off + 0.4
    w.run_until(t_off + 0.2)
    pouch_slots = {k: w.slot(k) for k in (1, 2, 3)}
    w.check(all(w.phase(k) == "Active" for k in (1, 2, 3)), "pouches should still be Active right after the ball died")
    # they notice after LOST_TIMEOUT_MS (counted from the last REMEMBER, at most one REMEMBER period older than the power-off)
    w.run_until(t_off + CFG.LOST_TIMEOUT_MS / 1000.0 + 1.5)
    lost = CFG.LOST_TIMEOUT_MS / 1000.0
    for k in (1, 2, 3):
        ln = w.grep(k, r"MEMBER phase (Active|Joined) -> Joining", t0=t_off)
        w.check(len(ln) == 1, "pouch %d: expected one fall back to Joining, got %d" % (k, len(ln)), nodes=[BALL, k])
        dt = ln[0].t - t_off
        w.note("pouch %d fell back to Joining %.2f s after the ball died (LOST_TIMEOUT_MS = %.1f s)" % (k, dt, lost))
        w.check(lost - CFG.REMEMBER_PERIOD_MS / 1000.0 - 0.2 <= dt <= lost + 0.4,
                "pouch %d fell back to Joining %.2f s after the ball died, expected %.1f..%.1f s" %
                (k, dt, lost - CFG.REMEMBER_PERIOD_MS / 1000.0, lost), nodes=[BALL, k])
    t = w.until(lambda: len(w.ball_table()) == 3 and all(w.phase(k) == "Joined" for k in (1, 2, 3)), 10.0)
    w.check(t is not None, "the pouches did not re-register in the new Roll Call: table %r, pouches %r" %
            (w.ball_table(), {k: w.pouch(k) for k in (1, 2, 3)}), nodes=[BALL, 1, 2, 3])
    w.check_eq(w.ball_state(), "Roll Call", "ball state while the pouches re-register")
    finish_and_check(w, (1, 2, 3), t_back)


@scenario("ball_reboot_pouches_off")
def pouches_off(w):
    start_game(w, 3)
    hit_pouch(w, 1, 3, 1)
    t_off = w.now + 0.5
    for k in (1, 2, 3):
        w.power_off(k, t_off - 0.1)
    w.reboot(BALL, t_off, off_for=0.4)
    for i, k in enumerate((3, 1, 2)):  # another order than the first time
        w.power_on(k, t_off + 1.6 + 0.5 * i)
    t = w.until(lambda: len(w.ball_table()) == 3 and all(w.booted(k) == 2 and w.phase(k) == "Joined" for k in (1, 2, 3)), 15.0)
    w.check(t is not None, "the pouches did not join the new Roll Call: table %r" % (w.ball_table(),), nodes=[BALL, 1, 2, 3])
    # the order of the new slots follows the switch-on order, whatever the old slots were
    w.check_eq([w.ball_slot_of(k) for k in (3, 1, 2)], [0, 1, 2], "slots follow the new switch-on order")
    finish_and_check(w, (1, 2, 3), t_off)


@scenario("ball_reboot_quick_press_ignored")
def quick_press(w):
    """Right after the reboot nobody is registered: a short press does nothing (a red flash), the game does not
    start without pouches, and the stale pouches get into the new game later."""
    start_game(w, 2)
    t_off = w.now + 0.5
    w.reboot(BALL, t_off, off_for=0.3)
    w.short_press(t_off + 0.3 + 1.5)
    w.run_until(t_off + 0.3 + 3.0)
    w.check(w.grep(BALL, r"short press ignored, no pouch has registered yet"), "the early press was not ignored")
    w.check_eq(w.ball_state(), "Roll Call", "ball state after the ignored press")
    t = w.until(lambda: len(w.ball_table()) == 2, 12.0)
    w.check(t is not None, "the pouches did not join the new Roll Call", nodes=[BALL, 1, 2])
    finish_and_check(w, (1, 2), t_off)
