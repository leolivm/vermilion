#!/bin/bash
# SPDX-License-Identifier: MIT
# m7test.sh — MOCK test for tools/phxctl/m7_boot.sh. NO hardware: a fake sysctl keeps the kext's oids in files and
# models the ladder (a level passes when the one below did; SIM_REFUSE=<level> refuses that level), head 1's start
# (debug.phxhead2=1 -> phxfb1_state 3 for SIM_H1_SLOW reads, then SIM_H1_STATE), registration and the flip.
# Scenarios: light_ok, light_no_monitor (survey refused: nothing written), light_stream_fail (undo), bind_ok, bind_head_fail
# (undo, no plane), bind_plane_fail (undo), bind_flip_refused (bind still ok, note), the S4b T4 head-1 sizes (bind_plan1920,
# bind_1080_*: 1920x1080 only under ws-headmap enforce + debug.phxsurfmap), old_kext (no oid). Exit 0 iff all pass.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); M7=${M7:-$HERE/../../tools/phxctl/m7_boot.sh}
PASSN=0; FAILN=0; MOCKDIRS=""
trap '[ -n "$MOCKDIRS" ] && rm -rf $MOCKDIRS' EXIT   # every mock dir this run made (11-02: 2000+ were left in /private/tmp)
mk() {
  W=$(mktemp -d /private/tmp/m7mock.XXXXXX); MOCKDIRS="$MOCKDIRS $W"; O=$W/oid; mkdir -p "$O"
  for k in phxpipe2:0 phxhead2:0 phxfb1_state:0 phxfb1_flip:0 nvaccel_heads_registered:1 phxfb1_scan:-1 phxfb1_w:1680 phxfb1_h:1050 phxfb1_pipe_refresh:60000 nvaccelfb:1 \
           phxpipe2_head1:0 phxfb1_force:0 phxsurfmap_learns:5 phxpipe2_mode:60 phxfb1_modes:0 phxfb1_nmodes:0 phxfb1_mode:0; do
    echo "${k#*:}" > "$O/debug.${k%%:*}"; done
  echo "last: none" > "$O/debug.phxpipe2_stat"
  cat > "$W/sysctl" <<EOS
#!/bin/bash
O=$O
if [ "\$1" = -n ]; then k=\$2; [ -e "\$O/\$k" ] || exit 1
  if [ "\$k" = debug.phxfb1_state ] && [ "\$(cat \$O/debug.phxfb1_state)" = 3 ]; then
    n=\$(cat \$O/h1reads 2>/dev/null || echo 0); n=\$((n+1)); echo \$n > \$O/h1reads
    [ \$n -gt \${SIM_H1_SLOW:-2} ] && { echo \${SIM_H1_STATE:-1} > \$O/debug.phxfb1_state; echo 3 > \$O/debug.nvaccel_heads_registered; }
  fi
  cat "\$O/\$k"; exit 0; fi
[ "\$1" = -w ] || exit 2
k=\${2%%=*}; v=\${2#*=}; [ -e "\$O/\$k" ] || exit 1
echo "\$k=\$v" >> \$O/writes
case "\$k" in
  debug.phxpipe2)
    cur=\$(cat \$O/debug.phxpipe2)
    if [ "\$v" = 0 ]; then echo 0 > \$O/debug.phxpipe2; exit 0; fi
    [ "\$v" = "\${SIM_REFUSE:-x}" ] && { [ -z "\${SIM_REFUSE_MODE:-}" ] || [ "\$(cat \$O/debug.phxpipe2_mode)" = "\$SIM_REFUSE_MODE" ]; } && { echo "last: level \$v FAILED (sim)" > \$O/debug.phxpipe2_stat; exit 1; }
    [ "\$v" -eq \$((cur + 1)) ] || exit 1
    echo \$v > \$O/debug.phxpipe2; [ "\$v" = 4 ] && echo 1 > \$O/debug.phxfb1_scan; exit 0 ;;
  debug.phxhead2) echo \$v > \$O/debug.phxhead2; [ "\$v" = 1 ] && echo 3 > \$O/debug.phxfb1_state; [ "\$v" = 0 ] && echo 2 > \$O/debug.phxfb1_state; exit 0 ;;
  debug.phxfb1_flip) [ "\${SIM_FLIP_REFUSE:-0}" = 1 ] && [ "\$v" = 1 ] && exit 5; echo \$v > \$O/debug.phxfb1_flip; exit 0 ;;
  debug.phxpipe2_head1) [ "\$(cat \$O/debug.phxpipe2)" = 4 ] && exit 16; echo \$v > \$O/debug.phxpipe2_head1; exit 0 ;;
  debug.phxpipe2_mode) [ "\$v" = "\${SIM_MODE_REFUSE:-x}" ] && exit 22; [ "\$(cat \$O/debug.phxpipe2)" = 0 ] || exit 16; echo \$v > \$O/debug.phxpipe2_mode; exit 0 ;;
  debug.phxpipe2_switch)
    [ "\$v" = "\${SIM_SW_REFUSE:-x}" ] && { echo "busy=0 last=refused why=sim" > \$O/debug.phxpipe2_switch_stat; exit 22; }
    echo \$v > \$O/debug.phxpipe2_mode; echo \$v > \$O/debug.phxpipe2_switch; echo "busy=0 last=done why=-" > \$O/debug.phxpipe2_switch_stat
    if [ "\$v" = 165 ] && [ -z "\${SIM_SCDC165_BAD:-}" ]; then echo "rc=0 sink_version=1 tmds_config=0x03 scrambler=0x01 flags0=0x0f locked=1 err0=0" > \$O/debug.phxscdc
    elif [ "\$v" = 165 ]; then echo "rc=0 sink_version=1 tmds_config=0x00 scrambler=0x00 flags0=0x01 locked=0 err0=32767" > \$O/debug.phxscdc
    else echo "rc=0 sink_version=1 tmds_config=0x00 scrambler=0x00 flags0=0x0f locked=1 err0=0" > \$O/debug.phxscdc; fi
    exit 0 ;;
  *) echo \$v > "\$O/\$k"; exit 0 ;;
esac
EOS
  chmod +x "$W/sysctl"
}
run() { SYSCTL=$W/sysctl SLEEP=true M7_WAIT=10 M7_HEADMAP=$W/ws-headmap M7_HEAD1="${MH1:-}" M7_MODES="${MM:-}" M7_PHASE="${MPH:-echo phase: aligned (mock)}" \
        M7_RATE="${MRATE:-}" M7_DIRECT165="${MD165:-}" M7_LAST="${MLAST:-$W/no-rate-last}" M7_PHXCTL="${MPHX:-false}" sh "$M7" "$@" > "$W/out" 2>&1; RC=$?; }
dalok() { printf '#!/bin/sh\necho "$*" >> %s/dal\necho "smu msg $1 arg $2 -> kr 0x0 resp 0x01 (OK)"\n' "$W" > $W/phx; chmod +x $W/phx; MPHX=$W/phx; }
# fake phxctl with registers (DET, HUBP2/3 clock, COMPBUF): a DET write latches CURRENT at once unless SIM_DET_STUCK
regfake() { R=$W/regs; mkdir -p $R; for k in 0x3c6c:0x00500000 0x3d48:0x00500000 0x397a:0x00040202 0x397b:0x303 0x397c:0x303 0x397d:0x3 0x397e:0x3; do echo ${k#*:} > $R/${k%%:*}; done
  cat > $W/phx <<EOS
#!/bin/bash
R=$R; echo "\$*" >> $W/dal
case "\$1" in
  smu) echo "smu msg \$2 arg \$3 -> kr 0x0 resp 0x01 (OK)";;
  regs) shift; for a in "\$@"; do printf '%s: %s\n' "\$a" "\$(cat \$R/\$a 2>/dev/null || echo 0x0)"; done;;
  wreg) v=\$((\$3)); [ -n "\${SIM_DET_STUCK:-}" ] && [ "\$2" = 0x397c ] && { echo "wreg"; exit 0; }; printf '0x%x\n' \$(( (v << 8) | v )) > \$R/\$2; echo "wreg \$2";;
esac
EOS
  chmod +x $W/phx; MPHX=$W/phx; }
dalbad() { printf '#!/bin/sh\necho "$*" >> %s/dal\necho "smu msg $1 arg $2 -> kr 0xe00002d5 resp 0x00 (no response)"\n' "$W" > $W/phx; chmod +x $W/phx; MPHX=$W/phx; }
ln_of() { grep -n "^$1\$" $O/writes | head -1 | cut -d: -f1; }
v() { cat "$O/debug.$1"; }
check() { if eval "$2"; then PASSN=$((PASSN+1)); echo "PASS $1"; else FAILN=$((FAILN+1)); echo "FAIL $1 (rc $RC)"; sed 's/^/   | /' "$W/out" | tail -8; fi; }
mk; run light; check light_ok '[ $RC = 0 ] && [ "$(v phxpipe2)" = 3 ] && grep -q "stream passed" $W/out'
mk; export SIM_REFUSE=1; run light; unset SIM_REFUSE; check light_no_monitor '[ $RC = 1 ] && [ "$(v phxpipe2)" = 0 ] && [ "$(cat $O/writes)" = debug.phxpipe2=1 ] && grep -q "one monitor" $W/out'
mk; export SIM_REFUSE=3; run light; unset SIM_REFUSE; check light_stream_fail '[ $RC = 1 ] && [ "$(v phxpipe2)" = 0 ] && grep -q "FAIL: stream refused" $W/out && grep -q "^debug.phxpipe2=0$" $O/writes'
mk; run light; run bind; check bind_ok '[ $RC = 0 ] && [ "$(v phxpipe2)" = 4 ] && [ "$(v phxhead2)" = 1 ] && [ "$(v phxfb1_flip)" = 1 ] && grep -q "double buffer on" $W/out && [ "$(grep -n phxhead2=1 $O/writes | cut -d: -f1)" -lt "$(grep -n phxpipe2=4 $O/writes | cut -d: -f1)" ] && [ "$(grep -n phxpipe2=4 $O/writes | cut -d: -f1)" -lt "$(grep -n phxfb1_flip=1 $O/writes | cut -d: -f1)" ]'
check bind_default_no_head1 '! grep -q "phxpipe2_head1=1\|phxfb1_force=1\|phxfb1_w=1920" $O/writes'
mk; run light; export SIM_H1_STATE=-4; run bind; unset SIM_H1_STATE; check bind_head_fail '[ $RC = 1 ] && [ "$(v phxpipe2)" = 0 ] && [ "$(v phxhead2)" = 0 ] && ! grep -q phxpipe2=4 $O/writes && grep -q "did not start (debug.phxfb1_state -4)" $W/out'
mk; run light; export SIM_REFUSE=4; run bind; unset SIM_REFUSE; check bind_plane_fail '[ $RC = 1 ] && [ "$(v phxpipe2)" = 0 ] && [ "$(v phxhead2)" = 0 ] && [ "$(v phxfb1_flip)" = 0 ] && grep -q "FAIL: plane refused" $W/out'
mk; run light; export SIM_FLIP_REFUSE=1; run bind; unset SIM_FLIP_REFUSE; check bind_flip_refused '[ $RC = 0 ] && [ "$(v phxpipe2)" = 4 ] && [ "$(v phxfb1_flip)" = 0 ] && grep -q "stays on the copy" $W/out'
# S4b T4: head-1 size (~/gpu/phxboot-m7-head1 = M7_HEAD1): default untouched, plan = T4a, 1080 = T4b only under enforce + surfmap
mk; run light; MH1=plan; run bind; MH1=; check bind_plan1920 '[ $RC = 0 ] && [ "$(v phxpipe2_head1)" = 1 ] && [ "$(v phxpipe2)" = 4 ] && [ "$(v phxfb1_w)" = 1680 ] && [ "$(v phxfb1_force)" = 0 ] && [ "$(ln_of debug.phxpipe2_head1=1)" -lt "$(ln_of debug.phxpipe2=4)" ] && grep -q "plane at 1920x1080" $W/out'
mk; printf "enforce\n" > $W/ws-headmap; run light; MH1=1080; run bind; MH1=; check bind_1080_enforce '[ $RC = 0 ] && [ "$(v phxfb1_w)" = 1920 ] && [ "$(v phxfb1_h)" = 1080 ] && [ "$(v phxfb1_force)" = 1 ] && [ "$(v phxpipe2_head1)" = 1 ] && [ "$(ln_of debug.phxfb1_force=1)" -lt "$(ln_of debug.phxhead2=1)" ] && [ "$(ln_of debug.phxpipe2_head1=1)" -lt "$(ln_of debug.phxpipe2=4)" ]'
mk; : > $W/ws-headmap; run light; MH1=1080; run bind; MH1=; check bind_1080_empty_file_is_enforce '[ $RC = 0 ] && [ "$(v phxfb1_w)" = 1920 ] && [ "$(v phxpipe2_head1)" = 1 ]'
mk; printf "observe\n" > $W/ws-headmap; run light; MH1=1080; run bind; MH1=; check bind_1080_observe_stays_1680 '[ $RC = 0 ] && [ "$(v phxfb1_w)" = 1680 ] && [ "$(v phxfb1_force)" = 0 ] && [ "$(v phxpipe2_head1)" = 0 ] && grep -q "is not enforce" $W/out'
mk; run light; MH1=1080; run bind; MH1=; check bind_1080_no_switch_file '[ $RC = 0 ] && [ "$(v phxfb1_w)" = 1680 ] && [ "$(v phxpipe2_head1)" = 0 ] && grep -q "is not enforce" $W/out'
mk; printf "enforce\n" > $W/ws-headmap; rm "$O/debug.phxsurfmap_learns"; run light; MH1=1080; run bind; MH1=; check bind_1080_no_surfmap '[ $RC = 0 ] && [ "$(v phxfb1_w)" = 1680 ] && [ "$(v phxfb1_force)" = 0 ] && grep -q "no debug.phxsurfmap" $W/out'
mk; printf "enforce\n" > $W/ws-headmap; rm "$O/debug.phxpipe2_head1"; run light; MH1=1080; run bind; MH1=; check bind_1080_old_bringup '[ $RC = 1 ] && [ "$(v phxfb1_w)" = 1680 ] && [ "$(v phxfb1_h)" = 1050 ] && [ "$(v phxfb1_force)" = 0 ] && [ "$(v phxhead2)" = 0 ] && grep -q "older than b87" $W/out'
mk; run light; MH1=bogus; run bind; MH1=; check bind_unknown_size '[ $RC = 0 ] && [ "$(v phxfb1_w)" = 1680 ] && grep -q "unknown head-1 size" $W/out'
mk; MPH=false; run light; MPH=; check light_phase_not_aligned '[ $RC = 3 ] && [ "$(v phxpipe2)" = 3 ] && grep -q "stream passed" $W/out && grep -q "phase NOT aligned" $W/out'
check light_ok_says_aligned 'mk; run light; [ $RC = 0 ] && grep -q "phase: aligned (mock)" $W/out'
# m7_phase.sh against a fake phxctl: OTG0 at line 500, OTG1 trails by $LAG lines; every OTG1 frame-counter read is one
# frame; a frame at the short V_TOTAL moves OTG1 STEP lines closer (the real counter advances once per frame)
PH=${PH:-$HERE/../../tools/phxctl/m7_phase.sh}
pk() {
  P=$(mktemp -d /private/tmp/m7phase.XXXXXX); echo 0 > $P/optc1; echo "${1:-135}" > $P/lag; echo 0 > $P/frame; echo "${2:-0x464}" > $P/vt1; echo "${3:-0x464}" > $P/vt0; : > $P/writes
  cat > $P/phxctl <<EOS
#!/bin/bash
P=$P
[ -n "\${SIM_FAIL:-}" ] && exit 1
if [ "\$1" = wreg ]; then echo "\$2=\$3" >> \$P/writes; [ "\$2" = 0x506f ] && { echo \$3 > \$P/vt1; [ \$((\$3)) -lt \$((\$(cat \$P/vt0))) ] && echo 1024 > \$P/optc1; }
  [ "\$2" = 0x4f9a ] && [ \$(( \$3 & 0x1000 )) -ne 0 ] && echo 0 > \$P/optc1; echo "wreg \$2 = \$3 -> kr 0x00000000  readback \$3"; exit 0; fi
[ "\$1" = regs ] || exit 2; shift
for r in "\$@"; do
  lag=\$(cat \$P/lag); vt1=\$((\$(cat \$P/vt1))); lines=\$(( \$((\$(cat \$P/vt0))) + 1 ))
  case \$r in
    0x500a) printf '0x%05x: 0x%08x\n' \$r 500 ;;
    0x508a) printf '0x%05x: 0x%08x\n' \$r \$(( (500 - lag + lines) % lines )) ;;
    0x4f9a) printf '0x%05x: 0x%08x\n' \$r \$(cat \$P/optc1) ;;
    0x4fef) printf '0x%05x: 0x%08x\n' \$r \$((\$(cat \$P/vt0))) ;;
    0x506f) printf '0x%05x: 0x%08x\n' \$r \$vt1 ;;
    0x508c) f=\$((\$(cat \$P/frame) + 1)); echo \$f > \$P/frame
            [ \$vt1 -lt \$((lines - 1)) ] && echo \$(( (lag - (lines - 1 - vt1) + lines) % lines )) > \$P/lag
            printf '0x%05x: 0x%08x\n' \$r \$f ;;
  esac
done
EOS
  chmod +x $P/phxctl; }
prun() { PHXCTL=$P/phxctl sh "$PH" > $P/out 2>&1; RC=$?; W=$P; }
pk 5; prun; check phase_already_aligned '[ $RC = 0 ] && grep -q "aligned, OTG1 trails OTG0 by 5" $P/out && [ ! -s $P/writes ] && ! grep -q sticky $P/out'
pk 1120; prun; check phase_ahead_counts_as_aligned '[ $RC = 0 ] && [ ! -s $P/writes ]'
pk 135; prun; check phase_align_135 '[ $RC = 0 ] && grep -q "aligned" $P/out && grep -q "sticky cleared (was 1, now 0)" $P/out && [ $(cat $P/optc1) = 0 ] && [ $(( $(cat $P/vt1) )) = 1124 ] && { [ $(cat $P/lag) -le 10 ] || [ $(cat $P/lag) -ge 1115 ]; } && grep -q "0x506f=1119" $P/writes'
pk 600; prun; check phase_align_600 '[ $RC = 0 ] && { [ $(cat $P/lag) -le 10 ] || [ $(cat $P/lag) -ge 1115 ]; } && [ $(( $(cat $P/vt1) )) = 1124 ]'
pk 135 0x464 0x465; prun; check phase_diff_timing_nothing '[ $RC = 0 ] && grep -q "different timings" $P/out && [ ! -s $P/writes ]'
pk 135; export SIM_FAIL=1; prun; unset SIM_FAIL; check phase_read_fail '[ $RC = 1 ] && grep -q "read failed" $P/out && [ ! -s $P/writes ]'
# pipe-1 rate (~/gpu/phxboot-m7-rate): 144 = two DAL hard mins + mode 144 BEFORE the survey; any refusal = 60
mk; dalok; MRATE=144; run light; MRATE=; check rate144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && [ "$(cat $W/dal)" = "$(printf "smu 0x10009 0x0008014b\nsmu 0x10009 0x0009014b")" ] && [ "$(ln_of debug.phxpipe2_mode=144)" -lt "$(ln_of debug.phxpipe2=1)" ] && grep -q "pipe 1 at 144 Hz" $W/out'
mk; dalbad; MRATE=144; run light; MRATE=; check rate144_dal_refused_60 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 60 ] && grep -q "144 Hz setup refused: 60 Hz" $W/out && [ "$(wc -l < $W/dal)" -eq 1 ]'
mk; dalok; rm "$O/debug.phxpipe2_mode"; MRATE=144; run light; MRATE=; check rate144_old_kext_60 '[ $RC = 0 ] && grep -q "older than b90" $W/out && [ ! -e $W/dal ]'
mk; dalok; run light; check rate_default_60_no_dal '[ $RC = 0 ] && [ ! -e $W/dal ] && [ "$(v phxpipe2_mode)" = 60 ]'
mk; regfake; MRATE=144; run light; run bind; MRATE=; check bind144_det99 '[ $RC = 0 ] && [ "$(cat $W/regs/0x397c)" = 0x909 ] && [ "$(cat $W/regs/0x397b)" = 0x909 ] && [ "$(cat $W/regs/0x397d)" = 0x0 ] && grep -q "DET 9/9/0/0" $W/out'
mk; regfake; echo 0x00500001 > $W/regs/0x3d48; MRATE=144; run light; run bind; MRATE=; check bind144_det_pipe3_on_kept '[ $RC = 0 ] && [ "$(cat $W/regs/0x397b)" = 0x303 ] && grep -q "DET: HUBP clock 0x3d48" $W/out'
mk; regfake; export SIM_DET_STUCK=1; MRATE=144; run light; run bind; MRATE=; unset SIM_DET_STUCK; check bind144_det_stuck_stops '[ $RC = 0 ] && grep -q "never current" $W/out && [ "$(v phxfb1_flip)" = 1 ]'
mk; regfake; run light; run bind; check bind60_no_det '[ $RC = 0 ] && [ "$(cat $W/regs/0x397b)" = 0x303 ] && ! grep -q "DET 9" $W/out'
mk; rm "$O/debug.phxpipe2"; run light; check old_kext '[ $RC = 1 ] && grep -q "needs b82" $W/out && [ ! -e $O/writes ]'
# boot rate (11-02): 165 = the 165 plan's DAL hard mins (391/391) + mode 165 before the
# survey; a refused 165 setup or a 165 ladder that does not light = one try with the 144 setup; "last" = ~/gpu/phxboot-rate-last
D165="$(printf "smu 0x10009 0x00080187\nsmu 0x10009 0x00090187")"; D144="$(printf "smu 0x10009 0x0008014b\nsmu 0x10009 0x0009014b")"
mk; dalok; MRATE=165; MD165=1; run light; MD165=; MRATE=; check rate165 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 165 ] && [ "$(v phxpipe2)" = 3 ] && [ "$(cat $W/dal)" = "$D165" ] && [ "$(ln_of debug.phxpipe2_mode=165)" -lt "$(ln_of debug.phxpipe2=1)" ] && grep -q "pipe 1 at 165 Hz: DISPCLK/DPPCLK hard min 391" $W/out && ! grep -q "144" $W/out'
mk; dalok; export SIM_MODE_REFUSE=165; MRATE=165; MD165=1; run light; MD165=; MRATE=; unset SIM_MODE_REFUSE; check rate165_old_kext_144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && [ "$(cat $W/dal)" = "$(printf "%s\n%s" "$D165" "$D144")" ] && grep -q "165 Hz setup refused" $W/out && grep -q "pipe 1 at 144 Hz" $W/out'
mk; dalbad; MRATE=165; MD165=1; run light; MD165=; MRATE=; check rate165_dal_refused_60 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 60 ] && grep -q "165 Hz setup refused" $W/out && grep -q "144 Hz setup refused: 60 Hz" $W/out && [ "$(wc -l < $W/dal)" -eq 2 ]'
mk; dalok; export SIM_REFUSE=3 SIM_REFUSE_MODE=165; MRATE=165; MD165=1; run light; MD165=; MRATE=; unset SIM_REFUSE SIM_REFUSE_MODE; check rate165_stream_fail_retry144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && [ "$(v phxpipe2)" = 3 ] && grep -q "FAIL: stream refused" $W/out && grep -q "did not light pipe 1 (undone): one try at 144 Hz" $W/out && [ "$(ln_of debug.phxpipe2=0)" -lt "$(ln_of debug.phxpipe2_mode=144)" ] && [ "$(cat $W/dal)" = "$(printf "%s\n%s" "$D165" "$D144")" ]'
mk; dalok; export SIM_REFUSE=1; MRATE=165; MD165=1; run light; MD165=; MRATE=; unset SIM_REFUSE; check rate165_no_monitor '[ $RC = 1 ] && [ "$(v phxpipe2)" = 0 ] && grep -q "survey refused): one try at 144 Hz" $W/out && [ "$(grep -c "one monitor" $W/out)" = 2 ] && ! grep -q "^debug.phxpipe2=2" $O/writes'
mk; dalok; export SIM_REFUSE=3; MRATE=144; run light; MRATE=; unset SIM_REFUSE; check rate144_stream_fail_no_retry '[ $RC = 1 ] && [ "$(v phxpipe2)" = 0 ] && ! grep -q "one try" $W/out && [ "$(grep -c "^debug.phxpipe2=1$" $O/writes)" = 1 ]'
mk; dalok; echo "aoc=144 acer=165 at 2026-11-02 10:00:00" > $W/last; MRATE=last; MLAST=$W/last; MD165=1; run light; MD165=; MRATE=; MLAST=; check rate_last_165 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 165 ] && grep -q "remembered rate 165 Hz" $W/out'
mk; dalok; echo "aoc=60 acer=60" > $W/last; MRATE=last; MLAST=$W/last; run light; MRATE=; MLAST=; check rate_last_60 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 60 ] && [ ! -e $W/dal ] && grep -q "remembered rate 60 Hz" $W/out'
mk; dalok; MRATE=last; run light; MRATE=; check rate_last_none_144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && grep -q "no remembered Acer rate" $W/out'
mk; dalok; echo "aoc=144 acer=999" > $W/last; MRATE=last; MLAST=$W/last; run light; MRATE=; MLAST=; check rate_last_bogus_144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && grep -q "no remembered Acer rate" $W/out'
mk; dalok; MRATE=" 165
"; MD165=1; run light; MD165=; MRATE=; check rate165_whitespace '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 165 ]'
mk; regfake; MRATE=165; MD165=1; run light; run bind; MRATE=; MD165=; check bind165_det99 '[ $RC = 0 ] && [ "$(cat $W/regs/0x397b)" = 0x909 ] && [ "$(cat $W/regs/0x397c)" = 0x909 ] && grep -q "DET 9/9/0/0" $W/out'
# mode switch: ~/gpu/phxboot-m7-modes = 1 arms debug.phxfb1_modes before head 1 starts; absent = never written; old PhxAccel = note
mk; run light; MM=1; run bind; MM=; check bind_modes '[ $RC = 0 ] && [ "$(v phxfb1_modes)" = 1 ] && [ "$(ln_of debug.phxfb1_modes=1)" -lt "$(ln_of debug.phxhead2=1)" ] && grep -q "mode list armed" $W/out'
mk; run light; run bind; check bind_modes_absent '[ $RC = 0 ] && ! grep -q "phxfb1_modes=1" $O/writes'
mk; rm "$O/debug.phxfb1_modes"; run light; MM=1; run bind; MM=; check bind_modes_old_accel '[ $RC = 0 ] && grep -q "no debug.phxfb1_modes" $W/out && [ "$(v phxhead2)" = 1 ]'
mk; run light; MM=1; run bind; run undo; MM=; check undo_modes_off '[ "$(v phxfb1_modes)" = 0 ]'

# FS-2 (11-02): 165 = light at 144, then in bind wait for 2 locked SCDC reads, switch to 165, verify
fs2mk() { mk; regfake; echo 60 > "$O/debug.phxpipe2_switch"; echo "busy=0 last=none why=" > "$O/debug.phxpipe2_switch_stat"
  echo "rc=0 sink_version=1 tmds_config=0x00 scrambler=0x00 flags0=0x0f locked=1 err0=0" > "$O/debug.phxscdc"; }
fs2mk; MRATE=165; run light; check fs2_light_at_144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && grep -q "lit at 144 first" $W/out && ! grep -q 0x00080187 $W/dal'
run bind; MRATE=; check fs2_bind_switches_165 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 165 ] && grep -q "FS-2: SCDC locked at 144" $W/out && grep -q "FS-2: pipe 1 at 165, SCDC scrambled and locked" $W/out && [ "$(ln_of debug.phxfb1_flip=1)" -lt "$(ln_of debug.phxpipe2_switch=165)" ] && ! grep -q "phxpipe2_switch=144" $O/writes'
fs2mk; echo "rc=0 sink_version=1 tmds_config=0x00 scrambler=0x00 flags0=0x01 locked=0 err0=32767" > "$O/debug.phxscdc"; MRATE=165; run light; run bind; MRATE=; check fs2_never_locked_stays_144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && grep -q "not locked twice in 10 s" $W/out && ! grep -q phxpipe2_switch $O/writes'
fs2mk; export SIM_SCDC165_BAD=1; MRATE=165; run light; run bind; MRATE=; unset SIM_SCDC165_BAD; check fs2_no_lock_at_165_back_144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && grep -q "no scrambled lock at 165 in 3 s" $W/out && [ "$(ln_of debug.phxpipe2_switch=165)" -lt "$(ln_of debug.phxpipe2_switch=144)" ]'
fs2mk; export SIM_SW_REFUSE=165; MRATE=165; run light; run bind; MRATE=; unset SIM_SW_REFUSE; check fs2_switch_refused_stays_144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && grep -q "switch to 165 not done (refused: sim)" $W/out'
fs2mk; rm "$O/debug.phxpipe2_switch"; MRATE=165; run light; run bind; MRATE=; check fs2_old_kext_no_switch '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && grep -q "no debug.phxpipe2_switch" $W/out'
fs2mk; MRATE=144; run light; run bind; MRATE=; check fs2_not_for_144 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 144 ] && ! grep -q "FS-2" $W/out && ! grep -q phxpipe2_switch $O/writes'
fs2mk; echo "aoc=144 acer=165" > $W/last; MRATE=last; MLAST=$W/last; run light; run bind; MRATE=; MLAST=; check fs2_last_165 '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 165 ] && grep -q "FS-2: pipe 1 at 165" $W/out'
fs2mk; MD165=1; MRATE=165; run light; run bind; MRATE=; MD165=; check fs2_direct165_optout '[ $RC = 0 ] && [ "$(v phxpipe2_mode)" = 165 ] && grep -q 0x00080187 $W/dal && ! grep -q "FS-2" $W/out && ! grep -q phxpipe2_switch $O/writes'

# C3 (the Acer dark after a cold boot): on by default (M7_C3 unset; the mock's script dir has no phxboot-c3-off) -> debug.phxc3=9 first (before the DAL mins / mode / survey); a refusal or a kext without
# the oid only logs; without the flag nothing is written to debug.phxc3
c3mk() { mk; echo -1 > $O/debug.phxc3; printf 'result=PASS mode=9 step=L1475 why=loaded\nboot: x\nring: loaded 1475/1475\npipe0: same=1\n' > $O/debug.phxc3_stat; }
c3mk; dalok; MRATE=144; run light; MRATE=; check c3_load_first '[ $RC = 0 ] && [ "$(head -1 $O/writes)" = debug.phxc3=9 ] && grep -q "C3: PHY D SRAM load done: result=PASS" $W/out && grep -q "m7: C3 ring: loaded 1475/1475" $W/out && grep -q "stream passed" $W/out && [ "$(head -1 $W/dal | cut -c1-7)" = "smu 0x1" ]'
c3mk; MRATE=144; dalok; M7_C3=0 run light; MRATE=; check c3_off_untouched '[ $RC = 0 ] && ! grep -q phxc3 $O/writes && ! grep -q C3 $W/out'
c3mk; dalok; mv $W/sysctl $W/sysctl.real; printf '#!/bin/bash\n[ "$1" = -w ] && [ "${2%%%%=*}" = debug.phxc3 ] && exit 1\nexec %s "$@"\n' "$W/sysctl.real" > $W/sysctl; chmod +x $W/sysctl
echo 'result=refused mode=9 step=- why=G2: PHY D programmed' > $O/debug.phxc3_stat; MRATE=144; M7_C3=1 run light; MRATE=; check c3_refused_goes_on '[ $RC = 0 ] && grep -q "C3: PHY D SRAM load not done: result=refused" $W/out && grep -q "stream passed" $W/out'
mk; M7_C3=1 run light; check c3_old_kext_skip '[ $RC = 0 ] && grep -q "needs b114" $W/out && ! grep -q phxc3 $O/writes && grep -q "stream passed" $W/out'
echo "$PASSN passed, $FAILN failed"; [ $FAILN = 0 ]
