#!/bin/bash
# SPDX-License-Identifier: MIT
# vcn_up.sh: WP6 M1 "VCN 4 alive" on the RX 7900 XT, instance 0 only, driven from userspace through the kext's debug
# selectors (phxctl). The order and values are Linux 6.8 vcn_v4_0_start_dpg_mode as traced on this card
# (a register trace and the PSP command log of Linux amdgpu; the capture tool is not included);
# offsets and sources are in vcn_m1_gen.py.
#
#   bash vcn_up.sh            dry run (default): print every command and the value it expects, touch nothing
#   bash vcn_up.sh --check    pre-flight only (read-only: boot log, kext build, boot-args, files, PSP/VCN state)
#   bash vcn_up.sh --run      do it; stops at the first unexpected read-back
#
# Environment: PHXCTL (default ~/gpu/phxctl.bin), MESBOOT (default /usr/local/sbin/mesboot, doorbell mode only),
# FW (default ~/gpu/fw/navi31/vcn_4_0_0.bin), WPTR_MODE=mmio (default: UVD_RB_WPTR register, Linux's non-doorbell
# set_wptr) or db (VCN_RB1_DB_CTRL + doorbell 0x312, as Linux ran it), POWER_DOWN=1 (stop VCN0 and PowerDownVcn at the
# end, as Linux's idle work did), FROM=<step> (resume at a step after a stop; the steps before it must have passed in
# this boot).
#
# Needs kext b59 or later (PowerUpVcn in the SMU allow-list) and boot-arg phxdebug=1. Data files come from vcn_m1_gen.py
# (vcn/ next to this script). A PSP or VCN hang can take the GPU and the desktop with it: the only way back is a
# reboot. The caller warns the user and arms a reboot watchdog before --run; this script arms none.
set -u
MODE=dry
case "${1:-}" in
  --run) MODE=run ;;
  --check) MODE=check ;;
  ""|--dry) MODE=dry ;;
  *) echo "usage: vcn_up.sh [--dry|--check|--run]"; exit 2 ;;
esac
HERE=$(cd "$(dirname "$0")" && pwd)
DATA=$HERE/vcn
PHXCTL=${PHXCTL:-$HOME/gpu/phxctl.bin}
MESBOOT=${MESBOOT:-/usr/local/sbin/mesboot}
FW=${FW:-$HOME/gpu/fw/navi31/vcn_4_0_0.bin}
WPTR_MODE=${WPTR_MODE:-mmio}
FROM=${FROM:-1}
SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo -n"

FW_MD5=cfe244431aaaa120bee275c95af658f9     # linux-firmware amdgpu/vcn_4_0_0.bin (403,856 B; ucode 403,600 B at +256)
AREA_MD5=3bb711a8f3820176495d91b6a40751a2   # vcn0_area.bin from vcn_m1_gen.py
RAM_MD5=030438198bb43e60d047f84c43bca304    # vcn0_ram.bin (27 DPG SRAM pairs)

[ -f "$DATA/vcn0_layout.sh" ] || { echo "missing $DATA/vcn0_layout.sh (run vcn_m1_gen.py)"; exit 1; }
. "$DATA/vcn0_layout.sh"

hex(){ printf '0x%08x' "$(( $1 ))"; }
say(){ echo "-- $*"; }
die(){ echo "STOP: $*"; echo "VCN0 left as it is now. A hang (no PSP/SMU answer, reads stuck at 0xffffffff) needs a reboot."; exit 1; }
P(){ $SUDO "$PHXCTL" "$@"; }

# --- primitives: in dry mode they print and return the expected value, so the flow reads like the real run;
# reads (rd, vrd) also run in --check mode, writes only in --run
rd(){   # rd <reg> -> prints the value
  if [ $MODE = dry ]; then echo "[dry] $SUDO $PHXCTL regs $(hex $1)" >&2; echo "${2:-0x????????}"; return 0; fi
  P regs "$(hex $1)" | awk '{print $NF}'
}
expect_rd(){   # expect_rd <reg> <value> <what>
  local v; v=$(rd "$1" "$(hex $2)")
  [ $MODE != run ] && { say "expect $3 ($(hex $1)) == $(hex $2)"; return 0; }
  say "$3 ($(hex $1)) = $v"
  [ $(( v )) -eq $(( $2 )) ] || die "$3 is $v, expected $(hex $2)"
}
wr(){   # wr <reg> <value> <what> [expected readback, default = value]
  local want=${4:-$2}
  if [ $MODE != run ]; then echo "[dry] $SUDO $PHXCTL wreg $(hex $1) $(hex $2)   # $3, expect readback $(hex $want)"; return 0; fi
  local out rb; out=$(P wreg "$(hex $1)" "$(hex $2)")
  rb=$(echo "$out" | awk '{print $NF}')
  echo "$out" | grep -q "kr 0x00000000" || die "wreg $3: $out"
  say "$3: wrote $(hex $2), readback $rb"
  [ $(( rb )) -eq $(( want )) ] || die "$3 readback $rb, expected $(hex $want)"
}
vw(){   # vw <vram byte offset> <value> <what>
  if [ $MODE != run ]; then echo "[dry] $SUDO $PHXCTL vramw $(hex $1) $(hex $2)   # $3"; return 0; fi
  local out; out=$(P vramw "$(hex $1)" "$(hex $2)")
  echo "$out" | grep -q "kr 0x00000000" || die "vramw $3: $out"
  local v; v=$(P vram "$(hex $1)" 1 | awk 'NF>1{print $2}')
  [ $(( 0x$v )) -eq $(( $2 )) ] || die "$3: VRAM reads 0x$v after writing $(hex $2)"
  say "$3: VRAM $(hex $1) = 0x$v"
}
vrd(){   # vrd <vram byte offset> -> 0x value
  if [ $MODE = dry ]; then echo "[dry] $SUDO $PHXCTL vram $(hex $1) 1" >&2; echo "${2:-0x????????}"; return 0; fi
  echo "0x$(P vram "$(hex $1)" 1 | awk 'NF>1{print $2}')"
}
hdp(){   # HDP flush (Linux hdp_v6_0_flush_hdp; mesboot hdp_flush): CPU writes through BAR0 must reach VRAM first
  if [ $MODE != run ]; then echo "[dry] $SUDO $PHXCTL regs 0xe4d; $SUDO $PHXCTL wreg <remap>>2 0   # HDP flush"; return 0; fi
  local remap; remap=$(rd 0xE4D)
  [ $(( remap )) -ne 0 ] && [ $(( remap & 3 )) -eq 0 ] && [ "$remap" != 0xffffffff ] || die "HDP remap register reads $remap"
  P wreg "$(hex $(( remap >> 2 )))" 0 >/dev/null
}
smu(){   # smu <msg> <arg> <what>
  if [ $MODE != run ]; then echo "[dry] $SUDO $PHXCTL smu $1 $2   # $3, expect resp 0x01 (OK)"; return 0; fi
  local out; out=$(P smu "$1" "$2"); echo "   $out"
  echo "$out" | grep -q "(OK)" || die "SMU $3: $out"
}
loadfw(){   # loadfw <file> <file offset> <size> <fw_type> <fw_buf offset> <what>
  if [ $MODE != run ]; then echo "[dry] $SUDO $PHXCTL loadfw $1 $2 $3 $4 $(hex $5)   # $6, expect kr 0 status 0"; return 0; fi
  local out; out=$(P loadfw "$1" "$2" "$3" "$4" "$(hex $5)"); echo "   $out"
  echo "$out" | grep -q "kr 0x00000000  status 0x00000000" || die "LOAD_IP_FW $6: $out"
}
poll(){   # poll <reg> <value> <tries> <what>: read until equal (Linux SOC15_WAIT_ON_RREG / ring test loop)
  if [ $MODE != run ]; then echo "[dry] poll $SUDO $PHXCTL regs $(hex $1) until == $(hex $2) ($3 tries)   # $4"; return 0; fi
  local v i
  for i in $(seq 1 "$3"); do
    v=$(rd "$1"); [ $(( v )) -eq $(( $2 )) ] && { say "$4: $(hex $1) = $v after $i reads"; return 0; }
    sleep 0.01
  done
  die "$4: $(hex $1) = $v, expected $(hex $2)"
}
vpoll(){   # vpoll <vram offset> <value> <tries> <what>
  if [ $MODE != run ]; then echo "[dry] poll $SUDO $PHXCTL vram $(hex $1) 1 until == $(hex $2)   # $4"; return 0; fi
  local v i
  for i in $(seq 1 "$3"); do
    v=$(vrd "$1"); [ $(( v )) -eq $(( $2 )) ] && { say "$4: VRAM $(hex $1) = $v after $i reads"; return 0; }
    sleep 0.01
  done
  die "$4: VRAM $(hex $1) = $v, expected $(hex $2)"
}
set_wptr(){   # set_wptr <value> <what>
  if [ "$WPTR_MODE" = db ]; then
    if [ $MODE != run ]; then echo "[dry] $SUDO $MESBOOT db $(hex $DB_BYTE) $1   # $2 (doorbell 0x312, HDP flush first)"; return 0; fi
    hdp; $SUDO "$MESBOOT" db "$(hex $DB_BYTE)" "$1" | grep -q "kr=0x0" || die "doorbell $2"
    say "$2: doorbell $(hex $DB_BYTE) <- $1"
  else
    hdp; wr $R_RB_WPTR "$1" "$2 (UVD_RB_WPTR)"
  fi
}
step(){ STEP=$1; shift; echo; echo "== step $STEP: $*"; }
run_step(){ [ "$STEP" -ge "$FROM" ]; }

# --- pre-flight (read-only)
preflight(){
  echo "== pre-flight (read-only)"
  local x
  if [ $MODE = dry ]; then
    echo "[dry] the current boot's section of /var/log/phxboot.log must hold a 'done' line; ioreg phx-build must be >= 59; nvram boot-args must hold phxdebug=1"
    echo "[dry] md5 of $FW, $DATA/vcn0_area.bin, $DATA/vcn0_ram.bin; PSP C2PMSG_64 (0x16080) bit 31; VCN0 UVD_POWER_STATUS ($(hex $R_POWER_STATUS)) = 0xffffffff (powered off, untouched this boot)"
    return 0
  fi
  # the current boot's section (from its last "== " header) must hold a done line; lines after it (b15, m7) are fine (10-30)
  x=$(sed -n "$(grep -n '^== ' /var/log/phxboot.log | tail -1 | cut -d: -f1),\$p" /var/log/phxboot.log | grep '^done' | tail -1)
  [ -n "$x" ] && say "boot daemon: $x" || die "boot daemon has not logged done in this boot ($(tail -1 /var/log/phxboot.log)); never touch the GPU before it"
  x=$(ioreg -l | awk -F'= ' '/"phx-build"/{print $2; exit}'); [ "${x:-0}" -ge 59 ] 2>/dev/null || die "PhxBringup build is '$x', need 59 or later (PowerUpVcn allow-list)"
  say "kext build $x"
  nvram boot-args 2>/dev/null | grep -q "phxdebug=1" || die "boot-arg phxdebug=1 missing (debug selectors off)"
  [ -x "$PHXCTL" ] || die "no $PHXCTL"
  [ "$(md5 -q "$FW" 2>/dev/null)" = $FW_MD5 ] || die "$FW md5 is not $FW_MD5"
  [ "$(md5 -q "$DATA/vcn0_area.bin" 2>/dev/null)" = $AREA_MD5 ] || die "vcn0_area.bin md5 mismatch (regenerate and update AREA_MD5)"
  [ "$(md5 -q "$DATA/vcn0_ram.bin" 2>/dev/null)" = $RAM_MD5 ] || die "vcn0_ram.bin md5 mismatch"
  x=$(rd 0x16080); [ $(( x >> 31 )) -eq 1 ] || die "PSP C2PMSG_64 = $x (sOS ring not idle)"
  say "PSP C2PMSG_64 $x, KM ring wptr $(rd 0x16083)"
  x=$(rd 0x1629A); [ $(( x )) -eq 1 ] || die "SMU response register = $x (last message not OK)"
  x=$(rd $R_POWER_STATUS)
  if [ "$FROM" -le 1 ]; then
    [ "$x" = 0xffffffff ] || die "VCN0 UVD_POWER_STATUS = $x: VCN0 was already powered this boot; reboot for a clean run, or set FROM"
    say "VCN0 is powered off (UVD_POWER_STATUS reads 0xffffffff), as expected before PowerUpVcn"
  else
    say "FROM=$FROM: VCN0 UVD_POWER_STATUS = $x"
  fi
  say "GB_ADDR_CONFIG $(rd 0x263e) (vcn0_ram.bin has 0x545)"
  [ "$(rd 0x263e)" = 0x00000545 ] || die "GB_ADDR_CONFIG differs from the SRAM list"
}

echo "vcn_up.sh mode=$MODE wptr=$WPTR_MODE from=$FROM phxctl=$PHXCTL"
preflight
[ $MODE = check ] && { echo; echo "pre-flight passed (nothing written)"; exit 0; }
[ $MODE = run ] && echo "RUN: a hang needs a reboot; the caller has armed the watchdog"

step 1 "PSP LOAD_IP_FW VCN (GFX_FW_TYPE_VCN 13): vcn_4_0_0.bin ucode +256, 403600 B -> fw_buf+0x400000 (Linux +684.7 ms, VCN still off)"
run_step && loadfw "$FW" 256 403600 13 $FW_BUF_VCN "VCN ucode"

step 2 "SMU PowerUpVcn (0x2A) instance 0, argument 0 (Linux +1114.3 ms); UVD_POWER_STATUS must then read 1"
if run_step; then
  smu 0x2A 0 "PowerUpVcn inst 0"
  expect_rd $R_POWER_STATUS 0x1 "UVD_POWER_STATUS"
  say "UVD_PGFSM_STATUS $(rd $R_PGFSM_STATUS) (Linux 0x2a6aaaaa with JPEG still off), UVD_VERSION $(rd $R_VERSION)"
fi

step 3 "stage vcn0_area.bin ($(hex $AREA_SIZE) B: ring with the test commands, fence, IBs, messages, VCPU BO, fw_shared) at fw_buf+$(hex $AREA_BUF) = VRAM $(hex $AREA_VRAM)"
if run_step; then
  if [ $MODE = run ]; then
    out=$(P stage "$DATA/vcn0_area.bin" 0 $(( AREA_SIZE )) "$(hex $AREA_BUF)"); echo "   $out"
    echo "$out" | grep -q "^staged" || die "stage: $out"
  else
    echo "[dry] $SUDO $PHXCTL stage $DATA/vcn0_area.bin 0 $(( AREA_SIZE )) $(hex $AREA_BUF)"
  fi
  [ $MODE = run ] && { [ $(( $(vrd $FW_SHARED_VRAM) )) -eq $(( 0x804 )) ] || die "fw_shared present_flag_0 not 0x804"; }
  [ $MODE = run ] && { [ $(( $(vrd $(( FW_SHARED_VRAM + 16 ))) )) -eq 1 ] || die "fw_shared sq.is_enabled not 1"; }
  [ $MODE = run ] && { [ $(( $(vrd $FENCE_VRAM) )) -eq 0 ] || die "fence page not 0"; }
  [ $MODE = run ] && { [ $(( $(vrd $RING_VRAM) )) -eq 1 ] || die "ring[0] not VCN_ENC_CMD_END"; }
  say "expect fw_shared[0] 0x804, fw_shared[16] 0x1, fence 0, ring[0] 0x1"
  hdp
fi

step 4 "UVD_POWER_STATUS: anti-hang off (POWER_STATUS field = 1), then PG_MODE | PG_EN (trace: 0x1, then 0x105)"
if run_step; then
  ps=$(rd $R_POWER_STATUS 0x1)
  wr $R_POWER_STATUS $(( (ps & ~3) | 1 )) "UVD_POWER_STATUS (anti-hang)" 0x1
  ps=$(rd $R_POWER_STATUS 0x1)
  wr $R_POWER_STATUS $(( ps | 0x104 )) "UVD_POWER_STATUS (PG_MODE | PG_EN)" 0x105
fi

step 5 "PSP LOAD_IP_FW VCN0_RAM (type 49): the 27 DPG SRAM writes (VCPU clock, LMI, cache windows 2/NC0 -> our BO, MASTINT); the VCPU boots here"
if run_step; then
  loadfw "$DATA/vcn0_ram.bin" 0 216 49 $FW_BUF_RAM "VCN0_RAM"
  say "UVD_POWER_STATUS now $(rd $R_POWER_STATUS) (Linux read 0x105 later), UVD_STATUS $(rd $R_STATUS) (info only)"
fi

step 6 "unified ring: RB_BASE = MC $(printf 0x%x_%08x $RING_MC_HI $RING_MC_LO), RB_SIZE 0x400 dw, RB1 off, ring reset, RPTR = WPTR = 0, RB1 on"
if run_step; then
  wr $R_RB_BASE_LO $RING_MC_LO "UVD_RB_BASE_LO"
  wr $R_RB_BASE_HI $RING_MC_HI "UVD_RB_BASE_HI"
  wr $R_RB_SIZE $RB_SIZE_DW "UVD_RB_SIZE"
  expect_rd $R_RB_ENABLE 0x1ff "VCN_RB_ENABLE before (trace)"
  wr $R_RB_ENABLE 0x1fb "VCN_RB_ENABLE &= ~RB1_EN"
  vw $(( FW_SHARED_VRAM + 16 )) 0x101 "fw_shared sq: is_enabled 1, queue_mode |= FW_QUEUE_RING_RESET"
  hdp
  wr $R_RB_RPTR 0 "UVD_RB_RPTR"
  wr $R_RB_WPTR 0 "UVD_RB_WPTR"
  r=$(rd $R_RB_RPTR 0x0)
  wr $R_RB_WPTR "$r" "UVD_RB_WPTR = RPTR"
  wr $R_RB_ENABLE 0x1ff "VCN_RB_ENABLE |= RB1_EN"
  vw $(( FW_SHARED_VRAM + 16 )) 0x1 "fw_shared sq: queue_mode &= ~(RING_RESET | DPG_HOLD_OFF)"
  hdp
  if [ "$WPTR_MODE" = db ]; then
    wr $R_RB1_DB_CTRL $DB_CTRL_VAL "VCN_RB1_DB_CTRL (doorbell 0x312, EN)"
  else
    say "VCN_RB1_DB_CTRL $(rd $R_RB1_DB_CTRL) left as is (WPTR_MODE=mmio)"
  fi
fi

step 7 "ring test (amdgpu_vcn_enc_ring_test_ring): ring[0] = VCN_ENC_CMD_END + 63 NOPs, WPTR 0x40, RPTR must reach 0x40 (Linux: 0.9 ms)"
if run_step; then
  expect_rd $R_RB_RPTR 0x0 "UVD_RB_RPTR before"
  set_wptr 0x40 "ring test"
  poll $R_RB_RPTR 0x40 200 "ring test RPTR"
fi

step 8 "IB test (amdgpu_vcn_dec_sw_ring_test_ib): decode create-session IB (unified header, handle 1) + FENCE seq 1, then destroy + seq 2"
if run_step; then
  set_wptr 0x80 "create IB"
  poll $R_RB_RPTR 0x80 300 "create IB RPTR"
  vpoll $FENCE_VRAM 0x1 300 "fence seq 1"
  set_wptr 0xc0 "destroy IB"
  poll $R_RB_RPTR 0xc0 300 "destroy IB RPTR"
  vpoll $FENCE_VRAM 0x2 300 "fence seq 2"
  say "UVD_POWER_STATUS $(rd $R_POWER_STATUS), UVD_STATUS $(rd $R_STATUS) (info only)"
fi

if [ "${POWER_DOWN:-0}" = 1 ]; then
  step 9 "stop (vcn_v4_0_stop_dpg_mode + PowerDownVcn, trace +2238 ms): wait POWER_STATUS == 1 field, RPTR == WPTR, PG_MODE off, PowerDownVcn"
  if run_step; then
    w=$(rd $R_RB_WPTR 0xc0); poll $R_RB_RPTR "$w" 100 "RPTR == WPTR"
    ps=$(rd $R_POWER_STATUS 0x105)
    wr $R_POWER_STATUS $(( ps & ~4 )) "UVD_POWER_STATUS &= ~PG_MODE" 0x101
    smu 0x2B 0 "PowerDownVcn inst 0"
  fi
fi

echo
if [ $MODE = run ]; then
  echo "M1 PASS: VCN0 ring test and decode create/destroy IB test passed (fence 2 in VRAM $(hex $FENCE_VRAM))"
else
  echo "dry run only; nothing was sent. Run with --check, then --run"
fi
