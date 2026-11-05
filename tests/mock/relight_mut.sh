#!/bin/bash
# SPDX-License-Identifier: MIT
# relight_mut.sh (11-03): mutation check of b15_boot.sh's Acer relight
# (watch2's rl_* steps). Each mutation breaks one property on a copy of b15_boot.sh; b15test.sh's relight scenarios
# (B15ONLY='rl_*') must then FAIL. A mutation whose text is not found exactly once is an error (the check would be vacuous).
# Exit 0 iff every mutation is caught.
#   bash tests/mock/relight_mut.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); SRC=$HERE/../../tools/phxctl/b15_boot.sh
TMP=$(mktemp -d /private/tmp/relight-mut.XXXXXX)
. "$HERE/mutlib.sh"   # PAR mutations at once (default 2)
# mut <name> <B15ONLY glob: the rl_* scenarios that exercise the mutated step> <old> <new>. RLALL=1: every mutation against all
# rl_* (the pre-11-04 check: each stops at its first FAIL anyway, but runs the scenarios before it)
mut() {
  local name=$1 only=$2 f=$TMP/$1.sh
  [ "${RLALL:-0}" = 1 ] && only='rl_*'
  mut_make "$SRC" "$f" "$3" "$4" || { mut_bg echo "BAD     $name: text not found exactly once"; return; }
  mut_bg mut_check "$name" "$only" "$f"; }
mut_check() { local out
  out=$(B15=$3 B15ONLY="$2" B15STOP=1 PAR=1 nice -n 19 bash "$HERE/b15test.sh" 2>&1)   # a caught mutation stops at its first FAIL
  if echo "$out" | grep -q '^FAIL'; then echo "CAUGHT  $1 ($(echo "$out" | grep '^FAIL' | awk '{print $2}' | head -4 | tr '\n' ' '))"
  else echo "MISSED  $1 (B15ONLY=$2)"; fi; }
mut no_event_gate 'rl_dark_relight'        '  if [ "$rlho" -gt "$rlo" ] || [ "$rlhg" -gt "$rlg" ]; then' '  if true; then'
mut no_delay 'rl_dark_relight'        'rls=wait; rlt=$((rlnow + RLDELAY)); rln=0' 'rls=wait; rlt=$rlnow; rln=0'
mut no_165_gate 'rl_at144'               'if [ "$m1" != 165 ] || ! is165; then rls=idle
    echo "b15: $(date '"'+%T'"') relight: pipe 1 at ${m1:-?} Hz (OTG1' 'if false; then rls=idle
    echo "b15: $(date '"'+%T'"') relight: pipe 1 at ${m1:-?} Hz (OTG1'
mut no_lit_gate 'rl_scdc_fine'           'if rl_lit "$x"; then rls=idle; echo "b15: $(date '"'+%T'"') relight: pipe 1 at 165, SCDC' 'if false; then rls=idle; echo "b15: $(date '"'+%T'"') relight: pipe 1 at 165, SCDC'
mut lit_locked_only 'rl_unscrambled_locked'  '[ "$(rl_kv "$1" tmds_config)" = 0x03 ] && [ "$(rl_kv "$1" scrambler)" = 0x01 ] && ' ''
mut no_rc_gate 'rl_unreadable'          '  case "$(rl_kv "$x" rc)" in 0) ;; *)' '  case "$(rl_kv "$x" rc)" in *) ;; x)'
mut lock_any_read 'rl_nolock144'           'then rlk=$((rlk + 1)); else rlk=0; fi' 'then rlk=$((rlk + 1)); else rlk=$((rlk + 1)); fi'
mut no_lock_wait 'rl_nolock144'           'if [ $rlk -ge 2 ]; then' 'if [ $rlk -ge 0 ]; then'
mut lock_once 'rl_flicker144'          'if [ $rlk -ge 2 ]; then' 'if [ $rlk -ge 1 ]; then'
mut no_lock_timeout 'rl_nolock144'           'if [ $(( rlnow - rl0 )) -ge 10000 ]; then rls=idle' 'if false; then rls=idle'
mut no_fallback 'rl_fail165'             'if rl_sw 144; then echo "b15: $(date '"'+%T'"') relight: no scrambled' 'if true; then echo "b15: $(date '"'+%T'"') relight: no scrambled'
mut no_busy_gate 'rl_p0busy'              'rl_ready() { case "$(sw0)$(sw)" in *busy=1*) return 1 ;; esac;' 'rl_ready() { case "" in *busy=1*) return 1 ;; esac;'
mut no_pend_gate 'rl_pend'                '[ "$rlpend" = none ]; }' 'true; }'
mut no_user_gate 'rl_user60'              '[ "$m1" = 144 ] || { rls=idle; echo "b15: $(date '"'+%T'"') relight: pipe 1 at' 'true || { rls=idle; echo "b15: $(date '"'+%T'"') relight: pipe 1 at'
mut no_rl_in_watch2 'rl_dark_relight'        '    [ $rlon = 1 ] && rl_tick' '    :'
mut no_stat_gate 'rl_dark_relight'        '  hpstat && { rlon=1; rlo=$rlho; rlg=$rlhg; return 0; }' '  hpstat && { rlon=1; rlo=0; rlg=0; return 0; }'
mut no_arm_wait 'rl_arm_late'            '"hotplug "*) rlon=2;' '"hotplug "*) rlon=0;'
mut no_arm_in_watch2 'rl_arm_late'            '; [ $rlon = 2 ] && rl_arm' ''
mut no_retry 'rl_retry_ok'            '  elif [ $rlr -lt $RLRETRY ]; then' '  elif false; then'
mut no_retry_reset 'rl_retry_event2'        'rln=0; rlr=0 ;;' 'rln=0 ;;'
mut retry_keeps_rln 'rl_retry_nack'          'rls=wait; rln=0; rlt=$((rlnow + RLRETRYMS))' 'rls=wait; rlt=$((rlnow + RLRETRYMS))'
mut retry_unbounded 'rl_switch_refused'      '  elif [ $rlr -lt $RLRETRY ]; then' '  elif [ $rlr -lt 999 ]; then'
mut why_parsed 'rl_dark_relight'        'if (a[1] == "why") break; ' ''
mut arm_baseline0 'rl_arm_late'            '  rlon=1; rlo=$rlho; rlg=$rlhg
  echo' '  rlon=1; rlo=0; rlg=0
  echo'
# the macOS path (rl_sw, 11-04)
mut always_sysctl 'rl_dark_relight'        'if [ -z "$rlu" ] || [ "$rlu" = 0 ]; then rlwhy="no console user"' 'if true; then rlwhy="no console user"'
mut never_sysctl 'rl_switch_refused'      '  echo "b15: $(date '"'+%T'"') relight: $rlhz through the kernel' '  return 1; echo "b15: $(date '"'+%T'"') relight: $rlhz through the kernel'
mut root_is_user 'rl_noconsole'           ' || [ "$rlu" = 0 ]; then rlwhy=' '; then rlwhy='
mut wrong_size 'rl_rotated'             'rlw=${rlsz%x*}; rlh=${rlsz#*x}' 'rlw=1920; rlh=1080'
mut first_current 'rl_rotated'             'awk -v d="$rld" '"'"'/^display/ {on = ($4 == d)} on && / current/' 'awk -v d="$rld" '"'"'/ current/'
mut any_model 'rl_dark_relight'        'tolower($6) == tolower(m) {print $4; exit}' '1 {print $4; exit}'
mut model_fixed 'rl_cg_model'            'ACERMODEL=${PHXB15_ACERMODEL:-0x74d}' 'ACERMODEL=0x74d'
mut cg_fail_ignored 'rl_switch_refused'      '      else rlwhy=$rlcg; fi' '      else return 0; fi'
mut no_result_check 'rl_cg_noop'             'if [ "$rlm" = "$rlhz" ]; then' 'if true; then'
mut macos_then_kernel 'rl_dark_relight'        'relight: $rlhz through macOS ($rlcg)"; return 0; fi' 'relight: $rlhz through macOS ($rlcg)"; fi'
mut no_clock_refresh 'rl_nolock144'           'if [ "$rlm" = "$rlhz" ]; then hpstat; return 0; fi' 'if [ "$rlm" = "$rlhz" ]; then return 0; fi'
mut wrong_hz 'rl_dark_relight'        '"$rlw" "$rlh" "$rlhz" 2>&1)' '"$rlw" "$rlh" 165 2>&1)'
# the settle re-set (11-04)
mut reset_always 'rl_reset_kernel'        'if [ $rlmac = 1 ] && [ "$RLRESET" -gt 0 ]; then' 'if [ "$RLRESET" -gt 0 ]; then'
mut reset_ignores_off 'rl_reset_off'           'if [ $rlmac = 1 ] && [ "$RLRESET" -gt 0 ]; then' 'if [ $rlmac = 1 ]; then'
mut rlmac_not_set 'rl_dark_relight'        'then rlmac=1; echo' 'then echo'
mut reset_no_wait 'rl_dark_relight'        'rls=settle; rlt=$((rlnow + RLRESET * 1000))' 'rls=settle; rlt=$rlnow'
mut reset_no_165_gate 'rl_reset_user60'        'if [ "$m1" != 165 ] || ! is165; then rls=idle
    echo "b15: $(date '"'+%T'"') relight: settle re-set skipped' 'if false; then rls=idle
    echo "b15: $(date '"'+%T'"') relight: settle re-set skipped'
mut reset_via_kernel 'rl_reset_cgfail'        '  if rl_cg 144; then rls=rs144;' '  if rl_sw 144; then rls=rs144;'
mut reset_165_via_kernel 'rl_reset_cgfail165'     '  if rl_cg 165; then rls=rs165;' '  if rl_sw 165; then rls=rs165;'
mut reset_no_lock_wait 'rl_reset_nolock'        '  if [ "$(rl_kv "$x" rc)" = 0 ] && [ "$(rl_kv "$x" locked)" = 1 ]; then rlv=' '  if true; then rlv='
mut reset_no_timeout 'rl_reset_nolock'        'elif [ $(( rlnow - rl0 )) -ge 10000 ]; then rlv=' 'elif false; then rlv='
mut reset_user_ignored 'rl_reset_user_rs144'    '[ "$m1" = 144 ] || { rls=idle; echo "b15: $(date '"'+%T'"') relight: settle re-set:' 'true || { rls=idle; echo "b15: $(date '"'+%T'"') relight: settle re-set:'
mut reset_no_verify 'rl_reset_fail165'       'if rl_lit "$x"; then rls=idle; echo "b15: $(date '"'+%T'"') relight: settle re-set done' 'if true; then rls=idle; echo "b15: $(date '"'+%T'"') relight: settle re-set done'
mut reset_loop 'rl_reset_fail165'       'left as it is, no retry"; }' 'left as it is, no retry"; rls=settle; rlt=$((rlnow + RLRESET * 1000)); }'
mut reset_event_ignored 'rl_reset_event'         '      idle|wait|settle) echo' '      idle|wait) echo'
mut reset_not_scheduled 'rl_reset'               '; settle) rl_settle ;;' ';'
mut_end; echo "RESULT: $CAUGHT caught, $MISSED missed, $BAD bad"; rm -rf "$TMP"; [ $MISSED = 0 ] && [ $BAD = 0 ]
