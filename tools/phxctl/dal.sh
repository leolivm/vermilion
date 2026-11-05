#!/bin/sh
# SPDX-License-Identifier: MIT
# dal.sh <DALSMC name | 0xID> [arg] (on the box, macOS, sudo -n, kext b75+): one DAL -> PMFW mailbox message through the
# kext's SMU debug selector (msg 0x10000 | id). Names = Linux dalsmc.h. Clock args: (PPCLK << 16) | MHz with
# UCLK 2, FCLK 3, DISPCLK 8, DPPCLK 9, DCFCLK 11, DTBCLK 12. Example: dal.sh NumOfDisplays 1; dal.sh SetFclkSwitchAllow 1
P=$HOME/gpu/phxctl.bin
case "$1" in
  GetSmuVersion) id=2;; GetDriverIfVersion) id=3;; GetMsgHeaderVersion) id=4;; SetHardMinByFreq) id=9;; SetHardMaxByFreq) id=10;;
  GetDpmFreqByIndex) id=11;; GetDcModeMaxDpmFreq) id=12;; SetMinDeepSleepDcfclk) id=13;; NumOfDisplays) id=14;; BacoAudioD3PME) id=16;;
  SetFclkSwitchAllow) id=17;; SetAlwaysWaitDmcubResp) id=20;; ReturnHardMinStatus) id=21;;
  0x*) id=$(($1));; *) echo "unknown DAL message $1" >&2; exit 2;;
esac
sudo -n "$P" smu $((0x10000 | id)) "${2:-0}" | sed "s/smu  */dal $1 /"
