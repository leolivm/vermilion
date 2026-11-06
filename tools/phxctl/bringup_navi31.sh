#!/bin/bash
# SPDX-License-Identifier: MIT
# RX 7900 XT (Navi 31, GC 11.0.0 / PSP 13.0.0 / SMU 13.0.0) GFX power-up, in the order the Linux 6.8 driver used on
# this card (from a log of the PSP commands Linux sent; the capture tool is not included):
# PSP ring, TOC, SMU firmware, TMR, every engine's firmware through LOAD_IP_FW, RLC autoload, wait for
# BOOTLOAD_COMPLETE, then the RS64 program-counter setup Linux wrote (gfx_v11_0_config_gfx_rs64).
# Differences from the Radeon 780M bring-up: TOC comes from inside psp_13_0_0_sos.bin; the SMU firmware is loaded by the
# driver; CP firmware is RS64 + per-pipe stacks; RLC_V is loaded; no EnableGfxImu SMU message (APU-only — on a dGPU
# AUTOLOAD_RLC starts IMU/RLC); the bootload status register is 0xee82 (0xee7e is the GC 11.0.1 variant).
# Offsets/sizes inside each firmware file follow Linux 6.8's ucode headers and match what Linux loaded, ucode by ucode.
# Firmware: linux-firmware amdgpu/ files (non-kicker: this card is 0x744c rev 0xcc) in $FW. Run as root or with sudo.
# A reboot is armed for 3 minutes in case the PSP hangs and cancelled at the end.
set -u
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
FW=${FW:-$HOME/gpu/fw/navi31}
PHXCTL=${PHXCTL:-/usr/local/sbin/phxctl}   # root-owned copy, so a password-less sudo rule for it does not grant arbitrary root
P(){ $SUDO "$PHXCTL" "$@"; }

# VRAM layout (PhxNativeABI.h, navi31): FB MC base 0x80_0000_0000; ring carve-out at 48 MiB; TMR at 96 MiB;
# fw_buf at 128 MiB. The TOC is staged in the last MiB of fw_buf so it never overlaps a LOAD_IP_FW slot.
RING_OFF=0x3000000
TOC_BUF=0xF00000            # fw_buf-relative
TOC_MC_LO=0x08F00000        # 0x80_0000_0000 + 128 MiB + 15 MiB
TMR_MC_LO=0x06000000        # 0x80_0000_0000 + 96 MiB
TMR_SIZE=0x1300000          # Linux SETUP_TMR size on this card
TMR_PA_LO=0x06000000        # system_phy_addr = VRAM offset (MMMC_VM_FB_OFFSET reads 0 on this dGPU)

for f in psp_13_0_0_sos.bin smu_13_0_0.bin sdma_6_0_0.bin gc_11_0_0_pfp.bin gc_11_0_0_me.bin gc_11_0_0_mec.bin \
         gc_11_0_0_mes_2.bin gc_11_0_0_mes1.bin gc_11_0_0_imu.bin gc_11_0_0_rlc.bin; do
  [ -f "$FW/$f" ] || { echo "missing $FW/$f"; exit 1; }
done

reg(){ P regs "$1" | awk '{print $NF}'; }

# PSP bootloader stage (Linux psp_v13_0_bootloader_load_component / _load_sos, as traced on this card): after a
# cold boot through the GOP the sOS is NOT running (C2PMSG_81 = 0) and the bootloader waits with C2PMSG_35 = 0x80000000.
# Each component is staged at fw_buf+0 (VRAM 128 MiB, 1 MiB aligned; MC 0x80_0800_0000 >> 20 = 0x80080), its address
# goes to C2PMSG_36 and the command to C2PMSG_35. Linux used a GTT buffer; VRAM is the driver's own debug option.
FWBUF_MC_1M=0x80080
ZERO=$FW/zero-1m.bin
[ -f "$ZERO" ] || head -c 1048576 /dev/zero > "$ZERO"
bl_wait(){
  local v
  for _ in $(seq 1 300); do v=$(reg 0x16063); [ $(( v >> 31 )) -eq 1 ] && return 0; sleep 0.01; done
  echo "PSP bootloader not ready (C2PMSG_35=$v)"; return 1
}
bl_load(){   # <name> <offset in sos.bin> <size> <bootloader cmd>
  bl_wait || return 1
  P stage "$ZERO" 0 1048576 0 >/dev/null
  P stage "$FW/psp_13_0_0_sos.bin" "$2" "$3" 0 >/dev/null
  P wreg 0x16064 $FWBUF_MC_1M >/dev/null
  P wreg 0x16063 "$4" >/dev/null
  echo "  $1 ($3 B) cmd $4 sent"
}

echo "== watchdog: reboot in 3 min unless cancelled =="
( $SUDO /sbin/shutdown -r +3 >/dev/null 2>&1 & )   # matches the navi31 sudoers rule exactly

L(){ P loadfw "$@" 2>&1 | grep loadfw | tee -a ~/gpu/fwaddr.log; }

# RESUME=after_tmr: continue on the ring left by an earlier STOP_AFTER=smu run (sOS up, SMU fw + TMR already
# loaded). Loading the SMU firmware a second time into a running SMU is something Linux never does.
if [ "${RESUME:-}" != after_tmr ]; then
  : > ~/gpu/fwaddr.log
  echo "== sOS alive? (C2PMSG_81 sign of life, C2PMSG_58 version) =="
  P regs 0x16091 0x1607a 0x16063
  if [ "$(reg 0x16091)" = "0x00000000" ]; then
    echo "== PSP bootloader: KDB, SPL, SYS_DRV, SOS (offsets of the psp_v2 firmware layout) =="
    bl_load KDB     259344   5888 0x00080000 || exit 1
    bl_load SPL     265232    672 0x10000000 || exit 1
    bl_load SYS_DRV    256 157584 0x00010000 || exit 1
    bl_load SOS     157840  99200 0x00020000 || exit 1
    for _ in $(seq 1 200); do [ "$(reg 0x16091)" != "0x00000000" ] && break; sleep 0.01; done
    P regs 0x16091 0x1607a 0x16080
    [ "$(reg 0x16091)" != "0x00000000" ] || { echo "sOS did not start; leaving the watchdog armed"; exit 1; }
    echo "sOS up (Linux saw version 0x00310035)"
  else
    P psp destroy >/dev/null 2>&1   # a ring may survive from an earlier run
  fi
  if [ "${STOP_AFTER:-}" = sos ]; then
    $SUDO /usr/bin/killall shutdown 2>/dev/null; echo "STOP_AFTER=sos: stopped with the sOS up, watchdog cancelled"; exit 0
  fi
  P psp create $RING_OFF
  echo "== LOAD_TOC (from psp_13_0_0_sos.bin) =="
  P stage "$FW/psp_13_0_0_sos.bin" 257040 2304 $TOC_BUF >/dev/null
  P raw 0x20 $TOC_MC_LO 0x80 0x900 0 0 0

  echo "== load firmware =="
  L "$FW/smu_13_0_0.bin" 256 261632 18 0x000000            # SMC (before SETUP_TMR, as Linux)
  echo "== SETUP_TMR =="
  P raw 0x5 $TMR_MC_LO 0x80 $TMR_SIZE 0x2 $TMR_PA_LO 0x0
  if [ "${STOP_AFTER:-}" = smu ]; then
    echo "== SMU check: TestMessage(0x1234) -> 0x1235, GetSmuVersion (Linux: 0x004e8200 = 78.130.0), GetDriverIfVersion (0x40) =="
    P smu 0x01 0x1234; P smu 0x02 0; P smu 0x03 0
    $SUDO /usr/bin/killall shutdown 2>/dev/null; echo "STOP_AFTER=smu: stopped after SMC + TMR, watchdog cancelled"; exit 0
  fi
else
  echo "== RESUME=after_tmr: reusing the existing ring; SMU check =="
  P psp status
  P smu 0x02 0
fi
L "$FW/sdma_6_0_0.bin" 256 17408 71 0x040000              # SDMA_UCODE_TH0
L "$FW/sdma_6_0_0.bin" 17664 16896 72 0x050000            # SDMA_UCODE_TH1
L "$FW/gc_11_0_0_pfp.bin" 256 91488 87 0x060000           # CP_RS64_PFP
L "$FW/gc_11_0_0_me.bin" 256 174992 88 0x080000           # CP_RS64_ME
L "$FW/gc_11_0_0_mec.bin" 256 267008 89 0x0b0000          # CP_RS64_MEC
L "$FW/gc_11_0_0_pfp.bin" 91744 139264 90 0x100000        # CP_RS64_PFP_P0_STACK
L "$FW/gc_11_0_0_pfp.bin" 91744 139264 91 0x130000        # CP_RS64_PFP_P1_STACK
L "$FW/gc_11_0_0_me.bin" 175248 139264 92 0x160000        # CP_RS64_ME_P0_STACK
L "$FW/gc_11_0_0_me.bin" 175248 139264 93 0x190000        # CP_RS64_ME_P1_STACK
L "$FW/gc_11_0_0_mec.bin" 267264 139264 94 0x1c0000       # CP_RS64_MEC_P0_STACK
L "$FW/gc_11_0_0_mec.bin" 267264 139264 95 0x1f0000       # CP_RS64_MEC_P1_STACK
L "$FW/gc_11_0_0_mec.bin" 267264 139264 96 0x220000       # CP_RS64_MEC_P2_STACK
L "$FW/gc_11_0_0_mec.bin" 267264 139264 97 0x250000       # CP_RS64_MEC_P3_STACK
L "$FW/gc_11_0_0_mes_2.bin" 256 126016 33 0x280000        # CP_MES
L "$FW/gc_11_0_0_mes_2.bin" 126272 131072 34 0x2a0000     # CP_MES_DATA
L "$FW/gc_11_0_0_mes1.bin" 256 86912 81 0x2c0000          # CP_MES1 (KIQ)
L "$FW/gc_11_0_0_mes1.bin" 87168 131072 82 0x2e0000       # CP_MES1_DATA
L "$FW/gc_11_0_0_imu.bin" 256 66048 68 0x300000           # IMU_I
L "$FW/gc_11_0_0_imu.bin" 66304 66048 69 0x320000         # IMU_D
L "$FW/gc_11_0_0_rlc.bin" 25344 2560 20 0x340000          # RLC_RESTORE_LIST_GPM_MEM
L "$FW/gc_11_0_0_rlc.bin" 27904 36640 21 0x350000         # RLC_RESTORE_LIST_SRM_MEM
L "$FW/gc_11_0_0_rlc.bin" 64544 66048 26 0x360000         # RLC_IRAM
L "$FW/gc_11_0_0_rlc.bin" 130592 33280 48 0x380000        # RLC_DRAM
L "$FW/gc_11_0_0_rlc.bin" 163872 8704 25 0x390000         # RLC_P
L "$FW/gc_11_0_0_rlc.bin" 172576 12800 7 0x3a0000         # RLC_V
L "$FW/gc_11_0_0_rlc.bin" 256 25088 8 0x3b0000            # RLC_G

echo "== AUTOLOAD_RLC =="
P autoload

# On a dGPU the SMU powers GFX up: Linux sent this right after AUTOLOAD_RLC (+745 ms), and the RLC autoload only
# completed afterwards. Tables go to our VRAM (+144 MiB driver table, +145 MiB tools table, MC 0x80_09xx_xxxx);
# the kext refuses DRAM addresses outside that window. TransferTableSmu2Dram(1) reads the combo PPTable from PMFW.
S(){ local out; out=$(P smu "$@"); echo "$out"; echo "$out" | grep -q "(OK)" || { echo "SMU message $1 failed; the watchdog reboots in a few minutes"; exit 1; }; }
echo "== SMU init (Linux order) =="
S 0x03 0              # GetDriverIfVersion
S 0x02 0              # GetSmuVersion
S 0x0E 0x80           # SetDriverDramAddrHigh
S 0x0F 0x09000000     # SetDriverDramAddrLow
S 0x10 0x80           # SetToolsDramAddrHigh
S 0x11 0x09100000     # SetToolsDramAddrLow
S 0x12 1              # TransferTableSmu2Dram(TABLE_COMBO_PPTABLE)
S 0x36 0              # RunDcBtc
S 0x06 0              # EnableAllSmuFeatures (Linux: ~135 ms)
S 0x0D 0; S 0x0C 0    # GetRunningSmuFeaturesHigh / Low
echo "== wait for CP_STAT == 0 and RLC_RLCS_BOOTLOAD_STATUS bit 31 (Linux saw 0xc000001f) =="
ok=0
for i in $(seq 1 50); do
  out=$(P regs 0x21a0 0xee82)
  cp=$(echo "$out" | awk '/0x021a0/{print $2}'); bl=$(echo "$out" | awk '/0x0ee82/{print $2}')
  if [ "$cp" = "0x00000000" ] && [ $(( bl >> 31 )) -eq 1 ]; then ok=1; break; fi
  sleep 0.1
done
echo "$out"
if [ $ok -ne 1 ]; then
  echo "autoload did not complete; the watchdog reboots in a few minutes (do not cancel it late: a killed"
  echo "shutdown leaves /etc/nologin behind and blocks every login until a manual restart)"; exit 1
fi

echo "== RS64 program-counter setup (Linux gfx_v11_0_config_gfx_rs64, 33 writes from the trace) =="
P wreg 0x0a900 0x00000000   # GRBM_GFX_CNTL pipe 0
P wreg 0x030a4 0x00000c00   # CP_PFP_PRGRM_CNTR_START
P wreg 0x030b9 0x0001c000   # CP_PFP_PRGRM_CNTR_START_HI
P wreg 0x0a900 0x00000001   # GRBM_GFX_CNTL pipe 1
P wreg 0x030a4 0x00000c00   # CP_PFP_PRGRM_CNTR_START
P wreg 0x030b9 0x0001c000   # CP_PFP_PRGRM_CNTR_START_HI
P wreg 0x0a900 0x00000000   # GRBM_GFX_CNTL
P wreg 0x0a803 0x153c0000   # CP_ME_CNTL
P wreg 0x0a803 0x15300000   # CP_ME_CNTL
P wreg 0x0a900 0x00000000   # GRBM_GFX_CNTL pipe 0
P wreg 0x030a5 0x00000c00   # CP_ME_PRGRM_CNTR_START
P wreg 0x030d9 0x0001c000   # CP_ME_PRGRM_CNTR_START_HI
P wreg 0x0a900 0x00000001   # GRBM_GFX_CNTL pipe 1
P wreg 0x030a5 0x00000c00   # CP_ME_PRGRM_CNTR_START
P wreg 0x030d9 0x0001c000   # CP_ME_PRGRM_CNTR_START_HI
P wreg 0x0a900 0x00000000   # GRBM_GFX_CNTL
P wreg 0x0a803 0x15300000   # CP_ME_CNTL
P wreg 0x0a803 0x15000000   # CP_ME_CNTL
P wreg 0x0a900 0x00000004   # GRBM_GFX_CNTL MEC pipe 0
P wreg 0x0c900 0x00000c00   # CP_MEC_RS64_PRGRM_CNTR_START
P wreg 0x0c938 0x0001c000   # CP_MEC_RS64_PRGRM_CNTR_START_HI
P wreg 0x0a900 0x00000005   # GRBM_GFX_CNTL MEC pipe 1
P wreg 0x0c900 0x00000c00   # CP_MEC_RS64_PRGRM_CNTR_START
P wreg 0x0c938 0x0001c000   # CP_MEC_RS64_PRGRM_CNTR_START_HI
P wreg 0x0a900 0x00000006   # GRBM_GFX_CNTL MEC pipe 2
P wreg 0x0c900 0x00000c00   # CP_MEC_RS64_PRGRM_CNTR_START
P wreg 0x0c938 0x0001c000   # CP_MEC_RS64_PRGRM_CNTR_START_HI
P wreg 0x0a900 0x00000007   # GRBM_GFX_CNTL MEC pipe 3
P wreg 0x0c900 0x00000c00   # CP_MEC_RS64_PRGRM_CNTR_START
P wreg 0x0c938 0x0001c000   # CP_MEC_RS64_PRGRM_CNTR_START_HI
P wreg 0x0a900 0x00000000   # GRBM_GFX_CNTL
P wreg 0x0c904 0x400f0000   # CP_MEC_RS64_CNTL
P wreg 0x0c904 0x40000000   # CP_MEC_RS64_CNTL

echo "== doorbells (Linux values; the GOP boot leaves them off) =="
# NBIO doorbell aperture, S2A doorbell routing (above the 1 MiB BAR5 window: indirect through
# BIF_BX0_PCIE_INDEX2/DATA2 = 0xe/0xf, as amdgpu_device_indirect_wreg), CP MEC/RB doorbell ranges.
IW(){ P wreg 0xe "$(printf 0x%x $(( $1 * 4 )))" >/dev/null; P wreg 0xf "$2" >/dev/null; }
P wreg 0xde0 0x1 >/dev/null                                   # RCC DOORBELL_APER_EN
IW 0x507a40 0x30000007; IW 0x507a41 0x00057801; IW 0x507a42 0x3051001d
IW 0x507a43 0x3000000d; IW 0x507a44 0x40118809; IW 0x507a45 0x70118c0f
P wreg 0x305c 0x0 >/dev/null; P wreg 0x305d 0x450 >/dev/null # CP_MEC_DOORBELL_RANGE_LOWER/UPPER
P wreg 0x305a 0x458 >/dev/null; P wreg 0x305b 0x7f8 >/dev/null # CP_RB_DOORBELL_RANGE_LOWER/UPPER
P regs 0xde0 0x305c 0x305d 0x305a 0x305b

echo "== unhalt MEC (4 pipes) and ME, as Linux's last CP writes before MES start =="
P wreg 0xc904 0x3c000000 >/dev/null   # CP_MEC_RS64_CNTL: pipes 0-3 active, MEC_HALT clear
P wreg 0xa803 0x01000000 >/dev/null   # CP_ME_CNTL
P regs 0xc904 0xa803

echo "== state: CP_STAT, GRBM_STATUS, CP_ME_CNTL, CP_MEC_RS64_CNTL, IMU fw (expect 0x0b1f4b00) =="
P regs 0x21a0 0x2004 0xa803 0xc904 0xe068
$SUDO /usr/bin/killall shutdown 2>/dev/null; echo "watchdog cancelled rc=$?"
