"""
Building blocks shared by the scenarios (tests/sim/scenarios/*.py): game set-up, the moves of a player
(pick up, carry, throw), and invariants that hold in every scenario.
"""
import collections

from simlib import (ACTIVE, BALL, CFG, HELD, JOINED, REST, SPINNING, Fail, LED_ALL, LED_BLUE, LED_GREEN,
                    LED_NONE, LED_RED)

# A standard court: pouch k stands at COURT[k-1]. The distances (6 m / 7 m) are well beyond THROW_RANGE_MM
# (2 m) and HIT_DISTANCE_MM, and the pouches are far enough apart that a ball next to one is far from the rest.
COURT = {1: (0.0, 0.0), 2: (6.0, 0.0), 3: (0.0, 7.0), 4: (6.0, 7.0), 5: (3.0, 3.5), 6: (-4.0, 3.0),
         7: (9.0, 3.0), 8: (3.0, -4.0)}

BOOT_S = 0.80   # a node's self-test (3 green blinks) takes 0.75 s before it starts its radio
ROLLCALL_S = 1.0  # extra time a Joining pouch needs to reach Joined in Roll Call


def lives_led(lives):
    """The LED mask of an Active pouch with that many lives (shared/led_patterns.h)."""
    return {3: LED_GREEN, 2: LED_BLUE, 1: None, 0: LED_RED}[min(lives, 3)]


def power_times(order, n, stagger=0.3):
    """Power-on times {node: t} for the orders of the first scenario."""
    if order == "ball_first":
        t = {BALL: 0.0}
        t.update({k: 0.2 + stagger * k for k in range(1, n + 1)})
    elif order == "pouches_first":
        t = {k: stagger * (k - 1) for k in range(1, n + 1)}
        t[BALL] = 1.5 + stagger * n
    elif order == "interleaved":  # pouch, ball, pouch, pouch ...
        t = {1: 0.0, BALL: 0.7}
        t.update({k: 0.7 + stagger * (k - 1) for k in range(2, n + 1)})
    elif order == "together":  # everything switched on at the same instant
        t = {k: 0.0 for k in range(0, n + 1)}
    else:
        raise ValueError(order)
    return t


def start_game(w, n=3, positions=None, ball_pos=None, leave="press", stagger=0.3, order="ball_first"):
    """Power the ball and n pouches, wait until all of them registered, end Roll Call, let the pouches become Active.
    leave: 'press' (short button press) or 'timeout' (ROLLCALL_TIMEOUT_MS after the last new pouch).
    The ball starts 0.4 m from pouch 1 (a player holds it) unless ball_pos says otherwise. Returns the time at
    which the ball left Roll Call."""
    pos = positions or {k: COURT[k] for k in range(1, n + 1)}
    for k, p in pos.items():
        w.place(k, *p)
    w.place(BALL, *(ball_pos or (pos[1][0] + 0.4, pos[1][1])))
    for node, t in power_times(order, n, stagger).items():
        w.power_on(node, t)
    ok = w.until(lambda: len(w.ball_table()) == n, 20)
    w.check(ok is not None, "only %d of %d pouches registered in Roll Call: %r" % (len(w.ball_table()), n, w.ball_table()))
    if leave == "press":
        w.short_press(w.now + 0.5)
        w.until(lambda: w.ball_state() != "Roll Call", 5.0)
    else:
        w.until(lambda: w.ball_state() != "Roll Call", CFG.ROLLCALL_TIMEOUT_MS / 1000.0 + 3.0)
    w.check(w.ball_state() in ("Unheld", "Ready"), "Roll Call did not end (state %s)" % w.ball_state())
    left = w.now
    for t, _, to in w.ball_transitions():
        if to in ("Unheld", "Ready"):
            left = t
            break
    w.run_for(1.0)  # the pouches see the first POLL and become Active
    return left


WINDOW = CFG.RESET_WINDOW_MS / 1000.0


def begin_reset(w, hold_at=None):
    """Hold the ball's button until it enters Reset and run to the end of the Reset window.
    Returns (t_reset_start, t_reset_end, text of the 'Reset: ...' summary line)."""
    t_press = w.now + 0.2 if hold_at is None else hold_at
    w.long_press(t_press)
    t0 = w.run_until_log(BALL, "-> Reset", CFG.BUTTON_HOLD_MS / 1000.0 + 2.0 + max(0.0, t_press - w.now))
    w.check(t0 is not None, "no Reset after holding the button", nodes=[BALL])
    t1 = w.run_until_log(BALL, "Reset: ", WINDOW + 1.5)
    w.check(t1 is not None, "the Reset never ended", nodes=[BALL])
    w.run_for(0.1)  # the ball leaves the Reset state a moment after it logged the summary
    return t0, t1, w.grep(BALL, r"Reset: \d+ joined")[-1].text


def pickup(w, t, pos=None):
    """A player picks the ball up (accelerometer Held from t) and, optionally, it is at pos."""
    if pos is not None:
        w.move(BALL, [(t, pos)])
    w.accel(t, HELD)


def carry(w, t, to, speed=1.5, level=HELD):
    """The ball is carried from where it is to `to` at `speed` m/s, accelerometer Held; returns the arrival time."""
    a = w.pos(BALL, t)
    d = ((a[0] - to[0]) ** 2 + (a[1] - to[1]) ** 2) ** 0.5
    dt = max(d / speed, 0.05)
    w.move(BALL, [(t, a), (t + dt, to)])
    w.accel(t, level)
    return t + dt


def throw(w, t, to, flight=0.7, hit=True, mg=8000, rest_after=0.8, spin_lag=0.04):
    """The ball is thrown at time t from where it is towards `to`: Spinning in flight, a Hit event on arrival
    (if hit), then it rolls a little and lies at Rest. Returns the arrival (impact) time."""
    a = w.pos(BALL, t)
    w.move(BALL, [(t, a), (t + flight, to)])
    w.accel(t + spin_lag, SPINNING)
    if hit:
        w.hit(t + flight, mg)
    w.accel(t + flight + rest_after, REST)
    return t + flight


def pouch_view(w, nodes):
    return {k: w.pouch(k) for k in nodes}


def hit_pouch(w, thrower, target, hits=1, t=None, positions=None):
    """Play `hits` complete throws (pick-up at the thrower's pouch, throw, impact at the target's pouch) and
    return the time afterwards. The ball ends Unheld next to the target; the next throw carries it back."""
    t = w.now if t is None else t
    pos = positions or COURT
    for _ in range(hits):
        t = carry(w, t + 0.05, (pos[thrower][0] + 0.4, pos[thrower][1]))
        t += 1.0  # held next to the thrower: Unheld -> Ready (2 confirmation rounds)
        w.run_until(t)
        w.check(w.ball_state() == "Ready", "the ball did not become Ready after the pick-up (state %s)" % w.ball_state())
        arr = throw(w, t, (pos[target][0] - 0.1, pos[target][1]), flight=0.7)
        t = arr + 1.5
        w.run_until(t)
    return t


# ---------------------------------------------------------------------------------------------------
# invariants
# ---------------------------------------------------------------------------------------------------
def check_consistent(w, pouches, msg="", quiet=True):
    """The ball's table and the pouches agree: every given pouch that is in the game (Joined / Active) holds the
    slot and lives the ball has recorded for its uid; slots are unique."""
    tbl = w.ball_table()
    by_uid = {u: (s, l) for s, (u, l) in tbl.items()}
    seen = {}
    bad = []
    for k in pouches:
        if not w.powered(k):
            continue
        phase, slot, lives = w.pouch(k)
        if phase not in ("Joined", "Active"):
            continue
        row = by_uid.get(w.uid(k))
        if row is None:
            bad.append("pouch %d (%s slot %d) is not in the ball's table" % (k, phase, slot))
            continue
        if row[0] != slot:
            bad.append("pouch %d thinks slot %d, ball table has slot %d" % (k, slot, row[0]))
        if row[1] != lives:
            bad.append("pouch %d shows %d lives, ball table has %d" % (k, lives, row[1]))
        if slot in seen:
            bad.append("pouches %d and %d both use slot %d" % (seen[slot], k, slot))
        seen[slot] = k
    slots = [s for s in tbl]
    if len(set(u for u, _ in tbl.values())) != len(tbl):
        bad.append("duplicate uid in the ball's table")
    if bad:
        w.fail("%s inconsistent: %s (ball table %r)" % (msg, "; ".join(bad), tbl), nodes=[BALL] + list(pouches))


def check_air_clean(w, t0, t1, msg="", ignore=("JOIN",)):
    """No two frames overlap on the air in [t0, t1] (everything except the JOINs is scheduled not to)."""
    pairs = w.overlapping_frames(t0, t1, ignore=ignore)
    if pairs:
        a, b = pairs[0]
        w.fail("%s: %d overlapping frame pair(s) on the air, first: %r / %r" % (msg, len(pairs), a, b),
               t0=a.t - 0.05, t1=a.t + 0.05, air=True, last=60)


def check_remembers_heard(w, t0, t1, msg=""):
    """Every REMEMBER the ball sent in [t0, t1] reached the pouch it was meant for. (A REMEMBER is lost for
    good while the pouch is busy; a lost one in a quiet, lossless run is a protocol race.)"""
    uid_node = {w.uid(k): k for k in range(1, 9) if w.booted(k)}
    lost = []
    for f in w.frames("REMEMBER", t0, t1):
        k = uid_node.get(f.uid)
        if k is not None and f.fate.get(k) not in ("good", None):
            lost.append(f)
    if lost:
        w.fail("%s: %d REMEMBER(s) did not reach their pouch, first: %r" % (msg, len(lost), lost[0]),
               t0=lost[0].t - 0.05, t1=lost[0].t + 0.05, air=True, last=60)
    return lost


def episodes(masks, wanted, t0, t1):
    """Time segments [(ts, te)] in [t0, t1] during which the LED mask is `wanted`. masks: [(t, mask)]."""
    segs = []
    cur = 0
    start = None
    prev_t = t0
    # initial level
    for t, m in masks:
        if t <= t0:
            cur = m
    if cur == wanted:
        start = t0
    for t, m in masks:
        if t <= t0 or t > t1:
            continue
        if cur == wanted and m != wanted:
            segs.append((start, t))
            start = None
        elif cur != wanted and m == wanted:
            start = t
        cur = m
    if cur == wanted and start is not None:
        segs.append((start, t1))
    return segs


def hit_effect_plays(w, node, t0, t1):
    """How many separate hit effects (all four LEDs flashing at 5 Hz for LED_HIT_EFFECT_MS) the pouch showed in
    [t0, t1]: flashes closer together than half a second belong to one effect."""
    plays = 0
    last = None
    for ts, _ in episodes(w.leds(node), LED_ALL, t0, t1):
        if last is None or ts - last > 0.5:
            plays += 1
        last = ts
    return plays
