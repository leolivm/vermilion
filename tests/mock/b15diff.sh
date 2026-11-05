#!/bin/bash
# SPDX-License-Identifier: MIT
# b15diff.sh <old b15_boot.sh> <new b15_boot.sh> [VAR=value ...]: decision diff of two b15_boot.sh versions on every
# b15test.sh scenario. Runs the mock twice (B15KEEP), normalizes each scenario's world (clock times, pids, the mock's temp
# paths; the new script's one "watchdog reads ..." line dropped) and compares everything the scenario left: the log, the
# recipe-script calls, the register files and writes, the sysctl stubs' logs and call counters (= the same reads in the
# same order), the rate memory. The VAR=value pairs are exported for the NEW run only (e.g. PHXB15_SERVE=0).
# Exit 0 iff no scenario differs.   bash tests/mock/b15diff.sh old.sh tools/phxctl/b15_boot.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); OLD=${1:?old b15_boot.sh}; NEW=${2:?new b15_boot.sh}; shift 2
T=$(mktemp -d /private/tmp/b15diff.XXXXXX)
B15KEEP=$T/old B15=$OLD bash "$HERE/b15test.sh" > "$T/old.txt" 2>&1
( for kv in "$@"; do export "$kv"; done; B15KEEP=$T/new B15=$NEW bash "$HERE/b15test.sh" ) > "$T/new.txt" 2>&1
norm() {   # norm <scenario world dir> <out dir>: text copies with the run-specific parts replaced
  mkdir -p "$2"; ( cd "$1" && find . -type f ! -name phxctl.bin ) | while read -r f; do mkdir -p "$2/$(dirname "$f")"
    sed -E -e 's#/private/tmp/b15mock\.[A-Za-z0-9]+#W#g; s#[0-9]{2}:[0-9]{2}:[0-9]{2}#T#g; s#pid [0-9]+#pid P#g' \
      -e '/^b15: watchdog reads/d' "$1/$f" > "$2/$f"; done; }
same=0; diffn=0
for d in "$T"/old/*/; do n=$(basename "$d")
  [ -d "$T/new/$n" ] || { echo "MISSING $n (not run by the new script)"; diffn=$((diffn + 1)); continue; }
  norm "$d" "$T/n/old/$n"; norm "$T/new/$n" "$T/n/new/$n"
  if diff -r "$T/n/old/$n" "$T/n/new/$n" > "$T/$n.diff"; then same=$((same + 1)); else diffn=$((diffn + 1)); echo "DIFF    $n"; head -20 "$T/$n.diff"; fi
done
echo "old run: $(tail -1 "$T/old.txt")"; echo "new run: $(tail -1 "$T/new.txt")"
echo "RESULT: $same scenarios identical, $diffn differ ($T)"; [ $diffn = 0 ]
