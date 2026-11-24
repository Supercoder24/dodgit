"""
Scenario 6 - a Reset deletes the pouches that stay silent: their slot is freed and reused by the next joiner, the
ranging round gets shorter, an all-silent table sends the ball to Roll Call, a pouch that was deleted although it is
alive falls back to Joining and rejoins at the next Reset, and a Reset with an empty table still accepts a JOIN.
"""
from simlib import BALL, CFG, LED_BLUE, LED_GREEN, scenario
from helpers import (COURT, WINDOW, begin_reset, check_air_clean, check_consistent, hit_pouch, start_game)


def poll_slots(w, t0, t1):
    """The n_slots field of the ball's POLLs in [t0, t1]."""
    return sorted({f.payload[0] for f in w.frames("POLL", t0, t1) if f.payload})


@scenario("reset_deletes_silent_pouch")
def delete_silent(w):
    """Pouch 2 is switched off; after the Reset it is gone, the others keep slot and (reset) lives, the game resumes
    without a Roll Call."""
    start_game(w, 3)
    hit_pouch(w, 1, 3, 1)
    w.power_off(2, w.now + 0.2)
    w.run_for(2.0)
    gone_uid = w.uid(2)
    w.check_eq(sorted(w.ball_table()), [0, 1, 2], "the ball keeps the table of a switched-off pouch until a Reset")
    t0, t1, summary = begin_reset(w)
    w.check("0 joined, 1 deleted" in summary and "2 pouch" in summary, "Reset summary: %r" % summary, nodes=[BALL])
    w.check(len(w.grep(BALL, r"MEMBER deleted uid=0x%08x slot 1" % gone_uid)) == 1, "the silent pouch was not deleted", nodes=[BALL])
    w.check_eq(sorted(w.ball_table()), [0, 2], "slots in the table after the Reset (slot 1 freed)")
    w.check(all(l == CFG.LIVES_MAX for _, l in w.ball_table().values()), "Reset must restore every life: %r" % w.ball_table())
    w.check(w.ball_state() in ("Unheld", "Ready"), "nobody joined, so the game resumes directly: ball state %s" % w.ball_state(), nodes=[BALL])
    w.check("Roll Call" not in w.ball_path(t0=t0), "the ball went to Roll Call although nobody joined")
    w.run_for(2.0)
    check_consistent(w, (1, 3), "after the Reset")
    for k in (1, 3):
        w.check_eq(w.pouch(k)[0], "Active", "pouch %d after the Reset" % k)
        w.check_eq(w.lives(k), CFG.LIVES_MAX, "lives of pouch %d" % k)
    # the highest slot is still 2, so the ranging round is as long as before
    w.check_eq(poll_slots(w, w.now - 1.5, w.now), [3], "n_slots of the POLLs while slots 0 and 2 are in use")


@scenario("reset_slot_reused_round_shortens")
def slot_reused(w):
    """Pouch 3 (slot 2, the highest) is deleted: the POLL's n_slots drops from 3 to 2 and a ranging round gets shorter.
    A pouch that joins afterwards gets the freed slot."""
    start_game(w, 3)
    before = poll_slots(w, w.now - 1.0, w.now)
    rounds = w.frames("POLL", w.now - 1.0, w.now)
    period_before = (rounds[-1].t - rounds[0].t) / (len(rounds) - 1)
    w.check_eq(before, [3], "n_slots of the POLLs with 3 pouches")
    w.power_off(3, w.now + 0.2)
    w.run_for(1.0)
    t0, t1, summary = begin_reset(w)
    w.check("0 joined, 1 deleted" in summary, "Reset summary: %r" % summary, nodes=[BALL])
    w.check_eq(sorted(w.ball_table()), [0, 1], "slots after deleting the highest one")
    w.run_for(2.0)
    after = poll_slots(w, w.now - 1.0, w.now)
    rounds = w.frames("POLL", w.now - 1.0, w.now)
    period_after = (rounds[-1].t - rounds[0].t) / (len(rounds) - 1)
    w.check_eq(after, [2], "n_slots of the POLLs after the Reset")
    w.note("POLL period %.1f ms with 3 slots, %.1f ms with 2 slots" % (period_before * 1000, period_after * 1000))
    w.check(period_after < period_before - 0.002, "the ranging round did not get shorter (%.1f -> %.1f ms)" % (period_before * 1000, period_after * 1000))
    # a new pouch reuses slot 2 (the lowest free slot)
    w.place(4, *COURT[4])
    w.power_on(4, w.now + 0.2)
    w.until(lambda: w.booted(4) == 1 and w.phase(4) == "Listening", 6.0)
    t0, t1, summary = begin_reset(w)
    w.check("1 joined, 0 deleted" in summary and "3 pouch" in summary, "Reset summary: %r" % summary, nodes=[BALL, 4])
    w.check_eq(w.ball_slot_of(4), 2, "the new pouch takes the freed slot")
    w.check_eq(sorted(w.ball_table()), [0, 1, 2], "slots after the new pouch joined")
    check_consistent(w, (1, 2, 4), "after the slot was reused")


@scenario("reset_all_silent_goes_to_roll_call")
def all_silent(w):
    start_game(w, 3)
    for k in (1, 2, 3):
        w.power_off(k, w.now + 0.2)
    w.run_for(1.5)
    t0, t1, summary = begin_reset(w)
    w.check("0 joined, 3 deleted" in summary and "0 pouch" in summary, "Reset summary: %r" % summary, nodes=[BALL])
    w.check_eq(w.ball_table(), {}, "the table after every pouch was deleted")
    w.run_for(0.5)
    w.check_eq(w.ball_state(), "Roll Call", "an empty table sends the ball to Roll Call (nobody to play with)")
    # a short press does nothing without pouches, and the pouches can join afterwards as in a normal start
    w.short_press(w.now + 0.2)
    w.run_for(1.0)
    w.check_eq(w.ball_state(), "Roll Call", "a short press with an empty table must be ignored")
    for i, k in enumerate((3, 2, 1)):
        w.power_on(k, w.now + 0.3 + 0.4 * i)
    t = w.until(lambda: len(w.ball_table()) == 3 and all(w.phase(k) == "Joined" for k in (1, 2, 3)), 10.0)
    w.check(t is not None, "the pouches did not join the new Roll Call: table %r" % (w.ball_table(),), nodes=[BALL, 1, 2, 3])
    w.check_eq([w.ball_slot_of(k) for k in (3, 2, 1)], [0, 1, 2], "slots in the order of switching on")
    w.short_press(w.now + 0.3)
    w.until(lambda: w.ball_state() != "Roll Call", 4.0)
    w.run_for(2.0)
    check_consistent(w, (1, 2, 3), "after the empty-table Reset and a new Roll Call")


@scenario("reset_empty_table_accepts_join")
def empty_table_join(w):
    """The Reset with an empty table (everybody deleted before) still accepts a JOIN: a pouch that is Joining (not Listening)
    when the Reset starts, or that is switched on during it, gets in."""
    start_game(w, 2)
    for k in (1, 2):
        w.power_off(k, w.now + 0.2)
    w.run_for(1.0)
    t0, t1, summary = begin_reset(w)  # table is empty afterwards; ball goes to Roll Call
    w.check_eq(w.ball_table(), {}, "table empty")
    # now Reset again from Roll Call is impossible (the button hold does nothing there), so get out of Roll Call first:
    w.check_eq(w.ball_state(), "Roll Call", "ball state")
    # a pouch joins in Roll Call, the game starts, everybody else gets deleted
    w.power_on(1, w.now + 0.2)
    w.until(lambda: len(w.ball_table()) == 1, 6.0)
    w.short_press(w.now + 0.3)
    w.until(lambda: w.ball_state() != "Roll Call", 4.0)
    w.power_off(1, w.now + 0.3)
    w.run_for(1.0)
    t0, t1, summary = begin_reset(w)  # empties the table again
    w.check("0 joined, 1 deleted" in summary and "0 pouch" in summary, "Reset summary: %r" % summary, nodes=[BALL])
    w.check_eq(w.ball_state(), "Roll Call", "ball state after the second empty table")
    # the real test: a Reset that STARTS with an empty table. Get there: power the pouch on during Roll Call? (no, Roll Call
    # cannot be Reset). Instead: delete the only pouch while the others are away, and switch a pouch on during that Reset.
    w.power_on(2, w.now + 0.2)
    w.until(lambda: len(w.ball_table()) == 1, 6.0)
    w.short_press(w.now + 0.3)
    w.until(lambda: w.ball_state() != "Roll Call", 4.0)
    w.run_for(1.0)
    w.power_off(2, w.now + 0.1)
    w.run_for(1.0)
    w.place(3, *COURT[3])
    t_press = w.now + 0.2
    w.long_press(t_press)
    t0 = w.run_until_log(BALL, "-> Reset", 5.0)
    w.check(t0 is not None, "no Reset", nodes=[BALL])
    # the table is NOT empty yet at the start (pouch 2's entry is deleted at the end); a joiner during the window is accepted
    w.power_on(3, t0 + 0.2)
    t1 = w.run_until_log(BALL, "Reset: ", WINDOW + 1.5)
    summary = w.grep(BALL, r"Reset: \d+ joined")[-1].text
    w.check("1 joined, 1 deleted" in summary and "1 pouch" in summary, "Reset summary: %r" % summary, nodes=[BALL, 3])
    w.check_eq(w.ball_state(), "Roll Call", "ball state after the Reset that took in a joiner")
    w.check_eq(w.ball_slot_of(3), 1, "the joiner got the lowest free slot (slot 0 was still held by the silent pouch during the window)")


@scenario("reset_deleted_but_alive_pouch")
def deleted_alive(w):
    """A pouch that hears nothing during the Reset (100 % loss ball -> pouch) is deleted although it is alive. It keeps
    answering POLLs in its old slot until LOST_TIMEOUT_MS, then falls back to Joining, hears the POLLs (Listening) and
    rejoins at the next Reset."""
    start_game(w, 3)
    hit_pouch(w, 1, 2, 1)
    t_press = w.now + 0.2
    t_hold = t_press + CFG.BUTTON_HOLD_MS / 1000.0
    w.loss(BALL, 3, 1.0, t=t_hold - 0.2, both=False)  # 100 % loss ball -> pouch 3 ...
    w.loss(BALL, 3, 0.0, t=t_hold + WINDOW + 0.3, both=False)  # ... for the Reset window only
    t0, t1, summary = begin_reset(w, hold_at=t_press)
    w.check("0 joined, 1 deleted" in summary and "2 pouch" in summary, "Reset summary: %r" % summary, nodes=[BALL, 3])
    uid3 = w.uid(3)
    w.check(w.ball_slot_of(3) is None, "pouch 3 should be deleted")
    # the pouch does not know: it still shows Active with its old lives (it never heard the RESET)
    w.run_for(0.6)
    w.check_eq(w.phase(3), "Active", "the pouch that heard nothing still believes it is in the game")
    # it falls back to Joining after LOST_TIMEOUT_MS without REMEMBER, hears POLLs and goes Listening
    t = w.until(lambda: w.phase(3) == "Listening", CFG.LOST_TIMEOUT_MS / 1000.0 + 3.0)
    w.check(t is not None, "the deleted pouch did not fall back: %r" % (w.pouch(3),), nodes=[BALL, 3])
    w.check(w.grep(3, r"-> Joining"), "the deleted pouch never went through Joining", nodes=[BALL, 3])
    # no frame of the deleted pouch collided with the others in the meantime
    check_air_clean(w, t1, w.now, "while a deleted pouch is still alive")
    # the next Reset takes it back in, with full lives
    t0b, t1b, summary_b = begin_reset(w)
    w.check("1 joined, 0 deleted" in summary_b and "3 pouch" in summary_b, "second Reset summary: %r" % summary_b, nodes=[BALL, 3])
    w.check_eq(w.ball_state(), "Roll Call", "ball state after the pouch rejoined")
    w.check(w.ball_slot_of(3) is not None, "pouch 3 is not back in the table")
    w.short_press(w.now + 0.3)
    w.until(lambda: w.ball_state() != "Roll Call", 4.0)
    w.run_for(2.0)
    check_consistent(w, (1, 2, 3), "after the deleted pouch rejoined")
    w.check_eq(w.lives(3), CFG.LIVES_MAX, "lives after rejoining")
