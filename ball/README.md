# Ball
Firmware for the throwable ball (DWM3001CDK, `decawave_dwm3001cdk/nrf52833`).
Uses the UWB radio (ranging, roll call, hit/reset messages) and the on-board accelerometer (impact detection).
Shared code is in [`../shared`](../shared/README.md); the state machine is specified in [`../docs/FSMs.md`](../docs/FSMs.md).

## Startup
* led3 (blue, D10) is steady ON: this identifies the board as the ball.
* Self-test: DW3110 UWB radio probe (device ID `0xDECAxxxx`, printed over the console) and accelerometer init.
  * PASS: led0 (green) blinks 3 times, then the state machine starts in **Roll Call**.
  * FAIL: led1 + led2 (red) blink forever; the console says which part failed.

## Roll Call
When the ball is first turned on it enters the "Roll Call" state. It starts with an empty table of 8 timeslots (`MAX_POUCHES`) and listens.

As each pouch is turned on it sends a short message containing its 32-bit hardware ID (see [Pouch README](../pouch/README.md)). The ball gives it the lowest free timeslot (first come, first served) and sends back a message repeating the ID together with the timeslot. Each pouch then uses its timeslot to answer all ranging requests and to acknowledge hits and resets.

* A pouch that asks again (its ACK was lost) gets the **same** timeslot.
* A ninth pouch is ignored: no reply, and it does **not** restart the timeout.
* Roll Call ends with a short press of SW0, or `ROLLCALL_TIMEOUT_MS` (15 s) after the last *new* pouch registered. Neither can happen before at least one pouch has registered.
* Holding SW0 in Roll Call does nothing (Reset needs a game to reset).
* Pouches that are switched on after Roll Call has ended cannot join (see Pouch README). The ball never re-opens Roll Call.
* Random JOIN back-off makes collisions between pouches turned on at the same moment unlikely, but not impossible; a collided JOIN is simply retried. The ball is not told about collisions, so if a pouch seems stuck in "joining", switch it off and on again.

## Buttons
SW0 (user button) stands in for the future external button. It is unrelated to the hardware RESET button, which reboots the MCU.
* Short press (< 3 s): end Roll Call (if at least one pouch registered). Ignored in all other states.
* Hold (>= 3 s, fires while still held): **Reset** from any state except Roll Call.

## State machine
See `docs/FSMs.md`. Summary of what the code does in each state:

| State     | Ranging | Accel | Exit |
|-----------|---------|-------|------|
| Roll Call | no (listens for JOIN) | no | SW0 short press / timeout, once >= 1 pouch registered -> Disarmed |
| Disarmed  | every loop | no | pouch with lives > 0 within `PICKUP_DISTANCE_MM` -> Ready (remembers it as the thrower) |
| Ready     | every loop | no (freefall detection ignored for now) | thrower farther than `LEAVE_DISTANCE_MM`, or not heard for `LEAVE_MISS_ROUNDS` rounds -> Armed |
| Armed     | off (radio idle) | yes | impact >= `IMPACT_MG` -> Classify |
| Classify  | one quick round (up to `CLASSIFY_ATTEMPTS` if nobody answers) | no | no pouch within `HIT_DISTANCE_MM` -> Disarmed; closest is the thrower -> Ready; else -> Inform |
| Inform    | HIT message + wait for ACK | no | ACK, or `INFORM_TIMEOUT_MS` -> Disarmed |
| Reset     | RESET broadcast every `RESET_RETRY_MS` | no | after 3 slow blinks -> Disarmed (silent pouches are not waited for) |

The state number is shown in binary on led2 led1 led0 (led0 = 1s bit). There is no LED strip yet; its color is listed for later.

| State     | # | led2 | led1 | led0 | Strip color |
|-----------|---|------|------|------|--------------|
| Roll Call | 0 | off  | off  | off (flashes on each new pouch) | White |
| Disarmed  | 1 | off  | off  | on   | Green |
| Ready     | 2 | off  | on   | off  | Yellow |
| Armed     | 3 | off  | on   | on   | Red |
| Classify  | 4 | on   | off  | off  | Blue |
| Inform    | 5 | on   | off  | on   | Magenta |
| Reset     | - | all three blink together, 3 x (0.5 s on / 0.5 s off) | | | Cyan |

## Console output (J-Link USB serial, 115200 baud)
State changes, `ROLLCALL ...` registrations, `RANGE ... slot= uid= distance= mm lives=` (at most every 200 ms),
`IMPACT`, `Classify: ...`, `HIT_ACK` / `HIT_UNACKED`, and the Reset summary.

## Bluetooth debug log
`BLE_DEBUG` is defined at the top of `src/main.c`. With it, the ball advertises as **`dodgit-ball`** (Nordic UART Service) and sends every log line to a connected laptop. Delete the `#define` for a build with no Bluetooth (the Bluetooth stack is configured in `prj.conf` but never started). Nothing depends on the link: with nobody connected the ball behaves identically and still prints to the console.

```bash
.venv/bin/python tools/ball_monitor.py              # scan, connect, print + save to data/ball_<time>.csv
.venv/bin/python tools/ball_monitor.py --no-status  # hide the 4-per-second STATUS lines (still saved)
```
The Bluetooth log uses the **nRF Connect SDK** Nordic UART Service library (`CONFIG_BT_NUS`, `<bluetooth/services/nus.h>`), the same as `examples/accel_ble` - not the Zephyr-upstream NUS of `accel_ble_nus`. It therefore needs a build with the nRF Connect SDK (the VS Code extension).

(One-time setup: `uv pip install --python .venv/bin/python -r tools/requirements.txt`; macOS must allow Bluetooth for your terminal.) Close the monitor before connecting another BLE client; the ball accepts one connection.

Every `STATUS` line shows the accelerometer peak since the previous line: about **1000 mg when the ball sits still**, so you can confirm the sensor works without doing anything. `accel_errors` should stay 0.

## Testing with one pouch (two boards)
With exactly one pouch registered the thrower is always the closest pouch, so the normal rule "thrower hit itself -> back to Ready" would make a hit impossible. While `pouch_count == 1` (and `SELF_HIT_IF_SINGLE_POUCH` is 1 in `settings.h`), Classify therefore treats a thrower-pouch impact as a **hit** and logs `treating it as hit (single-pouch test)`. With two or more pouches the normal rule applies again automatically.

Suggested bench run (ball on USB or battery with the monitor running, pouch on battery):
1. Power the ball, then the pouch: expect `ROLLCALL new pouch ... -> slot 0`. Press SW0 on the ball -> `Disarmed`.
2. Put the ball within 0.5 m of the pouch -> `Pickup by slot 0` -> `Ready`.
3. Move the ball more than 1 m away -> `thrown` -> `Armed`.
4. Bring the ball back within 1 m and tap/bump it (>= 2.5 g) -> `IMPACT`, `Classify: ... HIT on slot 0`, `HIT_ACK`, pouch loses a life, ball is `Disarmed`.
5. Tap the ball while it is more than 1 m away (Armed) -> `Classify: no pouch within 1000 mm -> miss`.
6. After 3 hits the pouch has 0 lives and cannot be picked up: hold SW0 for 3 s (Reset) to restore it.

## Tuning
All thresholds are placeholders in [`../shared/settings.h`](../shared/settings.h). `ANTENNA_DELAY` is not calibrated per board; compare printed distances with a tape measure.

## Build
In VS Code with the nRF Connect extension: add `ball` as an application, board `decawave_dwm3001cdk/nrf52833`, then Build and Flash (after changing `prj.conf`, regenerate/pristine-build the configuration). Alternatively `./flash.sh ball` from the repository root (a standalone Zephyr workspace; `--build-only` just compiles, `JLINK_SN=<serial>` selects the board), or `west build -b decawave_dwm3001cdk/nrf52833` from this folder.
