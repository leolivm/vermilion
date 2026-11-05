#!/bin/sh
# SPDX-License-Identifier: MIT
# gfx_cg.sh on|off|show: GFX clock gating as Linux amdgpu enables it on Navi31 after the RLC autoload
# (gfx_v11_0 update_gfx_clock_gating; register values from a register trace of Linux amdgpu on this card).
# Without it the PMFW counts GFX as always busy (100 % activity, ~250 W and 110 C hotspot at idle).
# "off" writes back the values our bring-up leaves (hardware reset values). Run with sudo on the box; GC must be idle.
set -eu
P=${PHXCTL:-$HOME/gpu/phxctl.bin}
r() { "$P" regs "$1" | awk '{print $2}'; }
w() { "$P" wreg "$1" "$2" >/dev/null; }
show() { "$P" regs 0xec48 0xec49 0x21c2 0x3049 0x306a 0x12bc 0x18bc 0xee6b | tr '\n' ' '; echo; }
safe_enter() { w 0xa980 3; i=0; while [ $(( $(r 0xa980) & 1 )) -ne 0 ]; do i=$((i + 1)); [ $i -lt 1000 ] || { echo "RLC safe mode not acknowledged"; exit 1; }; done; }
safe_exit() { w 0xa980 1; }
case ${1:?on|off|show} in
show) show ;;
on)
    safe_enter
    w 0xec48 0x000607e7          # RLC_CGTT_MGCG_OVERRIDE: drop the CGCG/CGLS overrides
    w 0xec49 0x0000363f          # RLC_CGCG_CGLS_CTRL: CGCG + CGLS on, Linux idle thresholds
    w 0x21c2 0x00900100          # CP_RB_WPTR_POLL_CNTL
    w 0x3049 $(( $(r 0x3049) | 0x003c0000 ))   # CP_INT_CNTL_RING0: GUI idle / context busy-empty interrupts
    w 0x12bc 0x00400002          # SDMA0_RLC_CGCG_CTRL
    w 0x18bc 0x00400002          # SDMA1_RLC_CGCG_CTRL
    w 0xec48 0x000600c1          # RLC_CGTT_MGCG_OVERRIDE: MGCG/MGLS on (Linux steps through 607c1, 605c1, 604c1)
    w 0x306a $(( $(r 0x306a) | 0x003c0000 ))   # CP_INT_CNTL
    safe_exit
    show ;;
off)
    safe_enter
    w 0xec48 0x000607ff; w 0xec49 0x0001003c; w 0x21c2 0x00400000
    w 0x3049 $(( $(r 0x3049) & ~0x003c0000 )); w 0x306a $(( $(r 0x306a) & ~0x003c0000 ))
    w 0x12bc 0x00400000; w 0x18bc 0x00400000
    safe_exit
    show ;;
esac
