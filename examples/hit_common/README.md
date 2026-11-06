# Always-active ball impact within 1 metre

`examples/hit_ball` detects automatically when powered. **SW2 is not required.**
A hit requires an impact on the **ball** and fresh UWB proximity within **1 metre**
of the wearable. Wearable accelerometer readings do not trigger or gate hits.

## Flashing

| Role | J-Link serial | Blue D10 |
| --- | --- | --- |
| Ball | `760227476` | On |
| Wearable | `760227559` | Off |

```bash
JLINK_SN=760227476 ./flash.sh examples/hit_ball
# Only needed when initially programming/replacing the wearable:
JLINK_SN=760227559 ./flash.sh examples/hit_wearable
```

The ball remains compatible with the v2 wearable firmware already installed.
Wearable sensor reports are ignored by the ball's classifier. Use the existing
Zephyr workspace/ARM SDK; append `--build-only` to compile without hardware.
Each board starts automatically from its supported battery supply after flashing.
Detection/LED feedback do not depend on a Mac or Bluetooth connection. Green means
a range was valid in the last 200 ms. Red lights for one second on a hit; a steady
red startup error means initialization failed.

## Wireless terminal and sound

```bash
# One-time host setup (already installed on this Mac):
uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python -r tools/requirements.txt

.venv/bin/python tools/hit_monitor.py
```

The monitor connects to `dodgit-hit-ball`. Each new `HIT` plays the Mac Glass
sound once. Wearable movement, proximity alone, acknowledgements and rejected
impacts stay silent. `--mute` disables sound; Ctrl+C exits. `--csv <path>` selects
the capture file (default: timestamped file in `data/`). `--duration 30` limits a
capture. Logs are live, not stored on the board for later offline recovery.

## Hit rule

1. The ball accelerometer measures **2.5 g or more**.
2. At least one valid UWB range is **0–1000 mm inclusive**, measured within
   **75 ms before or after** that ball impact.

The ball waits 75 ms for range evidence and then emits `HIT` or `REJECT`. It sends
an accepted hit ID to the wearable; `HIT_ACK` confirms receipt and wearable red
feedback. No wearable impact is required. Hysteresis plus a **1 second cooldown**
limits repeated counts from one shock. Detection starts after a 500 ms sensor
settling period. There is no manual or freefall arming step.

This is intentionally a **proximity rule, not contact identification**: a ball
impact against a wall, floor or other object within 1 metre of the wearable also
counts. A wearable-only impact cannot create a hit. Missing, stale, negative or
out-of-range measurements do not count. Proximity is based on measured antenna
distance; board calibration, orientation and body shielding affect the result.

## Checks

- Put the wearable still, within 1 m; tap the ball without SW2: expect `IMPACT`,
  `HIT`, sound and `HIT_ACK` when both threshold and fresh range requirements pass.
- Tap/move only the wearable: no sound. Keep both still but nearby: no sound.
- Move the ball beyond 1 m and tap it: expect `REJECT`, no sound.
- Turn the wearable off and tap the ball: stale range must not produce a hit.
- Repeat on battery and with the final mounted assemblies. Check displayed UWB
  distance against measured physical distances before tuning thresholds.

## Telemetry and tuning

```text
DATA,ball_ms,last_distance_mm,range_age_ms,ble_subscribed,peak_accel_mg,valid_ranges,range_failures,telemetry_dropped,sensor_errors,impact_events_dropped
STATE,ball_ms,active=<0|1>,dual_impact=0,pending_hit=<id>
IMPACT,ball_impact_ms,ball_accel_mg
HIT,hit_id,ball_impact_ms,closest_valid_distance_mm,ball_accel_mg
HIT_ACK,hit_id
REJECT,ball_impact_ms,no_close_fresh_range,ball_accel_mg
```

`settings.h` controls the ball threshold, range limit and timing. The initial
`ANTENNA_DELAY` is not individually calibrated. `RANGE_OFFSET_MM` is subtracted on
the ball. The wearable may still sample its sensor for diagnostics, but these
reports have no effect on the sound rule. The protocol supports one ball and one
wearable with fixed IDs/PAN; it is not an authenticated multiplayer game protocol.
Earlier ball/pouch demos are unchanged.

## Validation

```bash
cc -std=c11 -Wall -Wextra -Werror -Iexamples/hit_common tests/hit_logic_test.c -lm -o /tmp/hit_logic_test
/tmp/hit_logic_test
python3 -m unittest discover -s tests -p 'test_hit_monitor.py'
```

Tests cover DS-TWR rollover/invalid timestamps, the inclusive 1000 mm boundary,
stale-range rejection, always-active ball impacts, hysteresis/cooldown, BLE framing
and sound deduplication. Existing `hit_bench_validation.csv`,
`hit_battery_validation.csv` and `hit_v2_rejection_validation.csv` capture earlier
rules; do not treat them as validation of the current 1 metre classifier.
