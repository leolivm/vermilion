#!/bin/sh
# SPDX-License-Identifier: MIT
# m7_phase.sh (10-30): put OTG1's vblank on top of OTG0's. Pipe 1 (the kext's M7 stream)
# runs the same CEA timing as the GOP's pipe 0 from the same reference clock, so its vblank keeps the phase it started
# with; 135 lines apart (vblank ~45 lines) the DCN never grants the DRAM p-state change the PMFW waits for, and every
# PMFW main-loop message stalls (PowerUpVcn, SetHardMinByFreq, TransferTable). Fix: shorten OTG1's frame by 5 lines for
# n frames (counted on OTG1's frame counter) until OTG1 trails OTG0 by at most TOL lines; then restore V_TOTAL.
# Writes only pipe-1 registers: OTG1_OTG_V_TOTAL, then (after a shift) the OPTC1 underflow sticky clear: the short frames
# set ODM1_OPTC_INPUT_GLOBAL_CONTROL bit 10, which the kext's plane check S2.3-85 refuses (boot 15:30 10-30).
# Different V_TOTALs = no fixed phase, nothing to do.
# Exit 0 aligned (or nothing to do), 1 not aligned after 3 tries / read failure (V_TOTAL always restored).
# Env for the mock (tests/mock/m7test.sh): PHXCTL (sudo phxctl.bin), TOL (lines, default 10).
set -u
PHXCTL=${PHXCTL:-sudo -n $(cd "$(dirname "$0")" && pwd)/phxctl.bin}; TOL=${TOL:-10}; STEP=5
OTG0_POS=0x500a; OTG1_POS=0x508a; OTG0_VT=0x4fef; OTG1_VT=0x506f; OTG1_FRAME=0x508c; OPTC1=0x4f9a
rd() { $PHXCTL regs "$@" 2>/dev/null | sed -n 's/^0x[0-9a-f]*: \(0x[0-9a-f]*\)$/\1/p'; }
wr() { $PHXCTL wreg "$1" "$2" >/dev/null 2>&1; }
vt0=$(rd $OTG0_VT); vt1=$(rd $OTG1_VT)
[ -n "$vt0" ] && [ -n "$vt1" ] || { echo "phase: register read failed"; exit 1; }
[ $((vt0)) -eq $((vt1)) ] || { echo "phase: V_TOTAL $vt0 vs $vt1: different timings, no fixed phase"; exit 0; }
lines=$(( vt1 + 1 ))
frames() {   # wait n OTG1 frames
  f0=$(( $(rd $OTG1_FRAME) & 0xffffff )); i=0
  while [ $i -lt 600 ]; do f=$(( $(rd $OTG1_FRAME) & 0xffffff )); [ $(( (f - f0 + 0x1000000) % 0x1000000 )) -ge "$1" ] && return 0; i=$((i + 1)); done; }
clear_ufl() {   # det.sh clear for pipe 1: the shift's own underflow mark, after two frames at the restored timing
  [ "$1" -gt 0 ] || return 0
  frames 2; o=$(rd $OPTC1); [ -n "$o" ] || return 0
  wr $OPTC1 $(( (o & ~0x400) | 0x1000 ))
  echo "phase: OPTC1 underflow sticky cleared (was $(( (o >> 10) & 1 )), now $(( ($(rd $OPTC1) >> 10) & 1 )))"; }
lag() { set -- $(rd $OTG0_POS $OTG1_POS); [ $# -eq 2 ] || { echo -1; return; }; echo $(( ( ($1 & 0x7fff) - ($2 & 0x7fff) + lines ) % lines )); }
t=0
while [ $t -lt 3 ]; do
  l=$(lag); [ "$l" -ge 0 ] || { echo "phase: position read failed"; exit 1; }
  if [ "$l" -le "$TOL" ] || [ "$l" -ge $(( lines - TOL )) ]; then
    echo "phase: aligned, OTG1 trails OTG0 by $l of $lines lines (tries $t)"; clear_ufl $t; exit 0; fi
  n=$(( (l + STEP / 2) / STEP ))
  wr $OTG1_VT $(( vt1 - STEP ))
  frames "$n"
  wr $OTG1_VT "$vt1"
  echo "phase: try $((t + 1)): lag $l lines -> $n short frames"
  t=$((t + 1))
done
wr $OTG1_VT "$vt1"
clear_ufl 1
echo "phase: NOT aligned after 3 tries (lag $(lag) of $lines lines)"
exit 1
