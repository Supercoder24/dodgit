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
for t in twr_test members_test led_patterns_test; do
  cc "${CFLAGS[@]}" "tests/$t.c" -lm -o "$OUT/$t"
  "$OUT/$t"
done

python3 -m unittest discover -s tests -p 'test_*.py'
