# Ball
Firmware for the throwable ball (DWM3001CDK, `decawave_dwm3001cdk/nrf52833`).
Uses the UWB radio (ranging, joining, hit / reset messages) and the on-board accelerometer (Rest / Held / Spinning levels and Hit events).
Shared code is in [`../shared`](../shared/README.md); the state machine is specified in [`../docs/FSMs.md`](../docs/FSMs.md#ball-main-fsm).

The ball is the **source of truth** for the game: it keeps the table of pouches (hardware ID, timeslot, remaining lives; RAM only, so a ball reboot starts a new game), counts every hit in it, and reminds each pouch of its slot and lives once a second (REMEMBER). The pouches only display.

| File | Purpose |
|------|---------|
| `src/main.c` | startup and the state machine (states, LEDs, button handling, logging) |
| `src/ball_logic.h` | the decision rules as pure functions: `Disarm()`, `Classify()`, Ready, Thrown (host test `tests/ball_logic_test.c`) |
| `src/impact.c` / `impact.h` | accelerometer service: sampling thread, stall recovery, level + Hit / stall events, accelerometer log lines |
| `src/accel_class.c` / `accel_class.h` | pure-C classifier: Rest / Held / Spinning + Hit ([Accelerometer.md](../docs/Accelerometer.md), host tests `tests/accel_class_test.c`) |
| `src/hw.c` / `hw.h` | the four LEDs, the button, the self-test patterns |
| `src/ble_log.c` / `ble_log.h` | `dodgit_log()`: console + Bluetooth debug log |
| `../shared/*` | UWB protocol and the table of pouches (`uwb.c`, `members.h`), LED patterns, logging macro, settings |

## Startup
* Self-test: DW3110 UWB radio probe (device ID `0xDECAxxxx`, printed over the console) and accelerometer init.
  * PASS: led0 (green) blinks 3 times, then the state machine starts in **Roll Call**.
  * FAIL: led1 + led2 (red) blink forever; the console says which part failed.
* There is no identity LED: the ball and the pouch are obviously different hardware.

## Roll Call
When the ball is first turned on it enters "Roll Call". It starts with an empty table of 8 timeslots (`MAX_POUCHES`) and listens.

As each pouch is turned on it sends a short JOIN message containing its 32-bit hardware ID (see [Pouch README](../pouch/README.md)). The ball gives it the lowest free timeslot (first come, first served) and answers with a REMEMBER holding the ID, the timeslot and the lives. The pouch then uses its timeslot to answer all ranging requests and to acknowledge hits and resets.

* A pouch that asks again (its answer was lost, or it rebooted) gets the **same** timeslot and lives.
* A ninth pouch is ignored: no reply, and it does **not** restart the timeout.
* Roll Call ends with a short press of the button, or `ROLLCALL_TIMEOUT_MS` (15 s) after the last *new* pouch registered. Neither can happen before at least one pouch has registered. It then calls `Disarm()`: the ball may already be in a player's hand.
* Holding the button in Roll Call does nothing (Reset needs a game to reset).
* **The order of switching on does not matter**: pouches that were on before the ball keep sending JOIN until they hear it.
* Pouches switched on **after** Roll Call has ended are not lost: they wait silently ("Listening") until a Reset, which is also the join window (below).
* Random JOIN back-off makes collisions between pouches turned on at the same moment unlikely; a collided JOIN is simply retried.

## Buttons
The `sw0` alias is the external button, wired between header J10 pin 15 (nRF P0.28) and GND (e.g. pin 9) with the internal pull-up; see `boards/decawave_dwm3001cdk_nrf52833.overlay`. The on-board SW0 (P0.02) is no longer used. Neither is related to the hardware RESET button, which reboots the MCU.
* Short press (< `BUTTON_HOLD_MS`, 3 s): end Roll Call (if at least one pouch registered). Ignored in all other states (it flashes red in Roll Call when ignored). In a build with `LOG_ACCEL_RAW` on, a short press outside Roll Call dumps the last seconds of raw accelerometer samples instead ([Logging.md](../docs/Logging.md#raw-accelerometer-capture-log_accel_raw)).
* Hold (>= `BUTTON_HOLD_MS`, fires while still held): **Reset** from any state except Roll Call.

## Reset: the join window
Reset resets everybody's lives to `LIVES_MAX` and is also how a pouch that was switched on late joins, without turning anything else off. It lasts `RESET_WINDOW_MS` (3 s, three blinks) and:
* sends RESET every `RESET_RETRY_MS` (200 ms) until every pouch in the table has acknowledged it (each in its own timeslot), then every `RESET_KEEPALIVE_MS` (500 ms) so a late pouch that missed one still hears another;
* **listens for JOIN for the whole 3 s**, also after everybody has answered and also with an empty table: a late pouch is registered, told its slot and lives at once, and counts as acknowledged (so one that joins in the last 200 ms is kept);
* deletes at the end every pouch that never answered (its slot is freed, ranging rounds get shorter): safe, because everybody's lives were just reset;
* then goes to **Roll Call** if nobody is left or a *new* pouch joined (it gives several late players time to join; press the button to resume), otherwise back into the game with `Disarm()`.

Adding a player therefore resets everybody's lives. Details: [FSMs.md](../docs/FSMs.md#reset).

## State machine
See [`../docs/FSMs.md`](../docs/FSMs.md#ball-main-fsm). Summary of what the code does in each state:

| State | Ranging | Accelerometer | Exit |
|-------|---------|---------------|------|
| Roll Call | no (listens for JOIN) | - | short press / timeout, once >= 1 pouch registered -> `Disarm()` |
| Unheld | every loop | level | alive pouch within `PICKUP_DISTANCE_MM` **and** level Held for `TRANSITION_CONFIRM_ROUNDS` rounds -> Ready (the pouch is the thrower) |
| Ready | every loop | level (Hit events ignored) | level Spinning **and** the thrower beyond `THROW_RANGE_MM` -> Thrown; no alive pouch within `THROW_RANGE_MM` for `READY_ORPHAN_MS`, or not Held for `READY_NOT_HELD_MS` -> Unheld; another alive pouch closer than the thrower for `THROWER_SWITCH_MS` -> that pouch is the thrower |
| Thrown | one round every `THROWN_RECHECK_MS`, otherwise radio idle | Hit / stall events, level | Hit or sensor stall -> `Classify()`; `THROW_TIMEOUT_MS` or Rest for `THROWN_REST_MS` -> `Disarm()`; the same alive pouch the closest within `THROW_RANGE_MM` for `THROWN_RETURN_MS` -> Ready with it as the thrower |
| Hit | HIT message + wait for the acknowledgment | - | ack, or `HIT_ACK_TIMEOUT_MS` -> `Disarm()` |
| Reset | RESET broadcast (every `RESET_RETRY_MS`, later every `RESET_KEEPALIVE_MS`); JOINs accepted | - | after `RESET_WINDOW_MS` -> Roll Call (table empty or a new pouch joined) or `Disarm()` |

`Disarm()` ranges once (up to `RANGE_ATTEMPTS` rounds if nobody answers) and goes to Ready if an alive pouch is within `PICKUP_DISTANCE_MM` and the ball is Held, otherwise to Unheld. `Classify()` ranges `CLASSIFY_ROUNDS` times, merges the rounds per pouch (a pouch is missing from some rounds, the very close one that was just hit more often) and looks at the closest pouch, alive or not: nobody within `HIT_DISTANCE_MM` or the closest one is out of lives -> miss (Unheld); the thrower -> Ready; another pouch -> Hit.

The ball counts a hit in its own table **immediately** on entering Hit, whether or not the pouch acknowledges; a pouch that missed the HIT is corrected by the next REMEMBER.

### LEDs
One color at a time, steady vs. blinking (the patterns are data in [`../shared/led_patterns.h`](../shared/led_patterns.h)). Players look into the ball in Roll Call, Reset, Ready and Unheld, so those four have one simple, mutually distinct pattern each.

| State | LEDs |
|-------|------|
| Self-test | pass: green blinks 3 times; fail: both reds blink forever |
| Roll Call | blue steady; green flash (150 ms) for each new pouch; red flash if a short press is ignored |
| Unheld | green steady (idle, safe) |
| Ready | red steady (armed) |
| Thrown | red fast blink (4 Hz) |
| Hit | red + blue steady while informing (at most 1 s) |
| Reset | all four LEDs blink together (0.5 s on / 0.5 s off) for the whole Reset window |

Color families: green = idle, red = armed, blue = setup. led0 green (D9), led1 + led2 red (D12, D11, always lit together), led3 blue (D10). Whether all four are visible from inside the assembled ball, and their brightness outdoors, has to be checked on hardware (Plan.md, E5).

## Logging
Every line goes to the console (J-Link USB serial, 115200 baud) and, with a laptop connected, over Bluetooth. Lines are sorted into categories with **compile-time switches** (`LOG_*` in [`../shared/settings.h`](../shared/settings.h)); the expensive ones (RANGE, REMEMBER, BURST, FEAT, RAW) are off by default. What each category prints, why the switches exist (the console blocks ranging, the Bluetooth queue drops lines) and the STATUS format: [Logging.md](../docs/Logging.md). The accelerometer part of the log: [Accelerometer.md](../docs/Accelerometer.md).

* `State: A -> B (up=ms)`: every transition carries the ball's uptime, so it can be lined up with the other lines (`up=` / the first number of `STATUS` and `RANGE`; the laptop's receive times are bunched by Bluetooth batching).
* `STATUS ... level=<L> level_ms=<n> ... log_dropped=<n> accel_errors=<n>`: every 250 ms. `peak` is about **1000 mg when the ball sits still**, so you can confirm the sensor works without doing anything. `accel_errors` should stay 0; `log_dropped` counts lines the Bluetooth log lost.
* `RANGE` lines (with `LOG_RANGE`) are printed in every ranging state, also in Thrown (every `THROWN_RECHECK_MS`), so the ball's distance to each pouch during a flight is visible: use it to measure the short-throw detection window.

## Bluetooth debug log
`BLE_DEBUG` is defined at the top of `src/main.c`. With it, the ball advertises as **`dodgit-ball`** (Nordic UART Service) and sends every log line to a connected laptop. Delete the `#define` for a build with no Bluetooth (the Bluetooth stack is configured in `prj.conf` but never started). Nothing depends on the link: with nobody connected the ball behaves identically and still prints to the console.

```bash
.venv/bin/python tools/ball_monitor.py              # scan, connect, print + save to data/ball_<time>.csv
.venv/bin/python tools/ball_monitor.py --no-status  # hide the 4-per-second STATUS lines (still saved)
```
The Bluetooth log uses the **nRF Connect SDK** Nordic UART Service library (`CONFIG_BT_NUS`, `<bluetooth/services/nus.h>`), the same as `examples/accel_ble` - not the Zephyr-upstream NUS of `accel_ble_nus`. It therefore needs a build with the nRF Connect SDK (the VS Code extension).

(One-time setup: `uv pip install --python .venv/bin/python -r tools/requirements.txt`; macOS must allow Bluetooth for your terminal.) Close the monitor before connecting another BLE client; the ball accepts one connection.

## Testing with one pouch (two boards)
With exactly one pouch registered the thrower is always the closest pouch, so the normal rule "thrower hit itself -> back to Ready" would make a hit impossible. While there is exactly one pouch (and `SELF_HIT_IF_SINGLE_POUCH` is 1 in `settings.h`), `Classify()` therefore treats a hit on the thrower's pouch as a **hit** and logs `treating it as hit (single-pouch test)`. With two or more pouches the normal rule applies again automatically.

Remember the rules of the game: a throw is only recognized when the ball is Spinning **and** the thrower's pouch is more than `THROW_RANGE_MM` (2 m) away, and hits only count in Thrown. Suggested bench run (ball on USB or battery with the monitor running, pouch on battery and left on a chair):
1. Power the ball, then the pouch (or the other way round): expect `ROLLCALL new pouch ... -> slot 0` and `MEMBER join`. Press the button on the ball: `Disarm: ... -> Unheld` (green) if the ball lies on the table, or `-> Ready` (red) if you hold it within 1 m of the pouch.
2. Lift the ball within 1 m of the pouch: the ball shows level `HELD` and goes `Pickup by slot 0` -> Ready (red). Put it back down and wait: `Ready: the ball was not Held for 5000 ms` -> Unheld.
3. Reboot the pouch (switch it off and on): within about a second it is Joined again with the same slot and lives (`MEMBER phase ...` on its console).
4. Pick the ball up near the pouch, walk more than 2 m away holding it, and throw it back towards the pouch so that it lands within 0.9 m of it: `thrown` (level `SPINNING`, thrower beyond 2000 mm) -> Thrown (red blinking), then `Thrown: Hit ... mg -> Classify`, `Classify: HIT on slot 0 ... lives left 2`, `HIT_ACK`; the pouch loses a life and shows the all-LED flash. Bounces right after the first impact must not cost a second life (the ball waits in Unheld until it is picked up).
5. Throw it so that it lands more than 0.9 m from the pouch: `Classify: no pouch within 900 mm -> miss` -> Unheld. A throw that never gets beyond 2 m does not enter Thrown at all, and its impact is ignored.
6. After 3 hits the pouch has 0 lives (red steady) and cannot pick the ball up: hold the button for 3 s (Reset) to restore it.
7. Switch a second pouch on after the game has started (it blinks blue every 2 s: Listening) and hold the button for 3 s: Reset resets the lives, the new pouch joins (`MEMBER join ...`), the ball continues in Roll Call, a short press resumes the game. Switch one pouch off and hold the button again: `MEMBER deleted ...`.

To measure the short-throw detection window (Plan.md, Risk 6), switch `LOG_RANGE` on and throw from 3 m: the `RANGE` lines show the distance during the last part of the flight.

## Accelerometer behavior
How the on-board LIS2DH12 is classified (Rest / Held / Spinning, the Hit event, the thresholds and the data behind them, known failure modes, how to replay recorded data on the host): [Accelerometer.md](../docs/Accelerometer.md).
* **Hit events** are reported in every state, but only Thrown acts on them; every other state drains and ignores them (the launch spike, bumps in the hand and point-blank throws are not hits).
* **Stall recovery:** after a hard shock the LIS2DH sometimes stops delivering data (the driver reports "no new data" forever, which used to show up as `peak=0`). If no valid sample arrives for `ACCEL_STALL_MS`, the sensor is re-initialized from software (`lis2dh_init_chip()` plus +/-16 g, 400 Hz), the classifier starts over and a stall event is queued: in Thrown that counts as a hit (it often follows a hard shock) and goes to `Classify()`. The log shows `ACCEL stalled, sensor re-initialized (reset #n ...)`; `resets=` in STATUS counts them. While stalled `peak=-1` and the level is `UNKNOWN` (not Held, so the ball cannot be picked up until the data is back). If `resets` keeps rising or data does not come back, suspect power (battery / regulator dip on impact) rather than software.
* Peaks above 16 g (the configured range) have been seen (30-40 g); do not trust absolute values that high. Thresholds still work as relative triggers.

## Tuning
All thresholds are placeholders in [`../shared/settings.h`](../shared/settings.h). `ANTENNA_DELAY` is not calibrated per board; compare printed distances with a tape measure.
Most useful to tune from the Bluetooth log: `PICKUP_DISTANCE_MM` / `THROW_RANGE_MM` / `HIT_DISTANCE_MM` (distances, with `LOG_RANGE` on), `THROW_TIMEOUT_MS` / `THROWN_RETURN_MS` / `THROWN_REST_MS`, `READY_NOT_HELD_MS` / `READY_ORPHAN_MS` / `THROWER_SWITCH_MS`, and the `ACCEL_*` classifier thresholds (watch the `ACCEL level` and `FEAT` lines during real throws; a real flight must be `SPINNING` and a real hit must never be missed).

## Build
In VS Code with the nRF Connect extension: add `ball` as an application, board `decawave_dwm3001cdk/nrf52833`, then Build and Flash (after changing `prj.conf`, regenerate/pristine-build the configuration). Alternatively `./flash.sh ball` from the repository root (a standalone Zephyr workspace; `--build-only` just compiles, `JLINK_SN=<serial>` selects the board), or `west build -b decawave_dwm3001cdk/nrf52833` from this folder.
