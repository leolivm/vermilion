#!/bin/bash
# SPDX-License-Identifier: MIT
# b21guard_mut.sh (11-04): mutation check of the daemon's login-window guard (SHOWFULLNAME
# true at the start / a shutdown TERM / every revert to GOP, the user's value back right before the Metal WindowServer
# restart). Each mutation breaks one property on a copy of the daemon; run.sh must then FAIL at least one of the named
# b21_* scenarios. A mutation whose text is not found is an error (the check would be vacuous). Exit 0 iff all are caught.
#   PAR=1 nice -n 19 bash tests/mock/b21guard_mut.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${DAEMON:-$HERE/../../tools/phxctl/phxboot-navi31.sh}
TMP=$(mktemp -d /private/tmp/b21guard-mut.XXXXXX)
. "$HERE/mutlib.sh"   # PAR mutations at once (default 2), each one's run.sh serial (PAR=1) on the named scenarios only
# mut <name> <scenarios> <python replace: old> <new>
mut() {
  local name=$1 scen=$2 f=$TMP/$1.sh
  mut_make "$SRC" "$f" "$3" "$4" || { mut_bg echo "BAD     $name: text not found exactly once"; return; }
  mut_bg mut_check "$name" "$scen" "$f"; }
mut_check() { local out
  out=$(SCR=$TMP/mock.$1 DAEMON=$3 PAR=1 nice -n 19 bash "$HERE/run.sh" $2 2>&1)
  if echo "$out" | grep -q '^FAIL'; then echo "CAUGHT  $1 ($(echo "$out" | grep '^FAIL' | awk '{print $2}' | tr '\n' ' '))"
  else echo "MISSED  $1 (all of: $2 passed)"; fi; }
# the three hooks of the design
mut no_start_hook       "b21_start b21_gop"     'if b21_ok; then b21_on "daemon start"' 'if b21_ok; then :'
mut no_shutdown_trap    "b21_shutdown b21_two_boots" "trap 'b21_on \"shutdown (TERM)\"; trap - TERM; kill -TERM \$\$' TERM" "trap 'trap - TERM; kill -TERM \$\$' TERM"
mut no_off_hook         "b21_happy b21_two_boots" '  b21_off      # login-window guard' '  :      # login-window guard'
# the other ON paths (a revert to GOP / a TERM during the attempt)
mut no_m2fail_hook      "b21_crash_loop"        'b21_on "FAIL: the next boot is GOP"; ' ''
mut no_attempt_term     "b21_attempt_term"      '        b21_on "shutdown (TERM) during the desktop attempt"; exit 0' '        exit 0'
mut no_revert_hook      "b21_late_revert"       'b21_on "revert to GOP after the hangs"; ' ''
# properties of the hooks
mut off_after_kill      "b21_happy"             '  b21_off      # login-window guard: the user'"'"'s SHOWFULLNAME for the accelerated loginwindow the restart brings up
  echo "restarting WindowServer once (Metal ready, AGDC up) at $(date '"'"'+%T'"'"')"
  killall -9 WindowServer 2>/dev/null' '  echo "restarting WindowServer once (Metal ready, AGDC up) at $(date '"'"'+%T'"'"')"
  killall -9 WindowServer 2>/dev/null
  b21_off'
mut user_true_deleted   "b21_user_true"         '    1|true|YES|yes) want=1;;' '    1|true|YES|yes) defaults delete "$LWDOM" SHOWFULLNAME >/dev/null 2>&1; want=absent;;'
mut user_false_deleted  "b21_user_false"        '    *) defaults write "$LWDOM" SHOWFULLNAME -bool false >/dev/null 2>&1; rc=$?; want=0;;' '    *) defaults delete "$LWDOM" SHOWFULLNAME >/dev/null 2>&1; rc=$?; want=absent;;'
mut rerecord_each_on    "b21_two_boots"         '  if [ -e "$B21ORIG" ]; then o=$(cat "$B21ORIG")' '  if false; then o=$(cat "$B21ORIG")'
mut no_file_write       "b21_term_nocfp_false"  'f=$(b21_file); [ "$f" = true ] ||' 'f=true; [ "$f" = true ] ||'
mut no_file_read        "b21_term_nocfp_true"   '[ "$o" = absent ] && { o=$(b21_file);' '[ "$o" = never ] && { o=$(b21_file);'
mut rm_on_failed_off    "b21_off_fails"         '  else echo "b21guard: $(date '"'"'+%T'"'"') OFF FAILED' '  else rm -f "$B21ORIG"; echo "b21guard: $(date '"'"'+%T'"'"') OFF FAILED'
mut flag_ignored        "b21_flag_off b21_flag_off_left" 'b21_ok(){ [' 'b21_ok(){ return 0; ['
mut flag_err_to_log     "b21_happy"             "2>/dev/null <\"\$G/phxboot-b21guard\")" "<\"\$G/phxboot-b21guard\" 2>/dev/null)"
mut_end
echo "-----------------------------------------------------------"
echo "b21guard mutations: $CAUGHT caught, $MISSED missed, $BAD bad"
rm -rf "$TMP"
[ "$MISSED" = 0 ] && [ "$BAD" = 0 ]
