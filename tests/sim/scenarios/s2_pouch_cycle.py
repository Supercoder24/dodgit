"""
Scenario 2 - a pouch is switched off and on again in the middle of a game: it comes back with the SAME slot and
lives (invariant 1), without a hit effect, and without disturbing the ranging of the others.
"""
from simlib import ACTIVE, BALL, CFG, LED_ALL, LED_BLUE, LED_GREEN, LED_NONE, LED_RED, scenario
from helpers import (BOOT_S, check_air_clean, check_consistent, check_remembers_heard, hit_pouch, hit_effect_plays,
                     start_game)


def expect_lives_pattern(w, node, lives, t0, t1, what):
    masks = w.led_masks(node, t0, t1)
    want = {3: {LED_GREEN}, 2: {LED_BLUE}, 1: {LED_RED, LED_NONE}, 0: {LED_RED}}[lives]
    w.check(masks == want, "%s: LEDs of the pouch with %d lives over [%.2f, %.2f]: %r, expected %r" %
            (what, lives, t0, t1, sorted(masks), sorted(want)), nodes=[BALL, node])


def pouch_cycle(w, hits, off_for=0.4):
    start_game(w, 3)
    target = 2
    if hits:
        hit_pouch(w, 1, target, hits)
    lives = CFG.LIVES_MAX - hits
    slot = w.slot(target)
    w.check_eq(w.lives(target), lives, "lives before the power cycle")
    w.check_eq(w.ball_lives(target), lives, "the ball's lives before the power cycle")
    table_before = w.ball_table()
    t_off = w.now + 0.5
    w.reboot(target, t_off, off_for=off_for)
    t_on = t_off + off_for
    w.run_until(t_off + 0.2)
    w.check(not w.powered(target), "the pouch is still on")
    w.check_eq(w.ball_table(), table_before, "the ball forgot the pouch while it was off")
    t = w.until(lambda: w.phase(target) == "Active" and w.slot(target) == slot, 5.0)
    w.check(t is not None, "the rebooted pouch is not Active again after 5 s (pouch: %r, ball table %r)" %
            (w.pouch(target), w.ball_table()), nodes=[BALL, target])
    took = t - t_on
    w.note("lives=%d: pouch Active again %.2f s after it was switched on (slot %d)" % (lives, took, slot))
    w.check(took <= BOOT_S + CFG.REMEMBER_PERIOD_MS / 1000.0 + 0.5,
            "the pouch needed %.2f s to come back (boot %.2f s + one REMEMBER period %.2f s expected)" %
            (took, BOOT_S, CFG.REMEMBER_PERIOD_MS / 1000.0), nodes=[BALL, target])
    w.check_eq(w.pouch(target), ("Active", slot, lives), "pouch after the power cycle")
    w.check_eq(w.ball_table(), table_before, "the ball's table after the power cycle")
    w.run_for(2.0)
    check_consistent(w, (1, 2, 3), "after the power cycle")
    # no hit effect, no HIT line: the rebooted pouch was told its lives, nothing happened to it
    w.check_eq(hit_effect_plays(w, target, t_on, w.now), 0, "hit effects shown after the reboot")
    w.check(not w.grep(target, r"^HIT,", t0=t_on), "the rebooted pouch logged a hit")
    expect_lives_pattern(w, target, lives, w.now - 1.2, w.now, "after the reboot")
    # ranging is clean: no overlapping frames (a stale slot would make two pouches answer at once)
    check_air_clean(w, t_off - 1.0, w.now, "around the power cycle")
    rounds = [r for r in w.ranges(t0=t_on + 0.5) if r["ret"] > 0]
    w.check(len(rounds) > 20, "the ball stopped ranging")
    slot_t = w.ball_slot_of(target)
    after = [r for r in w.ranges(t0=t - 0.1) if r["ret"] > 0]
    w.check(all(slot_t in r["by_slot"] or r["t"] < t + 0.05 for r in after) or
            sum(slot_t in r["by_slot"] for r in after) >= 0.95 * len(after),
            "after coming back the pouch is missing from ranging rounds")


for _hits in (0, 1, 2, 3):
    scenario("pouch_cycle_lives_%d" % (3 - _hits))((lambda h: lambda w: pouch_cycle(w, h))(_hits))


@scenario("pouch_cycle_two_at_once")
def two_at_once(w):
    start_game(w, 3)
    hit_pouch(w, 1, 2, 1)
    hit_pouch(w, 1, 3, 2)
    before = w.ball_table()
    slots = {k: w.slot(k) for k in (1, 2, 3)}
    lives = {k: w.lives(k) for k in (1, 2, 3)}
    w.check_eq(sorted(l for _, l in before.values()), [1, 2, 3], "lives before")
    t_off = w.now + 0.3
    w.reboot(2, t_off, 0.5)
    w.reboot(3, t_off, 0.5)
    t = w.until(lambda: w.booted(2) == 2 and w.booted(3) == 2 and all(w.phase(k) == "Active" for k in (1, 2, 3)),
                10.0)
    w.check(t is not None, "not everybody is Active again: %r" % {k: w.pouch(k) for k in (1, 2, 3)}, nodes=[BALL, 1, 2, 3])
    w.run_for(1.0)
    w.check_eq({k: w.slot(k) for k in (1, 2, 3)}, slots, "slots after two pouches were switched on together")
    w.check_eq({k: w.lives(k) for k in (1, 2, 3)}, lives, "lives after two pouches were switched on together")
    w.check_eq(w.ball_table(), before, "ball table")
    check_consistent(w, (1, 2, 3), "after two reboots")
    check_air_clean(w, t_off, w.now, "two pouches rejoining")


@scenario("pouch_away_long")
def away_long(w):
    """A pouch stays off for a minute: the ball keeps its slot (nothing deletes it outside a Reset) and the others
    are ranged as before; when it comes back it has its slot and lives again."""
    start_game(w, 3)
    hit_pouch(w, 1, 3, 1)
    before = w.ball_table()
    w.power_off(2, w.now + 0.2)
    w.run_for(60.0)
    w.check_eq(w.ball_table(), before, "table after 60 s without pouch 2")
    ok = [r for r in w.ranges(t0=w.now - 5.0) if r["ret"] > 0]
    s1, s3 = w.ball_slot_of(1), w.ball_slot_of(3)
    w.check(len(ok) > 100 and all(s1 in r["by_slot"] and s3 in r["by_slot"] for r in ok),
            "the other pouches are not ranged every round while pouch 2 is away")
    w.power_on(2, w.now + 0.1)
    t = w.until(lambda: w.phase(2) == "Active", 6.0)
    w.check(t is not None, "pouch 2 did not come back after 60 s: %r" % (w.pouch(2),), nodes=[BALL, 2])
    w.check_eq(w.ball_table(), before, "table after pouch 2 came back")
    check_consistent(w, (1, 2, 3), "after the long absence")


@scenario("pouch_cycle_single_pouch_game")
def single_pouch(w):
    """The 2-board bench setup: ONE pouch in the game. While it is switched off no round is answered, and when it is back
    (Listening, silent) still none is, so the ball holds every REMEMBER back (it thinks the pouches wait for a FINAL) and
    the pouch never gets its slot back."""
    start_game(w, 1)
    slot, lives = w.slot(1), w.lives(1)
    t_off = w.now + 0.5
    w.reboot(1, t_off, off_for=0.4)
    t = w.until(lambda: w.booted(1) == 2 and w.phase(1) == "Active", 8.0)
    w.check(t is not None, "the only pouch of the game never came back after a power cycle: %r, REMEMBER frames sent since: %d" %
            (w.pouch(1), len(w.frames("REMEMBER", t0=t_off + 0.4))), nodes=[BALL, 1], t0=t_off, air=True, types=("REMEMBER", "POLL"), last=30)
    w.check_eq((w.slot(1), w.lives(1)), (slot, lives), "slot and lives after the power cycle")


@scenario("pouch_cycle_all_pouches_at_once")
def all_at_once(w):
    """Every pouch of the game is switched off and on together (e.g. the players' pouches all lose power): nobody answers,
    so no REMEMBER is ever sent."""
    start_game(w, 3)
    hit_pouch(w, 1, 2, 1)
    before = w.ball_table()
    slots = {k: w.slot(k) for k in (1, 2, 3)}
    t_off = w.now + 0.3
    for k in (1, 2, 3):
        w.reboot(k, t_off, 0.5)
    t = w.until(lambda: all(w.booted(k) == 2 and w.phase(k) == "Active" for k in (1, 2, 3)), 10.0)
    w.check(t is not None, "the pouches did not come back: %r" % {k: w.pouch(k) for k in (1, 2, 3)}, nodes=[BALL, 1, 2, 3],
            t0=t_off, air=True, types=("REMEMBER",), last=30)
    w.check_eq({k: w.slot(k) for k in (1, 2, 3)}, slots, "slots")
    w.check_eq(w.ball_table(), before, "table")
