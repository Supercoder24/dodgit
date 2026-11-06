# Examples with the existing Zephyr setup

This experiment branch starts from `jerkDetection` and imports the examples,
ball/pouch firmware, driver submodule and reference docs from `setup` at
`a9e8b58`. The original `dwm3001-app` source and default flash command are preserved.

## Setup

Use the existing `~/zephyrproject/.venv` and ARM Zephyr SDK. For a new machine,
the existing installer is `bash setup/setup-macos.sh`. It pins a different version
for fresh installations; the test results below apply to the locally installed
version shown here, not to a fresh install.

Initialize the pinned UWB driver after cloning:

```bash
git submodule update --init --recursive
```

## Build and flash

Run from the repository root. `--build-only` never accesses the board; omit it
to build and flash through J-Link. Flashing replaces the current application.
Each app has its own ignored `build/` directory.

```bash
./flash.sh examples/blinky --build-only
./flash.sh examples/accel_polling --build-only
./flash.sh examples/qorvo-sdk-zephyr --build-only
./flash.sh examples/accel_ble --build-only
./flash.sh ball --build-only
./flash.sh pouch --build-only

# First hardware check: green LED blink
./flash.sh examples/blinky

# UWB interface check: single-board tests, no ranging peer required
./flash.sh examples/qorvo-sdk-zephyr

# Restore your jerk-detection firmware
./flash.sh
```

The board target defaults to `decawave_dwm3001cdk/nrf52833`.
`ZEPHYR_WORKSPACE` overrides the workspace location and `BOARD` overrides the
board target. Console output uses the J-Link USB serial port at 115200 baud;
open the terminal before resetting the board to capture startup diagnostics.
For BLE streaming, see [accel_ble/README.md](accel_ble/README.md).

## Compatibility changes

- The pinned decadriver includes `version.h`. The three UWB applications add
  Zephyr's generated `zephyr/` include directory for the header's new location;
  the submodule itself remains unmodified.
- The BLE example uses upstream Zephyr NUS headers, configuration, notification
  callbacks and registration instead of Nordic SDK-specific APIs. Notification
  payload size is calculated from the ATT MTU minus its three-byte overhead.
- `flash.sh` accepts an application directory and `--build-only`. Build caches
  live inside the app's ignored build directory.

## Validation (2026-11-05)

Environment: Apple Silicon macOS, Zephyr `v4.5.0-rc3-40-gec98918869c`,
Zephyr SDK `1.0.1` ARM toolchain, Python `3.12.16`, West `1.5.0`.

| Application | Build | Flash used | RAM used |
| --- | --- | ---: | ---: |
| Original jerk detection | Passed | 27,668 B | 4,800 B |
| Blinky | Passed | 20,468 B | 4,608 B |
| Accelerometer polling | Passed | 33,716 B | 4,800 B |
| UWB self-test | Passed | 48,108 B | 6,464 B |
| Accelerometer BLE | Passed | 119,648 B | 34,892 B |
| Ball | Passed | 42,236 B | 6,464 B |
| Pouch | Passed | 42,088 B | 6,464 B |

The board has 512 KiB flash and 128 KiB RAM. `flash.sh` was also exercised for
the original app and UWB example in build-only mode.

Build validation and board runtime validation are separate. Initial USB checks
found no J-Link device; hardware results are pending a working USB connection.
The upstream BLE port has not yet been checked for sampling rate, drops or scaling.
The older BLE README's capture results describe the teammate's original firmware.