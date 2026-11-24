"""
Scenario 11 - three behaviours that a mutation test of the suite showed nothing was checking:

  * the LAUNCH SPIKE of a throw (a Hit event while the ball is still Held in Ready) is drained in Ready and never
    classified: the throw that follows costs the target exactly one life;
  * two players standing close together do NOT swap the thrower every round: a ball that is only briefly closer
    to the other pouch (less than THROWER_SWITCH_MS) keeps its thrower;
  * a Reset started in the middle of the game (Ready, Thrown, Hit), not only from Unheld, resets every life, deletes
    nobody and leaves the ball working.
"""
from simlib import BALL, CFG, HELD, SPINNING, scenario
from helpers import COURT, WINDOW, carry, check_consistent, hit_effect_plays, start_game, throw  # noqa: F401

PICKUP_S = 1.0  # held next to a pouch: Unheld -> Ready needs 2 confirmation rounds


def lives_of(w, pouches=(1, 2, 3)):
    return {k: w.ball_lives(k) for k in pouches}


def ready_with_pouch1(w):
    """3 pouches on the standard court, the ball picked up next to pouch 1 and Ready."""
    start_game(w, 3)
    t = carry(w, w.now + 0.05, (COURT[1][0] + 0.4, COURT[1][1])) + PICKUP_S
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "ball state after the pick-up by pouch 1", nodes=[BALL])
    return w.now


# ---------------------------------------------------------------------------------------------------
# 1. the launch spike is not a hit
# ---------------------------------------------------------------------------------------------------
@scenario("flow_launch_spike_is_not_a_hit")
def launch_spike(w):
    t = ready_with_pouch1(w)
    w.hit(t + 0.2, 9000)  # a bump in the hand while Held
    t += 0.6
    w.hit(t, 12000)       # the launch spike: the hand lets go and the ball is Held for one more instant
    t_imp = throw(w, t, (COURT[2][0] - 0.1, COURT[2][1]), flight=0.7)
    w.run_until(t_imp + 2.0)
    path = w.ball_path(t0=t - 0.1)
    w.check_eq(path[:2], ["Thrown", "Hit"],
               "the launch spike must not be classified (that would end Thrown at the thrower with a miss): %r" % path, nodes=[BALL])
    classify = w.grep(BALL, r"Classify:", t0=t - 0.1)
    w.check_eq(len(classify), 1, "Classify lines for one throw: %r" % [ln.text for ln in classify], nodes=[BALL])
    w.check("HIT on slot %d" % w.ball_slot_of(2) in classify[0].text, "Classify decided: %s" % classify[0].text, nodes=[BALL])
    w.check_eq(lives_of(w), {1: 3, 2: 2, 3: 3}, "lives after one throw with a launch spike (exactly one life, from the real impact)")
    check_consistent(w, (1, 2, 3), "after the throw")
    # the Hit event the ball acted on is the impact, not the spike
    hits = w.grep(BALL, r"Thrown: Hit (\d+) mg", t0=t - 0.1)
    w.check_eq(len(hits), 1, "'Thrown: Hit' lines", nodes=[BALL])
    w.check(abs(hits[0].t - t_imp) < 0.2, "the Hit the ball acted on came %.2f s after the impact, it must be the impact" % (hits[0].t - t_imp))


# ---------------------------------------------------------------------------------------------------
# 2. two close players do not swap the thrower
# ---------------------------------------------------------------------------------------------------
@scenario("flow_close_players_do_not_swap_thrower")
def close_players(w):
    """Pouch 1 at (0, 0), pouch 2 at (1.0, 0). The ball is held 0.5 m beside the line between them and sways between
    x = 0.3 (closer to pouch 1) and x = 0.7 (closer to pouch 2) every half second: it is closer to pouch 2 for 0.5 s at a
    time, never THROWER_SWITCH_MS in a row, for 8 s. Nothing may change hands, and nothing else may happen either."""
    pos = {1: (0.0, 0.0), 2: (1.0, 0.0), 3: (0.0, 7.0)}
    start_game(w, 3, positions=pos)
    t = carry(w, w.now + 0.05, (0.3, 0.5)) + PICKUP_S
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "pick-up by pouch 1", nodes=[BALL])
    pickups = w.grep(BALL, r"Pickup by slot (\d+)")
    w.check_eq(int(pickups[-1].text.split("slot ")[1].split()[0]), w.ball_slot_of(1), "the thrower after the pick-up")
    half = 0.5
    assert half < CFG.THROWER_SWITCH_MS / 1000.0, "the scenario needs the sway to be shorter than THROWER_SWITCH_MS"
    path = []
    x = 0.3
    n = 16
    for i in range(n):
        nx = 0.7 if x < 0.5 else 0.3
        path.append((t + i * half, (x, 0.5)))
        path.append((t + (i + 1) * half - 0.05, (nx, 0.5)))
        x = nx
    w.move(BALL, path)
    w.run_until(t + n * half)
    sw = w.grep(BALL, r"Ready: possession changes", t0=t)
    w.check_eq(len(sw), 0, "the thrower swapped while the ball only swayed between two close pouches: %r" % [ln.text for ln in sw], nodes=[BALL])
    w.check_eq(w.ball_path(t0=t), [], "the ball left Ready while it swayed")
    w.check_eq(w.ball_state(), "Ready", "ball state after 8 s of swaying")
    # control: if it STAYS closer to pouch 2 it does change hands (so the assertion above is not vacuous)
    w.move(BALL, [(w.now, (0.7, 0.5))])
    w.run_until(w.now + CFG.THROWER_SWITCH_MS / 1000.0 + 1.5)
    sw = w.grep(BALL, r"Ready: possession changes, thrower slot %d -> %d" % (w.ball_slot_of(1), w.ball_slot_of(2)), t0=t)
    w.check_eq(len(sw), 1, "control: staying closer to pouch 2 must hand the ball over", nodes=[BALL])


# ---------------------------------------------------------------------------------------------------
# 3. a Reset from the middle of the game
# ---------------------------------------------------------------------------------------------------
def finish_reset(w, expect_from):
    """The ball is held at the button until it enters Reset; run through the window and check the outcome."""
    t0 = w.run_until_log(BALL, "-> Reset", CFG.BUTTON_HOLD_MS / 1000.0 + 6.0)
    w.check(t0 is not None, "no Reset after holding the button", nodes=[BALL])
    came_from = [frm for tt, frm, to in w.ball_transitions() if to == "Reset"][-1]
    w.check_eq(came_from, expect_from, "the Reset must start from %s" % expect_from, nodes=[BALL])
    t1 = w.run_until_log(BALL, "Reset: ", WINDOW + 1.5)
    w.check(t1 is not None, "the Reset never ended", nodes=[BALL])
    w.run_for(1.5)
    summary = w.grep(BALL, r"Reset: \d+ joined")[-1].text
    w.check("0 joined, 0 deleted" in summary and "3 pouch(es)" in summary, "Reset summary: %r" % summary, nodes=[BALL])
    w.check(w.ball_state() in ("Unheld", "Ready"), "the ball did not go back into the game after the Reset: %s" % w.ball_state(), nodes=[BALL])
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "every life must be restored")
    for k in (1, 2, 3):
        w.check_eq(w.pouch(k)[0], "Active", "pouch %d after the Reset" % k)
        w.check_eq(w.lives(k), CFG.LIVES_MAX, "lives shown by pouch %d" % k)
    check_consistent(w, (1, 2, 3), "after the Reset")
    # and the ball still works: it ranges every pouch
    rounds = [r for r in w.ranges(t0=w.now - 1.0) if r["ret"] > 0]
    w.check(len(rounds) > 10 and all(len(r["by_slot"]) == 3 for r in rounds), "ranging after the Reset does not hear all 3 pouches")


@scenario("reset_from_ready")
def reset_from_ready(w):
    t = ready_with_pouch1(w)
    w.long_press(t + 0.2)
    finish_reset(w, "Ready")


@scenario("reset_from_thrown")
def reset_from_thrown(w):
    t = ready_with_pouch1(w)
    # thrown and never landing: 4.4 m from every pouch, Spinning, no Hit. THROW_TIMEOUT_MS is longer than the hold.
    assert CFG.BUTTON_HOLD_MS + 600 < CFG.THROW_TIMEOUT_MS + 300, "the scenario needs the hold to end inside Thrown"
    w.move(BALL, [(t, w.pos(BALL, t)), (t + 0.7, (3.0, 3.5))])
    w.accel(t + 0.05, SPINNING)
    w.long_press(t + 0.1)
    finish_reset(w, "Thrown")
    w.check("Thrown" in w.ball_path(t0=t), "the ball never became Thrown", nodes=[BALL])


@scenario("reset_from_hit")
def reset_from_hit(w):
    """The hit pouch never acknowledges, so the Hit state lasts HIT_ACK_TIMEOUT_MS; the hold ends inside it."""
    t = ready_with_pouch1(w) + 4.0  # everything below is scheduled BEFORE the clock moves: the hold starts earlier than the throw
    t_imp = t + 0.7
    w.drop("HIT_ACK", src=2, dst=BALL, count=-1, t0=t, t1=t + 6.0)
    throw(w, t, (COURT[2][0] - 0.1, COURT[2][1]), flight=0.7)
    w.long_press(t_imp + 0.5 - CFG.BUTTON_HOLD_MS / 1000.0)  # the hold completes 0.5 s into the Hit state
    w.run_until(t_imp + 0.45)
    w.check_eq(w.ball_state(), "Hit", "the ball must be waiting for the HIT_ACK that never comes", nodes=[BALL], air=True, types=("HIT", "HIT_ACK"))
    w.check_eq(w.lives(2), CFG.LIVES_MAX - 1, "the pouch received the HIT (only its acknowledgement was lost)")
    finish_reset(w, "Hit")
