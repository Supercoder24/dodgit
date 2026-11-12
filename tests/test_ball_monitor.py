import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("ball_monitor", Path(__file__).parents[1] / "tools/ball_monitor.py")
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)


class BallMonitorTests(unittest.TestCase):
    def test_line_categories(self):
        c = monitor.classify
        self.assertEqual(c("State: Armed -> Classify"), "state")
        self.assertEqual(c("STATUS 1000 state=Armed pouches=1 peak=1003 mg accel_errors=0"), "status")
        self.assertEqual(c("STATUS 1000 state=Armed pouches=1 peak=1003 mg accel_errors=7"), "warn")
        self.assertEqual(c("RANGE 5 slot=0 uid=0x1 distance=500 mm lives=3"), "range")
        self.assertEqual(c("IMPACT 99 ms, 3100 mg"), "impact")
        self.assertEqual(c("Classify: HIT on slot 0 (400 mm), hit #3"), "hit")
        self.assertEqual(c("Classify: no pouch within 1000 mm -> miss"), "other")
        self.assertEqual(c("HIT_ACK slot 0 hit #3"), "hit")
        self.assertEqual(c("HIT_UNACKED slot 0 hit #3"), "warn")

    def test_render(self):
        self.assertEqual(monitor.render("State: A -> B", False), "State: A -> B")
        self.assertTrue(monitor.render("State: A -> B", True).endswith("\033[0m"))

    def test_framing_is_shared_with_hit_monitor(self):
        buf = monitor.LineBuffer()
        self.assertEqual(buf.feed(b"State: A"), [])
        self.assertEqual(buf.feed(b" -> B\nRANGE 1\n"), ["State: A -> B", "RANGE 1"])


if __name__ == "__main__":
    unittest.main()
