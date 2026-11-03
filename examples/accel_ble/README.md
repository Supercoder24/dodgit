# accel_ble
Streams the DWM3001CDK accelerometer over BLE (Nordic UART Service) so impacts can be recorded while the ball is thrown.

* Set to +/-16 g, 400 Hz (`SAMPLE_RATE_HZ`, `FULL_SCALE_G` in `src/main.c`). See [Clipping](#clipping): the real saturation point is higher than 16 g.
* led3 (blue) on = running, led0 (green) toggles for every packet sent.
* Advertises as `dodgit-ball-accel`. Sampling starts when a host enables notifications on the NUS TX characteristic.

## Packet format
One NUS notification, little endian:

| Bytes | Field |
|-------|-------|
| 2 | `dropped` - running total of samples lost (buffer overflow / send failure) |
| 10 x N | `u32 t_us, i16 x_mg, i16 y_mg, i16 z_mg` |

## Build
`west build -b decawave_dwm3001cdk/nrf52833` from this folder, or use the nRF Connect extension (see `DEV.md`). Tested with nRF Connect SDK v3.4.1.

## Capture
From the repo root, using the venv (Python 3.15, `bleak`):
```
.venv/bin/python examples/accel_ble/tools/capture.py [out.csv]
```
Without a filename it writes `accel_<date>_<time>.csv` to the current directory. Press Ctrl+C to stop. macOS asks for Bluetooth permission for your terminal/VS Code the first time.

Columns: `t_us,x_mg,y_mg,z_mg,dropped`. The script prints a line for every sample above 4 g total, e.g.

```
impact? |a| = 41.4 g at t = 12.345 s - MAX X/Z
```

The `- MAX` suffix lists the axes that hit the sensor's saturation value (`CLIP_MG` in `capture.py`), so you know the real peak on those axes was higher than recorded. It is omitted if no axis saturated.

nRF Connect for Mobile can also connect and show the raw notifications, but it can't save a CSV.

## Clipping
At the +/-16 g setting every axis saturates at about +/-23906 mg, not 16000 mg. The reported values therefore look scaled about 1.5x too high; the cause (driver scaling of the full-scale setting) hasn't been investigated, so treat absolute "g" numbers as approximate. Any axis at or above `CLIP_MG` (23900) is flagged as `MAX`.

## Test results
Eight captures on the DWM3001CDK (hand-held shakes, taps and drops):
* Sampling held a steady 400 Hz with `dropped` = 0 in seven of the eight.
* One capture lost 1132 samples (effective rate about 292 Hz), most likely a weak or interrupted BLE link. If `dropped` increases, move closer to the host or lower `SAMPLE_RATE_HZ` to 200.
* A proper at-rest check (z about 1000 mg, x and y about 0) hasn't been recorded yet, so the scaling above is unconfirmed.
