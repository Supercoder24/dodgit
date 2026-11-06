#!/usr/bin/env python3
"""Wireless terminal + CSV logger for examples/hit_ball (BLE Nordic UART)."""
import argparse
import asyncio
import csv
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
import sys
import time

NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


async def play_hit_sound():
    """Use the Mac's built-in sound without blocking BLE notifications."""
    if sys.platform == "darwin":
        try:
            player = await asyncio.create_subprocess_exec(
                "/usr/bin/afplay", "/System/Library/Sounds/Glass.aiff",
                stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
            )
            if await player.wait() == 0:
                return
        except OSError:
            pass
    # Terminal bell fallback for other platforms or unavailable audio playback.
    print("\a", end="", flush=True)


class HitSound:
    def __init__(self, enabled=True):
        self.enabled = enabled
        self.seen = deque(maxlen=256)
        self.task = None

    def handle(self, line):
        if not self.enabled:
            return
        fields = line.split(",")
        if len(fields) != 5 or fields[0] != "HIT":
            return
        try:
            hit_id, impact_ms, distance_mm, accel_mg = map(int, fields[1:])
        except ValueError:
            return
        if hit_id <= 0 or impact_ms < 0 or distance_mm < 0 or accel_mg <= 0:
            return
        key = (hit_id, impact_ms)
        if key in self.seen:
            return
        self.seen.append(key)
        if self.task is None or self.task.done():
            self.task = asyncio.create_task(play_hit_sound())

    async def close(self):
        if self.task is not None:
            await self.task


class LineBuffer:
    """Notifications may split a line, or contain several lines."""
    def __init__(self):
        self.pending = bytearray()

    def feed(self, data):
        self.pending.extend(data)
        lines = []
        while b"\n" in self.pending:
            line, _, rest = self.pending.partition(b"\n")
            self.pending = bytearray(rest)
            lines.append(line.decode("utf-8", errors="replace").rstrip("\r"))
        if len(self.pending) > 8192:
            self.pending.clear()
            lines.append("MONITOR_ERROR,line_exceeded_8192_bytes")
        return lines


def describe(line):
    parts = line.split(",")
    try:
        if parts[0] == "DATA" and len(parts) == 11:
            _, ms, mm, age, ble, mg, good, bad, lost, sensor, impact = parts
            distance = "NO RANGE" if int(age) > 200 or int(mm) < 0 else f"{int(mm)/1000:.3f} m"
            return (f"{int(ms)/1000:8.2f}s  range={distance:>10}  age={age}ms  "
                    f"peak={int(mg)/1000:.2f}g  ranges={good}/{int(good)+int(bad)}  "
                    f"log_lost={lost} sensor_errors={sensor} impact_lost={impact}")
        if parts[0] == "HIT":
            return f"*** HIT CANDIDATE *** {line}"
        if parts[0] == "HIT_ACK":
            return f"*** WEARABLE CONFIRMED RECEIPT *** {line}"
    except (ValueError, IndexError):
        pass
    return line


async def monitor(args):
    # Imported here so framing/format tests and --help need no BLE installation.
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError

    start = time.monotonic()
    sound = HitSound(enabled=not args.mute)
    path = Path(args.csv)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", newline="") as f:
        writer = csv.writer(f)
        if f.tell() == 0:
            writer.writerow(["host_utc", "record", "raw_line"])

        def expired():
            return args.duration is not None and time.monotonic() - start >= args.duration

        def remaining(limit):
            return limit if args.duration is None else max(0.1, min(limit, args.duration - (time.monotonic() - start)))

        while not expired():
            print(f"Scanning for {args.name}…", flush=True)
            try:
                device = await BleakScanner.find_device_by_name(args.name, timeout=remaining(10))
                if device is None:
                    print("Not found. Check battery power, Bluetooth and Mac Bluetooth permission.", flush=True)
                    if args.once:
                        return 1
                    continue
                # A fresh buffer prevents a reconnect from joining unrelated fragments.
                buffer = LineBuffer()
                last_data = time.monotonic()

                def received(_, data):
                    nonlocal last_data
                    last_data = time.monotonic()
                    for line in buffer.feed(data):
                        stamp = datetime.now(timezone.utc).isoformat()
                        writer.writerow([stamp, line.split(",", 1)[0], line])
                        f.flush()
                        print(describe(line), flush=True)
                        sound.handle(line)

                async with BleakClient(device, timeout=remaining(20)) as client:
                    await client.start_notify(NUS_TX, received)
                    print(f"Connected wirelessly. Saving {path}. Ctrl+C to stop.", flush=True)
                    while client.is_connected and not expired():
                        await asyncio.sleep(0.25)
                        if time.monotonic() - last_data > 5:
                            print("No telemetry for 5 seconds; reconnecting.", flush=True)
                            break
                if args.once:
                    await sound.close()
                    return 0
                print("Disconnected; rescanning…", flush=True)
            except (BleakError, OSError, asyncio.TimeoutError) as exc:
                print(f"Bluetooth: {exc}", file=sys.stderr, flush=True)
                if args.once:
                    return 1
                if not expired():
                    await asyncio.sleep(1)
    await sound.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="dodgit-hit-ball")
    parser.add_argument("--csv", default=f"data/hit_{datetime.now():%Y%m%d_%H%M%S}.csv")
    parser.add_argument("--duration", type=float, help="stop after this many seconds (includes scans)")
    parser.add_argument("--once", action="store_true", help="exit instead of reconnecting")
    parser.add_argument("--mute", action="store_true", help="disable the sound on HIT events")
    args = parser.parse_args()
    if args.duration is not None and args.duration <= 0:
        parser.error("--duration must be positive")
    try:
        return asyncio.run(monitor(args))
    except KeyboardInterrupt:
        return 0
    except ModuleNotFoundError as exc:
        print(f"{exc}. Install: uv pip install --python .venv/bin/python -r tools/requirements.txt", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
