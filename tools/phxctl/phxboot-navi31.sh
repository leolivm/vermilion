#!/bin/bash
# SPDX-License-Identifier: MIT
# Navi31 phxboot (LaunchDaemon local.phxboot): boot-time GPU bring-up + Metal claim, then (on a desktop-arm boot) it
# confirms PhxAMDFB took the panel and a LIVE Metal device exists, arms the display, and does ONE clean WindowServer
# restart so WindowServer enumerates Metal fresh (Cobalt64's proven ordering), 2026-10-24.
#
# TESTABLE BUILD: every path + HOME is env-overridable (PHXBOOT_*) with the production value as the default, so running
# it unchanged on the box behaves exactly as before, while the mock harness (tests/mock/run.sh) can redirect every
# path into a fake root and shim every external command via PATH. This file is meant to REPLACE the deployed
# ~/gpu/phxboot-navi31.sh after review (defaults == current production, so no behaviour change on the box).
#
# SQUAT MODE (2026-10-24): a boot where PhxBringup's PhxFBSquatter holds the IOFramebuffer category (armed by the
# one-shot NVRAM var phxfb-squat, which the kext clears at probe) has NO IONDRV console: PhxAMDFB is the only display,
# so that boot is the desktop-arm boot. SUCCESS re-arms phxfb-squat for the next boot only while $AUTO exists; any
# failure (or kill switch / unfinished previous attempt) on a squat boot reboots, and the next boot comes up on IONDRV.
#
# Fail-safes: kill switch $OFF; unfinished boot (MARK) -> GOP; any failure -> m2_fail reverts to GOP. NO forced
# WindowServer kickstart (that wedged the GPU). Exactly ONE killall -9 WindowServer, only at 0 users, only at the end.
# Install layout (docs/INSTALL.md): the box tools live in ~/gpu of the desktop user, PHX_USER. The LaunchDaemon plist
# (local.phxboot.plist.in) sets PHX_USER; without it the owner of this script is taken (the install copies it as that user).
PHX_USER=${PHX_USER:-$(/usr/bin/stat -f %Su "$0" 2>/dev/null)}
if [ -z "${PHXBOOT_G:-}" ] || [ -z "${PHXBOOT_HOME:-}" ]; then
  PHX_UHOME=${PHX_UHOME:-$(/usr/bin/dscl . -read "/Users/$PHX_USER" NFSHomeDirectory 2>/dev/null | /usr/bin/awk '{print $2}')}
fi
G=${PHXBOOT_G:-${PHX_UHOME:-}/gpu}; export HOME=${PHXBOOT_HOME:-${PHX_UHOME:-}}
LOG=${PHXBOOT_LOG:-/var/log/phxboot.log}; MARK=${PHXBOOT_MARK:-/var/db/phxboot.pending}
OFF=${PHXBOOT_OFF:-$G/phxboot-off}; DESKTOP=${PHXBOOT_DESKTOP:-$G/phxboot-desktop}
# Failure marker (11-02): every revert to GOP leaves it; while it exists a GOP boot does NOT re-arm
# phxfb-squat (no reboot loop after a real failure). Remove it by hand (or a SUCCESS boot removes it).
FAILF=${PHXBOOT_FAILF:-$G/phxboot-failed}
ALLOW=${PHXBOOT_ALLOW:-/Library/GPUBundles/nvmtl-allow.txt}; DR=${PHXBOOT_DR:-/Library/Logs/DiagnosticReports}
MB=${PHXBOOT_MB:-/usr/local/sbin/mesboot}; AUTO=${PHXBOOT_AUTO:-$G/phxboot-auto}
PHXCTL=${PHXBOOT_PHXCTL:-/usr/local/sbin/phxctl}
ACCEL=${PHXBOOT_ACCEL:-/Library/Extensions/VermilionAccel.kext}; RUNAS=${PHXBOOT_RUNAS-sudo -u $PHX_USER}
LOGCMD=${PHXBOOT_LOGCMD:-/usr/bin/log}
exec >>"$LOG" 2>&1
echo "== $(date '+%F %T') phxboot(navi31, Option 1b)"
# First-WindowServer crash fix (11-03).
# The first WindowServer (up since boot, started before any display pipe existed) restores the stored one-display set as
# soon as the arm below publishes the display pipe (debug.nvaccel_iop/nvaccelfb=1). Since 11-01 that set is "AOC alone"
# at IODisplayModeID 2 (144 Hz); PhxAMDFB refuses id 2 while the pipe-0 kill switch is on, the refused switch still runs
# WillChange/DidChange, the pipe's framebuffer resource is rebuilt and that WindowServer's software blit writes through a
# failed CPU lock (NULL): SIGSEGV on almost every boot, mostly with no .ips.
# The stored sets cannot be kept away from that WindowServer: it reads com.apple.windowserver once, before this daemon runs,
# and every display change of a logged-in session copies the user's own DisplaySets (with "AOC
# alone" at 144) into DisplayAnyUserSets at once (SkyLight WSSaveServerWidePreferences). So the
# fix is PhxAccel's head-0 offer gate (src/phxaccel/accel/phxoffer.h): with a head-0 mode list, head 0 offers only the
# lit mode from its start, the stored 144 set matches nothing and the first WindowServer keeps the current mode; this daemon
# opens the gate (debug.phxfb0_offer=1, which also drops CoreDisplay's IOFBConfig cache) just before its ONE restart, so the
# restarted WindowServer lists every mode. The v4 prefs edits are gone; a v4 stash left on disk is put back once.
WSPFLAG=${PHXBOOT_WSPFLAG:-$G/phxboot-wsprefs}; WSPDOM=${PHXBOOT_WSPDOM:-/Library/Preferences/com.apple.windowserver}
WSPSTASH=${PHXBOOT_WSPSTASH:-$G/phxboot-wsprefs.stash.d}; WSPTMP=${PHXBOOT_WSPTMP:-/tmp}; PB=${PHXBOOT_PLISTBUDDY:-/usr/libexec/PlistBuddy}
wsp_val(){ "$PB" -c "Print :DisplayAnyUserSets:$2:0:$3" "$1" 2>/dev/null; }      # <plist> <set index> <entry key>
wsp_has(){ "$PB" -c "Print :DisplayAnyUserSets:$2" "$1" >/dev/null 2>&1; }
wsp_one(){ wsp_has "$1" "$2:0" && ! wsp_has "$1" "$2:1"; }                          # a one-display set
wsp_export(){ rm -f "$1"; defaults export "$WSPDOM" "$1" >/dev/null 2>&1 && [ -s "$1" ]; }
# v4 leftover (daemon 04c751b1 kept the user's one-display values in $WSPSTASH/<DisplayID>): put them back into the live
# one-display set of that display still at the neutral id 1, once; a stash file whose set is not found stays
wsp_restore(){ ls "$WSPSTASH"/* >/dev/null 2>&1 || return 0
  local cur="$WSPTMP/phxboot-wsprefs-r.$$.plist" j sf d f n=0 k v
  wsp_export "$cur" || { echo "wsprefs: restore ($1): cannot export $WSPDOM: values kept"; rm -f "$cur"; return 1; }
  for sf in "$WSPSTASH"/*; do d=${sf##*/}; case "$d" in *.new) continue;; esac; f=""; j=0
    while wsp_has "$cur" $j; do
      wsp_one "$cur" $j && [ "$(wsp_val "$cur" $j DisplayID)" = "$d" ] && [ "$(wsp_val "$cur" $j Mode:IODisplayModeID)" = 1 ] && { f=$j; break; }
      j=$((j+1)); done
    if [ -z "$f" ]; then echo "wsprefs: restore ($1): no live one-display set of display $d at id 1: left as is, values kept"; continue; fi
    while IFS=$'\t' read -r k v; do [ -n "$k" ] && "$PB" -c "Set :DisplayAnyUserSets:$f:0:$k $v" "$cur"; done <"$sf"
    n=$((n+1)); echo "$sf" >>"$cur.done"; done
  if [ $n -gt 0 ] && ! defaults import "$WSPDOM" "$cur"; then echo "wsprefs: restore ($1): import failed: values kept"; rm -f "$cur" "$cur.done"; return 1; fi
  [ -e "$cur.done" ] && while read -r sf; do rm -f "$sf"; done <"$cur.done"
  rmdir "$WSPSTASH" 2>/dev/null
  echo "wsprefs: $(date '+%T') restored $n set(s) ($1)"
  rm -f "$cur" "$cur.done"; sync; }
# the WindowServer pid (the first one is up since boot; a change before the restart = it died, usually with no .ips)
wspid(){ pgrep -x WindowServer 2>/dev/null | head -1; }
# head 0's offer gate (PhxAccel phxoffer.h): 0 = only the lit mode offered, 1 = every mode, "" = no such oid.
# PhxAMDFB registers debug.phxfb0_offer at the END of its start (the gate itself is decided before IOFramebuffer::start),
# seconds after `ioreg` already shows PhxAMDFB (an early probe read "absent"), so the gate is read and opened
# only right before the restart, after a wait for the oid; the write runs whatever a resync did (the kernel ignores a second
# opening) and must read back 1.
offer_state(){ sysctl -n debug.phxfb0_offer 2>/dev/null; }
offer_open(){ local st out i opens0
  for ((i=0; i<${PHXBOOT_OFFER_WAIT:-10}; i++)); do st=$(offer_state); [ -n "$st" ] && break; nap 1; done
  case "$st" in
    0|1) ;;
    *) echo "head-0 offer gate: absent after ${PHXBOOT_OFFER_WAIT:-10}s (PhxAccel without debug.phxfb0_offer: the first WindowServer may have restored a stored 144 set)"; return 0;; esac
  opens0=$(sysctl -n debug.phxfb0_offer_opens 2>/dev/null)
  out=$(sysctl -w debug.phxfb0_offer=1 2>&1)
  if [ "$(offer_state)" = 1 ]; then
    if [ "$st" = 0 ]; then echo "head-0 offer gate open (was closed; debug.phxfb0_offer=1, IOFBConfig dropped): the restarted WindowServer lists every head-0 mode"
    else echo "head-0 offer gate open (verified; already opened before the restart: ${opens0:-?} opening(s) by a resync, or -phxfb0open / no head-0 list)"; fi
  else echo "head-0 offer gate: debug.phxfb0_offer=1 FAILED (state ${st}, now $(offer_state); $(echo "$out" | tail -1 | cut -c1-100)): the restarted WindowServer lists only the lit mode"; fi; }
# rotation rot15 (11-03): a display override per connected display with
# default-resolution 0x0 @255 Hz, so CoreDisplay's default at a display set it never saw (a first rotation, a new cable
# layout) is the highest rate instead of the mode under 86 Hz; with it PhxAccel needs no verticalBlankingExtension rate
# marks (they made System Settings show "Variable (0-N Hertz)"). Flag ~/gpu/phxboot-dispovr; the tool only writes or
# refreshes files it wrote itself (key phx-override) and never touches another override.
# Never blocks the restart: a missing tool or a failed write is logged, and a tool still running
# after PHXBOOT_DISPOVR_WAIT s (10; it only reads ioreg and writes a few small files) is killed and the boot goes on.
DISPOVR=${PHXBOOT_DISPOVR:-$G/dispovr.sh}
disp_ovr(){ [ -e "$G/phxboot-dispovr" ] || return 0
  [ -x "$DISPOVR" ] || [ -r "$DISPOVR" ] || { echo "dispovr: $DISPOVR missing (flag set): no display overrides"; return 0; }
  local out="$WSPTMP/phxboot-dispovr.$$.log" p end rc
  rm -f "$out"; sh "$DISPOVR" >"$out" 2>&1 & p=$!
  end=$(( $(date +%s) + ${PHXBOOT_DISPOVR_WAIT:-10} ))   # wall clock (not a count of naps)
  while kill -0 $p 2>/dev/null && [ "$(date +%s)" -lt $end ]; do nap 1; done
  if kill -0 $p 2>/dev/null; then kill -9 $p 2>/dev/null; wait $p 2>/dev/null; sed 's/^/  /' "$out" 2>/dev/null
    echo "dispovr: $DISPOVR still running after ${PHXBOOT_DISPOVR_WAIT:-10}s: killed, the restart goes on"; rm -f "$out"; return 0; fi
  wait $p; rc=$?; sed 's/^/  /' "$out" 2>/dev/null; rm -f "$out"
  [ $rc = 0 ] || echo "dispovr: $DISPOVR exit $rc: some overrides not written, the restart goes on"; return 0; }
release(){ sysctl -w debug.phxfb_hold=0 >/dev/null 2>&1; }
apps_only(){ printf '# phxboot: Metal for test tools only\n' > "$ALLOW"; }
DENY="mediaanalysisd photoanalysisd Spotlight Dock"   # NotificationCenter allowed again 2026-10-26 (BIG_PAGE fix); iconservicesagent allowed 2026-10-26: its "?" icons were cold shader compiles, not broken rendering
all_apps(){ { printf '# phxboot: Metal for every app%s\n' "$1"; for d in $DENY; do printf -- '-%s\n' "$d"; done; printf '*\n'; [ -z "$2" ] || printf '%s\n' "$2"; } > "$ALLOW"; }
# revert to a safe GOP boot: drop the panel/arm, consume the one-shots, reboot.
# The clean-shutdown TERM trap is dropped first: the TERM of the reboot below must not run it (it would log a clean
# shutdown and re-arm phxfb-squat after a failure).
m2_fail(){ trap - TERM; echo "FAIL ($1): reverting to GOP"; b21_on "FAIL: the next boot is GOP"; touch "$FAILF"; apps_only; sysctl -w debug.phxagdc=0 >/dev/null 2>&1
           nvram -d phxfb-boot; nvram -d phxfb-squat; rm -f "$MARK" "$DESKTOP"; sync; shutdown -r now; exit 0; }
# Login-window guard (11-04): a WindowServer without Metal (the first
# one of every boot until the restart below, or the only one of a GOP boot) aborts in QuartzCore CA::OGL push_surface when
# loginwindow draws the Memoji lock-screen avatar. SHOWFULLNAME true (name + password fields) avoids the avatar. loginwindow
# starts ~1 s before this daemon, so the key must already be true when the box goes down: ON at a shutdown TERM, at every
# revert to GOP and at the daemon's start (that covers a loginwindow restarted after a crash this boot; after an unclean
# shutdown the first loginwindow still reads the session value). OFF only right before the Metal WindowServer restart, back
# to the value the user had (absent = delete, the user's own true stays true), kept in $B21ORIG between ON and OFF.
# ~/gpu/phxboot-b21guard holding 0 = the key is never touched. At a shutdown TERM `defaults` may not reach cfprefsd (the v4
# `defaults export` at TERM returned no sets 3/3), so the plist file is the fallback for both
# the read of the user's value and the write.
LWDOM=${PHXBOOT_LWDOM:-/Library/Preferences/com.apple.loginwindow}; B21ORIG=${PHXBOOT_B21ORIG:-$G/phxboot-b21guard.orig}
b21_ok(){ [ "$(tr -d ' \t\r\n' 2>/dev/null <"$G/phxboot-b21guard")" != 0 ]; }   # 2> first: a missing flag file prints nothing
b21_cfp(){ defaults read "$LWDOM" SHOWFULLNAME 2>/dev/null || echo absent; }               # what loginwindow reads
b21_file(){ "$PB" -c "Print :SHOWFULLNAME" "$LWDOM.plist" 2>/dev/null || echo absent; }    # the plist on disk
b21_on(){ b21_ok || return 0
  local was o rec=earlier rc f how=""
  was=$(b21_cfp)
  if [ -e "$B21ORIG" ]; then o=$(cat "$B21ORIG")
  else o=$was; [ "$o" = absent ] && { o=$(b21_file); case "$o" in true) o=1;; false) o=0;; esac; }
    echo "$o" >"$B21ORIG"; rec=now; fi
  defaults write "$LWDOM" SHOWFULLNAME -bool true >/dev/null 2>&1; rc=$?
  f=$(b21_file); [ "$f" = true ] || { "$PB" -c "Set :SHOWFULLNAME true" "$LWDOM.plist" >/dev/null 2>&1 || "$PB" -c "Add :SHOWFULLNAME bool true" "$LWDOM.plist" >/dev/null 2>&1
    how=", file was $f: written directly, now $(b21_file)"; }
  sync; echo "b21guard: $(date '+%T') ON ($1): SHOWFULLNAME was '$was', set true (defaults rc $rc$how); the user's value '$o' recorded $rec"; }
b21_off(){ b21_ok || return 0
  [ -e "$B21ORIG" ] || { echo "b21guard: $(date '+%T') OFF skipped: no recorded user value ($B21ORIG): SHOWFULLNAME stays '$(b21_cfp)'"; return 0; }
  local o rc=0 want now
  o=$(cat "$B21ORIG")
  case "$o" in
    absent) defaults delete "$LWDOM" SHOWFULLNAME >/dev/null 2>&1; rc=$?; want=absent;;
    1|true|YES|yes) want=1;;    # the user's own true: never removed
    *) defaults write "$LWDOM" SHOWFULLNAME -bool false >/dev/null 2>&1; rc=$?; want=0;;
  esac
  now=$(b21_cfp)
  if [ "$now" = "$want" ]; then rm -f "$B21ORIG"; sync
    echo "b21guard: $(date '+%T') OFF before the Metal WindowServer restart: SHOWFULLNAME back to the user's '$o' (rc $rc, reads '$now'): avatar on the accelerated login"
  else echo "b21guard: $(date '+%T') OFF FAILED: SHOWFULLNAME reads '$now', not the user's '$o' (rc $rc): name + password this login, the user's value kept in $B21ORIG"; fi; }
if b21_ok; then b21_on "daemon start"
else echo "b21guard: off (~/gpu/phxboot-b21guard = 0): SHOWFULLNAME not managed$([ -e "$B21ORIG" ] && echo " (a guard ON left by an earlier boot stays: the user's value '$(cat "$B21ORIG")' is in $B21ORIG)")"; fi
# Squat detection (11-02): poll until PhxFBSquatter (squat boot) or an
# IOFramebuffer (IONDRV/GOP boot) appears, up to 60 s. The old one-shot check ran before the squatter attached on fast
# boots (no -v: the daemon starts ~4 s after the kernel), read 0, and wait_fb then waited 90 s for a framebuffer the
# squatter blocks: the Apple logo stayed (22:46 / 22:58 11-02, misread as a lost NVRAM write).
SQUAT=0; SQWHY="neither in ${PHXBOOT_SQUAT_WAIT:-60}s"
for ((sq=0; sq<${PHXBOOT_SQUAT_WAIT:-60}; sq++)); do
  ioreg -rc PhxFBSquatter -d0 | grep -q PhxFBSquatter && { SQUAT=1; SQWHY="PhxFBSquatter after ${sq}s"; break; }
  ioreg -rc IOFramebuffer -d0 | grep -q 'class ' && { SQWHY="IOFramebuffer after ${sq}s"; break; }
  sleep 1
done
# GPU state at a hang, logged before any revert so the next boot can tell a GPU hang from a WindowServer-side stall
HANGDIR=${PHXBOOT_HANGDIR:-/var/log}; PTWALK=${PHXBOOT_PTWALK:-$G/ptwalk.sh}
# GPU state at a hang, kept in $HANGDIR/phxhang-<time>/ (survives the revert reboot; /tmp does not): GC status, the VM
# telemetry, a walk of the faulting VMID's page table to the faulting VA, and every process's plugin log
gpu_dump(){ local d="$HANGDIR/phxhang-$(date '+%Y%m%d-%H%M%S')"; mkdir -p "$d"
            echo "--- GPU state ($1) at $(date '+%T'): frames=$(sysctl -n debug.nvaccel_iop_ok 2>/dev/null) -> $d"
            "$PHXCTL" gc 2>&1 | head -20; sleep 1; echo "--- 1s later"; "$PHXCTL" gc 2>&1 | head -4
            { echo "$1"; date; sysctl -n debug.phxvm 2>&1; "$PHXCTL" regs 0x2004 0x2828 0x2829 0x282a 2>&1; } > "$d/state.txt"
            [ -f "$PTWALK" ] && bash "$PTWALK" > "$d/ptwalk.txt" 2>&1
            ps -axo pid,stat,comm > "$d/ps.txt" 2>&1
            find /tmp /private/var/folders -name 'nvmtl*.log' 2>/dev/null | while read -r f; do cp "$f" "$d/$(echo "$f" | tr '/' '_')" 2>/dev/null; done
            grep -E 'fault|VMID' "$d/state.txt" | head -12; }
# A sleep that a TERM ends at once. Bash runs a trap only after the foreground child exits, so a plain `sleep 5` held
# the TERM trap below for up to 5 s, the time launchd gives the job before SIGKILL at shutdown (its default ExitTimeOut);
# `wait` returns as soon as the trapped signal arrives.
nap(){ sleep "$1" & wait $!; }
# phxfb-squat for the next boot, logged with an NVRAM read-back (a lost write turned every later boot GOP on 11-02)
rearm(){ nvram phxfb-squat=1 || return 1; sync; if nvram phxfb-squat >/dev/null 2>&1; then echo "phxfb-squat re-armed for the next boot ($1; read-back ok)"; else echo "phxfb-squat re-armed for the next boot ($1; read-back FAILED)"; fi; }
wait_fb(){ for i in $(seq 1 90); do ioreg -rc IOFramebuffer -d0 | grep -q 'class ' && break; sleep 1; done; }

echo "squat boot (PhxFBSquatter holds IOFramebuffer, no IONDRV console): $SQUAT (decided by $SQWHY)"
# a squat boot has no display until we arm, so every early exit on one must reboot (back to IONDRV) instead
if [ -e "$OFF" ]; then
  echo "kill switch present"; release
  [ $SQUAT = 1 ] && { echo "squat boot with the kill switch on: rebooting onto IONDRV"; nvram -d phxfb-squat; sync; shutdown -r now; }
  exit 0
fi
if [ -e "$MARK" ]; then
  echo "previous attempt did not finish -> GOP (kill switch on, one-shots cleared)"; touch "$OFF"
  rm -f "$MARK" "$DESKTOP"; nvram -d phxfb-boot; nvram -d phxfb-squat; sync
  [ $SQUAT = 1 ] && { echo "squat boot: rebooting onto IONDRV"; shutdown -r now; }
  exit 0
fi

DESK=0; { [ -e "$DESKTOP" ] || [ $SQUAT = 1 ]; } && DESK=1
echo "desktop-arm: $DESK (phxboot-desktop file or squat boot)"
for i in $(seq 1 60); do ioreg -l | grep -q '"phx-build"' && break; sleep 1; done
[ $SQUAT = 1 ] || wait_fb    # a squat boot has no framebuffer until PhxAMDFB starts at claim
ioreg -l | grep -q '"phx-build"' || { [ $DESK = 1 ] && m2_fail "PhxBringup not loaded"; echo "PhxBringup not loaded"; exit 0; }

# bring-up + claim (needed for Metal either way)
# PhxAMDFB takes the panel at start only with phxfb-boot set (it clears it at once)
[ $DESK = 1 ] && nvram phxfb-boot=1
# PhxAccel lives in the AuxKC, which does not always load it at boot
kmutil showloaded 2>/dev/null | grep -q org.vermilion.accel || kmutil load -p "$ACCEL" 2>&1 | tail -2
echo "== bring-up + claim =="
UPOUT=$(bash "$G/phxctl/navi31_up.sh" 2>&1); echo "$UPOUT" | tail -4
# Idle power: clock gating and the UCLK pin right after the GPU is up, not only after `done`: without them the card sat at
# ~110 C hotspot / 260 W for the ~2 min of the boot; the late calls below re-apply them after
# vcn_boot.
# M7 (10-29): the second monitor at boot when $G/phxboot-m7 exists (m7_boot.sh = tools/phxctl/m7_boot.sh):
# light (pipe 1 stream) before the AGDC arm, bind (head 1 + plane + flip) after it, so the ONE WindowServer restart
# below already sees both displays. Any M7 failure is logged and undone; the boot goes on with one monitor.
M7LIT=0; M7PHASE=0; M7BOUND=0
m7_light(){ [ -e "$G/phxboot-m7" ] || return 0
  [ -e "$G/m7_boot.sh" ] || { echo "m7: $G/m7_boot.sh missing"; return 0; }
  sh "$G/m7_boot.sh" light; r=$?
  [ $r = 0 ] && { M7LIT=1; M7PHASE=1; }; [ $r = 3 ] && M7LIT=1; return 0; }
m7_bind(){ [ "$M7LIT" = 1 ] || return 0
  if sh "$G/m7_boot.sh" bind; then M7BOUND=1; else echo "m7: bind failed: one monitor this boot"; fi; return 0; }
# 11-01: two head-1 features validated live on 10-31/11-01, each
# behind its own flag, both called after `done` and the late SMU calls, before b15_boot starts. Only on a desktop boot
# whose head 1 came up (m7_bind exit 0): that is the state both live passes ran in. A refused write is logged, never fatal.
# Hot-plug: ~/gpu/phxboot-hotplug
# = debug.phxhotplug=2 (kext b102+ with PhxAccel hpd1+: the Acer's unplug/plug takes head 1 offline / online). The
# boot-arg -phxhotplugoff keeps it at 0 (the kext refuses the write too, EPERM).
hotplug_boot(){ [ -e "$G/phxboot-hotplug" ] || return 0
  [ $DESK = 1 ] || { echo "hotplug: not a desktop boot: debug.phxhotplug stays 0"; return 0; }
  case " $(sysctl -n kern.bootargs 2>/dev/null) " in
    *" -phxhotplugoff "*) echo "hotplug: boot-arg -phxhotplugoff: debug.phxhotplug stays 0"; return 0 ;; esac
  [ "$M7BOUND" = 1 ] || { echo "hotplug: head 1 not up this boot: debug.phxhotplug stays 0"; return 0; }
  out=$(sysctl -w debug.phxhotplug=2 2>&1); rc=$?
  echo "hotplug: $(date '+%T') debug.phxhotplug=2 rc $rc, reads $(sysctl -n debug.phxhotplug 2>/dev/null) ($(echo "$out" | tail -1 | cut -c1-100))"; }
# 11-04 head-0 hot-plug: ~/gpu/phxboot-hotplug0 =
# debug.phxhotplug0 after `done` (PhxAccel hp1+: the AOC's DP unplug / plug takes head 0 offline / online, the Acer is the
# main display meanwhile). The flag holds the level, 1 (observe) or 2 (act); empty = 1. Not on a GOP boot; boot-arg
# -phxhotplugoff keeps it at 0 (the kext refuses the write too). A PhxAccel without the oid is logged and nothing is armed.
hotplug0_boot(){ [ -e "$G/phxboot-hotplug0" ] || return 0
  [ $DESK = 1 ] || { echo "hotplug0: not a desktop boot: debug.phxhotplug0 stays 0"; return 0; }
  case " $(sysctl -n kern.bootargs 2>/dev/null) " in
    *" -phxhotplugoff "*) echo "hotplug0: boot-arg -phxhotplugoff: debug.phxhotplug0 stays 0"; return 0 ;; esac
  local lvl out rc
  lvl=$(tr -d ' \t\r\n' <"$G/phxboot-hotplug0" 2>/dev/null); [ -n "$lvl" ] || lvl=1
  case "$lvl" in 1|2) ;; *) echo "hotplug0: ~/gpu/phxboot-hotplug0 holds '$lvl', not 1 or 2: debug.phxhotplug0 stays 0"; return 0 ;; esac
  sysctl -n debug.phxhotplug0 >/dev/null 2>&1 || { echo "hotplug0: PhxAccel without debug.phxhotplug0 (hp1+): nothing armed"; return 0; }
  out=$(sysctl -w debug.phxhotplug0=$lvl 2>&1); rc=$?
  echo "hotplug0: $(date '+%T') debug.phxhotplug0=$lvl rc $rc, reads $(sysctl -n debug.phxhotplug0 2>/dev/null) ($(echo "$out" | tail -1 | cut -c1-100))"; }
# 11-03 multiport S3: ~/gpu/phxboot-dpwatch = debug.phxdpwatch after `done`
# (kext b120+: the read-only DP stream watcher of DP B / DP A / DP E; b121+: level 2 = also the DP B dark-AOC fix, Linux's
# DIG FIFO reset + stream re-enable on a STUCK, once per plug). The flag holds the level, 1 or 2 (empty = 1). Not on a GOP
# boot; boot-arg -phxdpwatchoff keeps it off (the kext refuses the write too). A kext without the oid is logged and nothing
# is armed; one that refuses level 2 (before b121) gets level 1. watch_hangs then logs every new STUCK / recovery / fix.
DPWATCH=0; DPWPREV=""
dpwatch_boot(){ [ -e "$G/phxboot-dpwatch" ] || return 0
  [ $DESK = 1 ] || { echo "dpwatch: not a desktop boot: debug.phxdpwatch stays 0"; return 0; }
  case " $(sysctl -n kern.bootargs 2>/dev/null) " in
    *" -phxdpwatchoff "*) echo "dpwatch: boot-arg -phxdpwatchoff: debug.phxdpwatch stays 0"; return 0 ;; esac
  local lvl out rc
  lvl=$(tr -d ' \t\r\n' <"$G/phxboot-dpwatch" 2>/dev/null); [ -n "$lvl" ] || lvl=1
  case "$lvl" in 1|2) ;; *) echo "dpwatch: ~/gpu/phxboot-dpwatch holds '$lvl', not 1 or 2: debug.phxdpwatch stays 0"; return 0 ;; esac
  sysctl -n debug.phxdpwatch >/dev/null 2>&1 || { echo "dpwatch: PhxBringup without debug.phxdpwatch (kext b120+): nothing armed"; return 0; }
  out=$(sysctl -w debug.phxdpwatch=$lvl 2>&1); rc=$?
  if [ $rc != 0 ] && [ "$lvl" = 2 ]; then
    echo "dpwatch: level 2 refused ($(echo "$out" | tail -1 | cut -c1-80)): a kext before b121? level 1"
    lvl=1; out=$(sysctl -w debug.phxdpwatch=1 2>&1); rc=$?
  fi
  [ $rc = 0 ] && DPWATCH=1
  echo "dpwatch: $(date '+%T') debug.phxdpwatch=$lvl rc $rc, reads $(sysctl -n debug.phxdpwatch 2>/dev/null) ($(echo "$out" | tail -1 | cut -c1-100))"; }
# One look at the watcher (called by watch_hangs every 15 s): a line for each counter that went up since the last look
# (per connector STUCK / recovered / ended by an unplug; DP B's fix ok / fail / refused), with the newest matching event.
dpwatch_check(){ [ "$DPWATCH" = 1 ] || return 0
  local s cur k c n p ev
  s=$(sysctl -n debug.phxdpwatch_stat 2>/dev/null) || return 0
  cur=$(printf '%s\n' "$s" | sed -n -e 's/^\(DP-[ABE]\): .* STUCK=\([0-9]*\) recovered=\([0-9]*\) stuck_ended_off=\([0-9]*\) .*/\1:STUCK=\2 \1:recovered=\3 \1:stream-off=\4/p' \
        -e 's/^DP-B fix (level 2): .* ok=\([0-9]*\) fail=\([0-9]*\) refused=\([0-9]*\) .*/DP-B:FIX-OK=\1 DP-B:FIX-FAIL=\2 DP-B:fix-refused=\3/p' | tr '\n' ' ')
  for k in $cur; do c=${k%%=*}; n=${k#*=}
    p=$(printf '%s\n' $DPWPREV | sed -n "s/^$c=//p"); [ -n "$p" ] || p=0
    [ "$n" -gt "$p" ] 2>/dev/null || continue
    ev=$(printf '%s\n' "$s" | grep -E "^ev .* ${c%%:*} ${c#*:} " | tail -1 | cut -c1-220)
    echo "dpwatch: $(date '+%F %T') ${c%%:*} ${c#*:} $p -> $n${ev:+ | $ev}"
  done
  DPWPREV=$cur; }
# Gamma: ~/gpu/phxboot-gamma = debug.phxgamma=1 (kext b98+: head-1 tables
# written to MPCC_OGAM1; the 0 -> 1 write applies the last table CoreDisplay sent). Head 1 gets tables only when CoreDisplay
# reads useIOP_gamma_onAMD = false: logged from ~/gpu/cdpref, never set here (a user/system setting).
# The kext answers EBUSY while the pipe-1 engine is held (a mode switch, a hot-plug action): up to 3 writes, 1 s apart.
CDPREF=${PHXBOOT_CDPREF:-$G/cdpref}
gamma_boot(){ [ -e "$G/phxboot-gamma" ] || return 0
  [ $DESK = 1 ] || { echo "gamma: not a desktop boot: debug.phxgamma stays 0"; return 0; }
  [ "$M7BOUND" = 1 ] || { echo "gamma: head 1 not up this boot: debug.phxgamma stays 0"; return 0; }
  if [ -x "$CDPREF" ]; then p=$("$CDPREF" get useIOP_gamma_onAMD 2>&1 | tail -1)
    case "$p" in *"useIOP_gamma_onAMD = false") echo "gamma: CoreDisplay useIOP_gamma_onAMD = false: head-1 tables reach the kext" ;;
      *) echo "gamma: CoreDisplay useIOP_gamma_onAMD is not false ($p): head 1 gets no tables (sudo ~/gpu/cdpref set useIOP_gamma_onAMD 0 + a WindowServer restart)" ;; esac
  else echo "gamma: $CDPREF missing: CoreDisplay useIOP_gamma_onAMD not checked"; fi
  t=0; while :; do t=$((t + 1)); out=$(sysctl -w debug.phxgamma=1 2>&1); rc=$?
    [ $rc != 0 ] && [ $t -lt 3 ] && echo "$out" | grep -qi busy || break; sleep 1; done
  echo "gamma: $(date '+%T') debug.phxgamma=1 rc $rc after $t write(s), reads $(sysctl -n debug.phxgamma 2>/dev/null) ($(echo "$out" | tail -1 | cut -c1-100))"; }
# Boot rate (11-02): ~/gpu/phxboot-aoc-rate = 144 (or "last" = the aoc= rate b15_boot.sh
# remembered in ~/gpu/phxboot-rate-last, 144 when none) puts the AOC at 144 BEFORE the one WindowServer restart, so the
# restarted WindowServer starts with the AOC at 144 and nothing switches after the login: b15_boot.sh aocboot (the 60
# recipe's pipe-0 part + the kext's macOS sequence to 144 + aoc144), then PhxAccel head 0 takes OTG0's new totals as its
# current mode (debug.phxfb_resync=1; PhxAccel boot-rate build). Only when head 1 came up phase-aligned (pipe 1 at 144 or 165
# with DET 9/9 is aocboot's own check), the head-0 mode list is armed and b15 will follow (phxboot-b15 + -b15-2pipe). Any
# refusal = the AOC stays at 60 as today (b15 then asks 144 after the login as before); a resync that does not report id 2
# = the engine back to 60 (b15_boot.sh aocback). Never fatal for the boot.
AOCRATE=${PHXBOOT_AOCRATE:-$G/phxboot-aoc-rate}; RATELAST=${PHXBOOT_RATELAST:-$G/phxboot-rate-last}
aoc_boot(){ [ -e "$AOCRATE" ] || return 0
  local want src out rc id
  want=$(tr -d ' \t\r\n' < "$AOCRATE"); src="$AOCRATE"
  if [ "$want" = last ]; then want=$(sed -n 's/.*aoc=\([0-9][0-9]*\).*/\1/p' "$RATELAST" 2>/dev/null | head -1); src="remembered in $RATELAST"
    case "$want" in 60|144) ;; *) want=144; src="no remembered rate in $RATELAST" ;; esac; fi
  [ "$want" = 144 ] || { echo "aocboot: boot rate '$want' ($src): the AOC stays at 60"; return 0; }
  { [ -e "$G/phxboot-m7-aocmodes" ] || [ -e "$G/phxboot-m7-aoc144" ]; } || { echo "aocboot: no head-0 mode list flag (phxboot-m7-aocmodes): the AOC stays at 60"; return 0; }
  # two displays (head 1 up and phase-aligned) = aocboot next to the Acer; the AOC alone (pipe 1 never lit this boot: no
  # Acer) = aocboot1 (11-03 p0solo: needs ~/gpu/phxboot-m7 for
  # the DAL hard mins and PhxBringup's debug.phxpipe0_solo); pipe 1 lit but not bound / not aligned = no 144 (as before)
  local cmd alone=""
  if [ "$M7BOUND" = 1 ] && [ "$M7PHASE" = 1 ]; then cmd=aocboot
    { [ -e "$G/phxboot-b15" ] && [ -e "$G/phxboot-b15-2pipe" ]; } || { echo "aocboot: b15 two-display flags missing (phxboot-b15, -b15-2pipe): the AOC stays at 60"; return 0; }
  elif [ "$M7LIT" = 0 ]; then cmd=aocboot1; alone=" (alone)"
    [ -e "$G/phxboot-b15" ] || { echo "aocboot: one display: no ~/gpu/phxboot-b15: the AOC stays at 60"; return 0; }
    [ -e "$G/phxboot-m7" ] || { echo "aocboot: one display: no ~/gpu/phxboot-m7 (its rate setup sends the DAL hard mins the 144 set needs): the AOC stays at 60"; return 0; }
    sysctl -n debug.phxpipe0_solo >/dev/null 2>&1 || { echo "aocboot: one display: PhxBringup without debug.phxpipe0_solo (kernel-p0solo.patch): the AOC stays at 60"; return 0; }
  else echo "aocboot: head 1 not up and phase-aligned this boot: the AOC stays at 60"; return 0; fi
  [ -e "$G/b15_boot.sh" ] || { echo "aocboot: $G/b15_boot.sh missing: the AOC stays at 60"; return 0; }
  sysctl -n debug.phxfb_resync >/dev/null 2>&1 || { echo "aocboot: PhxAccel without debug.phxfb_resync: the AOC stays at 60"; return 0; }
  [ "$(sysctl -n debug.phxfb_nmodes 2>/dev/null)" = 2 ] || { echo "aocboot: head 0 has no 60/144 mode list (debug.phxfb_nmodes $(sysctl -n debug.phxfb_nmodes 2>/dev/null)): the AOC stays at 60"; return 0; }
  echo "aocboot: $(date '+%T') boot rate 144 ($src): the AOC$alone to 144 before the WindowServer restart"
  out=$(sh "$G/b15_boot.sh" $cmd 2>&1); rc=$?; printf '%s\n' "$out"
  [ $rc = 0 ] || { echo "aocboot: b15_boot.sh $cmd rc $rc: the AOC stays at 60"; return 0; }
  sysctl -w debug.phxfb_resync=1 >/dev/null 2>&1; id=$(sysctl -n debug.phxfb_resync 2>/dev/null)
  if [ "$id" = 2 ]; then echo "aocboot: $(date '+%T') head 0 resynced: id 2, VBL $(sysctl -n debug.phxfb_refresh 2>/dev/null) mHz"; return 0; fi
  echo "aocboot: head 0 resync answered '$id', not id 2: the AOC back to 60"
  sh "$G/b15_boot.sh" aocback 2>&1; return 0; }
cg_boot(){
  [ -e "$G/phxboot-cg" ] || return 0
  [ -e "$G/gfx_cg.sh" ] || { echo "cg: $G/gfx_cg.sh missing"; return 0; }
  echo "cg: $(PHXCTL=$G/phxctl.bin sh "$G/gfx_cg.sh" on 2>&1 | tail -1 | cut -c1-120)"; }
# Memory clock hard minimum (MHz in $G/phxboot-uclk, e.g. 456): without the DAL hand-off a UCLK switch under GPU load
# underflows the display (glitches under video decode); needs kext >= b71
uclk_boot(){
  [ -e "$G/phxboot-uclk" ] || return 0
  [ -e "$G/uclk.sh" ] || { echo "uclk: $G/uclk.sh missing"; return 0; }
  echo "uclk: $(PHXCTL=$G/phxctl.bin sh "$G/uclk.sh" pin "$(cat "$G/phxboot-uclk")" 2>&1 | tail -1 | cut -c1-120)"; }
# Idle power root cause: the RLC grants CGCG only once both SDMA engines report idle; never-started engines stay "busy" and
# the card idles at ~150 W with PMFW activity 100 %. Start their queue 0 idle, before cg_boot.
sdma_boot(){
  [ -e "$G/phxboot-sdma" ] || return 0
  [ -e "$G/sdma_idle.sh" ] || { echo "sdma: $G/sdma_idle.sh missing"; return 0; }
  echo "sdma: $(PHXCTL=$G/phxctl.bin sh "$G/sdma_idle.sh" 2>&1 | tail -1 | cut -c1-100)"; }
if echo "$UPOUT" | grep -q "navi31 up:"; then   # only on a GPU that came up (gfx_cg.sh polls the RLC)
  [ "${PHXBOOT_NO_SDMA:-0}" = 1 ] || sdma_boot
  [ "${PHXBOOT_NO_CG:-0}" = 1 ] || cg_boot
  [ "${PHXBOOT_NO_UCLK:-0}" = 1 ] || uclk_boot
fi
# AOC at 144 (10-31): with $G/phxboot-m7-aocmodes or -aoc144, PhxAMDFB head 0
# lists the AOC's 60 and 144 Hz modes. It reads the kext's phx-pipe0-modes once, when it starts at the claim below, so the
# kext flag goes up first (kext b101+; an older kext has no such sysctl: one mode as before). The switch itself stays
# refused until b15_boot.sh arms the pipe-0 engine after `done`.
aoc_modes(){ { [ -e "$G/phxboot-m7-aocmodes" ] || [ -e "$G/phxboot-m7-aoc144" ]; } || return 0
  if sysctl -w debug.phxpipe0_modes=1 >/dev/null 2>&1; then echo "aoc: head-0 mode list armed (debug.phxpipe0_modes=1)"
  else echo "aoc: no debug.phxpipe0_modes (kext < b101): head 0 keeps one mode"; fi; }
[ "${PHXBOOT_NO_AOC:-0}" = 1 ] || aoc_modes
WS0=$(wspid)                 # the first WindowServer (up since boot; it read com.apple.windowserver before this daemon ran)
"$MB" claim 2>&1
sysctl -w debug.phxbos_flush=0 >/dev/null 2>&1
for i in $(seq 1 20); do ioreg -rc NMXAccel -d0 | grep -q NMXAccel && break; sleep 1; done
if ! ioreg -rc NMXAccel -d0 | grep -q NMXAccel; then
  [ $DESK = 1 ] && m2_fail "NMXAccel did not start"
  echo "NMXAccel did not start (apps-only this boot)"; release; exit 0
fi
echo "NMXAccel up"

if [ $DESK = 1 ]; then
  rm -f "$DESKTOP"            # one-shot: exactly one desktop attempt per arm
  touch "$MARK"; sync         # in-progress: a hang before we finish -> next boot reverts
  # a clean restart or shutdown (launchd sends TERM; a hang never does) is not a failed attempt: clear the mark and
  # re-arm the next boot as SUCCESS would, so a restart from the login window during the watch keeps acceleration
  trap 'echo "clean shutdown (TERM) at $(date "+%T") during the desktop attempt: in-progress mark cleared"; rm -f "$MARK"
        [ -e "$AUTO" ] && nvram phxfb-squat=1 && echo "phxfb-squat re-armed for the next boot (phxboot-auto present)"; sync
        b21_on "shutdown (TERM) during the desktop attempt"; exit 0' TERM
  # 1) PhxAMDFB should already be on the panel (took it at claim via nmxgpu-claimed match).
  pub=0
  for i in $(seq 1 30); do ioreg -rc PhxAMDFB -d0 | grep -q PhxAMDFB && { pub=1; echo "PhxAMDFB on panel after ${i}s"; break; }; nap 1; done
  [ $pub = 1 ] || m2_fail "PhxAMDFB never took the panel (nmxgpu-claimed match)"
  # 2) confirm a LIVE Metal device (clean probe; metaltest logs via NSLog=stderr so merge 2>&1).
  echo "waiting for a live Metal device (clean probe)..."
  mtlok=0
  for i in $(seq 1 40); do
    $RUNAS "$G/metaltest" 2>&1 | grep -q 'Metal devices: 1' && { mtlok=1; echo "Metal device ready after ${i}s"; break; }
    nap 1
  done
  [ $mtlok = 1 ] || m2_fail "Metal device never came up"
  # 3) WindowServer may use Metal + set the Core Image backstop NOW (the restarted WindowServer inherits it).
  all_apps "; WindowServer composites on the GPU (Metal allowed)" ''
  release   # harmless no-op unless -phxfbhold put the hold back
  launchctl setenv CI_USE_MTL_DAG_FOR_CIKL_SRC 0 2>/dev/null   # Core Image backstop (Cobalt64 app-crash-study)
  # 3b) a user must NOT be logged in -- check BEFORE arming, so if someone is in
  #     we touch nothing (no arm, no pipe) and revert clean.
  if who 2>/dev/null | grep -q . ; then m2_fail "a user is logged in before arming -- refusing (log out / stay on the login screen)"; fi
  # (the 11-02 scdclog boot hook was removed 11-03: its data is in)
  m7_light
  # 4) arm the display pipe now that the panel is ours and Metal is ready.
  #    same order as the hand-run test that reached the desktop (2026-10-24 17:55)
  # 10-29: debug.phxagdc exists only once PhxAMDFB::start has finished
  # (phxAgdcInstall), and "on panel" above matches while start() still runs: a write before that is lost. Wait for the
  # oid; the poll below writes 1 again while it reads 0.
  for i in $(seq 1 15); do sysctl -n debug.phxagdc >/dev/null 2>&1 && break; nap 1; done
  sysctl -w debug.phxagdc=1 >/dev/null; nap 1
  sysctl -w debug.nvaccel_iop=1 >/dev/null
  sysctl -w debug.nvaccelfb=1 >/dev/null; nap 1
  sysctl -w debug.nvaccelfb=3 >/dev/null; nap 2
  # 4b) AGDC arms ASYNC (thread_call sets gState: 1 ok, <0 fail e.g. -6). Blindly
  #     sleeping then restarting could killall before AGDC is up, or after it silently failed -> WindowServer
  #     comes back in SOFTWARE compositing and we'd falsely print SUCCESS. Poll the readback instead.
  agdc=""
  for i in $(seq 1 15); do
    agdc=$(sysctl -n debug.phxagdc 2>/dev/null || echo "")
    [ "$agdc" = 1 ] && { echo "AGDC up after ${i}s"; break; }
    [ "$agdc" = 0 ] && sysctl -w debug.phxagdc=1 >/dev/null 2>&1
    case "$agdc" in -[0-9]*) m2_fail "AGDC failed to arm (debug.phxagdc=$agdc)";; esac
    nap 1
  done
  [ "$agdc" = 1 ] || m2_fail "AGDC never armed (debug.phxagdc='$agdc' after 15s)"
  echo "armed at $(date '+%T'); agdc=$agdc"
  m7_bind
  aoc_boot   # boot rate: the AOC at 144 before the restart (flag ~/gpu/phxboot-aoc-rate)
  # 5) ONE clean WindowServer restart. Metal caches an empty device list per process if asked too early, so
  #    WindowServer -- up since boot -- never sees our device until it restarts fresh. Only at 0 interactive users.
  #    final 0-users guard IMMEDIATELY before the killall, nothing slow between arm-confirm and kill, so the
  #    window where the stale-cache boot WindowServer could hit the assert on its own is as small as possible.
  if who 2>/dev/null | grep -q . ; then m2_fail "a user logged in during arm -- refusing the WindowServer restart"; fi
  # 5a) GPU present (2026-10-25): with $GPUPRESENT the plugin copies WindowServer's frames into VRAM slots on the GPU
  #     and flips HUBP0 itself (PhxBringup scanout, debug.phxscan); it needs the scanout allowed before WindowServer
  #     comes back, and phxfb's own flip left off (both would program HUBP0)
  GPUPRESENT=${PHXBOOT_GPUPRESENT:-/Library/GPUBundles/nvmtl/ws-gpu-present}
  [ -e "$GPUPRESENT" ] && { sysctl -w debug.phxscan=1 >/dev/null 2>&1 && echo "gpu present allowed (debug.phxscan=1)"; }
  WS1=$(wspid)
  if [ -n "$WS0" ] && [ "$WS1" != "$WS0" ]; then echo "first WindowServer died before the restart (pid $WS0 -> ${WS1:-none}; usually no .ips)"
  else echo "first WindowServer survived to the restart (pid ${WS0:-none})"; fi
  wsp_restore "v4 leftover, before the WindowServer restart"   # once: a v4 (04c751b1) stash goes back to the user's values
  [ -e "$WSPFLAG" ] && echo "wsprefs: $WSPFLAG is no longer used (the kernel offer gate replaced it; rm it)"
  offer_open   # every head-0 mode for the restarted WindowServer
  disp_ovr     # rotation rot15: the highest rate is the default at a new display set (read by the restarted WindowServer)
  b21_off      # login-window guard: the user's SHOWFULLNAME for the accelerated loginwindow the restart brings up
  echo "restarting WindowServer once (Metal ready, AGDC up) at $(date '+%T')"
  killall -9 WindowServer 2>/dev/null
  nap 6
  # 5b) the DCN A/B flip (validated 2026-10-24: 0 timeouts in >20k flips) inside the crash watch below;
  #     $G/phxboot-noflip keeps the copy into the scanned surface; GPU present replaces it
  #     with GPU present the display pipe's copy into A is skipped while HUBP0 scans a slot (PhxAccel with
  #     debug.nvaccel_iop_skipforeign; an older PhxAccel has no such sysctl and keeps copying)
  if [ -e "$GPUPRESENT" ]; then echo "phxfb flip left off (GPU present owns HUBP0)"
    sysctl -w debug.nvaccel_iop_skipforeign=1 >/dev/null 2>&1 && echo "display pipe copy skipped while GPU present scans (debug.nvaccel_iop_skipforeign=1)"
  elif [ ! -e "$G/phxboot-noflip" ]; then sysctl -w debug.phxfb_flip=1 >/dev/null 2>&1 && echo "flip on (debug.phxfb_flip=1)"; fi
  before=$(ls "$DR" 2>/dev/null | grep -c '^WindowServer[-_]')   # baseline AFTER the clean restart: earlier crashes don't count
  # 11-03: a WindowServer crash often writes NO .ips (12 corpses, 0 files), so a WindowServer
  # pid change also counts (0 users here: no logout restarts it); the verdict uses the larger of the two counts
  wsr=$(wspid); died=0
  echo "watching 120s for crashes with Metal ready (baseline $before .ips, WindowServer pid ${wsr:-none})..."
  for ((t=0;t<120;t+=5)); do nap 5; n=$(( $(ls "$DR" 2>/dev/null | grep -c '^WindowServer[-_]') - before ))
    p=$(wspid); if [ -n "$p" ] && [ "$p" != "$wsr" ]; then died=$((died+1)); echo "watch: WindowServer pid ${wsr:-none} -> $p at $(date '+%T') (a silent crash counts too)"; wsr=$p; fi
    [ $died -gt $n ] && n=$died
    [ $n -ge 3 ] && { gpu_dump "WindowServer crashed/hung $n times"; m2_fail "WindowServer crashed/hung $n times after the clean restart"; }; done
  pgrep -qx WindowServer || m2_fail "no WindowServer after the restart"
  echo "SUCCESS at $(date '+%T'): WindowServer pid $(pgrep -x WindowServer), frames=$(sysctl -n debug.nvaccel_iop_ok 2>/dev/null) -- accelerated desktop survived"
  # the session's shutdown TERM: login-window guard ON for the next boot's first loginwindow, then die of the TERM as before
  trap 'b21_on "shutdown (TERM)"; trap - TERM; kill -TERM $$' TERM; rm -f "$MARK"
  rm -f "$FAILF"
  if [ -e "$AUTO" ]; then rearm "phxboot-auto present"; fi
  sync
else
  release; all_apps " (WindowServer stays on firmware framebuffer)" ""
  echo "no desktop arm: GOP + Metal for apps"
  # a GOP boot with phxboot-auto (no kill switch, no unfinished MARK: both exit earlier) re-arms the next boot unless a
  # failure marker says the last accelerated attempt failed (11-02: one lost NVRAM write kept every boot GOP)
  if [ -e "$AUTO" ]; then
    if [ -e "$FAILF" ]; then echo "phxfb-squat NOT re-armed: the last accelerated attempt failed ($FAILF; rm it to resume)"
    else rearm "GOP boot, phxboot-auto present"; fi
  fi
fi
echo "done $(date '+%T')"

# WP6 (10-27): with $G/phxboot-vcn present, after "done": bring VCN0 up (vcn_up.sh, its own log in $G/vcnup/boot.log),
# let the kext adopt its ring (debug.phxvcn=1) and turn MMHUB VM on (debug.phxmmvm=1) so VCN can decode for apps.
# Off by default; a failure only leaves VCN off (the desktop does not depend on it).
vcn_boot(){
  [ -e "$G/phxboot-vcn" ] || return 0
  [ -x "$G/vcnup/vcn_up.sh" ] || { echo "vcn: $G/vcnup/vcn_up.sh missing"; return 0; }
  if bash "$G/vcnup/vcn_up.sh" --run > "$G/vcnup/boot.log" 2>&1; then
    sysctl -w debug.phxvcn=1 >/dev/null 2>&1 && sysctl -w debug.phxmmvm=1 >/dev/null 2>&1 \
      && echo "vcn: up, ring adopted, MMHUB VM on ($(sysctl -n debug.phxmmvm_stat 2>/dev/null | cut -c1-40))" \
      || echo "vcn: up, but adopt / MMHUB VM failed"
  else
    echo "vcn: bring-up failed ($(tail -1 "$G/vcnup/boot.log")): VCN stays off"
  fi; }
# 10-30: with pipe 1 lit at boot, the first SMU message after it
# (PowerUpVcn, or the late UCLK pin) stalls the PMFW message handler for the rest of the boot. Until that is understood,
# a boot that lit pipe 1 sends no SMU message after `done`: no VCN bring-up, no late UCLK pin (the early pin stays).
# Since then m7_boot.sh aligns OTG1's vblank on OTG0's (exit 0); only an unaligned pipe 1 skips.
if [ "$M7LIT" = 1 ] && [ "$M7PHASE" != 1 ]; then
  [ -e "$G/phxboot-vcn" ] && echo "vcn: skipped: pipe 1 lit at boot (an SMU message then stalls the PMFW)"
else
  [ "${PHXBOOT_NO_VCN:-0}" = 1 ] || vcn_boot
fi

# 10-28: with $G/phxboot-cg present, after "done": GFX clock gating as Linux enables it (gfx_cg.sh on). Without it the
# idle card runs ~250 W at a 110 C hotspot with the fans at full.
[ "${PHXBOOT_NO_CG:-0}" = 1 ] || cg_boot
if [ "$M7LIT" = 1 ] && [ "$M7PHASE" != 1 ]; then
  [ -e "$G/phxboot-uclk" ] && echo "uclk: late pin skipped: pipe 1 lit at boot (an SMU message then stalls the PMFW)"
else
  [ "${PHXBOOT_NO_UCLK:-0}" = 1 ] || uclk_boot
fi
# 11-01: hot-plug and gamma (defined next to m7_bind): after the one WindowServer restart and its crash watch, so neither
# can blur that verdict; before b15_boot starts, so these one-time writes never overlap its recipe.
hotplug_boot
hotplug0_boot   # 11-04: the AOC's hot-plug level (flag ~/gpu/phxboot-hotplug0), after the Acer's
gamma_boot
dpwatch_boot   # 11-03 S3: after hotplug / gamma, before b15 (flag ~/gpu/phxboot-dpwatch)

# 10-29: with $G/phxboot-b15 present, on a desktop boot after the late cg/uclk calls: the display power recipe
# (watermarks, DET, DAL WM table, HUBP0 DLG) then stutter allowed, with an underflow watchdog that stays in the background
# for the whole boot (b15_boot.sh). Its output goes to this log.
b15_boot(){
  [ -e "$G/phxboot-b15" ] || return 0
  [ $DESK = 1 ] || { echo "b15: not a desktop boot: skip"; return 0; }
  [ -e "$G/b15_boot.sh" ] || { echo "b15: $G/b15_boot.sh missing"; return 0; }
  sh "$G/b15_boot.sh" & echo "b15: started (pid $!)"; }
[ "${PHXBOOT_NO_B15:-0}" = 1 ] || b15_boot

# 11-03: a WindowServer pid change counts as a crash only with the kernel's "WindowServer[pid] Corpse allowed" line for the
# old pid (a logout also restarts WindowServer, cleanly); .ips files alone missed silent crashes
watch_hangs(){ local base n now first=0 wsp p c silent=0 it=0
  base=$(ls "$DR" 2>/dev/null | grep -c '^WindowServer-.*\.ips$'); wsp=$(wspid)
  while nap 15; do
    it=$((it+1)); [ -n "${PHXBOOT_WATCH_MAX:-}" ] && [ $it -gt "$PHXBOOT_WATCH_MAX" ] && return 0
    dpwatch_check   # 11-03 S3: the DP stream watcher's new STUCK / recovery / fix events into this log
    p=$(wspid)
    if [ -n "$p" ] && [ -n "$wsp" ] && [ "$p" != "$wsp" ]; then
      c=$("$LOGCMD" show --last 60s --style compact --predicate 'eventMessage CONTAINS "Corpse"' 2>/dev/null | grep -c "WindowServer\[$wsp\] Corpse")
      if [ "${c:-0}" -gt 0 ]; then silent=$((silent+1)); echo "$(date '+%F %T') WindowServer pid $wsp crashed (kernel corpse; now pid $p)"
      else echo "$(date '+%F %T') WindowServer pid $wsp -> $p with no kernel corpse (a logout or a clean restart): not counted"; fi
    fi; [ -n "$p" ] && wsp=$p
    n=$(( $(ls "$DR" 2>/dev/null | grep -c '^WindowServer-.*\.ips$') - base )); [ $silent -gt $n ] && n=$silent
    n=$((n+base)); [ $n -gt $base ] || continue
    now=$(date +%s); [ $first = 0 ] && first=$now
    if [ $((n-base)) -ge 2 ]; then
      echo "$(date '+%F %T') WindowServer hung $((n-base))x on GPU: reverting to GOP"; b21_on "revert to GOP after the hangs"; gpu_dump "WindowServer hung $((n-base))x"; touch "$FAILF"; nvram -d phxfb-boot; nvram -d phxfb-squat; rm -f "$DESKTOP"; sync; shutdown -r now; return
    fi
    [ $((now-first)) -gt 300 ] && { base=$(ls "$DR" 2>/dev/null | grep -c '^WindowServer-.*\.ips$'); silent=0; first=0; }
  done; }
[ "${PHXBOOT_NO_WATCH:-0}" = 1 ] || { [ $DESK = 1 ] && watch_hangs; }
