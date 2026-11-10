import importlib.util
from pathlib import Path
import unittest
from unittest.mock import AsyncMock, patch

spec = importlib.util.spec_from_file_location("hit_monitor", Path(__file__).parents[1] / "tools/hit_monitor.py")
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)


class MonitorTests(unittest.TestCase):
    def test_fragmented_and_combined_notifications(self):
        buf = monitor.LineBuffer()
        self.assertEqual(buf.feed(b"HI"), [])
        self.assertEqual(buf.feed(b"T,1\nDATA,2\r\nSTA"), ["HIT,1", "DATA,2"])
        self.assertEqual(buf.feed(b"TE\n"), ["STATE"])

    def test_missing_range_is_not_displayed_as_close(self):
        self.assertIn("NO RANGE", monitor.describe("DATA,1000,100,201,1,1000,5,0,0,0,0"))
        self.assertIn("0.100 m", monitor.describe("DATA,1000,100,10,1,1000,5,0,0,0,0"))

    def test_malformed_line_and_overflow(self):
        self.assertEqual(monitor.describe("DATA,bad"), "DATA,bad")
        self.assertIn("MONITOR_ERROR", monitor.LineBuffer().feed(b"x" * 8193)[0])


class SoundTests(unittest.IsolatedAsyncioTestCase):
    async def test_hit_sounds_once_and_ack_or_rejection_stay_silent(self):
        with patch.object(monitor, "play_hit_sound", new_callable=AsyncMock) as play:
            sound = monitor.HitSound()
            sound.handle("HIT,575103600,125707,116,10916")
            await sound.close()
            sound.handle("HIT_ACK,575103600")
            sound.handle("HIT,575103600,125707,116,10916")
            sound.handle("REJECT,172727,no_close_fresh_range,12389")
            sound.handle("IMPACT,172727,12389")
            sound.handle("WEARABLE_IMPACT,1,172727,12389")
            sound.handle("HIT,bad,data,1,2")
            await sound.close()
            self.assertEqual(play.await_count, 1)
            sound.handle("HIT,575103601,176142,16,6018")
            await sound.close()
            self.assertEqual(play.await_count, 2)

    async def test_muted_hit_is_silent(self):
        with patch.object(monitor, "play_hit_sound", new_callable=AsyncMock) as play:
            sound = monitor.HitSound(enabled=False)
            sound.handle("HIT,575103600,125707,116,10916")
            await sound.close()
            play.assert_not_awaited()


if __name__ == "__main__":
    unittest.main()
