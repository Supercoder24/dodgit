import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("ball_monitor", Path(__file__).parents[1] / "tools/ball_monitor.py")
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)


class BallMonitorTests(unittest.TestCase):
    def test_line_categories(self):
        c = monitor.classify
        self.assertEqual(c("State: Thrown -> Hit (up=98400)"), "state")
        self.assertEqual(c("State: Ready -> Thrown"), "state")
        # STATUS is colored by its last field, accel_errors
        self.assertEqual(c("STATUS 1000 state=Thrown pouches=1 peak=1003 mg accel_errors=0"), "status")
        self.assertEqual(c("STATUS 1000 state=Thrown pouches=1 peak=1003 mg accel_errors=7"), "warn")
        self.assertEqual(c("STATUS 1000 state=Ready pouches=2 peak=1003 mg min=980 mg level=HELD level_ms=1200 "
                           "resets=0 log_dropped=0 accel_errors=0"), "status")
        self.assertEqual(c("STATUS 1000 state=Ready pouches=2 peak=1003 mg min=980 mg level=HELD level_ms=1200 "
                           "resets=0 log_dropped=3 accel_errors=2"), "warn")
        self.assertEqual(c("RANGE 5 slot=0 uid=0x1 distance=500 mm lives=3"), "range")
        # accelerometer lines
        self.assertEqual(c("BURST #3 trigger up=98400 mg=5413 hit level=SPINNING also=0 samples=140 pre=100"), "burst")
        self.assertEqual(c("BURST #3 -495..-420 ms: 1001 998 1003"), "burst")
        self.assertEqual(c("FEAT up=98400 level=HELD shape=ROUGH mean=1013 trend=40 masd=90 samples=20"), "burst")
        self.assertEqual(c("RAW #1 t=0 x,y,z: 1,2,3 4,5,6"), "burst")
        self.assertEqual(c("ACCEL level HELD -> SPINNING up=98400 after=1200"), "accel")
        self.assertEqual(c("ACCEL hit up=98400 mg=3100 level=SPINNING"), "impact")
        self.assertEqual(c("ACCEL stalled, sensor re-initialized (reset #1, data is back)"), "warn")
        # membership
        self.assertEqual(c("MEMBER join uid=0x1234 -> slot 2 (new)"), "member")
        self.assertEqual(c("MEMBER deleted uid=0x1234 slot 2: no answer to Reset"), "member")
        self.assertEqual(c("ROLLCALL new pouch uid=0x1234 -> slot 2 (3 registered)"), "member")
        self.assertEqual(c("REMEMBER sent uid=0x1234 slot=2 lives=3"), "status")
        # decisions
        self.assertEqual(c("Thrown: Hit 3100 mg -> Classify"), "impact")
        self.assertEqual(c("Classify: HIT on slot 0 (400 mm), hit #3, lives left 2"), "hit")
        self.assertEqual(c("Classify: thrower is the only pouch -> treating it as hit (single-pouch test)"), "hit")
        self.assertEqual(c("Classify: closest is the thrower (slot 1) -> back to Ready"), "other")
        self.assertEqual(c("Classify: no pouch within 900 mm -> miss"), "other")
        self.assertEqual(c("Classify: closest pouch (slot 1, 300 mm) is out of lives -> miss"), "other")
        self.assertEqual(c("HIT_ACK slot 0 hit #3"), "hit")
        self.assertEqual(c("HIT_UNACKED slot 0 hit #3"), "warn")
        self.assertEqual(c("Bluetooth debug log unavailable (-5)"), "warn")

    def test_render(self):
        self.assertEqual(monitor.render("State: A -> B", False), "State: A -> B")
        self.assertTrue(monitor.render("State: A -> B", True).endswith("\033[0m"))

    def test_framing_is_shared_with_hit_monitor(self):
        buf = monitor.LineBuffer()
        self.assertEqual(buf.feed(b"State: A"), [])
        self.assertEqual(buf.feed(b" -> B\nRANGE 1\n"), ["State: A -> B", "RANGE 1"])


if __name__ == "__main__":
    unittest.main()
