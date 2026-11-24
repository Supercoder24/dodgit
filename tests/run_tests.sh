#!/usr/bin/env bash
# Run every host test: the pure-C ones (no board, no Zephyr) and the Python ones.
#   tests/run_tests.sh            all tests
# Build products go to a temporary directory. Exits non-zero if anything fails.
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
CFLAGS=(-std=c11 -Wall -Wextra -Werror -I"$ROOT/shared" -I"$ROOT/ball/src")

cd "$ROOT"
for t in twr_test members_test pouch_fsm_test led_patterns_test ball_logic_test; do
  cc "${CFLAGS[@]}" "tests/$t.c" -lm -o "$OUT/$t"
  "$OUT/$t"
done

# accelerometer log lines and the BURST / RAW buffers (they print level names, so the classifier is linked)
cc "${CFLAGS[@]}" tests/accel_log_test.c ball/src/accel_log.c ball/src/accel_class.c -lm -o "$OUT/accel_log_test"
"$OUT/accel_log_test"

# the accelerometer classifier on recorded traces (tests/fixtures/accel/) and synthetic ones; the python
# tests of tools/accel_trace.py and tools/accel_replay_data.py are picked up by the discover below
cc "${CFLAGS[@]}" tests/accel_class_test.c ball/src/accel_class.c -lm -o "$OUT/accel_class_test"
"$OUT/accel_class_test"

# the earlier hit prototype in examples/ (see examples/hit_common/README.md) keeps its own test
cc -std=c11 -Wall -Wextra -Werror -Iexamples/hit_common tests/hit_logic_test.c -lm -o "$OUT/hit_logic_test"
"$OUT/hit_logic_test"

python3 -m unittest discover -s tests -p 'test_*.py'

# the host simulation of the real firmware (core scenarios, a few seconds); `tests/sim/run.sh --all` runs everything
tests/sim/run.sh
