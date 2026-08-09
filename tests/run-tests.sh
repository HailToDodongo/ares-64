#!/usr/bin/env bash
# Runs every JS test case through the headless ares-test runner.
#
# Usage: tests/run-tests.sh [script.test.js ...]
#   With no arguments, runs tests/*.test.js (committed, no ROMs required) plus
#   tests/local/*.test.js (machine-local, gitignored — put scripts with private
#   ROM paths there). Set ARES_TEST to override the runner binary location.
#
# A test passes when its script exits 0 (script completed or ares.exit(0));
# uncaught JS exceptions exit 1, load errors/timeouts exit 2.

set -u
cd "$(dirname "$0")/.."

ARES_TEST=${ARES_TEST:-build_headless/test-runner/ares-test}
if [[ ! -x "$ARES_TEST" ]]; then
  echo "error: runner not found at $ARES_TEST (build the linux-headless preset first)" >&2
  echo "  cmake --preset linux-headless && cmake --build build_headless --target ares-test" >&2
  exit 2
fi

scripts=("$@")
if [[ ${#scripts[@]} -eq 0 ]]; then
  for f in tests/*.test.js tests/local/*.test.js; do
    [[ -f "$f" ]] && scripts+=("$f")
  done
fi

pass=0 fail=0
outdir=$(mktemp -d)
trap 'rm -rf "$outdir"' EXIT

for script in "${scripts[@]}"; do
  name=$(basename "$script" .test.js)
  log="$outdir/$name.log"
  env -u DISPLAY -u WAYLAND_DISPLAY "$ARES_TEST" "$script" >"$log" 2>&1
  code=$?
  if [[ $code -ne 0 ]]; then
    echo "FAIL $name (exit $code)"
    sed 's/^/  | /' "$log"
    fail=$((fail + 1))
  else
    echo "PASS $name"
    pass=$((pass + 1))
  fi
done

echo "----"
echo "pass=$pass fail=$fail"
[[ $fail -eq 0 ]] || exit 1
