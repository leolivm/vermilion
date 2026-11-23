#!/bin/bash
# SPDX-License-Identifier: MIT
# recover_test.sh: host test of debug.phxrecover (kext b127, src/vermilion-boot/PhxRecover.h + phx_recover.cpp). NO
# hardware. 1) recover_test.cpp runs the header: STATUS decode, the kill value per VMID, the stall clock, the stalled
# queue head ownership, the decision (refused with phxfaultdef off, only the VMID STATUS names), the 1 ms x 10 kill
# loop, the CNTL clear pulse, never MMHUB. 3) Mutations of the header, each must be CAUGHT. (2, the kext glue source
# checks, comes with the kext build that uses the header.)
# Exit 0 iff all pass.   bash tests/mock/recover_test.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); SRC=${REC_SRC:-$HERE/../../src/vermilion-boot}
T=$(mktemp -d /private/tmp/recover.XXXXXX); PASSN=0; FAILN=0
ok() { if eval "$2"; then PASSN=$((PASSN + 1)); echo "PASS  $1"; else FAILN=$((FAILN + 1)); echo "FAIL  $1 ($2)"; fi; }
build() { xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$1" -o "$2" "$HERE/recover_test.cpp" 2> "$T/cc.log"; }

# 1. the logic and the register sequences
if build "$SRC" "$T/t"; then
  "$T/t" > "$T/out" 2>&1; rc=$?
  sed 's/^/  /' "$T/out"
  ok unit_all_pass '[ $rc = 0 ] && grep -q "RESULT: [0-9]* passed, 0 failed" "$T/out"'
else
  ok unit_build false; cat "$T/cc.log"
fi

# 3. mutations of the header: each must make the unit test fail
mut() {   # mut <name> <perl substitution>
  mkdir -p "$T/m/$1"; cp "$SRC/PhxRecover.h" "$T/m/$1/"
  perl -0pi -e "$2" "$T/m/$1/PhxRecover.h"
  if cmp -s "$SRC/PhxRecover.h" "$T/m/$1/PhxRecover.h"; then ok "mutation_$1_applied" false; return; fi
  if ! build "$T/m/$1" "$T/m/$1/t"; then ok "mutation_$1_builds" false; cat "$T/cc.log"; return; fi   # a build error is not a catch
  if ( "$T/m/$1/t" > "$T/m/$1/out" 2>&1; exit $? ) 2> /dev/null; then ok "mutation_$1_caught" false; else ok "mutation_$1_caught" true; fi
}
mut kill_no_broadcast 's/kKillBase = 0x00000093u/kKillBase = 0x00000083u/'
mut kill_no_checkvmid 's/kKillBase = 0x00000093u/kKillBase = 0x00000013u/'
mut kill_once 's/kKillTries = 10,/kKillTries = 1,/'
mut kill_no_stop 's/if \(e\.moved\(\)\)\n            return kKillOk;/if (e.moved() \&\& false)\n            return kKillOk;/'
mut kill_no_delay 's/        e\.delayUs\(kKillStepUs\);\n//'
mut kill_any_vmid 's/return v >= kFirstUserVmid && v <= kLastUserVmid;/return v <= kLastUserVmid;/'
mut decide_ignores_faultdef 's/    if \(!in\.faultDefOn\)\n        return kFaultDefOff;\n//'
mut decide_ignores_owner 's/    if \(!\(in\.ownedMask & \(1u << f\.vmid\)\)\)\n        return kNotOwned;\n//'
mut decide_kills_ws 's/\*vmidOut = f\.vmid;/*vmidOut = kFirstUserVmid;/'
mut decide_ignores_acted 's/    if \(in\.acted\)\n        return kActed;\n//'
mut decide_threshold_off 's/in\.stalledNs < in\.thresholdNs/in.stalledNs < in.thresholdNs \/ 2/'
mut latched_walker 's/return f\.mapping \|\| f\.perm;/return f.mapping || f.perm || f.walker;/'
mut vmid_field 's/\(st >> 20\) & 0xFu\}/(st >> 21) \& 0xFu}/'
mut track_no_idle_reset 's/if \(retired >= emitted\) \{/if (retired >= emitted \&\& false) {/'
mut track_no_restart 's/if \(!s\.armed \|\| s\.retired != retired\)/if (!s.armed)/'
mut heads_vcn 's/if \(!q\[qi\]\.ready \|\| q\[qi\]\.vcn\)/if (!q[qi].ready)/'
mut heads_any_entry 's/if \(q\[qi\]\.fseq\[i & 63\] > q\[qi\]\.done\) \{/if (true) {/'
mut clear_no_pulse_back 's/    e\.wr\(kRegCntl, c\);\n//'
mut clear_no_dummy 's/    \(void\)e\.rd\(kRegStatus\);\n//'
mut clear_drops_bits 's/e\.wr\(kRegCntl, c \| kCntlClear\);/e.wr(kRegCntl, kCntlClear);/'
mut sqcmd_on_mmhub 's/kRegSqCmd = 0x237B;/kRegSqCmd = 0x1A751;/'

rm -rf "$T"; echo "RESULT: $PASSN passed, $FAILN failed"; [ $FAILN = 0 ]
