#!/bin/sh
# SPDX-License-Identifier: MIT
# uclk.sh levels | pin <MHz> | unpin | show  (on the box, kext >= b71; run with sudo or sudo -n inside): memory clock
# (PPCLK_UCLK = 2) DPM limits through the SMU (smu_v13_0_0_ppsmc.h). UCLK switches under GPU load underflow the
# display; Linux pins UCLK (hard min) when the display cannot take a switch. `pin` sets the hard minimum, `unpin` sets it
# back to 0 (PMFW default). `levels` prints the DPM table (GetDpmFreqByIndex; index 0xff = level count). PHXCTL overrides the
# phxctl path.
P=${PHXCTL:-$HOME/gpu/phxctl.bin}
smu() { sudo -n "$P" smu "$1" "$2"; }
case "${1:-}" in
  levels) smu 0x1f $(( (2 << 16) | 0xff )); i=0; while [ $i -lt 4 ]; do smu 0x1f $(( (2 << 16) | i )); i=$((i + 1)); done ;;
  pin) smu 0x1b $(( (2 << 16) | ${2:?MHz} )) ;;
  unpin) smu 0x1b $(( 2 << 16 )) ;;
  show) SMU_CLOCKS=1 sudo -n -E python3 "$HOME/gpu/smumetrics.py" "${2:-4}" 0.5 ;;
  *) echo "usage: uclk.sh levels | pin <MHz> | unpin | show [n]" >&2; exit 2 ;;
esac
