## Two-board hit prototype

Use `examples/hit_ball` and `examples/hit_wearable` for always-active ball-impact + 1 metre proximity
hit candidates, wearable LED feedback and wireless Mac terminal logs. See the
[setup and bench-test guide](examples/hit_common/README.md).

## Examples on the jerkDetection branch

The examples, ball/pouch apps and pinned UWB driver from `setup` are available
here using the existing standalone Zephyr environment. No Nordic SDK installation
is needed for the builds tested here. See [examples/README.md](examples/README.md)
for commands, compatibility changes and validation status.

```bash
# Build only, without accessing the board:
./flash.sh examples/qorvo-sdk-zephyr --build-only

# Build and flash a connected DWM3001CDK:
./flash.sh examples/qorvo-sdk-zephyr

# Original jerk-detection application:
./flash.sh
```


## Build and Flash Firmware

### Requirements

- macOS with Zephyr development environment installed
- DWM3001CDK development board
- SEGGER J-Link tools
- USB connection through J9

### Build and Flash

After updating `dwm3001-app/src/main.c` or `prj.conf`,
run the following from the repository root:

```bash
./flash.sh
```

The script will:

1. Activate the Zephyr Python environment.
2. Build the firmware with West.
3. Generate `zephyr.hex`.
4. Flash the DWM3001CDK using J-Link.
5. Reset the board.

### Notes

- The script stops if compilation fails.
- Successful incremental builds reuse cached artifacts.
- The `build/` directory should remain in `.gitignore`.
- If the project is moved or renamed, delete
  `dwm3001-app/build/` before rebuilding.
- Flashing replaces the board's existing application firmware.
git