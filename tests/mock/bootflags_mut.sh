#!/bin/bash
# SPDX-License-Identifier: MIT
# bootflags_mut.sh (11-01): mutation check of the daemon's hot-plug and
# gamma boot flags (hotplug_boot / gamma_boot in phxboot-navi31.sh). Each mutation breaks one property on a copy of the
# daemon; run.sh must then FAIL at least one of the named scenarios. A mutation whose text is not found is an error (the
# check would be vacuous). Exit 0 iff every mutation is caught.
#   bash tests/mock/bootflags_mut.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${DAEMON:-$HERE/../../tools/phxctl/phxboot-navi31.sh}
TMP=$(mktemp -d /private/tmp/bootflags-mut.XXXXXX)
. "$HERE/mutlib.sh"   # PAR mutations at once (default 2), each one's run.sh serial (PAR=1) on the named scenarios only
# mut <name> <scenarios> <python replace: old> <new>
mut() {
  local name=$1 scen=$2 f=$TMP/$1.sh
  mut_make "$SRC" "$f" "$3" "$4" || { mut_bg echo "BAD     $name: text not found exactly once"; return; }
  mut_bg mut_check "$name" "$scen" "$f"; }
mut_check() { local out
  out=$(SCR=$TMP/mock.$1 DAEMON=$3 PAR=1 nice -n 19 bash "$HERE/run.sh" $2 2>&1)   # work dirs of the failing runs stay under $TMP
  if echo "$out" | grep -q '^FAIL'; then echo "CAUGHT  $1 ($(echo "$out" | grep '^FAIL' | awk '{print $2}' | tr '\n' ' '))"
  else echo "MISSED  $1 (all of: $2 passed)"; fi; }
mut no_hotplug_write    "hotplug_on flags_both" 'out=$(sysctl -w debug.phxhotplug=2 2>&1); rc=$?' 'out=""; rc=0'
mut no_bootarg_check    "hotplug_bootarg"       '*" -phxhotplugoff "*) echo "hotplug: ' '*" -phxhotplugoff-never "*) echo "hotplug: '
mut bootarg_substring   "hotplug_bootarg_like"  '*" -phxhotplugoff "*) echo "hotplug: ' '*-phxhotplugoff*) echo "hotplug: '
mut hotplug_no_h1_gate  "hotplug_nohead1 hotplug_nom7" '[ "$M7BOUND" = 1 ] || { echo "hotplug: head 1 not up this boot: debug.phxhotplug stays 0"; return 0; }' ':'
mut hotplug_fatal       "hotplug_fail"          'echo "hotplug: $(date' '[ $rc = 0 ] || exit 1; echo "hotplug: $(date'
mut hotplug_before_ws   "hotplug_on"            '  m7_bind
' '  m7_bind; hotplug_boot
'
mut hotplug_after_b15   "hotplug_on"            'hotplug_boot
hotplug0_boot   #' 'trap hotplug_boot EXIT
hotplug0_boot   #'
mut bound_ignores_bind  "hotplug_nom7 hotplug_nohead1 gamma_nohead1" 'M7LIT=0; M7PHASE=0; M7BOUND=0' 'M7LIT=0; M7PHASE=0; M7BOUND=1'
mut no_gamma_write      "gamma_on flags_both"   'out=$(sysctl -w debug.phxgamma=1 2>&1); rc=$?' 'out=""; rc=0'
mut gamma_no_retry      "gamma_busy"            '[ $t -lt 3 ]' '[ $t -lt 1 ]'
mut gamma_retry_any     "gamma_fail"            ' && echo "$out" | grep -qi busy || break' ' || break'
mut gamma_pref_inverted "gamma_on gamma_pref_true" '*"useIOP_gamma_onAMD = false")' '*"useIOP_gamma_onAMD = true")'
mut gamma_sets_pref     "gamma_on"              'p=$("$CDPREF" get' '"$CDPREF" set useIOP_gamma_onAMD 0 >/dev/null 2>&1; p=$("$CDPREF" get'
mut gamma_no_h1_gate    "gamma_nohead1"         '[ "$M7BOUND" = 1 ] || { echo "gamma: head 1 not up this boot: debug.phxgamma stays 0"; return 0; }' ':'
mut gamma_before_ws     "gamma_on"              '  m7_bind
' '  m7_bind; gamma_boot
'
mut gamma_fatal         "gamma_fail gamma_busy_stuck" 'echo "gamma: $(date' '[ $rc = 0 ] || exit 1; echo "gamma: $(date'
mut_end; rm -rf "$TMP"
echo "RESULT: $CAUGHT caught, $MISSED missed, $BAD bad"
[ $MISSED = 0 ] && [ $BAD = 0 ]
