# Pouch
Firmware for the wearable pouch (DWM3001CDK, `decawave_dwm3001cdk/nrf52833`).
All pouches run the **same** firmware; each identifies itself by its hardware ID. The pouch has no accelerometer or button function.
Shared code is in [`../shared`](../shared/README.md).

## Startup
* led3 (blue, D10) is OFF while playing: this identifies the board as a pouch.
* Self-test: DW3110 UWB radio probe (device ID `0xDECAxxxx`, printed over the console).
  * PASS: led0 (green) blinks 3 times, then Roll Call starts.
  * FAIL: led1 + led2 (red) blink together, slowly, forever (the console prints the error).

## Roll Call/Timeslots
When the pouch is first turned on it sends a short JOIN message containing its **hardware ID**, and repeats it after a random 100-300 ms delay until it gets an answer. Meanwhile it keeps listening.

* **The ball answers with an ASSIGN message** repeating the ID and giving a timeslot (0-7). The pouch stores it and stops sending JOIN.
* **If the pouch instead hears the ball ranging**, the ball has already left Roll Call and will never give it a timeslot: the join has failed.

The timeslot is used for all ranging replies and for acknowledging hits and resets (see [shared/README.md](../shared/README.md)).

### Hardware ID
The ID is 32 bits: the nRF52833's factory-programmed 64-bit `DEVICEID` (read through Zephyr `hwinfo`) folded with the FNV-1a hash. 0 is never used. With at most 8 pouches the chance of two sharing an ID is about 28 / 2^32 (about 1 in 150 million), which is why 32 bits were chosen over "the last few digits". Two pouches with the same ID would share one timeslot and could not be told apart. The ID is printed at startup (`Pouch hardware ID: 0x...`), so IDs can be matched to physical boards.

### Known limitations
* **Late pouches:** a pouch switched on after the ball has left Roll Call cannot join and shows the failure pattern. To add a player, switch off all pouches and the ball, then start again in the usual order. A pouch that reboots mid-game behaves the same way.
* **Collisions:** two pouches turned on at the very same moment may transmit JOIN simultaneously and miss each other. The random retry interval makes repeated collisions unlikely, but this is not guaranteed.
* If the ball's ACK for a JOIN is lost, the pouch asks again and receives the same timeslot.

## LEDs
| Phase | Meaning | LEDs |
|-------|---------|------|
| Joining | sending JOIN, no timeslot yet | blue blinks (2 Hz), others off |
| Joined | timeslot received, waiting for the ball to start ranging | blue **steady** + timeslot in binary on led2 led1 led0 (led0 = 1s bit) |
| Active | ball is ranging: game running | blue off, lives (below) |
| Join failed | too late, ball was already ranging | **all four LEDs flash fast (5 Hz)** forever. Deliberately much more obvious than the radio self-test failure (only led1 + led2, slow). |

## Lives
`LIVES_MAX` (`../shared/settings.h`, default 3) sets the starting lives and the value after a reset.

* Lives are shown as that many of led0..led2 lit, steady (the green led0 goes out first).
* At 0 lives, led0..led2 blink together once per second.
* A HIT message from the ball costs one life (ignored at 0 lives). The ball repeats a hit until acknowledged; the pouch recognizes the repeat by its hit number and loses only one life.
* A RESET message from the ball sets lives = `LIVES_MAX`.
* The pouch reports its lives to the ball in every ranging reply (the ball only counts pouches with lives as "picked up").

## Build
In VS Code with the nRF Connect extension: add `pouch` as an application, board `decawave_dwm3001cdk/nrf52833`, then Build and Flash. Alternatively `./flash.sh pouch` from the repository root (add `--build-only` to just compile; `JLINK_SN=<serial>` selects the board), or `west build -b decawave_dwm3001cdk/nrf52833` from this folder.
