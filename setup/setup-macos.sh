#!/usr/bin/env bash
# DWM3001CDK development environment installer for Apple Silicon macOS.
# Installs host tools, Python 3.12 via uv, west, Zephyr, ARM SDK and optional
# VS Code / J-Link host tools. Builds a sample for validation. NEVER flashes.
set -Eeuo pipefail

trap 'echo "[ERROR] Setup stopped near line $LINENO. Fix the error and rerun this script." >&2' ERR

say() { printf '\n==> %s\n' "$*"; }
fail() { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyrproject}"
ZEPHYR_REVISION="${ZEPHYR_REVISION:-v4.4.0}"
INSTALL_GUI_TOOLS="${INSTALL_GUI_TOOLS:-1}"
BUILD_TEST="${BUILD_TEST:-1}"
BOARD="decawave_dwm3001cdk"

[[ "$(uname -s)" == "Darwin" ]] || fail "This script supports macOS only."
[[ "$(uname -m)" == "arm64" ]] || fail "Zephyr's current macOS setup requires Apple Silicon (arm64); Intel Macs are not supported by this script."
[[ "${EUID}" -ne 0 ]] || fail "Run as your normal macOS user, not with sudo."
case "$WORKSPACE" in *[[:space:]]*) fail "Choose ZEPHYR_WORKSPACE without whitespace (CMake/Kconfig paths may fail): $WORKSPACE" ;; esac

if ! command -v brew >/dev/null 2>&1; then
  if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
  else
    fail 'Homebrew is required first. Install it from https://brew.sh/, reopen Terminal, and rerun this script.'
  fi
fi

say "Install macOS build dependencies (Homebrew)"
brew install git cmake ninja gperf ccache qemu dtc libmagic wget openocd uv

# VS Code and J-Link are host tools only; installation never accesses the board.
if [[ "$INSTALL_GUI_TOOLS" == "1" ]]; then
  if [[ ! -d '/Applications/Visual Studio Code.app' ]] && ! command -v code >/dev/null 2>&1; then
    say "Install Visual Studio Code"
    brew install --cask visual-studio-code
  fi
  if ! command -v JLinkExe >/dev/null 2>&1 && [[ ! -x /Applications/SEGGER/JLink/JLinkExe ]]; then
    say "Install SEGGER J-Link host tools for later use (NO flashing)"
    brew install --cask segger-jlink
  fi
fi

say "Install Python 3.12 managed by uv (independent of Homebrew/Conda Python)"
uv python install 3.12
mkdir -p "$WORKSPACE"
VENV="$WORKSPACE/.venv"
if [[ ! -x "$VENV/bin/python" ]]; then
  uv venv --python 3.12 --managed-python "$VENV"
else
  say "Reuse existing virtual environment: $VENV"
fi

say "Install pip and west in Zephyr's virtual environment"
uv pip install --python "$VENV/bin/python" pip west
# Constrain Python tooling to the selected virtual environment for this script.
export PATH="$VENV/bin:$PATH"
unset PYTHONHOME PYTHONPATH || true
hash -r
python --version
west --version

say "Initialize the Zephyr workspace if not already initialized"
if [[ ! -d "$WORKSPACE/.west" ]]; then
  [[ ! -e "$WORKSPACE/zephyr" ]] || fail "$WORKSPACE/zephyr exists but the workspace is not initialized; inspect it rather than overwriting it."
  west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_REVISION" "$WORKSPACE"
else
  say "Existing workspace detected; preserving its current Zephyr checkout"
fi

cd "$WORKSPACE"
say "Download/update Zephyr modules according to the workspace manifest"
west update

say "Install the Python packages required by the checked-out Zephyr version"
west packages pip --install

say "Register Zephyr with CMake"
west zephyr-export

say "Install Zephyr SDK (ARM toolchain only)"
cd "$WORKSPACE/zephyr"
west sdk install --toolchains arm-zephyr-eabi

say "Verify DWM3001CDK board support"
west boards | grep -F "$BOARD" || fail "Board definition $BOARD was not found"

if [[ "$BUILD_TEST" == "1" ]]; then
  say "Build the Zephyr Blinky sample for DWM3001CDK (BUILD ONLY)"
  west build -p always \
    -b "$BOARD" \
    "$WORKSPACE/zephyr/samples/basic/blinky" \
    -d "$WORKSPACE/build-dwm3001-setup-test"
  [[ -f "$WORKSPACE/build-dwm3001-setup-test/zephyr/zephyr.hex" ]] || fail "Build completed but zephyr.hex was not found"
fi

say "Setup complete — no firmware was flashed"
printf 'Workspace:     %s\n' "$WORKSPACE"
printf 'Virtual env:   %s\n' "$VENV"
printf 'Zephyr commit: %s\n' "$(git -C "$WORKSPACE/zephyr" rev-parse --short HEAD)"
printf 'Board:         %s\n' "$BOARD"
printf '\nNext terminal session:\n  source "%s/bin/activate"\n' "$VENV"
printf '\nBuild an application from your VS Code repo (see README.md).\n'
if ! command -v JLinkExe >/dev/null 2>&1 && [[ ! -x /Applications/SEGGER/JLink/JLinkExe ]]; then
  printf '\nNOTE: J-Link CLI was not found. Install SEGGER J-Link before future flashing.\n'
fi
