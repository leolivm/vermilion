#!/bin/sh
# SPDX-License-Identifier: MIT
# dalwm.sh (reviewed; in use since 10-29: boot daemon b15_boot.sh step 3; needs kext b76+;
# origin: step A5 of the 10-29 idle-power plan).
# dalwm.sh: hands the PMFW the DAL WATERMARKS table exactly as Linux dcn32_notify_wm_ranges() builds it:
# WatermarksExternal_t (dcn32_smu13_driver_if.h) = 4 rows {WmSetting, Flags, pad[2]} + Spare[16] + MmHubPadding[8]
# = 28 dwords, all zero except row i: WmSetting = i, Flags = wm_type (dcn32_build_wm_range_table_fpu):
#   row 0 set A WATERMARKS_CLOCK_RANGE (0), row 1 set B CLOCK_RANGE (0), row 2 set C DUMMY_PSTATE (1), row 3 set D MALL (2).
# Then SetDalDramAddrHigh/Low (0x05/0x06, kext b76 accepts only the PHX_VRAM_DALWM_OFF slot) and
# TransferTableDram2Smu (0x08) with TABLE_WATERMARKS (2), the Linux order (a Linux register trace, at 2331.9755 s).
P=$HOME/gpu/phxctl.bin
OFF=$((0x9000000 + 0xFF000))   # PHX_VRAM_DALWM_OFF (PhxNativeABI.h, kext b76+); VRAM byte offset
MC=$(ioreg -l | sed -n 's/.*"phx-fb-mc-base" = \([0-9]*\).*/\1/p' | head -1)
[ -n "$MC" ] || { echo "phx-fb-mc-base not found" >&2; exit 2; }
ADDR=$((MC + OFF))
i=0; while [ $i -lt 28 ]; do
  case $i in 0) v=0x00000000;; 1) v=0x00000001;; 2) v=0x00000102;; 3) v=0x00000203;; *) v=0;; esac
  sudo -n "$P" vramw $(printf 0x%X $((OFF + 4 * i))) $v >/dev/null || exit 1; i=$((i + 1)); done
sudo -n "$P" vram $(printf 0x%X $OFF) 4
D="$(dirname "$0")/dal.sh"; [ -x "$D" ] || D=$HOME/gpu/dal.sh
"$D" 0x5 $(printf 0x%X $((ADDR >> 32))) && "$D" 0x6 $(printf 0x%X $((ADDR & 0xFFFFFFFF))) && "$D" 0x8 2
