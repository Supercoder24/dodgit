"""
Scenario 9 - soak: 4 pouches, 10 % random frame loss on every link, a random game (pick-ups, throws that hit, miss or are too
short, accelerometer stalls, pouches that are switched off and on, Resets) for 5 virtual minutes. No crash, no stuck state
(every state leaves within its documented timeout), and the invariants hold: lives never increase without a Reset, slots are
unique, the ball's table equals what the pouches show once things are quiet.
"""
import random
from collections import Counter

from simlib import BALL, CFG, HELD, REST, SPINNING, scenario
from helpers import COURT, WINDOW, check_consistent, start_game

POUCHES = (1, 2, 3, 4)


class Soak:
    def __init__(self, w, seed):
        self.w = w
        self.rng = random.Random(seed)
        self.last_lives = {}
        self.log_seen = 0
        self.resets_seen = 0
        self.dup_since = {}
        self.stats = Counter()

    def sample(self):
        w = self.w
        n_reset = sum(1 for _, _, to in w.ball_transitions() if to == "Reset")
        reset_now = n_reset != self.resets_seen
        self.resets_seen = n_reset
        tbl = w.ball_table()
        for slot, (uid, lives) in tbl.items():
            prev = self.last_lives.get(uid)
            if prev is not None and lives > prev and not reset_now:
                w.fail("lives of uid %08x rose from %d to %d without a Reset (slot %d)" % (uid, prev, lives, slot), nodes=[BALL], last=50)
            self.last_lives[uid] = lives
        if reset_now:
            for uid in list(self.last_lives):
                self.last_lives[uid] = max(l for u, l in tbl.values() if u == uid) if any(u == uid for u, _ in tbl.values()) else CFG.LIVES_MAX
        # slots of the pouches: duplicates may exist briefly (a pouch the ball forgot, until LOST_TIMEOUT_MS), not for ever
        slots = {}
        for k in POUCHES:
            if w.powered(k) and w.phase(k) in ("Joined", "Active"):
                slots.setdefault(w.slot(k), []).append(k)
        now = w.now
        for s, ks in slots.items():
            if len(ks) > 1:
                first = self.dup_since.setdefault(s, now)
                if now - first > CFG.LOST_TIMEOUT_MS / 1000.0 + 2.0:
                    w.fail("pouches %r have used slot %d for %.1f s" % (ks, s, now - first), last=50)
            else:
                self.dup_since.pop(s, None)
        for s in list(self.dup_since):
            if s not in slots:
                self.dup_since.pop(s)

    def run(self, t_end, step=0.25):
        # (the simulator's clock is whole picoseconds, so after run_until(t_end) now can be a hair below t_end: compare with a tolerance)
        while t_end - self.w.now > 1e-6:
            self.w.run_until(min(t_end, self.w.now + step))
            self.sample()

    def alive_pouches(self):
        tbl = self.w.ball_table()
        return [k for k in POUCHES if self.w.ball_slot_of(k) is not None and tbl[self.w.ball_slot_of(k)][1] > 0 and self.w.powered(k)
                and self.w.phase(k) == "Active"]

    def turn(self):
        w, rng = self.w, self.rng
        alive = self.alive_pouches()
        if len(alive) < 1:
            self.run(w.now + 2.0)
            return
        a = rng.choice(alive)
        others = [k for k in POUCHES if k != a]
        b = rng.choice(others)
        t = w.now + 0.1
        pa = (COURT[a][0] + rng.uniform(-0.5, 0.5), COURT[a][1] + rng.uniform(0.3, 0.6))
        cur = w.pos(BALL, t)
        d = ((cur[0] - pa[0]) ** 2 + (cur[1] - pa[1]) ** 2) ** 0.5
        dt = max(d / 1.5, 0.1)
        w.move(BALL, [(t, cur), (t + dt, pa)])
        w.accel(t, HELD)
        t += dt + rng.uniform(1.0, 1.8)  # held next to the thrower
        kind = rng.choices(["hit", "miss", "short", "stall"], [60, 15, 10, 15])[0]
        flight = rng.uniform(0.5, 0.9)
        if kind == "hit" or kind == "stall":
            target = (COURT[b][0] + rng.uniform(-0.3, 0.3), COURT[b][1] + rng.uniform(-0.3, 0.3))
        elif kind == "miss":
            target = (rng.uniform(1.5, 4.5), rng.uniform(2.0, 5.0))
        else:
            target = (pa[0] + rng.uniform(0.8, 1.5), pa[1])
        w.move(BALL, [(t, pa), (t + flight, target)])
        w.accel(t + 0.05, SPINNING)
        if kind == "stall":
            w.stall(t + flight, 0.5)
        else:
            w.hit(t + flight, 8000)
            if rng.random() < 0.4:
                w.hit(t + flight + 0.1, 4000)  # a bounce
        w.accel(t + flight + rng.uniform(0.4, 1.0), REST)
        self.stats[kind] += 1
        self.run(t + flight + rng.uniform(1.5, 3.0))

    def reboot(self):
        w, rng = self.w, self.rng
        k = rng.choice(POUCHES)
        off = rng.uniform(0.5, 3.0)
        w.reboot(k, w.now + 0.1, off_for=off)
        self.stats["reboot"] += 1
        self.run(w.now + 0.1 + off + 3.0)

    def reset(self):
        w = self.w
        t_press = w.now + 0.2
        w.long_press(t_press)
        self.run(t_press + CFG.BUTTON_HOLD_MS / 1000.0 + WINDOW + 1.0)
        for _ in range(10):  # a Roll Call follows if somebody joined (or everybody was deleted): end it
            if w.ball_state() == "Roll Call":
                w.short_press(w.now + 1.0)
            self.run(w.now + 3.0)
            if w.ball_state() != "Roll Call":
                break
        self.stats["reset"] += 1


@scenario("soak_4_pouches_10pct_loss", timeout=600, seed=7)
def soak(w):
    sk = Soak(w, 20240607)
    w.loss_all(0.10, [BALL] + list(POUCHES))
    start_game(w, 4)
    sk.sample()
    T_END = 300.0
    next_reset = w.now + 70.0
    while w.now < T_END - 30.0:
        if w.now >= next_reset:
            sk.reset()
            next_reset = w.now + sk.rng.uniform(60.0, 90.0)
            continue
        r = sk.rng.random()
        if r < 0.10:
            sk.reboot()
        else:
            sk.turn()
    w.note("actions: %s" % dict(sk.stats))
    # --- the game ends: quiet period, no loss; everything must settle ---
    w.accel(w.now, REST)
    w.loss_all(0.0, [BALL] + list(POUCHES))
    sk.run(w.now + 12.0)
    # no crash
    for n in [BALL] + list(POUCHES):
        w.check(w.powered(n) and not w.exited(n), "node %d is dead" % n, nodes=[n])
    # no stuck state: every state leaves within its documented timeout
    limits = {"Thrown": CFG.THROW_TIMEOUT_MS / 1000.0 + 0.6, "Hit": CFG.HIT_ACK_TIMEOUT_MS / 1000.0 + 0.6,
              "Reset": WINDOW + 0.6}
    tr = w.ball_transitions()
    for (t0, _, st), (t1, _, _) in zip(tr, tr[1:]):
        if st in limits and t1 - t0 > limits[st]:
            w.fail("the ball stayed in %s for %.2f s (limit %.2f s)" % (st, t1 - t0, limits[st]), t0=t0 - 0.5, t1=t1 + 0.5, nodes=[BALL], last=60)
    if tr and tr[-1][2] in limits:
        w.check(w.now - tr[-1][0] < limits[tr[-1][2]], "the ball is stuck in %s" % tr[-1][2], nodes=[BALL], last=60)
    w.check(w.ball_state() != "Reset", "ball stuck in Reset", nodes=[BALL])
    # the ball's table and the pouches agree: everybody in the table is Active with the table's slot and lives; nobody else is
    # in the game
    check_consistent(w, POUCHES, "at the end of the soak")
    tbl_uids = {u for u, _ in w.ball_table().values()}
    for k in POUCHES:
        ph = w.phase(k)
        if w.uid(k) in tbl_uids:
            w.check(ph == "Active", "pouch %d is in the ball's table but %s" % (k, ph), nodes=[BALL, k], last=60)
        else:
            w.check(ph in ("Listening", "Joining"), "pouch %d is not in the table but shows %s" % (k, ph), nodes=[BALL, k], last=60)
    tbl = w.ball_table()
    w.note("final table %s; pouch views %s" % (tbl, {k: w.pouch(k) for k in POUCHES}))
    # the air: scheduled frames never overlap (JOINs may collide: they are random)
    pairs = w.overlapping_frames(ignore=("JOIN",))
    w.note("%d frames on the air, %d collisions, %d overlapping non-JOIN pairs" % (w.stat("frames"), w.stat("collisions"), len(pairs)))
    if pairs:
        a, b = pairs[0]
        w.fail("%d overlapping scheduled frames, first: %r / %r" % (len(pairs), a, b), t0=a.t - 0.05, t1=a.t + 0.05, air=True, last=60)
