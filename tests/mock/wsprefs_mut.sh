#!/bin/bash
# SPDX-License-Identifier: MIT
# wsprefs_mut.sh (11-03): mutation check of the daemon's first-WindowServer
# fix (the PhxAccel offer gate: offer_report / offer_open; the v4 leftover restore) and of its silent-crash watches. Each mutation breaks one property on a copy of the
# daemon; run.sh must then FAIL at least one of the named scenarios. A mutation whose text is not found is an error (the
# check would be vacuous). Exit 0 iff every mutation is caught.
#   bash tests/mock/wsprefs_mut.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${DAEMON:-$HERE/../../tools/phxctl/phxboot-navi31.sh}
TMP=$(mktemp -d /private/tmp/wsprefs-mut.XXXXXX)
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
mut no_offer_open       "gate_fix gate_session_race" '  offer_open   # every head-0 mode' '  :   # every head-0 mode'
mut offer_no_wait       "gate_late_oid gate_noresync_rate" 'for ((i=0; i<${PHXBOOT_OFFER_WAIT:-10}; i++)); do st=$(offer_state)' 'for ((i=0; i<1; i++)); do st=$(offer_state)'
mut offer_skip_if_open  "gate_resync_opens"     '  opens0=$(sysctl -n debug.phxfb0_offer_opens 2>/dev/null)' '  [ "$st" = 1 ] && return 0; opens0=$(sysctl -n debug.phxfb0_offer_opens 2>/dev/null)'
mut offer_no_verify     "gate_open_fails"       'if [ "$(offer_state)" = 1 ]; then
    if [ "$st" = 0 ]' 'if true; then
    if [ "$st" = 0 ]'
mut offer_msg_swapped   "gate_fix"              'if [ "$st" = 0 ]; then echo "head-0 offer gate open (was closed;' 'if [ "$st" = 9 ]; then echo "head-0 offer gate open (was closed;'
mut no_v4_restore       "v4_leftover"           '  wsp_restore "v4 leftover, before the WindowServer restart"' '  :'
mut no_flag_note        "v4_leftover"           '[ -e "$WSPFLAG" ] && echo "wsprefs: $WSPFLAG is no longer used' '[ -e "$WSPFLAG.never" ] && echo "wsprefs: $WSPFLAG is no longer used'
mut restore_any_set     "v4_leftover_moved"     '[ "$(wsp_val "$cur" $j Mode:IODisplayModeID)" = 1 ] && { f=$j; break; }' '{ f=$j; break; }'
mut restore_rm_on_fail  "v4_leftover_import_fail" 'echo "wsprefs: restore ($1): import failed: values kept"; rm -f "$cur" "$cur.done"; return 1;' 'echo "wsprefs: restore ($1): import failed: values kept"; rm -rf "$cur" "$cur.done" "$WSPSTASH"; return 1;'
mut dispovr_after_kill  "dispovr_on"            '  disp_ovr     # rotation rot15: the highest rate is the default at a new display set (read by the restarted WindowServer)
  b21_off      # login-window guard: the user'"'"'s SHOWFULLNAME for the accelerated loginwindow the restart brings up
  echo "restarting WindowServer once (Metal ready, AGDC up) at $(date '"'"'+%T'"'"')"
  killall -9 WindowServer 2>/dev/null' '  b21_off
  echo "restarting WindowServer once (Metal ready, AGDC up) at $(date '"'"'+%T'"'"')"
  killall -9 WindowServer 2>/dev/null
  disp_ovr'
mut dispovr_flag_ignored "dispovr_off"          'disp_ovr(){ [ -e "$G/phxboot-dispovr" ] || return 0' 'disp_ovr(){ :'
mut dispovr_abort_on_fail "dispovr_fail"        '[ $rc = 0 ] || echo "dispovr: $DISPOVR exit $rc: some overrides not written, the restart goes on"; return 0; }' '[ $rc = 0 ] || { echo "dispovr: exit $rc"; m2_fail "dispovr"; }; return 0; }'
mut dispovr_no_kill     "dispovr_hang"          '  if kill -0 $p 2>/dev/null; then kill -9 $p' '  if false; then kill -9 $p'
mut no_pid_watch        "silent_crash_loop silent_crash_once" 'died=$((died+1));' 'died=$died;'
mut late_no_corpse_chk  "late_logout"           'if [ "${c:-0}" -gt 0 ]; then silent=' 'if :; then silent='
mut late_no_pid_watch   "late_silent"           '[ $silent -gt $n ] && n=$silent
    n=$((n+base))' 'n=$((n+base))'
mut firstws_no_check    "ws_crash_oldkext gate_fix" 'if [ -n "$WS0" ] && [ "$WS1" != "$WS0" ]; then' 'if false; then'
mut_end; rm -rf "$TMP"
echo "RESULT: $CAUGHT caught, $MISSED missed, $BAD bad"
[ $MISSED = 0 ] && [ $BAD = 0 ]
