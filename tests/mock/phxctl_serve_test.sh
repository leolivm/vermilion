#!/bin/bash
# SPDX-License-Identifier: MIT
# phxctl_serve_test.sh: host test of `phxctl serve` (tools/phxctl/phxctl.c), the b15 watchdog's read server. NO
# hardware: phxctl.c is compiled with its main renamed and serve() called directly (this Mac has no PhxBringup, so every
# register item gets the no-connection empty line, as `regs` prints nothing then; the sysctl items are real). Checks: one
# reply line per item in order, @<sysctl> = `sysctl -n` (newlines as blanks), unknown sysctl / junk = an empty line,
# each request answered before the next one is sent (the coprocess round trip b15_boot.sh makes), exit 0 at EOF.
# Exit 0 iff all pass.   bash tests/mock/phxctl_serve_test.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); SRC=${PHXCTL_SRC:-$HERE/../../tools/phxctl/phxctl.c}
T=$(mktemp -d /private/tmp/phxserve.XXXXXX); PASSN=0; FAILN=0
ok() { if eval "$2"; then PASSN=$((PASSN + 1)); echo "PASS  $1"; else FAILN=$((FAILN + 1)); echo "FAIL  $1 ($2)"; fi; }
printf '#define main phxctl_main\n#include "%s"\n#undef main\nint main(void) { return serve(); }\n' "$SRC" > "$T/t.c"
xcrun clang -arch x86_64 -O2 -Wall -o "$T/serve" "$T/t.c" -framework IOKit -framework CoreFoundation 2> "$T/cc.log" ||
  { echo "FAIL  build"; cat "$T/cc.log"; exit 1; }
ostype=$(sysctl -n kern.ostype); ver=$(sysctl -n kern.version); ver=${ver//$'\n'/ }
printf '@kern.ostype 0x39BC @no.such.sysctl zz\n\n@kern.version\n' | "$T/serve" > "$T/out1" 2> "$T/err1"; rc=$?
ok batch_rc '[ $rc = 0 ]'
ok batch_lines '[ "$(wc -l < "$T/out1" | tr -d " ")" = 5 ]'
ok sysctl_value '[ "$(sed -n 1p "$T/out1")" = "$ostype" ]'
ok reg_no_kext_empty '[ -z "$(sed -n 2p "$T/out1")" ] && grep -q "PhxBringup service not found" "$T/err1"'
ok unknown_sysctl_empty '[ -z "$(sed -n 3p "$T/out1")" ]'
ok junk_empty '[ -z "$(sed -n 4p "$T/out1")" ]'
ok long_value '[ "$(sed -n 5p "$T/out1")" = "$ver" ]'
# the round trip as b15_boot.sh does it: requests on a read-write FIFO fd, one reply read before the next request
mkfifo "$T/q" "$T/a"; "$T/serve" < "$T/q" > "$T/a" 2> /dev/null & sp=$!
( sleep 10; kill $sp 2> /dev/null ) & wd=$!
exec 4<> "$T/q" 3< "$T/a"
printf '@kern.ostype @kern.ostype\n' >&4; IFS= read -r x1 <&3; IFS= read -r x2 <&3
printf '@kern.ostype\n' >&4; IFS= read -r x3 <&3
ok round_trip '[ "$x1" = "$ostype" ] && [ "$x2" = "$ostype" ] && [ "$x3" = "$ostype" ]'
exec 4>&-; wait $sp; rc=$?; exec 3<&-; kill $wd 2> /dev/null; wait $wd 2> /dev/null
ok eof_exit '[ $rc = 0 ]'
rm -rf "$T"; echo "RESULT: $PASSN passed, $FAILN failed"; [ $FAILN = 0 ]
