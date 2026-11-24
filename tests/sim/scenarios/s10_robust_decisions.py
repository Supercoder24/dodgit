"""
Scenario 10 - the decisions are robust against frames that are missing (a pouch is silent in about 8 % of the rounds even
when it is close): Classify() ranges CLASSIFY_ROUNDS times and merges, a hand-over survives 8 % frame loss, a thrower that
is silent for a moment keeps its last range, and a REMEMBER is never sent into a pouch's FINAL wait after an unanswered round.
"""
from simlib import BALL, CFG, HELD, LED_BLUE, REST, SPINNING, scenario
from helpers import COURT, carry, check_consistent, start_game, throw

# ---------------------------------------------------------------------------------------------------
# (b) Classify() finds the hit pouch although it is silent in some of the rounds
# ---------------------------------------------------------------------------------------------------


def classify_with_silence(w, silent_rounds):
    start_game(w, 3)
    t = carry(w, w.now + 0.05, (COURT[1][0] + 0.4, COURT[1][1])) + 1.0
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "ball state before the throw")
    t_imp = throw(w, t, (6.0 - 0.1, 0.0), flight=0.7)
    # stop right when the ball's FSM sees the Hit (before its Classify rounds start) and make the target silent
    t_hit = w.run_until_log(BALL, "Thrown: Hit", 3.0)
    w.check(t_hit is not None, "the ball never saw the Hit event", nodes=[BALL])
    rule = w.drop("RESPONSE", src=2, dst=BALL, count=silent_rounds, t0=w.now)
    w.run_until(t_imp + 2.0)
    return rule, t_imp


@scenario("classify_pouch_silent_in_one_round")
def silent_one(w):
    rule, t_imp = classify_with_silence(w, 1)
    w.check_eq(w.dropped(rule), 1, "RESPONSEs of the target dropped during Classify")
    w.check(w.grep(BALL, r"Classify: HIT on slot %d" % w.ball_slot_of(2)), "the silent-in-one-round target was not found", nodes=[BALL], air=True,
            types=("POLL", "RESPONSE", "FINAL", "REPORT", "HIT"), t0=t_imp)
    w.check_eq({k: w.ball_lives(k) for k in (1, 2, 3)}, {1: 3, 2: 2, 3: 3}, "lives")


@scenario("classify_pouch_silent_in_two_of_three_rounds")
def silent_two(w):
    rule, t_imp = classify_with_silence(w, 2)
    w.check_eq(w.dropped(rule), 2, "RESPONSEs of the target dropped during Classify")
    w.check(w.grep(BALL, r"Classify: HIT on slot %d" % w.ball_slot_of(2)), "the target heard in one of three rounds was not found", nodes=[BALL])
    w.check_eq({k: w.ball_lives(k) for k in (1, 2, 3)}, {1: 3, 2: 2, 3: 3}, "lives")


@scenario("classify_pouch_silent_in_every_round_is_a_miss")
def silent_all(w):
    rule, t_imp = classify_with_silence(w, CFG.CLASSIFY_ROUNDS)
    w.check_eq(w.dropped(rule), CFG.CLASSIFY_ROUNDS, "RESPONSEs of the target dropped during Classify")
    w.check(w.grep(BALL, r"Classify: no pouch within %d mm -> miss" % CFG.HIT_DISTANCE_MM), "a pouch that answered no round must not be hit", nodes=[BALL])
    w.check_eq({k: w.ball_lives(k) for k in (1, 2, 3)}, {1: 3, 2: 3, 3: 3}, "lives")
    w.check_eq(w.ball_state(), "Unheld", "ball state after the miss")


# ---------------------------------------------------------------------------------------------------
# (c) a hand-over succeeds with 8 % frame loss on the two links involved
# ---------------------------------------------------------------------------------------------------
def handover_with_loss(w, p):
    pos = {1: (0.0, 0.0), 2: (1.6, 0.0), 3: (0.0, 7.0)}
    start_game(w, 3, positions=pos)
    w.loss(BALL, 1, p)
    w.loss(BALL, 2, p)
    t = carry(w, w.now + 0.05, (0.4, 0.0)) + 1.0
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "pick-up by pouch 1 (8 % loss on its link)")
    t1 = carry(w, t, (1.2, 0.0), speed=1.0)
    w.run_until(t1 + 4.0)
    ln = w.grep(BALL, r"Ready: possession changes, thrower slot (\d+) -> (\d+)", t0=t)
    w.check_eq(len(ln), 1, "possession changes (hand-over with %.0f %% frame loss)" % (p * 100), nodes=[BALL])
    s_from, s_to = [int(x) for x in ln[0].text.split("slot ")[1].replace("->", " ").split()]
    w.check_eq((s_from, s_to), (w.ball_slot_of(1), w.ball_slot_of(2)), "thrower slots")
    w.check_eq(w.ball_state(), "Ready", "ball state after the hand-over")
    w.check("Unheld" not in w.ball_path(t0=t) and "Thrown" not in w.ball_path(t0=t), "the ball left Ready during the hand-over: %r" % w.ball_path(t0=t))
    late = ln[0].t - (t + 0.4)  # the ball is closer to pouch 2 from 0.8 m on: 0.4 s into the carry
    w.note("hand-over %.2f s after the ball got closer to pouch 2 (loss %.0f %%)" % (late, p * 100))


for _seed in (1, 2, 3, 4, 5, 6):
    scenario("handover_with_8pct_loss_seed%d" % _seed, seed=_seed)((lambda: lambda w: handover_with_loss(w, 0.08))())


# ---------------------------------------------------------------------------------------------------
# a thrower that is silent for a moment keeps its last range
# ---------------------------------------------------------------------------------------------------
def thrower_silence(w, dist_m, silence_s):
    start_game(w, 3)
    t = carry(w, w.now + 0.05, (COURT[1][0] + 0.4, COURT[1][1])) + 1.0
    w.run_until(t)
    w.check_eq(w.ball_state(), "Ready", "pick-up")
    # a short toss that stays within the throw range: Spinning, the thrower at dist_m, silent for silence_s
    w.move(BALL, [(t, (COURT[1][0] + 0.4, COURT[1][1])), (t + 0.3, (dist_m, 0.0))])
    w.accel(t + 0.05, SPINNING)
    w.drop("RESPONSE", src=1, dst=BALL, count=-1, t0=t + 0.5, t1=t + 0.5 + silence_s)
    w.run_until(t + 1.2)
    return t


@scenario("thrower_silent_briefly_keeps_last_range")
def silent_brief(w):
    t = thrower_silence(w, 1.5, 0.05)  # 50 ms: shorter than THROWER_SILENT_MS
    w.check_eq(w.ball_state(), "Ready", "a thrower that is silent for %d ms must keep its last range (1.5 m)" % 50)
    w.check("Thrown" not in w.ball_path(t0=t), "one dropped round started Thrown")


@scenario("thrower_silent_long_counts_as_far")
def silent_long(w):
    t = thrower_silence(w, 1.5, 0.6)
    w.check_eq(w.ball_path(t0=t)[:1], ["Thrown"], "a thrower silent for 600 ms while the ball spins is out of range -> Thrown")


@scenario("thrower_silent_when_touching_stays_in_range")
def silent_close(w):
    t = thrower_silence(w, 0.2, 0.6)  # last range 0.2 m <= TOO_CLOSE_MM: silent because it is right there
    w.check("Thrown" not in w.ball_path(t0=t), "a thrower that went silent at 200 mm (ranging fails when touching) started Thrown")


@scenario("thrown_return_survives_missing_rounds", seed=3)
def return_with_loss(w):
    start_game(w, 3)
    t = carry(w, w.now + 0.05, (COURT[1][0] + 0.4, COURT[1][1])) + 1.0
    w.run_until(t)
    w.loss(BALL, 1, 0.08)
    w.move(BALL, [(t, (0.4, 0.0)), (t + 0.5, (3.0, 0.0)), (t + 1.1, (1.0, 0.3))])
    w.accel(t + 0.05, SPINNING)
    w.accel(t + 1.2, HELD)
    w.run_until(t + 5.0)
    path = w.ball_path(t0=t)
    w.check(path[:2] == ["Thrown", "Ready"], "the return streak broke on missing rounds: %r" % path, nodes=[BALL])


# ---------------------------------------------------------------------------------------------------
# (d) REMEMBER is not sent into a pouch's FINAL wait after an unanswered round
# ---------------------------------------------------------------------------------------------------
FINAL_WAIT_SLACK_MS = 10  # shared/uwb.c: a pouch waits for FINAL this long beyond the response window of its POLL


def window_ms(n):
    return (CFG.SLOT_BASE_UUS + n * CFG.SLOT_UUS) * 1026 // 1000000 + 4


@scenario("remember_not_sent_into_final_wait")
def remember_vs_final(w):
    """With most RESPONSEs lost the ball often hears nobody and sends no FINAL; the pouches that did hear the POLL wait for
    it (and ignore everything else) until FINAL_WAIT_SLACK_MS after the response window. No REMEMBER may start in there, and the
    display of a pouch whose HIT was lost still converges."""
    start_game(w, 3)
    t0 = w.now
    for k in (1, 2, 3):
        w.loss(k, BALL, 0.75, t=t0, both=False)  # a quarter of the RESPONSEs get through
    w.run_until(t0 + 20.0)
    polls = w.frames("POLL", t0 + 0.2, w.now - 0.2)
    finals = w.frames("FINAL", t0, w.now)
    remembers = w.frames("REMEMBER", t0 + 0.2, w.now - 0.2)
    unanswered = []
    fi = 0
    for i, p in enumerate(polls):
        nxt = polls[i + 1].t if i + 1 < len(polls) else p.t + 0.03
        answered = any(p.t < f.t < nxt for f in finals)
        if not answered:
            unanswered.append(p)
    w.note("%d POLLs, %d without FINAL, %d REMEMBERs" % (len(polls), len(unanswered), len(remembers)))
    w.check(len(unanswered) > 100 and len(remembers) > 40, "the set-up does not produce unanswered rounds (%d) / REMEMBERs (%d)" % (len(unanswered), len(remembers)))
    bad = []
    for p in unanswered:
        n = p.payload[0]
        until = p.t + window_ms(n) / 1000.0 + FINAL_WAIT_SLACK_MS / 1000.0 + 0.0006  # the latest a waiting pouch gives up
        for r in remembers:
            if p.t < r.t < until:
                bad.append((p, r))
    if bad:
        p, r = bad[0]
        w.fail("%d REMEMBERs started inside the FINAL wait of an unanswered round (a pouch that heard that POLL drops them); first: REMEMBER %.4f s "
               "after the POLL at %.4f s" % (len(bad), r.t - p.t, p.t), t0=p.t - 0.01, t1=p.t + 0.06, air=True, nodes=[BALL], last=40)
    # no REMEMBER was lost for the pouches that were not waiting either: the pouches' view stays in sync
    check_consistent(w, (1, 2, 3), "under heavy RESPONSE loss")
    w.check(not w.grep(1, r"-> Joining") and not w.grep(2, r"-> Joining") and not w.grep(3, r"-> Joining"), "a pouch lost the ball under RESPONSE loss (no REMEMBER for LOST_TIMEOUT_MS)")
