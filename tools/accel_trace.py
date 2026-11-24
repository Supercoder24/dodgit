#!/usr/bin/env python3
"""Extract accelerometer traces from the recordings in data/ and summarize them.

The ball classifier (ball/src/accel_class.c, docs/Accelerometer.md) is tuned and tested on recorded
traces. This script turns the recordings into plain text traces, one sample per line:

    t_ms mg            |a| in milli-g (BURST windows, one file per window)
    t_ms mg x y z      raw x/y/z recordings, |a| computed

with '#' header lines ("# key=value ..." metadata, then a "# t_ms mg ..." column line), and prints
per-segment statistics (mean, standard deviation, peak-to-peak, largest sample-to-sample jump, and
the classifier's own features), which is how the thresholds were characterized.

Sources (written by tools/ball_monitor.py and the 2026-11-03 accelerometer logger):
  data/ball_*.csv    BURST lines: |a| samples (mg, clipped at 32767) around a threshold crossing,
                     ball firmware with LOG_ACCEL_BURST on. The header gives the time of the crossing;
                     each data line gives the time of its first and last sample, so the times in
                     between are interpolated (a line holds 16 samples, about 100 ms).
  data/ball_*.csv    RAW captures (LOG_ACCEL_RAW, x/y/z history dumped on request): none recorded yet.
  data/ball_*.csv    STATUS lines every 250 ms: peak (and, in the newest recordings, min) |a| and
                     still= (ms since the ball last moved). Used to find resting stretches only.
  data/accel_*.csv   raw x/y/z at 400 Hz (t_us,x_mg,y_mg,z_mg,dropped).

Usage:
  python3 -I tools/accel_trace.py summary [DATA_DIR]            statistics of every segment
  python3 -I tools/accel_trace.py extract OUT_DIR [DATA_DIR]    one text trace per BURST / raw file
  python3 -I tools/accel_trace.py status [DATA_DIR]             resting stretches found in STATUS lines
  python3 -I tools/accel_trace.py fixtures OUT_DIR [DATA_DIR]   regenerate tests/fixtures/accel/ (only from the
                                                                files named in FIXTURE_BALL_FILES and RAW_FIXTURES)
  python3 -I tools/accel_trace.py features [DATA_DIR]           the classifier's features per labeled segment
--decimate N keeps every Nth raw sample (2 = 400 Hz -> 200 Hz, what the tests use).
"""
import argparse
import csv
import glob
import math
import os
import re
import sys

# Two header formats: the 2026-11-19 firmware ("rejected ffage=1012 ms min150=1016 mg", the old free-fall
# gate's verdict) and the classifier firmware ("hit level=SPINNING" / "cooldown level=..").
BURST_HEADER = re.compile(
    r"^BURST #(\d+) trigger up=(\d+) mg=(\d+) (\w+)(?: ffage=-?\d+ ms min150=-?\d+ mg| level=(\w+)) "
    r"also=(\d+) samples=(\d+) pre=(\d+)")
BURST_DATA = re.compile(r"^BURST #(\d+) ([+-]\d+)\.\.([+-]\d+) ms:((?: -?\d+)+)\s*$")
RAW_HEADER = re.compile(r"^RAW #(\d+) begin up=(\d+) rate=(\d+) samples=(\d+)")
RAW_DATA = re.compile(r"^RAW #(\d+) (\d+):((?: -?\d+,-?\d+,-?\d+)+)\s*$")
RAW_END = re.compile(r"^RAW #(\d+) end")
STATUS = re.compile(
    r"^STATUS (\d+) state=(.*?) pouches=(\d+) peak=(-?\d+) mg(?: min=(-?\d+) mg)?"
    r"(?: still=(\d+) ms)?")

# Raw recordings copied into tests/fixtures/accel/ by the "fixtures" command:
# (fixture name, data file, first second, last second or None for the end, what it shows).
# The recordings are unlabeled; what they show was inferred from the statistics (docs/Accelerometer.md).
RAW_FIXTURES = [
    ("raw_134755", "accel_20261103_134755.csv", 0, None,
     "rest 0-5 s, shaken by hand 5-10 s (held), rest again from 11 s"),
    ("raw_140116", "accel_20261103_140116.csv", 0, None,
     "rest 3-4.7 s, picked up at ~4.7 s, tossed at ~6.1 s"),
    ("raw_135212", "accel_20261103_135212.csv", 0, None,
     "tossed and bounced 1-8 s, lying still from 8.5 s"),
    ("raw_135646", "accel_20261103_135646.csv", 0, None,
     "handled and bounced; 1.7 s of samples were lost (BLE) at about 9 s"),
    ("raw_135847_rest", "accel_20261103_135847.csv", 24, 32,
     "lying still for 8 s on the noisiest board (sd 38 mg)"),
]
STATUS_FIXTURE = "status_rest"
# The ball logs the BURST fixtures and the resting STATUS stretches come from. Only these files: a new
# recording in data/ must not change tests/fixtures/accel/ (and so break the committed-fixtures test)
# until someone adds it here together with a test that uses it (docs/Accelerometer.md, "Adding traces").
FIXTURE_BALL_FILES = ["ball_20261117_154719.csv", "ball_20261119_150326.csv", "ball_20261119_150548.csv"]


def _line(row):
    """The log line of a ball_*.csv row (host_utc,category,line)."""
    return row[2] if len(row) >= 3 else ""


def parse_bursts(path):
    """Return the BURST windows of one ball_*.csv as dicts.

    Keys: file, seq, run, trigger_up (ms), trigger_mg, outcome (rejected / accepted / cooldown, or hit /
    cooldown from the classifier firmware), level (the classifier's level at the crossing, None in the
    older recordings), also, expected, samples [(t_ms, mg)]
    with t_ms on the firmware's uptime clock (so one window is directly replayable), complete (all data
    lines arrived: the ball drops whole bursts when its queue is full, never single lines).
    `run` counts ball reboots inside one file: the sequence number restarts at 1 after a reboot.
    """
    bursts = []
    cur = None
    run, last_seq = 1, 0
    with open(path, newline="") as f:
        for row in csv.reader(f):
            text = _line(row)
            m = BURST_HEADER.match(text)
            if m:
                seq = int(m.group(1))
                if seq <= last_seq:
                    run += 1
                last_seq = seq
                cur = {
                    "file": os.path.basename(path), "seq": seq, "run": run,
                    "trigger_up": int(m.group(2)), "trigger_mg": int(m.group(3)),
                    "outcome": m.group(4), "level": m.group(5), "also": int(m.group(6)),
                    "expected": int(m.group(7)), "samples": [],
                }
                bursts.append(cur)
                continue
            m = BURST_DATA.match(text)
            if m and cur is not None and int(m.group(1)) == cur["seq"]:
                first, last = int(m.group(2)), int(m.group(3))
                values = [int(v) for v in m.group(4).split()]
                n = len(values)
                for k, mg in enumerate(values):
                    # The line states the time of its first and last sample; the ones between
                    # are interpolated (the sampling jitter inside a line is lost).
                    off = first if n == 1 else first + (last - first) * k / (n - 1)
                    cur["samples"].append((cur["trigger_up"] + round(off), mg))
    for b in bursts:
        b["complete"] = len(b["samples"]) == b["expected"]
    return bursts


def parse_raw_captures(path):
    """Return the LOG_ACCEL_RAW captures of one ball_*.csv (ball/src/accel_log.h documents the lines).

    A capture is "RAW #n begin up=<ms> rate=<Hz> samples=<n>", data lines "RAW #n <t_off>: x,y,z x,y,z
    ..." and "RAW #n end". t_off is the offset (ms after up=) of the first sample of the line, the next
    ones are 1000/rate ms apart. Returns dicts with file, seq, run, up, rate, expected, samples
    [(t_ms, mg, x, y, z)] on the uptime clock, and complete (all samples and the end line arrived).
    """
    caps = []
    cur = None
    run, last_seq = 1, 0
    with open(path, newline="") as f:
        for row in csv.reader(f):
            text = _line(row)
            m = RAW_HEADER.match(text)
            if m:
                seq = int(m.group(1))
                if seq <= last_seq:
                    run += 1
                last_seq = seq
                cur = {"file": os.path.basename(path), "seq": seq, "run": run, "up": int(m.group(2)),
                       "rate": int(m.group(3)), "expected": int(m.group(4)), "samples": [], "ended": False}
                caps.append(cur)
                continue
            m = RAW_DATA.match(text)
            if m and cur is not None and int(m.group(1)) == cur["seq"]:
                period = 1000 / cur["rate"] if cur["rate"] else 0
                for k, xyz in enumerate(m.group(3).split()):
                    x, y, z = (int(v) for v in xyz.split(","))
                    cur["samples"].append((cur["up"] + round(int(m.group(2)) + k * period),
                                           int(math.sqrt(x * x + y * y + z * z)), x, y, z))
                continue
            m = RAW_END.match(text)
            if m and cur is not None and int(m.group(1)) == cur["seq"]:
                cur["ended"] = True
    for c in caps:
        c["complete"] = c["ended"] and len(c["samples"]) == c["expected"]
    return caps


def parse_status(path):
    """Return the STATUS lines of one ball_*.csv: dicts with up, state, peak, min, still (None if absent)."""
    out = []
    with open(path, newline="") as f:
        for row in csv.reader(f):
            m = STATUS.match(_line(row))
            if m:
                out.append({
                    "up": int(m.group(1)), "state": m.group(2), "pouches": int(m.group(3)),
                    "peak": int(m.group(4)),
                    "min": int(m.group(5)) if m.group(5) else None,
                    "still": int(m.group(6)) if m.group(6) else None,
                })
    return out


def load_raw(path, decimate=1):
    """Return the samples of a raw accel_*.csv as (t_ms, mg, x, y, z); decimate keeps every Nth."""
    out = []
    with open(path, newline="") as f:
        for i, row in enumerate(csv.DictReader(f)):
            if i % decimate:
                continue
            x, y, z = int(row["x_mg"]), int(row["y_mg"]), int(row["z_mg"])
            out.append((int(row["t_us"]) // 1000, int(math.sqrt(x * x + y * y + z * z)), x, y, z))
    return out


def stats(values):
    """Mean, population standard deviation, peak-to-peak and largest consecutive jump of values."""
    n = len(values)
    if n == 0:
        return {"n": 0, "mean": 0.0, "sd": 0.0, "p2p": 0, "jump": 0, "min": 0, "max": 0}
    mean = sum(values) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in values) / n)
    jump = max((abs(b - a) for a, b in zip(values, values[1:])), default=0)
    return {"n": n, "mean": mean, "sd": sd, "p2p": max(values) - min(values), "jump": jump,
            "min": min(values), "max": max(values)}


def window_stats(samples, start_ms, end_ms):
    """stats() of the |a| of the (t_ms, mg, ...) samples with start_ms <= t < end_ms."""
    return stats([s[1] for s in samples if start_ms <= s[0] < end_ms])


def resting_stretches(status, max_range=150, min_still_ms=1000, min_len_ms=2000):
    """Find stretches of STATUS lines that look like a ball lying still.

    A stretch is a run of lines with still >= min_still_ms (the ball has not moved for a second),
    peak <= 1250 mg and, where the line has a min, peak - min <= max_range (only sensor noise inside
    the 250 ms window). Returns lists of status dicts, each spanning at least min_len_ms.
    """
    out, run = [], []
    prev = None
    for s in status:
        ok = (s["still"] is not None and s["still"] >= min_still_ms and s["peak"] <= 1250 and
              (s["min"] is None or s["peak"] - s["min"] <= max_range))
        if ok and not (prev is not None and s["up"] < prev):  # a reboot restarts the clock
            run.append(s)
        else:
            if run and run[-1]["up"] - run[0]["up"] >= min_len_ms:
                out.append(run)
            run = [s] if ok else []
        prev = s["up"]
    if run and run[-1]["up"] - run[0]["up"] >= min_len_ms:
        out.append(run)
    return out


def write_trace(path, samples, meta, columns="t_ms mg"):
    """Write (t_ms, mg[, x, y, z]) samples as text, t_ms relative to the first sample.

    meta is a dict written as one "# key=value ..." line (values without spaces) after a free-text
    first line it may carry under the key "source".
    """
    t0 = samples[0][0]
    with open(path, "w") as f:
        f.write(f"# source: {meta.get('source', '')}\n")
        f.write("# " + " ".join(f"{k}={v}" for k, v in meta.items() if k != "source") + "\n")
        f.write(f"# {columns}\n")
        for s in samples:
            f.write(f"{s[0] - t0} " + " ".join(str(v) for v in s[1:]) + "\n")


def read_trace(path):
    """Read a trace written by write_trace(): returns ([(t_ms, mg, ...)], {key: int-or-str})."""
    samples, meta = [], {}
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                for token in line[1:].split():
                    if "=" in token:
                        k, v = token.split("=", 1)
                        meta[k] = int(v) if v.lstrip("-").isdigit() else v
            elif line.strip():
                samples.append(tuple(int(v) for v in line.split()))
    return samples, meta


def _data_files(data_dir, pattern):
    return sorted(glob.glob(os.path.join(data_dir, pattern)))


def burst_fixture_name(b):
    stem = os.path.splitext(b["file"])[0].replace("ball_", "")
    return f"burst_{stem}_r{b['run']}_s{b['seq']:02d}"


def write_burst(out_dir, b):
    t0 = b["samples"][0][0]
    name = burst_fixture_name(b)
    write_trace(os.path.join(out_dir, name + ".txt"), b["samples"],
                {"source": f"{b['file']} BURST #{b['seq']} (run {b['run']}), old gate: {b['outcome']}",
                 "trigger_t": b["trigger_up"] - t0, "trigger_mg": b["trigger_mg"],
                 "outcome": b["outcome"]})
    return name


# Segments that tell the classifier's three features apart (docs/Accelerometer.md, "Evidence"):
# (class, label, data file, first, last, unit). first..last is the range in which the 100 ms windows
# END: seconds from the start of a raw file, or ms relative to the crossing for a BURST window
# ("file#seq", run 1 of that file). Plateaus start >= 100 ms after the impact so no window contains it.
EVIDENCE = [
    ("rest", "134755 0-4.5 s", "accel_20261103_134755.csv", 0, 4.5, "s"),
    ("rest", "134755 11-15.3 s", "accel_20261103_134755.csv", 11, 15.3, "s"),
    ("rest", "135212 8.5-10.7 s", "accel_20261103_135212.csv", 8.5, 10.7, "s"),
    ("rest", "135847 24-38.5 s", "accel_20261103_135847.csv", 24, 38.5, "s"),
    ("rest", "150548#6 -700..-30 ms", "ball_20261119_150548.csv#6", -700, -30, "ms"),
    ("drift flight", "150548#8 -480..-30 ms", "ball_20261119_150548.csv#8", -480, -30, "ms"),
    ("drift flight", "150548#3 -575..-10 ms", "ball_20261119_150548.csv#3", -575, -10, "ms"),
    ("flat 3.8 g", "150548#8 +190..+246 ms", "ball_20261119_150548.csv#8", 190, 246, "ms"),
    ("flat 6.4 g", "150548#3 +140..+190 ms", "ball_20261119_150548.csv#3", 140, 190, "ms"),
    ("flat 7.7 g", "150548#14 +170..+250 ms", "ball_20261119_150548.csv#14", 170, 250, "ms"),
    ("flat 1.26 g", "150548#20 -240..-60 ms", "ball_20261119_150548.csv#20", -240, -60, "ms"),
    ("free fall", "150548#25 -450..-120 ms", "ball_20261119_150548.csv#25", -450, -120, "ms"),
    ("free fall", "150548#27 -450..-120 ms", "ball_20261119_150548.csv#27", -450, -120, "ms"),
    ("free fall", "135409 2.5-3.0 s", "accel_20261103_135409.csv", 2.5, 3.0, "s"),
    ("shaken (held)", "134755 5.3-9.9 s", "accel_20261103_134755.csv", 5.3, 9.9, "s"),
    ("handled (inferred)", "135847 14-24 s", "accel_20261103_135847.csv", 14, 24, "s"),
    ("handled (inferred)", "135409 25-37 s", "accel_20261103_135409.csv", 25, 37, "s"),
    ("handled (inferred)", "150326#1 -700..-60 ms", "ball_20261119_150326.csv#1", -700, -60, "ms"),
]


def window_features(samples, window_ms=100, min_samples=4):
    """The classifier's features of every window of `window_ms` ending at a sample.

    Same definitions as ball/src/accel_class.c, in floating point: samples are (t_ms, mg, ...);
    yields (t_ms, mean, masd, trend) where masd is the mean absolute sample-to-sample difference and
    trend the absolute difference between the mean of the newer and the mean of the older half.
    """
    out = []
    first = 0
    for i in range(len(samples)):
        while samples[i][0] - samples[first][0] > window_ms:
            first += 1
        w = [s[1] for s in samples[first:i + 1]]
        if len(w) < min_samples or samples[i][0] - samples[first][0] < window_ms / 2:
            continue
        half = len(w) // 2
        masd = sum(abs(a - b) for a, b in zip(w, w[1:])) / (len(w) - 1)
        trend = abs(sum(w[-half:]) / half - sum(w[:half]) / half)
        out.append((samples[i][0], sum(w) / len(w), masd, trend))
    return out


def _quantiles(values, qs=(0.1, 0.5, 0.9, 0.99)):
    v = sorted(values)
    return [v[min(len(v) - 1, int(q * len(v)))] for q in qs]


def cmd_features(args):
    print("classifier features over sliding 100 ms windows: quantiles p10 / p50 / p90 / p99")
    print(f"{'class':<19}{'segment':<24}{'windows':>8}{'mean mg':>22}{'masd mg':>22}{'trend mg':>22}")
    bursts = {}
    for path in _data_files(args.data_dir, "ball_*.csv"):
        for b in parse_bursts(path):
            if b["complete"] and b["run"] == 1:
                bursts[(b["file"], b["seq"])] = b
    for cls, label, source, first, last, unit in EVIDENCE:
        if "#" in source:
            name, seq = source.split("#")
            b = bursts.get((name, int(seq)))
            if b is None:
                continue
            samples = [(t - b["trigger_up"], mg) for t, mg in b["samples"]]
            lo, hi = first, last
        else:
            path = os.path.join(args.data_dir, source)
            if not os.path.exists(path):
                continue
            raw = load_raw(path, 2)
            samples = [(s[0] - raw[0][0], s[1]) for s in raw]
            lo, hi = first * 1000, last * 1000
        # Windows may reach back before `lo` (they need 100 ms of history), but must end inside it.
        feats = [f for f in window_features(samples) if lo <= f[0] <= hi]
        if not feats:
            continue
        cols = [" ".join(f"{q:>5.0f}" for q in _quantiles([f[k] for f in feats])) for k in (1, 2, 3)]
        print(f"{cls:<19}{label:<24}{len(feats):>8}" + "".join(f"{c:>22}" for c in cols))


def cmd_summary(args):
    print("BURST windows (|a| around a threshold crossing; t=0 is the crossing)")
    print(f"{'window':<28}{'old gate':>9}{'trig mg':>9}{'n':>5}{'dt ms':>7}"
          f"{'-500..-50: mean/sd/p2p/jump':>30}{'+50..+250: mean/sd':>22}")
    for path in _data_files(args.data_dir, "ball_*.csv"):
        for b in parse_bursts(path):
            s = b["samples"]
            if not s or not b["complete"]:
                continue
            dt = (s[-1][0] - s[0][0]) / max(1, len(s) - 1)
            t = b["trigger_up"]
            pre = window_stats(s, t - 500, t - 50)
            post = window_stats(s, t + 50, t + 250)
            print(f"{burst_fixture_name(b)[6:]:<28}{b['outcome']:>9}{b['trigger_mg']:>9}{len(s):>5}"
                  f"{dt:>7.1f}{pre['mean']:>12.0f}{pre['sd']:>6.0f}{pre['p2p']:>6.0f}{pre['jump']:>6.0f}"
                  f"{post['mean']:>11.0f}{post['sd']:>6.0f}")
    print("\nRaw x/y/z recordings (whole file, 250 ms windows: distribution of the window sd)")
    for path in _data_files(args.data_dir, "accel_*.csv"):
        raw = load_raw(path, args.decimate)
        whole = stats([s[1] for s in raw])
        sds = sorted(window_stats(raw, a, a + 250)["sd"] for a in range(raw[0][0], raw[-1][0] - 250, 250))
        q = (lambda p: sds[min(len(sds) - 1, int(p * len(sds)))]) if sds else (lambda p: 0)
        print(f"{os.path.basename(path):<26}{len(raw):>6} samples {(raw[-1][0] - raw[0][0]) / 1000:>5.1f} s"
              f"  mean {whole['mean']:>5.0f} max {whole['max']:>6}  window sd p10/p50/p90 "
              f"{q(0.1):.0f}/{q(0.5):.0f}/{q(0.9):.0f}")


def cmd_status(args):
    for path in _data_files(args.data_dir, "ball_*.csv"):
        for run in resting_stretches(parse_status(path)):
            ranges = [s["peak"] - s["min"] for s in run if s["min"] is not None]
            print(f"{os.path.basename(path)}: up {run[0]['up']}..{run[-1]['up']} ms "
                  f"({(run[-1]['up'] - run[0]['up']) / 1000:.1f} s, {len(run)} STATUS lines, "
                  f"peak {min(s['peak'] for s in run)}..{max(s['peak'] for s in run)} mg, "
                  f"peak-min <= {max(ranges) if ranges else 'n/a'}, still up to {run[-1]['still']} ms)")


def write_raw_capture(out_dir, c, decimate=1):
    """Write one LOG_ACCEL_RAW capture (t_ms mg x y z); returns the trace name."""
    name = f"{os.path.splitext(c['file'])[0]}_raw_r{c['run']}_c{c['seq']:02d}"
    write_trace(os.path.join(out_dir, name + ".txt"), c["samples"][::decimate],
                {"source": f"{c['file']} RAW #{c['seq']} (run {c['run']}), {c['rate']} Hz"}, "t_ms mg x y z")
    return name


def write_ball_fixtures(out_dir, paths):
    """Write every complete BURST window and RAW capture of the given ball_*.csv files; returns the names."""
    names = []
    for path in paths:
        for b in parse_bursts(path):
            if b["complete"]:
                names.append(write_burst(out_dir, b))
        for c in parse_raw_captures(path):
            if c["complete"] and c["samples"]:
                names.append(write_raw_capture(out_dir, c))
    return names


def cmd_extract(args):
    os.makedirs(args.out_dir, exist_ok=True)
    count = 0
    for path in _data_files(args.data_dir, "ball_*.csv"):
        for b in parse_bursts(path):
            if b["complete"]:
                write_burst(args.out_dir, b)
                count += 1
        for c in parse_raw_captures(path):
            if c["complete"] and c["samples"]:
                write_raw_capture(args.out_dir, c, args.decimate)
                count += 1
    for path in _data_files(args.data_dir, "accel_*.csv"):
        raw = load_raw(path, args.decimate)
        stem = os.path.splitext(os.path.basename(path))[0]
        write_trace(os.path.join(args.out_dir, stem + ".txt"), raw,
                    {"source": f"{os.path.basename(path)} raw, every {args.decimate}. sample"},
                    "t_ms mg x y z")
        count += 1
    print(f"wrote {count} traces to {args.out_dir}")


def cmd_fixtures(args):
    """Write the small set of traces the host tests need (tests/fixtures/accel/)."""
    os.makedirs(args.out_dir, exist_ok=True)
    ball_files = [os.path.join(args.data_dir, f) for f in FIXTURE_BALL_FILES]
    names = write_ball_fixtures(args.out_dir, ball_files)
    for name, filename, first_s, last_s, what in RAW_FIXTURES:
        raw = load_raw(os.path.join(args.data_dir, filename), 2)  # 400 Hz -> 200 Hz
        t0 = raw[0][0]
        seg = [s for s in raw if s[0] - t0 >= first_s * 1000 and (last_s is None or s[0] - t0 < last_s * 1000)]
        write_trace(os.path.join(args.out_dir, name + ".txt"), [s[:2] for s in seg],
                    {"source": f"{filename} {first_s}..{last_s if last_s is not None else 'end'} s, every 2nd sample: {what}"})
        names.append(name)
    # STATUS lines of lying-still stretches: one "t_ms min_mg peak_mg" row per line (the 250 ms before
    # it). Older recordings have no min; the 100 mg band assumed for them is wider than any rest
    # noise measured (docs/Accelerometer.md).
    rows = []
    for path in ball_files:
        for run in resting_stretches(parse_status(path)):
            for s in run:
                rows.append((os.path.basename(path), s["up"] - run[0]["up"], s["min"] if s["min"] is not None
                             else s["peak"] - 100, s["peak"]))
    with open(os.path.join(args.out_dir, STATUS_FIXTURE + ".txt"), "w") as f:
        f.write("# source: STATUS lines of lying-still stretches (still >= 1 s, peak <= 1250 mg), see "
                "tools/accel_trace.py resting_stretches\n# file t_ms min_mg peak_mg   "
                "(t_ms restarts at 0 for every stretch)\n")
        for r in rows:
            f.write("%s %d %d %d\n" % r)
    print(f"wrote {len(names)} traces and {STATUS_FIXTURE}.txt to {args.out_dir}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name, fn in (("summary", cmd_summary), ("extract", cmd_extract), ("status", cmd_status),
                     ("fixtures", cmd_fixtures), ("features", cmd_features)):
        p = sub.add_parser(name)
        if name in ("extract", "fixtures"):
            p.add_argument("out_dir")
        p.add_argument("data_dir", nargs="?", default=os.path.join(os.path.dirname(__file__), "..", "data"))
        p.add_argument("--decimate", type=int, default=1, help="keep every Nth raw sample")
        p.set_defaults(fn=fn)
    args = parser.parse_args(argv)
    args.fn(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
