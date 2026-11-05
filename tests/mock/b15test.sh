#!/bin/bash
# SPDX-License-Identifier: MIT
# b15test.sh — MOCK test for tools/phxctl/b15_boot.sh. NO hardware: a fake phxctl.bin keeps registers in files,
# the four recipe scripts are stubs that log their call. Scenarios: happy (stutter allowed, watchdog runs to MAXPOLL), dpm (+ uclk unpin, FCLK switch),
# underflow (HUBP0 sticky appears right after the allow write -> 0x39BC back to 0x1002), step_fail (dalwm fails -> stutter never
# allowed), not_gop (0x39BC already 0x1000 -> skip); two-pipe, pipe-1/pipe-0 switch skips; AOC at 144 (aoc144 / aoc60 live
# sets, the boot arm, the follower, the 144 + 144 branch, the T7 request; b103: AOC 144 + Acer 60, the follower on both
# OTGs; SCDC2: the Acer at 165 next to the AOC at 144 and at 60, live, at boot and through the follower; the watchdog read
# server: srv_* = with / without it / gone mid-watch, *gen_skip, spawn_count = processes per poll). Exit 0 iff all pass.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); B15=${B15:-$HERE/../../tools/phxctl/b15_boot.sh}
PASSN=0; FAILN=0; IDX=0
# PAR=<n>: the scenarios are split round-robin over n copies of this script run at once (B15SHARD=<k>/<n>, the same shell),
# each scenario's lines are collected and printed in scenario order, then one RESULT. Default 2 for a full run, 1 with
# B15ONLY / B15STOP (the mutation checks: their runners run several mutations at once instead). PAR=1 = the plain serial run.
case "${B15ONLY:-}${B15STOP:-}" in '') PAR=${PAR:-2} ;; *) PAR=${PAR:-1} ;; esac
if [ -z "${B15SHARD:-}" ] && [ "$PAR" -gt 1 ]; then
  case ":${SHELLOPTS:-}:" in *:posix:*) SHB=sh ;; *) SHB=bash ;; esac
  PD=$(mktemp -d /private/tmp/b15par.XXXXXX); k=0; pids=""
  while [ $k -lt "$PAR" ]; do B15SHARD=$k/$PAR B15PARDIR=$PD B15=$B15 $SHB "$0" > "$PD/shard.$k" 2>&1 & pids="$pids $!"; k=$((k + 1)); done
  bad=0; k=0; for p in $pids; do wait $p; grep -q '^RESULT: [0-9]* passed' "$PD/shard.$k" || { bad=1; echo "SHARD $k/$PAR did not finish:"; cat "$PD/shard.$k"; }; k=$((k + 1)); done
  for i in $(ls "$PD" | grep -E '^[0-9]+$' | sort -n); do [ -e "$PD/$i.pre" ] && cat "$PD/$i.pre"; cat "$PD/$i"
    case "$(head -1 "$PD/$i")" in PASS*) PASSN=$((PASSN + 1)) ;; *) FAILN=$((FAILN + 1)) ;; esac; done
  k=0; while [ $k -lt "$PAR" ]; do grep -v '^RESULT: [0-9]* passed' "$PD/shard.$k"; k=$((k + 1)); done
  rm -rf "$PD"; echo "RESULT: $PASSN passed, $FAILN failed"; [ $FAILN = 0 ] && [ $bad = 0 ]; exit; fi
# A scenario B15ONLY skips runs its setup line against one shared scratch world (SKIPW, made once): no per-skip mktemp, stubs or rm.
SKIPW=""; trap '[ -n "$SKIPW" ] && rm -rf "$SKIPW"' EXIT
mk() {   # mk <name>: fresh fake ~/gpu with GOP register state
  NAME=$1; SKIP=0; IDX=$((IDX + 1)); case "$NAME" in ${B15ONLY:-*}) ;; *) SKIP=1 ;; esac
  [ -n "${B15SHARD:-}" ] && [ $(( (IDX - 1) % ${B15SHARD#*/} )) -ne ${B15SHARD%/*} ] && SKIP=1
  if [ $SKIP = 1 ] && [ -n "$SKIPW" ]; then W=$SKIPW; G=$W/gpu; R=$W/regs; return 0; fi
  W=$(mktemp -d /private/tmp/b15mock.XXXXXX); G=$W/gpu; R=$W/regs; mkdir -p "$G" "$R"; [ $SKIP = 1 ] && SKIPW=$W
  echo 0x00001002 > "$R/0x39BC"; echo 0x00000000 > "$R/0x4F8A"; echo 0x00000000 > "$R/0x3AB3"; echo 0x00000807 > "$R/0x4FEA"
  # serve = phxctl.bin's read server (one reply line per item: a register's value, @<sysctl> = the value with newlines as
  # blanks; the two engine-state sysctls through PHXB15_SWSTAT / PHXB15_SW0STAT when set, as sw / sw0); SIM_NOSERVE=1 = an
  # older phxctl.bin (no serve), SIM_SERVE_DIE_AT=<n> = the server exits at its n-th request. B15SPAWN=<file>: each start
  # logs "phxctl <command>" there (the spawn count; what the fake runs inside is not a process on the box).
  cat > "$G/phxctl.bin" <<'EOF'
#!/bin/bash
# builtins only for the registers (printf -v, read): the fake costs one process per start, like the real phxctl.bin
R=@R@; printf -v a 0x%X $(($2))
rreg() { v=0x00000000; [ -r "$R/$1" ] && IFS= read -r v < "$R/$1"; }   # = $(cat "$R/$1" 2>/dev/null || echo 0x00000000) for a one-line file
[ -n "${B15SPAWN:-}" ] && echo "phxctl $1" >> "$B15SPAWN"; unset B15SPAWN
case "$1" in
  regs) rreg "$a"; echo "$a: $v" ;;
  wreg) printf -v v 0x%08X $(($3))
        [ "$a" = 0x3AB3 ] && printf -v v 0x%08X $(( $3 & 0x0FFFFFFF ))   # UNDERFLOW_CLEAR clears the sticky
        [ "$a" = 0x4F8A ] && v=0x00000000
        echo "$v" > "$R/$a"; echo "$a $v" >> "$R/writes"
        if [ -n "${SIM_UF_AT:-}" ]; then n=0; while IFS= read -r l; do n=$((n + 1)); done < "$R/writes"; [ "$n" -ge "$SIM_UF_AT" ] && echo 0x10000000 > "$R/0x3AB3"; fi
        echo "wreg $a <- $v" ;;
  serve) [ "${SIM_NOSERVE:-0}" = 1 ] && { echo "unknown command" >&2; exit 2; }
        [ -n "${B15CPU:-}" ] && { trap 'times >> "$B15CPU"' EXIT; trap 'exit 143' TERM; }   # measurement: the server is an orphan, its CPU is not in the caller's rusage
        SW=${PHXB15_SWSTAT:-sysctl -n debug.phxpipe2_switch_stat}; SW0=${PHXB15_SW0STAT:-sysctl -n debug.phxpipe0_stat}; nq=0
        # a state stub (one script path, no args) is sourced in a subshell: the same output and files, no exec per item
        # (the real server reads the sysctl in-process); anything else (the default sysctl command) runs as before
        st() { case "$1" in *" "*) x=$($1 2>/dev/null) ;; *) if [ -f "$1" ]; then x=$( . "$1" 2>/dev/null ); else x=$($1 2>/dev/null); fi ;; esac; }
        while IFS= read -r q; do nq=$((nq + 1)); [ -n "${SIM_SERVE_DIE_AT:-}" ] && [ $nq -ge "$SIM_SERVE_DIE_AT" ] && exit 1
          for it in $q; do case $it in
            @debug.phxpipe2_switch_stat) st "$SW" ;;
            @debug.phxpipe0_stat) st "$SW0" ;;
            @*) x=$(sysctl -n "${it#@}" 2>/dev/null) ;;
            *) printf -v a 0x%X $((it)); rreg "$a"; x=$v ;;
          esac; echo "${x//$'\n'/ }"; done; done ;;
esac
EOF
  sed -i '' "s|@R@|$R|" "$G/phxctl.bin"
  for s in wm144 det dalwm dal hubp_dlg uclk; do printf '#!/bin/sh\necho "%s $*" >> %s/calls; exit ${SIM_FAIL_%s:-0}\n' "$s" "$W" "$s" > "$G/$s.sh"; done
  chmod +x "$G"/*; }
swstub() {   # swstub <busy> <gen>: the switch-state sysctl; each call also sets the OPTC1 underflow sticky (the switch's own mark)
  printf '#!/bin/sh\necho 0x00000400 > %s/0x4F9A\necho "busy=%s gen=%s mode=60 last=done"\n' "$R" "$1" "$2" > "$W/swstat"; chmod +x "$W/swstat"; export PHXB15_SWSTAT=$W/swstat; }
# B15ONLY=<glob> (e.g. 'rl_*'): only the scenarios whose name matches run and count (the mutation checks); B15STOP=1: stop at the first failure
run() { [ $SKIP = 1 ] && return 0; ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=${MAXPOLL:-20}; sh "$B15" ) > "$W/out" 2>&1; RC=$?; }
# B15KEEP=<dir>: each scenario's mock world is kept as <dir>/<name> (the old/new decision diff, tests/mock/b15diff.sh)
check() { [ $SKIP = 1 ] && return 0; [ -n "${B15KEEP:-}" ] && { mkdir -p "$B15KEEP/$NAME"; cp -R "$W/." "$B15KEEP/$NAME/"; }
  if [ -n "${B15PARDIR:-}" ]; then exec 5> "$B15PARDIR/$IDX"; else exec 5>&1; fi   # a shard: this scenario's lines to its own file
  { if eval "$1"; then PASSN=$((PASSN + 1)); echo "PASS  $NAME"; else FAILN=$((FAILN + 1)); echo "FAIL  $NAME ($1)"; cat "$W/out"; fi; } >&5; exec 5>&-; rm -rf "${W:?}"
  [ $FAILN -gt 0 ] && [ -n "${B15STOP:-}" ] && { echo "RESULT: stopped at the first failure (B15STOP)"; exit 1; }; return 0; }
say() { if [ -n "${B15PARDIR:-}" ]; then echo "$*" >> "$B15PARDIR/$IDX.pre"; else echo "$*"; fi; }   # a line before this scenario's check
st() { cat "$R/0x39BC"; }

mk happy; run
check '[ $RC = 0 ] && [ "$(st)" = 0x00001000 ] && grep -q "stutter allowed" $W/out && [ "$(cat $W/calls | tr "\n" ,)" = "wm144 linux,det set 9,dalwm ,hubp_dlg linux," ]'
mk dpm; touch "$G/phxboot-b15-dpm"; run
check '[ $RC = 0 ] && [ "$(st)" = 0x00001000 ] && [ "$(cat $W/calls | tr "\n" ,)" = "wm144 linux,det set 9,dalwm ,hubp_dlg linux,uclk unpin,dal SetFclkSwitchAllow 1," ]'
mk underflow; export SIM_UF_AT=4; run; unset SIM_UF_AT
check '[ $RC = 2 ] && [ "$(st)" = 0x00001002 ] && grep -q "hubp underflow" $W/out'
mk dpm_underflow; touch "$G/phxboot-b15-dpm"; export SIM_UF_AT=4; run; unset SIM_UF_AT
check '[ $RC = 2 ] && [ "$(st)" = 0x00001002 ] && grep -q "uclk pin 456" $W/calls'
mk step_fail; export SIM_FAIL_dalwm=1; run; unset SIM_FAIL_dalwm
check '[ $RC = 1 ] && [ "$(st)" = 0x00001002 ] && ! grep -q "stutter allowed" $W/out && ! grep -q hubp_dlg $W/calls'
mk not_gop; echo 0x00001000 > "$R/0x39BC"; run
check '[ $RC = 1 ] && grep -q "not the GOP" $W/out && [ ! -e $W/calls ]'
mk otg1_lit; echo 0x00000001 > "$R/0x5081"; run
check '[ $RC = 1 ] && grep -q "OTG1 is lit" $W/out && [ ! -e $W/calls ] && [ "$(st)" = 0x00001002 ]'
mk otg0_60hz; echo 0x0000089F > "$R/0x4FEA"; run
check '[ $RC = 1 ] && grep -q "not the 1080p144" $W/out && [ ! -e $W/calls ] && [ "$(st)" = 0x00001002 ]'
# two displays (OTG1 lit): the 10-30 two-pipe branch
two() { echo 0x00000001 > "$R/0x5081"; echo 0x00000897 > "$R/0x4FEA"; echo 0x00000909 > "$R/0x397B"; echo 0x00000909 > "$R/0x397C"; }
mk two_noflag; two; run
check '[ $RC = 1 ] && grep -q "no ~/gpu/phxboot-b15-2pipe" $W/out && [ ! -e $W/calls ]'
mk two_wm_only; two; touch "$G/phxboot-b15-2pipe"; run
check '[ $RC = 0 ] && [ "$(cat $W/calls | tr "\n" ,)" = "wm144 linux2,hubp_dlg linux60," ] && [ "$(st)" = 0x00001002 ]'
mk two_dpm; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; run
check '[ $RC = 0 ] && [ "$(cat $W/calls | tr "\n" ,)" = "wm144 linux2,hubp_dlg linux60,uclk unpin,dal SetFclkSwitchAllow 1," ] && [ "$(st)" = 0x00001002 ] && grep -q "UCLK/FCLK free" $W/out'
mk two_det3; two; echo 0x00000303 > "$R/0x397C"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; run
check '[ $RC = 1 ] && grep -q "not 9/9" $W/out && [ ! -e $W/calls ]'
mk two_otg0_144; two; echo 0x00000807 > "$R/0x4FEA"; touch "$G/phxboot-b15-2pipe"; run
check '[ $RC = 1 ] && grep -q "not 1080p60" $W/out && [ ! -e $W/calls ]'
mk two_underflow_repin; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; export SIM_UF_AT=5; run; unset SIM_UF_AT
check '[ $RC = 2 ] && grep -q "hubp0 underflow" $W/out && grep -q "uclk pin 456" $W/calls && [ "$(st)" = 0x00001002 ]'
mk two_sr; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-b15-2pipe-sr"; run
check '[ $RC = 0 ] && [ "$(st)" = 0x00001000 ] && grep -q "stutter allowed" $W/out'
mk two_sr_underflow; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-b15-2pipe-sr"; export SIM_UF_AT=6; run; unset SIM_UF_AT
check '[ $RC = 2 ] && [ "$(st)" = 0x00001002 ] && grep -q "stutter forced off, UCLK pinned" $W/out && grep -q "uclk pin 456" $W/calls'
mk missing; rm -f "$G/dalwm.sh"; run
check '[ $RC = 1 ] && grep -q "dalwm.sh missing" $W/out && [ "$(st)" = 0x00001002 ]'
# b91: a pipe-1 sticky while the kext switches modes is the switch's own: not a watchdog hit; when idle it is
mk two_switch_busy; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; swstub 1 3; run; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && ! grep -q underflow $W/out && ! grep -q "uclk pin" $W/calls'
mk two_switch_idle; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; swstub 0 3; run; unset PHXB15_SWSTAT
check '[ $RC = 2 ] && grep -q "optc1 underflow" $W/out && grep -q "uclk pin 456" $W/calls'
# b99: a pipe-0 sticky while the kext re-times pipe 0 is the switch's own: skipped; when idle it is a hit
sw0stub() {   # sw0stub <busy> <gen>: debug.phxpipe0_stat; each call also sets the OPTC0 underflow sticky
  printf '#!/bin/sh\necho 0x00000400 > %s/0x4F8A\necho "busy=%s gen=%s enable=1 mode=60 last=done"\n' "$R" "$1" "$2" > "$W/sw0stat"; chmod +x "$W/sw0stat"; export PHXB15_SW0STAT=$W/sw0stat; }
mk two_p0switch_busy; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; sw0stub 1 2; run; unset PHXB15_SW0STAT
check '[ $RC = 0 ] && ! grep -q underflow $W/out && ! grep -q "uclk pin" $W/calls'
mk two_p0switch_idle; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; sw0stub 0 2; run; unset PHXB15_SW0STAT
check '[ $RC = 2 ] && grep -q "optc0 underflow" $W/out && grep -q "uclk pin 456" $W/calls'
# a poll whose engine state changed generation between its two reads (a switch ran meanwhile, never seen busy) skips that
# pipe: each call of the stub = the next gen and that pipe's OPTC sticky set
genstub() { printf '#!/bin/sh\ncf=%s/%s.n; c=0; [ -r $cf ] && read -r c < $cf; c=$((c + 1)); echo $c > %s/%s.n\necho 0x00000400 > %s/%s\necho "busy=0 gen=$c mode=60 last=done"\n' "$W" "$1" "$W" "$1" "$R" "$2" > "$W/$1"; chmod +x "$W/$1"; }
mk two_p1gen_skip; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; genstub swstat 0x4F9A; export PHXB15_SWSTAT=$W/swstat; run; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && ! grep -q underflow $W/out && ! grep -q "uclk pin" $W/calls && [ "$(cat $W/swstat.n)" -ge 40 ]'
mk two_p0gen_skip; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; genstub sw0stat 0x4F8A; export PHXB15_SW0STAT=$W/sw0stat; run; unset PHXB15_SW0STAT
check '[ $RC = 0 ] && ! grep -q underflow $W/out && ! grep -q "uclk pin" $W/calls && [ "$(cat $W/sw0stat.n)" -ge 40 ]'
mk watch2_only; two; echo 0x00001000 > "$R/0x39BC"; swstub 1 4; [ $SKIP = 1 ] || ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=5; sh "$B15" watch2 ) > "$W/out" 2>&1; RC=$?; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && grep -q "watchdog only" $W/out && grep -q "stopped after 5" $W/out && [ ! -e $W/calls ] && [ ! -e $R/writes ]'
# AOC at 144 (10-31): the live sets, the boot arm, the follower, the 144 + 144
# boot branch, the System Settings request
runa() { [ $SKIP = 1 ] && return 0; ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=${MAXPOLL:-20} PHXB15_SYSCTL=$W/sysctl PHXB15_AOCSETTLE=0; sh "$B15" "$@" ) > "$W/out" 2>&1; RC=$?; }
scstub() { printf '#!/bin/sh\necho "sysctl $*" >> %s/sysctl.log\n' "$W" > "$W/sysctl"; chmod +x "$W/sysctl"; }
at144() {   # OTG0 at the AOC 144 plan's totals and its HUBP0 rate group (u3), OTG1 = the Acer at 144, DET 9/9
  echo 0x00000807 > "$R/0x4FEA"; echo 0x0000044B > "$R/0x4FEF"; echo 0x00000001 > "$R/0x5081"; echo 0x00000807 > "$R/0x506A"
  echo 0x00000449 > "$R/0x506F"; echo 0x00000909 > "$R/0x397B"; echo 0x00000909 > "$R/0x397C"
  for x in 0x3B05:0x00013BA8 0x3B1C:0x00013A6D 0x3B19:0x00000126 0x3B1A:0x00000126 0x3AEB:0x0800274D 0x3AEC:0x0000274D 0x3AE9:0x04EE0000 0x3AEA:0xE0000A14; do
    echo "${x#*:}" > "$R/${x%%:*}"; done; }
calls() { [ -e "$W/calls" ] && tr '\n' , < "$W/calls"; }
mk aoc144_live; at144; scstub; runa aoc144
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144x2," ] && grep -q "AOC 144 + Acer 144" $W/out && grep -q "HUBP0 rate group: 0 of 8" $W/out && [ "$(st)" = 0x00001002 ]'
mk aoc144_hubp0_off; at144; echo 0x0002E4BD > "$R/0x3B05"; scstub; runa aoc144
check '[ $RC = 1 ] && [ "$(calls)" = "uclk pin 456," ] && grep -q "not the 144 plan" $W/out'
mk aoc144_at60; at144; echo 0x00000897 > "$R/0x4FEA"; echo 0x00000464 > "$R/0x4FEF"; scstub; runa aoc144
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not the AOC at 1080p144" $W/out && [ ! -e $R/writes ]'
mk aoc144_acer60; at144; echo 0x00000464 > "$R/0x506F"; scstub; runa aoc144
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not the Acer at 1080p144" $W/out'
mk aoc144_stutter; at144; echo 0x00001000 > "$R/0x39BC"; scstub; runa aoc144
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "stutter allowed" $W/out'
mk aoc144_busy; at144; scstub; sw0stub 1 3; runa aoc144; unset PHXB15_SW0STAT
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "engine is busy" $W/out'
mk aoc60_live; two; scstub; runa aoc60
check '[ $RC = 0 ] && [ "$(calls)" = "wm144 linux2," ] && grep -q "back at 60" $W/out'
mk aoc60_at144; at144; scstub; runa aoc60
check '[ $RC = 1 ] && [ ! -e $W/calls ]'
mk two_dpm_noarm; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; scstub; runa
check '[ $RC = 0 ] && ! grep -v "^sysctl -n debug.phxhotplug_stat$" $W/sysctl.log 2>/dev/null | grep -q . && ! grep -q "aoc:" $W/out'
mk two_aoc_arm; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; runa
check '[ $RC = 0 ] && grep -q "sysctl -w debug.phxpipe0_enable=1" $W/sysctl.log && grep -q "pipe-0 engine armed" $W/out && [ "$(calls)" = "wm144 linux2,hubp_dlg linux60,uclk unpin,dal SetFclkSwitchAllow 1," ] && ! grep -q "144 asked" $W/out'
sw0flip() {   # sw0flip <from call n>: debug.phxpipe0_stat idle (gen 5); from its n-th call on, OTG0 runs the AOC 144 plan (a done switch)
  printf '#!/bin/sh\ncf=%s/sw0n; c=0; [ -r $cf ] && read -r c < $cf; c=$((c + 1)); echo $c > %s/sw0n\n[ $c -ge %s ] && { echo 0x00000807 > %s/0x4FEA; echo 0x0000044B > %s/0x4FEF; }\necho "busy=${SW0BUSY:-0} gen=5 enable=1 mode=60 last=done"\n' \
    "$W" "$W" "$1" "$R" "$R" > "$W/sw0stat"; chmod +x "$W/sw0stat"; export PHXB15_SW0STAT=$W/sw0stat; }
mk two_follow_144; two; at144; echo 0x00000897 > "$R/0x4FEA"; echo 0x00000464 > "$R/0x4FEF"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; sw0flip 4; runa; unset PHXB15_SW0STAT
check '[ $RC = 0 ] && [ "$(calls)" = "wm144 linux2,hubp_dlg linux60,uclk unpin,dal SetFclkSwitchAllow 1,uclk pin 456,wm144 linux144x2," ] && grep -q "AOC 144 + Acer 144" $W/out && ! grep -q underflow $W/out'
mk two_follow_busy; two; at144; echo 0x00000897 > "$R/0x4FEA"; echo 0x00000464 > "$R/0x4FEF"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; export SW0BUSY=1; sw0flip 1; runa; unset PHXB15_SW0STAT SW0BUSY
check '[ $RC = 0 ] && ! grep -q "linux144x2" $W/calls && ! grep -q "AOC 144" $W/out'
mk two_follow_noflag; two; at144; echo 0x00000897 > "$R/0x4FEA"; echo 0x00000464 > "$R/0x4FEF"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; scstub; sw0flip 4; runa; unset PHXB15_SW0STAT
check '[ $RC = 0 ] && ! grep -q "linux144x2" $W/calls && ! grep -q "AOC 144" $W/out'
mk two_at144_branch; two; at144; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; runa
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144x2," ] && grep -q "two displays at 144 + 144" $W/out && grep -q "stopped after" $W/out'
mk two_at144_hubp0_off; two; at144; echo 0x0002E4BD > "$R/0x3B05"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; runa
check '[ $RC = 1 ] && [ "$(calls)" = "uclk pin 456," ] && ! grep -q "watchdog every" $W/out'
# b103: the AOC at 144 next to the Acer at 60 (u4 sets), the follower on both OTGs
acer60() { echo 0x00000897 > "$R/0x506A"; echo 0x00000464 > "$R/0x506F"; echo 0xE0000AFB > "$R/0x3AEA"; }   # + HUBP0 = the 1080p144a60 plan
mk aoc144a60_live; at144; acer60; scstub; runa aoc144
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144a60," ] && grep -q "AOC 144 + Acer 60" $W/out && grep -q "HUBP0 rate group: 0 of 8 differ from Linux u4" $W/out && ! grep -q HUBP1 $W/out'
mk aoc144a60_hubp0_u3; at144; acer60; echo 0xE0000A14 > "$R/0x3AEA"; scstub; runa aoc144
check '[ $RC = 1 ] && [ "$(calls)" = "uclk pin 456," ] && grep -q "not the 1080p144a60 plan" $W/out'
mk aoc144_acer_other; at144; echo 0x0000089F > "$R/0x506A"; scstub; runa aoc144
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not the Acer at 1080p144 or 1080p60" $W/out'
mk two_at144a60_branch; two; at144; acer60; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; runa
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144a60," ] && grep -q "two displays at 144 + 60" $W/out && grep -q "stopped after" $W/out'
swflip() {   # swflip <from call n> <h1> <v1> <ttu>: debug.phxpipe2_switch_stat idle (gen 7); from its n-th call on OTG1 + HUBP0 MIN_TTU = the args (a done switch + pair re-plan)
  printf '#!/bin/sh
cf=%s/swn; c=0; [ -r $cf ] && read -r c < $cf; c=$((c + 1)); echo $c > %s/swn
[ $c -ge %s ] && { echo %s > %s/0x506A; echo %s > %s/0x506F; echo %s > %s/0x3AEA; }
echo "busy=${SWBUSY:-0} gen=7 mode=60 last=done"
' \
    "$W" "$W" "$1" "$2" "$R" "$3" "$R" "$4" "$R" > "$W/swstat"; chmod +x "$W/swstat"; export PHXB15_SWSTAT=$W/swstat; }
mk two_follow_acer60; two; at144; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; swflip 6 0x00000897 0x00000464 0xE0000AFB; runa; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144x2,uclk pin 456,wm144 linux144a60," ] && grep -q "AOC 144 + Acer 60" $W/out && ! grep -q underflow $W/out'
mk two_follow_acer144; two; at144; acer60; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; swflip 6 0x00000807 0x00000449 0xE0000A14; runa; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144a60,uclk pin 456,wm144 linux144x2," ] && grep -q "AOC 144 + Acer 144" $W/out'
mk two_follow_p1busy; two; at144; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; export SWBUSY=1; swflip 1 0x00000897 0x00000464 0xE0000AFB; runa; unset PHXB15_SWSTAT SWBUSY
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144x2," ] && ! grep -q "Acer 60" $W/out'
mk two_follow_aoc60_acer; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; swflip 4 0x00000807 0x00000449 0x00000000; runa; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && [ "$(calls)" = "wm144 linux2,hubp_dlg linux60,uclk unpin,dal SetFclkSwitchAllow 1," ] && ! grep -q "back at 60" $W/out'
# SCDC2 (11-01, from a Linux register capture): the Acer at 1080p165 (OTG1 0x81f/0x455) next to either AOC mode
acer165() { echo 0x00000001 > "$R/0x5081"; echo 0x0000081F > "$R/0x506A"; echo 0x00000455 > "$R/0x506F"; }
mk aoc144a165_live; at144; acer165; echo 0xE0000AE8 > "$R/0x3AEA"; scstub; runa aoc144
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144a165," ] && grep -q "AOC 144 + Acer 165" $W/out && grep -q "HUBP0 rate group: 0 of 8 differ from Linux c3" $W/out && grep -q "HUBP1 rate group: 8 of 8 differ from Linux c3" $W/out && [ "$(st)" = 0x00001002 ]'
mk aoc144a165_hubp0_144; at144; acer165; scstub; runa aoc144
check '[ $RC = 1 ] && [ "$(calls)" = "uclk pin 456," ] && grep -q "not the 1080p144a165 plan" $W/out'
mk aoc144a165_v_other; at144; acer165; echo 0x00000449 > "$R/0x506F"; scstub; runa aoc144
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "or 1080p165" $W/out'
mk aoc60a165_live; two; acer165; echo 0x00001000 > "$R/0x39BC"; scstub; runa aoc60
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux60a165," ] && [ "$(st)" = 0x00001002 ] && grep -q "AOC 60 + Acer 165" $W/out && grep -q "HUBP0 rate group: 8 of 8 differ from Linux c2" $W/out'
mk aoc60a165_stutter_off; two; acer165; scstub; runa aoc60
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux60a165," ] && ! grep -q 0x39BC $R/writes 2>/dev/null'
mk aoc60a165_det3; two; acer165; echo 0x00000303 > "$R/0x397C"; scstub; runa aoc60
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not 9/9" $W/out && [ ! -e $R/writes ]'
mk aoc60a165_pinfail; two; acer165; echo 0x00001000 > "$R/0x39BC"; scstub; export SIM_FAIL_uclk=1; runa aoc60; unset SIM_FAIL_uclk
check '[ $RC = 1 ] && [ "$(calls)" = "uclk pin 456," ] && [ "$(st)" = 0x00001000 ]'
p0pfstub() { printf '#!/bin/sh\necho "p0prefetch $* pair165=${PHXP0_PAIR165:-unset}" >> %s/calls; exit ${SIM_FAIL_p0prefetch:-0}\n' "$W" > "$G/p0prefetch.sh"; chmod +x "$G/p0prefetch.sh"; }
mk two_boot_60a165; two; acer165; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; scstub; runa
check '[ $RC = 0 ] && [ "$(calls)" = "hubp_dlg linux60,uclk pin 456,wm144 linux60a165," ] && grep -q "two displays at 60 + 165" $W/out && grep -q "stopped after" $W/out && ! grep -q unpin $W/calls'
mk two_boot_60a165_p0pf; two; acer165; echo 0x00000400 > "$R/0x4F8A"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; scstub; p0pfstub; runa
check '[ $RC = 0 ] && [ "$(calls)" = "hubp_dlg linux60,uclk pin 456,wm144 linux60a165,p0prefetch linux pair165=1," ] && grep -q "boot stickies before the clear: OPTC0 0x00000400" $W/out && grep -q "^0x4F8A " $R/writes && grep -q "two displays at 60 + 165" $W/out'
mk two_boot_60a165_p0pf_fail; two; acer165; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; scstub; p0pfstub; export SIM_FAIL_p0prefetch=3; runa; unset SIM_FAIL_p0prefetch
check '[ $RC = 0 ] && [ "$(calls)" = "hubp_dlg linux60,uclk pin 456,wm144 linux60a165,p0prefetch linux pair165=1," ] && grep -q "p0prefetch failed: an AOC 144 pick is refused" $W/out && grep -q "two displays at 60 + 165" $W/out && grep -q "stopped after" $W/out'
mk two_at144a165_branch; two; at144; acer165; echo 0xE0000AE8 > "$R/0x3AEA"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; runa
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144a165," ] && grep -q "two displays at 144 + 165" $W/out && grep -q "stopped after" $W/out'
mk two_follow_aoc60_to165; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; swflip 4 0x0000081F 0x00000455 0x00000000; runa; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && [ "$(calls)" = "wm144 linux2,hubp_dlg linux60,uclk unpin,dal SetFclkSwitchAllow 1,uclk pin 456,wm144 linux60a165," ] && grep -q "AOC 60 + Acer 165" $W/out && ! grep -q underflow $W/out'
mk two_follow_aoc60_from165; two; acer165; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; swflip 6 0x00000807 0x00000449 0x00000000; runa; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && [ "$(calls)" = "hubp_dlg linux60,uclk pin 456,wm144 linux60a165,wm144 linux2," ] && grep -q "back at 60" $W/out && [ "$(st)" = 0x00001002 ]'
mk two_follow_aoc60_165_p1busy; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; export SWBUSY=1; swflip 1 0x0000081F 0x00000455 0x00000000; runa; unset PHXB15_SWSTAT SWBUSY
check '[ $RC = 0 ] && ! grep -q linux60a165 $W/calls && ! grep -q "Acer 165" $W/out'
mk two_follow_144_to165; two; at144; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; swflip 6 0x0000081F 0x00000455 0xE0000AE8; runa; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144x2,uclk pin 456,wm144 linux144a165," ] && grep -q "AOC 144 + Acer 165" $W/out && ! grep -q underflow $W/out'
mk two_follow_165_to144; two; at144; acer165; echo 0xE0000AE8 > "$R/0x3AEA"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; swflip 6 0x00000807 0x00000449 0xE0000A14; runa; unset PHXB15_SWSTAT
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144a165,uclk pin 456,wm144 linux144x2," ] && grep -q "AOC 144 + Acer 144" $W/out'
waitfor() { [ $SKIP = 1 ] && return 0; i=0; while [ $i -lt 60 ] && ! grep -q "$1" "$2" 2>/dev/null; do sleep 0.05; i=$((i + 1)); done; }
mk t7_request; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aoc144"; scstub
printf '#!/bin/sh\necho 501\n' > "$W/console"; printf '#!/bin/sh\nshift 5\nexec "$@"\n' > "$W/asuser"
printf '#!/bin/sh\necho "cgmode $*" >> %s/calls\n[ "$1" = list ] && printf "display 0: id 478249941 model 0x74d\\ndisplay 1: id 69734662 model 0x2402 main\\n"\n[ "$1" = set ] && echo "read back @144.00 id 2"\nexit 0\n' "$W" > "$G/cgmode"
chmod +x "$W/console" "$W/asuser" "$G/cgmode"; export PHXB15_CONSOLE=$W/console PHXB15_ASUSER=$W/asuser; runa; waitfor "aoc: cgmode set" "$W/out"; unset PHXB15_CONSOLE PHXB15_ASUSER
check '[ $RC = 0 ] && grep -q "sysctl -w debug.phxpipe0_enable=1" $W/sysctl.log && grep -q "144 asked once a user" $W/out && grep -q "cgmode set 69734662 1920 1080 144" $W/calls && grep -q "aoc: cgmode set 69734662 1920 1080 144 rc 0" $W/out'
mk t7_noconsole; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aoc144"; scstub
printf '#!/bin/sh\necho 0\n' > "$W/console"; chmod +x "$W/console"; export PHXB15_CONSOLE=$W/console PHXB15_AOCWAIT=0; runa; waitfor "no console user" "$W/out"; unset PHXB15_CONSOLE PHXB15_AOCWAIT
check '[ $RC = 0 ] && grep -q "no console user within 0s" $W/out && ! grep -q cgmode $W/calls'
# Boot rate (11-02): `aocboot` (the AOC at 144 before the daemon's WindowServer
# restart), `aocback`, the 144 branch arming the engine, the after-login request only as a boot-rate fallback, the memory.
sc0stub() {   # sysctl: logs; -n debug.phxpipe0_enable exists (SIM_NOP0=1: not); -w debug.phxpipe0=4 = the kext's set to the last
  # debug.phxpipe0_mode: OTG0's totals + (at 144) the HUBP0 group of the plan for OTG1 (SIM_P0FAIL=1: refused, nothing moves)
  cat > "$W/sysctl" <<EOS
#!/bin/sh
R=$R; W=$W
echo "sysctl \$*" >> \$W/sysctl.log
rd() { RV=\$2; [ -r "\$1" ] && IFS= read -r RV < "\$1"; return 0; }   # = \$(cat <file> 2>/dev/null || echo <default>), no process
if [ "\$1" = -n ]; then [ "\$2" = debug.phxpipe0_enable ] && [ "\${SIM_NOP0:-0}" = 1 ] && exit 1; [ "\$2" = debug.phxpipe0_solo ] && [ "\${SIM_NOSOLO:-0}" = 1 ] && exit 1; echo 1; exit 0; fi
case "\$2" in
  debug.phxpipe0_mode=*) echo "\${2#*=}" > \$W/p0mode ;;
  debug.phxpipe0=4) [ "\${SIM_P0FAIL:-0}" = 1 ] && { echo "sysctl: debug.phxpipe0=4: Invalid argument" >&2; exit 1; }
    rd \$W/p0mode ""; if [ "\$RV" = 144 ]; then echo 0x00000807 > \$R/0x4FEA; echo 0x0000044B > \$R/0x4FEF
      for x in 0x3B05:0x00013BA8 0x3B1C:0x00013A6D 0x3B19:0x00000126 0x3B1A:0x00000126 0x3AEB:0x0800274D 0x3AEC:0x0000274D 0x3AE9:0x04EE0000 0x3AEA:0xE0000A14; do
        echo "\${x#*:}" > "\$R/\${x%%:*}"; done
      rd \$R/0x506A ""; [ "\$RV" = 0x0000081F ] && echo 0xE0000AE8 > \$R/0x3AEA
    else echo 0x00000897 > \$R/0x4FEA; echo 0x00000464 > \$R/0x4FEF; fi ;;
esac
exit 0
EOS
  chmod +x "$W/sysctl"; }
at60b() { two; echo 0x00000464 > "$R/0x4FEF"; echo 0x00000807 > "$R/0x506A"; echo 0x00000449 > "$R/0x506F"; echo 0x00000400 > "$R/0x4F8A"; }   # AOC 60 + Acer 144, the boot's ODM0 sticky set
sclog() { [ -e "$W/sysctl.log" ] && tr '\n' , < "$W/sysctl.log"; }
mk aocboot_144; at60b; p0pfstub; sc0stub; runa aocboot
check '[ $RC = 0 ] && [ "$(calls)" = "wm144 linux2,hubp_dlg linux60,p0prefetch linux pair165=0,uclk pin 456,wm144 linux144x2," ] && [ "$(sclog)" = "sysctl -n debug.phxpipe0_enable,sysctl -w debug.phxpipe0_enable=1,sysctl -w debug.phxpipe0_mode=144,sysctl -w debug.phxpipe0=4," ] && grep -q "^0x4F8A " $R/writes && grep -q "^0x4F9A " $R/writes && [ "$(cat $R/0x4F8A)" = 0x00000000 ] && grep -q "AOC 144 + Acer 144" $W/out && grep -q "aocboot: the AOC at 144 before the WindowServer restart" $W/out && [ "$(st)" = 0x00001002 ]'
mk aocboot_165; at60b; acer165; p0pfstub; sc0stub; runa aocboot
check '[ $RC = 0 ] && [ "$(calls)" = "wm144 linux60a165,hubp_dlg linux60,p0prefetch linux pair165=1,uclk pin 456,wm144 linux144a165," ] && grep -q "next to the Acer at 165" $W/out && grep -q "AOC 144 + Acer 165" $W/out'
mk aocboot_acer60; at60b; echo 0x00000897 > "$R/0x506A"; echo 0x00000464 > "$R/0x506F"; p0pfstub; sc0stub; runa aocboot
check '[ $RC = 1 ] && [ ! -e $W/calls ] && [ ! -e $W/sysctl.log ] && grep -q "not the Acer at 1080p144 or 1080p165" $W/out && ! grep -q "^0x" $R/writes 2>/dev/null'
mk aocboot_at144; at144; p0pfstub; sc0stub; runa aocboot
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not the AOC at 1080p60" $W/out'
mk aocboot_det3; at60b; echo 0x00000303 > "$R/0x397B"; p0pfstub; sc0stub; runa aocboot
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not 9/9" $W/out'
mk aocboot_stutter; at60b; echo 0x00001000 > "$R/0x39BC"; p0pfstub; sc0stub; runa aocboot
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not the GOP 0x1002" $W/out'
mk aocboot_busy; at60b; p0pfstub; sc0stub; sw0stub 1 2; runa aocboot; unset PHXB15_SW0STAT
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "engine is busy" $W/out'
mk aocboot_old_kext; at60b; p0pfstub; sc0stub; export SIM_NOP0=1; runa aocboot; unset SIM_NOP0
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "kext < b99" $W/out && ! grep -q "\-w" $W/sysctl.log'
mk aocboot_no_p0pf; at60b; sc0stub; runa aocboot
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "p0prefetch.sh missing" $W/out'
mk aocboot_p0pf_fail; at60b; p0pfstub; sc0stub; export SIM_FAIL_p0prefetch=3; runa aocboot; unset SIM_FAIL_p0prefetch
check '[ $RC = 1 ] && [ "$(calls)" = "wm144 linux2,hubp_dlg linux60,p0prefetch linux pair165=0," ] && ! grep -q "\-w debug.phxpipe0" $W/sysctl.log'
mk aocboot_set_refused; at60b; p0pfstub; sc0stub; export SIM_P0FAIL=1; runa aocboot; unset SIM_P0FAIL
check '[ $RC = 1 ] && [ "$(calls)" = "wm144 linux2,hubp_dlg linux60,p0prefetch linux pair165=0," ] && grep -q "after the set: the AOC stays at 60" $W/out && grep -q "debug.phxpipe0=4 (.*) rc 1" $W/out'
mk aocboot_hubp0_off; at60b; p0pfstub; sc0stub; printf '#!/bin/sh\necho "uclk $*" >> %s/calls\necho 0x0002E4BD > %s/0x3B05\n' "$W" "$R" > "$G/uclk.sh"; chmod +x "$G/uclk.sh"; runa aocboot
check '[ $RC = 1 ] && [ "$(calls)" = "wm144 linux2,hubp_dlg linux60,p0prefetch linux pair165=0,uclk pin 456," ] && grep -q "not the 144 plan" $W/out && ! grep -q "before the WindowServer restart" $W/out'
mk aocback_144; at144; p0pfstub; sc0stub; runa aocback
check '[ $RC = 0 ] && [ "$(sclog)" = "sysctl -w debug.phxpipe0_mode=60,sysctl -w debug.phxpipe0=4," ] && [ "$(calls)" = "wm144 linux2," ] && grep -q "aocback: debug.phxpipe0=4 to 60 rc 0, OTG0 0x00000897/0x00000464" $W/out && grep -q "back at 60" $W/out'
mk aocback_165; at144; acer165; echo 0xE0000AE8 > "$R/0x3AEA"; p0pfstub; sc0stub; runa aocback
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux60a165," ] && grep -q "AOC 60 + Acer 165" $W/out'
mk two_at144_arms; two; at144; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; scstub; runa
check '[ $RC = 0 ] && grep -q "sysctl -w debug.phxpipe0_enable=1" $W/sysctl.log && grep -q "the AOC at 144 since the boot" $W/out && ! grep -q "asked once" $W/out && [ "$(calls)" = "uclk pin 456,wm144 linux144x2," ]'
cgstub() { printf '#!/bin/sh\necho 501\n' > "$W/console"; printf '#!/bin/sh\nshift 5\nexec "$@"\n' > "$W/asuser"
  printf '#!/bin/sh\necho "cgmode $*" >> %s/calls\n[ "$1" = list ] && printf "display 0: id 478249941 model 0x74d\\ndisplay 1: id 69734662 model 0x2402 main\\n"\n[ "$1" = set ] && echo "read back @144.00 id 2"\nexit 0\n' "$W" > "$G/cgmode"
  chmod +x "$W/console" "$W/asuser" "$G/cgmode"; export PHXB15_CONSOLE=$W/console PHXB15_ASUSER=$W/asuser; }
mk rate144_fallback_request; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aoc144"; echo 144 > "$G/phxboot-aoc-rate"; scstub; cgstub; runa; waitfor "aoc: cgmode set" "$W/out"; unset PHXB15_CONSOLE PHXB15_ASUSER
check '[ $RC = 0 ] && grep -q "boot rate 144 not in place before the WindowServer restart: 144 asked" $W/out && grep -q "cgmode set 69734662 1920 1080 144" $W/calls'
mk rate144_request_rotated; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aoc144"; echo 144 > "$G/phxboot-aoc-rate"; scstub; cgstub
printf '#!/bin/sh\necho "cgmode $*" >> %s/calls\n[ "$1" = list ] && printf "display 0: id 478249941 model 0x74d\\n  1920x1080 @165.00 Hz id 3 flags 0x3 current\\ndisplay 1: id 69734662 model 0x2402 main\\n  1080x1920 @60.00 Hz id -2147479552 flags 0x500003 current\\n  1080x1920 @144.00 Hz id -2147479551 flags 0x500007\\n"\n[ "$1" = set ] && echo "read back @144.00 id 2"\nexit 0\n' "$W" > "$G/cgmode"; runa; waitfor "aoc: cgmode set" "$W/out"; unset PHXB15_CONSOLE PHXB15_ASUSER
check '[ $RC = 0 ] && grep -q "cgmode set 69734662 1080 1920 144" $W/calls && grep -q "aoc: cgmode set 69734662 1080 1920 144 rc 0" $W/out'
mk rate60_no_request; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aoc144"; echo 60 > "$G/phxboot-aoc-rate"; scstub; cgstub; runa; [ $SKIP = 1 ] || sleep 0.3; unset PHXB15_CONSOLE PHXB15_ASUSER
check '[ $RC = 0 ] && grep -q "boot rate 60 (.*phxboot-aoc-rate): no request" $W/out && ! grep -q cgmode $W/calls && grep -q "pipe-0 engine armed" $W/out'
mk ratelast60_no_request; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; echo last > "$G/phxboot-aoc-rate"; echo "aoc=60 acer=144 at x" > "$G/phxboot-rate-last"; scstub; cgstub; runa; [ $SKIP = 1 ] || sleep 0.3; unset PHXB15_CONSOLE PHXB15_ASUSER
check '[ $RC = 0 ] && grep -q "boot rate 60" $W/out && ! grep -q cgmode $W/calls'
mk ratelast_none_request; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; echo last > "$G/phxboot-aoc-rate"; scstub; cgstub; MAXPOLL=1; runa; unset MAXPOLL; waitfor "aoc: cgmode set" "$W/out"; unset PHXB15_CONSOLE PHXB15_ASUSER
check '[ $RC = 0 ] && grep -q "boot rate 144 not in place" $W/out && grep -q "cgmode set 69734662" $W/calls'
runm() { [ $SKIP = 1 ] && return 0; ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=20 PHXB15_SYSCTL=$W/sysctl PHXB15_AOCSETTLE=0 PHXB15_RATESTABLE=0 PHXB15_RMEMN=1; sh "$B15" "$@" ) > "$W/out" 2>&1; RC=$?; }
mk remember_60_144; two; echo 0x00000464 > "$R/0x4FEF"; echo 0x00000807 > "$R/0x506A"; echo 0x00000449 > "$R/0x506F"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; echo last > "$G/phxboot-m7-rate"; scstub; runm
check '[ $RC = 0 ] && grep -q "^aoc=60 acer=144 at " $G/phxboot-rate-last && [ "$(grep -c "rates remembered" $W/out)" = 1 ] && [ ! -e $G/phxboot-rate-last.tmp ]'
mk remember_not_stable; two; echo 0x00000464 > "$R/0x4FEF"; echo 0x00000807 > "$R/0x506A"; echo 0x00000449 > "$R/0x506F"; echo last > "$G/phxboot-m7-rate"; scstub
[ $SKIP = 1 ] || ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=20 PHXB15_SYSCTL=$W/sysctl PHXB15_RATESTABLE=3600 PHXB15_RMEMN=1; sh "$B15" watch2 ) > "$W/out" 2>&1; RC=$?
check '[ $RC = 0 ] && [ ! -e $G/phxboot-rate-last ]'
mk remember_off; two; echo 0x00000464 > "$R/0x4FEF"; echo 0x00000807 > "$R/0x506A"; echo 0x00000449 > "$R/0x506F"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; echo 144 > "$G/phxboot-m7-rate"; scstub; runm
check '[ $RC = 0 ] && [ ! -e $G/phxboot-rate-last ] && ! grep -q remembered $W/out'
mk remember_same_no_write; two; echo 0x00000464 > "$R/0x4FEF"; echo 0x00000807 > "$R/0x506A"; echo 0x00000449 > "$R/0x506F"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; echo last > "$G/phxboot-aoc-rate"; echo "aoc=60 acer=144 at old" > "$G/phxboot-rate-last"; scstub; runm
check '[ $RC = 0 ] && [ "$(cat $G/phxboot-rate-last)" = "aoc=60 acer=144 at old" ] && ! grep -q remembered $W/out'
mk remember_follow_144_165; two; at144; acer165; echo 0xE0000AE8 > "$R/0x3AEA"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; echo last > "$G/phxboot-aoc-rate"; echo "aoc=60 acer=144 at old" > "$G/phxboot-rate-last"; scstub; runm
check '[ $RC = 0 ] && grep -q "^aoc=144 acer=165 at " $G/phxboot-rate-last && grep -q "rates remembered for the next boot: aoc=144 acer=165" $W/out'
mk remember_unknown_otg0; two; echo 0x0000089F > "$R/0x4FEA"; echo 0x00000807 > "$R/0x506A"; echo 0x00000449 > "$R/0x506F"; echo last > "$G/phxboot-m7-rate"; scstub; runm watch2
check '[ ! -e $G/phxboot-rate-last ] && ! grep -q remembered $W/out'
mk remember_busy; two; echo 0x00000464 > "$R/0x4FEF"; echo 0x00000807 > "$R/0x506A"; echo 0x00000449 > "$R/0x506F"; echo last > "$G/phxboot-m7-rate"; scstub; sw0stub 1 2; runm watch2; unset PHXB15_SW0STAT
check '[ ! -e $G/phxboot-rate-last ]'
mk remember_watch2_60_165; two; acer165; echo 0x00000464 > "$R/0x4FEF"; echo 0x00001000 > "$R/0x39BC"; echo last > "$G/phxboot-m7-rate"; scstub; runm watch2
check '[ $RC = 0 ] && grep -q "^aoc=60 acer=165 at " $G/phxboot-rate-last'
# p0solo (11-03): the AOC alone (OTG1-3 dark). aocboot1 before the
# daemon's restart, the one-display AOC branch after `done` (at 144 / at 60), watch1, aocback with one display; the Acer
# alone on pipe 0 (OTG0 0x807/0x449) keeps the one-display recipe.
p0pf1stub() { printf '#!/bin/sh\necho "p0prefetch $* solo=${PHXP0_SOLO:-unset}" >> %s/calls; exit ${SIM_FAIL_p0prefetch:-0}\n' "$W" > "$G/p0prefetch.sh"; chmod +x "$G/p0prefetch.sh"; }
alone60() { echo 0x00000897 > "$R/0x4FEA"; echo 0x00000464 > "$R/0x4FEF"; echo 0x00000303 > "$R/0x397B"; echo 0x00000400 > "$R/0x4F8A"; }   # the AOC alone at 60, GOP DET0 3, the boot's ODM0 sticky
alone144() { echo 0x00000807 > "$R/0x4FEA"; echo 0x0000044B > "$R/0x4FEF"; echo 0x00000909 > "$R/0x397B"
  for x in 0x3B05:0x00013BA8 0x3B1C:0x00013A6D 0x3B19:0x00000126 0x3B1A:0x00000126 0x3AEB:0x0800274D 0x3AEC:0x0000274D 0x3AE9:0x04EE0000 0x3AEA:0xE0000A14; do
    echo "${x#*:}" > "$R/${x%%:*}"; done; }
# det.sh stub that really sets DET0 (the engine and aoc144solo read it)
detstub() { printf '#!/bin/sh\necho "det $*" >> %s/calls\n[ "$1" = set ] && echo 0x00000909 > %s/0x397B\nexit ${SIM_FAIL_det:-0}\n' "$W" "$R" > "$G/det.sh"; chmod +x "$G/det.sh"; }
mk aocboot1_ok; alone60; p0pf1stub; detstub; sc0stub; runa aocboot1
check '[ $RC = 0 ] && [ "$(calls)" = "det set 9,hubp_dlg linux60,p0prefetch linux solo=1,uclk pin 456,wm144 linux," ] && grep -q "sysctl -w debug.phxpipe0_solo=1" $W/sysctl.log && grep -q "sysctl -w debug.phxpipe0=4" $W/sysctl.log && grep -q "AOC 144 alone: UCLK pinned 456, watermarks linux (one display)" $W/out && grep -q "aocboot1: the AOC (alone) at 144 before the WindowServer restart" $W/out && [ "$(st)" = 0x00001002 ] && grep -q "^0x4F8A " $R/writes'
mk aocboot1_otg1_lit; alone60; echo 0x00000001 > "$R/0x5081"; p0pf1stub; detstub; sc0stub; runa aocboot1
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "a second pipe is lit" $W/out && ! grep -q "\-w debug.phxpipe0" $W/sysctl.log'
mk aocboot1_no_solo; alone60; p0pf1stub; detstub; sc0stub; export SIM_NOSOLO=1; runa aocboot1; unset SIM_NOSOLO
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "no debug.phxpipe0_solo" $W/out'
mk aocboot1_at144; alone144; p0pf1stub; detstub; sc0stub; runa aocboot1
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "is not the AOC at 1080p60" $W/out'
mk aocboot1_stutter; alone60; echo 0x00001000 > "$R/0x39BC"; p0pf1stub; detstub; sc0stub; runa aocboot1
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not the GOP 0x1002" $W/out'
mk aocboot1_det_fail; alone60; p0pf1stub; detstub; sc0stub; export SIM_FAIL_det=1; runa aocboot1; unset SIM_FAIL_det
check '[ $RC = 1 ] && [ "$(calls)" = "det set 9," ] && ! grep -q "\-w debug.phxpipe0" $W/sysctl.log'
mk aocboot1_p0pf_fail; alone60; p0pf1stub; detstub; sc0stub; export SIM_FAIL_p0prefetch=3; runa aocboot1; unset SIM_FAIL_p0prefetch
check '[ $RC = 1 ] && [ "$(calls)" = "det set 9,hubp_dlg linux60,p0prefetch linux solo=1," ] && ! grep -q "\-w debug.phxpipe0" $W/sysctl.log'
mk aocboot1_set_refused; alone60; p0pf1stub; detstub; sc0stub; export SIM_P0FAIL=1; runa aocboot1; unset SIM_P0FAIL
check '[ $RC = 1 ] && grep -q "aocboot1: OTG0 .* after the set: the AOC stays at 60" $W/out && ! grep -q "wm144" $W/calls'
mk aocboot1_hubp0_off; alone60; p0pf1stub; detstub; sc0stub; printf '#!/bin/sh\necho "uclk $*" >> %s/calls\necho 0x0002E4BD > %s/0x3B05\n' "$W" "$R" > "$G/uclk.sh"; chmod +x "$G/uclk.sh"; runa aocboot1
check '[ $RC = 1 ] && grep -q "not the 1080p144 plan" $W/out && ! grep -q "wm144" $W/calls && ! grep -q "before the WindowServer restart" $W/out'
mk aocback_alone; alone144; p0pf1stub; detstub; sc0stub; runa aocback
check '[ $RC = 0 ] && grep -q "AOC back at 60 alone" $W/out && ! grep -q "wm144" $W/calls 2>/dev/null'
# after `done`: the one-display AOC branch
mk one_aoc_144; alone144; touch "$G/phxboot-m7-aocmodes"; p0pf1stub; detstub; sc0stub; MAXPOLL=3; runa; unset MAXPOLL
check '[ $RC = 0 ] && [ "$(calls)" = "uclk pin 456,wm144 linux," ] && grep -q "one display (the AOC) at 144" $W/out && grep -q "sysctl -w debug.phxpipe0_solo=1" $W/sysctl.log && grep -q "stopped after 3" $W/out && [ "$(st)" = 0x00001002 ]'
mk one_aoc_60_rate144; alone60; touch "$G/phxboot-m7-aocmodes"; echo 144 > "$G/phxboot-aoc-rate"; p0pf1stub; detstub; sc0stub; export PHXB15_CONSOLE=true PHXB15_AOCWAIT=0; MAXPOLL=3; runa; unset MAXPOLL; unset PHXB15_CONSOLE PHXB15_AOCWAIT
check '[ $RC = 0 ] && [ "$(calls)" = "det set 9,hubp_dlg linux60,p0prefetch linux solo=1," ] && grep -q "pipe-0 engine armed with solo" $W/out && grep -q "144 asked once a user is logged in" $W/out && grep -q "one display (the AOC) at 60" $W/out && [ "$(st)" = 0x00001002 ]'
mk one_aoc_60_no_solo; alone60; touch "$G/phxboot-m7-aocmodes"; p0pf1stub; detstub; sc0stub; export SIM_NOSOLO=1; runa; unset SIM_NOSOLO
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "without debug.phxpipe0_solo: the AOC stays at 60" $W/out'
mk one_aoc_60_noflag; alone60; p0pf1stub; detstub; sc0stub; runa
check '[ $RC = 1 ] && [ ! -e $W/calls ] && grep -q "not the 1080p144 0x807 the recipe is for: skipped" $W/out'
mk acer_alone_recipe; echo 0x00000807 > "$R/0x4FEA"; echo 0x00000449 > "$R/0x4FEF"; touch "$G/phxboot-m7-aocmodes"; sc0stub; run
check '[ $RC = 0 ] && [ "$(cat $W/calls | tr "\n" ,)" = "wm144 linux,det set 9,dalwm ,hubp_dlg linux," ] && [ "$(st)" = 0x00001000 ]'
mk watch1_underflow; alone144; echo 0x10000000 > "$R/0x3AB3"; sc0stub; [ $SKIP = 1 ] || ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=5 PHXB15_SYSCTL=$W/sysctl; sh "$B15" watch1 ) > "$W/out" 2>&1; RC=$?
check '[ $RC = 2 ] && grep -q "hubp0 underflow: stutter forced off" $W/out && grep -q "uclk pin 456" $W/calls && grep -q "^0x39BC 0x00001002" $R/writes'
mk watch1_busy_skip; alone144; echo 0x10000000 > "$R/0x3AB3"; sc0stub; sw0stub 1 2; [ $SKIP = 1 ] || ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=4 PHXB15_SYSCTL=$W/sysctl; sh "$B15" watch1 ) > "$W/out" 2>&1; RC=$?; unset PHXB15_SW0STAT
check '[ $RC = 0 ] && ! grep -q underflow $W/out && grep -q "stopped after 4" $W/out'
# Acer relight (11-03): after an Acer hot-plug event (debug.phxhotplug_stat
# online / head-1 plugs up) watch2 reads the SCDC once and, pipe 1 at 165 with a dark SCDC, runs FS-2's 144 -> lock x2 -> 165
# -> scrambled lock (else back to 144). The sysctl stub: the stat (now_ms = 250 ms per read; SIM_EV_AT = the read from which
# plugs and online are 2, SIM_PEND_UNTIL = pend=online and online still 1 before that read, SIM_USER60_AT = macOS sets the Acer
# to 60 at that read, SIM_NOSTAT = no stat, SIM_ARM_AT = "never armed" before that read), debug.phxscdc (SIM_SCDC0 = the sink at 165 before any switch: dark / lit / l144
# (locked, not scrambled) / bad (rc=5); at 144: locked (SIM_NOLOCK144 = never, SIM_FLICKER144 = every other read); at 165
# after a switch: lit (SIM_FAIL165 = dark, SIM_LATE165=<k> = dark on its first k reads); the settle re-set's switches (the
# 3rd and 4th): SIM_RSNOLOCK=1 = never locked at 144, SIM_RSFAIL165=1 = dark at 165; SIM_EV2_AT = a second plug from that read), debug.phxpipe2_switch (read = the mode; a write sets OTG1's totals and, the AOC
# at 144, HUBP0's pair-plan MIN_TTU, as the kext's pair switch; SIM_SWFAIL=<hz> refuses that write).
# The macOS path (11-04): ~/gpu/cgmode in the console user's session ($W/uid, 501; 0 = the login
# window), its calls in $W/cgmode.log (not $W/calls). list = the AOC (display 0, 1920x1080, first, with its own current
# line) and the Acer (model 0x74d, its modes at 1920x1080, SIM_CGROT=1 1080x1920, "current" at pipe 1's mode; SIM_CGNOACER=1
# = not listed). set <id> <w> <h> <hz> = the kext's switch through the sysctl stub's write (logged "via=cgmode", so swlog and
# the register effects are the same) and the stat's clock 4 s on (cgmode blocks ~4 s); the size not listed = rc 2,
# SIM_CGFAIL=<hz> = rc 1 without a switch, SIM_CGNOOP=1 = rc 0 without a switch, a refused write (SIM_SWFAIL) = rc 1.
rlstub() {
  cat > "$W/sysctl" <<'EOS'
#!/bin/sh
R=@R@; W=@W@
echo "sysctl $*${RLVIA:+ via=$RLVIA}" >> $W/sysctl.log
rd() { RV=$2; [ -r "$1" ] && IFS= read -r RV < "$1"; return 0; }   # = $(cat <file> 2>/dev/null || echo <default>), no process
rd $W/p1mode ""; mode=$RV
LIT="rc=0 sink_version=1 tmds_config=0x03 scrambler=0x01 flags0=0x0f locked=1 err0=0 err1=0 err2=0 status=0x0 step=0x00 stray=0"
DARK="rc=0 sink_version=1 tmds_config=0x00 scrambler=0x00 flags0=0x01 locked=0 err0=32767 err1=32767 err2=32767 status=0x0 step=0x00 stray=0"
L144="rc=0 sink_version=1 tmds_config=0x00 scrambler=0x00 flags0=0x0f locked=1 err0=0 err1=0 err2=0 status=0x0 step=0x00 stray=0"
BAD="rc=5 sink_version=0 tmds_config=0x00 scrambler=0x00 flags0=0x00 locked=0 err0=-1 err1=-1 err2=-1 status=0x2 step=0x03 stray=0"
case "$1 $2" in
  "-n debug.phxhotplug_stat")
    [ "${SIM_NOSTAT:-0}" = 1 ] && exit 1
    rd $W/hpn 0; c=$((RV + 1)); echo $c > $W/hpn
    [ -n "${SIM_ARM_AT:-}" ] && [ $c -lt $SIM_ARM_AT ] && { echo "hotplug v1 level=0 never armed"; exit 0; }
    on=1; pl=1; pend=none; why=-
    if [ -n "${SIM_EV_AT:-}" ] && [ $c -ge $SIM_EV_AT ]; then pl=2; on=2
      why="plug: the same monitor, pipe 1 lit: head 1 online"   # the kext's text after a plug (box 11-04 13:01)
      [ -n "${SIM_PEND_UNTIL:-}" ] && [ $c -lt $SIM_PEND_UNTIL ] && { on=1; pend=online; why=-; }; fi
    [ -n "${SIM_EV2_AT:-}" ] && [ $c -ge $SIM_EV2_AT ] && { pl=3; on=3; }
    [ -n "${SIM_USER60_AT:-}" ] && [ $c = "$SIM_USER60_AT" ] && { echo 60 > $W/p1mode; echo 0x00000897 > $R/0x506A; echo 0x00000464 > $R/0x506F; }
    rd $W/clk 0; echo "hotplug v1 level=2 latched=0 now_ms=$((1000000 + 250 * c + RV)) h1_hpd=0 h1_src=0x10 polls=$c bad=0 holds=0 busy=0 offline=1 online=$on refused=0 fails=0 cancels=0 edid_reads=1 edid_bad=0 own=0 pend=$pend why=$why"
    echo "hpd0 conn=HDMI head1 state=high cand=high plugs=$pl unplugs=1 glitches=0"
    echo "hpd2 conn=DP state=high cand=high plugs=0 unplugs=0 glitches=0"
    echo "ev 0 t_ms=1 hpd0 unplugged act=want h1=1" ;;
  "-n debug.phxscdc")
    rd $W/swn 0; sw=$RV; rd $W/hpn ""; echo "scdc hpn=$RV mode=$mode sw=$sw" >> $W/scdc.log
    if [ "$mode" = 165 ] && [ $sw = 0 ]; then case ${SIM_SCDC0:-dark} in dark) echo "$DARK" ;; lit) echo "$LIT" ;; l144) echo "$L144" ;; bad) echo "$BAD" ;; esac
    elif [ "$mode" = 144 ]; then rd $W/fl 0; f=$((RV + 1)); echo $f > $W/fl
      if [ "${SIM_NOLOCK144:-0}" = 1 ] || { [ "${SIM_RSNOLOCK:-0}" = 1 ] && [ $sw -ge 3 ]; }; then echo "$DARK"; elif [ "${SIM_FLICKER144:-0}" = 1 ] && [ $((f % 2)) = 0 ]; then echo "$DARK"; else echo "$L144"; fi
    elif [ "${SIM_FAIL165:-0}" = 1 ] || { [ "${SIM_RSFAIL165:-0}" = 1 ] && [ $sw -ge 4 ]; }; then echo "$DARK"
    elif [ -n "${SIM_LATE165:-}" ]; then rd $W/l165 0; f=$((RV + 1)); echo $f > $W/l165
      if [ $f -le $SIM_LATE165 ]; then echo "$DARK"; else echo "$LIT"; fi
    else echo "$LIT"; fi ;;
  "-n debug.phxpipe2_switch") echo "$mode" ;;
  "-w debug.phxpipe2_switch="*)
    v=${2#*=}; [ "${SIM_SWFAIL:-}" = "$v" ] && exit 1
    rd $W/swtry 0; wn=$((RV + 1)); echo $wn > $W/swtry   # SIM_SWFAIL_UNTIL=N: writes 1..N-1 refused (pipe 0 out, HP1-P8)
    [ -n "${SIM_SWFAIL_UNTIL:-}" ] && [ $wn -lt $SIM_SWFAIL_UNTIL ] && exit 1
    echo "$v" > $W/p1mode; rd $W/swn 0; echo $((RV + 1)) > $W/swn; rd $W/gen ""; echo $((RV + 1)) > $W/gen
    rd $R/0x4FEA ""; p0=$RV
    case $v in
      165) echo 0x0000081F > $R/0x506A; echo 0x00000455 > $R/0x506F; [ "$p0" = 0x00000807 ] && echo 0xE0000AE8 > $R/0x3AEA ;;
      144) echo 0x00000807 > $R/0x506A; echo 0x00000449 > $R/0x506F; [ "$p0" = 0x00000807 ] && echo 0xE0000A14 > $R/0x3AEA ;;
    esac ;;
esac
exit 0
EOS
  cat > "$G/cgmode" <<'EOS'
#!/bin/sh
W=@W@
echo "cgmode $*" >> $W/cgmode.log
rd() { RV=$2; [ -r "$1" ] && IFS= read -r RV < "$1"; return 0; }   # = $(cat <file> 2>/dev/null || echo <default>), no process
rd $W/p1mode ""; mode=$RV; sz=1920x1080; [ "${SIM_CGROT:-0}" = 1 ] && sz=1080x1920
case "$1" in
  list) printf 'display 0: id 69734662 model 0x2402 main\n  1920x1080 @60.00 Hz id 1 flags 0x100003\n  1920x1080 @144.00 Hz id 2 flags 0x100007 current\n'
    [ "${SIM_CGNOACER:-0}" = 1 ] && exit 0
    echo "display 1: id 478249941 model 0x74d"
    for hz in 60 144 165; do c=""; [ $hz = "$mode" ] && c=" current"; echo "  $sz @$hz.00 Hz id $hz flags 0x3$c"; done ;;
  set) [ "$3x$4" = "$sz" ] || { echo "no $3x$4 @$5 mode on display $2" >&2; exit 2; }
    [ "${SIM_CGFAIL:-}" = "$5" ] && { echo "set: CGError 1001 in 12 ms; read back: $sz @$mode.00 Hz"; exit 1; }
    [ "${SIM_CGNOOP:-0}" = 1 ] && { echo "set: CGError 0 in 30 ms; read back: $sz @$5.00 Hz"; exit 0; }
    rd $W/clk 0; echo $((RV + 4000)) > $W/clk
    RLVIA=cgmode $W/sysctl -w debug.phxpipe2_switch=$5 || { echo "set: CGError 0 in 4041 ms; read back: $sz @$mode.00 Hz"; exit 1; }
    echo "set: CGError 0 in 4041 ms; read back: $sz @$5.00 Hz id $5 flags 0x3 current" ;;
esac
exit 0
EOS
  sed -i '' "s|@R@|$R|; s|@W@|$W|" "$W/sysctl" "$G/cgmode"
  printf '#!/bin/sh\nIFS= read -r g < %s/gen; IFS= read -r m < %s/p1mode\necho "busy=${SWBUSY:-0} gen=$g mode=$m head1=1 last=done stage=7 why=done"\n' "$W" "$W" > "$W/swstat"
  printf '#!/bin/sh\necho "busy=${SW0BUSY:-0} gen=4 enable=1 mode=144 last=done"\n' > "$W/sw0"
  printf '#!/bin/sh\nIFS= read -r u < %s/uid && echo "$u"\n' "$W" > "$W/console"; printf '#!/bin/sh\nshift 5\nexec "$@"\n' > "$W/asuser"; echo 501 > "$W/uid"
  echo 3 > "$W/gen"; chmod +x "$W/sysctl" "$W/swstat" "$W/sw0" "$W/console" "$W/asuser" "$G/cgmode"; }
rlset() {   # the box at P0: AOC 144 + Acer 165 (c3 HUBP0 plan), the follower flag, pipe-1 mode 165
  two; at144; acer165; echo 0xE0000AE8 > "$R/0x3AEA"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; echo 165 > "$W/p1mode"; rlstub; }
runr() { [ $SKIP = 1 ] && return 0; ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=${MAXPOLL:-60} PHXB15_SYSCTL=$W/sysctl PHXB15_SWSTAT=$W/swstat PHXB15_SW0STAT=$W/sw0 PHXB15_CONSOLE=$W/console PHXB15_ASUSER=$W/asuser; sh "$B15" "$@" ) > "$W/out" 2>&1; RC=$?; }
swlog() { grep -o 'debug.phxpipe2_switch=[0-9]*' "$W/sysctl.log" 2>/dev/null | tr '\n' ,; }
# the switches by path: via = the kernel's (k) or cgmode's (m) write, in order ("m144,m165,"); cgsets = cgmode's set calls
via() { sed -n 's/.*-w debug.phxpipe2_switch=\([0-9]*\)\( via=cgmode\)*$/\2 \1/p' "$W/sysctl.log" 2>/dev/null | sed 's/^ via=cgmode /m/; s/^ /k/' | tr '\n' ,; }
cgsets() { grep '^cgmode set' "$W/cgmode.log" 2>/dev/null | cut -d' ' -f3- | tr '\n' ,; }
nscdc() { grep -c 'debug.phxscdc' "$W/sysctl.log" 2>/dev/null; }
scdc1st() { sed -n '1s/^scdc hpn=\([0-9]*\).*/\1/p' "$W/scdc.log"; }
RLF='uclk pin 456,wm144 linux144x2,uclk pin 456,wm144 linux144a165,'   # the follower: 165 -> 144 -> 165 next to the AOC at 144
mk rl_dark_relight; rlset; export SIM_EV_AT=3; runr watch2; unset SIM_EV_AT
check '[ $RC = 0 ] && [ "$(swlog)" = "debug.phxpipe2_switch=144,debug.phxpipe2_switch=165," ] && [ "$(calls)" = "$RLF" ] && grep -q "relight: Acer hot-plug event (online 1 -> 2, head-1 plugs 1 -> 2)" $W/out && grep -q "SCDC dark (rc=0 .*locked=0.*): pipe 1 switched to 144" $W/out && grep -q "SCDC locked twice at 144" $W/out && grep -q "relight: done, pipe 1 at 165" $W/out && [ "$(cat $R/0x506A)" = 0x0000081F ] && [ "$(scdc1st)" -ge 7 ] && [ "$(grep -c "relight:" $W/out)" = 7 ] && grep -q "relight: settle re-set in 20 s (the relight used the macOS path)" $W/out && [ "$(via)" = "m144,m165," ] && grep -q "relight: 144 through macOS (cgmode set 478249941 1920 1080 144 rc 0: " $W/out && grep -q "relight: 165 through macOS (cgmode set 478249941 1920 1080 165 rc 0: " $W/out && ! grep -q underflow $W/out && [ "$(st)" = 0x00001002 ]'   # 7 = 4 + one path line per switch + the scheduled re-set (20 s: not reached in 60 polls)
mk rl_scdc_fine; rlset; export SIM_EV_AT=3 SIM_SCDC0=lit; runr watch2; unset SIM_EV_AT SIM_SCDC0
check '[ $RC = 0 ] && [ -z "$(swlog)" ] && [ "$(nscdc)" = 1 ] && grep -q "SCDC scrambled and locked (.*): nothing to do" $W/out && [ ! -e $W/calls ]'
mk rl_at144; two; at144; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; echo 144 > "$W/p1mode"; rlstub; export SIM_EV_AT=3; runr watch2; unset SIM_EV_AT
check '[ $RC = 0 ] && [ -z "$(swlog)" ] && [ "$(nscdc)" = 0 ] && grep -q "relight: pipe 1 at 144 Hz (OTG1 0x00000807/0x00000449), not 165: nothing to do" $W/out && [ ! -e $W/calls ]'
mk rl_fail165; rlset; export SIM_EV_AT=3 SIM_FAIL165=1; runr watch2; unset SIM_EV_AT SIM_FAIL165
check '[ $RC = 0 ] && [ "$(swlog)" = "debug.phxpipe2_switch=144,debug.phxpipe2_switch=165,debug.phxpipe2_switch=144," ] && grep -q "no scrambled lock at 165 in 3 s (.*): pipe 1 back at 144" $W/out && [ "$(cat $R/0x506A)" = 0x00000807 ] && [ "$(calls)" = "${RLF}uclk pin 456,wm144 linux144x2," ] && ! grep -q "relight: done" $W/out'
mk rl_noevent; rlset; runr watch2
check '[ $RC = 0 ] && [ "$(nscdc)" = 0 ] && [ -z "$(swlog)" ] && [ "$(grep -c phxhotplug_stat $W/sysctl.log)" -ge 10 ] && [ "$(grep -c phxhotplug_stat $W/sysctl.log)" -le 16 ] && ! grep -q "relight" $W/out && [ ! -e $W/calls ]'
mk rl_nolock144; rlset; export SIM_EV_AT=3 SIM_NOLOCK144=1; runr watch2; unset SIM_EV_AT SIM_NOLOCK144
check '[ $RC = 0 ] && [ "$(swlog)" = "debug.phxpipe2_switch=144," ] && grep -q "not locked twice in 10 s at 144 (.*): the Acer stays at 144" $W/out && [ "$(nscdc)" -ge 10 ] && [ "$(nscdc)" -le 12 ] && [ "$(calls)" = "uclk pin 456,wm144 linux144x2," ]'
mk rl_flicker144; rlset; export SIM_EV_AT=3 SIM_FLICKER144=1; runr watch2; unset SIM_EV_AT SIM_FLICKER144
check '[ $RC = 0 ] && [ "$(swlog)" = "debug.phxpipe2_switch=144," ] && grep -q "not locked twice in 10 s" $W/out'
mk rl_p0busy; rlset; export SIM_EV_AT=3 SW0BUSY=1; runr watch2; unset SIM_EV_AT SW0BUSY
check '[ $RC = 0 ] && [ "$(nscdc)" = 0 ] && [ -z "$(swlog)" ] && grep -q "relight: Acer hot-plug event" $W/out && [ "$(grep -c "relight:" $W/out)" = 1 ]'
mk rl_p1busy; rlset; export SIM_EV_AT=3 SWBUSY=1; runr watch2; unset SIM_EV_AT SWBUSY
check '[ $RC = 0 ] && [ "$(nscdc)" = 0 ] && [ -z "$(swlog)" ]'
mk rl_user60; rlset; export SIM_EV_AT=3 SIM_USER60_AT=9; runr watch2; unset SIM_EV_AT SIM_USER60_AT
check '[ $RC = 0 ] && [ "$(swlog)" = "debug.phxpipe2_switch=144," ] && grep -q "relight: pipe 1 at 60 Hz meanwhile (not the relight.s 144): stop" $W/out && [ "$(cat $W/p1mode)" = 60 ]'
mk rl_pend; rlset; export SIM_EV_AT=3 SIM_PEND_UNTIL=14; runr watch2; unset SIM_EV_AT SIM_PEND_UNTIL
check '[ $RC = 0 ] && [ "$(scdc1st)" -ge 18 ] && [ "$(swlog)" = "debug.phxpipe2_switch=144,debug.phxpipe2_switch=165," ] && grep -q "online 1 -> 1, head-1 plugs 1 -> 2" $W/out && grep -q "online 1 -> 2, head-1 plugs 2 -> 2" $W/out && grep -q "relight: done" $W/out'
mk rl_unscrambled_locked; rlset; export SIM_EV_AT=3 SIM_SCDC0=l144; runr watch2; unset SIM_EV_AT SIM_SCDC0
check '[ $RC = 0 ] && [ "$(swlog)" = "debug.phxpipe2_switch=144,debug.phxpipe2_switch=165," ] && grep -q "relight: done" $W/out'
mk rl_unreadable; rlset; export SIM_EV_AT=3 SIM_SCDC0=bad; runr watch2; unset SIM_EV_AT SIM_SCDC0
check '[ $RC = 0 ] && [ "$(grep -c "SCDC not readable (rc=5 .*again in 1 s" $W/out)" = 2 ] && grep -q "SCDC not readable 3 times (rc=5 .*taken as dark" $W/out && [ "$(swlog)" = "debug.phxpipe2_switch=144,debug.phxpipe2_switch=165," ] && grep -q "relight: done, pipe 1 at 165" $W/out && [ "$(calls)" = "$RLF" ]'   # 11-04: unreadable x3 = dark
mk rl_switch_refused; rlset; export SIM_EV_AT=3 SIM_SWFAIL=144 PHXB15_RLRETRY=0; runr watch2; unset SIM_EV_AT SIM_SWFAIL PHXB15_RLRETRY
check '[ $RC = 0 ] && [ "$(swlog)" = "debug.phxpipe2_switch=144,debug.phxpipe2_switch=144," ] && [ "$(via)" = "m144,k144," ] && grep -q "relight: 144 through the kernel (debug.phxpipe2_switch=144): cgmode set 478249941 1920 1080 144 rc 1: " $W/out && grep -q "the switch to 144 not done (done: done) after 0 tries: pipe 1 at 165" $W/out && [ "$(nscdc)" = 1 ] && [ ! -e $W/calls ]'   # the refused switch: cgmode rc 1, then the kernel's write, refused too
# 11-04 HP1-P8: the switch to 144 refused while the AOC was out (preflight: pipe 0) -> retried every PHXB15_RLRETRYMS
mk rl_retry_ok; rlset; export SIM_EV_AT=3 SIM_SWFAIL_UNTIL=3; MAXPOLL=160; runr watch2; unset MAXPOLL SIM_EV_AT SIM_SWFAIL_UNTIL
check '[ $RC = 0 ] && grep -q "the switch to 144 not done (.*again in 5 s (try 1 of 60)" $W/out && grep -q "relight: done, pipe 1 at 165" $W/out && [ "$(grep -c "relight: Acer hot-plug event" $W/out)" = 1 ] && ! grep -q "given up" $W/out'
mk rl_retry_giveup; rlset; export SIM_EV_AT=3 SIM_SWFAIL=144 PHXB15_RLRETRY=2 PHXB15_RLRETRYMS=1000; MAXPOLL=120; runr watch2; unset MAXPOLL SIM_EV_AT SIM_SWFAIL PHXB15_RLRETRY PHXB15_RLRETRYMS
check '[ $RC = 0 ] && grep -q "try 1 of 2" $W/out && grep -q "after 2 tries: .*given up until the next plug" $W/out && ! grep -q "relight: done" $W/out && [ "$(grep -o "debug.phxpipe2_switch=144" $W/sysctl.log | wc -l | tr -d " ")" = 6 ]'
mk rl_retry_event2; rlset; export SIM_EV_AT=3 SIM_SWFAIL=144 PHXB15_RLRETRY=2 PHXB15_RLRETRYMS=1000 SIM_EV2_AT=40; MAXPOLL=200; runr watch2; unset MAXPOLL SIM_EV_AT SIM_SWFAIL PHXB15_RLRETRY PHXB15_RLRETRYMS SIM_EV2_AT
check '[ $RC = 0 ] && [ "$(grep -c "try 1 of 2" $W/out)" = 2 ] && [ "$(grep -c "given up until the next plug" $W/out)" = 2 ] && [ "$(grep -c "relight: Acer hot-plug event" $W/out)" = 2 ]'   # a new plug after giving up = a fresh set of tries
mk rl_retry_nack; rlset; export SIM_EV_AT=3 SIM_SCDC0=bad SIM_SWFAIL_UNTIL=3; MAXPOLL=200; runr watch2; unset MAXPOLL SIM_EV_AT SIM_SCDC0 SIM_SWFAIL_UNTIL
check '[ $RC = 0 ] && [ "$(grep -c "SCDC not readable (rc=5 .*again in 1 s" $W/out)" = 4 ] && [ "$(grep -c "SCDC not readable 3 times" $W/out)" = 2 ] && grep -q "relight: done, pipe 1 at 165" $W/out'   # each retry re-reads the SCDC 3 times


mk rl_nostat; rlset; export SIM_EV_AT=3 SIM_NOSTAT=1; runr watch2; unset SIM_EV_AT SIM_NOSTAT
check '[ $RC = 0 ] && grep -q "no debug.phxhotplug_stat (PhxBringup < b102): the Acer relight is off" $W/out && [ "$(nscdc)" = 0 ] && [ "$(grep -c phxhotplug_stat $W/sysctl.log)" = 2 ]'   # rl_init: hpstat + the one absent-vs-unarmed read
mk rl_arm_late; rlset; export SIM_ARM_AT=3 SIM_EV_AT=5; MAXPOLL=90; runr watch2; unset MAXPOLL SIM_ARM_AT SIM_EV_AT
check '[ $RC = 0 ] && grep -q "relight: hot-plug not armed (debug.phxhotplug_stat level=0)" $W/out && grep -q "relight: hot-plug armed (online 1, head-1 plugs 1): the Acer relight is on" $W/out && grep -q "relight: Acer hot-plug event (online 1 -> 2, head-1 plugs 1 -> 2)" $W/out && grep -q "relight: done, pipe 1 at 165" $W/out && [ "$(swlog)" = "debug.phxpipe2_switch=144,debug.phxpipe2_switch=165," ] && [ "$(calls)" = "$RLF" ] && [ "$(grep -c "relight: Acer hot-plug event" $W/out)" = 1 ] && ! grep -q "relight is off" $W/out'
mk rl_never_armed; rlset; export SIM_ARM_AT=1000 SIM_EV_AT=3; runr watch2; unset SIM_ARM_AT SIM_EV_AT
check '[ $RC = 0 ] && grep -q "relight: hot-plug not armed" $W/out && ! grep -q "relight: hot-plug armed\|relight: Acer hot-plug event\|relight is off" $W/out && [ "$(nscdc)" = 0 ] && [ -z "$(swlog)" ] && [ "$(grep -c phxhotplug_stat $W/sysctl.log)" -ge 10 ] && [ "$(grep -c phxhotplug_stat $W/sysctl.log)" -le 16 ]'
mk rl_boot_branch; rlset; export SIM_EV_AT=3; runr; unset SIM_EV_AT
check '[ $RC = 0 ] && grep -q "two displays at 144 + 165" $W/out && [ "$(calls)" = "uclk pin 456,wm144 linux144a165,$RLF" ] && grep -q "relight: done" $W/out'
# The macOS path (11-04): every relight switch through cgmode with a console user; the kernel's
# write without one (login window), without cgmode, the Acer not listed, or for the one step whose cgmode set failed or left
# pipe 1 off the asked rate; a rotated Acer asked at its 1080x1920 size; the lock / verify clocks start after cgmode's 4 s.
CG144='478249941 1920 1080 144'; CG165='478249941 1920 1080 165'
mk rl_macos_all; rlset; export SIM_EV_AT=3 SIM_FAIL165=1; runr watch2; unset SIM_EV_AT SIM_FAIL165
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165,m144," ] && [ "$(cgsets)" = "$CG144,$CG165,$CG144," ] && [ "$(grep -c "^cgmode list" $W/cgmode.log)" = 3 ] && [ "$(grep -c "relight: 1[46][45] through macOS (cgmode set 478249941 1920 1080 1[46][45] rc 0: set: CGError 0" $W/out)" = 3 ] && ! grep -q "through the kernel" $W/out && grep -q "no scrambled lock at 165 in 3 s (.*): pipe 1 back at 144 (done: done)" $W/out && [ "$(cat $R/0x506A)" = 0x00000807 ] && [ "$(calls)" = "${RLF}uclk pin 456,wm144 linux144x2," ] && ! grep -q "meanwhile" $W/out'
mk rl_noconsole; rlset; echo 0 > "$W/uid"; export SIM_EV_AT=3; runr watch2; unset SIM_EV_AT
check '[ $RC = 0 ] && [ "$(via)" = "k144,k165," ] && [ ! -e $W/cgmode.log ] && grep -q "relight: 144 through the kernel (debug.phxpipe2_switch=144): no console user" $W/out && grep -q "relight: 165 through the kernel (debug.phxpipe2_switch=165): no console user" $W/out && grep -q "relight: done, pipe 1 at 165" $W/out && [ "$(calls)" = "$RLF" ]'
mk rl_noconsole_empty; rlset; : > "$W/uid"; export SIM_EV_AT=3; runr watch2; unset SIM_EV_AT
check '[ $RC = 0 ] && [ "$(via)" = "k144,k165," ] && [ ! -e $W/cgmode.log ] && [ "$(grep -c "through the kernel (debug.phxpipe2_switch=1[46][45]): no console user" $W/out)" = 2 ] && grep -q "relight: done" $W/out'
mk rl_nocgmode; rlset; rm -f "$G/cgmode"; export SIM_EV_AT=3; runr watch2; unset SIM_EV_AT
check '[ $RC = 0 ] && [ "$(via)" = "k144,k165," ] && grep -q "relight: 144 through the kernel (debug.phxpipe2_switch=144): no .*/gpu/cgmode" $W/out && grep -q "relight: done" $W/out'
mk rl_cg_noacer; rlset; export SIM_EV_AT=3 SIM_CGNOACER=1; runr watch2; unset SIM_EV_AT SIM_CGNOACER
check '[ $RC = 0 ] && [ "$(via)" = "k144,k165," ] && [ -z "$(cgsets)" ] && [ "$(grep -c "^cgmode list" $W/cgmode.log)" = 2 ] && [ "$(grep -c "through the kernel (debug.phxpipe2_switch=1[46][45]): the Acer (model 0x74d) is not in cgmode list" $W/out)" = 2 ] && grep -q "relight: done" $W/out && [ "$(calls)" = "$RLF" ]'
mk rl_cg_model; rlset; export SIM_EV_AT=3 PHXB15_ACERMODEL=0x999; runr watch2; unset SIM_EV_AT PHXB15_ACERMODEL
check '[ $RC = 0 ] && [ "$(via)" = "k144,k165," ] && [ -z "$(cgsets)" ] && grep -q "through the kernel (debug.phxpipe2_switch=144): the Acer (model 0x999) is not in cgmode list" $W/out'
mk rl_cg_fail; rlset; export SIM_EV_AT=3 SIM_CGFAIL=165; runr watch2; unset SIM_EV_AT SIM_CGFAIL
check '[ $RC = 0 ] && [ "$(via)" = "m144,k165," ] && [ "$(cgsets)" = "$CG144,$CG165," ] && grep -q "relight: 144 through macOS (cgmode set $CG144 rc 0: " $W/out && grep -q "relight: 165 through the kernel (debug.phxpipe2_switch=165): cgmode set $CG165 rc 1: set: CGError 1001" $W/out && grep -q "SCDC locked twice at 144 (.*): pipe 1 switched to 165 (done: done)" $W/out && grep -q "relight: done, pipe 1 at 165" $W/out && [ "$(calls)" = "$RLF" ]'
mk rl_cg_noop; rlset; export SIM_EV_AT=3 SIM_CGNOOP=1; runr watch2; unset SIM_EV_AT SIM_CGNOOP
check '[ $RC = 0 ] && [ "$(via)" = "k144,k165," ] && [ "$(cgsets)" = "$CG144,$CG165," ] && grep -q "relight: 144 through the kernel (debug.phxpipe2_switch=144): cgmode set $CG144 rc 0: .*; pipe 1 at 165 after it (done: done)" $W/out && grep -q "relight: 165 through the kernel (debug.phxpipe2_switch=165): cgmode set $CG165 rc 0: .*; pipe 1 at 144 after it" $W/out && grep -q "relight: done" $W/out && ! grep -q "meanwhile" $W/out'
mk rl_rotated; rlset; export SIM_EV_AT=3 SIM_CGROT=1; runr watch2; unset SIM_EV_AT SIM_CGROT
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165," ] && [ "$(cgsets)" = "478249941 1080 1920 144,478249941 1080 1920 165," ] && grep -q "relight: 165 through macOS (cgmode set 478249941 1080 1920 165 rc 0: " $W/out && grep -q "relight: done" $W/out'
mk rl_macos_clock; rlset; export SIM_EV_AT=3 SIM_LATE165=2; runr watch2; unset SIM_EV_AT SIM_LATE165
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165," ] && [ "$(cat $W/l165)" = 3 ] && grep -q "relight: done, pipe 1 at 165" $W/out && ! grep -q "no scrambled lock" $W/out'   # the 3 s verify counts from the end of cgmode's 4 s
# The settle re-set (11-04): after "relight: done" of a macOS-path relight, PHXB15_RLRESET s
# later (stat clock) one cgmode 144 -> one SCDC lock -> cgmode 165 -> scrambled lock; never the kernel, never a loop.
RS='relight: settle re-set'
mk rl_reset; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=1; MAXPOLL=150; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165,m144,m165," ] && [ "$(cgsets)" = "$CG144,$CG165,$CG144,$CG165," ] && grep -q "relight: done, pipe 1 at 165" $W/out && grep -q "$RS in 1 s (the relight used the macOS path)" $W/out && grep -q "$RS: 144 through macOS (cgmode set $CG144 rc 0: .*), waiting for one SCDC lock" $W/out && grep -q "$RS: SCDC locked at 144 (rc=0 .*locked=1.*): 165 through macOS (cgmode set $CG165 rc 0: .*), checking the scrambled lock" $W/out && [ "$(grep -c "$RS done, pipe 1 at 165 with the SCDC scrambled and locked" $W/out)" = 1 ] && [ "$(grep -c "$RS" $W/out)" = 4 ] && ! grep -q "through the kernel" $W/out && [ "$(calls)" = "$RLF$RLF" ] && [ "$(cat $R/0x506A)" = 0x0000081F ] && [ "$(sed -n "/sw=3/{s/.*hpn=\([0-9]*\).*/\1/p;q;}" $W/scdc.log)" -ge 27 ]'   # done at stat read 19, the re-set's 144 after 1 s (4 reads at the idle cadence), its first lock read 1 s later: 28
mk rl_reset_off; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=0; MAXPOLL=150; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165," ] && grep -q "relight: done" $W/out && ! grep -q "settle" $W/out'
mk rl_reset_kernel; rlset; echo 0 > "$W/uid"; export SIM_EV_AT=3 PHXB15_RLRESET=1; MAXPOLL=150; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET
check '[ $RC = 0 ] && [ "$(via)" = "k144,k165," ] && grep -q "relight: done" $W/out && ! grep -q "settle" $W/out'
mk rl_reset_user60; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=3 SIM_USER60_AT=24; MAXPOLL=200; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET SIM_USER60_AT
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165," ] && grep -q "$RS in 3 s" $W/out && grep -q "$RS skipped: pipe 1 at 60 Hz (OTG1 0x00000897/0x00000464), not 165: the mode is left as it is" $W/out && [ "$(grep -c "$RS" $W/out)" = 2 ] && [ "$(cat $W/p1mode)" = 60 ]'
mk rl_reset_event; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=3 SIM_EV2_AT=24; MAXPOLL=200; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET SIM_EV2_AT
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165," ] && grep -q "relight: Acer hot-plug event (online 2 -> 3, head-1 plugs 2 -> 3): SCDC check in 1000 ms" $W/out && grep -q "relight: pipe 1 at 165, SCDC scrambled and locked (.*): nothing to do" $W/out && [ "$(grep -c "$RS" $W/out)" = 1 ]'   # the event restarts the relight: the check finds the Acer lit, no re-set
mk rl_reset_nolock; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=1 SIM_RSNOLOCK=1; MAXPOLL=250; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET SIM_RSNOLOCK
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165,m144,m165," ] && grep -q "$RS: no SCDC lock in 10 s at 144 (rc=0 .*locked=0.*), 165 anyway: 165 through macOS (cgmode set $CG165 rc 0" $W/out && grep -q "$RS done" $W/out && [ "$(grep -c "mode=144 sw=3" $W/scdc.log)" -ge 10 ] && [ "$(grep -c "mode=144 sw=3" $W/scdc.log)" -le 12 ]'
mk rl_reset_fail165; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=1 SIM_RSFAIL165=1; MAXPOLL=250; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET SIM_RSFAIL165
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165,m144,m165," ] && grep -q "$RS: no scrambled lock at 165 in 3 s (rc=0 .*locked=0.*): left as it is, no retry" $W/out && [ "$(grep -c "$RS in" $W/out)" = 1 ] && ! grep -q "$RS done" $W/out && [ "$(grep -c "mode=165 sw=4" $W/scdc.log)" -ge 6 ] && [ "$(grep -c "mode=165 sw=4" $W/scdc.log)" -le 8 ]'
mk rl_reset_cgfail; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=1 SIM_CGFAIL=144; MAXPOLL=150; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET SIM_CGFAIL
check '[ $RC = 0 ] && [ "$(via)" = "k144,m165," ] && grep -q "$RS in 1 s" $W/out && grep -q "$RS not done: cgmode set $CG144 rc 1: set: CGError 1001 .* (pipe 1 at 165)" $W/out && [ "$(cgsets)" = "$CG144,$CG165,$CG144," ]'   # the re-set never falls back to the kernel
mk rl_reset_cgfail165; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=1 SIM_CGFAIL=165; MAXPOLL=150; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET SIM_CGFAIL
check '[ $RC = 0 ] && [ "$(via)" = "m144,k165,m144," ] && grep -q "$RS: SCDC locked at 144 (.*), the switch to 165 not done: cgmode set $CG165 rc 1: .* (pipe 1 at 144)" $W/out && ! grep -q "$RS done" $W/out'
mk rl_reset_user_rs144; rlset; export SIM_EV_AT=3 PHXB15_RLRESET=1 SIM_USER60_AT=26; MAXPOLL=150; runr watch2; unset MAXPOLL SIM_EV_AT PHXB15_RLRESET SIM_USER60_AT
check '[ $RC = 0 ] && [ "$(via)" = "m144,m165,m144," ] && grep -q "$RS: pipe 1 at 60 Hz meanwhile (not the re-set.s 144): stop, the mode is left as it is" $W/out && [ "$(cat $W/p1mode)" = 60 ]'
mk rl_macos_user60; rlset; export SIM_EV_AT=3 SIM_USER60_AT=12; runr watch2; unset SIM_EV_AT SIM_USER60_AT
check '[ $RC = 0 ] && [ "$(via)" = "m144," ] && grep -q "relight: 144 through macOS" $W/out && grep -q "relight: pipe 1 at 60 Hz meanwhile (not the relight.s 144): stop" $W/out && [ "$(cat $W/p1mode)" = 60 ]'
# Reads (11-03): the watchdogs read through one `phxctl.bin serve`; without it (an
# older phxctl.bin, PHXB15_SERVE=0, the server gone mid-watch) one process per read, the same decisions.
mk srv_used; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; swstub 0 3; run; unset PHXB15_SWSTAT
check '[ $RC = 2 ] && grep -q "optc1 underflow" $W/out && grep -q "watchdog reads through one .*phxctl.bin serve (pid [0-9]*)" $W/out && grep -q "uclk pin 456" $W/calls'
mk srv_noserve; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; swstub 0 3; export SIM_NOSERVE=1; run; unset PHXB15_SWSTAT SIM_NOSERVE
check '[ $RC = 2 ] && grep -q "optc1 underflow" $W/out && grep -q "has no serve (an older phxctl.bin?): one process per read" $W/out && ! grep -q "server is gone" $W/out && grep -q "uclk pin 456" $W/calls'
mk srv_env_off; two; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm"; swstub 0 3; export PHXB15_SERVE=0; run; unset PHXB15_SWSTAT PHXB15_SERVE
check '[ $RC = 2 ] && grep -q "optc1 underflow" $W/out && grep -q "one process per read (PHXB15_SERVE=0)" $W/out'
# the server exits at its 3rd request (the probe, poll 1, poll 2): logged once, the watch goes on one process per read and
# still sees the HUBP0 sticky the pipe-0 state stub sets from its 12th call on
lateuf() { printf '#!/bin/sh
cf=%s/sw0n; c=0; [ -r $cf ] && read -r c < $cf; c=$((c + 1)); echo $c > %s/sw0n
[ $c -ge 12 ] && echo 0x10000000 > %s/0x3AB3
echo "busy=0 gen=2 enable=1 mode=60 last=done"
' "$W" "$W" "$R" > "$W/sw0stat"; chmod +x "$W/sw0stat"; export PHXB15_SW0STAT=$W/sw0stat; }
mk srv_dies; two; echo 0x00001000 > "$R/0x39BC"; lateuf; export SIM_SERVE_DIE_AT=3; [ $SKIP = 1 ] || ( export PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=20; sh "$B15" watch2 ) > "$W/out" 2>&1; RC=$?; unset PHXB15_SW0STAT SIM_SERVE_DIE_AT
check '[ $RC = 2 ] && [ "$(grep -c "the read server is gone: one process per read from now on" $W/out)" = 1 ] && grep -q "hubp0 underflow" $W/out && [ "$(cat $W/sw0n)" -ge 12 ]'
mk srv_watch1; alone144; touch "$G/phxboot-m7-aocmodes"; p0pf1stub; detstub; sc0stub; lateuf; MAXPOLL=20; runa; unset MAXPOLL; unset PHXB15_SW0STAT
check '[ $RC = 2 ] && grep -q "watchdog reads through one" $W/out && grep -q "one display (the AOC) at 144" $W/out && grep -q "hubp0 underflow: stutter forced off" $W/out && [ "$(cat $W/sw0n)" -ge 12 ]'
# Spawns per poll, box-like (AOC 144 + Acer 165, the follower and the relight on, no stub for the engine states): every
# external command through a PATH shim that logs it (the canned sysctl too), per poll = (spawns of 60 polls - spawns of 20
# polls) / 40. With the server: 0 phxctl.bin and <= 2 processes per poll (the sleep, every RLHPN-th hot-plug stat).
mk spawn_count; two; at144; acer165; echo 0xE0000AE8 > "$R/0x3AEA"; touch "$G/phxboot-b15-2pipe" "$G/phxboot-b15-dpm" "$G/phxboot-m7-aocmodes"; mkdir -p "$W/bin"
for c in awk sed tr head cut date sleep cat mktemp mkfifo rm wc grep env stat; do
  printf '#!/bin/sh\n[ -n "${B15SPAWN:-}" ] && echo %s >> "$B15SPAWN"\nexec %s "$@"\n' "$c" "$(command -v $c)" > "$W/bin/$c"; done
cat > "$W/bin/sysctl" <<'EOS'
#!/bin/sh
[ -n "${B15SPAWN:-}" ] && echo sysctl >> "$B15SPAWN"
case "$1 $2" in
  "-n debug.phxpipe2_switch_stat") echo "busy=0 gen=3 mode=165 head1=1 last=done stage=7 why=done" ;;
  "-n debug.phxpipe0_stat") echo "busy=0 gen=4 enable=1 mode=144 last=done" ;;
  "-n debug.phxhotplug_stat") echo "hotplug v1 level=2 now_ms=1000 online=1 pend=none why=-"; echo "hpd0 conn=HDMI head1 state=high plugs=1" ;;
  -w*) ;;
  *) exit 1 ;;
esac
EOS
chmod +x "$W/bin"/*
spn() { [ $SKIP = 1 ] && return 0; : > "$W/spawns.$1"
  ( export PATH=$W/bin:$PATH PHXBOOT_G=$G PHXB15_POLL=0 PHXB15_MAXPOLL=$1 PHXB15_SYSCTL=$W/bin/sysctl B15SPAWN=$W/spawns.$1; sh "$B15" watch2 ) > "$W/out.$1" 2>&1; }
per() { awk -v a="$(grep -c "$1" "$W/spawns.20")" -v b="$(grep -c "$1" "$W/spawns.60")" 'BEGIN {printf "%.2f", (b - a) / 40}'; }
spn 20; spn 60; cp "$W/out.60" "$W/out" 2>/dev/null
[ $SKIP = 1 ] || say "INFO  spawn_count: per poll $(per .) processes: phxctl.bin $(per '^phxctl') sysctl $(per '^sysctl$') awk $(per '^awk$') sleep $(per '^sleep$')"
check 'grep -q "stopped after 60" $W/out.60 && grep -q "stopped after 20" $W/out.20 && ! grep -q relight $W/out.60 && [ "$(per ^phxctl)" = 0.00 ] && awk -v p="$(per .)" "BEGIN {exit !(p <= 2)}"'
echo "RESULT: $PASSN passed, $FAILN failed"; [ $FAILN = 0 ]
