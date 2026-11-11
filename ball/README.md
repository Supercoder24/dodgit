# Ball
Firmware for the throwable ball (DWM3001CDK, `decawave_dwm3001cdk/nrf52833`).

## Startup
* led3 (blue, D10) is steady ON: this identifies the board as the ball.
* Quick DW3110 ID check (`dwt_probe` + device ID `0xDECAxxxx`), printed over the console.
  * PASS: led0 (green) blinks 3 times, then the state machine starts.
  * FAIL: led1 + led2 (red) blink forever.

## Roll Call
When the ball is first turned on, it enter's the "Roll Call" state until it has registered all of the wearables for players participating in this round. It starts with an empty table, showing 8 timeslots with no IDs assigned. Then the ball begins listening.

As each wearable is turned on, they send a short message containing the last few digits of their unique hardware ID. See [Pouch Docs](../pouch/README.md) When the ball receives this message, it assigns them one of the 8 timeslots then sends back a message repeating the unique hardware ID and also informing it of its timeslot.

Each wearable then saves its assigned timeslot and uses this timeslot to respond to all future ranging requests, as well as for reporting Acknowledgement for hits and resets.

## State machine
See `docs/FSMs.md`. For now the user button drives it (no radio or accelerometer):
* Short press (< 3 s): Disarmed -> Ready -> Armed -> Classify -> Inform -> Disarmed
* Hold (>= 3 s, fires while still held): any state -> Reset -> Disarmed

The state number is shown in binary on led2 led1 led0 (led0 = 1s bit):

| State    | # | led2 | led1 | led0 |
|----------|---|------|------|------|
| Disarmed | 1 | off  | off  | on   |
| Ready    | 2 | off  | on   | off  |
| Armed    | 3 | off  | on   | on   |
| Classify | 4 | on   | off  | off  |
| Inform   | 5 | on   | off  | on   |

Reset blinks led0..led2 together 3 times slowly (0.5 s on / 0.5 s off), then returns to Disarmed.

## Build
`west build -b decawave_dwm3001cdk/nrf52833` from this folder, or use the nRF Connect extension.
