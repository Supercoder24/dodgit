"""
dodgit host co-simulation - the scenario DSL (Python 3, standard library only).

A World is one simulation: a ball (node 0) and up to 8 pouches (nodes 1..8) running the UNMODIFIED
firmware (see README.md) on a virtual clock, talking over a simulated UWB air. A scenario script builds a
World, schedules things (power, positions, button, accelerometer, frame loss), advances the virtual time
and asserts on what the nodes logged, showed and put on the air. Failures raise Fail with the relevant
slice of the logs.

    w = World(seed=1)
    w.place(0, 0, 0); w.place(1, 0.5, 0); w.place(2, 0, 0.7)
    w.power_on(0, 0.0); w.power_on(1, 0.5); w.power_on(2, 0.9)
    w.run_until(8.0)
    w.check(w.ball_table() == {0: (w.uid(1), 3), 1: (w.uid(2), 3)}, "table")

Times are seconds of virtual time. All of it is deterministic for a given seed.
"""
import ctypes
import os
import random
import re
import sys

PS = 10**12
BALL = 0

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

PHASES = ["Joining", "Listening", "Joined", "Active"]
JOINING, LISTENING, JOINED, ACTIVE = range(4)
LEVELS = {"unknown": 0, "rest": 1, "held": 2, "spinning": 3}
REST, HELD, SPINNING = "rest", "held", "spinning"

MSG = {0x20: "JOIN", 0x21: "REMEMBER", 0x31: "POLL", 0x32: "RESPONSE", 0x33: "FINAL", 0x34: "REPORT",
       0x35: "HIT", 0x36: "HIT_ACK", 0x37: "RESET", 0x38: "RESET_ACK"}
MSG_ID = {v: k for k, v in MSG.items()}
FATES = ["pending", "good", "crc", "collision", "loss", "rule", "deaf", "txbusy", "busy", "off", "abort"]

LED_NONE, LED_GREEN, LED_RED, LED_BLUE, LED_ALL = 0x0, 0x1, 0x6, 0x8, 0xF
LED_NAMES = {0x0: "off", 0x1: "green", 0x6: "red", 0x8: "blue", 0xF: "all", 0x9: "green+blue",
             0xE: "red+blue", 0x7: "green+red"}


SCENARIOS = {}


def scenario(name=None, known_bug=None, timeout=300, seed=1):
    """Register a scenario function f(w) (w is a World built by the runner with a fixed seed).
    known_bug=n marks a scenario that fails because of product bug #n of tests/sim/FINDINGS.md: it stays
    failing (the assertion is not weakened) and run.sh prints it as KNOWN BUG #n."""
    def deco(f):
        nm = name or f.__name__
        SCENARIOS[nm] = {"fn": f, "known_bug": known_bug, "timeout": timeout, "seed": seed}
        return f
    return deco


class Fail(Exception):
    """A scenario assertion failed (message already contains the log excerpt)."""


# ---------------------------------------------------------------------------------------------
# The product's settings.h: the scenarios follow the tuning instead of hard-coding it
# ---------------------------------------------------------------------------------------------
class Settings:
    """The #define NAME value lines of shared/settings.h and shared/led_patterns.h, evaluated."""

    HEADERS = ("settings.h", "led_patterns.h")

    def __init__(self):
        self.v = {}
        pending = []
        for h in self.HEADERS:
            for line in open(os.path.join(ROOT, "shared", h)):
                m = re.match(r"\s*#define\s+([A-Z][A-Z0-9_]*)\s+(.*)$", line)
                if not m:
                    continue
                name, expr = m.group(1), m.group(2)
                expr = re.sub(r"/\*.*?(\*/|$)", "", expr).strip()
                if expr:
                    pending.append((name, expr))
        for _ in range(4):  # resolve defines that use earlier ones
            for name, expr in pending:
                if name in self.v:
                    continue
                try:
                    self.v[name] = eval(expr.replace("/", "//"), {}, dict(self.v))
                except Exception:
                    pass

    def __getattr__(self, name):
        try:
            return self.v[name]
        except KeyError:
            raise AttributeError(name)


CFG = Settings()

# ---------------------------------------------------------------------------------------------
# ctypes binding of libsim
# ---------------------------------------------------------------------------------------------


class TraceRec(ctypes.Structure):
    _fields_ = [("t_start", ctypes.c_int64), ("t_end", ctypes.c_int64), ("src", ctypes.c_int8),
                ("len", ctypes.c_uint8), ("aborted", ctypes.c_uint8), ("data", ctypes.c_uint8 * 32),
                ("fate", ctypes.c_uint8 * 12)]


class RangeRec(ctypes.Structure):
    _fields_ = [("t", ctypes.c_int64), ("ret", ctypes.c_int), ("count", ctypes.c_int),
                ("slot", ctypes.c_uint8 * 8), ("lives", ctypes.c_uint8 * 8), ("uid", ctypes.c_uint32 * 8),
                ("mm", ctypes.c_int32 * 8)]


_lib = None


def _load():
    global _lib
    if _lib is not None:
        return _lib
    build = os.environ.get("SIM_BUILD")
    if not build:
        raise RuntimeError("SIM_BUILD is not set: run tests/sim/run.sh (it builds the simulator)")
    ext = "dylib" if sys.platform == "darwin" else "so"
    lib = ctypes.CDLL(os.path.join(build, "libsim." + ext), mode=ctypes.RTLD_GLOBAL)
    c = ctypes
    sigs = {
        "sim_reset": (None, [c.c_uint64, c.c_char_p]),
        "sim_run": (c.c_int64, [c.c_int64, c.c_char_p, c.c_int]),
        "sim_now": (c.c_int64, []),
        "sim_set_console_baud": (None, [c.c_int]),
        "sim_set_trace": (None, [c.c_int]),
        "sim_power_on": (None, [c.c_int, c.c_int64, c.c_char_p]),
        "sim_power_off": (None, [c.c_int, c.c_int64]),
        "sim_button": (None, [c.c_int, c.c_int64, c.c_int]),
        "sim_accel_level_at": (None, [c.c_int, c.c_int64, c.c_int]),
        "sim_accel_hit_at": (None, [c.c_int, c.c_int64, c.c_int]),
        "sim_accel_stall_at": (None, [c.c_int, c.c_int64, c.c_int64]),
        "sim_loss_at": (None, [c.c_int, c.c_int, c.c_int64, c.c_double, c.c_double]),
        "sim_drop": (c.c_int, [c.c_int, c.c_int, c.c_int, c.c_int, c.c_int64, c.c_int64]),
        "sim_drop_hits": (c.c_int, [c.c_int]),
        "sim_waypoint": (None, [c.c_int, c.c_int64, c.c_double, c.c_double, c.c_double]),
        "sim_clear_waypoints": (None, [c.c_int]),
        "sim_position_at": (None, [c.c_int, c.c_int64, c.POINTER(c.c_double)]),
        "sim_set_clock": (None, [c.c_int, c.c_int32, c.c_int64]),
        "sim_set_antenna_error": (None, [c.c_int, c.c_int32, c.c_int32]),
        "sim_set_ts_noise": (None, [c.c_int, c.c_double]),
        "sim_set_accel_params": (None, [c.c_int, c.c_int64, c.c_int]),
        "sim_rand_force": (None, [c.c_int, c.POINTER(c.c_uint32), c.c_int]),
        "sim_node_state": (c.c_int, [c.c_int]),
        "sim_node_boots": (c.c_int, [c.c_int]),
        "sim_log_count": (c.c_int, [c.c_int]),
        "sim_log_line": (c.c_char_p, [c.c_int, c.c_int, c.POINTER(c.c_int64)]),
        "sim_led_count": (c.c_int, [c.c_int]),
        "sim_led_get": (c.c_int, [c.c_int, c.c_int, c.POINTER(c.c_int64)]),
        "sim_pouch_phase": (c.c_int, [c.c_int]),
        "sim_pouch_slot": (c.c_int, [c.c_int]),
        "sim_pouch_lives": (c.c_int, [c.c_int]),
        "sim_dev_id": (c.c_uint64, [c.c_int]),
        "sim_ball_count": (c.c_int, [c.c_int]),
        "sim_ball_mask": (c.c_int, [c.c_int]),
        "sim_ball_slot_uid": (c.c_uint32, [c.c_int, c.c_int]),
        "sim_ball_slot_lives": (c.c_int, [c.c_int, c.c_int]),
        "sim_range_count": (c.c_int, [c.c_int]),
        "sim_range_base": (c.c_void_p, [c.c_int]),
        "sim_range_rec_size": (c.c_int, []),
        "sim_trace_count": (c.c_int, []),
        "sim_trace_base": (c.c_void_p, []),
        "sim_trace_rec_size": (c.c_int, []),
        "sim_stat": (c.c_uint64, [c.c_int]),
        "sim_accel_stat": (c.c_uint32, [c.c_int, c.c_int]),
    }
    for name, (res, args) in sigs.items():
        f = getattr(lib, name)
        f.restype = res
        f.argtypes = args
    assert lib.sim_trace_rec_size() == ctypes.sizeof(TraceRec), "TraceRec layout mismatch"
    assert lib.sim_range_rec_size() == ctypes.sizeof(RangeRec), "RangeRec layout mismatch"
    _lib = lib
    return lib


def ps(t):
    return int(round(t * PS))


def fnv_uid(dev_id):
    """shared/uwb.c compute_uid(): FNV-1a over the 8 device-ID bytes, 0 is reserved."""
    h = 2166136261
    for b in dev_id.to_bytes(8, "little"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h or 1


class Line:
    __slots__ = ("t", "text")

    def __init__(self, t, text):
        self.t = t
        self.text = text

    def __repr__(self):
        return "%9.4f  %s" % (self.t, self.text)


class Frame:
    """One frame on the air, from the simulator's trace."""

    def __init__(self, idx, rec):
        self.idx = idx
        self.t = rec.t_start / PS
        self.t_end = rec.t_end / PS
        self.src = rec.src
        self.len = rec.len
        self.aborted = bool(rec.aborted)
        self.data = bytes(rec.data)[: min(rec.len, 32)]
        d = self.data
        self.type_id = d[9] if len(d) > 9 else -1
        self.type = MSG.get(self.type_id, "?%02x" % self.type_id)
        self.dst_addr = d[5] | (d[6] << 8) if len(d) > 6 else 0
        self.src_addr = d[7] | (d[8] << 8) if len(d) > 8 else 0
        self.payload = d[10:] if len(d) > 10 else b""
        self.fate = {n: FATES[rec.fate[n]] for n in range(12) if rec.fate[n]}

    @property
    def dst_slot(self):
        """Timeslot a unicast frame is addressed to (pouches), else None."""
        return self.dst_addr - 0x0100 if 0x0100 <= self.dst_addr < 0x0200 else None

    @property
    def src_slot(self):
        return self.src_addr - 0x0100 if 0x0100 <= self.src_addr < 0x0200 else None

    @property
    def uid(self):
        """The uid in the payload (JOIN, REMEMBER)."""
        return int.from_bytes(self.payload[0:4], "little") if len(self.payload) >= 4 else None

    def __repr__(self):
        extra = ""
        if self.type == "REMEMBER" and len(self.payload) >= 6:
            extra = " uid=%08x slot=%d lives=%d" % (int.from_bytes(self.payload[0:4], "little"),
                                                     self.payload[4], self.payload[5])
        elif self.type == "POLL" and self.payload:
            extra = " n=%d" % self.payload[0]
        elif self.type == "FINAL" and self.payload:
            extra = " mask=%02x" % self.payload[0]
        elif self.type in ("HIT", "HIT_ACK") and self.payload:
            extra = " lives=%d" % self.payload[0]
        fates = " ".join("n%d:%s" % (n, f) for n, f in sorted(self.fate.items()) if f != "off")
        return "%9.4f  AIR n%d %-9s%s  [%s]" % (self.t, self.src, self.type, extra, fates)


class World:
    def __init__(self, seed=1, console_baud=115200, name=None, realistic=True):
        build = os.environ["SIM_BUILD"]
        self.lib = _load()
        self.seed = seed
        self.name = name or "scenario"
        self.copy_dir = os.path.join(build, "copies-%d" % os.getpid())
        os.makedirs(self.copy_dir, exist_ok=True)
        self.lib.sim_reset(seed, self.copy_dir.encode())
        self.lib.sim_set_console_baud(console_baud)
        self.ball_lib = os.path.join(build, "node_ball." + ("dylib" if sys.platform == "darwin" else "so"))
        self.pouch_lib = os.path.join(build, "node_pouch." + ("dylib" if sys.platform == "darwin" else "so"))
        if realistic:
            # crystals within +-10 ppm of each other, 40 ps of timestamp noise (about 1 cm of range noise)
            rng = random.Random(seed * 7919 + 13)
            for n in range(12):
                self.clock(n, drift_ppm=rng.uniform(-10.0, 10.0))
                self.timestamp_noise(n, 40.0)
        self._logs = {}
        self._frames = None
        self._frames_n = -1
        self.notes = []

    # ---- scenario actions ----------------------------------------------------------------------
    def place(self, node, x, y=0.0, z=0.0):
        """Fixed position (meters) of a node for the whole run (or from now on: replaces its waypoints)."""
        self.lib.sim_clear_waypoints(node)
        self.lib.sim_waypoint(node, 0, x, y, z)

    def move(self, node, path):
        """Waypoints [(t, (x, y, z)), ...] with linear interpolation; before the first / after the last the
        node stays at the first / last."""
        for t, p in path:
            p = tuple(p) + (0.0,) * (3 - len(p))
            self.lib.sim_waypoint(node, ps(t), p[0], p[1], p[2])

    def pos(self, node, t=None):
        out = (ctypes.c_double * 3)()
        self.lib.sim_position_at(node, ps(self.now if t is None else t), out)
        return tuple(out)

    def dist(self, a, b, t=None):
        pa, pb = self.pos(a, t), self.pos(b, t)
        return sum((x - y) ** 2 for x, y in zip(pa, pb)) ** 0.5

    def power_on(self, node, t):
        self.lib.sim_power_on(node, ps(t), (self.ball_lib if node == BALL else self.pouch_lib).encode())

    def power_off(self, node, t):
        self.lib.sim_power_off(node, ps(t))

    def reboot(self, node, t, off_for=0.3):
        self.power_off(node, t)
        self.power_on(node, t + off_for)

    def press(self, t, hold):
        """The ball's button: down at t, released hold seconds later."""
        self.lib.sim_button(BALL, ps(t), 1)
        self.lib.sim_button(BALL, ps(t + hold), 0)

    def short_press(self, t, hold=0.15):
        self.press(t, hold)

    def long_press(self, t, hold=None):
        """Hold the button for the Reset (BUTTON_HOLD_MS plus a margin); returns when it is released."""
        hold = CFG.BUTTON_HOLD_MS / 1000.0 + 0.4 if hold is None else hold
        self.press(t, hold)
        return t + hold

    def accel(self, t, level):
        """The (mock) accelerometer's level from time t: 'rest' / 'held' / 'spinning' / 'unknown'."""
        self.lib.sim_accel_level_at(BALL, ps(t), LEVELS[level])

    def hit(self, t, mg=8000):
        """A Hit event (impact spike) in the accelerometer's event queue."""
        self.lib.sim_accel_hit_at(BALL, ps(t), mg)

    def stall(self, t, duration=0.5):
        """The accelerometer stalls: a stall event now, level Unknown for `duration`, then the previous level."""
        self.lib.sim_accel_stall_at(BALL, ps(t), ps(duration))

    def accel_params(self, cooldown=0.0, fail=False):
        """cooldown: Hit events injected this soon after an accepted one are suppressed (the real classifier
        has IMPACT_COOLDOWN_MS = 300 ms; default 0 = every injected Hit reaches the ball's FSM)."""
        self.lib.sim_set_accel_params(BALL, ps(cooldown), 1 if fail else 0)

    def loss(self, a, b, p, t=0.0, both=True, crc=0.0):
        """Random frame loss with probability p on the link a -> b (and b -> a) from time t on. crc: of the
        lost frames this fraction arrives with a CRC error (the receiver reports an error) instead of vanishing."""
        self.lib.sim_loss_at(a, b, ps(t), p, crc)
        if both:
            self.lib.sim_loss_at(b, a, ps(t), p, crc)

    def loss_all(self, p, nodes, t=0.0):
        for a in nodes:
            for b in nodes:
                if a != b:
                    self.lib.sim_loss_at(a, b, ps(t), p, 0.0)

    def drop(self, mtype, src=-1, dst=-1, count=1, t0=0.0, t1=1e6):
        """Drop `count` frames (-1 = all) of message type `mtype` ('RESPONSE', ...; None = any) sent by node
        `src` (-1 = any) as heard by node `dst` (-1 = any), starting within [t0, t1). Returns a rule id for
        dropped(rule)."""
        mt = -1 if mtype is None else MSG_ID[mtype]
        return self.lib.sim_drop(mt, src, dst, count, ps(t0), ps(t1))

    def dropped(self, rule):
        return self.lib.sim_drop_hits(rule)

    def clock(self, node, drift_ppm=0.0, offset=None):
        """Device clock of a node: drift in ppm and the 40-bit start offset (default random)."""
        self.lib.sim_set_clock(node, int(drift_ppm * 1000), -1 if offset is None else offset)

    def antenna_error(self, node, tx=0, rx=0):
        self.lib.sim_set_antenna_error(node, tx, rx)

    def timestamp_noise(self, node, sigma_ps):
        self.lib.sim_set_ts_noise(node, sigma_ps)

    def force_random(self, node, values):
        """The next draws of the node's sys_rand32_get() return `values` (JOIN back-off delays: the delay is
        min + value % (max - min + 1))."""
        arr = (ctypes.c_uint32 * len(values))(*values)
        self.lib.sim_rand_force(node, arr, len(values))

    # ---- running ---------------------------------------------------------------------------------
    @property
    def now(self):
        return self.lib.sim_now() / PS

    def run_until(self, t):
        self.lib.sim_run(ps(t), None, 0)
        return self.now

    def run_for(self, dt):
        return self.run_until(self.now + dt)

    def run_until_log(self, node, text, timeout):
        """Run until a line containing `text` appears in the node's log (returns its time) or `timeout`
        seconds passed (returns None)."""
        before = self.lib.sim_log_count(node)
        start = self.now
        self.lib.sim_run(ps(start + timeout), text.encode(), node)
        lines = self.log(node)
        for ln in lines[before:]:
            if text in ln.text:
                return ln.t
        return None

    def until(self, pred, timeout, step=0.02, what="condition"):
        """Run in `step` slices until pred() is true; returns the time, or None after `timeout` seconds."""
        end = self.now + timeout
        while self.now < end:
            if pred():
                return self.now
            self.run_until(min(end, self.now + step))
        return self.now if pred() else None

    def run_until_state(self, state, timeout):
        """Run until the ball's latest State: line enters `state`."""
        t = self.until(lambda: self.ball_state() == state, timeout)
        return t

    # ---- observation -----------------------------------------------------------------------------
    def log(self, node):
        cache = self._logs.setdefault(node, [])
        n = self.lib.sim_log_count(node)
        if n > len(cache):
            t = ctypes.c_int64()
            for i in range(len(cache), n):
                s = self.lib.sim_log_line(node, i, ctypes.byref(t))
                cache.append(Line(t.value / PS, s.decode("utf-8", "replace")))
        return cache

    def grep(self, node, pattern, t0=-1.0, t1=1e9):
        rx = re.compile(pattern)
        return [ln for ln in self.log(node) if t0 <= ln.t <= t1 and rx.search(ln.text)]

    def uid(self, node):
        return fnv_uid(self.lib.sim_dev_id(node))

    def booted(self, node):
        return self.lib.sim_node_boots(node)

    def powered(self, node):
        return bool(self.lib.sim_node_state(node) & 1)

    def exited(self, node):
        return bool(self.lib.sim_node_state(node) & 2)

    def phase(self, node):
        p = self.lib.sim_pouch_phase(node)
        return PHASES[p] if 0 <= p < len(PHASES) else None

    def slot(self, node):
        return self.lib.sim_pouch_slot(node)

    def lives(self, node):
        return self.lib.sim_pouch_lives(node)

    def pouch(self, node):
        """(phase, slot, lives) as the pouch itself knows them."""
        return (self.phase(node), self.slot(node), self.lives(node))

    def ball_table(self):
        """{slot: (uid, lives)} of the ball's table."""
        mask = self.lib.sim_ball_mask(BALL)
        tbl = {}
        for s in range(8):
            if mask > 0 and mask & (1 << s):
                tbl[s] = (self.lib.sim_ball_slot_uid(BALL, s), self.lib.sim_ball_slot_lives(BALL, s))
        return tbl

    def ball_slot_of(self, pouch_node):
        uid = self.uid(pouch_node)
        for s, (u, _) in self.ball_table().items():
            if u == uid:
                return s
        return None

    def ball_lives(self, pouch_node):
        s = self.ball_slot_of(pouch_node)
        return None if s is None else self.ball_table()[s][1]

    STATE_RX = re.compile(r"State: (.+) -> (.+) \(up=(\d+)\)")

    def ball_transitions(self, t0=-1.0, t1=1e9):
        """[(t, from, to)] from the ball's 'State:' lines."""
        out = []
        for ln in self.log(BALL):
            m = self.STATE_RX.search(ln.text)
            if m and t0 <= ln.t <= t1:
                out.append((ln.t, m.group(1), m.group(2)))
        return out

    def ball_state(self, t=None):
        cur = "Off"
        for tt, _, to in self.ball_transitions():
            if t is not None and tt > t:
                break
            cur = to
        return cur

    def ball_state_times(self, state):
        """Times at which the ball entered `state`."""
        return [t for t, _, to in self.ball_transitions() if to == state]

    def ball_path(self, t0=-1.0, t1=1e9):
        return [to for _, _, to in self.ball_transitions(t0, t1)]

    def leds(self, node):
        """[(t, mask)] LED level changes."""
        out = []
        t = ctypes.c_int64()
        for i in range(self.lib.sim_led_count(node)):
            m = self.lib.sim_led_get(node, i, ctypes.byref(t))
            out.append((t.value / PS, m))
        return out

    def led_at(self, node, t=None):
        t = self.now if t is None else t
        cur = 0
        for tt, m in self.leds(node):
            if tt > t:
                break
            cur = m
        return cur

    def led_masks(self, node, t0, t1):
        """The set of LED masks shown in [t0, t1]."""
        seen = {self.led_at(node, t0)}
        for tt, m in self.leds(node):
            if t0 < tt <= t1:
                seen.add(m)
        return seen

    def ranges(self, t0=-1.0, t1=1e9):
        """The ball's ranging rounds: [{'t', 'ret', 'by_slot': {slot: (uid, mm, lives)}}]."""
        n = self.lib.sim_range_count(BALL)
        if n == 0:
            return []
        base = self.lib.sim_range_base(BALL)
        arr = (RangeRec * n).from_address(base)
        out = []
        for r in arr:
            t = r.t / PS
            if t < t0 or t > t1:
                continue
            out.append({"t": t, "ret": r.ret,
                        "by_slot": {r.slot[i]: (r.uid[i], r.mm[i], r.lives[i]) for i in range(r.count)}})
        return out

    def frames(self, types=None, t0=-1.0, t1=1e9, src=None):
        """Frames on the air (all, in start order). types: a name or a collection of names."""
        n = self.lib.sim_trace_count()
        if self._frames is None or self._frames_n != n:
            if n:
                base = self.lib.sim_trace_base()
                arr = (TraceRec * n).from_address(base)
                self._frames = [Frame(i, arr[i]) for i in range(n)]
            else:
                self._frames = []
            self._frames_n = n
        if isinstance(types, str):
            types = (types,)
        return [f for f in self._frames
                if t0 <= f.t <= t1 and (types is None or f.type in types) and (src is None or f.src == src)]

    def overlapping_frames(self, t0=-1.0, t1=1e9, ignore=("JOIN",)):
        """Pairs of frames on the air at the same time (anything but JOINs is scheduled not to overlap)."""
        fs = [f for f in self.frames(None, t0, t1) if f.type not in ignore]
        pairs = []
        for i, a in enumerate(fs):
            for b in fs[i + 1:]:
                if b.t >= a.t_end:
                    break
                if a.src != b.src:
                    pairs.append((a, b))
        return pairs

    def stat(self, name):
        return int(self.lib.sim_stat({"frames": 0, "collisions": 1}[name]))

    # ---- diagnostics -----------------------------------------------------------------------------
    def timeline(self, nodes=(BALL,), t0=None, t1=None, last=40, air=False, types=None, status=False):
        """The merged log (and optionally air) of some nodes in [t0, t1], as text (STATUS lines left out)."""
        rows = []
        for n in nodes:
            for ln in self.log(n):
                if not status and ln.text.startswith("STATUS "):
                    continue
                if (t0 is None or ln.t >= t0) and (t1 is None or ln.t <= t1):
                    rows.append((ln.t, 0, "%9.4f  n%d %s" % (ln.t, n, ln.text)))
        if air:
            for f in self.frames(types, t0 if t0 is not None else -1.0, t1 if t1 is not None else 1e9):
                rows.append((f.t, 1, repr(f)))
        rows.sort(key=lambda r: (r[0], r[1]))
        if last and len(rows) > last:
            rows = rows[-last:]
        return "\n".join(r[2] for r in rows)

    def fail(self, msg, nodes=None, t0=None, t1=None, last=40, air=False, types=None):
        nodes = nodes if nodes is not None else [BALL] + [n for n in range(1, 10) if self.booted(n)]
        raise Fail("%s\n--- log excerpt (virtual time %.3f s, nodes %s) ---\n%s"
                   % (msg, self.now, nodes, self.timeline(nodes, t0, t1, last, air, types)))

    def check(self, cond, msg, **kw):
        if not cond:
            self.fail(msg, **kw)

    def check_eq(self, got, want, msg, **kw):
        if got != want:
            self.fail("%s: got %r, expected %r" % (msg, got, want), **kw)

    def note(self, text):
        self.notes.append(text)
        print("  note: " + text)
