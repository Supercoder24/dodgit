# Logging
How the ball (and, in a smaller way, the pouch) logs, why some lines are compiled out, and how to read the result. The switches are in the "Logging" section of [`shared/settings.h`](../shared/settings.h); the macro is [`shared/log.h`](../shared/log.h).

## Why categories with compile-time switches
Every ball log line goes to **two** places ([`ball/src/ble_log.c`](../ball/src/ble_log.c)): the console (`printk`, 115200 baud, about 11 KB/s at most) and, if a laptop is subscribed ([`tools/ball_monitor.py`](../tools/ball_monitor.py)), a queue of 48 lines (160 bytes each) that a low-priority thread sends over Bluetooth. Two costs follow:

* **The Bluetooth queue drops lines when it is full.** A dropped line is only counted (`log_dropped=` in the STATUS line, and gaps in the `BURST #n` numbers), so a log with everything on looks complete but is not.
* **The console can block the ball's own loop.** At 115200 baud a ~100 character line takes roughly 9 ms to print, about as long as a Ready ranging round (~20 ms). Heavy logging can therefore slow ranging down and make detection worse: the thing being observed changes. For that reason the switches gate the console print too, not only the Bluetooth queue.

So each category has a `LOG_*` switch (`1` / `0`). `LOG_CAT(LOG_RANGE, "RANGE ...", ...)` with the switch at `0` compiles to nothing: no formatting, no console print, no queue slot. Switch a category on, rebuild, flash. The expensive ones are off by default.

## Categories
| Flag | Default | Lines | Rate |
|------|---------|-------|------|
| `LOG_STATE` | on | `State: A -> B (up=ms)`, `ROLLCALL new pouch` / `timeout`, the Reset summary, `Disarm:` / `Classify:` / `Thrown:` / `Pickup` / `Ready:` decisions, `HIT_ACK` / `HIT_UNACKED`, `Button:` | a few per minute |
| `LOG_STATUS` | on | the `STATUS` line (state, pouches, accelerometer peak / min / level, `log_dropped`, `accel_errors`), every `STATUS_LOG_PERIOD_MS` (250 ms) | 4 / s |
| `LOG_RANGE` | off | `RANGE` line per pouch per ranging round, at most every `RANGE_LOG_PERIOD_MS` (200 ms) | up to 5 rounds / s x pouches (about 40 lines / s with 8) |
| `LOG_MEMBERSHIP` | on | `MEMBER join` (new / known / table full), `MEMBER deleted`; on the pouch's console: phase changes. Events only | rare |
| `LOG_REMEMBER` | off | every REMEMBER sent (ball) / received (pouch) | up to 8 / s on the ball |
| `LOG_ACCEL_CLASS` | on | `ACCEL level A -> B`, `ACCEL hit`, `ACCEL stalled` | on change |
| `LOG_ACCEL_BURST` | off | `BURST` windows of raw \|a\| around every threshold crossing | ~11 lines per crossing |
| `LOG_ACCEL_FEATURES` | off | `FEAT` lines: the classifier's window features, for tuning thresholds | per window |
| `LOG_ACCEL_RAW` | off | `RAW` dump of raw x / y / z samples, on a short button press | one 10 s dump per press |

The first measurement to take on hardware, before deciding the defaults for good: switch every flag on and watch `log_dropped=`. That tells how many lines per second the Bluetooth link really sustains.

## STATUS
```
STATUS <up ms> state=<state> pouches=<n> peak=<mg> mg min=<mg> mg level=<REST|HELD|SPINNING|UNKNOWN> level_ms=<ms> resets=<n> log_dropped=<n> accel_errors=<n>
```
`peak` / `min` are the largest / smallest \|a\| since the previous STATUS line (about 1000 mg when the ball sits still: a quick check that the accelerometer is alive; -1 = no sample, the sensor is stalled). `level` and `level_ms` are the classifier's current level and how long it has been that. `resets` counts accelerometer re-initializations, `log_dropped` the lines the Bluetooth log lost. `accel_errors` stays the last field: the monitor colors the line by it.

## Raw accelerometer capture (`LOG_ACCEL_RAW`)
Raw samples at 200 Hz do not fit through the line queue even in a compact format. With the flag on, the ball keeps the last `ACCEL_RAW_CAPTURE_MS` of raw x / y / z samples in a RAM ring buffer; a **short button press** outside Roll Call dumps the ring through `impact_log_line()` paced to what the link can take (lines are spread over main-loop passes, so ranging is never stalled). A labeled capture is: do the movement (rest, hold, walk, throw, bounce, bump), press the button, save the log with `ball_monitor.py`, and note the label. `tools/accel_trace.py` turns the saved CSV into traces; see [Accelerometer.md](Accelerometer.md).

## The pouch
The pouch has no Bluetooth: its flags only gate console output. It logs **events only** (phase changes, hits, resets, optionally REMEMBER), never periodically, and always after the radio step that handled the frame, so a slow console cannot make a reply miss its timeslot. Details: [pouch/README.md](../pouch/README.md#console-output).

## Reading the log
`tools/ball_monitor.py` colors lines by their prefix and saves everything to `data/ball_<time>.csv` (`--no-status` hides the 4-per-second STATUS lines on screen but still saves them). All times in the lines (`up=`, the first number of `STATUS` / `RANGE`) are the ball's uptime in ms: the laptop's receive times are bunched by Bluetooth batching and cannot be used to order events. `grep -v BURST` removes the raw windows when reading a log.
