# Pouch
Firmware for the wearable pouch (DWM3001CDK, `decawave_dwm3001cdk/nrf52833`).

## Startup
* led3 (blue, D10) stays OFF: this identifies the board as a pouch.
* Quick DW3110 ID check (`dwt_probe` + device ID `0xDECAxxxx`), printed over the console.
  * PASS: led0 (green) blinks 3 times, then the main loop starts.
  * FAIL: led1 + led2 (red) blink forever.

## Lives
`LIVES_MAX` (top of `src/main.c`, default 3) sets the starting lives and the value after a reset.

* Lives are shown as that many of led0..led2 lit, steady.
* At 0 lives, led0..led2 blink together once per second.
* Short press (< 3 s): lose one life (ignored at 0 lives). Stands in for a HIT message from the ball.
* Hold (>= 3 s, fires while still held): lives = `LIVES_MAX`. Stands in for a RESET message from the ball.

## Build
`west build -b decawave_dwm3001cdk/nrf52833` from this folder, or use the nRF Connect extension.
