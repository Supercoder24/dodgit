"""
Scenario 4 - lost HIT / HIT_ACK / REMEMBER. The ball's table is authoritative: the pouch display converges to it
within one REMEMBER period, a repeated HIT never costs a second life, the hit effect is played once.
"""
from simlib import BALL, CFG, LED_ALL, LED_BLUE, LED_GREEN, LED_NONE, LED_RED, scenario
from helpers import (COURT, carry, check_air_clean, check_consistent, hit_effect_plays, start_game, throw)

THROWER, TARGET, OTHER = 1, 2, 3


def play_until_impact(w, setup=None):
    """Start a game, pick the ball up at the thrower, throw it at the target. `setup(t_impact)` may install drop
    rules before the impact. Returns the time of the impact (Hit event)."""
    start_game(w, 3)
    t = carry(w, w.now + 0.1, (COURT[THROWER][0] + 0.4, COURT[THROWER][1])) + 1.0
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "ball state before the throw")
    t_imp = throw(w, t, (COURT[TARGET][0] - 0.1, COURT[TARGET][1]), flight=0.7)
    if setup:
        setup(t_imp)
    return t_imp


def hit_lines(w, node, t0):
    return w.grep(node, r"^HIT, lives = ", t0=t0)


def common_checks(w, t_imp, expect_lives=2):
    """After the dust settled: ball and pouch agree, one life lost, the effect played once."""
    w.check_eq(w.ball_lives(TARGET), expect_lives, "the ball's lives of the hit pouch")
    w.check_eq(w.lives(TARGET), expect_lives, "the hit pouch's own lives")
    w.check_eq(w.ball_lives(THROWER), CFG.LIVES_MAX, "lives of the thrower")
    w.check_eq(w.ball_lives(OTHER), CFG.LIVES_MAX, "lives of the bystander")
    check_consistent(w, (1, 2, 3), "after the hit")
    w.check_eq(len(hit_lines(w, TARGET, t_imp - 0.1)), 1, "'HIT, lives' lines logged by the pouch")
    w.check_eq(hit_effect_plays(w, TARGET, t_imp - 0.1, w.now), 1, "hit effects shown by the pouch")
    w.check_eq(w.grep(BALL, r"Classify: HIT on slot", t0=t_imp - 0.1) and len(w.grep(BALL, r"Classify: HIT on slot", t0=t_imp - 0.1)), 1,
               "Classify decisions that hit")
    w.check_eq(w.led_at(TARGET, w.now), LED_BLUE, "LED of a pouch with 2 lives (blue, steady)")


@scenario("lost_hit_first_frame")
def lost_hit_first(w):
    rule = {}

    def setup(t_imp):
        rule["r"] = w.drop("HIT", src=BALL, dst=TARGET, count=1, t0=t_imp)

    t_imp = play_until_impact(w, setup)
    w.run_until(t_imp + 6.0)
    w.check_eq(w.dropped(rule["r"]), 1, "HIT frames dropped")
    w.check(w.grep(BALL, r"HIT_ACK slot"), "the ball never got the HIT_ACK (it should resend the HIT)")
    w.check(not w.grep(BALL, r"HIT_UNACKED"), "the ball gave up on the hit")
    common_checks(w, t_imp)
    resent = w.frames("HIT", t0=t_imp)
    w.check_eq(len(resent), 2, "HIT frames sent (one lost, one resent)")


@scenario("lost_hit_all_remember_fixes")
def lost_hit_all(w):
    rule = {}

    def setup(t_imp):
        rule["r"] = w.drop("HIT", src=BALL, dst=TARGET, count=-1, t0=t_imp - 0.1, t1=t_imp + 30)

    t_imp = play_until_impact(w, setup)
    w.run_until(t_imp + 3.0)
    t_cls = w.grep(BALL, r"Classify: HIT on slot")[0].t
    w.check(w.grep(BALL, r"HIT_UNACKED slot"), "the ball should give up (HIT_UNACKED) after %d ms" % CFG.HIT_ACK_TIMEOUT_MS)
    w.check(w.dropped(rule["r"]) >= 2, "the HIT was not even resent")
    # the table has the lost life at once; the pouch's display catches up with the next REMEMBER
    w.check_eq(w.ball_lives(TARGET), 2, "the ball's lives (counted at once)")
    ln = hit_lines(w, TARGET, t_imp - 0.1)
    w.check(len(ln) == 1, "the pouch should learn about the hit from a REMEMBER: HIT lines %d" % len(ln), nodes=[BALL, TARGET], air=True, types=("HIT", "REMEMBER"))
    lag = ln[0].t - t_cls
    w.note("pouch display caught up %.2f s after the hit was counted (REMEMBER_PERIOD_MS = %d)" % (lag, CFG.REMEMBER_PERIOD_MS))
    w.check(lag <= CFG.REMEMBER_PERIOD_MS / 1000.0 + 0.3, "the pouch learned of the hit only %.2f s late" % lag, nodes=[BALL, TARGET], air=True, types=("HIT", "REMEMBER"))
    common_checks(w, t_imp)


@scenario("lost_hit_ack")
def lost_hit_ack(w):
    rule = {}

    def setup(t_imp):
        rule["r"] = w.drop("HIT_ACK", src=TARGET, dst=BALL, count=-1, t0=t_imp - 0.1, t1=t_imp + 30)

    t_imp = play_until_impact(w, setup)
    w.run_until(t_imp + 4.0)
    w.check(w.dropped(rule["r"]) >= 2, "the ball should resend the HIT until the timeout (acks dropped: %d)" % w.dropped(rule["r"]))
    w.check(w.grep(BALL, r"HIT_UNACKED slot"), "the ball should give up (HIT_UNACKED)")
    nhit = len(w.frames("HIT", t0=t_imp))
    w.check(nhit >= 3, "HIT frames sent: %d" % nhit)
    # the same HIT, seen several times: ONE life lost, one effect
    common_checks(w, t_imp)


@scenario("lost_remember_after_lost_hit")
def lost_remember(w):
    """The HIT never arrives and the next REMEMBERs to that pouch are lost too (2.5 s): the display still catches up
    with the first REMEMBER that gets through, and the pouch does not give up the game (LOST_TIMEOUT_MS)."""
    rules = {}

    def setup(t_imp):
        rules["hit"] = w.drop("HIT", src=BALL, dst=TARGET, count=-1, t0=t_imp - 0.1, t1=t_imp + 60)
        rules["rem"] = w.drop("REMEMBER", src=BALL, dst=TARGET, count=-1, t0=t_imp - 0.1, t1=t_imp + 2.5)

    t_imp = play_until_impact(w, setup)
    w.run_until(t_imp + 2.4)
    w.check_eq(w.lives(TARGET), CFG.LIVES_MAX, "the pouch cannot know about the hit yet")
    w.check_eq(w.ball_lives(TARGET), 2, "the ball's lives")
    w.run_until(t_imp + 6.0)
    ln = hit_lines(w, TARGET, t_imp - 0.1)
    w.check(len(ln) == 1, "the pouch never caught up", nodes=[BALL, TARGET], air=True, types=("HIT", "REMEMBER"))
    w.check(ln[0].t - (t_imp + 2.5) <= CFG.REMEMBER_PERIOD_MS / 1000.0 + 0.3,
            "caught up %.2f s after the REMEMBERs came through again" % (ln[0].t - t_imp - 2.5))
    w.check(not w.grep(TARGET, r"-> Joining", t0=t_imp - 0.1), "the pouch gave up the game although the ball only lost 2.5 s of REMEMBERs")
    common_checks(w, t_imp)


@scenario("lost_remember_long_rejoin")
def lost_remember_long(w):
    """No REMEMBER for LOST_TIMEOUT_MS: the pouch assumes the ball forgot it and goes Joining (it falls silent). It hears
    the ball ranging (Listening) and gets its slot and lives back from the next REMEMBER."""
    start_game(w, 3)
    hit_pouch_t = w.now
    from helpers import hit_pouch
    hit_pouch(w, 1, 3, 1)
    slot, lives = w.slot(2), w.lives(2)
    t0 = w.now + 0.2
    t1 = t0 + CFG.LOST_TIMEOUT_MS / 1000.0 + 2.0
    w.drop("REMEMBER", src=BALL, dst=2, count=-1, t0=t0, t1=t1)
    w.run_until(t1 - 0.5)
    w.check(w.grep(2, r"-> Joining", t0=t0), "the pouch should have fallen back to Joining", nodes=[BALL, 2])
    w.check(w.grep(2, r"Joining -> Listening", t0=t0), "the pouch should hear the POLLs and go Listening", nodes=[BALL, 2])
    w.check(not w.grep(2, r"RESET|HIT,", t0=t0), "nothing should have happened to the pouch")
    # while it is silent, the others are ranged undisturbed
    check_air_clean(w, t0, w.now, "while a pouch is Listening")
    t = w.until(lambda: w.phase(2) == "Active", 4.0)
    w.check(t is not None, "the pouch did not come back after the REMEMBERs resumed: %r" % (w.pouch(2),), nodes=[BALL, 2], air=True, types="REMEMBER")
    w.check_eq((w.slot(2), w.lives(2)), (slot, lives), "slot and lives after the long loss")
    check_consistent(w, (1, 2, 3), "after the long REMEMBER loss")
