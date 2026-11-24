#!/usr/bin/env bash
# Build the host co-simulation and run its scenarios (no Zephyr, no board: cc and python3 only).
#   tests/sim/run.sh                     build + the CORE scenarios (small and quick), in parallel
#   tests/sim/run.sh --all               build + every scenario (includes the 5 minute soak test)
#   tests/sim/run.sh late_pouch          only scenarios whose name contains one of the arguments (of all)
#   tests/sim/run.sh --list              list the scenarios
#   tests/sim/run.sh --build-only        only build (with SIM_BUILD=dir, to keep the products)
#   SIM_BUILD=dir tests/sim/run.sh       keep the build products in dir (default: a temporary directory)
#   SIM_UWB_C=file.c tests/sim/run.sh    use file.c instead of shared/uwb.c (to try a patch without touching the tree)
#   SIM_JOBS=4 tests/sim/run.sh          scenarios run at the same time (default: number of CPUs)
#   SIM_VERBOSE=1 tests/sim/run.sh       also print the notes and the log excerpt of the passing scenarios' checks
#   SIM_ALLOW_KNOWN=1 tests/sim/run.sh   exit 0 when the only failures are scenarios marked "KNOWN BUG #n"
# Exits non-zero if a scenario fails (a KNOWN BUG stays a failure until the product code is fixed).
# What it builds (see tests/sim/README.md): libsim (scheduler + DW3110 model), and one dynamic library per
# node type: the UNMODIFIED ball and pouch firmware sources compiled against the shims in tests/sim/shim.
set -Eeuo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/../.." && pwd)"
CC="${CC:-cc}"

if [[ -n "${SIM_BUILD:-}" ]]; then
  OUT="$SIM_BUILD"
  mkdir -p "$OUT"
else
  OUT="$(mktemp -d)"
  trap 'rm -rf "$OUT"' EXIT
fi
export SIM_BUILD="$OUT"

case "$(uname -s)" in
  Darwin)
    EXT=dylib
    SHARED=(-dynamiclib)
    NODE_LINK=(-undefined dynamic_lookup) # the shim calls are resolved in libsim when a node is loaded
    ;;
  *)
    EXT=so
    SHARED=(-shared)
    NODE_LINK=()
    ;;
esac

OPT=(-O2 -g -fPIC)

build() {
  # the simulator: coroutine scheduler, DW3110 model, Zephyr/GPIO/random shims, scenario API
  "$CC" -std=gnu11 "${OPT[@]}" -Wall -Wextra -Wno-deprecated-declarations "${SHARED[@]}" \
    -I"$HERE/src" -I"$HERE/shim" \
    "$HERE/src/sim_core.c" "$HERE/src/sim_dw.c" "$HERE/src/sim_api.c" -lm -o "$OUT/libsim.$EXT"

  # product sources, unmodified; main() of each firmware becomes node_main(). -Wno-sign-compare: Zephyr's
  # ARRAY_SIZE is unsigned too, the product loops compare it with an int.
  local NODE=(-std=c11 "${OPT[@]}" -Wall -Wextra -Wno-sign-compare "${SHARED[@]}" "${NODE_LINK[@]}"
    -Dmain=node_main -I"$HERE/shim" -I"$ROOT/shared")
  if [[ -n "${SIM_UWB_C:-}" ]]; then
    NODE+=("-DSIM_UWB_SOURCE=\"$(cd -- "$(dirname -- "$SIM_UWB_C")" && pwd)/$(basename -- "$SIM_UWB_C")\"")
    echo "NOTE: shared/uwb.c is replaced by $SIM_UWB_C" >&2
  fi
  "$CC" "${NODE[@]}" -I"$ROOT/ball/src" \
    "$HERE/mocks/uwb_hook.c" "$ROOT/ball/src/main.c" "$ROOT/ball/src/hw.c" "$ROOT/ball/src/accel_class.c" \
    "$HERE/mocks/impact.c" "$HERE/mocks/ble_log.c" -lm -o "$OUT/node_ball.$EXT"
  "$CC" "${NODE[@]}" -I"$ROOT/pouch/src" \
    "$HERE/mocks/uwb_hook.c" "$ROOT/pouch/src/main.c" "$ROOT/pouch/src/display.c" -lm -o "$OUT/node_pouch.$EXT"
}

build
if [[ "${1:-}" == "--build-only" ]]; then
  exit 0
fi

rc=0
python3 -I "$HERE/run_scenarios.py" "$@" || rc=$?
exit "$rc"
