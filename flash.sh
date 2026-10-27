
#!/usr/bin/env bash
set -Eeuo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$REPO_DIR/dwm3001-app"
BUILD_DIR="$APP_DIR/build"
ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyrproject}"
BOARD="decawave_dwm3001cdk"

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

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

command -v JLinkExe >/dev/null ||
    fail "SEGGER J-Link Commander is not installed."

# Build latest source code
echo "========== BUILDING FIRMWARE =========="

cd "$ZEPHYR_WORKSPACE"

west build -p auto \
    -b "$BOARD" \
    "$APP_DIR" \
    -d "$BUILD_DIR" \
    -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

[[ -f "$BUILD_DIR/zephyr/zephyr.hex" ]] ||
    fail "Firmware was not generated."

# Flash board
echo "========== FLASHING DWM3001CDK =========="

west flash \
    -d "$BUILD_DIR" \
    --runner jlink

echo "========== COMPLETE =========="
echo "Firmware build and flash commands completed."
