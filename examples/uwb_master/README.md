# uwb_master
UWB ranging demo, master side (DWM3001CDK, `decawave_dwm3001cdk/nrf52833`). Pair with `uwb_slave`.
Connect this board to the laptop and read the console; the slave runs from a battery.

## Mode
Set `RANGING_MODE` at the top of `src/main.c` (or build with `-DRANGING_MODE=MODE_TRIGGER`):
* `MODE_POLLING` - ranges `POLLING_RATE_HZ` times per second.
* `MODE_TRIGGER` - ranges once per press of the user button (SW0).

## Output
```
RANGE seq=7 distance=1234 mm duration=6.812 ms
RANGE_FAIL seq=8 err=-116 duration=25.010 ms
```
`duration` runs from just before the POLL is sent until the distance is calculated
(resolution ~30 us, the 32.768 kHz system timer).

## LEDs
led3 blue = master; led0 green toggles on each good range; led1 red flashes on a failed range;
led1 + led2 steady = radio init failed.

## Method
DS-TWR with four frames (POLL, RESPONSE, FINAL, REPORT). The slave only timestamps and sends its
timestamps back in REPORT; the master calculates the distance. Details are commented in `src/main.c`.
`ANTENNA_DELAY` / `RANGE_OFFSET_MM` are uncalibrated; check against a tape measure.

## Build / flash
```bash
JLINK_SN=<master serial> ./flash.sh examples/uwb_master
JLINK_SN=<slave serial>  ./flash.sh examples/uwb_slave
```
Open the J-Link USB serial port at 115200 baud.
