#!/usr/bin/env python3
"""Watch the ball's debug log over Bluetooth (ball/ built with BLE_DEBUG, Nordic UART Service).

The ball advertises as "dodgit-ball". This script scans for it, subscribes to its log, prints
every line (colored by type) and saves them to a CSV in data/. It reconnects automatically
if the link drops. The ball works the same with or without this script running.

Line types printed by the ball (see ball/src/main.c):
  State: A -> B       state machine transitions
  STATUS ...          every 250 ms: state, pouches registered, accelerometer peak (mg)
  RANGE ...           one per pouch per ranging round (at most every 200 ms)
  IMPACT ...          accelerometer impact >= threshold
  Classify/HIT/...    what the ball decided about an impact
"""
import argparse
import asyncio
import csv
from datetime import datetime, timezone
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hit_monitor import LineBuffer, NUS_TX  # same BLE framing as the earlier hit prototype

COLORS = {
    "state": "\033[1;36m",   # bold cyan
    "impact": "\033[1;33m",  # bold yellow
    "hit": "\033[1;32m",     # bold green
    "warn": "\033[1;31m",    # bold red
    "range": "\033[0m",
    "status": "\033[2m",     # dim
    "other": "\033[0m",
}
RESET = "\033[0m"


def classify(line):
    """Return the display category of a log line."""
    if line.startswith("State:"):
        return "state"
    if line.startswith("STATUS"):
        # accel_errors=N with N > 0 means sensor reads are failing
        if "accel_errors=" in line and not line.rstrip().endswith("accel_errors=0"):
            return "warn"
        return "status"
    if line.startswith("RANGE"):
        return "range"
    if line.startswith("IMPACT"):
        return "impact"
    if line.startswith("HIT_UNACKED") or "unavailable" in line or "failed" in line.lower():
        return "warn"
    if line.startswith(("HIT", "Classify")) and "miss" not in line:
        return "hit"
    return "other"


def render(line, color):
    if not color:
        return line
    return f"{COLORS[classify(line)]}{line}{RESET}"


async def monitor(args):
    # Imported here so --help and the unit tests need no BLE installation.
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError

    start = time.monotonic()
    path = Path(args.csv)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", newline="") as f:
        writer = csv.writer(f)
        if f.tell() == 0:
            writer.writerow(["host_utc", "category", "line"])

        def expired():
            return args.duration is not None and time.monotonic() - start >= args.duration

        def remaining(limit):
            if args.duration is None:
                return limit
            return max(0.1, min(limit, args.duration - (time.monotonic() - start)))

        while not expired():
            print(f"Scanning for {args.name}...", flush=True)
            try:
                device = await BleakScanner.find_device_by_name(args.name, timeout=remaining(10))
                if device is None:
                    print("Not found. Is the ball on, built with BLE_DEBUG, and Bluetooth "
                          "permission granted to this terminal?", flush=True)
                    if args.once:
                        return 1
                    continue
                buffer = LineBuffer()  # fresh per connection: no fragments carried across
                last_data = time.monotonic()

                def received(_, data):
                    nonlocal last_data
                    last_data = time.monotonic()
                    for line in buffer.feed(data):
                        category = classify(line)
                        writer.writerow([datetime.now(timezone.utc).isoformat(), category, line])
                        f.flush()
                        if not (args.no_status and category == "status"):
                            print(render(line, not args.no_color), flush=True)

                async with BleakClient(device, timeout=remaining(20)) as client:
                    await client.start_notify(NUS_TX, received)
                    print(f"Connected. Logging to {path}. Ctrl+C to stop.", flush=True)
                    while client.is_connected and not expired():
                        await asyncio.sleep(0.25)
                        if time.monotonic() - last_data > 5:
                            print("No data for 5 seconds; reconnecting.", flush=True)
                            break
                if args.once:
                    return 0
                print("Disconnected; rescanning...", flush=True)
            except (BleakError, OSError, asyncio.TimeoutError) as exc:
                print(f"Bluetooth: {exc}", file=sys.stderr, flush=True)
                if args.once:
                    return 1
                if not expired():
                    await asyncio.sleep(1)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", default="dodgit-ball", help="advertised device name")
    parser.add_argument("--csv", default=f"data/ball_{datetime.now():%Y%m%d_%H%M%S}.csv",
                        help="capture file (default: timestamped file in data/)")
    parser.add_argument("--duration", type=float, help="stop after this many seconds (includes scans)")
    parser.add_argument("--once", action="store_true", help="exit instead of reconnecting")
    parser.add_argument("--no-status", action="store_true",
                        help="do not print the 4-per-second STATUS lines (still saved to the CSV)")
    parser.add_argument("--no-color", action="store_true", help="plain text output")
    args = parser.parse_args()
    if args.duration is not None and args.duration <= 0:
        parser.error("--duration must be positive")
    try:
        return asyncio.run(monitor(args))
    except KeyboardInterrupt:
        return 0
    except ModuleNotFoundError as exc:
        print(f"{exc}. Install: uv pip install --python .venv/bin/python -r tools/requirements.txt",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
