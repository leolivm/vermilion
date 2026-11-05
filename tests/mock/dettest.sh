#!/bin/bash
# SPDX-License-Identifier: MIT
# dettest.sh — MOCK test for tools/phxctl/det.sh (M7 S5a `-p <pipe>`). NO hardware: a fake phxctl.bin keeps
# registers in files; a DET write updates SIZE_CURRENT (12:8) at once, like dcn32 hubbub. Scenarios: pipe-0 default
# (b15_boot.sh call `det.sh set 9` unchanged), DET1 3 -> 9 with the GOP split (fits 20), over-budget refusal, 0 refused on a
# live pipe and allowed on a blanked pipe 2, --lock uses OTG1's lock 0x50CA, rollback per pipe, stuck CURRENT -> rollback.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); DETSH=${DETSH:-$HERE/../../tools/phxctl/det.sh}
PASSN=0; FAILN=0
mk() {
  W=$(mktemp -d /private/tmp/detmock.XXXXXX); R=$W/regs; mkdir -p "$R" "$W/gpu"
  for a in 0x397B 0x397C 0x397D 0x397E; do echo 0x00000303 > "$R/$a"; done
  echo 0x00000202 > "$R/0x397A"; echo 0x00001002 > "$R/0x39BC"
  echo 0x00000000 > "$R/0x3AB3"; echo 0x00000000 > "$R/0x3B8F"; echo 0x00000001 > "$R/0x3C6B"; echo 0x00000001 > "$R/0x3D47"
  cat > "$W/phxctl.bin" <<EOS
#!/bin/bash
R=$R; a=\$(printf 0x%X \$((\$2)))
case "\$1" in
  regs) v=\$(cat "\$R/\$a" 2>/dev/null || echo 0x00000000); echo "\$a: \$v" ;;
  wreg) v=\$(printf 0x%08X \$((\$3)))
        [ "\$a" = 0x4F9A ] && [ \$(( \$3 & 0x1000 )) != 0 ] && v=\$(printf 0x%08X \$(( \$3 & ~0x1400 )))
        [ "\$a" = 0x3B8F ] && v=\$(printf 0x%08X \$(( \$3 & 0x0FFFFFFF )))
        case "\$a" in 0x397[B-E]) [ -n "\${SIM_STUCK:-}" ] || v=\$(printf 0x%08X \$(( (\$3 & ~0x1F00) | ((\$3 & 0x1F) << 8) ))) ;; esac
        echo "\$v" > "\$R/\$a"; echo "\$a \$v" >> "\$R/writes"; echo "wreg \$a <- \$v" ;;
esac
EOS
  chmod +x "$W/phxctl.bin"
}
run() { HOME=$W SUDO= PHXCTL=$W/phxctl.bin bash "$DETSH" "$@" > "$W/out" 2>&1; RC=$?; }
rv() { cat "$R/$1"; }
check() { if eval "$2"; then PASSN=$((PASSN+1)); echo "PASS $1"; else FAILN=$((FAILN+1)); echo "FAIL $1 (rc $RC)"; sed 's/^/   | /' "$W/out" | tail -8; fi; }
sleep() { :; }; export -f sleep
mk; run set 9;              check pipe0_default '[ $RC = 0 ] && [ "$(rv 0x397B)" = 0x00000909 ] && [ "$(rv 0x397C)" = 0x00000303 ] && [ -f $W/gpu/det0.saved ]'
mk; run -p 1 set 9;         check det1_fits_gop_split '[ $RC = 0 ] && [ "$(rv 0x397C)" = 0x00000909 ] && [ "$(rv 0x397B)" = 0x00000303 ] && ! grep -q 0x397B $R/writes'
mk; run set 9; run -p 1 set 9; check over_budget_refused '[ $RC = 3 ] && [ "$(rv 0x397C)" = 0x00000303 ] && grep -q "> 20" $W/out'
mk; run -p 1 set 0;         check zero_live_refused '[ $RC = 2 ] && [ ! -f $R/writes ]'
mk; run -p 2 set 0;         check zero_blanked_pipe2 '[ $RC = 0 ] && [ "$(rv 0x397D)" = 0x00000000 ]'
mk; run -p 2 set 0; run -p 3 set 0; run set 9; run -p 1 set 9; check linux_split_9_9 '[ $RC = 0 ] && [ "$(rv 0x397B)" = 0x00000909 ] && [ "$(rv 0x397C)" = 0x00000909 ]'
mk; run -p 1 set 9 --lock;  check lock_uses_otg1 '[ $RC = 0 ] && grep -q "^0x50CA 0x00000001" $R/writes && grep -q "^0x50CA 0x00000000" $R/writes && ! grep -q 0x504A $R/writes'
mk; run -p 1 set 9; run -p 1 rollback; check rollback_pipe1 '[ $RC = 0 ] && [ "$(rv 0x397C)" = 0x00000303 ] && [ ! -f $W/gpu/det1.saved ]'
mk; SIM_STUCK=1; export SIM_STUCK; run -p 1 set 9; unset SIM_STUCK; check stuck_rolls_back '[ $RC = 4 ] && [ "$(sz=$(rv 0x397C); echo $(( sz & 0x1F )))" = 3 ]'
mk; echo 0x00000400 > $R/0x4F9A; echo 0x2000001B > $R/0x3B8F; run -p 1 clear; check clear_pipe1 '[ $RC = 0 ] && [ "$(rv 0x4F9A)" = 0x00000000 ] && [ "$(rv 0x3B8F)" = 0x0000001B ] && ! grep -q "0x4F8A\|0x3AB3" $R/writes'
mk; run -p 4 show;          check bad_pipe '[ $RC = 2 ]'
echo "$PASSN passed, $FAILN failed"; [ $FAILN = 0 ]
