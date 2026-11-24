# Pouch
Firmware for the wearable pouch (DWM3001CDK, `decawave_dwm3001cdk/nrf52833`).
All pouches run the **same** firmware; each identifies itself by its hardware ID. The pouch has no accelerometer or button function.
The **ball owns the game state** (who is in the game, each pouch's timeslot and lives); the pouch only answers the ball and displays what the ball tells it. Nothing is terminal: a pouch can be switched on, rebooted or sent back to the start at any time.
Shared code is in [`../shared`](../shared/README.md) (messages, timeslots, ranging); the membership state machine is [`../shared/pouch_fsm.h`](../shared/pouch_fsm.h) (pure logic, host test `tests/pouch_fsm_test.c`).

| File | Purpose |
|------|---------|
| `src/main.c` | startup, the main loop (one radio step, then display and console), event logging, `dodgit_log()` (the console sink for `LOG_CAT`) |
| `src/display.c` / `display.h` | display layer: the only code that touches the LEDs ([below](#display-layer)) |
| `../shared/uwb.c` | radio glue around the FSM: sends JOIN, answers POLL / HIT / RESET in the pouch's timeslot |
| `../shared/pouch_fsm.h` | the membership state machine (pure logic) |
| `../shared/led_patterns.h` | what the LEDs show, as data |
| `../shared/log.h` | `LOG_CAT(flag, ...)`: log lines gated by the compile-time `LOG_*` switches in `../shared/settings.h` |
| `CMakeLists.txt` | sources `src/main.c`, `src/display.c`, `../shared/uwb.c`; adds the DW3000 driver module |
| `prj.conf` | DW3000 driver, SPI, GPIO, FPU, Zephyr logging; `CONFIG_HWINFO` (hardware ID) and `CONFIG_ENTROPY_GENERATOR` (random JOIN delays) |
| `boards/decawave_dwm3001cdk_nrf52833.overlay` | SPI / reset / IRQ wiring of the DW3110 |

## Startup
* The LEDs are configured first. If an LED GPIO is not ready the console prints `LED n init failed` and the pouch stops there, with no LED signal.
* Self-test: DW3110 UWB radio probe (device ID `0xDECAxxxx`, printed over the console as `DW3110 device ID: 0x...`).
  * PASS: led0 (green) blinks 3 times, then the pouch prints its hardware ID and starts **Joining**.
  * FAIL: led1 + led2 (red) blink together, slowly (250 ms on / 250 ms off), forever (the console prints the error).
* There is no identity LED: ball vs. pouch is obvious from the hardware.

## Phases
| Phase | Radio | LEDs |
|-------|-------|------|
| **Joining** | sends JOIN (its hardware ID) after a random `JOIN_RETRY_MIN_MS`..`JOIN_RETRY_MAX_MS` (100-300 ms), again and again; listens in between. Answers nothing else | blue fast blink |
| **Listening** | silent, receiver on. The ball is already ranging, so it is not in Roll Call or Reset and would not answer a JOIN | blue flash every 2 s |
| **Joined** | slot and lives known (told by a REMEMBER); waiting for the ball's first POLL, HIT or RESET. Transmits nothing yet | blue slow blink |
| **Active** | answers every POLL (RESPONSE, REPORT), HIT (HIT_ACK) and RESET (RESET_ACK) in its own timeslot | the lives ([below](#leds)) |

| From | Event | To |
|------|-------|----|
| power-up | self-test passed | Joining (first JOIN after a random 100-300 ms, so a pouch switched on mid-game first hears whether the ball is ranging) |
| Joining | POLL heard | Listening |
| Joining / Listening | REMEMBER with its ID | Joined |
| Listening | RESET heard | Joining (first JOIN after a random `0`..`REJOIN_DELAY_MAX_MS` = 1000 ms, so several late pouches do not collide) |
| Joined | POLL / HIT / RESET | Active |
| Joined / Active | no REMEMBER with its ID for `LOST_TIMEOUT_MS` (5000 ms) | Joining (first JOIN after a random 100-300 ms) |
| Listening | no POLL for `LOST_TIMEOUT_MS` | Joining (same) |

A RESET heard in Joining is ignored (it is already sending JOINs). Entering Joining forgets the slot: it may belong to somebody else by now.

Loss rules (`LOST_TIMEOUT_MS`):
* **Joined / Active:** the heartbeat is a REMEMBER with the pouch's **own** ID, sent by the ball once per `REMEMBER_PERIOD_MS` (1000 ms) to every pouch, in every ball state. Any other frame from the ball would not do: a pouch the ball deleted still hears its POLLs. No REMEMBER means the ball rebooted or deleted the pouch, so it stops answering POLLs and joins again.
* **Listening:** the ball ranges constantly during a game, so no POLL means it rebooted (and may be in Roll Call).

A REMEMBER with the pouch's ID overwrites its slot and lives **in any phase**: the ball is the source of truth. One with a slot >= `MAX_POUCHES` or lives > `LIVES_MAX` is ignored (no change, not logged).

Frame filtering (`pouch_dispatch()` in `../shared/uwb.c`): the pouch reacts only to frames whose source is the ball. POLL and RESET count only as broadcasts, HIT only when addressed to its own slot address, REMEMBER only with its own ID in the payload (its destination is not checked). While the pouch is inside a ranging exchange waiting for FINAL (up to the response window of the POLL plus 10 ms) it ignores everything else, so a REMEMBER sent then is lost: harmless, the next one comes within `REMEMBER_PERIOD_MS`.

## LEDs
Board LEDs: led0 green (D9), led1 red (D12), led2 red (D11), led3 blue (D10), all plain on/off GPIOs. "Red" always lights **both** red LEDs (twice the light, closer to green's brightness). The table is `shared/led_patterns.h` (timings `LED_*_MS`); a color or timing is a one-line change there.

| Situation | LEDs |
|-----------|------|
| Joining | **blue**, fast blink (125 ms on / 125 ms off) |
| Listening | **blue**, 100 ms flash every 2 s |
| Joined | **blue**, slow blink (500 ms on / 500 ms off) |
| Active, 3 lives | **green**, steady |
| Active, 2 lives | **blue**, steady |
| Active, 1 life | **red**, fast blink |
| Active, out of lives | **red**, steady |
| just lost a life | all four LEDs at 5 Hz (100 ms on / 100 ms off) for 1 s (`LED_HIT_EFFECT_MS`), then the lives pattern |
| RESET received | all four LEDs on for 0.5 s (`LED_RESET_EFFECT_MS`), then the lives pattern |

The effects follow the FSM's events, not only an Active pouch: a Joined pouch whose first message is a HIT that lowers its lives, or a RESET, becomes Active and plays the effect too. A new effect replaces one still playing.

Why:
* **One color at a time**, so the bright green LED never hides the others. Only the two effects light several colors.
* **Color is never the only cue**: about 8% of boys (the target players) are red-green color blind, so steady vs. blinking carries meaning too. Steady = a lasting condition, blinking = needs attention or is in progress. Blue means connection status when it blinks and 2 lives when steady; green and red mean lives only.
* **One known color-only pair:** out of lives (red steady) and 3 lives (green steady) differ by color alone, which a red-green color-blind player may not tell apart.
* The lives patterns cover at most 3 lives: `led_patterns.h` has a `_Static_assert(LIVES_MAX <= 3)`. The LED strips will show one pixel per life and lift this limit.
* No binary codes: the slot is not shown (it is in the console log).

## Using it
| Goal | What to do | What happens |
|------|-----------|--------------|
| Start a game | Switch on the ball and the pouches in any order (before Roll Call ends) | Each pouch blinks blue (Joining) until the ball answers, then blinks blue slowly (Joined). When Roll Call ends (short press of the ball button, or `ROLLCALL_TIMEOUT_MS` = 15 s after the last new pouch) the pouches show their lives |
| Pouch rebooted / battery bumped mid-game | Nothing | After its startup blinks the pouch gets its **same slot and lives** back with the next REMEMBER (at most about a second) and answers again from the next POLL |
| Add a player mid-game | Switch their pouch on (blue flash every 2 s: Listening), then **hold the ball button for 3 s** | The Reset resets **everybody's** lives (every pouch in the game shows the 0.5 s all-on effect). The new pouch hears the RESET, joins after a random delay of at most `REJOIN_DELAY_MAX_MS`, gets its slot and lives from the ball and blinks blue slowly. Because a new pouch joined, the ball continues in Roll Call: a short press (or the timeout) resumes the game |
| Remove a player | Switch their pouch off | The ball deletes it at the next Reset (no answer to RESET) and frees the slot |

## Hardware ID
The ID is 32 bits: the nRF52833's factory-programmed 64-bit `DEVICEID` (read through Zephyr `hwinfo`) folded with the FNV-1a hash. 0 is never used. With at most 8 pouches the chance of two sharing an ID is about 28 / 2^32 (about 1 in 150 million), which is why 32 bits were chosen over "the last few digits". Two pouches with the same ID would share one timeslot and could not be told apart. The ID is printed at startup (`Pouch hardware ID: 0x...`), so IDs can be matched to physical boards.

## Lives
`LIVES_MAX` (`../shared/settings.h`, default 3) sets the starting lives and the value after a Reset. The **ball** keeps the table of lives; the pouch displays what the ball tells it:

| Message | Pouch does |
|---------|-----------|
| REMEMBER (ID, slot, lives) | takes the ball's slot and lives (any phase; an out-of-range slot or lives is ignored). Joining / Listening become Joined |
| HIT (lives remaining), Joined / Active | sets its lives to that value, acknowledges it (HIT_ACK echoes the value) |
| RESET, Joined / Active | lives = `LIVES_MAX`, acknowledges (RESET_ACK) |

A HIT in Joining / Listening is ignored and not acknowledged. A RESET in Joining is ignored; in Listening it sends the pouch to Joining, again without an acknowledgement ([phases](#phases)).

* The ball counts a hit in its own table at once, whether or not the pouch ever acknowledges. HIT carries the absolute value, so a repeated HIT sets the same number again: only a **decrease** is a hit and plays the hit effect.
* A HIT the pouch missed is corrected by the next REMEMBER (within `REMEMBER_PERIOD_MS`), and an Active pouch plays the hit effect for it too.
* The pouch no longer reports lives: its ranging RESPONSE has no payload. A pouch at 0 lives keeps answering the ball; it only shows red steady.

## Display layer
`src/display.c` (API in `display.h`) is the only code that touches the LEDs. Only `main.c` calls it, and tells it **what** to show from the FSM's phase, lives and events (read through `uwb_pouch_phase()`, `uwb_pouch_lives()` and the `struct pouch_event`; the FSM and `uwb.c` never call the display): `display_update(phase, lives, now)` every loop pass, `display_hit()` / `display_reset()` when those events happen, plus the blocking self-test patterns. There is one effect slot: a new hit or reset effect replaces one that is still playing. The patterns are data in `shared/led_patterns.h` (pure C, host test `tests/led_patterns_test.c`); `display.c` maps the pattern's bit mask to the four GPIOs and writes them only when the mask changes. The WS2812 LED strips will replace only this backend: the FSM, `main.c` and the radio code stay as they are.

## Console output
Events only, each gated by a flag in the "Logging" section of `../shared/settings.h` (the pouch has no Bluetooth: the flags only gate the console):

| Line | Flag (default) |
|------|----------------|
| `MEMBER phase Joining: ...` at start, then `MEMBER phase A -> B (slot N, lives L)` on every phase change | `LOG_MEMBERSHIP` (1) |
| `HIT, lives = N`, `RESET, lives = N` | `LOG_STATE` (1) |
| `REMEMBER slot N, lives L` for every accepted REMEMBER that is not reported as a HIT (about one per second) | `LOG_REMEMBER` (0) |

`dodgit_log()` in `src/main.c` is the console sink behind `LOG_CAT`. The banner, the DW3110 device ID, the hardware ID and init errors are always printed. The pouch never logs periodically (no status or range lines): a slow console must not make it miss a reply timeslot. Event lines are printed after the radio step that handled the frame, i.e. after the reply was sent.

## Known limitations
* **JOIN collisions:** two pouches switched on at the same moment may transmit JOIN simultaneously and miss each other. Each repeats after a new random delay, so repeated collisions are unlikely, but not impossible. The ball only listens for JOINs in Roll Call and during a Reset; a JOIN sent at any other time is simply not answered.
* **Same ID:** two pouches with the same hardware ID would share one slot (see [Hardware ID](#hardware-id)).
* **Listening waits silently** until a Reset (or until it hears no POLL for `LOST_TIMEOUT_MS`, e.g. after a ball reboot, and joins again). The blue flash every 2 s is the only sign. If it misses the RESET (the ball stops re-sending it once every pouch in its table has acknowledged), hold the ball button again.
* **A ninth pouch:** all `MAX_POUCHES` (8) slots taken: the ball ignores its JOIN, so it stays Joining (then Listening).
* **A pouch that misses a Reset** (out of range, say) is deleted by the ball. It notices after `LOST_TIMEOUT_MS`, drops to Joining, then Listening, and rejoins with the next Reset.
* **A ball reboot starts a new game:** its table is RAM only. The pouches notice after `LOST_TIMEOUT_MS` and join again with full lives, possibly in different slots.

## Build
In VS Code with the nRF Connect extension: add `pouch` as an application, board `decawave_dwm3001cdk/nrf52833`, then Build and Flash. Alternatively `./flash.sh pouch` from the repository root (add `--build-only` to just compile; `JLINK_SN=<serial>` selects the board), or `west build -b decawave_dwm3001cdk/nrf52833` from this folder.
