#!/usr/bin/env python3
"""Replay every accelerometer recording in data/ through the C classifier and summarize.

Builds tests/accel_replay.c (the same ball/src/accel_class.c as the firmware) with the host C
compiler, feeds it each segment found by tools/accel_trace.py and prints
  1. the raw x/y/z recordings (data/accel_*.csv, decimated 400 Hz -> 200 Hz): time per level,
     level changes, hits, the shortest level (flicker check);
  2. every BURST window of data/ball_*.csv: the level just before the crossing, the Hit reported,
     the level 250 ms after, and what the ball's own STATUS/old gate said at that time (and, once
     there are any, every LOG_ACCEL_RAW capture, like a raw file);
  3. DISAGREEMENTS: windows where the classifier contradicts that context. Most are expected (the
     old firmware had no notion of Spinning); each line says why it is listed. Read them, do not
     treat them as failures.

Usage:  python3 -I tools/accel_replay_data.py [DATA_DIR] [--cc CC] [-v]
        -v also prints every level change of every segment.
Needs a C compiler (cc). docs/Accelerometer.md explains how to read the output.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import accel_trace as trace  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
LEVEL_RE = re.compile(r"^(\d+) ms LEVEL (\w+) -> (\w+) \((\w+) lasted (\d+) ms\)")
HIT_RE = re.compile(r"^(\d+) ms HIT (\d+) mg")
SUMMARY_RE = re.compile(r"^SUMMARY (.*)$")
FLICKER_MS = 100   # a level shorter than this is reported as a flicker ...
WARMUP_MS = 300    # ... unless it started in the first WARMUP_MS of a segment (the classifier starts cold)


def build(cc, out_dir):
    exe = os.path.join(out_dir, "accel_replay")
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + os.path.join(ROOT, "shared"),
           "-I" + os.path.join(ROOT, "ball", "src"), os.path.join(ROOT, "tests", "accel_replay.c"),
           os.path.join(ROOT, "ball", "src", "accel_class.c"), "-o", exe]
    subprocess.run(cmd, check=True)
    return exe


def replay(exe, samples):
    """Run the classifier over [(t_ms, mg, ...)]; returns dict with levels, hits, summary."""
    text = "".join(f"{s[0]} {s[1]}\n" for s in samples)
    out = subprocess.run([exe], input=text, capture_output=True, text=True, check=True).stdout
    t0 = samples[0][0]
    res = {"changes": [], "hits": [], "summary": {}}
    for line in out.splitlines():
        m = LEVEL_RE.match(line)
        if m:
            res["changes"].append((t0 + int(m.group(1)), m.group(2), m.group(3), int(m.group(5))))
        m = HIT_RE.match(line)
        if m:
            res["hits"].append((t0 + int(m.group(1)), int(m.group(2))))
        m = SUMMARY_RE.match(line)
        if m:
            res["summary"] = {k: int(v) for k, v in (kv.split("=") for kv in m.group(1).split())}
    return res


def level_at(res, t):
    """The level at absolute time t (UNKNOWN before the first change)."""
    level = "UNKNOWN"
    for when, _, to, _ in res["changes"]:
        if when <= t:
            level = to
    return level


def is_flicker(left, entered, lasted, at_ms):
    """A level that was left again after less than FLICKER_MS. Not counted: UNKNOWN (warm-up, or the reset
    after a gap in the data), and levels that began while the classifier was still starting up."""
    return (lasted < FLICKER_MS and "UNKNOWN" not in (left, entered) and at_ms - lasted >= WARMUP_MS)


def raw_section(exe, data_dir, verbose, flickers):
    print("== Raw x/y/z recordings (decimated to 200 Hz)")
    print(f"{'file':<26}{'span s':>7}{'REST%':>7}{'HELD%':>7}{'SPIN%':>7}{'chg':>5}{'hits':>6}{'supp':>6}{'shortest ms':>13}")
    for path in sorted(glob.glob(os.path.join(data_dir, "accel_*.csv"))):
        raw = trace.load_raw(path, 2)
        res = replay(exe, raw)
        s = res["summary"]
        span = max(1, s["span"])
        print(f"{os.path.basename(path):<26}{span / 1000:>7.1f}{100 * s['REST'] / span:>7.0f}"
              f"{100 * s['HELD'] / span:>7.0f}{100 * s['SPINNING'] / span:>7.0f}{s['changes']:>5}"
              f"{s['hits']:>6}{s['suppressed']:>6}{s['shortest']:>13}")
        for when, a, b, lasted in res["changes"]:
            if is_flicker(a, b, lasted, when - raw[0][0]):
                flickers.append(f"{os.path.basename(path)}: {a} lasted {lasted} ms before {when - raw[0][0]} ms")
            if verbose:
                print(f"    {when - raw[0][0]:>7} ms  {a} -> {b}  ({a} lasted {lasted} ms)")


def raw_capture_section(exe, data_dir, verbose, flickers):
    """LOG_ACCEL_RAW captures inside ball_*.csv (none recorded yet): replayed like the raw files."""
    caps = [c for path in sorted(glob.glob(os.path.join(data_dir, "ball_*.csv")))
            for c in trace.parse_raw_captures(path) if c["complete"] and c["samples"]]
    if not caps:
        return
    print("\n== RAW captures of data/ball_*.csv (LOG_ACCEL_RAW)")
    print(f"{'capture':<30}{'span s':>7}{'REST%':>7}{'HELD%':>7}{'SPIN%':>7}{'chg':>5}{'hits':>6}")
    for c in caps:
        res = replay(exe, c["samples"])
        s = res["summary"]
        span = max(1, s["span"])
        print(f"{c['file'][5:-4] + '#' + str(c['seq']):<30}{span / 1000:>7.1f}{100 * s['REST'] / span:>7.0f}"
              f"{100 * s['HELD'] / span:>7.0f}{100 * s['SPINNING'] / span:>7.0f}{s['changes']:>5}{s['hits']:>6}")
        for when, a, b, lasted in res["changes"]:
            if is_flicker(a, b, lasted, when - c["samples"][0][0]):
                flickers.append(f"{c['file']}#{c['seq']}: {a} lasted {lasted} ms")
            if verbose:
                print(f"    {when - c['samples'][0][0]:>7} ms  {a} -> {b}  ({a} lasted {lasted} ms)")


def status_context(status, up):
    """The latest STATUS line at or before `up` (within 600 ms), or None."""
    best = None
    for s in status:
        if s["up"] <= up and up - s["up"] <= 600:
            best = s
    return best


def burst_section(exe, data_dir, verbose, disagreements, flickers):
    print("\n== BURST windows (t=0 is the threshold crossing; logged outcome: the old free-fall gate said "
          "accepted/rejected, the classifier firmware says hit/cooldown)")
    print(f"{'window':<24}{'logged':>9}{'trig mg':>8}{'ball state':>12}{'before':>10}{'hit':>5}{'+250 ms':>10}")
    for path in sorted(glob.glob(os.path.join(data_dir, "ball_*.csv"))):
        status = trace.parse_status(path)
        for b in trace.parse_bursts(path):
            if not b["complete"]:
                continue
            samples = b["samples"]
            t = b["trigger_up"]
            res = replay(exe, samples)
            before = level_at(res, t - 1)
            after = level_at(res, t + 250)
            hit = any(abs(h[0] - t) <= 40 for h in res["hits"])
            ctx = status_context(status, t)
            state = ctx["state"] if ctx else "?"
            name = f"{b['file'][5:-4]}/r{b['run']}#{b['seq']}"
            print(f"{name:<24}{b['outcome']:>9}{b['trigger_mg']:>8}{state:>12}{before:>10}"
                  f"{'yes' if hit else 'NO':>5}{after:>10}")
            if verbose:
                for when, a, c, lasted in res["changes"]:
                    print(f"    {when - t:>+5} ms  {a} -> {c}  ({a} lasted {lasted} ms)")
            for when, a, c, lasted in res["changes"]:
                if is_flicker(a, c, lasted, when - samples[0][0]):
                    flickers.append(f"{name}: {a} lasted {lasted} ms at {when - t:+d} ms")
            if not hit:
                disagreements.append(f"{name}: crossing of {b['trigger_mg']} mg was not reported as a Hit "
                                     "(the first 500 ms after a window starts are settling time)")
            if b["outcome"] == "accepted" and before != "SPINNING":
                disagreements.append(
                    f"{name}: the old gate saw |a| < 700 mg for 30 ms just before the impact but the classifier "
                    f"says {before}: the dip was part of a rough (hand) motion, or the window starts too late "
                    "for the classifier to have settled")
            if state in ("Armed", "Thrown") and before != "SPINNING":
                disagreements.append(
                    f"{name}: ball state was {state} (thrower out of range, so flying) but the classifier "
                    f"says {before} before the impact (a drifting ball near 1 g without free fall or a "
                    "preceding impact in the window is not recognized, see docs/Accelerometer.md)")
            if before == "SPINNING" and state in ("Ready", "Disarmed", "Unheld"):
                disagreements.append(
                    f"{name}: classifier says SPINNING before the impact but the ball state was {state} "
                    "(a toss or short throw with the thrower still within range, or a bounce after a hit)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data_dir", nargs="?", default=os.path.join(ROOT, "data"))
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("-v", action="store_true", help="print every level change")
    args = ap.parse_args(argv)
    disagreements, flickers = [], []
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(args.cc, tmp)
        raw_section(exe, args.data_dir, args.v, flickers)
        burst_section(exe, args.data_dir, args.v, disagreements, flickers)
        raw_capture_section(exe, args.data_dir, args.v, flickers)
    print(f"\n== Levels shorter than {FLICKER_MS} ms (flicker): {len(flickers)}")
    for line in flickers:
        print("  " + line)
    print(f"\n== DISAGREEMENTS with the ball's own state / old gate: {len(disagreements)}")
    for line in disagreements:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
