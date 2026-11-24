"""
Scenario 8 - the game flow with a scripted accelerometer and moving positions: pick-up, throw, hit, miss, hand-over,
bystanders, timeouts and the special cases of docs/FSMs.md (ball FSM v2). Standard court: pouch 1 at (0, 0), pouch 2 at
(6, 0), pouch 3 at (0, 7); the ball starts 0.4 m from pouch 1.
"""
from simlib import BALL, CFG, HELD, LED_BLUE, LED_GREEN, LED_RED, REST, SPINNING, scenario
from helpers import (COURT, carry, check_consistent, hit_effect_plays, hit_pouch, pickup, start_game, throw)

TOO_FAR = CFG.THROW_RANGE_MM / 1000.0


def near(k, d=0.4):
    return (COURT[k][0] + d, COURT[k][1])


def ready_fixture(w, who=1, positions=None):
    """A game with 3 pouches in which the ball is Ready in the hands of pouch `who`."""
    start_game(w, 3, positions=positions)
    t = carry(w, w.now + 0.05, near(who) if positions is None else (positions[who][0] + 0.4, positions[who][1]))
    w.run_until(t + 1.0)
    w.check_eq(w.ball_state(), "Ready", "ball state after the pick-up by pouch %d" % who)
    return w.now


def lives_of(w, pouches=(1, 2, 3)):
    return {k: w.ball_lives(k) for k in pouches}


def states_since(w, t):
    return w.ball_path(t0=t)


@scenario("flow_pickup")
def pickup_rules(w):
    start_game(w, 3)
    t = w.now
    # a resting ball next to a pouch is not "picked up"
    w.run_until(t + 3.0)
    w.check_eq(w.ball_state(), "Unheld", "a ball lying next to a pouch (accelerometer Rest)")
    w.check_eq(w.led_at(BALL, w.now), LED_GREEN, "ball LED in Unheld")
    # Held, but far from every pouch: nobody picked it up
    t2 = carry(w, w.now + 0.05, (3.0, 3.5))
    w.run_until(t2 + 2.0)
    w.check_eq(w.ball_state(), "Unheld", "a ball carried by somebody without a pouch (Held, 3 m from every pouch)")
    # Held within PICKUP_DISTANCE_MM of pouch 2: Ready with that pouch as the thrower
    t3 = carry(w, w.now + 0.05, near(2))
    w.run_until(t3 + 0.6)
    w.check_eq(w.ball_state(), "Ready", "pick-up by pouch 2")
    ln = w.grep(BALL, r"Pickup by slot (\d+)")
    w.check_eq(int(ln[-1].text.split("slot ")[1].split()[0]), w.ball_slot_of(2), "thrower slot")
    took = ln[-1].t - t3
    w.check(took < 0.5, "the pick-up took %.2f s to be recognized" % took)
    w.check_eq(w.led_at(BALL, w.now), LED_RED, "ball LED in Ready")
    # put down again (Rest) next to the pouch: Ready -> Unheld after READY_NOT_HELD_MS
    w.accel(w.now + 0.05, REST)
    w.run_until(w.now + CFG.READY_NOT_HELD_MS / 1000.0 + 1.0)
    w.check_eq(w.ball_state(), "Unheld", "ball state after the ball was put down")
    check_consistent(w, (1, 2, 3), "after pick-up and put-down")


@scenario("flow_throw_needs_spin_and_distance")
def throw_rules(w):
    ready_fixture(w)
    t = w.now
    # (1) Spinning, but the thrower stays within THROW_RANGE_MM: not Thrown (a throw has to travel further than that)
    w.move(BALL, [(t, near(1)), (t + 0.6, (1.5, 0.0))])
    w.accel(t + 0.05, SPINNING)
    w.run_until(t + 1.2)
    w.check_eq(w.ball_state(), "Ready", "Spinning within 2 m of the thrower")
    w.accel(w.now, HELD)
    w.run_until(w.now + 0.5)
    # (2) beyond 2 m but only carried (Held): nobody threw it; after READY_ORPHAN_MS -> Unheld, never Thrown
    t = w.now
    carry(w, t, (4.0, 0.0), speed=2.0)
    w.run_until(t + 3.0)
    w.check("Thrown" not in w.ball_path(t0=t), "a carried ball beyond the throw range became Thrown")
    w.check_eq(w.ball_state(), "Unheld", "ball carried away by somebody without a pouch")
    # (3) pick it up again, throw it: Spinning AND beyond 2 m -> Thrown
    t = carry(w, w.now + 0.1, near(1))
    w.run_until(t + 1.2)
    w.check_eq(w.ball_state(), "Ready", "second pick-up")
    t = w.now
    w.move(BALL, [(t, near(1)), (t + 0.8, (5.0, 0.0))])
    w.accel(t + 0.05, SPINNING)
    w.run_until(t + 1.2)
    w.check_eq(w.ball_state(), "Thrown", "Spinning and 5 m from the thrower")
    ln = w.grep(BALL, r"Thrower slot \d+ beyond", t0=t)
    w.check(ln and ln[0].t > t + 0.1, "the throw was recognized at once?")
    w.note("thrown recognized %.0f ms after release (ball crosses 2 m at about %.0f ms)" % ((ln[0].t - t) * 1000, 2.0 / 5.0 * 800))
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody was hit")


@scenario("flow_hit_other_pouch_loses_one_life")
def hit_other(w):
    ready_fixture(w)
    t0 = w.now
    t_imp = throw(w, t0, (6.0 - 0.1, 0.0), flight=0.7)
    w.run_until(t_imp + 1.5)
    w.check_eq(w.ball_path(t0=t0), ["Thrown", "Hit", "Unheld"], "ball states during the throw")
    w.check_eq(lives_of(w), {1: 3, 2: 2, 3: 3}, "exactly one life lost, by the pouch that was hit")
    w.check_eq((w.lives(2), w.lives(1), w.lives(3)), (2, 3, 3), "the pouches' own view")
    w.check_eq(w.led_at(2, w.now), LED_BLUE, "LED of the pouch with 2 lives")
    w.check_eq(hit_effect_plays(w, 2, t_imp - 0.1, w.now), 1, "hit effect")
    w.check(w.grep(BALL, r"HIT_ACK slot %d" % w.ball_slot_of(2)), "no HIT_ACK")
    check_consistent(w, (1, 2, 3), "after the hit")
    # (the ball is Unheld: rolling around next to the pouch it hit; it is not Ready again until somebody holds it)
    w.run_until(w.now + 2.0)
    w.check_eq(w.ball_state(), "Unheld", "a ball that stays at Rest next to the hit pouch")


@scenario("flow_hit_on_thrower_goes_back_to_ready")
def hit_thrower(w):
    ready_fixture(w)
    t = w.now
    # thrown out to 3.5 m, bounces off a wall and comes back to the thrower's feet: the closest pouch is the thrower
    w.move(BALL, [(t, near(1)), (t + 0.5, (3.5, 0.0)), (t + 1.2, (0.2, 0.0))])
    w.accel(t + 0.05, SPINNING)
    w.hit(t + 1.2, 6000)
    w.run_until(t + 2.5)
    path = w.ball_path(t0=t)
    w.check(path[:2] == ["Thrown", "Ready"], "a hit on the thrower's own pouch should bring the ball back to Ready: %r" % path, nodes=[BALL])
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody loses a life when the thrower is the closest pouch")
    w.check(w.grep(BALL, r"Classify: closest is the thrower"), "no 'closest is the thrower' decision")


@scenario("flow_hit_out_of_lives_is_miss")
def hit_dead(w):
    """Pouch 2 is out of lives. A ball landing at it is a miss, even with an alive pouch (3) within hit range: hitting a
    player who is already out must not cost the players around them a life. And a dead pouch is not 'picking up'."""
    pos = {1: (0.0, 0.0), 2: (6.0, 0.0), 3: (6.7, 0.0)}
    start_game(w, 3, positions=pos)
    hit_pouch(w, 1, 2, 3, positions=pos)
    w.check_eq(lives_of(w), {1: 3, 2: 0, 3: 3}, "after three hits on pouch 2")
    w.check_eq(w.led_at(2, w.now), LED_RED, "an out-of-lives pouch shows red, steady")
    # pick-up by the dead pouch's player: ignored (the ball lies 0.8 m from pouch 3: first move it 1.2 m away from it)
    w.move(BALL, [(w.now + 0.1, (5.5, 0.0))])
    w.run_until(w.now + 0.5)
    w.accel(w.now, HELD)
    t = w.now
    w.run_until(t + 2.0)
    w.check_eq(w.ball_state(), "Unheld", "a Held ball next to an out-of-lives pouch is not picked up")
    # back to pouch 1, throw at pouch 2 again: closest pouch is dead -> miss
    t = carry(w, w.now + 0.1, near(1)) + 1.2
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "pick-up by pouch 1")
    t_imp = throw(w, t, (6.1, 0.0), flight=0.7)  # 0.1 m from pouch 2 (dead), 0.6 m from pouch 3 (alive)
    w.run_until(t_imp + 1.5)
    w.check(w.grep(BALL, r"Classify: closest pouch \(slot \d+, \d+ mm\) is out of lives -> miss", t0=t), "no 'out of lives -> miss' decision", nodes=[BALL])
    w.check_eq(lives_of(w), {1: 3, 2: 0, 3: 3}, "a miss on a dead pouch must not cost anybody a life")
    w.check_eq(w.ball_state(), "Unheld", "ball state after the miss")


@scenario("flow_hit_nobody_near_is_miss")
def hit_nobody(w):
    ready_fixture(w)
    t = w.now
    t_imp = throw(w, t, (3.0, 3.5), flight=0.7)  # lands in the middle of the court: > 900 mm from every pouch
    w.run_until(t_imp + 1.5)
    w.check(w.grep(BALL, r"Classify: no pouch within %d mm -> miss" % CFG.HIT_DISTANCE_MM, t0=t), "no 'no pouch within' decision", nodes=[BALL])
    w.check_eq(w.ball_path(t0=t), ["Thrown", "Unheld"], "ball states")
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody loses a life")


@scenario("flow_short_throw_ignored")
def short_throw(w):
    ready_fixture(w)
    t = w.now
    # a 1.5 m toss: Spinning, the ball lands 1.5 m from the thrower, the impact spike is just an event in Ready
    t_imp = throw(w, t, (1.5, 0.0), flight=0.4, rest_after=0.5)
    w.run_until(t_imp + 1.0)
    w.check_eq(w.ball_state(), "Ready", "just after a throw shorter than THROW_RANGE_MM")
    w.run_until(t_imp + CFG.READY_NOT_HELD_MS / 1000.0 + 1.5)
    w.check("Thrown" not in w.ball_path(t0=t) and "Hit" not in w.ball_path(t0=t), "a short throw entered Thrown / Hit: %r" % w.ball_path(t0=t))
    w.check_eq(w.ball_state(), "Unheld", "the ball times out to Unheld (READY_NOT_HELD_MS)")
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody loses a life")


@scenario("flow_hit_event_in_ready_ignored")
def hit_in_ready(w):
    ready_fixture(w)
    t = w.now
    for dt in (0.2, 0.9, 1.6):  # bumps in the hand, the launch spike: Hit events in Ready mean nothing
        w.hit(t + dt, 9000)
    w.run_until(t + 2.5)
    w.check_eq(w.ball_state(), "Ready", "ball state after Hit events while Held in Ready")
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody loses a life")
    # and in Unheld
    w.accel(w.now, REST)
    w.run_until(w.now + CFG.READY_NOT_HELD_MS / 1000.0 + 1.0)
    t = w.now
    w.hit(t + 0.2, 9000)
    w.run_until(t + 1.0)
    w.check_eq(w.ball_state(), "Unheld", "a Hit event in Unheld")
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody loses a life")


@scenario("flow_handover")
def handover(w):
    """The ball is handed from the thrower's player to another pouch's player standing close by: possession changes after
    THROWER_SWITCH_MS of being closer to the other pouch; Ready is never left."""
    pos = {1: (0.0, 0.0), 2: (1.6, 0.0), 3: (0.0, 7.0)}
    start_game(w, 3, positions=pos)
    t = carry(w, w.now + 0.05, (0.4, 0.0)) + 0.8
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "pick-up by pouch 1")
    w.check(not w.grep(BALL, r"possession changes"), "a thrower switch before the hand-over")
    t1 = carry(w, t, (1.2, 0.0), speed=1.0)  # 0.8 m: closer to pouch 2 from the half-way point on
    w.run_until(t1 + 2.5)
    ln = w.grep(BALL, r"Ready: possession changes, thrower slot (\d+) -> (\d+)", t0=t)
    w.check_eq(len(ln), 1, "possession changes", nodes=[BALL])
    s_from, s_to = [int(x) for x in ln[0].text.split("slot ")[1].replace("->", " ").split()]
    w.check_eq((s_from, s_to), (w.ball_slot_of(1), w.ball_slot_of(2)), "thrower slots")
    t_mid = t + 0.8 / 1.0 * 0.5 + 0.0  # the ball is closer to pouch 2 from (0.8, 0) on
    w.note("possession changed %.2f s after the ball was closer to pouch 2 (THROWER_SWITCH_MS = %d)" % (ln[0].t - t_mid, CFG.THROWER_SWITCH_MS))
    w.check(ln[0].t - t_mid >= CFG.THROWER_SWITCH_MS / 1000.0 - 0.1, "possession changed too early")
    w.check(ln[0].t - t_mid <= CFG.THROWER_SWITCH_MS / 1000.0 + 0.8, "possession changed %.2f s late" % (ln[0].t - t_mid - CFG.THROWER_SWITCH_MS / 1000.0))
    w.check_eq(w.ball_state(), "Ready", "the ball stays Ready during a hand-over")
    # the new thrower can throw: from pouch 2 at 1.6 m, a throw to pouch 3 (7 m away) hits it
    t = w.now
    w.move(BALL, [(t, (1.2, 0.0)), (t + 0.8, (0.0, 6.9))])
    w.accel(t + 0.05, SPINNING)
    w.hit(t + 0.8, 8000)
    w.run_until(t + 2.0)
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 2}, "pouch 3 was hit by the new thrower")


@scenario("flow_bystander_does_not_block_throw")
def bystander(w):
    """A third pouch stands right next to the flight path: only the thrower's range matters for a throw."""
    pos = {1: (0.0, 0.0), 2: (6.0, 0.0), 3: (1.0, 0.8)}  # pouch 3 stands 1.3 m from the thrower, 0.8 m from the line of flight
    start_game(w, 3, positions=pos)
    t = carry(w, w.now + 0.05, (-0.4, 0.0)) + 0.8  # picked up by pouch 1, away from pouch 3
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "pick-up by pouch 1")
    w.move(BALL, [(t, (-0.4, 0.0)), (t + 0.9, (5.9, 0.0))])
    w.accel(t + 0.05, SPINNING)
    w.hit(t + 0.9, 8000)
    w.accel(t + 1.5, REST)
    w.run_until(t + 2.5)
    path = w.ball_path(t0=t)
    w.check(path[:3] == ["Thrown", "Hit", "Unheld"], "a bystander near the flight path blocked the throw: %r" % path, nodes=[BALL])
    w.check_eq(lives_of(w), {1: 3, 2: 2, 3: 3}, "the target loses the life, not the bystander")


@scenario("flow_thrown_timeout_disarm")
def thrown_timeout(w):
    ready_fixture(w)
    t = w.now
    # thrown far away, the accelerometer keeps reading Spinning (rolling): no Hit for THROW_TIMEOUT_MS
    w.move(BALL, [(t, near(1)), (t + 1.0, (3.0, 3.5)), (t + 1.6, (3.2, 3.5))])
    w.accel(t + 0.05, SPINNING)
    w.run_until(t + 1.0)
    w.check_eq(w.ball_state(), "Thrown", "after the throw")
    t_thrown = w.ball_state_times("Thrown")[-1]
    w.run_until(t_thrown + CFG.THROW_TIMEOUT_MS / 1000.0 + 0.5)
    path = w.ball_path(t0=t)
    w.check_eq(path, ["Thrown", "Unheld"], "ball states")
    ln = w.grep(BALL, r"Thrown: no hit for %d ms -> Disarm" % CFG.THROW_TIMEOUT_MS)
    w.check_eq(len(ln), 1, "timeout line")
    dt = ln[0].t - t_thrown
    w.check(abs(dt - CFG.THROW_TIMEOUT_MS / 1000.0) < 0.15, "Thrown lasted %.2f s, expected %.1f s" % (dt, CFG.THROW_TIMEOUT_MS / 1000.0))
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody loses a life")


@scenario("flow_thrown_rest_shortcut")
def thrown_rest(w):
    ready_fixture(w)
    t = w.now
    w.move(BALL, [(t, near(1)), (t + 0.8, (3.0, 3.5))])
    w.accel(t + 0.05, SPINNING)
    w.accel(t + 1.2, REST)  # it landed (no Hit event) and lies still
    w.run_until(t + 4.0)
    ln = w.grep(BALL, r"Thrown: the ball lies at Rest -> Disarm", t0=t)
    w.check_eq(len(ln), 1, "the Rest shortcut", nodes=[BALL])
    t_thrown = w.ball_state_times("Thrown")[-1]
    w.check(ln[0].t - t_thrown < CFG.THROW_TIMEOUT_MS / 1000.0 - 0.5, "the shortcut did not shorten Thrown (%.2f s)" % (ln[0].t - t_thrown))
    w.check_eq(w.ball_state(), "Unheld", "ball state")


@scenario("flow_thrown_returns_to_ready")
def thrown_back(w):
    """The throw is cancelled (the ball comes back within the throw range and stays there for THROWN_RETURN_MS, no Hit):
    Ready with the same thrower."""
    ready_fixture(w)
    t = w.now
    w.move(BALL, [(t, near(1)), (t + 0.5, (3.0, 0.0)), (t + 1.1, (1.0, 0.3))])
    w.accel(t + 0.05, SPINNING)
    w.accel(t + 1.2, HELD)  # somebody grabbed it again
    w.run_until(t + 4.0)
    path = w.ball_path(t0=t)
    w.check(path[:2] == ["Thrown", "Ready"], "ball states: %r" % path, nodes=[BALL])
    ln = w.grep(BALL, r"Thrown: slot (\d+) within", t0=t)
    w.check_eq(len(ln), 1, "return line")
    w.check("possession" not in ln[0].text, "the same pouch should keep the ball: %s" % ln[0].text)
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody loses a life")


@scenario("flow_thrown_possession_change")
def thrown_possession(w):
    """The ball ends up lying near ANOTHER pouch (no Hit event): after THROWN_RETURN_MS it is Ready with that pouch as the
    thrower (a possession change), without a Held check."""
    ready_fixture(w)
    t = w.now
    w.move(BALL, [(t, near(1)), (t + 1.0, (6.0 - 1.5, 0.0))])  # stops 1.5 m from pouch 2
    w.accel(t + 0.05, SPINNING)
    w.accel(t + 1.3, REST)
    w.run_until(t + 4.0)
    ln = w.grep(BALL, r"Thrown: slot (\d+) within .*possession changes", t0=t)
    w.check_eq(len(ln), 1, "possession change line", nodes=[BALL])
    w.check_eq(int(ln[0].text.split("slot ")[1].split()[0]), w.ball_slot_of(2), "new thrower")
    w.check_eq(w.ball_state(), "Ready", "ball state")
    w.check_eq(lives_of(w), {1: 3, 2: 3, 3: 3}, "nobody loses a life")


@scenario("flow_stall_in_thrown_classifies")
def stall(w):
    """The accelerometer stalls in Thrown (it does after a hard shock): Classify() decides by position, like a Hit."""
    ready_fixture(w)
    t = w.now
    w.move(BALL, [(t, near(1)), (t + 0.7, (6.0 - 0.1, 0.0))])
    w.accel(t + 0.05, SPINNING)
    w.stall(t + 0.7, 0.6)
    w.run_until(t + 2.5)
    w.check(w.grep(BALL, r"Thrown: accelerometer stalled", t0=t), "no stall line", nodes=[BALL])
    w.check_eq(w.ball_path(t0=t)[:3], ["Thrown", "Hit", "Unheld"], "ball states")
    w.check_eq(lives_of(w), {1: 3, 2: 2, 3: 3}, "the pouch next to the stalled ball was hit")


@scenario("flow_hit_bounce_one_life")
def bounce(w):
    """The ball hits the pouch and bounces: two Hit events 100 ms apart. ONE life is lost."""
    ready_fixture(w)
    t = w.now
    w.accel_params(cooldown=0.0)  # even if the accelerometer reported both (the real one suppresses the second)
    w.move(BALL, [(t, near(1)), (t + 0.7, (5.9, 0.0)), (t + 0.9, (5.6, 0.2))])
    w.accel(t + 0.05, SPINNING)
    w.hit(t + 0.7, 8000)
    w.hit(t + 0.8, 5000)
    w.hit(t + 1.0, 3000)
    w.accel(t + 1.6, REST)
    w.run_until(t + 4.0)
    w.check_eq(lives_of(w), {1: 3, 2: 2, 3: 3}, "a hit and its bounces cost exactly one life")
    w.check_eq(len(w.grep(BALL, r"Classify: HIT on slot")), 1, "Classify decisions that hit")
    w.check_eq(w.ball_path(t0=t), ["Thrown", "Hit", "Unheld"], "ball states")
    w.check_eq(hit_effect_plays(w, 2, t, w.now), 1, "hit effects")
