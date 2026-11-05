#!/bin/sh
# SPDX-License-Identifier: MIT
# wm144.sh (reviewed; in use since 10-29: boot daemon b15_boot.sh step 1; origin: step A1 of
# the 10-29 idle-power plan).
# wm144.sh linux | linux2 | linux144x2 | linux144a60 | linux60a165 | linux144a165 | gop | show  (on the box, macOS, sudo -n, any kext with PHXN_SEL_DBG_WRITE_REG = 108):
# linux2 (10-30): Linux's two-display sets (AOC 60 + Acer 144, a Linux register capture), for
# DET 9/9.
# linux144x2 (10-31, AOC at 144): Linux's sets with BOTH displays at 1080p144 (capture u3:
# its registers + DTN log WM_Set A/D dram_clk_change 413.18 us, C 63.18 us),
# DET 9/9. Against linux2 only SR enter/exit (0xad4/0x978 -> 0xad0/0x973; B 0xa79/0x91c -> 0xa74/0x918) and
# FRAC_URG_BW_NOM (0x59 -> 0x77; B 0x39 -> 0x4d) differ; urgency, refcyc, UCLK and FCLK p-state are the same.
# linux144a60 (10-31, b103, AOC at 144 + Acer at 60): Linux's sets of capture u4 (its
# registers, DTN log WM_Set A/D dram_clk_change 413.18 us, C 63.18 us), DET 9/9. = linux2 except
# FRAC_URG_BW_NOM A/C/D 0x59 -> 0x5a, B 0x39 -> 0x3a (36/36 against u4).
# linux60a165 (11-01, SCDC2, AOC 60 + Acer 1080p165): Linux's sets of capture c2 (its
# registers, steady 120 s; = the last writes of the c1 trace 36/36; dtn.txt WM_Set A/D dram_clk_change 413.18 us, B 409.36,
# C 63.18 = c0), DET 9/9. = linux2 except SR enter/exit A/C/D 0xad4/0x978 -> 0xad3/0x976, B 0xa79/0x91c -> 0xa77/0x91b and
# FRAC_URG_BW_NOM A/C/D 0x59 -> 0x62, B 0x39 -> 0x3f (36/36 against c2).
# linux144a165 (11-01, SCDC2, AOC 144 + Acer 1080p165): the last writes of the c3 trace (c3 has no steady read; the method
# gives c2 from c1 and c4 from c4, 36/36 each; every one of the 36 is written in c3, last at 201.3747 of a trace to 210.10),
# DET 9/9. = linux144x2 except SR enter/exit A/C/D 0xad0/0x973 -> 0xacf/0x972, B 0x918 -> 0x917 and FRAC_URG_BW_NOM
# A/C/D 0x77 -> 0x85, B 0x4d -> 0x58 (36/36 against the c3 trace).
# programs the four DCHUBBUB watermark sets A-D with the values Linux amdgpu uses at our 1920x1080@144 mode
# (a Linux register capture, 10-28) or puts back the GOP values (a macOS read of the same registers, 10-28).
# Absolute dword = DMU BASE_IDX 2 (0x34C0, the card's IP discovery table) + dcn_3_2_0_offset.h value; every register here
# is BASE_IDX 2. Never touches DCHUBBUB_ARB_DRAM_STATE_CNTL (0x39BC, its own step A2) or WATERMARK_CHANGE_CNTL (0x39E2).
# Order: sets B, C, D (not selected while WATERMARK_CHANGE_SELECT = 0), then A, each set in the hubbub32 order
# (urgent, refcyc, frac urg bw, SR enter/exit, UCLK p-state, FCLK p-state, USR retraining).
P=$HOME/gpu/phxctl.bin
w() { out=$(sudo -n "$P" wreg "$1" "$2"); rb=$(echo "$out" | awk '{print $NF}'); [ "$((rb))" -eq "$(($2))" ] || { echo "MISMATCH $1 wrote $2 read $rb" >&2; exit 1; }; }
r() { sudo -n "$P" regs "$1" | awk '{print $2}'; }
# per set: base urg usr refcyc srent srex uclk fclk nom flip   (base = absolute address of DATA_URGENCY_WATERMARK_x)
set_wm() { b=$1; w $(printf 0x%X $b) $2; w $(printf 0x%X $((b + 2))) $4; w $(printf 0x%X $((b + 7))) $9; w $(printf 0x%X $((b + 8))) ${10}
  w $(printf 0x%X $((b + 3))) $5; w $(printf 0x%X $((b + 4))) $6; w $(printf 0x%X $((b + 5))) $7; w $(printf 0x%X $((b + 6))) $8
  w $(printf 0x%X $((b + 1))) $3; }
case "${1:-}" in
  show) a=$((0x39BC)); while [ $a -le $((0x39E5)) ]; do printf '0x%X DCHUBBUB+0x%x %s\n' $a $((a - 0x34C0)) "$(r $(printf 0x%X $a))"; a=$((a + 1)); done ;;
  linux)
    t=$(r 0x39E5); [ "$((t))" -eq $((0x1002)) ] || { echo "DCHUBBUB_GLOBAL_TIMER_CNTL $t != Linux 0x1002: refclk differs, values would need scaling; stop" >&2; exit 3; }
    #        base   urg   usr   refcyc srent srex  uclk   fclk  nom  flip
    set_wm 0x39C7 0x1c8 0x290 0x12c  0xa73 0x916 0x4fe8 0x6aa 0x26 0x0   # B
    set_wm 0x39D0 0x281 0x349 0x18f  0xac8 0x96b 0xc45  0xc45 0x3b 0x0   # C (dummy p-state set)
    set_wm 0x39D9 0x281 0x349 0x18f  0xac8 0x96b 0x50a1 0x763 0x3b 0x0   # D (MALL set)
    set_wm 0x39BE 0x281 0x349 0x18f  0xac8 0x96b 0x50a1 0x763 0x3b 0x0   # A last
    echo "watermarks = Linux 144 Hz" ;;
  linux2)
    t=$(r 0x39E5); [ "$((t))" -eq $((0x1002)) ] || { echo "DCHUBBUB_GLOBAL_TIMER_CNTL $t != Linux 0x1002: stop" >&2; exit 3; }
    #        base   urg   usr   refcyc srent srex  uclk   fclk  nom  flip
    set_wm 0x39C7 0x1d4 0x29c 0x12c  0xa79 0x91c 0x4ff4 0x6b6 0x39 0x0   # B
    set_wm 0x39D0 0x293 0x35b 0x18f  0xad4 0x978 0xc57  0xc57 0x59 0x0   # C (dummy p-state set)
    set_wm 0x39D9 0x293 0x35b 0x18f  0xad4 0x978 0x50b3 0x775 0x59 0x0   # D
    set_wm 0x39BE 0x293 0x35b 0x18f  0xad4 0x978 0x50b3 0x775 0x59 0x0   # A last
    echo "watermarks = Linux two displays (60 + 144)" ;;
  linux144x2)
    t=$(r 0x39E5); [ "$((t))" -eq $((0x1002)) ] || { echo "DCHUBBUB_GLOBAL_TIMER_CNTL $t != Linux 0x1002: stop" >&2; exit 3; }
    #        base   urg   usr   refcyc srent srex  uclk   fclk  nom  flip
    set_wm 0x39C7 0x1d4 0x29c 0x12c  0xa74 0x918 0x4ff4 0x6b6 0x4d 0x0   # B
    set_wm 0x39D0 0x293 0x35b 0x18f  0xad0 0x973 0xc57  0xc57 0x77 0x0   # C (dummy p-state set)
    set_wm 0x39D9 0x293 0x35b 0x18f  0xad0 0x973 0x50b3 0x775 0x77 0x0   # D
    set_wm 0x39BE 0x293 0x35b 0x18f  0xad0 0x973 0x50b3 0x775 0x77 0x0   # A last
    echo "watermarks = Linux two displays (144 + 144)" ;;
  linux144a60)
    t=$(r 0x39E5); [ "$((t))" -eq $((0x1002)) ] || { echo "DCHUBBUB_GLOBAL_TIMER_CNTL $t != Linux 0x1002: stop" >&2; exit 3; }
    #        base   urg   usr   refcyc srent srex  uclk   fclk  nom  flip
    set_wm 0x39C7 0x1d4 0x29c 0x12c  0xa79 0x91c 0x4ff4 0x6b6 0x3a 0x0   # B
    set_wm 0x39D0 0x293 0x35b 0x18f  0xad4 0x978 0xc57  0xc57 0x5a 0x0   # C (dummy p-state set)
    set_wm 0x39D9 0x293 0x35b 0x18f  0xad4 0x978 0x50b3 0x775 0x5a 0x0   # D
    set_wm 0x39BE 0x293 0x35b 0x18f  0xad4 0x978 0x50b3 0x775 0x5a 0x0   # A last
    echo "watermarks = Linux two displays (AOC 144 + Acer 60)" ;;
  linux60a165)
    t=$(r 0x39E5); [ "$((t))" -eq $((0x1002)) ] || { echo "DCHUBBUB_GLOBAL_TIMER_CNTL $t != Linux 0x1002: stop" >&2; exit 3; }
    #        base   urg   usr   refcyc srent srex  uclk   fclk  nom  flip
    set_wm 0x39C7 0x1d4 0x29c 0x12c  0xa77 0x91b 0x4ff4 0x6b6 0x3f 0x0   # B
    set_wm 0x39D0 0x293 0x35b 0x18f  0xad3 0x976 0xc57  0xc57 0x62 0x0   # C (dummy p-state set)
    set_wm 0x39D9 0x293 0x35b 0x18f  0xad3 0x976 0x50b3 0x775 0x62 0x0   # D
    set_wm 0x39BE 0x293 0x35b 0x18f  0xad3 0x976 0x50b3 0x775 0x62 0x0   # A last
    echo "watermarks = Linux two displays (AOC 60 + Acer 165)" ;;
  linux144a165)
    t=$(r 0x39E5); [ "$((t))" -eq $((0x1002)) ] || { echo "DCHUBBUB_GLOBAL_TIMER_CNTL $t != Linux 0x1002: stop" >&2; exit 3; }
    #        base   urg   usr   refcyc srent srex  uclk   fclk  nom  flip
    set_wm 0x39C7 0x1d4 0x29c 0x12c  0xa74 0x917 0x4ff4 0x6b6 0x58 0x0   # B
    set_wm 0x39D0 0x293 0x35b 0x18f  0xacf 0x972 0xc57  0xc57 0x85 0x0   # C (dummy p-state set)
    set_wm 0x39D9 0x293 0x35b 0x18f  0xacf 0x972 0x50b3 0x775 0x85 0x0   # D
    set_wm 0x39BE 0x293 0x35b 0x18f  0xacf 0x972 0x50b3 0x775 0x85 0x0   # A last
    echo "watermarks = Linux two displays (AOC 144 + Acer 165)" ;;
  gop)
    for b in 0x39BE 0x39C7 0x39D0 0x39D9; do set_wm $b 0x1fe 0x0 0x0 0x4ba 0x424 0x4ee8 0x0 0x0 0x0; done
    echo "watermarks = GOP" ;;
  *) echo "usage: wm144.sh linux | linux2 | linux144x2 | linux144a60 | linux60a165 | linux144a165 | gop | show" >&2; exit 2 ;;
esac
