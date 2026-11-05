#!/bin/sh
# SPDX-License-Identifier: MIT
# p0prefetch.sh (10-31, the two-display pipe-0 underflow fix; mock tests/mock/p0prefetch_test.sh; NOT deployed yet).
# Pipe 0 (AOC, 1080p60 on DP, timing left by the GOP) frame-position group -> Linux v6.8 DML values for the same timing
# next to the Acer at 144 (a Linux register capture, 10-30). The GOP leaves VSTARTUP 13 lines: DML's
# budget between VSTARTUP and the prefetch (VStartup - dst_y_prefetch - after_scaler) is 3 lines = 44 us, while Linux's
# VSTARTUP 44 leaves 32.2 lines = 477 us >= DML TWait (400 us UCLK latency + urgent latency) + TSetup.
#   p0prefetch.sh show     read-only: the group now / GOP / Linux, the related registers we do NOT write, the checks
#   p0prefetch.sh linux    save the GOP values once (~/gpu/p0prefetch.saved), write the Linux values, verify
#   p0prefetch.sh gop      write back the saved values, verify byte-exact, remove the save file
# Write order = Linux's (dc.c commit: interdependent_update_lock -> dcn20_program_pipe: program_global_sync
# (VSTARTUP/VUPDATE/VREADY), wait_for_state(VACTIVE), set_vtg_params (VTG0 FP2), then hubp setup (deadline:
# BLANK_OFFSET_1, DST_AFTER_SCALER) and setup_interdependent (PREFETCH_SETTINGS, VBLANK_PARAMETERS_0) -> unlock), all
# inside OTG0_OTG_MASTER_UPDATE_LOCK (0x504A, optc3_lock: write 1, wait UPDATE_LOCK_STATUS bit 8). The double-buffered
# registers latch together at the first VUPDATE after the unlock.
# Holding the lock blocks pipe-0 flips: phxfb gives a flip up after 3 frames and turns flipping off for head 0
# (phxfb.cpp flipWaitLocked). If debug.phxfb_flip was 1 before and is 0 after, this script sets it back to 1 and says so.
# A guard subshell unlocks OTG0 if the script dies inside the lock (after $PHXP0_GUARD_S s, default 2; it leaves
# /tmp/p0prefetch.lock.fired). Manual emergency unlock: sudo ~/gpu/phxctl.bin wreg 0x504A 0
# Precondition (refuses otherwise, nothing written): OTG0 lit at the Linux-captured 1080p60 timing and OTG1 at 1080p144
# (the pair of the Linux capture), stutter forced off
# (0x39BC 0x1002), HUBP0 not blanked and HUBP_VREADY_AT_OR_AFTER_VSYNC 0, the rate group at hubp_dlg.sh linux60
# values, OTG0 not locked and its lock select = OTG0, no OPTC0 underflow in progress, no kext mode switch busy, and every
# register of the group at either its GOP value (or the saved one) or its Linux value.
# Registers: absolute dword = 0x34C0 + dcn_3_2_0_offset.h (BASE_IDX 2). Writes only the 8 group registers + 0x504A.
# Never touches watermarks, DET, 0x39BC, surface registers, the clock controls, or 0x1A751-0x1A762.
# Run as root (sudo sh p0prefetch.sh ...) so each phxctl call skips sudo and the lock is held for fewer frames.
# Boot rate (11-02): PHXP0_PAIR165=1 also accepts OTG1 at the Acer's 1080p165
# (0x81F/0x455), for b15_boot.sh aocboot only: the kext's 60 -> 60 round trip needs every 60-plan register equal, and the
# 60 plan's frame group IS these Linux values (phx_p0_plan_1080p60.inc stage 3). Pipe 0 ran exactly these values next to
# the Acer at 165 with 0 underflow in a live test (the kext's 60 plan, 1.6 min); Linux's own 60 + 165 group (c2) differs
# in VUPDATE width, DST_AFTER_SCALER and DST_Y_PREFETCH. Default 0 = the 144 pair only, as before.
# p0solo (11-03): PHXP0_SOLO=1 also accepts OTG1 dark (the AOC
# alone), for b15_boot.sh aocboot1 / the one-display branch only: the same reason (the kext's 60 round trip needs every
# 60-plan register equal, and the 60 plan's frame group IS these values); with one display pipe 0 has all the bandwidth
# the two-display DML budget shared, so these values are the conservative side [I]. Default 0 = OTG1 must be lit, as before.
set -u
G=${PHXP0_G:-$HOME/gpu}; P=${PHXP0_PHXCTL:-$G/phxctl.bin}; SAVE=${PHXP0_SAVE:-$G/p0prefetch.saved}
if [ -n "${PHXP0_SUDO+x}" ]; then SU=$PHXP0_SUDO; elif [ "$(id -u)" = 0 ]; then SU=; else SU="sudo -n"; fi
SW=${PHXP0_SWSTAT:-sysctl -n debug.phxpipe2_switch_stat}
FLIPGET=${PHXP0_FLIPGET:-sysctl -n debug.phxfb_flip}; FLIPON=${PHXP0_FLIPON:-sysctl -w debug.phxfb_flip=1}
GUARD=${PHXP0_GUARD_S:-2}; LK=${PHXP0_LOCKFILE:-/tmp/p0prefetch.lock}; PAIR165=${PHXP0_PAIR165:-0}; SOLO=${PHXP0_SOLO:-0}
# abs | name | GOP value (a macOS register capture, 10-31) | Linux value (the Linux capture), write order
GRP='0x5046|OTG0_OTG_VSTARTUP_PARAM (VSTARTUP_START, lines: 13 -> 44 = V_TOTAL - V_ACTIVE - 1)|0x0000000D|0x0000002C
0x5047|OTG0_OTG_VUPDATE_PARAM (WIDTH 320 -> 106 px, OFFSET 550 = H_TOTAL/4)|0x01400226|0x006A0226
0x5048|OTG0_OTG_VREADY_PARAM (VREADY_OFFSET 300 -> 149 px)|0x0000012C|0x00000095
0x39EE|VTG0_CONTROL (FP2 0 -> 2: VSTARTUP before VSYNC; ENABLE, VCOUNT_INIT 0x461 kept)|0x84610000|0x84610002
0x3B04|HUBPREQ0_BLANK_OFFSET_1 (MIN_DST_Y_NEXT_START 1101.5 -> 1122 lines)|0x00001136|0x00001188
0x3B06|HUBPREQ0_DST_AFTER_SCALER (REFCYC_X_AFTER_SCALER 0 -> 51)|0x00000000|0x00000033
0x3B07|HUBPREQ0_PREFETCH_SETTINGS (DST_Y_PREFETCH 10.0 -> 11.75 lines, VRATIO 1.0 -> 1.143)|0x28080000|0x2F092492
0x3B09|HUBPREQ0_VBLANK_PARAMETERS_0 (DST_Y_PER_ROW_VBLANK 0.75 -> 0.5, PER_VM 0.25)|0x00000301|0x00000201'
# Linux-different pipe-0 registers deliberately NOT written (shared with pipe 1, which never underflows; next variable
# only if this group fails): request sizes, row/meta deadlines, chroma/cursor TTU.
OTHER='0x3AB1|HUBP0_DCHUBP_REQ_SIZE_CONFIG|0x055D0300|0x033D1B40
0x3AB2|HUBP0_DCHUBP_REQ_SIZE_CONFIG_C|0x00000000|0x003D1B01
0x3AE8|HUBPREQ0_DCN_EXPANSION_MODE|0x00000055|0x00000056
0x3AED|HUBPREQ0_DCN_SURF1_TTU_CNTL0|0x00000000|0x08000000
0x3AEF|HUBPREQ0_DCN_CUR0_TTU_CNTL0|0x00000000|0x08000000
0x3B0A|HUBPREQ0_VBLANK_PARAMETERS_1 (REFCYC_PER_PTE_GROUP_VBLANK_L)|0x000001A4|0x00000000
0x3B0C|HUBPREQ0_VBLANK_PARAMETERS_3 (REFCYC_PER_META_CHUNK_VBLANK_L)|0x000001A4|0x00000000
0x3B12|HUBPREQ0_NOM_PARAMETERS_1 (REFCYC_PER_PTE_GROUP_NOM_L)|0x0000D249|0x00000000
0x3B15|HUBPREQ0_NOM_PARAMETERS_4 (DST_Y_PER_META_ROW_NOM_L)|0x00000020|0x00000080
0x3B16|HUBPREQ0_NOM_PARAMETERS_5 (REFCYC_PER_META_CHUNK_NOM_L)|0x00000D24|0x00000000'
SYNC="0x5046 0x5047 0x5048"; VTG=0x39EE; HUBP="0x3B04 0x3B06 0x3B07 0x3B09"
r() { $SU "$P" regs "$1" | awk '{print $NF}'; }
w() { o=$($SU "$P" wreg "$1" "$2") || { echo "wreg $1 failed" >&2; return 1; }; echo "  $o"; }
eq() { [ "$(( $1 ))" -eq "$(( $2 ))" ]; }
col() { echo "$GRP" | awk -F'|' -v a="$1" -v c="$2" '$1 == a {print $c}'; }
saved() { awk -F'|' -v a="$1" '$1 == a {print $3}' "$SAVE"; }
x8() { printf '0x%08X' "$(( $1 ))"; }
show_rows() { echo "$1" | while IFS='|' read -r a n g l; do c=$(r "$a"); s=OTHER; eq "$c" "$l" && s=LINUX; eq "$c" "$g" && s=GOP
    printf '%s %-92s now %s gop %s linux %s %s\n' "$a" "$n" "$c" "$g" "$l" "$s"; done; }
budget() { vs=$(( $(r 0x5046) & 0x3FF )); pf=$(( ($(r 0x3B07) >> 24) & 0xFF )); fp2=$(( $(r 0x39EE) & 0x7FFF ))
    xa=$(( $(r 0x3B06) & 0x1FFF )); rf=$(( $(r 0x3B1C) ))
    awk -v vs="$vs" -v pf="$pf" -v fp2="$fp2" -v xa="$xa" -v rf="$rf" 'BEGIN { lt = 2200 / 148.5; s = 41 - vs + 1; if (s < 0) s += 1125
      xl = (rf > 0 ? xa / (rf / 524288) / 2200 : 0); b = vs - pf / 4 - xl
      printf "frame layout: VSTARTUP %d lines (OTG line %d), FP2 %d (Linux rule %d), prefetch %.2f lines (from line %.2f);\n", vs, s, fp2, (vs > 41 ? vs - 42 : 0), pf / 4, 41 - pf / 4
      printf "  DML budget VStartup - dst_y_prefetch - after_scaler = %.2f lines = %.0f us (TSetup + TWait: one UCLK switch needs >= 27.3 lines = 405 us)\n", b, b * lt }'; }
checks() {   # prints the first failed precondition and returns 1, or returns 0
  eq "$(( $(r 0x5001) & 1 ))" 1 || { echo "OTG0 not enabled"; return 1; }
  for p in 0x4FEA:0x00000897 0x4FEF:0x00000464 0x4FEB:0x00C00840 0x4FF6:0x00290461; do a=${p%%:*}; v=${p#*:}; c=$(r "$a")
    eq "$c" "$v" || { echo "OTG0 timing $a = $c, not $v (the Linux-captured 1080p60): the target values do not apply"; return 1; }; done
  if eq "$(( $(r 0x5081) & 1 ))" 1 && eq "$(r 0x506A)" 0x807 && eq "$(r 0x506F)" 0x449; then :
  elif [ "$PAIR165" = 1 ] && eq "$(( $(r 0x5081) & 1 ))" 1 && eq "$(r 0x506A)" 0x81F && eq "$(r 0x506F)" 0x455; then :
  elif [ "$SOLO" = 1 ] && eq "$(( $(r 0x5081) & 1 ))" 0; then :
  else echo "OTG1 is not lit at 1080p144 (H_TOTAL $(r 0x506A)): Linux's values are for the AOC 60 + Acer 144 pair$([ "$PAIR165" = 1 ] && echo " (or, PHXP0_PAIR165=1, the Acer at 1080p165)")"; return 1; fi
  c=$(r 0x39BC); eq "$c" 0x1002 || { echo "0x39BC = $c, not 0x1002 (stutter allowed): two variables"; return 1; }
  h=$(r 0x3AB3); [ $(( h & 1 )) -eq 0 ] || { echo "HUBP0 blanked ($h)"; return 1; }
  [ $(( h & 0x100 )) -eq 0 ] || { echo "HUBP0 VREADY_AT_OR_AFTER_VSYNC = 1 ($h): Linux's target state has 0"; return 1; }
  for p in 0x3AEA:0xE0005C6C 0x3B05:0x0002E4BD 0x3B1C:0x0002B18F; do a=${p%%:*}; v=${p#*:}; c=$(r "$a")
    eq "$c" "$v" || { echo "rate group $a = $c, not the linux60 value $v (run hubp_dlg.sh linux60 first)"; return 1; }; done
  c=$(r 0x504A); eq "$c" 0 || { echo "OTG0_OTG_MASTER_UPDATE_LOCK = $c (already locked)"; return 1; }
  c=$(r 0x504F); [ $(( c & 0x80000000 )) -eq 0 ] || { echo "OTG0 MASTER_UPDATE_LOCK_DB_EN set ($c)"; return 1; }
  c=$(r 0x5051); [ $(( (c >> 25) & 7 )) -eq 0 ] || { echo "OTG0 OTG_MASTER_UPDATE_LOCK_SEL = $(( (c >> 25) & 7 )), not OTG0"; return 1; }
  c=$(r 0x4F8A); [ $(( c & 0x2000 )) -eq 0 ] || { echo "OPTC0 underflow in progress (0x4F8A $c)"; return 1; }
  case "$($SW 2>/dev/null)" in *busy=1*) echo "kext mode switch busy"; return 1 ;; esac
  return 0; }
# apply <source>: source = linux (column 4) or gop (the save file)
apply() {
  for a in $SYNC $VTG $HUBP; do if [ "$1" = linux ]; then v=$(col "$a" 4); else v=$(saved "$a"); fi
    [ -n "$v" ] || { echo "no value for $a" >&2; exit 2; }; eval "V_$a=\$v"; done
  f0=$(r 0x500C); fl0=$($FLIPGET 2>/dev/null)
  rm -f "$LK.fired"
  ( sleep "$GUARD"; [ -e "$LK" ] && { $SU "$P" wreg 0x504A 0x0; rm -f "$LK"; date '+%T guard: OTG0 unlocked' > "$LK.fired"; } ) > /dev/null 2>&1 &
  : > "$LK"; trap 'w 0x504A 0x0 > /dev/null; rm -f "$LK"' EXIT INT TERM
  w 0x504A 0x00000001 || exit 4; i=0
  while [ $(( $(r 0x504A) & 0x100 )) -eq 0 ]; do i=$((i + 1)); [ $i -lt 20 ] || { echo "OTG0 UPDATE_LOCK_STATUS never 1: unlocked, nothing else written" >&2; exit 4; }; done
  for a in $SYNC; do eval "w $a \$V_$a" || exit 4; done            # program_global_sync
  i=0; while [ $(( $(r 0x5009) & 2 )) -eq 0 ] && [ $i -lt 40 ]; do i=$((i + 1)); done   # wait_for_state(VACTIVE)
  eval "w $VTG \$V_$VTG" || exit 4                                  # set_vtg_params (FP2)
  for a in $HUBP; do eval "w $a \$V_$a" || exit 4; done            # hubp setup + setup_interdependent
  [ -e "$LK" ] || { trap - EXIT INT TERM; echo "guard fired before the unlock: the writes were not atomic" >&2; exit 6; }
  w 0x504A 0x00000000; rm -f "$LK"; trap - EXIT INT TERM
  i=0; while [ $(( $(r 0x501B) & 0x201 )) -ne 0 ] && [ $i -lt 20 ]; do i=$((i + 1)); done   # UPDATE / VSTARTUP_DB pending
  f1=$(r 0x500C); echo "OTG0 frames while locked (upper bound): $(( (f1 - f0) & 0xFFFFFF ))"
  fl1=$($FLIPGET 2>/dev/null)
  if [ "$fl0" = 1 ] && [ "$fl1" = 0 ]; then $FLIPON > /dev/null && echo "phxfb head-0 flip had turned itself off under the lock: set back to 1 ($($FLIPGET 2>/dev/null))"; fi
  bad=0; for a in $SYNC $VTG $HUBP; do c=$(r "$a"); eval "v=\$V_$a"; eq "$c" "$v" || { echo "verify $a = $c, expected $v" >&2; bad=1; }; done
  return $bad; }
case "${1:-}" in
  show) show_rows "$GRP"; echo "-- not written (shared with pipe 1):"; show_rows "$OTHER"; budget
     why=$(checks) && echo "preconditions: OK" || echo "preconditions: NO ($why)"
     if [ -f "$SAVE" ]; then echo "save file $SAVE present"; fi ;;
  linux) why=$(checks) || { echo "refused: $why" >&2; exit 3; }
     n=0; for a in $SYNC $VTG $HUBP; do c=$(r "$a"); l=$(col "$a" 4); if [ -f "$SAVE" ]; then g=$(saved "$a"); else g=$(col "$a" 3); fi
       eq "$c" "$l" || { n=$((n + 1)); eq "$c" "$g" || { echo "refused: $a = $c is neither the GOP/saved $g nor Linux $l" >&2; exit 3; }; }
       [ -f "$SAVE" ] || eq "$c" "$g" || { echo "refused: $a already Linux but no save file (state unknown)" >&2; exit 3; }; done
     [ $n -gt 0 ] || { echo "already Linux, nothing written"; exit 0; }
     [ -f "$SAVE" ] || { echo "$GRP" | while IFS='|' read -r a nm g l; do echo "$a|$nm|$(x8 "$(r "$a")")"; done > "$SAVE"; echo "saved $SAVE"; }
     echo "--- $(date +%T) pipe-0 frame-position group -> Linux (OTG0 lock)"; apply linux || exit 5; budget ;;
  gop) [ -f "$SAVE" ] || { echo "no $SAVE: nothing to restore (GOP values are column 3 of this script)" >&2; exit 2; }
     why=$(checks) || { echo "refused: $why" >&2; exit 3; }
     echo "--- $(date +%T) pipe-0 frame-position group -> saved GOP values (OTG0 lock)"
     apply gop || { echo "gop restore NOT byte-exact: $SAVE kept" >&2; exit 5; }
     echo "gop restore byte-exact: OK"; rm -f "${SAVE:?}"; budget ;;
  *) echo "usage: p0prefetch.sh show | linux | gop" >&2; exit 2 ;;
esac
