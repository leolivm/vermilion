#!/bin/sh
# SPDX-License-Identifier: MIT
# det.sh (reviewed; in use since 10-29: boot daemon b15_boot.sh step 2).
# Resizes the DCHUBBUB DET of pipe 0 the way amdgpu dcn32_program_det_size() does it
# (REG_UPDATE(DCHUBBUB_DET0_CTRL, DET0_SIZE, n) on an enabled pipe, then wait for DET0_SIZE_CURRENT like the REG_WAIT in
# dcn32_program_compbuf_size()). macOS, on the box, sudo -n, any kext with phxctl wreg (selector 108).
# M7 S5a (10-29): `-p <pipe>` (first argument) works on DET<pipe> instead of DET0, for the two-display split of the
# Linux capture (DET0 = DET1 = 9; a register capture of Linux amdgpu, 10-29). Without -p
# everything is as before (pipe 0; b15_boot.sh step 2). 0 segments is allowed only on a blanked pipe 1-3.
#   det.sh [-p N] show          DET0-3, COMPBUF, CRB budget, HUBP<N> status (read-only)
#   det.sh [-p N] set <seg> [--lock|--blank]  DET<N> = <seg> x 64 KB (9 = Linux 2-pipe value, 18 = Linux 1-pipe linear value).
#                               Default: live, plain write. --lock: inside OTG0_OTG_MASTER_UPDATE_LOCK (0x504A), as Linux
#                               programs pipes (fallback 1). --blank: HUBP0 blanked around the write, as in the Linux
#                               pipe-enable path of the trace (one black frame; fallback 2).
#   det.sh [-p N] clear         clears the OPTC<N> + HUBP<N> underflow stickies (as b15_boot.sh does for pipe 0), then show:
#                               clear, wait, show = did pipe N underflow in that window (the stickies survive from boot)
#   det.sh [-p N] rollback      puts back the DET<N>_CTRL value saved by the last `set` (default GOP 0x303 = 3 segments)
# Never touches COMPBUF (stays 2 = 128 KB; our scanout is linear, uncompressed: compbuf is not used by it), watermarks
# or DRAM_STATE_CNTL. Stutter stays as it is: run with 0x39BC = 0x1002 first (one variable).
# Registers (0x34C0 + dcn_3_2_0_offset.h, BASE_IDX 2): COMPBUF_CTRL 0x397A (SIZE 4:0, CURRENT 12:8, CONFIG_ERROR 31),
# DET0..3_CTRL 0x397B..0x397E (SIZE 4:0, CURRENT 12:8), HUBP0_DCHUBP_CNTL 0x3AB3 (BLANK_EN 0, NO_OUTSTANDING_REQ 1,
# IN_BLANK 3, SEG_ALLOC_ERR 11, UNDERFLOW_STATUS 30:28), ODM0_OPTC_INPUT_GLOBAL_CONTROL 0x4F8A (underflow bit 10).
set -u
N=0; if [ "${1:-}" = -p ]; then N=${2:?pipe}; shift 2; fi
case "$N" in 0|1|2|3) ;; *) echo "pipe 0..3 only" >&2; exit 2;; esac
hx() { printf 0x%X "$1"; }
DET=$(hx $(( 0x397B + N ))); LOCK=$(hx $(( 0x504A + 0x80 * N ))); HUBP=$(hx $(( 0x3AB3 + 0xDC * N ))); OPTC=$(hx $(( 0x4F8A + 0x10 * N )))
P=${PHXCTL:-$HOME/gpu/phxctl.bin}; SAVE=$HOME/gpu/det$N.saved; CRB=20   # 1280 KB / 64 KB (dcn32_fpu.c config_return_buffer_size_in_kbytes)
SUDO=${SUDO-sudo -n}
r() { $SUDO "$P" regs "$1" | awk '{print $NF}'; }
w() { $SUDO "$P" wreg "$1" "$2" > /dev/null || { echo "wreg $1 failed" >&2; exit 1; }; }
sz() { echo $(( $1 & 0x1F )); }
cur() { echo $(( ($1 >> 8) & 0x1F )); }
wait_cur() { i=0; while [ $i -lt 200 ]; do v=$(r $DET); [ "$(cur $v)" -eq "$1" ] && { echo "DET${N}_SIZE_CURRENT = $1 after $i polls"; return 0; }; i=$((i + 1)); done; echo "DET${N}_SIZE_CURRENT stuck at $(cur $v) (want $1)" >&2; return 1; }
show() {
  c=$(r 0x397A); d0=$(r 0x397B); d1=$(r 0x397C); d2=$(r 0x397D); d3=$(r 0x397E); h=$(r $HUBP); o=$(r $OPTC)
  used=$(( $(sz $d0) + $(sz $d1) + $(sz $d2) + $(sz $d3) + $(sz $c) ))
  echo "COMPBUF_CTRL $c (size $(sz $c) cur $(cur $c) config_error $(( ($c >> 31) & 1 )))"
  echo "DET0 $d0 DET1 $d1 DET2 $d2 DET3 $d3 (size/cur: $(sz $d0)/$(cur $d0) $(sz $d1)/$(cur $d1) $(sz $d2)/$(cur $d2) $(sz $d3)/$(cur $d3))"
  echo "CRB used $used of $CRB segments; Linux compbuf for this DET0 = $(( CRB - $(sz $d0) - $(sz $d1) - $(sz $d2) - $(sz $d3) )) (Linux formula 1280 KB - DET; Linux keeps 2 for its 1-pipe DET0 18; we keep 2)"
  echo "HUBP${N}_DCHUBP_CNTL $h (blank $(( $h & 1 )) seg_alloc_err $(( ($h >> 11) & 1 )) underflow $(( ($h >> 28) & 7 )))  OPTC $OPTC $o (underflow $(( ($o >> 10) & 1 )))"
}
case "${1:-}" in
  show) show ;;
  set)
    n=${2:?segments}; blank=${3:-}
    [ "$n" -ge 0 ] && [ "$n" -le 18 ] || { echo "segments 0..18 only" >&2; exit 2; }
    h0=$(r $HUBP)
    [ "$n" -ge 1 ] || { [ "$N" != 0 ] && [ $(( h0 & 1 )) -eq 1 ]; } || { echo "0 segments only on a blanked pipe 1-3 (HUBP$N $h0)" >&2; exit 2; }
    [ "$(r 0x39BC)" = 0x00001002 ] || echo "NOTE: 0x39BC is not 0x1002 (stutter allowed): this run has two variables" >&2
    c=$(r 0x397A); d=$(r $DET); all=0
    for a in 0x397B 0x397C 0x397D 0x397E; do all=$(( all + $(sz $(r $a)) )); done
    [ $(( c >> 31 )) -eq 0 ] || { echo "COMPBUF CONFIG_ERROR already set: stop" >&2; exit 3; }
    [ "$(sz $d)" -eq "$(cur $d)" ] || { echo "DET$N size/current differ ($d): a change is in flight, stop" >&2; exit 3; }
    other=$(( all - $(sz $d) + $(sz $c) ))
    [ $(( n + other )) -le $CRB ] || { echo "DET$N $n + the other DETs + COMPBUF = $(( n + other )) > $CRB: free another DET first (a separate step)" >&2; exit 3; }
    [ -f "$SAVE" ] || echo "$d" > "$SAVE"   # keep the FIRST (GOP) value across repeated sets
    show; echo "--- $(date +%T) DET$N $(sz $d) -> $n ${blank:-live}"
    d0=$d; nv=$(printf 0x%X $(( (d & ~0x1F) | n )))
    if [ "$blank" = --blank ]; then
      b=$(( h0 & ~0x84000000 ))                     # never write UNDERFLOW_CLEAR (31) / TIMEOUT_STATUS_CLEAR (26)
      w $HUBP $(printf 0x%X $(( b | 1 ))); i=0
      while [ $i -lt 200 ] && [ $(( $(r $HUBP) & 0xA )) -ne $((0xA)) ]; do i=$((i + 1)); done   # IN_BLANK + NO_OUTSTANDING_REQ
      w $DET "$nv"; wait_cur "$n"; ok=$?
      w $HUBP $(printf 0x%X $(( b & ~1 )))
    elif [ "$blank" = --lock ]; then
      w $LOCK 0x1; w $DET "$nv"; w $LOCK 0x0; wait_cur "$n"; ok=$?
    else
      w $DET "$nv"; wait_cur "$n"; ok=$?
    fi
    sleep 1; show
    c2=$(r 0x397A); h2=$(r $HUBP)
    if [ $ok -ne 0 ] || [ $(( c2 >> 31 )) -ne 0 ] || [ $(( (h2 >> 11) & 1 )) -ne 0 ]; then
      echo "FAILED (current/config_error/seg_alloc_err): rolling back to $d0" >&2
      w $DET "$d0"; wait_cur "$(sz $d0)"; show; exit 4
    fi
    echo "OK: DET$N = $n segments ($(( n * 64 )) KB). Now: blink-video with stutter still forced off, then the eye check." ;;
  clear)
    o=$(r $OPTC); w $OPTC $(printf 0x%X $(( (o & ~0x400) | 0x1000 )))
    h=$(r $HUBP); w $HUBP $(printf 0x%X $(( (h & 0x0FFFFFFF) | 0x80000000 ))); w $HUBP $(printf 0x%X $(( h & 0x0FFFFFFF )))
    echo "$(date +%T) pipe $N underflow stickies cleared"; show ;;
  rollback)
    v=$(cat "$SAVE" 2>/dev/null || echo 0x303)
    w $DET "$v"; wait_cur "$(sz $v)"; show; rm -f "${SAVE:?}" ;;
  *) echo "usage: det.sh [-p 0..3] show | clear | set <segments> [--lock|--blank] | rollback" >&2; exit 2 ;;
esac
