#!/bin/bash
# SPDX-License-Identifier: MIT
# p0prefetch_test.sh: MOCK test for tools/phxctl/p0prefetch.sh (the two-display pipe-0 underflow fix, 10-31).
# NO hardware: a fake phxctl.bin keeps one file per register, seeded from a real macOS two-pipe register dump
# ($P0PF_DUMP, default fixtures/p0prefetch-regs.txt: the state the script will meet; SKIP without it), logs every read and write,
# acknowledges OTG0_OTG_MASTER_UPDATE_LOCK (0x504A bit 8) and counts OTG0 frames on each read of 0x500C.
# Checks: the exact write list and order (lock, VSTARTUP/VUPDATE/VREADY, VTG0 FP2, BLANK_OFFSET_1, DST_AFTER_SCALER,
# PREFETCH_SETTINGS, VBLANK_PARAMETERS_0, unlock), that nothing outside the 8 group registers + 0x504A is written, that
# nothing in 0x1A751-0x1A762 is read or written, that `gop` restores every register byte-exact, the refusals (no write),
# the lock-not-acknowledged path, the flip re-enable and the dead-script guard. Exit 0 iff all pass.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); S=${P0PF:-$HERE/../../tools/phxctl/p0prefetch.sh}
DUMP=${P0PF_DUMP:-$HERE/fixtures/p0prefetch-regs.txt}
# The register dump (one "0xADDR: 0xVALUE" line per register, read on the box with PhxBringup loaded) is not shipped:
# it is a capture of one machine. Without it this test is skipped (exit 77).
[ -f "$DUMP" ] || { echo "p0prefetch_test: SKIP (no register dump at $DUMP; set P0PF_DUMP)"; exit 77; }
PASSN=0; FAILN=0
LINUXW='0x0504A 0x00000001,0x05046 0x0000002C,0x05047 0x006A0226,0x05048 0x00000095,0x039EE 0x84610002,0x03B04 0x00001188,0x03B06 0x00000033,0x03B07 0x2F092492,0x03B09 0x00000201,0x0504A 0x00000000,'
GOPW='0x0504A 0x00000001,0x05046 0x0000000D,0x05047 0x01400226,0x05048 0x0000012C,0x039EE 0x84610000,0x03B04 0x00001136,0x03B06 0x00000000,0x03B07 0x28080000,0x03B09 0x00000301,0x0504A 0x00000000,'
ALLOWED=' 0x0504A 0x05046 0x05047 0x05048 0x039EE 0x03B04 0x03B06 0x03B07 0x03B09 '
mk() {   # mk <name>: fresh fake ~/gpu with the dump's register state
  W=$(mktemp -d /private/tmp/p0pfmock.XXXXXX); G=$W/gpu; R=$W/regs; mkdir -p "$G" "$R"
  while read -r a v; do a=${a%:}; printf '0x%08X\n' "$((v))" > "$R/$(printf 0x%05X "$((a))")"; done < "$DUMP"
  echo 1 > "$W/flip"
  cat > "$G/phxctl.bin" <<EOF
#!/bin/bash
R=$R; W=$W; a=\$(printf 0x%05X \$((\$2)))
fb() { [ \$((\$1)) -ge \$((0x1A751)) ] && [ \$((\$1)) -le \$((0x1A762)) ] && echo "FORBIDDEN \$1" >> "\$R/forbidden"; }
case "\$1" in
  regs) fb "\$a"; echo "\$a" >> "\$R/reads"; v=\$(cat "\$R/\$a" 2>/dev/null || echo 0x00000000)
        [ "\$a" = 0x0500C ] && printf '0x%08X\n' \$(( v + 1 )) > "\$R/\$a"
        printf '%s: 0x%08x\n' "\$a" \$((v)) ;;
  wreg) fb "\$a"; v=\$(printf 0x%08X \$((\$3))); echo "\$a \$v" >> "\$R/writes"; n=\$(wc -l < "\$R/writes" | tr -d ' ')
        if [ -n "\${SIM_DIE_AT:-}" ] && [ "\$n" -ge "\$SIM_DIE_AT" ]; then echo "\$v" > "\$R/\$a"; kill -9 \$(cat "\$W/pid"); exit 1; fi
        s=\$v
        if [ "\$a" = 0x0504A ]; then
          if [ \$((\$3)) -eq 1 ] && [ -z "\${SIM_NOLOCKACK:-}" ]; then s=0x00000101; fi
          if [ \$((\$3)) -eq 0 ] && [ -n "\${SIM_FLIPOFF:-}" ]; then echo 0 > "\$W/flip"; fi
        fi
        echo "\$s" > "\$R/\$a"
        echo "wreg \$a = \$v -> kr 0x00000000  readback \$s" ;;
esac
EOF
  printf '#!/bin/sh\necho "busy=${SIM_BUSY:-0} gen=3 mode=144 last=done"\n' > "$W/swstat"
  printf '#!/bin/sh\ncat %s/flip\n' "$W" > "$W/flipget"
  printf '#!/bin/sh\necho 1 > %s/flip; echo flipon >> %s/calls\n' "$W" "$W" > "$W/flipon"
  chmod +x "$G/phxctl.bin" "$W/swstat" "$W/flipget" "$W/flipon"
  cp -R "$R" "$W/regs0"; NAME=$1; }
run() { ( export PHXP0_G=$G PHXP0_SUDO="" PHXP0_SWSTAT=$W/swstat PHXP0_FLIPGET=$W/flipget PHXP0_FLIPON=$W/flipon \
          PHXP0_GUARD_S=${GUARD:-5} PHXP0_LOCKFILE=$W/lock; exec sh "$S" "$@" ) > "$W/out" 2>&1; RC=$?; }
wl() { [ -e "$R/writes" ] && tr '\n' ',' < "$R/writes"; }
nowrites() { [ ! -e "$R/writes" ]; }
onlyallowed() { [ ! -e "$R/writes" ] || ! awk '{print $1}' "$R/writes" | while read -r a; do case "$ALLOWED" in *" $a "*) ;; *) echo "$a"; esac; done | grep -q .; }
noforbidden() { [ ! -e "$R/forbidden" ]; }
# every register file except the live ones equals the start state (the byte-exact restore)
same0() { diff -r -q -x reads -x writes -x forbidden -x 0x0500C "$W/regs0" "$R" > /dev/null; }
check() { if eval "$1" && noforbidden && onlyallowed; then PASSN=$((PASSN + 1)); echo "PASS  $NAME"
          else FAILN=$((FAILN + 1)); echo "FAIL  $NAME ($1)"; cat "$W/out"; [ -e "$R/writes" ] && cat "$R/writes"; fi; rm -rf "${W:?}"; }
st() { cat "$R/$1"; }

mk show; run show
check '[ $RC = 0 ] && nowrites && grep -q "preconditions: OK" $W/out && grep -q "0x5046 .* GOP$" $W/out && grep -q "3.00 lines = 44 us" $W/out'
mk linux; run linux
check '[ $RC = 0 ] && [ "$(wl)" = "$LINUXW" ] && [ "$(st 0x05046)" = 0x0000002C ] && [ "$(st 0x039EE)" = 0x84610002 ] && [ "$(st 0x0504A)" = 0x00000000 ] && grep -q "0x5046|.*|0x0000000D" $G/p0prefetch.saved && grep -q "32.18 lines = 477 us" $W/out'
mk linux_then_gop; run linux; rm -f "$R/writes"; run gop
check '[ $RC = 0 ] && [ "$(wl)" = "$GOPW" ] && same0 && grep -q "byte-exact: OK" $W/out && [ ! -e $G/p0prefetch.saved ]'
mk linux_twice; run linux; rm -f "$R/writes"; run linux
check '[ $RC = 0 ] && nowrites && grep -q "already Linux" $W/out && grep -q "0x3B07|.*|0x28080000" $G/p0prefetch.saved'
mk gop_no_save; run gop
check '[ $RC = 2 ] && nowrites'
mk stutter_allowed; echo 0x00001000 > "$R/0x039BC"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "stutter allowed" $W/out'
mk otg0_144; echo 0x00000807 > "$R/0x04FEA"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "do not apply" $W/out'
mk acer_60; echo 0x00000897 > "$R/0x0506A"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "OTG1 is not lit at 1080p144" $W/out'
# boot rate (11-02): the Acer at 1080p165 is accepted only with PHXP0_PAIR165=1 (b15_boot.sh aocboot), with the same writes.
# export + unset, never `VAR=x run`: under sh (bash POSIX mode) an assignment before a function call outlives the call
mk acer_165_default; echo 0x0000081F > "$R/0x0506A"; echo 0x00000455 > "$R/0x0506F"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "OTG1 is not lit at 1080p144" $W/out && ! grep -q PAIR165 $W/out'
mk acer_165_pair165; echo 0x0000081F > "$R/0x0506A"; echo 0x00000455 > "$R/0x0506F"; export PHXP0_PAIR165=1; run linux; unset PHXP0_PAIR165
check '[ $RC = 0 ] && [ "$(wl)" = "$LINUXW" ] && [ "$(st 0x05046)" = 0x0000002C ]'
mk acer_165_bad_v_pair165; echo 0x0000081F > "$R/0x0506A"; echo 0x00000449 > "$R/0x0506F"; export PHXP0_PAIR165=1; run linux; unset PHXP0_PAIR165
check '[ $RC = 3 ] && nowrites && grep -q "or, PHXP0_PAIR165=1, the Acer at 1080p165" $W/out'
mk acer_60_pair165; echo 0x00000897 > "$R/0x0506A"; export PHXP0_PAIR165=1; run linux; unset PHXP0_PAIR165
check '[ $RC = 3 ] && nowrites'
# p0solo (11-03): OTG1 dark is accepted only with PHXP0_SOLO=1 (b15_boot.sh aocboot1, the AOC alone), with the same writes
mk solo_default; echo 0x00000200 > "$R/0x05081"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "OTG1 is not lit at 1080p144" $W/out'
mk solo_on; echo 0x00000200 > "$R/0x05081"; export PHXP0_SOLO=1; run linux; unset PHXP0_SOLO
check '[ $RC = 0 ] && [ -s "$R/writes" ]'
mk solo_on_otg1_lit_60; echo 0x00000897 > "$R/0x0506A"; export PHXP0_SOLO=1; run linux; unset PHXP0_SOLO
check '[ $RC = 3 ] && nowrites'
mk no_linux60; echo 0xE00050AA > "$R/0x03AEA"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "linux60" $W/out'
mk vready_bit; echo 0x000F0102 > "$R/0x03AB3"; run linux
check '[ $RC = 3 ] && nowrites && grep -q VREADY_AT_OR_AFTER_VSYNC $W/out'
mk lock_sel_otg1; echo 0x02000000 > "$R/0x05051"; run linux
check '[ $RC = 3 ] && nowrites && grep -q LOCK_SEL $W/out'
mk already_locked; echo 0x00000101 > "$R/0x0504A"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "already locked" $W/out'
mk underflow_now; echo 0x00002400 > "$R/0x04F8A"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "in progress" $W/out'
mk switch_busy; export SIM_BUSY=1; run linux; unset SIM_BUSY
check '[ $RC = 3 ] && nowrites && grep -q "switch busy" $W/out'
mk unknown_value; echo 0x00000020 > "$R/0x05046"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "neither" $W/out'
mk partial_linux_no_save; echo 0x0000002C > "$R/0x05046"; run linux
check '[ $RC = 3 ] && nowrites && grep -q "no save file" $W/out'
mk lock_no_ack; export SIM_NOLOCKACK=1; run linux; unset SIM_NOLOCKACK
check '[ $RC = 4 ] && [ "$(wl)" = "0x0504A 0x00000001,0x0504A 0x00000000," ] && [ "$(st 0x05046)" = 0x0000000D ]'
mk flip_reenable; export SIM_FLIPOFF=1; run linux; unset SIM_FLIPOFF
check '[ $RC = 0 ] && grep -q flipon $W/calls && [ "$(cat $W/flip)" = 1 ] && grep -q "set back to 1" $W/out'
mk flip_untouched; run linux
check '[ $RC = 0 ] && [ ! -e $W/calls ]'
# the script dies inside the lock (SIGKILL on its 4th write: lock, VSTARTUP, VUPDATE, VREADY): the guard must unlock OTG0
mk guard_unlocks; ( export SIM_DIE_AT=4 PHXP0_G=$G PHXP0_SUDO="" PHXP0_SWSTAT=$W/swstat PHXP0_FLIPGET=$W/flipget PHXP0_FLIPON=$W/flipon \
  PHXP0_GUARD_S=0.5 PHXP0_LOCKFILE=$W/lock; sh "$S" linux > "$W/out" 2>&1 & echo $! > "$W/pid"; wait ) 2> /dev/null; sleep 1.5; RC=x
check '[ "$(st 0x0504A)" = 0x00000000 ] && [ "$(tail -1 $R/writes)" = "0x0504A 0x00000000" ] && [ -e $W/lock.fired ] && [ ! -e $W/lock ]'
echo "RESULT: $PASSN passed, $FAILN failed"; [ $FAILN = 0 ]
