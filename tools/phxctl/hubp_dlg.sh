#!/bin/sh
# SPDX-License-Identifier: MIT
# hubp_dlg.sh (reviewed; in use since 10-29: boot daemon b15_boot.sh step 4; origin: the 10-29 display
# self-refresh work).
# Programs the HUBP0 DLG/TTU "rate" registers (the ones the DLG uses to turn DET fill into time / request deadlines)
# with the Linux DML values for 1920x1080@144 (a Linux capture, 10-28: the DTN log = the register trace's HUBP1).
# macOS, on the box, sudo -n, any PhxBringup kext with phxctl wreg (selector 108, absolute dword, read-back).
#   hubp_dlg.sh show              the group, current vs Linux (read-only)
#   hubp_dlg.sh linux [--lock]    save current values once (~/gpu/hubp_dlg.saved), write the Linux values, read back
#   hubp_dlg.sh full  [--lock]    'linux' + the frame-position group + OTG0 global sync (VSTARTUP/VUPDATE/VREADY):
#                                 the complete Linux set; separate, later test (two coupled groups)
#   hubp_dlg.sh linux60 [--lock]  10-30 (M7 two displays): the same 8
#                                 registers with Linux's values for the AOC's 1080p60 pipe 0 next to the Acer at 144
#                                 (a Linux register capture, 10-30); refuses unless OTG0 is 1080p60
#   hubp_dlg.sh rollback [--lock] write back the saved values (GOP values)
# Precondition: stutter forced off (0x39BC = 0x1002). Then a frame with a mix of old/new deadlines only changes
# request priorities (memory never sleeps), so the default is a plain live write like Linux's dcn20_update_dchubp_dpp
# path minus the lock. --lock = inside OTG0_OTG_MASTER_UPDATE_LOCK (0x504A, wait UPDATE_LOCK_STATUS bit 8), as
# optc1_lock(); every phxctl call is a separate process, so the lock is held for ~N x (call time): phxfb gives a flip up
# after 3 frames (flipWaitLocked, "flipping off"), so check `sysctl debug.phxfb` flip timeouts after a --lock run.
# Registers (0x34C0 + dcn_3_2_0_offset.h, BASE_IDX 2). Never touches 0x39BC, watermarks, DET or surface registers.
set -u
P=$HOME/gpu/phxctl.bin; SAVE=$HOME/gpu/hubp_dlg.saved
# abs | name | Linux value
RATE='0x3B05|DST_DIMENSIONS (refcyc_per_htotal, 8 frac bits: 316.2 = 2056 x 50/325.08)|0x00013C3A
0x3B1C|REF_FREQ_TO_PIX_FREQ (19 frac bits: 0.15381 = 50/325.08)|0x00013AFF
0x3B19|PER_LINE_DELIVERY_PRE (refcyc_per_line_delivery_pre_l 295 = 5.9 us)|0x00000127
0x3B1A|PER_LINE_DELIVERY (refcyc_per_line_delivery_l 295)|0x00000127
0x3AEB|DCN_SURF0_TTU_CNTL0 (refcyc_per_req_delivery 9.84, QoS fixed 8)|0x0800275F
0x3AEC|DCN_SURF0_TTU_CNTL1 (refcyc_per_req_delivery_pre)|0x0000275F
0x3AE9|DCN_TTU_QOS_WM (qos_high 0x4F0 = 25.3 us)|0x04F00000
0x3AEA|DCN_GLOBAL_TTU_CNTL (min_ttu_vblank 0x7A5 = 39.1 us, QoS flip 0xE)|0xE00007A5'
RATE60='0x3B05|DST_DIMENSIONS (2200 x 50/148.5)|0x0002E4BD
0x3B1C|REF_FREQ_TO_PIX_FREQ (50/148.5)|0x0002B18F
0x3B19|PER_LINE_DELIVERY_PRE|0x0000013F
0x3B1A|PER_LINE_DELIVERY|0x00000286
0x3AEB|DCN_SURF0_TTU_CNTL0|0x08005631
0x3AEC|DCN_SURF0_TTU_CNTL1|0x00002A88
0x3AE9|DCN_TTU_QOS_WM|0x0B920000
0x3AEA|DCN_GLOBAL_TTU_CNTL|0xE0005C6C'
FULL='0x3AE8|DCN_EXPANSION_MODE (drq 2)|0x00000056
0x3AB1|DCHUBP_REQ_SIZE_CONFIG|0x033D1B40
0x3B04|BLANK_OFFSET_1 (min_dst_y_next_start)|0x00001120
0x3B06|DST_AFTER_SCALER|0x00000021
0x3B07|PREFETCH_SETTINGS (dst_y_prefetch 0x29)|0x29080000
0x3B09|VBLANK_PARAMETERS_0|0x00000201
0x3B0A|VBLANK_PARAMETERS_1|0x00000000
0x3B0C|VBLANK_PARAMETERS_3|0x00000000
0x3B12|NOM_PARAMETERS_1|0x00000000
0x3B15|NOM_PARAMETERS_4|0x00000080
0x3B16|NOM_PARAMETERS_5|0x00000000
0x5046|OTG0_OTG_VSTARTUP_PARAM (17 lines)|0x00000011
0x5047|OTG0_OTG_VUPDATE_PARAM|0x00C60202
0x5048|OTG0_OTG_VREADY_PARAM|0x00000103'
r() { sudo -n "$P" regs "$1" | awk '{print $NF}'; }
w() { o=$(sudo -n "$P" wreg "$1" "$2") || { echo "wreg $1 failed" >&2; return 1; }; echo "  $o"; }
lock() { [ "${LOCK:-}" = --lock ] || return 0; g=$(r 0x504F); [ $(( g & 0x7 )) -eq 0 ] || w 0x504F $(printf 0x%X $(( g & ~0x7 )))
         w 0x504A 0x1; i=0; while [ $i -lt 100 ] && [ $(( $(r 0x504A) & 0x100 )) -eq 0 ]; do i=$((i + 1)); done
         [ $i -lt 100 ] || { w 0x504A 0x0; echo "OTG0 UPDATE_LOCK_STATUS never 1: unlocked, stop" >&2; exit 4; }; }
unlock() { [ "${LOCK:-}" = --lock ] && w 0x504A 0x0; return 0; }
show() { echo "$1" | while IFS='|' read a n v; do c=$(r "$a"); s=SAME; [ "$(printf %d "$c")" -eq "$(printf %d "$v")" ] || s=DIFF
         printf '%s %-80s now %s linux %s %s\n' "$a" "$n" "$c" "$v" "$s"; done; }
pre() { [ "$(r 0x39BC)" = 0x00001002 ] || { echo "0x39BC is not 0x1002 (stutter allowed): refuse (two variables)" >&2; exit 3; }
        h=$(r 0x3AB3); [ $(( h & 1 )) -eq 0 ] || { echo "HUBP0 blanked: stop" >&2; exit 3; }; }
apply() { trap 'unlock' EXIT INT TERM; lock; echo "$1" | while IFS='|' read a n v; do w "$a" "$v"; done; unlock; trap - EXIT INT TERM; }
save() { [ -f "$SAVE" ] && return 0; printf '%s\n%s\n' "$RATE" "$FULL" | while IFS='|' read a n v; do echo "$a|$n|$(r "$a")"; done > "$SAVE"; echo "saved $SAVE"; }
LOCK=${2:-}
case "${1:-}" in
  show) show "$RATE"; echo "-- frame-position group (full):"; show "$FULL" ;;
  linux) pre; save; echo "--- $(date +%T) HUBP0 rate group -> Linux ${LOCK:-live}"; apply "$RATE"; show "$RATE" ;;
  linux60) pre; [ "$(r 0x4FEA)" = 0x00000897 ] || { echo "OTG0 H_TOTAL $(r 0x4FEA) is not 1080p60 (0x897): refuse" >&2; exit 3; }
     save; echo "--- $(date +%T) HUBP0 rate group -> Linux 60 Hz (two displays) ${LOCK:-live}"; apply "$RATE60"; show "$RATE60" ;;
  full) pre; save; echo "--- $(date +%T) HUBP0 rate + frame group + OTG0 global sync -> Linux ${LOCK:-live}"; apply "$RATE"; apply "$FULL"; show "$RATE"; show "$FULL" ;;
  rollback) [ -f "$SAVE" ] || { echo "no $SAVE (nothing saved to roll back to)" >&2; exit 2; }
     echo "--- $(date +%T) rollback ${LOCK:-live}"; apply "$(cat "$SAVE")"; show "$(cat "$SAVE")"; rm -f "${SAVE:?}" ;;
  *) echo "usage: hubp_dlg.sh show | linux [--lock] | linux60 [--lock] | full [--lock] | rollback [--lock]" >&2; exit 2 ;;
esac
