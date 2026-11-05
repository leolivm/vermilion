#!/bin/bash
# SPDX-License-Identifier: MIT
# watchsrv_mut.sh (11-03, found while chasing a video stall): mutation check of b15_boot.sh's read server path
# (srv_start / srv / sget / bgof, w2read, follow0's and watch1's reads, the one-display loop). Each mutation breaks one
# property on a copy of b15_boot.sh; b15test.sh (the scenarios named per mutation) must then FAIL. A mutation whose text is
# not found exactly once is an error (the check would be vacuous). Exit 0 iff every mutation is caught.
#   bash tests/mock/watchsrv_mut.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); SRC=${B15SRC:-$HERE/../../tools/phxctl/b15_boot.sh}
TMP=$(mktemp -d /private/tmp/watchsrv-mut.XXXXXX)
. "$HERE/mutlib.sh"   # PAR mutations at once (default 2); each one's b15test.sh stops at its first FAIL (B15STOP)
# mut <name> <B15ONLY glob> <old> <new>
mut() {
  local name=$1 only=$2 f=$TMP/$1.sh
  mut_make "$SRC" "$f" "$3" "$4" || { mut_bg echo "BAD     $name: text not found exactly once"; return; }
  mut_bg mut_check "$name" "$only" "$f"; }
mut_check() { local out
  out=$(B15=$3 B15ONLY="$2" B15STOP=1 PAR=1 nice -n 19 bash "$HERE/b15test.sh" 2>&1)
  if echo "$out" | grep -q '^FAIL'; then echo "CAUGHT  $1 ($(echo "$out" | grep '^FAIL' | awk '{print $2}' | head -4 | tr '\n' ' '))"
  else echo "MISSED  $1"; fi; }
mut w2read_vh_swap 'two_underflow_repin' 'sget wv0 && sget wh0 &&' 'sget wh0 && sget wv0 &&'
mut w2read_p1_is_p0 'two_p0gen_skip' 'bgof "$p1"; p1=$BG; bgof "$s1"; s1=$BG' 'p1=$p0; bgof "$s1"; s1=$BG'
mut w2read_s1_is_s0 'two_p1gen_skip' 'bgof "$p1"; p1=$BG; bgof "$s1"; s1=$BG' 'bgof "$p1"; p1=$BG; s1=$s0'
mut bgof_no_gen 'two_p?gen_skip' 'busy=*|gen=*) BG=' 'busy=*) BG='
mut fallback_blind 'srv_noserve' 'wv1=$(r 0x4F9A); wh1=$(r 0x3B8F); s1=$(sw); fi; }' 'wv1=0; wh1=$(r 0x3B8F); s1=$(sw); fi; }'
mut sget_no_eof 'srv_dies' 'sget() { IFS= read -r "$1" <&3 && return 0' 'sget() { IFS= read -r "$1" <&3; return 0'
mut probe_any 'srv_noserve' 'if case "$sx" in 0x????????) true ;; *) false ;; esac; then' 'if true; then'
mut server_unused 'spawn_count' 'rm -rf "$sd"; SRV=1' 'rm -rf "$sd"; SRV=0'
mut follow0_srv_busy 'two_follow_p1busy' 'q1=$BG
    case "$q0$q1" in *busy=1*) return 0 ;; esac' 'q1=$BG'
mut watch1_srv_blind 'watch1_underflow' 'sget p0 && sget v && sget h && sget p1; then bgof "$p0"; p0=$BG; bgof "$p1"; p1=$BG' 'sget p0 && sget v && sget h && sget p1; then bgof "$p0"; p0=$BG; bgof "$p1"; p1=$BG; v=0; h=0'
mut onedisp_srv_blind 'underflow' 'if srv 0x4F8A 0x3AB3 && sget v && sget h; then :;' 'if srv 0x4F8A 0x3AB3 && sget v && sget h; then v=0; h=0;'
mut_end; echo "RESULT: $CAUGHT caught, $MISSED missed, $BAD bad"; rm -rf "$TMP"; [ $MISSED = 0 ] && [ $BAD = 0 ]
