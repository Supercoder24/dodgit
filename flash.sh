#!/usr/bin/env bash
set -Eeuo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyrproject}"
BOARD="${BOARD:-decawave_dwm3001cdk/nrf52833}"

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

APP="dwm3001-app"
BUILD_ONLY=0
APP_SELECTED=0
for arg in "$@"; do
    case "$arg" in
        --build-only) BUILD_ONLY=1 ;;
        -h|--help)
            echo "Usage: $0 [application-directory] [--build-only]"
            echo "Set JLINK_SN to select a board when multiple J-Link probes are connected."
            echo "Default: dwm3001-app. Examples: examples/blinky, examples/accel_polling, examples/qorvo-sdk-zephyr, examples/accel_ble, examples/hit_ball, examples/hit_wearable, ball, pouch"
            exit 0 ;;
        --*) fail "Unknown option: $arg" ;;
        *)
            [[ "$APP_SELECTED" == 0 ]] || fail "Choose one application directory."
            APP="$arg"
            APP_SELECTED=1 ;;
    esac
done
case "$APP" in
    /*) APP_DIR="$APP" ;;
    *) APP_DIR="$REPO_DIR/$APP" ;;
esac
BUILD_DIR="$APP_DIR/build"

# Validate environment
[[ -f "$APP_DIR/CMakeLists.txt" ]] ||
    fail "Application not found. Place flash.sh in the repository root."

[[ -f "$ZEPHYR_WORKSPACE/.venv/bin/activate" ]] ||
    fail "Zephyr Python environment not found."

[[ "$APP_DIR" != *' '* ]] ||
    fail "Project path contains spaces. Rename the folder first."

# Activate Python environment
source "$ZEPHYR_WORKSPACE/.venv/bin/activate"

command -v west >/dev/null ||
    fail "West is not installed."

if [[ "$BUILD_ONLY" == 0 ]]; then
    command -v JLinkExe >/dev/null ||
        fail "SEGGER J-Link Commander is not installed."
fi

# Keep generated caches with the application build.
export CCACHE_DIR="${CCACHE_DIR:-$BUILD_DIR/ccache}"

# Build latest source code
echo "========== BUILDING FIRMWARE =========="

cd "$ZEPHYR_WORKSPACE"

west build -p auto \
    -b "$BOARD" \
    "$APP_DIR" \
    -d "$BUILD_DIR" \
    -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DUSER_CACHE_DIR="$BUILD_DIR/.cache"

[[ -f "$BUILD_DIR/zephyr/zephyr.hex" ]] ||
    fail "Firmware was not generated."

if [[ "$BUILD_ONLY" == 1 ]]; then
    echo "Build complete: $BUILD_DIR/zephyr/zephyr.hex"
    exit 0
fi

# Flash board
echo "========== FLASHING DWM3001CDK =========="

JLINK_ARGS=(--runner jlink)
if [[ -n "${JLINK_SN:-}" ]]; then
    JLINK_ARGS+=(--dev-id "$JLINK_SN")
fi
west flash \
    -d "$BUILD_DIR" \
    "${JLINK_ARGS[@]}"

echo "========== COMPLETE =========="
echo "Firmware build and flash commands completed."
