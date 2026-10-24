#!/bin/bash
# Visits scenes on the console, runs right for a while in each and logs the game's own timing.
#   tools/perf-sweep.sh [-i IP] "cat idx label" ...      (see tools/site.sh SCENES / "SCENE <cat>")
# Output: one line per scene with the median-ish of three STATE samples taken while running.
cd "$(dirname "$0")/.."
ARGS=(); [ "${1:-}" = "-i" ] && { ARGS=(-i "$2"); shift 2; }
S() { tools/site.sh "${ARGS[@]}" "$@"; }
LOAD_WAIT=${LOAD_WAIT:-16}; RUN_WAIT=${RUN_WAIT:-10}
for spec in "$@"; do
  read -r cat idx label <<<"$spec"
  S INPUT 0 >/dev/null
  S SCENE "$cat" "$idx" >/dev/null
  sleep "$LOAD_WAIT"
  S INPUT 11 >/dev/null          # right + A (rolls and jumps, so the screen scrolls)
  sleep "$RUN_WAIT"
  for i in 1 2 3; do echo "$label | $(S STATE)"; sleep 3; done
  S INPUT 0 >/dev/null
done
