import contextlib
import filecmp
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location("accel_trace", ROOT / "tools/accel_trace.py")
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def write_ball_csv(path, lines):
    """A ball_*.csv as tools/ball_monitor.py writes it: host_utc,category,line."""
    with open(path, "w", newline="") as f:
        f.write("host_utc,category,line\n")
        for line in lines:
            f.write(f'2026-11-19T21:00:00+00:00,x,"{line}"\n')


BURST = [
    "BURST #4 trigger up=50000 mg=33708 rejected ffage=1012 ms min150=1016 mg also=0 samples=19 pre=17",
    "BURST #4 -100..+50 ms: " + " ".join(str(1000 + i) for i in range(16)),
    "STATUS 50100 state=Armed pouches=2 peak=1321 mg min=943 mg still=14 ms resets=0 rejected=7 accel_errors=0",
    "BURST #4 +60..+80 ms: 32767 5000 100",
]


class ParseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_burst_times_are_interpolated_on_the_uptime_clock(self):
        write_ball_csv(self.dir / "ball_x.csv", BURST)
        (b,) = trace.parse_bursts(self.dir / "ball_x.csv")
        self.assertEqual((b["seq"], b["trigger_up"], b["trigger_mg"], b["outcome"]), (4, 50000, 33708, "rejected"))
        self.assertTrue(b["complete"])
        s = b["samples"]
        self.assertEqual(len(s), 19)
        self.assertEqual(s[0], (49900, 1000))                  # first sample of a line: its stated time
        self.assertEqual(s[15], (50050, 1015))                 # last sample of the line
        self.assertEqual(s[1][0], 49910)                       # 15 steps of 10 ms in between
        self.assertEqual([x[1] for x in s[16:]], [32767, 5000, 100])
        self.assertEqual((s[16][0], s[18][0]), (50060, 50080))
        self.assertEqual(s[17][0], 50070)                      # 3 samples: the middle one is halfway

    def test_burst_header_of_the_classifier_firmware(self):
        write_ball_csv(self.dir / "ball_x.csv", [
            "BURST #9 trigger up=70000 mg=4100 hit level=SPINNING also=1 samples=3 pre=2",
            "BURST #9 -10..+10 ms: 100 4100 90",
            "BURST #10 trigger up=70500 mg=2600 cooldown level=HELD also=0 samples=1 pre=1",
            "BURST #10 +0..+0 ms: 2600",
        ])
        a, b = trace.parse_bursts(self.dir / "ball_x.csv")
        self.assertEqual((a["outcome"], a["level"], a["also"], a["complete"]), ("hit", "SPINNING", 1, True))
        self.assertEqual((b["outcome"], b["level"], b["complete"]), ("cooldown", "HELD", True))
        self.assertEqual(b["samples"], [(70500, 2600)])

    def test_incomplete_burst_and_reboot_runs(self):
        header = "BURST #%d trigger up=%d mg=2600 accepted ffage=1 ms min150=1 mg also=0 samples=3 pre=1"
        write_ball_csv(self.dir / "ball_x.csv", [
            header % (1, 1000), "BURST #1 -10..+10 ms: 1 2 3",
            header % (2, 2000), "BURST #2 -10..+0 ms: 1 2",          # one sample short
            header % (1, 500), "BURST #1 -10..+10 ms: 4 5 6",         # sequence restarts: ball rebooted
            "BURST #7 -10..+10 ms: 9 9 9",                            # data line without its header
        ])
        bursts = trace.parse_bursts(self.dir / "ball_x.csv")
        self.assertEqual([(b["seq"], b["run"], b["complete"]) for b in bursts],
                         [(1, 1, True), (2, 1, False), (1, 2, True)])
        self.assertEqual([x[1] for x in bursts[2]["samples"]], [4, 5, 6])

    def test_raw_capture_lines(self):
        write_ball_csv(self.dir / "ball_x.csv", [
            "RAW #1 begin up=10000 rate=200 samples=5",
            "RAW #1 0: 0,0,1000 0,0,1010 300,400,0",
            "STATUS 10100 state=Ready pouches=1 peak=1000 mg accel_errors=0",
            "RAW #1 15: 0,0,990 -300,-400,0",
            "RAW #1 end",
            "RAW #2 begin up=20000 rate=200 samples=3",
            "RAW #2 0: 1,2,3 1,2,3 1,2,3",          # the end line never arrived
        ])
        c1, c2 = trace.parse_raw_captures(self.dir / "ball_x.csv")
        self.assertTrue(c1["complete"])
        self.assertEqual([(s[0], s[1]) for s in c1["samples"]],
                         [(10000, 1000), (10005, 1010), (10010, 500), (10015, 990), (10020, 500)])
        self.assertEqual(c1["samples"][2][2:], (300, 400, 0))
        self.assertFalse(c2["complete"])

    def test_status_formats(self):
        write_ball_csv(self.dir / "ball_x.csv", [
            "STATUS 1000 state=Roll Call pouches=1 peak=1014 mg accel_errors=0",
            "STATUS 1250 state=Disarmed pouches=2 peak=1020 mg still=4 ms resets=0 rejected=0 accel_errors=0",
            "STATUS 1500 state=Armed pouches=2 peak=987 mg min=950 mg still=700 ms resets=0 rejected=0 accel_errors=0",
            "State: Armed -> Hit",
        ])
        a, b, c = trace.parse_status(self.dir / "ball_x.csv")
        self.assertEqual((a["state"], a["peak"], a["min"], a["still"]), ("Roll Call", 1014, None, None))
        self.assertEqual((b["peak"], b["min"], b["still"]), (1020, None, 4))
        self.assertEqual((c["state"], c["peak"], c["min"], c["still"]), ("Armed", 987, 950, 700))

    def test_raw_recording_and_decimation(self):
        with open(self.dir / "accel_x.csv", "w") as f:
            f.write("t_us,x_mg,y_mg,z_mg,dropped\n")
            for i in range(6):
                f.write(f"{1000000 + i * 2500},0,0,{1000 + i},0\n")
        full = trace.load_raw(self.dir / "accel_x.csv")
        self.assertEqual([(s[0], s[1], s[4]) for s in full[:2]], [(1000, 1000, 1000), (1002, 1001, 1001)])
        half = trace.load_raw(self.dir / "accel_x.csv", 2)
        self.assertEqual([s[4] for s in half], [1000, 1002, 1004])
        pythagoras = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, dir=self.dir)
        pythagoras.write("t_us,x_mg,y_mg,z_mg,dropped\n5000,300,400,0,0\n")
        pythagoras.close()
        self.assertEqual(trace.load_raw(pythagoras.name)[0][1], 500)


class StatsTests(unittest.TestCase):
    def test_stats(self):
        s = trace.stats([1000, 1100, 900, 1000])
        self.assertEqual((s["n"], s["mean"], s["p2p"], s["jump"], s["min"], s["max"]), (4, 1000.0, 200, 200, 900, 1100))
        self.assertAlmostEqual(s["sd"], 70.71, places=1)
        self.assertEqual(trace.stats([])["n"], 0)

    def test_window_stats_selects_by_time(self):
        samples = [(t, 1000 + (t if t >= 100 else 0)) for t in range(0, 200, 10)]
        self.assertEqual(trace.window_stats(samples, 0, 100)["max"], 1000)
        self.assertEqual(trace.window_stats(samples, 100, 200)["min"], 1100)


class WindowFeatureTests(unittest.TestCase):
    def test_constant_signal_has_no_roughness_and_no_trend(self):
        feats = trace.window_features([(t, 930) for t in range(0, 400, 6)])
        self.assertTrue(feats)
        for _, mean, masd, trend in feats:
            self.assertEqual((mean, masd, trend), (930, 0, 0))

    def test_ramp_has_trend_and_roughness_of_its_slope(self):
        # 10 mg every 5 ms: successive difference 10, the newer half is ~50 mg above the older one
        feats = trace.window_features([(t, 1000 + 2 * t) for t in range(0, 300, 5)])
        _, mean, masd, trend = feats[-1]
        self.assertAlmostEqual(masd, 10)
        self.assertAlmostEqual(trend, 100, delta=10)

    def test_noise_only_has_roughness_but_little_trend(self):
        feats = trace.window_features([(t * 6, 930 + (30 if t % 2 else -30)) for t in range(60)])
        _, _, masd, trend = feats[-1]
        self.assertEqual(masd, 60)
        self.assertLess(trend, 10)

    def test_needs_enough_samples_and_span(self):
        self.assertEqual(trace.window_features([(0, 1), (5, 1), (10, 1)]), [])       # < 4 samples
        self.assertEqual(trace.window_features([(0, 1), (1, 1), (2, 1), (3, 1)]), [])  # < half a window


class RestingStretchTests(unittest.TestCase):
    @staticmethod
    def line(up, peak=1000, mn=950, still=5000):
        return {"up": up, "state": "Ready", "peak": peak, "min": mn, "still": still}

    def test_finds_long_still_stretches_only(self):
        lines = [self.line(t) for t in range(0, 3000, 250)]                 # 2.75 s: counts
        lines += [self.line(t, still=10) for t in range(3000, 4000, 250)]    # moving
        lines += [self.line(t) for t in range(4000, 5000, 250)]              # too short
        runs = trace.resting_stretches(lines)
        self.assertEqual(len(runs), 1)
        self.assertEqual((runs[0][0]["up"], runs[0][-1]["up"]), (0, 2750))

    def test_noise_range_and_level_matter(self):
        wide = [self.line(t, peak=1100, mn=900) for t in range(0, 3000, 250)]    # 200 mg range
        self.assertEqual(trace.resting_stretches(wide), [])
        high = [self.line(t, peak=2000, mn=1950) for t in range(0, 3000, 250)]  # not about 1 g
        self.assertEqual(trace.resting_stretches(high), [])
        no_min = [self.line(t, mn=None) for t in range(0, 3000, 250)]            # older recordings
        self.assertEqual(len(trace.resting_stretches(no_min)), 1)

    def test_reboot_splits_a_stretch(self):
        lines = [self.line(t) for t in range(0, 2500, 250)] + [self.line(t) for t in range(0, 2500, 250)]
        self.assertEqual(len(trace.resting_stretches(lines)), 2)


class TraceFileTests(unittest.TestCase):
    def test_write_and_read_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "t.txt")
            trace.write_trace(path, [(5000, 900), (5007, 905), (5013, 32767)],
                              {"source": "a file, #3", "trigger_t": 7, "trigger_mg": 33000})
            samples, meta = trace.read_trace(path)
            self.assertEqual(samples, [(0, 900), (7, 905), (13, 32767)])   # times relative to the first
            self.assertEqual((meta["trigger_t"], meta["trigger_mg"]), (7, 33000))
            self.assertIn("# source: a file, #3", Path(path).read_text())

    def test_raw_trace_keeps_xyz_columns(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "t.txt")
            trace.write_trace(path, [(10, 500, 300, 400, 0), (15, 501, 1, 2, 3)], {"source": "raw"}, "t_ms mg x y z")
            samples, _ = trace.read_trace(path)
            self.assertEqual(samples, [(0, 500, 300, 400, 0), (5, 501, 1, 2, 3)])


class CommandTests(unittest.TestCase):
    def test_extract_and_status_on_a_small_data_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / "data"
            data.mkdir()
            write_ball_csv(data / "ball_20261119_000000.csv", BURST + [
                f"STATUS {t} state=Ready pouches=1 peak=1000 mg min=950 mg still=5000 ms resets=0 rejected=0 accel_errors=0"
                for t in range(1000, 4000, 250)])
            out = Path(tmp) / "out"
            with contextlib.redirect_stdout(io.StringIO()) as printed:
                trace.main(["extract", str(out), str(data)])
                trace.main(["status", str(data)])
                trace.main(["summary", str(data)])
            self.assertIn("wrote 1 traces", printed.getvalue())
            self.assertIn("up 1000..3750 ms", printed.getvalue())
            samples, meta = trace.read_trace(out / "burst_20261119_000000_r1_s04.txt")
            self.assertEqual(len(samples), 19)
            self.assertEqual((meta["trigger_t"], meta["trigger_mg"]), (100, 33708))

    def test_ball_fixtures_hold_bursts_and_raw_captures(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / "data"
            data.mkdir()
            write_ball_csv(data / "ball_a.csv", BURST + [
                "RAW #1 begin up=10000 rate=200 samples=3", "RAW #1 0: 0,0,1000 0,0,1010 300,400,0", "RAW #1 end"])
            out = Path(tmp) / "out"
            out.mkdir()
            names = trace.write_ball_fixtures(out, [data / "ball_a.csv"])
            self.assertEqual(names, ["burst_a_r1_s04", "ball_a_raw_r1_c01"])
            samples, _ = trace.read_trace(out / "ball_a_raw_r1_c01.txt")
            self.assertEqual(samples, [(0, 1000, 0, 0, 1000), (5, 1010, 0, 0, 1010), (10, 500, 300, 400, 0)])

    def test_committed_fixtures_match_the_recordings(self):
        """tests/fixtures/accel/ is exactly what `accel_trace.py fixtures` writes from data/."""
        data = ROOT / "data"
        committed = ROOT / "tests/fixtures/accel"
        if not data.is_dir() or not list(data.glob("accel_*.csv")):
            self.skipTest("data/ recordings not available")
        with tempfile.TemporaryDirectory() as tmp:
            with contextlib.redirect_stdout(io.StringIO()):
                trace.main(["fixtures", tmp, str(data)])
            names = sorted(os.listdir(tmp))
            self.assertEqual(names, sorted(os.listdir(committed)))
            for name in names:
                self.assertTrue(filecmp.cmp(os.path.join(tmp, name), committed / name, shallow=False), name)

    def test_features_table_covers_every_class(self):
        data = ROOT / "data"
        if not list(data.glob("accel_*.csv")):
            self.skipTest("data/ recordings not available")
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            trace.main(["features", str(data)])
        text = printed.getvalue()
        for cls in ("rest", "drift flight", "flat 3.8 g", "free fall", "shaken (held)", "handled (inferred)"):
            self.assertIn(cls, text)

    def test_fixture_directory_stays_small(self):
        total = sum(p.stat().st_size for p in (ROOT / "tests/fixtures/accel").iterdir())
        self.assertLess(total, 400_000)


class ReplayDataTests(unittest.TestCase):
    def test_whole_data_replay_runs_and_finds_no_flicker(self):
        import shutil
        if shutil.which("cc") is None or not list((ROOT / "data").glob("accel_*.csv")):
            self.skipTest("needs a C compiler and the recordings in data/")
        spec = importlib.util.spec_from_file_location("accel_replay_data", ROOT / "tools/accel_replay_data.py")
        replay = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(replay)
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            replay.main([str(ROOT / "data")])
        text = printed.getvalue()
        self.assertIn("== Raw x/y/z recordings", text)
        self.assertIn("== BURST windows", text)
        self.assertIn("accel_20261103_134755.csv", text)
        self.assertIn("flicker): 0", text)

    def test_flicker_rule_ignores_warmup_and_resets(self):
        spec = importlib.util.spec_from_file_location("accel_replay_data", ROOT / "tools/accel_replay_data.py")
        replay = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(replay)
        self.assertTrue(replay.is_flicker("HELD", "SPINNING", 60, 2000))     # a real flicker
        self.assertFalse(replay.is_flicker("HELD", "SPINNING", 150, 2000))   # long enough
        self.assertFalse(replay.is_flicker("HELD", "SPINNING", 60, 250))     # began inside the warm-up
        self.assertFalse(replay.is_flicker("UNKNOWN", "HELD", 60, 2000))     # warm-up level itself
        self.assertFalse(replay.is_flicker("HELD", "UNKNOWN", 60, 2000))     # reset after a gap in the data


if __name__ == "__main__":
    unittest.main()
