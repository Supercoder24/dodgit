# Ball
Firmware for the throwable ball (DWM3001CDK, `decawave_dwm3001cdk/nrf52833`).

## Startup
* led3 (blue, D10) is steady ON: this identifies the board as the ball.
* Quick DW3110 ID check (`dwt_probe` + device ID `0xDECAxxxx`), printed over the console.
  * PASS: led0 (green) blinks 3 times, then the state machine starts.
  * FAIL: led1 + led2 (red) blink forever.

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
