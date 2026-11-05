#!/bin/sh
# SPDX-License-Identifier: MIT
# m7_boot.sh (M7 boot bring-up, 10-29): the
# second monitor at boot, run by the boot daemon (phxboot-navi31.sh m7_light / m7_bind) when ~/gpu/phxboot-m7
# exists. The same steps as the hand-run test, split around the AGDC arm:
#   m7_boot.sh light   before the AGDC arm: debug.phxpipe2 1 survey -> 2 ping -> 3 stream (OTG1 + DIG3 + PHY D lit,
#                      DPG colour; pipe 1 only). Refused survey (e.g. no second monitor) = skip, nothing written.
#                      Then m7_phase.sh puts OTG1's vblank on OTG0's. Exit 0 = lit and
#                      aligned, 3 = lit but not aligned (the daemon then sends no SMU message after done), 1 = not lit.
#   m7_boot.sh bind    after the AGDC arm, before the daemon's ONE WindowServer restart: debug.phxhead2=1 (head 1 up,
#                      state 1, registered) -> debug.phxpipe2=4 plane (HUBP1 scans head 1) -> debug.phxfb1_flip=1. The
#                      restarted WindowServer then sees both displays (no second restart).
#   m7_boot.sh undo    best effort: flip 0, head 2 0, pipe 1 undo (debug.phxpipe2=0).
# S4b T4 (10-29): head 1's size, from the file ~/gpu/phxboot-m7-head1
# (M7_HEAD1 overrides): absent = 1680x1050 (today); "plan" = T4a, the plane at 1920x1080 (kext b87 debug.phxpipe2_head1=1)
# with head 1 still 1680x1050; "1080" = T4b, head 1 AND the plane at 1920x1080, only when the plugin gates head 0
# (/Library/GPUBundles/nvmtl/ws-headmap is enforce) and PhxAccel publishes debug.phxsurfmap; otherwise 1680x1050.
# Pipe-1 rate (10-30): ~/gpu/phxboot-m7-rate = 144 (M7_RATE
# overrides) -> before the survey, the two DAL hard mins (SetHardMinByFreq DISPCLK 331 and DPPCLK 331) and
# debug.phxpipe2_mode=144 (kext b90+); any refusal = the 60 Hz plan. Absent = 60 Hz.
# Boot rate (11-02): 165 = the DAL hard mins of the 165 plan (DISPCLK/DPPCLK 391 MHz,
# Linux c1) + debug.phxpipe2_mode=165 (kext b108+: the ladder runs the 165 plan, its SCDC steps included, host-tested);
# a refused 165 setup, or a 165 ladder that does not light pipe 1, = one more try with the 144 setup (after a clean
# undo). "last" = the Acer's rate b15_boot.sh last saw stable (~/gpu/phxboot-rate-last "acer=<60|144|165>"; M7_LAST
# overrides the file), 144 when there is none. Any other content = 60 Hz as before.
# FS-2 (11-02, from an SCDC log of the boot): the Acer drops HPD ~0.6 s after its first
# stream and clears its SCDC (also at 144), so a boot-time 165 (scrambled) never locks. So 165 now lights at 144 (`light`),
# and `bind`, after the flip, waits up to 10 s for the sink's SCDC lock on 2 reads 1 s apart, then switches pipe 1 to 165
# through debug.phxpipe2_switch (the proven live path: TX off, SCDC, TX on; pipe 0 is still at 60) and checks the scrambled
# lock within 3 s, else switches back to 144. ~/gpu/phxboot-m7-direct165 (M7_DIRECT165=1) = the old direct 165 light.
# C3 (11-02, the fix for the Acer dark after a cold boot): ON by
# default (the load is harmless on an already-loaded SRAM); ~/gpu/phxboot-c3-off (M7_C3=0 overrides) turns it off.
# `light` first writes debug.phxc3=9 (kext b114+: PHY D's SRAM load through the GOP firmware's DMUB REG_SEQ commands), before the rate
# setup and the survey, so a power-on chain (the GOP loaded only its boot display's PHY) can light the Acer. The kext's
# own gates decide (a refusal writes nothing: e.g. PHY D already programmed by the GOP); any result only logs, the ladder
# then runs as before (a failed load leaves the DMUB dead and the ladder refuses = one monitor).
# Any failure: undo, exit 1; the daemon logs it and the boot goes on with one monitor (M7 never fails a boot).
# A pipe-0 halt or a DMUB timeout latches in the kext (debug.phxpipe2_stat): the ladder then refuses every level.
# Env for the mock (tests/mock/m7test.sh): SYSCTL, SLEEP, M7_WAIT (state polls of 0.25 s, default 40).
set -u
SYSCTL=${SYSCTL:-/usr/sbin/sysctl}; SLEEP=${SLEEP:-sleep}; WAITN=${M7_WAIT:-40}
PHASE=${M7_PHASE:-sh $(dirname "$0")/m7_phase.sh}
PHXCTL=${M7_PHXCTL:-$(cd "$(dirname "$0")" && pwd)/phxctl.bin}
RATE=${M7_RATE-$(cat "$(dirname "$0")/phxboot-m7-rate" 2>/dev/null)}; RATE=$(printf %s "$RATE" | tr -d ' \t\r\n')
LASTF=${M7_LAST-$(dirname "$0")/phxboot-rate-last}
DIRECT165=${M7_DIRECT165-$([ -e "$(dirname "$0")/phxboot-m7-direct165" ] && echo 1)}
rd() { $PHXCTL regs "$1" 2>/dev/null | sed -n 's/^0x[0-9a-f]*: \(0x[0-9a-f]*\)$/\1/p'; }
wrr() { $PHXCTL wreg "$1" "$2" >/dev/null 2>&1; }
# DET 9/9/0/0 (Linux's two-display split): at 144 Hz the 114 us vblank cannot hide the
# ~413 us DRAM p-state switch, DET 3 stalls every PMFW main-loop message, DET 9 does not. After the plane only (the plan
# requires DET1 = 3 up to it); DET2/3 -> 0 only while HUBP2/3 clocks are off. CRB budget: 9 + 9 + COMPBUF 2 = 20.
det99() {
  for a in 0x3c6c 0x3d48; do c=$(rd $a); [ -n "$c" ] && [ $(( c & 1 )) -eq 0 ] || { echo "m7: DET: HUBP clock $a = ${c:-?}: kept"; return 1; }; done
  for p in "0x397d 0" "0x397e 0" "0x397b 9" "0x397c 9"; do
    set -- $p; wrr "$1" "$2"; i=0
    while [ $i -lt 20 ]; do c=$(rd "$1"); [ -n "$c" ] && [ $(( (c >> 8) & 0x1f )) -eq "$2" ] && break; $SLEEP 0.05; i=$((i + 1)); done
    [ $i -lt 20 ] || { echo "m7: DET $1 -> $2 never current (${c:-?}): stop"; return 1; }
  done
  c=$(rd 0x397a); [ $(( c >> 31 )) -eq 0 ] || { echo "m7: COMPBUF config error ($c)"; return 1; }
  echo "m7: $(date '+%T') DET 9/9/0/0 (Linux two-display split)"; }
dal_min() {   # dal_min <arg>: one DAL SetHardMinByFreq (0x10000 | 9), OK = the PMFW answered 0x01
  o=$($PHXCTL smu 0x10009 "$1" 2>&1); case "$o" in *"(OK)"*) return 0 ;; *) echo "m7: DAL SetHardMinByFreq $1: $o"; return 1 ;; esac; }
C3=${M7_C3-$([ -e "$(dirname "$0")/phxboot-c3-off" ] && echo 0 || echo 1)}
HM=${M7_HEADMAP:-/Library/GPUBundles/nvmtl/ws-headmap}
H1=${M7_HEAD1-$(cat "$(dirname "$0")/phxboot-m7-head1" 2>/dev/null)}
# Mode switch (10-30): ~/gpu/phxboot-m7-modes = 1 (M7_MODES
# overrides) arms PhxAccel's head-1 mode list (debug.phxfb1_modes=1, read when head 1 starts: 60 / 144 from the EDID x
# the pipe-1 plans) so System Settings can switch the Acer's refresh; absent = the single mode as before.
MODES=${M7_MODES-$(cat "$(dirname "$0")/phxboot-m7-modes" 2>/dev/null)}
S() { $SYSCTL -n "$1" 2>/dev/null || echo NA; }
W() { $SYSCTL -w "$1" >/dev/null 2>&1; }
last() { S debug.phxpipe2_stat | sed -n 's/^last: //p' | head -1; }
undo() {
  W debug.phxfb1_flip=0; W debug.phxhead2=0; W debug.phxpipe2=0; W debug.phxpipe2_head1=0
  W debug.phxfb1_force=0; W debug.phxfb1_w=1680; W debug.phxfb1_h=1050; W debug.phxpipe2_mode=60; W debug.phxfb1_modes=0
  echo "m7: $(date '+%T') undo: flip $(S debug.phxfb1_flip) head2 $(S debug.phxhead2) pipe2 $(S debug.phxpipe2)"
}
fail() { echo "m7: $(date '+%T') FAIL: $1"; undo; exit 1; }
level() {   # level <n> <name>
  W debug.phxpipe2="$1" || fail "$2 refused ($(last))"
  echo "m7: $(date '+%T') $2 passed"
}
# The rate setup before the survey (nothing lit yet): 165 -> 144 -> 60, each refusal falls to the next.
setup144() {
  if [ "$(S debug.phxpipe2_mode)" = NA ]; then echo "m7: 144 Hz asked, PhxBringup older than b90: 60 Hz"
  elif dal_min 0x0008014b && dal_min 0x0009014b && W debug.phxpipe2_mode=144; then
    echo "m7: $(date '+%T') pipe 1 at 144 Hz: DISPCLK/DPPCLK hard min 331 MHz, debug.phxpipe2_mode 144"
  else W debug.phxpipe2_mode=60; echo "m7: 144 Hz setup refused: 60 Hz"; fi; }
setup165() {
  if [ "$(S debug.phxpipe2_mode)" = NA ]; then echo "m7: 165 Hz asked, PhxBringup older than b90: 60 Hz"; return 0; fi
  if dal_min 0x00080187 && dal_min 0x00090187 && W debug.phxpipe2_mode=165; then
    echo "m7: $(date '+%T') pipe 1 at 165 Hz: DISPCLK/DPPCLK hard min 391 MHz, debug.phxpipe2_mode 165"
  else echo "m7: 165 Hz setup refused (PhxBringup older than b108, or a DAL min refused): the 144 setup"; setup144; fi; }
# survey -> ping -> stream; 0 lit, 1 survey refused (nothing written), 2 a level failed (undone)
ladder() {
  W debug.phxpipe2=1 || { echo "m7: survey refused ($(last)): one monitor"; return 1; }
  echo "m7: $(date '+%T') survey passed"
  for lv in "2 ping" "3 stream"; do set -- $lv
    W debug.phxpipe2="$1" || { echo "m7: $(date '+%T') FAIL: $2 refused ($(last))"; undo; return 2; }
    echo "m7: $(date '+%T') $2 passed"; done
  return 0; }
# "last" -> the Acer's remembered rate (144 when none)
resolve_rate() {
  [ "$RATE" = last ] || return 0
  l=$(sed -n 's/.*acer=\([0-9][0-9]*\).*/\1/p' "$LASTF" 2>/dev/null | head -1)
  case "$l" in 60|144|165) RATE=$l; echo "m7: boot rate last: the Acer's remembered rate $l Hz" ;;
    *) RATE=144; echo "m7: boot rate last: no remembered Acer rate in $LASTF: 144 Hz" ;; esac; }
# C3: one debug.phxc3=9 when asked; logs the kext's verdict (result/ring/pipe0 lines of debug.phxc3_stat), never fails `light`
c3_load() {
  [ "${C3:-}" = 1 ] || return 0
  [ "$(S debug.phxc3)" = NA ] && { echo "m7: C3 asked, PhxBringup without debug.phxc3 (needs b114+): skip"; return 0; }
  if W debug.phxc3=9; then v=done; else v="not done"; fi
  echo "m7: $(date '+%T') C3: PHY D SRAM load $v: $(S debug.phxc3_stat | sed -n '1p')"
  S debug.phxc3_stat | sed -n '/^ring:/p;/^pipe0:/p' | cut -c1-160 | sed 's/^/m7: C3 /'
  return 0; }
scdc() { S debug.phxscdc | head -1; }
# FS-2: pipe 1 lit at 144 -> 165 once the sink's SCDC is locked (see the header). Never fails the bind.
fs2_165() {
  [ "$(S debug.phxpipe2_mode)" = 144 ] || { echo "m7: FS-2: pipe 1 at $(S debug.phxpipe2_mode), not 144: no switch"; return 0; }
  [ "$(S debug.phxpipe2_switch)" = NA ] && { echo "m7: FS-2: no debug.phxpipe2_switch: the Acer stays at 144"; return 0; }
  i=0; ok=0; s=NA
  while [ $i -lt 10 ]; do
    s=$(scdc); case "$s" in *" locked=1 "*) ok=$((ok + 1)) ;; *) ok=0 ;; esac
    [ $ok -ge 2 ] && break; $SLEEP 1; i=$((i + 1)); done
  [ $ok -ge 2 ] || { echo "m7: $(date '+%T') FS-2: the Acer's SCDC not locked twice in 10 s ($s): the Acer stays at 144"; return 0; }
  echo "m7: $(date '+%T') FS-2: SCDC locked at 144, switching pipe 1 to 165"
  if ! W debug.phxpipe2_switch=165; then
    echo "m7: $(date '+%T') FS-2: switch to 165 not done ($(S debug.phxpipe2_switch_stat | sed 's/.*last=\([a-z]*\).*why=/\1: /')): pipe 1 at $(S debug.phxpipe2_mode)"; return 0; fi
  i=0; ok=0
  while [ $i -lt 6 ]; do $SLEEP 0.5; s=$(scdc)
    case "$s" in *tmds_config=0x03*scrambler=0x01*" locked=1 "*) ok=1; break ;; esac; i=$((i + 1)); done
  if [ $ok = 1 ]; then echo "m7: $(date '+%T') FS-2: pipe 1 at 165, SCDC scrambled and locked"
  else echo "m7: $(date '+%T') FS-2: no scrambled lock at 165 in 3 s ($s): back to 144"; W debug.phxpipe2_switch=144
    echo "m7: $(date '+%T') FS-2: pipe 1 at $(S debug.phxpipe2_mode)"; fi
  return 0; }
case "${1:-}" in
light)
  [ "$(S debug.phxpipe2)" = NA ] && { echo "m7: PhxBringup without debug.phxpipe2 (needs b82+): skip"; exit 1; }
  c3_load
  resolve_rate
  case "$RATE" in
  165) if [ "${DIRECT165:-}" = 1 ]; then setup165
       else echo "m7: 165 Hz asked: lit at 144 first, 165 after the Acer's SCDC lock (FS-2, in bind)"; setup144; fi ;;
  144) setup144 ;; esac
  # survey reads only: a refusal (no Acer on HDMI, pipe 1 in use) leaves nothing to undo
  m=$(S debug.phxpipe2_mode); ladder; lr=$?   # m before the ladder: its undo writes debug.phxpipe2_mode=60
  if [ $lr != 0 ] && [ "$m" = 165 ]; then   # the 165 ladder did not light pipe 1: once at 144
    echo "m7: $(date '+%T') the 165 Hz ladder did not light pipe 1 ($([ $lr = 1 ] && echo "survey refused" || echo "undone")): one try at 144 Hz"
    W debug.phxpipe2_mode=60; setup144; ladder; lr=$?
  fi
  [ $lr = 0 ] || exit 1
  if out=$($PHASE 2>&1); then printf '%s\n' "$out" | sed "s/^/m7: $(date '+%T') /"
  else printf '%s\n' "$out" | sed "s/^/m7: $(date '+%T') /"; echo "m7: $(date '+%T') pipe 1 lit, phase NOT aligned"; exit 3; fi ;;
bind)
  [ "$(S debug.phxhead2)" = NA ] && fail "no debug.phxhead2 (PhxAccel older than m7head2)"
  plane1920=0
  case "$H1" in
  "") ;;
  plan) plane1920=1 ;;
  1080)
    hm=$(tr -d ' \t\r\n' < "$HM" 2>/dev/null; echo x)
    if [ ! -e "$HM" ] || { [ "$hm" != x ] && [ "$hm" != enforcex ]; }; then
      echo "m7: head 1 1920x1080 asked, but $HM is not enforce: 1680x1050"
    elif [ "$(S debug.phxsurfmap_learns)" = NA ]; then
      echo "m7: head 1 1920x1080 asked, but PhxAccel has no debug.phxsurfmap: 1680x1050"
    else
      W debug.phxfb1_w=1920 && W debug.phxfb1_h=1080 && W debug.phxfb1_force=1 || fail "head 1 1920x1080 sysctls refused"
      plane1920=1
    fi ;;
  *) echo "m7: unknown head-1 size '$H1' (plan | 1080): 1680x1050" ;;
  esac
  if [ $plane1920 = 1 ]; then
    [ "$(S debug.phxpipe2_head1)" = NA ] && fail "no debug.phxpipe2_head1 (PhxBringup older than b87)"
    W debug.phxpipe2_head1=1 || fail "debug.phxpipe2_head1=1 refused"
    echo "m7: $(date '+%T') plane at 1920x1080 (debug.phxpipe2_head1 1)"
  fi
  if [ "$MODES" = 1 ]; then
    if [ "$(S debug.phxfb1_modes)" = NA ]; then echo "m7: mode list asked, but PhxAccel has no debug.phxfb1_modes: single mode"
    else W debug.phxfb1_modes=1 && echo "m7: $(date '+%T') head 1 mode list armed (debug.phxfb1_modes 1)"; fi
  fi
  W debug.phxhead2=1 || fail "debug.phxhead2=1 refused"
  i=0; st=NA
  while [ $i -lt "$WAITN" ]; do st=$(S debug.phxfb1_state); case "$st" in 0|3) ;; *) break;; esac; $SLEEP 0.25; i=$((i + 1)); done
  [ "$st" = 1 ] || fail "head 1 did not start (debug.phxfb1_state $st)"
  i=0
  while [ $i -lt 12 ]; do [ $(( $(S debug.nvaccel_heads_registered) & 2 )) -ne 0 ] && break; $SLEEP 0.25; i=$((i + 1)); done
  [ $(( $(S debug.nvaccel_heads_registered) & 2 )) -ne 0 ] || { W debug.nvaccelfb=3; $SLEEP 2; }
  echo "m7: $(date '+%T') head 1 up ($(S debug.phxfb1_w)x$(S debug.phxfb1_h), OTG1 $(S debug.phxfb1_pipe_refresh) mHz, registered $(S debug.nvaccel_heads_registered))"
  [ "$MODES" = 1 ] && echo "m7: head 1 modes: $(S debug.phxfb1_nmodes) listed, current id $(S debug.phxfb1_mode)"
  level 4 plane
  [ "$(S debug.phxfb1_scan)" = 1 ] || echo "m7: note: debug.phxfb1_scan $(S debug.phxfb1_scan) (reads on the next head-1 tick)"
  case "$(S debug.phxpipe2_mode)" in 144|165) det99 ;; esac
  if W debug.phxfb1_flip=1; then echo "m7: $(date '+%T') head 1 double buffer on"
  else echo "m7: $(date '+%T') debug.phxfb1_flip=1 refused: head 1 stays on the copy (tears while dragging)"; fi
  resolve_rate >/dev/null
  if [ "$RATE" = 165 ] && [ "${DIRECT165:-}" != 1 ]; then fs2_165; fi ;;
undo) undo ;;
*) sed -n '3,14p' "$0"; exit 2 ;;
esac
