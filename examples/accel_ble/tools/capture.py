"""Capture accelerometer samples streamed by examples/accel_ble to a CSV file.

    pip install bleak
    python capture.py [output.csv]

Press Ctrl+C to stop. Columns: t_us,x_mg,y_mg,z_mg,dropped (dropped is the
ball's running total of lost samples; if it increases there is a gap).
"""
import asyncio
import csv
import math
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "dodgit-ball-accel"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # ball -> host notifications
SAMPLE = struct.Struct("<Ihhh")
# Per-axis saturation value (mg) observed in captures at the +/-16 g setting.
CLIP_MG = 23900


async def main(path):
    print(f"Scanning for {DEVICE_NAME}...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=20)
    if device is None:
        sys.exit("Ball not found. Is it powered and advertising?")

    count = 0
    with open(path, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["t_us", "x_mg", "y_mg", "z_mg", "dropped"])

        def on_data(_, data: bytearray):
            nonlocal count
            (dropped,) = struct.unpack_from("<H", data)
            for off in range(2, len(data) - SAMPLE.size + 1, SAMPLE.size):
                t, x, y, z = SAMPLE.unpack_from(data, off)
                out.writerow([t, x, y, z, dropped])
                count += 1
                mag = math.sqrt(x * x + y * y + z * z)
                if mag > 4000:
                    maxed = "".join(n for n, v in zip("XYZ", (x, y, z)) if abs(v) >= CLIP_MG)
                    suffix = f" - MAX {'/'.join(maxed)}" if maxed else ""
                    print(f"impact? |a| = {mag / 1000:.1f} g at t = {t / 1e6:.3f} s{suffix}")

        async with BleakClient(device) as client:
            await client.start_notify(NUS_TX, on_data)
            print(f"Connected, writing {path}. Ctrl+C to stop.")
            try:
                while client.is_connected:
                    await asyncio.sleep(1)
            except asyncio.CancelledError:
                pass
            print(f"{count} samples captured")


if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else time.strftime("accel_%Y%m%d_%H%M%S.csv")
    try:
        asyncio.run(main(name))
    except KeyboardInterrupt:
        pass
