#!/bin/bash
# SPDX-License-Identifier: MIT
# bootrate_mut.sh (11-02): mutation check of the boot-rate changes. Each mutation breaks
# one property on a copy of one script; its mock must then FAIL (daemon: run.sh with the named scenarios; b15_boot.sh: b15test.sh on
# the named scenarios (the first scenario that caught it, 11-04), stopping at the first FAIL; m7_boot.sh: m7test.sh; p0prefetch.sh: p0prefetch_test.sh). A mutation whose text is not found exactly once is
# an error (the check would be vacuous). Exit 0 iff every mutation is caught.
#   bash tests/mock/bootrate_mut.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); T=$HERE/../../tools/phxctl
TMP=$(mktemp -d /private/tmp/bootrate-mut.XXXXXX)
. "$HERE/mutlib.sh"   # PAR mutations at once (default 2); each one's mock serial (PAR=1), b15test.sh stopping at its first FAIL
# mut <target daemon|b15|m7|p0pf> <name> <run.sh scenarios (daemon) | b15test.sh B15ONLY glob (b15; "" = all)> <old> <new>
mut() {
  local tg=$1 name=$2 scen=$3 src f
  case $tg in daemon) src=$HERE/../../tools/phxctl/phxboot-navi31.sh ;; b15) src=$T/b15_boot.sh ;; m7) src=$T/m7_boot.sh ;; p0pf) src=$T/p0prefetch.sh ;; esac
  f=$TMP/$name.sh
  if [ $tg = m7 ]; then mkdir -p "$TMP/$name.d"; f=$TMP/$name.d/m7_boot.sh; cp "$T/m7_phase.sh" "$TMP/$name.d/"; fi
  mut_make "$src" "$f" "$4" "$5" || { mut_bg echo "BAD     $name: text not found exactly once"; return; }
  mut_bg mut_check "$tg" "$name" "$scen" "$f"; }
mut_check() { local out
  case $1 in
    daemon) out=$(SCR=$TMP/mock.$2 DAEMON=$4 PAR=1 nice -n 19 bash "$HERE/run.sh" $3 2>&1) ;;
    b15) out=$(B15=$4 B15ONLY="${3:-*}" B15STOP=1 PAR=1 nice -n 19 bash "$HERE/b15test.sh" 2>&1) ;;
    m7) out=$(M7=$4 nice -n 19 bash "$HERE/m7test.sh" 2>&1) ;;
    p0pf) out=$(P0PF=$4 nice -n 19 bash "$HERE/p0prefetch_test.sh" 2>&1) ;;
  esac
  if echo "$out" | grep -q '^FAIL'; then echo "CAUGHT  $1 $2 ($(echo "$out" | grep '^FAIL' | awk '{print $2}' | head -4 | tr '\n' ' '))"
  else echo "MISSED  $1 $2"; fi; }
# daemon aoc_boot
mut daemon no_phase_gate "aocrate_unaligned" 'if [ "$M7BOUND" = 1 ] && [ "$M7PHASE" = 1 ]; then cmd=aocboot' 'if [ "$M7BOUND" = 1 ]; then cmd=aocboot'
mut daemon after_restart "aocrate_144" '  aoc_boot   # boot rate' '  :   # boot rate'
mut daemon after_restart2 "aocrate_144" 'killall -9 WindowServer 2>/dev/null' 'killall -9 WindowServer 2>/dev/null; aoc_boot'
mut daemon no_resync "aocrate_144" 'sysctl -w debug.phxfb_resync=1 >/dev/null 2>&1; id=' 'id='
mut daemon resync_ignored "aocrate_resync_bad" 'if [ "$id" = 2 ]; then' 'if true; then'
mut daemon last_always_144 "aocrate_last_60" 'case "$want" in 60|144) ;;' 'case "$want" in 144) ;;'
mut daemon no_rate_gate "aocrate_60 aocrate_last_60" '[ "$want" = 144 ] || { echo "aocboot: boot rate' '[ -n "$want" ] || { echo "aocboot: boot rate'
mut daemon no_list_gate "aocrate_nolist" '{ [ -e "$G/phxboot-m7-aocmodes" ] || [ -e "$G/phxboot-m7-aoc144" ]; } || { echo "aocboot:' '{ true; } || { echo "aocboot:'
mut daemon no_b15flag_gate "aocrate_nob15flags" '{ [ -e "$G/phxboot-b15" ] && [ -e "$G/phxboot-b15-2pipe" ]; } || { echo "aocboot:' '{ true; } || { echo "aocboot:'
mut daemon no_oid_gate "aocrate_old_accel" 'sysctl -n debug.phxfb_resync >/dev/null 2>&1 || { echo "aocboot: PhxAccel' 'true || { echo "aocboot: PhxAccel'
mut daemon no_nmodes_gate "aocrate_nmodes" '[ "$(sysctl -n debug.phxfb_nmodes 2>/dev/null)" = 2 ] || { echo "aocboot: head 0' 'true || { echo "aocboot: head 0'
mut daemon set_fail_goes_on "aocrate_set_fail one_aoc_refused" '[ $rc = 0 ] || { echo "aocboot: b15_boot.sh $cmd rc $rc: the AOC stays at 60"; return 0; }' ':'
mut daemon no_aocback "aocrate_resync_bad" '  sh "$G/b15_boot.sh" aocback 2>&1; return 0; }' '  return 0; }'
# b15_boot.sh aocboot / aocback / aoc_arm / memory
mut b15 no_stickies "aocboot_144" '  clear_stickies
  step p0prefetch env PHXP0_PAIR165' '  step p0prefetch env PHXP0_PAIR165'
mut b15 no_stickies1 "aocboot1_ok" '  clear_stickies
  step p0prefetch env PHXP0_SOLO' '  step p0prefetch env PHXP0_SOLO'
mut b15 no_pair165 "aocboot_165" 'pf=0; [ $pr = 165 ] && pf=1' 'pf=0'
mut b15 wrong_wm165 "aocboot_165" 'if is165; then wmp=linux60a165; pr=165' 'if is165; then wmp=linux2; pr=165'
mut b15 no_gop_check "aocboot_stutter" '[ "$sa" = 0x00001002 ] || { echo "b15: aocboot:' 'true || { echo "b15: aocboot:'
mut b15 no_det_check "aocboot_det3" '[ $(( d0 & 0x1f )) -eq 9 ] && [ $(( d1 & 0x1f )) -eq 9 ] || { echo "b15: aocboot:' 'true || { echo "b15: aocboot:'
mut b15 no_set_verify "aocboot_set_refused" '  [ $(( ht )) -eq $((0x807)) ] && [ $(( vt )) -eq $((0x44B)) ] || { echo "b15: aocboot: OTG0 $ht/$vt after the set' '  true || { echo "b15: aocboot: OTG0 $ht/$vt after the set'
mut b15 acer60_accepted "aocboot_acer60" '  else echo "b15: aocboot: OTG1 ($c1 $h1/$v1) is not the Acer' '  else wmp=linux2; pr=60; fi; if false; then echo "b15: aocboot: OTG1 ($c1 $h1/$v1) is not the Acer'
mut b15 request_always "rate60_no_request" 'if [ "$(aocrate)" = 144 ]; then aoc_request &' 'if true; then aoc_request &'
mut b15 request_never "rate144_fallback_request" 'if [ "$(aocrate)" = 144 ]; then aoc_request &' 'if false; then aoc_request &'
mut b15 no_144branch_arm "two_at144_arms" '    $SC -w debug.phxpipe0_enable=1 > /dev/null 2>&1 && echo "b15: aoc: pipe-0 engine armed (debug.phxpipe0_enable=1), the AOC at 144 since the boot"' '    :'
mut b15 no_debounce "remember_not_stable" '  [ $(( rnow - rms )) -ge "$RATESTABLE" ] || return 0' '  :'
mut b15 memory_ignores_busy "remember_busy" '  case "$(sw0)$(sw)" in *busy=1*) return 0 ;; esac
  rh0=' '  rh0='
mut b15 memory_165_as_144 "remember_follow_144_165" 'elif [ $rh1 -eq $((0x81F)) ] && [ $rv1 -eq $((0x455)) ]; then rb=165' 'elif [ $rh1 -eq $((0x81F)) ] && [ $rv1 -eq $((0x455)) ]; then rb=144'
mut b15 memory_always "remember_off" 'remember=0; for f in' 'remember=1; for f in'
mut b15 aocback_no_aoc60 "aocback_144" '  if alone; then aoc60solo; else aoc60; fi; }' '  if alone; then aoc60solo; else :; fi; }'
mut b15 aocback_no_aoc60solo "aocback_alone" '  if alone; then aoc60solo; else aoc60; fi; }' '  if alone; then :; else aoc60; fi; }'
# m7_boot.sh 165 / last
mut m7 no_165_retry "" 'if [ $lr != 0 ] && [ "$m" = 165 ]; then' 'if false; then'
mut m7 mode_after_ladder "" 'm=$(S debug.phxpipe2_mode); ladder; lr=$?' 'ladder; lr=$?; m=$(S debug.phxpipe2_mode)'
mut m7 dal165_as_144 "" 'if dal_min 0x00080187 && dal_min 0x00090187 && W debug.phxpipe2_mode=165; then' 'if dal_min 0x0008014b && dal_min 0x0009014b && W debug.phxpipe2_mode=165; then'
mut m7 no_165_to_144 "" 'debug.phxpipe2_mode 165"
  else echo "m7: 165 Hz setup refused (PhxBringup older than b108, or a DAL min refused): the 144 setup"; setup144; fi; }' 'debug.phxpipe2_mode 165"
  else echo "m7: 165 Hz setup refused (PhxBringup older than b108, or a DAL min refused): the 144 setup"; fi; }'
mut m7 det_not_165 "" 'case "$(S debug.phxpipe2_mode)" in 144|165) det99 ;; esac' 'case "$(S debug.phxpipe2_mode)" in 144) det99 ;; esac'
mut m7 last_ignored "" 'RATE=$l; echo "m7: boot rate last' 'RATE=144; echo "m7: boot rate last'
mut m7 last_any_value "" 'case "$l" in 60|144|165) RATE=$l' 'case "$l" in *[0-9]*) RATE=$l'
# p0prefetch.sh
mut p0pf pair165_always "" 'elif [ "$PAIR165" = 1 ] && eq' 'elif eq'
mut p0pf pair165_never "" 'elif [ "$PAIR165" = 1 ] && eq' 'elif false && eq'
# 11-03 p0solo: the AOC alone
mut daemon solo_branch_off "one_aoc" 'elif [ "$M7LIT" = 0 ]; then cmd=aocboot1' 'elif false; then cmd=aocboot1'
mut daemon solo_when_lit "two_unaligned_no_solo" 'elif [ "$M7LIT" = 0 ]; then cmd=aocboot1' 'elif true; then cmd=aocboot1'
mut daemon solo_no_oid "one_aoc_no_solo" 'sysctl -n debug.phxpipe0_solo >/dev/null 2>&1 || {' 'true || {'
mut daemon solo_no_m7flag "one_aoc_m7off aocrate_nom7" '[ -e "$G/phxboot-m7" ] || { echo "aocboot: one display' 'true || { echo "aocboot: one display'
mut daemon solo_no_b15flag "one_aoc_noflag" '[ -e "$G/phxboot-b15" ] || { echo "aocboot: one display' 'true || { echo "aocboot: one display'
mut b15 aocboot1_no_alone "aocboot1_otg1_lit" '  alone || { echo "b15: aocboot1: a second pipe' '  true || { echo "b15: aocboot1: a second pipe'
mut b15 arm1_no_solo "aocboot1_ok" 'arm1() { $SC -w debug.phxpipe0_enable=1 > /dev/null 2>&1 && $SC -w debug.phxpipe0_solo=1 > /dev/null 2>&1; }' 'arm1() { $SC -w debug.phxpipe0_enable=1 > /dev/null 2>&1; }'
mut b15 prep1_no_det "aocboot1_ok" '  step det sh "$G/det.sh" set 9 || return 1' '  :'
mut b15 prep1_p0pf_pair "aocboot1_ok" 'step p0prefetch env PHXP0_SOLO=1 sh' 'step p0prefetch env PHXP0_SOLO=0 sh'
mut b15 aoc144solo_wm_pair "aocboot1_ok" '  step wm144 sh "$G/wm144.sh" linux || return 1
  echo "b15: $(date '"'"'+%T'"'"') AOC 144 alone' '  step wm144 sh "$G/wm144.sh" linux144x2 || return 1
  echo "b15: $(date '"'"'+%T'"'"') AOC 144 alone'
mut b15 aoc144solo_no_hubp "aocboot1_hubp0_off" 'grp HUBP0 "$AOC_HUBP0" || { echo "b15: aoc144solo:' 'true || { echo "b15: aoc144solo:'
mut b15 one_aoc_off "one_aoc_144" '
alone && one_aoc' '
: one_aoc'
mut b15 one_aoc_takes_acer "acer_alone_recipe" '  if [ $(( ht )) -eq $((0x807)) ] && [ $(( vt )) -eq $((0x44B)) ]; then
    aoc144solo || exit 1' '  if [ $(( ht )) -eq $((0x807)) ]; then
    aoc144solo || exit 1'
mut b15 watch1_blind "watch1_underflow" 'p1=$(sw0); fi; k=""' 'p1=$(sw0); fi; v=0; h=0; k=""'
mut b15 aocback_pair "aocback_alone" '  if alone; then aoc60solo; else aoc60; fi; }' '  aoc60; }'
mut p0pf solo_always "" 'elif [ "$SOLO" = 1 ] && eq' 'elif eq'
mut p0pf solo_never "" 'elif [ "$SOLO" = 1 ] && eq' 'elif false && eq'
mut_end; echo "caught $CAUGHT, missed $MISSED, bad $BAD"
rm -rf "$TMP"
[ $MISSED = 0 ] && [ $BAD = 0 ]
