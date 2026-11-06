#!/bin/sh
# SPDX-License-Identifier: MIT
# b15_boot.sh (boot daemon `b15_boot`, root, after `done`; flag ~/gpu/phxboot-b15): the idle-power display recipe validated
# live on 10-29 (-7 W idle, 2 h with 0 underflow), then display stutter (DRAM
# self-refresh) allowed under a watchdog that lives for the whole boot.
#   1 wm144.sh linux     DCHUBBUB watermark sets A-D = Linux 144 Hz values
#   2 det.sh set 9       DET0 3 -> 9 segments
#   3 dalwm.sh           DAL TABLE_WATERMARKS to the PMFW (SetDalDramAddrHigh/Low + TransferTableDram2Smu)
#   4 hubp_dlg.sh linux  HUBP0 DLG/TTU rate group = Linux DML values (the GOP ones are degenerate)
#   5 0x39BC DCHUBBUB_ARB_DRAM_STATE_CNTL 0x1002 -> 0x1000 (allow stutter)
#   6 only with ~/gpu/phxboot-b15-dpm: uclk.sh unpin + dal.sh SetFclkSwitchAllow 1 (memory/fabric clocks free to drop:
#     ~43 -> ~20 W idle)
# Any step failing stops before 5 (stutter stays forced off: the state every step was tested in). Watchdog: polls the OPTC0
# (0x4F8A bit 10) and HUBP0 (0x3AB3 bits 30:28) underflow stickies every $PHXB15_POLL s and at the first hit forces 0x1002
# for the rest of the boot, and with step 6 on re-pins UCLK 456 (the UCLK-switch underflow mitigation). Logs to stdout (the daemon's phxboot.log). Never touches anything else.
# Rollback: rm ~/gpu/phxboot-b15 (next boot = no recipe); live: `sudo ~/gpu/phxctl.bin wreg 0x39BC 0x1002` + kill this.
# Two displays (10-30), only with
# ~/gpu/phxboot-b15-2pipe, OTG0 + OTG1 lit, OTG0 1080p60, DET0 = DET1 = 9 (m7_boot.sh set them at 144): wm144.sh linux2,
# hubp_dlg.sh linux60, then with ~/gpu/phxboot-b15-dpm uclk unpin + FCLK switch allow (53 -> 23 W, 0 underflow), then with
# ~/gpu/phxboot-b15-2pipe-sr the stutter allowed (0x39BC 0x1000; -4.5 W more), and with
# ~/gpu/phxboot-b15-2pipe-dalwm dalwm.sh before the UCLK unpin, with ~/gpu/phxboot-b15-2pipe-p0pf p0prefetch.sh
# linux before the UCLK unpin. Watchdog:
# OPTC0/1 + HUBP0/1 stickies; first hit = stutter forced off (0x1002) and UCLK re-pinned 456 for the rest of the boot.
# AOC at 144 (10-31; kext b101 pipe-0 macOS path), only with
# ~/gpu/phxboot-m7-aocmodes or ~/gpu/phxboot-m7-aoc144: after the two-display recipe the pipe-0 engine is armed
# (debug.phxpipe0_enable=1) and the watchdog follows pipe 0's mode (AOC 144 -> `aoc144`, back at 60 -> `aoc60`); -aoc144
# also asks for 144 the System Settings way once a user is logged in; a boot that finds the AOC already at 144 takes the
# 144 + 144 branch. Live: `b15_boot.sh aoc144` / `b15_boot.sh aoc60` (the watermark sets after a pipe-0 switch by hand).
# b103: the AOC at 144 next to the Acer at 60 too. `aoc144` picks by OTG1's
# totals: the Acer at 144 = linux144x2 (u3), at 60 = linux144a60 (u4); the follower watches both OTGs (OTG0 / OTG1
# H_TOTAL) and skips a poll while either kext engine is busy or started meanwhile.
# SCDC2 (11-01; from a register capture of Linux amdgpu with the Acer at 165): the
# Acer at 1080p165 (OTG1 0x81f/0x455) next to either AOC mode. AOC 144 + Acer 165: `aoc144` = linux144a165 (c3), the HUBP0
# rate group checked against the kext's 1080p144a165 plan, HUBP1's reported against c3. AOC 60 + Acer 165: `aoc60` = UCLK
# pinned 456 + stutter forced off (an untested pair: the conservative state of the 144 branch / the underflow mitigation) +
# linux60a165 (c2), HUBP0's group reported against c2; the Acer leaving 165 next to the AOC at 60 = linux2, UCLK pinned and
# stutter off until the next boot. The follower also acts when the Acer enters or leaves 165 next to the AOC at 60.
# Boot rate (11-02): ~/gpu/phxboot-aoc-rate (144 | last) = the AOC at 144 from boot.
# `aocboot` (run by the boot daemon after m7_bind, before its WindowServer restart) does the 60 recipe's pipe-0 part and the
# kext's macOS sequence to 144, then aoc144; the daemon then has PhxAccel take the mode as current (debug.phxfb_resync), so
# this script's run after `done` finds the AOC at 144 (the 144 branch, engine armed, no request after the login). The
# request after the login stays as the fallback when that boot rate is 144 but the AOC is at 60 here. `aocback` = the
# engine back to 60 + aoc60 (the daemon's undo). With "last" in phxboot-aoc-rate or phxboot-m7-rate the watchdog writes the
# pair of rates held for PHXB15_RATESTABLE s to ~/gpu/phxboot-rate-last for the next boot ("aoc=144 acer=165 at <time>").
# p0solo (11-03): the AOC ALONE (OTG1-3 dark: no Acer, or no
# ~/gpu/phxboot-m7) at 144. Needs PhxBringup's debug.phxpipe0_solo (kernel-p0solo.patch: with OTG1 dark the kext accepts
# its 1080p144 plan = u3, whose HUBP0 group is Linux's for this AOC mode at DET 9). `aocboot1` (the daemon, before its
# restart) = DET0 9, hubp_dlg linux60 + the stickies cleared + p0prefetch linux PHXP0_SOLO=1 (the 60 round trip needs every
# 60-plan register equal), the engine armed with solo, the macOS sequence to 144, then `aoc144solo` (UCLK pinned 456, the
# HUBP0 group checked against u3, watermarks wm144.sh linux = Linux's one-display 144 set, stutter left forced off). After
# `done` the one-display AOC branch keeps that state (no stutter allowed: the conservative two-display-144 state), arms the
# engine with solo for System Settings picks (the AOC alone at 60 gets the same pipe-0 prep and, with boot rate 144, the
# after-login request) and runs `watch1`: OPTC0/HUBP0 underflow -> stutter forced off + UCLK 456 for the boot; OTG0 changes
# -> aoc144solo / aoc60solo. The Acer alone on pipe 0 (OTG0 0x807/0x449) keeps the one-display recipe as before.
# Acer relight (11-03): an Acer HDMI replug clears the sink's SCDC
# while pipe 1 keeps sending 165 Hz TMDS that needs scrambling -> a dark panel. watch2 reads debug.phxhotplug_stat (every
# PHXB15_RLHPN polls while idle, every poll during a relight; its now_ms is the relight's clock): the online counter or the
# head-1 plugs counter going up (re)arms a check PHXB15_RLDELAY ms later. The check (and every later step) waits while either
# kext engine is busy or a head-1 hot-plug action is pending; pipe 1 not at 165 (switch_stat mode + OTG1 totals) = nothing,
# no SCDC read. Else one debug.phxscdc read (root, DDC): scrambled + locked = nothing; a dark SCDC = FS-2's sequence (m7_boot.sh
# fs2_165): debug.phxpipe2_switch=144, SCDC locked on 2 reads 1 s apart within 10 s (else the Acer stays at 144), switch to
# 165, scrambled + locked within 3 s (reads 0.5 s apart) else back to 144. A pipe-1 mode the relight did not set (macOS /
# System Settings) stops it. The watermark / UCLK sets follow each switch through follow0, as for any 144 <-> 165 change.
# One log line per decision / action. The three switches (dark 165 -> 144, locked 144 -> 165, the fallback 165 -> 144) go
# through macOS (11-04: after a kernel-path 144 -> 165 every layer read 165 but the Acer
# looked 60 until a cgmode 144 -> 165 re-set): rl_sw = cgmode set in the console user's session as aoc_request does, the
# Acer found by model (PHXB15_ACERMODEL, default 0x74d) at its current size; debug.phxpipe2_switch only without a console
# user (login window), without ~/gpu/cgmode, the Acer not in cgmode list, or a cgmode set that failed or left pipe 1 off
# the asked rate. One line per switch says the path and why; after a macOS switch (cgmode blocks ~4 s) the stat is read
# again, so the 10 s lock and the 3 s verify count from the switch's end. Settle re-set (11-04:
# the macOS-path relight ~10 s after a plug still left the Acer looking 60; a macOS 144 -> 165
# re-set from the settled lit state made it smooth, 2/2): after "relight: done" of a relight that used the macOS path,
# PHXB15_RLRESET s later (stat clock, default 20, 0 = off) ONE re-set through cgmode only (never the kernel): 144, one
# SCDC lock read (1 s apart, up to 10 s, else 165 anyway), 165, scrambled + locked within 3 s (else logged, no retry).
# Skipped when pipe 1 is not at 165 then (the user's mode is left as it is); a hot-plug event before it restarts the relight.
# Reads (11-03, found while chasing a video stall: one process per read was ~6 phxctl.bin + 6 sysctl + 12 awk per
# poll, ~16 phxctl.bin execs/s on the box, each a kernel 'map_with_linking_np ... not code signed' line): the watchdogs read
# through ONE `phxctl.bin serve` for the whole watch (srv_start), the registers and the two engine-state sysctls of a poll
# in one round trip, the same reads in the same order, parsed in the shell (bgof = sw/sw0's busy= gen= words). An older
# phxctl.bin without serve, no FIFO, PHXB15_SERVE=0 or the server gone mid-watch (logged) = one process per read as before.
# The rare steps (a mode change, a relight step, the rate memory, every RLHPN-th hot-plug stat) keep their own processes.
set -u
G=${PHXBOOT_G:-$HOME/gpu}; P=${PHXB15_PHXCTL:-$G/phxctl.bin}; POLL=${PHXB15_POLL:-0.2}; MAXPOLL=${PHXB15_MAXPOLL:-0}
r() { "$P" regs "$1" | awk '{print $NF}'; }
w() { "$P" wreg "$1" "$2" > /dev/null; }
# The read server (see the header): requests on fd 4 (opened read-write, so a dead server never SIGPIPEs this shell),
# one reply line per item on fd 3. srv <item>... sends one request (1 = no server); sget <var> reads the next reply line
# (1 = the server is gone: logged once, then every read is its own process again).
SRV=0; SRVPID=""; SWN=debug.phxpipe2_switch_stat; SW0N=debug.phxpipe0_stat
srv_start() {
  [ "${PHXB15_SERVE:-1}" = 1 ] || { echo "b15: watchdog reads: one process per read (PHXB15_SERVE=0)"; return 0; }
  sd=$(mktemp -d "${TMPDIR:-/tmp}/b15srv.XXXXXX" 2> /dev/null) || sd=""
  [ -n "$sd" ] && mkfifo "$sd/q" "$sd/a" 2> /dev/null ||
    { echo "b15: watchdog reads: no FIFO (${sd:-mktemp failed}): one process per read"; [ -n "$sd" ] && rm -rf "$sd"; return 0; }
  "$P" serve < "$sd/q" > "$sd/a" &
  SRVPID=$!; exec 4<> "$sd/q" 3< "$sd/a"; rm -rf "$sd"; SRV=1
  sx=""; printf '0x39BC\n' >&4 && IFS= read -r sx <&3
  if case "$sx" in 0x????????) true ;; *) false ;; esac; then
    echo "b15: watchdog reads through one $P serve (pid $SRVPID), no process per poll"
  else srv_stop; echo "b15: watchdog reads: $P has no serve (an older phxctl.bin?): one process per read"; fi; }
srv_stop() { SRV=0; exec 3<&- 4>&-; [ -n "$SRVPID" ] && kill "$SRVPID" 2> /dev/null; return 0; }
srv() { [ $SRV = 1 ] || return 1; printf '%s\n' "$*" >&4 || { srv_stop; echo "b15: $(date '+%T') the read server is gone: one process per read from now on"; return 1; }; }
sget() { IFS= read -r "$1" <&3 && return 0
  [ $SRV = 1 ] && { srv_stop; echo "b15: $(date '+%T') the read server is gone: one process per read from now on"; }; return 1; }
# bgof <stat line> -> BG = its busy= / gen= words exactly as sw / sw0 print them ("busy=0 gen=7 ")
bgof() { BG=""; set -f; for bw in $1; do case $bw in busy=*|gen=*) BG="$BG$bw " ;; esac; done; set +f; }
# b91 mode switch (10-30): while the kext switches pipe 1's mode the switch's own
# undo/relight may set pipe-1 stickies; the two-pipe watchdog skips pipe 1 on a poll whose switch state says busy or
# changed generation (read before and after the registers). An older kext without the sysctl: pipe 1 always watched.
SW=${PHXB15_SWSTAT:-sysctl -n debug.phxpipe2_switch_stat}
sw() { $SW 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^(busy|gen)=/) printf "%s ", $i}'; }
# b99 pipe-0 engine (10-31): the same skip for pipe 0 while the kext re-times the
# AOC (debug.phxpipe0_stat busy= gen=); a kext without the sysctl prints nothing, so pipe 0 is always watched.
SW0=${PHXB15_SW0STAT:-sysctl -n debug.phxpipe0_stat}
sw0() { $SW0 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^(busy|gen)=/) printf "%s ", $i}'; }
step() { sn=$1; shift; out=$("$@" 2>&1); rc=$?   # sn, not n: n is the watchdog's poll counter
  echo "b15: $sn rc $rc $(echo "$out" | tail -1 | cut -c1-100)"; return $rc; }
# AOC at 144: the two-display set for OTG0 at 1080p144 next to the Acer at 144 (Linux capture u3):
# UCLK pinned 456 (kept pinned at 144 + 144 until its own test;
# Linux idles MCLK at 96 MHz there, u3 clk-samples), the HUBP0 rate group checked against the kext's 144 plan (= u3: the
# pipe-0 engine wrote it), watermarks wm144.sh linux144x2, HUBP1's rate group reported only (pipe 1 is the pipe-1 plan's;
# = u3 except MIN_TTU_VBLANK 0x7a5, Linux 0x79f), stutter left forced off (the kext's macOS path leaves 0x39BC so at 144).
# aoc60 = back at 60 + 144: wm144.sh linux2 (the kext's 60 plan already put the HUBP0 groups back); UCLK stays pinned and
# stutter off for the rest of the boot, the next boot restores the free-clock state.
SC=${PHXB15_SYSCTL:-sysctl}; CONSOLE=${PHXB15_CONSOLE:-stat -f %u /dev/console}; ASUSER=${PHXB15_ASUSER:-launchctl asuser}
AOCWAIT=${PHXB15_AOCWAIT:-1800}; AOCSETTLE=${PHXB15_AOCSETTLE:-20}
AOCRATEF=$G/phxboot-aoc-rate; RATELAST=${PHXB15_RATELAST:-$G/phxboot-rate-last}
RATESTABLE=${PHXB15_RATESTABLE:-30}; RMEMN=${PHXB15_RMEMN:-25}
flagval() { [ -r "$1" ] && tr -d ' \t\r\n' < "$1"; }
# the AOC's boot rate: 144, or "last" = the remembered aoc= (144 when none); anything else (or no file) = 60
aocrate() { av=$(flagval "$AOCRATEF")
  if [ "$av" = last ]; then av=$(sed -n 's/.*aoc=\([0-9][0-9]*\).*/\1/p' "$RATELAST" 2>/dev/null | head -1); case "$av" in 60|144) ;; *) av=144 ;; esac; fi
  [ "$av" = 144 ] && echo 144 || echo 60; }
remember=0; for f in "$AOCRATEF" "$G/phxboot-m7-rate"; do [ "$(flagval "$f")" = last ] && remember=1; done
AOC_HUBP0='0x3B05 0x001FFFFF 0x00013BA8
0x3B1C 0x001FFFFF 0x00013A6D
0x3B19 0x1FFF1FFF 0x00000126
0x3B1A 0x1FFF1FFF 0x00000126
0x3AEB 0x1F7FFFFF 0x0800274D
0x3AEC 0x007FFFFF 0x0000274D
0x3AE9 0x3FFF3FFF 0x04EE0000
0x3AEA 0xFBFFFFFF 0xE0000A14'
# b103: the kext's 1080p144a60 plan (the AOC at 144 next to the Acer at 60, u4): the same group but MIN_TTU_VBLANK
AOC_HUBP0_A60=$(echo "$AOC_HUBP0" | sed 's/^0x3AEA 0xFBFFFFFF 0xE0000A14$/0x3AEA 0xFBFFFFFF 0xE0000AFB/')
AOC_HUBP1='0x3BE1 0x001FFFFF 0x00013C3A
0x3BF8 0x001FFFFF 0x00013AFF
0x3BF5 0x1FFF1FFF 0x00000127
0x3BF6 0x1FFF1FFF 0x00000127
0x3BC7 0x1F7FFFFF 0x0800275F
0x3BC8 0x007FFFFF 0x0000275F
0x3BC5 0x3FFF3FFF 0x04F00000
0x3BC6 0xFBFFFFFF 0xE000079F'
# SCDC2: the kext's 1080p144a165 plan (the AOC at 144 next to the Acer at 165, c3): the 144 group but MIN_TTU_VBLANK
# (c3 0xE0000AE8); PER_LINE_DELIVERY(_PRE) keeps the plans' 1920 px rule 0x126 (c3 0x130 = modetest's 3840 px framebuffer
# for two connectors, as u4 for 1080p144a60)
AOC_HUBP0_A165=$(echo "$AOC_HUBP0" | sed 's/^0x3AEA 0xFBFFFFFF 0xE0000A14$/0x3AEA 0xFBFFFFFF 0xE0000AE8/')
# HUBP1 at 165 (the pipe-1 165 plan's; reported only): Linux c3 next to the AOC at 144
AOC_HUBP1_165='0x3BE1 0x001FFFFF 0x000110FE
0x3BF8 0x001FFFFF 0x00010CCB
0x3BF5 0x1FFF1FFF 0x00000104
0x3BF6 0x1FFF1FFF 0x00000104
0x3BC7 0x1F7FFFFF 0x08002199
0x3BC8 0x007FFFFF 0x00002199
0x3BC5 0x3FFF3FFF 0x04430000
0x3BC6 0xFBFFFFFF 0xE000125F'
# HUBP0 (the AOC at 60) next to the Acer at 165, Linux c2 (reported only): = hubp_dlg.sh linux60 except SURF0_TTU_CNTL1
# 0x2A88 -> 0x2A99 and MIN_TTU_VBLANK 0x5C6C -> 0x4A9E
AOC60_HUBP0_A165='0x3B05 0x001FFFFF 0x0002E4BD
0x3B1C 0x001FFFFF 0x0002B18F
0x3B19 0x1FFF1FFF 0x0000013F
0x3B1A 0x1FFF1FFF 0x00000286
0x3AEB 0x1F7FFFFF 0x08005631
0x3AEC 0x007FFFFF 0x00002A99
0x3AE9 0x3FFF3FFF 0x0B920000
0x3AEA 0xFBFFFFFF 0xE0004A9E'
# OPTC0/1 + HUBP0/1 underflow stickies cleared (OPTC_UNDERFLOW_CLEAR; HUBP_UNDERFLOW_CLEAR set then released)
clear_stickies() { for o in 0 1; do w $(printf 0x%X $((0x4F8A + 0x10 * o))) 0x1000; a=$(printf 0x%X $((0x3AB3 + 0xDC * o))); h=$(r $a)
    w $a $(printf 0x%X $(( (h & 0x0FFFFFFF) | 0x80000000 ))); w $a $(printf 0x%X $(( h & 0x0FFFFFFF ))); done; }
is165() { [ $(( $(r 0x5081) & 1 )) -eq 1 ] && [ $(( $(r 0x506A) )) -eq $((0x81F)) ] && [ $(( $(r 0x506F) )) -eq $((0x455)) ]; }
aocflag() { [ -e "$G/phxboot-m7-aocmodes" ] || [ -e "$G/phxboot-m7-aoc144" ]; }
grp() { lb=${3:-u3}; echo "$2" | { b=0; while read -r a m v; do c=$(r "$a"); [ $(( c & m )) -eq $(( v )) ] || { b=$((b + 1)); echo "b15: $1 $a = $c, Linux $lb $v (mask $m)"; }; done
  echo "b15: $1 rate group: $b of 8 differ from Linux $lb"; [ $b = 0 ]; }; }
aoc144() {
  ht=$(r 0x4FEA); vt=$(r 0x4FEF)
  [ $(( ht )) -eq $((0x807)) ] && [ $(( vt )) -eq $((0x44B)) ] || { echo "b15: aoc144: OTG0 $ht/$vt is not the AOC at 1080p144 (0x807/0x44b): nothing done"; return 1; }
  c1=$(r 0x5081); h1=$(r 0x506A); v1=$(r 0x506F)
  if [ $(( c1 & 1 )) -eq 1 ] && [ $(( h1 )) -eq $((0x807)) ] && [ $(( v1 )) -eq $((0x449)) ]; then a1=144
  elif [ $(( c1 & 1 )) -eq 1 ] && [ $(( h1 )) -eq $((0x897)) ] && [ $(( v1 )) -eq $((0x464)) ]; then a1=60
  elif [ $(( c1 & 1 )) -eq 1 ] && [ $(( h1 )) -eq $((0x81F)) ] && [ $(( v1 )) -eq $((0x455)) ]; then a1=165
  else echo "b15: aoc144: OTG1 ($c1 $h1/$v1) is not the Acer at 1080p144 or 1080p60 (or 1080p165): nothing done"; return 1; fi
  d0=$(r 0x397B); d1=$(r 0x397C)
  [ $(( d0 & 0x1f )) -eq 9 ] && [ $(( d1 & 0x1f )) -eq 9 ] || { echo "b15: aoc144: DET0/1 $d0/$d1 not 9/9: nothing done"; return 1; }
  sa=$(r 0x39BC); [ $(( sa & 3 )) -eq 2 ] || { echo "b15: aoc144: stutter allowed (0x39BC $sa) at 144: nothing done"; return 1; }
  case "$(sw0)" in *busy=1*) echo "b15: aoc144: the pipe-0 engine is busy: nothing done"; return 1 ;; esac
  step uclk_pin sh "$G/uclk.sh" pin 456 || return 1
  if [ $a1 = 60 ]; then   # b103: the kext's 1080p144a60 plan, the u4 sets
    grp HUBP0 "$AOC_HUBP0_A60" u4 || { echo "b15: aoc144: HUBP0's rate group is not the 1080p144a60 plan's: watermarks not changed"; return 1; }
    step wm144a60 sh "$G/wm144.sh" linux144a60 || return 1
    echo "b15: $(date '+%T') AOC 144 + Acer 60: UCLK pinned 456, watermarks linux144a60, stutter forced off ($(r 0x39BC))"; return 0; fi
  if [ $a1 = 165 ]; then   # SCDC2: the kext's 1080p144a165 plan, the c3 sets
    grp HUBP0 "$AOC_HUBP0_A165" c3 || { echo "b15: aoc144: HUBP0's rate group is not the 1080p144a165 plan's: watermarks not changed"; return 1; }
    step wm144a165 sh "$G/wm144.sh" linux144a165 || return 1
    grp HUBP1 "$AOC_HUBP1_165" c3 || true
    echo "b15: $(date '+%T') AOC 144 + Acer 165: UCLK pinned 456, watermarks linux144a165, stutter forced off ($(r 0x39BC))"; return 0; fi
  grp HUBP0 "$AOC_HUBP0" || { echo "b15: aoc144: HUBP0's rate group is not the 144 plan's: watermarks not changed"; return 1; }
  step wm144x2 sh "$G/wm144.sh" linux144x2 || return 1
  grp HUBP1 "$AOC_HUBP1" || true
  echo "b15: $(date '+%T') AOC 144 + Acer 144: UCLK pinned 456, watermarks linux144x2, stutter forced off ($(r 0x39BC))"; }
aoc60() {
  ht=$(r 0x4FEA); [ $(( ht )) -eq $((0x897)) ] || { echo "b15: aoc60: OTG0 H_TOTAL $ht is not the AOC at 1080p60 (0x897): nothing done"; return 1; }
  case "$(sw0)" in *busy=1*) echo "b15: aoc60: the pipe-0 engine is busy: nothing done"; return 1 ;; esac
  if is165; then   # SCDC2: the Acer at 165 next to the AOC at 60 (c2)
    d0=$(r 0x397B); d1=$(r 0x397C)
    [ $(( d0 & 0x1f )) -eq 9 ] && [ $(( d1 & 0x1f )) -eq 9 ] || { echo "b15: aoc60: Acer 165 but DET0/1 $d0/$d1 not 9/9: nothing done"; return 1; }
    step uclk_pin sh "$G/uclk.sh" pin 456 || return 1
    sa=$(r 0x39BC); [ $(( sa & 3 )) -eq 2 ] || w 0x39BC $(printf 0x%X $(( (sa & ~3) | 2 )))
    step wm60a165 sh "$G/wm144.sh" linux60a165 || return 1
    grp HUBP0 "$AOC60_HUBP0_A165" c2 || true
    echo "b15: $(date '+%T') AOC 60 + Acer 165: UCLK pinned 456, watermarks linux60a165, stutter forced off ($(r 0x39BC))"; return 0; fi
  sa=$(r 0x39BC); [ $(( sa & 3 )) -eq 2 ] || w 0x39BC $(printf 0x%X $(( (sa & ~3) | 2 )))
  step wm144_2 sh "$G/wm144.sh" linux2 || return 1
  echo "b15: $(date '+%T') AOC back at 60 (Acer OTG1 H_TOTAL $(r 0x506A)): watermarks linux2, UCLK stays pinned and stutter off until the next boot"; }
# p0solo: OTG1-3 all dark (one display)
alone() { for o in 1 2 3; do c=$(r $(printf 0x%X $((0x4FEA + 0x80 * o + 0x17)))); [ $(( c & 1 )) -eq 0 ] || return 1; done; return 0; }
# the AOC alone at 144 (the kext's 1080p144 plan through debug.phxpipe0_solo): UCLK 456, HUBP0 = u3, Linux one-display watermarks
aoc144solo() {
  ht=$(r 0x4FEA); vt=$(r 0x4FEF)
  [ $(( ht )) -eq $((0x807)) ] && [ $(( vt )) -eq $((0x44B)) ] || { echo "b15: aoc144solo: OTG0 $ht/$vt is not the AOC at 1080p144 (0x807/0x44b): nothing done"; return 1; }
  alone || { echo "b15: aoc144solo: a second pipe is lit: nothing done"; return 1; }
  d0=$(r 0x397B); [ $(( d0 & 0x1f )) -eq 9 ] || { echo "b15: aoc144solo: DET0 $d0 not 9: nothing done"; return 1; }
  sa=$(r 0x39BC); [ $(( sa & 3 )) -eq 2 ] || { echo "b15: aoc144solo: stutter allowed (0x39BC $sa) at 144: nothing done"; return 1; }
  case "$(sw0)" in *busy=1*) echo "b15: aoc144solo: the pipe-0 engine is busy: nothing done"; return 1 ;; esac
  step uclk_pin sh "$G/uclk.sh" pin 456 || return 1
  grp HUBP0 "$AOC_HUBP0" || { echo "b15: aoc144solo: HUBP0's rate group is not the 1080p144 plan's: watermarks not changed"; return 1; }
  step wm144 sh "$G/wm144.sh" linux || return 1
  echo "b15: $(date '+%T') AOC 144 alone: UCLK pinned 456, watermarks linux (one display), stutter forced off ($(r 0x39BC))"; }
# the AOC alone back at 60: stutter stays forced off and UCLK pinned for the rest of the boot (the one-display 144 watermarks
# stay: the same latency budget is easier at 60 [I]); the kext's 60 plan put the HUBP0 groups back
aoc60solo() {
  ht=$(r 0x4FEA); [ $(( ht )) -eq $((0x897)) ] || { echo "b15: aoc60solo: OTG0 H_TOTAL $ht is not the AOC at 1080p60 (0x897): nothing done"; return 1; }
  sa=$(r 0x39BC); [ $(( sa & 3 )) -eq 2 ] || w 0x39BC $(printf 0x%X $(( (sa & ~3) | 2 )))
  echo "b15: $(date '+%T') AOC back at 60 alone: UCLK stays pinned and stutter off until the next boot"; }
# the pipe-0 prep the kext's 60 round trip needs with one display (aocboot1 and the one-display AOC-at-60 branch)
prep1() {
  [ -e "$G/p0prefetch.sh" ] || { echo "b15: $1: $G/p0prefetch.sh missing: nothing done"; return 1; }
  step det sh "$G/det.sh" set 9 || return 1
  step hubp_dlg sh "$G/hubp_dlg.sh" linux60 || return 1
  clear_stickies
  step p0prefetch env PHXP0_SOLO=1 sh "$G/p0prefetch.sh" linux || return 1; }
arm1() { $SC -w debug.phxpipe0_enable=1 > /dev/null 2>&1 && $SC -w debug.phxpipe0_solo=1 > /dev/null 2>&1; }
# Boot rate, the AOC alone (the daemon, before its WindowServer restart): see the p0solo header lines
aocboot1() {
  ht=$(r 0x4FEA); vt=$(r 0x4FEF)
  [ $(( ht )) -eq $((0x897)) ] && [ $(( vt )) -eq $((0x464)) ] || { echo "b15: aocboot1: OTG0 $ht/$vt is not the AOC at 1080p60: nothing done"; return 1; }
  alone || { echo "b15: aocboot1: a second pipe is lit: nothing done (aocboot is the two-display path)"; return 1; }
  sa=$(r 0x39BC); [ "$sa" = 0x00001002 ] || { echo "b15: aocboot1: 0x39BC = $sa, not the GOP 0x1002: nothing done"; return 1; }
  case "$(sw0)" in *busy=1*) echo "b15: aocboot1: the pipe-0 engine is busy: nothing done"; return 1 ;; esac
  $SC -n debug.phxpipe0_enable > /dev/null 2>&1 || { echo "b15: aocboot1: no debug.phxpipe0_enable (kext < b99): nothing done"; return 1; }
  $SC -n debug.phxpipe0_solo > /dev/null 2>&1 || { echo "b15: aocboot1: no debug.phxpipe0_solo (PhxBringup without p0solo): nothing done"; return 1; }
  prep1 aocboot1 || return 1
  arm1 && $SC -w debug.phxpipe0_mode=144 > /dev/null 2>&1 || { echo "b15: aocboot1: the pipe-0 engine refused enable / solo / mode 144: the AOC stays at 60"; return 1; }
  out=$($SC -w debug.phxpipe0=4 2>&1); rc=$?
  echo "b15: $(date '+%T') aocboot1: debug.phxpipe0=4 (the macOS sequence to 144, the AOC alone) rc $rc: $($SW0 2>/dev/null | tr '\n' ' ' | cut -c1-240)"
  ht=$(r 0x4FEA); vt=$(r 0x4FEF)
  [ $(( ht )) -eq $((0x807)) ] && [ $(( vt )) -eq $((0x44B)) ] || { echo "b15: aocboot1: OTG0 $ht/$vt after the set: the AOC stays at 60"; return 1; }
  aoc144solo || return 1
  echo "b15: $(date '+%T') aocboot1: the AOC (alone) at 144 before the WindowServer restart"; }
# The one-display watchdog: OPTC0/HUBP0 stickies (skipped while the pipe-0 engine is busy or started meanwhile) -> stutter
# forced off + UCLK 456 for the boot; OTG0 mode changes -> aoc144solo / aoc60solo
watch1() {
  srv_start; n=0; last1=$(r 0x4FEA)
  while :; do
    if srv @$SW0N 0x4F8A 0x3AB3 @$SW0N && sget p0 && sget v && sget h && sget p1; then bgof "$p0"; p0=$BG; bgof "$p1"; p1=$BG
    else p0=$(sw0); v=$(r 0x4F8A); h=$(r 0x3AB3); p1=$(sw0); fi; k=""
    case "$p0$p1" in *busy=1*) ;; *) if [ "$p0" = "$p1" ]; then [ $(( v & 0x400 )) -ne 0 ] && k=optc0; [ $(( (h >> 28) & 7 )) -ne 0 ] && k="${k:+$k+}hubp0"; fi ;; esac
    if [ -n "$k" ]; then w 0x39BC 0x1002; echo "b15: $(date '+%T') $k underflow: stutter forced off, UCLK pinned 456 for this boot"; step uclk_repin sh "$G/uclk.sh" pin 456; exit 2; fi
    if srv @$SW0N 0x4FEA && sget q && sget ht; then bgof "$q"; q=$BG; else q=$(sw0); case "$q" in *busy=1*) ;; *) ht=$(r 0x4FEA) ;; esac; fi
    case "$q" in *busy=1*) ;; *)
      if [ "$ht" != "$last1" ] && [ "$(sw0)" = "$q" ]; then last1=$ht
        if [ $(( ht )) -eq $((0x807)) ]; then aoc144solo; elif [ $(( ht )) -eq $((0x897)) ]; then aoc60solo; else echo "b15: pipe 0 at H_TOTAL $ht: no set for it"; fi; fi ;; esac
    n=$((n + 1)); [ "$MAXPOLL" -gt 0 ] && [ $n -ge "$MAXPOLL" ] && { echo "b15: watchdog stopped after $n polls (test)"; exit 0; }
    sleep "$POLL"
  done; }
# After the two-display recipe, with a flag: the pipe-0 engine armed, so a System Settings pick on the AOC (PhxAMDFB head 0,
# listed when the daemon set debug.phxpipe0_modes before the claim) can run; -aoc144: 144 asked once a user is logged in.
aoc_arm() {
  aocflag || return 0
  $SC -w debug.phxpipe0_enable=1 > /dev/null 2>&1 || { echo "b15: aoc: no debug.phxpipe0_enable (kext < b99): the AOC stays at 60"; return 0; }
  echo "b15: aoc: pipe-0 engine armed (debug.phxpipe0_enable=1): the AOC's 144 Hz mode can be picked"
  rate_request
  return 0; }
# T7: the System Settings path (cgmode = CGConfigureDisplayWithDisplayMode in the console user's session): PhxAMDFB head 0
# -> the kext's macOS path (stutter off, UCLK pin, round trip, set); the watchdog's follower then applies aoc144.
# The after-login 144 request (both the two-display and the one-display AOC branch): with a boot-rate file only as the
# fallback of a boot rate 144 that did not take; without one, only with ~/gpu/phxboot-m7-aoc144
rate_request() {
  if [ -e "$AOCRATEF" ]; then
    if [ "$(aocrate)" = 144 ]; then aoc_request & echo "b15: aoc: boot rate 144 not in place before the WindowServer restart: 144 asked once a user is logged in (pid $!)"
    else echo "b15: aoc: boot rate 60 ($AOCRATEF): no request"; fi
  elif [ -e "$G/phxboot-m7-aoc144" ]; then aoc_request & echo "b15: aoc: 144 asked once a user is logged in (pid $!)"; fi
}
aoc_request() {
  u=""; i=0
  while [ "$i" -lt "$AOCWAIT" ]; do u=$($CONSOLE 2> /dev/null); [ -n "$u" ] && [ "$u" != 0 ] && break; u=""; sleep 5; i=$((i + 5)); done
  [ -n "$u" ] || { echo "b15: aoc: no console user within ${AOCWAIT}s: the AOC stays at 60"; return 0; }
  sleep "$AOCSETTLE"
  l=$($ASUSER "$u" sudo -n -u "#$u" "$G/cgmode" list 2> /dev/null)
  d=$(echo "$l" | awk '/model 0x2402/ {print $4; exit}')
  [ -n "$d" ] || { echo "b15: aoc: the AOC (model 0x2402) is not in cgmode list: the AOC stays at 60"; return 0; }
  # Ask for the size the AOC runs now: a rotated AOC lists only its 1080x1920 twins.
  sz=$(echo "$l" | awk -v d="$d" '/^display/ {on = ($4 == d)} on && / current/ {print $1; exit}')
  w=${sz%x*}; h=${sz#*x}
  case "$w$h" in *[!0-9]*|'') w=1920; h=1080;; esac
  out=$($ASUSER "$u" sudo -n -u "#$u" "$G/cgmode" set "$d" "$w" "$h" 144 2>&1); rc=$?
  echo "b15: $(date '+%T') aoc: cgmode set $d $w $h 144 rc $rc: $(echo "$out" | tail -1 | cut -c1-120)"; }
# Boot rate: the AOC at 144 before the daemon's WindowServer restart (see the header). Same steps and order as the after-login
# path reached them: the pair's 60 watermarks, hubp_dlg linux60, the underflow stickies cleared (the survey refuses a set
# ODM0 sticky), p0prefetch linux (the round trip needs every 60-plan register equal; the Acer at 165:
# PHXP0_PAIR165=1), the engine armed, debug.phxpipe0_mode=144 + debug.phxpipe0=4 (stutter off, UCLK 456, the 60 round
# trip, the set), then aoc144. Exit 0 = OTG0 at 1080p144 with aoc144 done; any refusal before the set leaves the AOC at 60
# (this script's 60 recipe after `done` repeats the same pipe-0 steps). Needs pipe 1 at 144 or 165 with DET 9/9 (m7_boot.sh).
aocboot() {
  ht=$(r 0x4FEA); vt=$(r 0x4FEF)
  [ $(( ht )) -eq $((0x897)) ] && [ $(( vt )) -eq $((0x464)) ] || { echo "b15: aocboot: OTG0 $ht/$vt is not the AOC at 1080p60: nothing done"; return 1; }
  c1=$(r 0x5081); h1=$(r 0x506A); v1=$(r 0x506F)
  if is165; then wmp=linux60a165; pr=165
  elif [ $(( c1 & 1 )) -eq 1 ] && [ $(( h1 )) -eq $((0x807)) ] && [ $(( v1 )) -eq $((0x449)) ]; then wmp=linux2; pr=144
  else echo "b15: aocboot: OTG1 ($c1 $h1/$v1) is not the Acer at 1080p144 or 1080p165: nothing done"; return 1; fi
  d0=$(r 0x397B); d1=$(r 0x397C)
  [ $(( d0 & 0x1f )) -eq 9 ] && [ $(( d1 & 0x1f )) -eq 9 ] || { echo "b15: aocboot: DET0/1 $d0/$d1 not 9/9: nothing done"; return 1; }
  sa=$(r 0x39BC); [ "$sa" = 0x00001002 ] || { echo "b15: aocboot: 0x39BC = $sa, not the GOP 0x1002: nothing done"; return 1; }
  case "$(sw0)" in *busy=1*) echo "b15: aocboot: the pipe-0 engine is busy: nothing done"; return 1 ;; esac
  [ -e "$G/p0prefetch.sh" ] || { echo "b15: aocboot: $G/p0prefetch.sh missing: nothing done"; return 1; }
  $SC -n debug.phxpipe0_enable > /dev/null 2>&1 || { echo "b15: aocboot: no debug.phxpipe0_enable (kext < b99): nothing done"; return 1; }
  pf=0; [ $pr = 165 ] && pf=1
  step wm_pair sh "$G/wm144.sh" $wmp || return 1
  step hubp_dlg sh "$G/hubp_dlg.sh" linux60 || return 1
  clear_stickies
  step p0prefetch env PHXP0_PAIR165=$pf sh "$G/p0prefetch.sh" linux || return 1
  $SC -w debug.phxpipe0_enable=1 > /dev/null 2>&1 && $SC -w debug.phxpipe0_mode=144 > /dev/null 2>&1 ||
    { echo "b15: aocboot: the pipe-0 engine refused enable / mode 144: the AOC stays at 60"; return 1; }
  out=$($SC -w debug.phxpipe0=4 2>&1); rc=$?
  echo "b15: $(date '+%T') aocboot: debug.phxpipe0=4 (the macOS sequence to 144 next to the Acer at $pr) rc $rc: $($SW0 2>/dev/null | tr '\n' ' ' | cut -c1-240)"
  ht=$(r 0x4FEA); vt=$(r 0x4FEF)
  [ $(( ht )) -eq $((0x807)) ] && [ $(( vt )) -eq $((0x44B)) ] || { echo "b15: aocboot: OTG0 $ht/$vt after the set: the AOC stays at 60"; return 1; }
  aoc144 || return 1
  echo "b15: $(date '+%T') aocboot: the AOC at 144 before the WindowServer restart"; }
# The daemon's undo when PhxAccel head 0 did not take the new mode: the engine back to 60, then aoc60's sets for the pair.
aocback() {
  $SC -w debug.phxpipe0_mode=60 > /dev/null 2>&1; out=$($SC -w debug.phxpipe0=4 2>&1); rc=$?
  echo "b15: $(date '+%T') aocback: debug.phxpipe0=4 to 60 rc $rc, OTG0 $(r 0x4FEA)/$(r 0x4FEF)"
  if alone; then aoc60solo; else aoc60; fi; }
# Boot rate memory: the pair both OTGs hold (totals only; an unknown mode, OTG1 dark or an engine busy = no sample), written
# once it has held RATESTABLE s and differs from the file. A transient (connect-change 60, a switch) never lasts that long.
rmc=""; rms=0
rates_now() {
  case "$(sw0)$(sw)" in *busy=1*) return 0 ;; esac
  rh0=$(( $(r 0x4FEA) )); rv0=$(( $(r 0x4FEF) ))
  if [ $rh0 -eq $((0x807)) ] && [ $rv0 -eq $((0x44B)) ]; then ra=144; elif [ $rh0 -eq $((0x897)) ] && [ $rv0 -eq $((0x464)) ]; then ra=60; else return 0; fi
  [ $(( $(r 0x5081) & 1 )) -eq 1 ] || return 0
  rh1=$(( $(r 0x506A) )); rv1=$(( $(r 0x506F) ))
  if [ $rh1 -eq $((0x807)) ] && [ $rv1 -eq $((0x449)) ]; then rb=144; elif [ $rh1 -eq $((0x897)) ] && [ $rv1 -eq $((0x464)) ]; then rb=60
  elif [ $rh1 -eq $((0x81F)) ] && [ $rv1 -eq $((0x455)) ]; then rb=165; else return 0; fi
  echo "aoc=$ra acer=$rb"; }
remember_tick() {
  rcand=$(rates_now); rnow=$(date +%s)
  [ -n "$rcand" ] || { rmc=""; return 0; }
  [ "$rcand" = "$rmc" ] || { rmc=$rcand; rms=$rnow; return 0; }
  [ $(( rnow - rms )) -ge "$RATESTABLE" ] || return 0
  [ "$(sed -n 's/^\(aoc=[0-9]* acer=[0-9]*\).*/\1/p' "$RATELAST" 2>/dev/null | head -1)" = "$rcand" ] && return 0
  printf '%s at %s\n' "$rcand" "$(date '+%F %T')" > "$RATELAST.tmp" && mv -f "$RATELAST.tmp" "$RATELAST" &&
    echo "b15: $(date '+%T') rates remembered for the next boot: $rcand ($RATELAST)"; }
for f in wm144.sh det.sh dalwm.sh dal.sh hubp_dlg.sh uclk.sh; do [ -e "$G/$f" ] || { echo "b15: $G/$f missing: skip"; exit 1; }; done
case "${1:-}" in aoc144) aoc144; exit $? ;; aoc60) aoc60; exit $? ;; aocboot) aocboot; exit $? ;; aocback) aocback; exit $? ;;
  aocboot1) aocboot1; exit $? ;; aoc144solo) aoc144solo; exit $? ;; aoc60solo) aoc60solo; exit $? ;; watch1) echo "b15: $(date '+%T') watchdog only (one display), every ${POLL}s"; watch1 ;; esac
[ "${1:-}" = watch2 ] || { s=$(r 0x39BC); [ "$s" = 0x00001002 ] || { echo "b15: 0x39BC = $s, not the GOP 0x1002: skip"; exit 1; }; }
# p0solo: the AOC alone (OTG1-3 dark, OTG0 at one of the AOC's modes, the head-0 mode list flag): at 144 (aocboot1 put it
# there) keep that state; at 60 the pipe-0 prep + the engine armed (and the after-login request with boot rate 144); then watch1
one_aoc() {
  aocflag || return 0
  ht=$(r 0x4FEA); vt=$(r 0x4FEF)
  if [ $(( ht )) -eq $((0x807)) ] && [ $(( vt )) -eq $((0x44B)) ]; then
    aoc144solo || exit 1
    arm1 && echo "b15: aoc: pipe-0 engine armed with solo (debug.phxpipe0_enable=1, debug.phxpipe0_solo=1), the AOC alone at 144 since the boot" ||
      echo "b15: aoc: the pipe-0 engine refused enable / solo: System Settings picks are refused this boot"
    echo "b15: $(date '+%T') one display (the AOC) at 144, watchdog every ${POLL}s"; watch1; fi
  [ $(( ht )) -eq $((0x897)) ] && [ $(( vt )) -eq $((0x464)) ] || return 0
  $SC -n debug.phxpipe0_solo > /dev/null 2>&1 || { echo "b15: aoc: one display, PhxBringup without debug.phxpipe0_solo: the AOC stays at 60"; exit 1; }
  prep1 one_aoc || { echo "b15: aoc: one display, the pipe-0 prep failed: the AOC stays at 60"; exit 1; }
  arm1 || { echo "b15: aoc: the pipe-0 engine refused enable / solo: the AOC stays at 60"; exit 1; }
  echo "b15: aoc: pipe-0 engine armed with solo: the AOC's 144 Hz mode can be picked (one display)"
  rate_request
  echo "b15: $(date '+%T') one display (the AOC) at 60, watchdog every ${POLL}s"; watch1; }
two_pipe() {
  [ -e "$G/phxboot-b15-2pipe" ] || { echo "b15: OTG1 is lit: two displays, no ~/gpu/phxboot-b15-2pipe: skipped"; exit 1; }
  for o in 2 3; do c=$(r $(printf 0x%X $((0x4FEA + 0x80 * o + 0x17)))); [ $(( c & 1 )) -eq 0 ] || { echo "b15: OTG$o lit too: skipped"; exit 1; }; done
  ht=$(r 0x4FEA)
  if [ "$(( ht ))" -eq $((0x807)) ] && aocflag; then   # the AOC already at 144: the 144 + 144 (b103: or 144 + 60) branch
    aoc144 || exit 1; case $(( $(r 0x506A) )) in $((0x897))) pr="144 + 60" ;; $((0x81F))) pr="144 + 165" ;; *) pr="144 + 144" ;; esac
    # boot rate: the engine armed for later System Settings picks (aocboot armed it already; no request: the AOC is at 144)
    $SC -w debug.phxpipe0_enable=1 > /dev/null 2>&1 && echo "b15: aoc: pipe-0 engine armed (debug.phxpipe0_enable=1), the AOC at 144 since the boot"
    echo "b15: $(date '+%T') two displays at $pr, watchdog every ${POLL}s"; watch2; fi
  [ "$(( ht ))" -eq $((0x897)) ] || { echo "b15: two displays but OTG0 H_TOTAL $ht is not 1080p60: skipped"; exit 1; }
  d0=$(r 0x397B); d1=$(r 0x397C)
  [ $(( d0 & 0x1f )) -eq 9 ] && [ $(( d1 & 0x1f )) -eq 9 ] || { echo "b15: two displays but DET0/1 $d0/$d1 not 9/9: skipped"; exit 1; }
  if is165; then   # SCDC2: the Acer already at 165 next to the AOC at 60: the AOC's 60 group, then aoc60's 165 branch, no unpin
    step hubp_dlg sh "$G/hubp_dlg.sh" linux60 || exit 1
    aoc60 || exit 1
    # 11-02: WindowServer restored the Acer at 165 and pipe 0 kept the GOP frame group (8 of the 60 plan's 32
    # registers), so the kext's 60 round trip refused every AOC 144 pick for the whole boot. The stickies the boot left (logged
    # first: the survey refuses a set ODM0 sticky) are cleared, then p0prefetch linux next to the Acer at 165, as aocboot.
    echo "b15: boot stickies before the clear: OPTC0 $(r 0x4F8A) OPTC1 $(r 0x4F9A)"; clear_stickies
    [ -e "$G/p0prefetch.sh" ] && { step p0prefetch env PHXP0_PAIR165=1 sh "$G/p0prefetch.sh" linux ||
      echo "b15: p0prefetch failed: an AOC 144 pick is refused this boot"; }
    echo "b15: $(date '+%T') two displays at 60 + 165, watchdog every ${POLL}s"; aoc_arm; watch2; fi
  step wm144 sh "$G/wm144.sh" linux2 || exit 1
  step hubp_dlg sh "$G/hubp_dlg.sh" linux60 || exit 1
  [ -e "$G/phxboot-b15-dpm" ] || { echo "b15: two displays: watermarks + HUBP0 done, no dpm flag"; exit 0; }
  clear_stickies
  # Pipe-0 underflow fix (10-31): pipe-0 frame-position group = Linux while
  # UCLK is still pinned, only with the flag.
  [ -e "$G/phxboot-b15-2pipe-p0pf" ] && { step p0prefetch sh "$G/p0prefetch.sh" linux || exit 1; }
  # Pipe-0 underflow (10-31): the DAL watermark table before UCLK is unpinned, only with the flag.
  [ -e "$G/phxboot-b15-2pipe-dalwm" ] && { step dalwm sh "$G/dalwm.sh" || exit 1; }
  step uclk_unpin sh "$G/uclk.sh" unpin || exit 1
  step fclk_switch sh "$G/dal.sh" SetFclkSwitchAllow 1
  if [ -e "$G/phxboot-b15-2pipe-sr" ]; then w 0x39BC 0x1000; sr="stutter allowed (0x39BC $(r 0x39BC))"; else sr="stutter forced off"; fi
  echo "b15: $(date '+%T') two displays: UCLK/FCLK free, $sr, watchdog every ${POLL}s"
  aoc_arm
  watch2; }
# The two-pipe watchdog alone (`b15_boot.sh watch2`: restart it in a running boot, e.g. after deploying a new copy;
# the clocks and stutter stay as the boot set them).
# One poll's reads, in the order the decisions need them: pipe-1 state, pipe-0 state, OPTC0, HUBP0, pipe-0 state, OPTC1,
# HUBP1, pipe-1 state (each pipe's stickies between two reads of its engine's state).
w2read() {
  if srv @$SWN @$SW0N 0x4F8A 0x3AB3 @$SW0N 0x4F9A 0x3B8F @$SWN && sget s0 && sget p0 && sget wv0 && sget wh0 && sget p1 && sget wv1 &&
    sget wh1 && sget s1; then bgof "$s0"; s0=$BG; bgof "$p0"; p0=$BG; bgof "$p1"; p1=$BG; bgof "$s1"; s1=$BG
  else s0=$(sw); p0=$(sw0); wv0=$(r 0x4F8A); wh0=$(r 0x3AB3); p1=$(sw0); wv1=$(r 0x4F9A); wh1=$(r 0x3B8F); s1=$(sw); fi; }
watch2() {
  srv_start; n=0; follow=0; aocflag && follow=1; lastht=$(r 0x4FEA)/$(r 0x506A); rl_init
  while :; do
    k=""; w2read
    case "$p0$p1" in *busy=1*) ;; *) if [ "$p0" = "$p1" ]; then [ $(( wv0 & 0x400 )) -ne 0 ] && k=optc0; [ $(( (wh0 >> 28) & 7 )) -ne 0 ] && k="${k:+$k+}hubp0"; fi ;; esac
    case "$s0$s1" in *busy=1*) ;; *) if [ "$s0" = "$s1" ]; then [ $(( wv1 & 0x400 )) -ne 0 ] && k="${k:+$k+}optc1"; [ $(( (wh1 >> 28) & 7 )) -ne 0 ] && k="${k:+$k+}hubp1"; fi ;; esac
    if [ -n "$k" ]; then w 0x39BC 0x1002; echo "b15: $(date '+%T') $k underflow: stutter forced off, UCLK pinned 456 for this boot"; step uclk_repin sh "$G/uclk.sh" pin 456; exit 2; fi
    [ $follow = 1 ] && follow0
    [ $rlon = 1 ] && rl_tick; [ $rlon = 2 ] && rl_arm
    [ $remember = 1 ] && [ $(( n % RMEMN )) -eq 0 ] && remember_tick
    n=$((n + 1)); [ "$MAXPOLL" -gt 0 ] && [ $n -ge "$MAXPOLL" ] && { echo "b15: watchdog stopped after $n polls (test)"; exit 0; }
    sleep "$POLL"
  done; }
# The AOC (or, b103, the Acer next to the AOC at 144) changed mode (the kext's macOS path, its pipe-1 switch, debug.phxpipe0):
# the matching set once per change of OTG0 / OTG1 H_TOTAL, never while either engine is busy or started meanwhile. AOC at
# 144: aoc144 (linux144x2 next to the Acer at 144, linux144a60 next to 60, SCDC2 linux144a165 next to 165); AOC back at 60:
# aoc60 (linux2); the Acer changing between 60 and 144 next to the AOC at 60: nothing (linux2 as before b103); SCDC2: the
# Acer entering or leaving 165 next to the AOC at 60: aoc60 (linux60a165 / linux2).
follow0() {
  if srv @$SW0N @$SWN 0x4FEA 0x506A && sget q0 && sget q1 && sget ht && sget fh1; then bgof "$q0"; q0=$BG; bgof "$q1"; q1=$BG
    case "$q0$q1" in *busy=1*) return 0 ;; esac
  else q0=$(sw0); q1=$(sw); case "$q0$q1" in *busy=1*) return 0 ;; esac; ht=$(r 0x4FEA); fh1=$(r 0x506A); fi
  key=$ht/$fh1; [ "$key" = "$lastht" ] && return 0
  [ "$(sw0)" = "$q0" ] && [ "$(sw)" = "$q1" ] || return 0
  o0=${lastht%%/*}; o1=${lastht#*/}; n1=${key#*/}; lastht=$key
  if [ $(( ht )) -eq $((0x807)) ]; then aoc144
  elif [ $(( ht )) -eq $((0x897)) ]; then   # SCDC2: also when the Acer enters or leaves 165 next to the AOC at 60
    if [ "$ht" != "$o0" ] || [ $(( n1 )) -eq $((0x81F)) ] || [ $(( o1 )) -eq $((0x81F)) ]; then aoc60; fi
  else echo "b15: pipe 0 at H_TOTAL $ht: no set for it"; fi
  return 0; }
# Acer relight (see the header). rls = idle | wait (the check after an event) | lock144 (after the switch to 144) | ver165
# (after the switch back to 165); rlt = the next step's time and rl0 the step's start, both in the stat's now_ms.
RLDELAY=${PHXB15_RLDELAY:-1000}; RLHPN=${PHXB15_RLHPN:-5}; RLRETRY=${PHXB15_RLRETRY:-60}; RLRETRYMS=${PHXB15_RLRETRYMS:-5000}; rlr=0; ACERMODEL=${PHXB15_ACERMODEL:-0x74d}; RLRESET=${PHXB15_RLRESET:-20}
case "$RLRESET" in ''|*[!0-9]*) RLRESET=20 ;; esac
rlmac=0; rlon=0; rls=idle; rlo=0; rlg=0; rlt=0; rl0=0; rln=0; rlk=0; rlnow=0; rlpend=none; rlho=0; rlhg=0
# debug.phxhotplug_stat -> rlnow (now_ms), rlho (online), rlhg (head-1 plugs), rlpend (pend); 1 = no readable stat.
# why= is the line's last field and free text ("plug: the same monitor, pipe 1 lit: head 1 online"): the parse stops there,
# else its bare "online" word blanks the counter and every read after a plug is unreadable (11-04).
hpstat() {
  hv=$($SC -n debug.phxhotplug_stat 2>/dev/null | awk '
    NR == 1 && $1 == "hotplug" {ok = 1; for (i = 2; i <= NF; i++) {split($i, a, "="); if (a[1] == "why") break; if (a[1] == "now_ms") t = a[2]; else if (a[1] == "online") o = a[2]; else if (a[1] == "pend") p = a[2]}}
    / head1 / {for (i = 1; i <= NF; i++) {split($i, a, "="); if (a[1] == "plugs") g = a[2]}}
    END {if (ok && t ~ /^[0-9]+$/ && o ~ /^[0-9]+$/) print t, o, g + 0, (p == "" ? "none" : p)}')
  [ -n "$hv" ] || return 1
  set -- $hv; rlnow=$1; rlho=$2; rlhg=$3; rlpend=$4; }
p1mode() { $SW 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^mode=/) {sub(/^mode=/, "", $i); print $i; exit}}'; }
swwhy() { $SW 2>/dev/null | sed -n 's/.*last=\([a-z]*\).*why=/\1: /p' | cut -c1-80; }
scdc1() { $SC -n debug.phxscdc 2>/dev/null | head -1; }
# one key=value of a debug.phxscdc line ("rc= sink_version= tmds_config= scrambler= flags0= locked= ...")
rl_kv() { echo "$1" | awk -v k="$2" '{for (i = 1; i <= NF; i++) if (index($i, k "=") == 1) {print substr($i, length(k) + 2); exit}}'; }
rl_lit() { [ "$(rl_kv "$1" rc)" = 0 ] && [ "$(rl_kv "$1" tmds_config)" = 0x03 ] && [ "$(rl_kv "$1" scrambler)" = 0x01 ] && [ "$(rl_kv "$1" locked)" = 1 ]; }
# no step while either kext engine is busy (a head-0 / pipe-1 switch) or a head-1 hot-plug action is pending
rl_ready() { case "$(sw0)$(sw)" in *busy=1*) return 1 ;; esac; [ "$rlpend" = none ]; }
# rl_sw <hz>: one relight switch of pipe 1 (see the header), 0 = done (or still running in the kext). The macOS path: cgmode
# set rc 0 (its read-back matched) and the kext's switch_stat at <hz> or busy (box 11-04 13:13: cgmode
# returned after 4 s with switch_stat last=done mode=165); a rotated Acer lists only its 1080x1920 modes (as the AOC,
# see aoc_request). Else the kernel path, debug.phxpipe2_switch=<hz>, with the reason in its line.
# rl_cg <hz>: the macOS path alone, 0 = done (rlcg = the cgmode line; the stat read again after it), else rlwhy = why not.
rl_cg() {
  rlhz=$1; rlwhy=""; rlu=$($CONSOLE 2> /dev/null)
  if [ -z "$rlu" ] || [ "$rlu" = 0 ]; then rlwhy="no console user"
  elif [ ! -x "$G/cgmode" ]; then rlwhy="no $G/cgmode"
  else
    rll=$($ASUSER "$rlu" sudo -n -u "#$rlu" "$G/cgmode" list 2> /dev/null)
    rld=$(echo "$rll" | awk -v m="$ACERMODEL" '$1 == "display" && $5 == "model" && tolower($6) == tolower(m) {print $4; exit}')
    if [ -z "$rld" ]; then rlwhy="the Acer (model $ACERMODEL) is not in cgmode list"
    else
      rlsz=$(echo "$rll" | awk -v d="$rld" '/^display/ {on = ($4 == d)} on && / current/ {print $1; exit}')
      rlw=${rlsz%x*}; rlh=${rlsz#*x}
      case "$rlw$rlh" in *[!0-9]*|'') rlw=1920; rlh=1080;; esac
      rlout=$($ASUSER "$rlu" sudo -n -u "#$rlu" "$G/cgmode" set "$rld" "$rlw" "$rlh" "$rlhz" 2>&1); rlrc=$?
      rlcg="cgmode set $rld $rlw $rlh $rlhz rc $rlrc: $(echo "$rlout" | tail -1 | cut -c1-100)"
      if [ $rlrc = 0 ]; then
        case "$(sw)" in *busy=1*) rlm=$rlhz ;; *) rlm=$(p1mode) ;; esac
        if [ "$rlm" = "$rlhz" ]; then hpstat; return 0; fi
        rlwhy="$rlcg; pipe 1 at ${rlm:-?} after it ($(swwhy))"
      else rlwhy=$rlcg; fi
    fi
  fi
  return 1; }
rl_sw() {
  if rl_cg "$1"; then rlmac=1; echo "b15: $(date '+%T') relight: $rlhz through macOS ($rlcg)"; return 0; fi
  echo "b15: $(date '+%T') relight: $rlhz through the kernel (debug.phxpipe2_switch=$rlhz): $rlwhy"
  $SC -w debug.phxpipe2_switch=$rlhz > /dev/null 2>&1; }
# rlon: 0 off (no stat), 1 on, 2 waiting: the stat exists but hot-plug was never armed this boot (debug.phxhotplug=0 at
# boot reads "hotplug v1 level=0 never armed", no now_ms / online): rl_arm re-reads it every RLHPN polls and turns the
# relight on, with the counters of that read as the baseline, once debug.phxhotplug is armed.
rl_init() {
  hpstat && { rlon=1; rlo=$rlho; rlg=$rlhg; return 0; }
  case "$($SC -n debug.phxhotplug_stat 2>/dev/null | head -1)" in
    "hotplug "*) rlon=2; echo "b15: relight: hot-plug not armed (debug.phxhotplug_stat level=0): the Acer relight starts once debug.phxhotplug is armed" ;;
    *) echo "b15: relight: no debug.phxhotplug_stat (PhxBringup < b102): the Acer relight is off" ;;
  esac; return 0; }
rl_arm() {
  [ $(( n % RLHPN )) -ne 0 ] && return 0
  hpstat || return 0
  rlon=1; rlo=$rlho; rlg=$rlhg
  echo "b15: $(date '+%T') relight: hot-plug armed (online $rlho, head-1 plugs $rlhg): the Acer relight is on"; }
rl_tick() {
  case $rls in idle|settle) [ $(( n % RLHPN )) -ne 0 ] && return 0 ;; esac
  hpstat || return 0
  [ "$rlho" -lt "$rlo" ] && rlo=$rlho; [ "$rlhg" -lt "$rlg" ] && rlg=$rlhg   # the counters restarted (kext reload)
  if [ "$rlho" -gt "$rlo" ] || [ "$rlhg" -gt "$rlg" ]; then
    case $rls in
      idle|wait|settle) echo "b15: $(date '+%T') relight: Acer hot-plug event (online $rlo -> $rlho, head-1 plugs $rlg -> $rlhg): SCDC check in $RLDELAY ms"
        rls=wait; rlt=$((rlnow + RLDELAY)); rln=0; rlr=0 ;;
      *) echo "b15: $(date '+%T') relight: Acer hot-plug event (online $rlo -> $rlho, head-1 plugs $rlg -> $rlhg) during the relight ($rls): it goes on" ;;
    esac
    rlo=$rlho; rlg=$rlhg; fi
  [ $rls = idle ] && return 0
  [ "$rlnow" -ge "$rlt" ] || return 0
  rl_ready || return 0
  case $rls in wait) rl_check ;; lock144) rl_lock ;; ver165) rl_verify ;; settle) rl_settle ;; rs144) rl_rs144 ;; rs165) rl_rs165 ;; esac
  return 0; }
rl_check() {
  m1=$(p1mode)
  if [ "$m1" != 165 ] || ! is165; then rls=idle
    echo "b15: $(date '+%T') relight: pipe 1 at ${m1:-?} Hz (OTG1 $(r 0x506A)/$(r 0x506F)), not 165: nothing to do"; return 0; fi
  x=$(scdc1); rln=$((rln + 1))
  if rl_lit "$x"; then rls=idle; echo "b15: $(date '+%T') relight: pipe 1 at 165, SCDC scrambled and locked ($x): nothing to do"; return 0; fi
  case "$(rl_kv "$x" rc)" in 0) ;; *)
    if [ $rln -lt 3 ]; then rlt=$((rlnow + 1000)); echo "b15: $(date '+%T') relight: SCDC not readable (${x:-no answer}): again in 1 s"
    else echo "b15: $(date '+%T') relight: SCDC not readable 3 times (${x:-no answer}): taken as dark (the sink is not decoding pipe 1's 165)"; fi
    [ $rln -lt 3 ] && return 0 ;; esac   # 11-04: right after a plug the dark sink answers its DDC only now and then
  case "$(sw0)$(sw)" in *busy=1*) rlt=$((rlnow + 1000)); return 0 ;; esac   # an engine started during the DDC read: check again
  [ "$($SC -n debug.phxpipe2_switch 2>/dev/null)" = 165 ] || { rls=idle; echo "b15: $(date '+%T') relight: SCDC dark ($x) but no debug.phxpipe2_switch at 165: nothing done"; return 0; }
  rlmac=0
  if rl_sw 144; then rls=lock144; rlk=0; rl0=$rlnow; rlt=$((rlnow + 1000))
    echo "b15: $(date '+%T') relight: pipe 1 at 165 with the Acer's SCDC dark ($x): pipe 1 switched to 144 ($(swwhy)), waiting for the SCDC lock"
  elif [ $rlr -lt $RLRETRY ]; then rlr=$((rlr + 1)); rls=wait; rln=0; rlt=$((rlnow + RLRETRYMS))   # 11-04 HP1-P8: refused while the AOC was out
    [ $rlr = 1 ] || [ $(( rlr % 12 )) = 0 ] && echo "b15: $(date '+%T') relight: SCDC dark ($x), the switch to 144 not done ($(swwhy)): again in $((RLRETRYMS / 1000)) s (try $rlr of $RLRETRY)"
  else rls=idle; echo "b15: $(date '+%T') relight: SCDC dark ($x), the switch to 144 not done ($(swwhy)) after $RLRETRY tries: pipe 1 at $(p1mode), given up until the next plug"; fi; }
rl_lock() {
  m1=$(p1mode)
  [ "$m1" = 144 ] || { rls=idle; echo "b15: $(date '+%T') relight: pipe 1 at ${m1:-?} Hz meanwhile (not the relight's 144): stop, the mode is left as it is"; return 0; }
  x=$(scdc1); if [ "$(rl_kv "$x" rc)" = 0 ] && [ "$(rl_kv "$x" locked)" = 1 ]; then rlk=$((rlk + 1)); else rlk=0; fi
  if [ $rlk -ge 2 ]; then
    case "$(sw0)$(sw)" in *busy=1*) rlk=1; rlt=$((rlnow + 1000)); return 0 ;; esac
    if rl_sw 165; then rls=ver165; rl0=$rlnow; rlt=$((rlnow + 500))
      echo "b15: $(date '+%T') relight: SCDC locked twice at 144 ($x): pipe 1 switched to 165 ($(swwhy)), checking the scrambled lock"
    else rls=idle; echo "b15: $(date '+%T') relight: the switch to 165 not done ($(swwhy)): pipe 1 at $(p1mode)"; fi; return 0; fi
  if [ $(( rlnow - rl0 )) -ge 10000 ]; then rls=idle; echo "b15: $(date '+%T') relight: the Acer's SCDC not locked twice in 10 s at 144 ($x): the Acer stays at 144"; return 0; fi
  rlt=$((rlnow + 1000)); }
rl_verify() {
  m1=$(p1mode)
  [ "$m1" = 165 ] || { rls=idle; echo "b15: $(date '+%T') relight: pipe 1 at ${m1:-?} Hz meanwhile (not the relight's 165): stop, the mode is left as it is"; return 0; }
  x=$(scdc1)
  if rl_lit "$x"; then rls=idle; echo "b15: $(date '+%T') relight: done, pipe 1 at 165 with the SCDC scrambled and locked ($x)"
    if [ $rlmac = 1 ] && [ "$RLRESET" -gt 0 ]; then rls=settle; rlt=$((rlnow + RLRESET * 1000))
      echo "b15: $(date '+%T') relight: settle re-set in $RLRESET s (the relight used the macOS path)"; fi
    return 0; fi
  [ $(( rlnow - rl0 )) -ge 3000 ] || { rlt=$((rlnow + 500)); return 0; }
  case "$(sw0)$(sw)" in *busy=1*) rlt=$((rlnow + 500)); return 0 ;; esac
  rls=idle
  if rl_sw 144; then echo "b15: $(date '+%T') relight: no scrambled lock at 165 in 3 s ($x): pipe 1 back at 144 ($(swwhy))"
  else echo "b15: $(date '+%T') relight: no scrambled lock at 165 in 3 s ($x), the switch back to 144 not done ($(swwhy)): pipe 1 at $(p1mode)"; fi; }
# The settle re-set (see the header): settle (the wait) -> rs144 (one SCDC lock read) -> rs165 (the scrambled lock), cgmode only.
rl_settle() {
  m1=$(p1mode)
  if [ "$m1" != 165 ] || ! is165; then rls=idle
    echo "b15: $(date '+%T') relight: settle re-set skipped: pipe 1 at ${m1:-?} Hz (OTG1 $(r 0x506A)/$(r 0x506F)), not 165: the mode is left as it is"; return 0; fi
  if rl_cg 144; then rls=rs144; rl0=$rlnow; rlt=$((rlnow + 1000))
    echo "b15: $(date '+%T') relight: settle re-set: 144 through macOS ($rlcg), waiting for one SCDC lock"
  else rls=idle; echo "b15: $(date '+%T') relight: settle re-set not done: $rlwhy (pipe 1 at $(p1mode))"; fi; }
rl_rs144() {
  m1=$(p1mode)
  [ "$m1" = 144 ] || { rls=idle; echo "b15: $(date '+%T') relight: settle re-set: pipe 1 at ${m1:-?} Hz meanwhile (not the re-set's 144): stop, the mode is left as it is"; return 0; }
  x=$(scdc1)
  if [ "$(rl_kv "$x" rc)" = 0 ] && [ "$(rl_kv "$x" locked)" = 1 ]; then rlv="SCDC locked at 144 ($x)"
  elif [ $(( rlnow - rl0 )) -ge 10000 ]; then rlv="no SCDC lock in 10 s at 144 ($x), 165 anyway"
  else rlt=$((rlnow + 1000)); return 0; fi
  case "$(sw0)$(sw)" in *busy=1*) rlt=$((rlnow + 1000)); return 0 ;; esac
  if rl_cg 165; then rls=rs165; rl0=$rlnow; rlt=$((rlnow + 500))
    echo "b15: $(date '+%T') relight: settle re-set: $rlv: 165 through macOS ($rlcg), checking the scrambled lock"
  else rls=idle; echo "b15: $(date '+%T') relight: settle re-set: $rlv, the switch to 165 not done: $rlwhy (pipe 1 at $(p1mode))"; fi; }
rl_rs165() {
  m1=$(p1mode)
  [ "$m1" = 165 ] || { rls=idle; echo "b15: $(date '+%T') relight: settle re-set: pipe 1 at ${m1:-?} Hz meanwhile (not the re-set's 165): stop, the mode is left as it is"; return 0; }
  x=$(scdc1)
  if rl_lit "$x"; then rls=idle; echo "b15: $(date '+%T') relight: settle re-set done, pipe 1 at 165 with the SCDC scrambled and locked ($x)"; return 0; fi
  [ $(( rlnow - rl0 )) -ge 3000 ] || { rlt=$((rlnow + 500)); return 0; }
  rls=idle; echo "b15: $(date '+%T') relight: settle re-set: no scrambled lock at 165 in 3 s ($x): left as it is, no retry"; }
[ "${1:-}" = watch2 ] && { echo "b15: $(date '+%T') watchdog only (two displays), every ${POLL}s"; watch2; }
c1=$(r 0x5081); [ $(( c1 & 1 )) -eq 0 ] || two_pipe
alone && one_aoc
# M7 P0 (10-29): the recipe is the one-display (1080p144 on pipe 0) set; with a
# second pipe lit (OTG1-3 OTG_CONTROL bit 0 = OTG_MASTER_EN) its watermarks/DET/stutter are wrong: skip it all.
for o in 1 2 3; do c=$(r $(printf 0x%X $((0x4FEA + 0x80 * o + 0x17)))); [ $(( c & 1 )) -eq 0 ] || { echo "b15: OTG$o is lit ($c): more than one display, the one-display recipe is skipped"; exit 1; }; done
# ... and pipe 0 must run the mode the recipe was measured for (Acer 1080p144: OTG0_OTG_H_TOTAL = 2056 - 1). With the
# AOC on DP plugged in, the GOP drove OTG0 at 1080p60 (H_TOTAL 2199) and the 144 Hz DLG/watermark values do not fit
# (seen 10-29).
ht=$(r 0x4FEA); [ "$(( ht ))" -eq $((0x807)) ] || { echo "b15: OTG0 H_TOTAL $ht is not the 1080p144 0x807 the recipe is for: skipped"; exit 1; }
step wm144 sh "$G/wm144.sh" linux || exit 1
step det sh "$G/det.sh" set 9 || exit 1
step dalwm sh "$G/dalwm.sh" || exit 1
step hubp_dlg sh "$G/hubp_dlg.sh" linux || exit 1
w 0x4F8A 0x1000; h=$(r 0x3AB3); w 0x3AB3 $(printf 0x%X $(( (h & 0x0FFFFFFF) | 0x80000000 ))); w 0x3AB3 $(printf 0x%X $(( h & 0x0FFFFFFF )))
v=$(r 0x4F8A); h=$(r 0x3AB3)
[ $(( v & 0x400 )) -eq 0 ] && [ $(( (h >> 28) & 7 )) -eq 0 ] || { echo "b15: underflow stickies did not clear ($v $h): stutter stays off"; exit 1; }
w 0x39BC 0x1000; echo "b15: $(date '+%T') stutter allowed (0x39BC $(r 0x39BC)), watchdog every ${POLL}s"
if [ -e "$G/phxboot-b15-dpm" ]; then
  step uclk_unpin sh "$G/uclk.sh" unpin && step fclk_switch sh "$G/dal.sh" SetFclkSwitchAllow 1
fi
srv_start; n=0
while :; do
  if srv 0x4F8A 0x3AB3 && sget v && sget h; then :; else v=$(r 0x4F8A); h=$(r 0x3AB3); fi; k=""
  [ $(( v & 0x400 )) -ne 0 ] && k=optc
  [ $(( (h >> 28) & 7 )) -ne 0 ] && k="${k:+$k+}hubp"
  if [ -n "$k" ]; then w 0x39BC 0x1002; echo "b15: $(date '+%T') $k underflow (OPTC $v HUBP0 $h): stutter forced off for this boot"
    [ -e "$G/phxboot-b15-dpm" ] && step uclk_repin sh "$G/uclk.sh" pin 456
    exit 2; fi
  n=$((n + 1)); [ "$MAXPOLL" -gt 0 ] && [ $n -ge "$MAXPOLL" ] && { echo "b15: watchdog stopped after $n polls (test)"; exit 0; }
  sleep "$POLL"
done
