#!/bin/bash
# SPDX-License-Identifier: MIT
# recover_test.sh: host test of debug.phxrecover (kext b127, src/vermilion-boot/PhxRecover.h + phx_recover.cpp). NO
# hardware. 1) recover_test.cpp runs the header: STATUS decode, the kill value per VMID, the stall clock, the stalled
# queue head ownership, the decision (refused with phxfaultdef off, only the VMID STATUS names), the 1 ms x 10 kill
# loop, the CNTL clear pulse, never MMHUB. 2) Source checks on the kext glue: boot defaults 0, the poll in every wait
# loop that runs without gpuLock and in none that holds it, the try-lock, gpuLock only for the clear, the lost context
# refused at submit and reported guilty, no reset path. 3) Mutations of the header, each must be CAUGHT.
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

# 2. the kext glue (text checks: the sources are one translation unit built only for the kernel)
RC=$SRC/phx_recover.cpp; SY=$SRC/phx_sysctl.cpp; CO=$SRC/phx_core.cpp; CL=$SRC/phx_client.cpp; VM=$SRC/phx_vm.cpp
WT=$SRC/phx_wait.cpp; QU=$SRC/phx_queue.cpp; CS=$SRC/phx_classes.cpp; BR=$SRC/PhxBringup.cpp
body() { awk "/^$1/,/^}/" "$2"; }   # body <regex of the definition line> <file>
ok build_127 'grep -q "^#define PHX_KEXT_BUILD 127u" "$BR" && grep -q "^- 127: " "$SRC/CHANGELOG.md"'
ok included_once '[ "$(grep -c "^#include \"phx_recover.cpp\"" "$BR")" = 1 ] && [ "$(grep -c "^#include \"PhxRecover.h\"" "$BR")" = 1 ]'
ok boot_defaults_off 'grep -q "^static int gRecOn = 0, gRecMs = 2000, gRecBoot = 0;" "$SY" && grep -q "^static int gFdBoot = 0;" "$SY"'
ok bootargs_exact_1 'grep -q "gFdBoot = PE_parse_boot_argn(\"phxfaultdef\", &ba, sizeof(ba)) && ba == 1;" "$CO" && grep -q "gRecBoot = PE_parse_boot_argn(\"phxrecover\", &ba, sizeof(ba)) && ba == 1;" "$CO" && grep -q "if (gRecBoot) gRecOn = 1;" "$CO"'
ok faultdef_boot_only_at_claim 'body "IOReturn PhxBringup::claimAccel" "$VM" | grep -q "if (gFdBoot == 1 && !gFdOn)" && [ "$(grep -l "faultDefCtl(1)" "$SRC"/*.cpp | tr "\n" " ")" = "$VM " ] && ! body "bool PhxBringup::start" "$CO" | grep -q "faultDefCtl"'
ok sysctls_registered '[ "$(grep -c "sysctl_register_oid(&sysctl__debug_phxrecover" "$CL")" = 3 ] && [ "$(grep -o "sysctl_unregister_oid(&sysctl__debug_phxrecover[a-z_]*)" "$CO" | wc -l | tr -d " ")" = 3 ]'
ok ms_bounds 'grep -q "if (v < 200 || v > 30000)" "$SY"'
ok poll_in_waitseq 'body "IOReturn IOAccelPhxNativeClient::doWaitSeq" "$WT" | grep -q "owner->recoverPoll();"'
ok poll_in_client_waits 'awk "/bool waitGpuPastLast/,/return false; }/" "$CS" | grep -q "owner->recoverPoll();" && body "bool IOAccelPhxNativeClient::waitRetired" "$CL" | grep -q "owner->recoverPoll();"'
ok poll_in_vmidfree 'body "void PhxBringup::vmidFree" "$VM" | awk "/recoverPoll/{p=NR} /IOLockLock\(gpuLock\)/{l=NR} END{exit !(p && l && p < l)}"'
ok poll_never_under_gpulock '! body "IOReturn PhxBringup::gpuSubmit" "$QU" | grep -q recoverPoll && ! body "uint32_t PhxBringup::vmBind" "$VM" | grep -q recoverPoll && ! body "int PhxBringup::faultDefCtl" "$VM" | grep -q recoverPoll && ! body "uint32_t PhxBringup::vmidAlloc" "$VM" | grep -q recoverPoll'
ok poll_trylock 'body "void PhxBringup::recoverPoll" "$RC" | grep -q "if (!IOLockTryLock(gRecLock))"'
ok poll_gates 'body "void PhxBringup::recoverPoll" "$RC" | grep -q "phxrec::Input in{gRecOn != 0, gFdOn != 0, stalled, thr, false, st, owned}"'
ok gpulock_only_for_clear 'body "void PhxBringup::recoverPoll" "$RC" | awk "/IOLockLock\(gpuLock\)/{n++; getline; if (\$0 !~ /clearStatus/) bad=1} END{exit !(n == 1 && !bad)}"'
ok clear_only_after_ok 'body "void PhxBringup::recoverPoll" "$RC" | awk "/kKillOk\) \{/{k=NR} /clearStatus/{c=NR} END{exit !(k && c && k < c)}"'
ok lost_before_kill 'body "void PhxBringup::recoverPoll" "$RC" | awk "/lost\[h - 1\] = 1/{m=NR} /phxrec::kill\(/{k=NR} END{exit !(m && k && m < k)}"'
ok fvmid_recorded 'body "IOReturn PhxBringup::gpuSubmit" "$QU" | awk "/q.fvmid\[q.tail & 63\] = /{f=NR} /q.tail = q.tail \+ 1/{t=NR} END{exit !(f && t && f < t)}"'
ok lost_cleared_on_alloc 'body "uint32_t PhxBringup::vmidAlloc" "$VM" | grep -q "lost\[i\] = 0;"'
ok submit_refused_aborted 'body "IOReturn IOAccelPhxNativeClient::doSubmit" "$CL" | grep -A3 "owner->ctxLost(vmid)" | grep -q "return kIOReturnAborted;"'
ok query_guilty 'body "IOReturn IOAccelPhxNativeClient::doCtx" "$CL" | grep -q "out.op = PHX_AMDGPU_CTX_QUERY2_FLAGS_RESET | PHX_AMDGPU_CTX_QUERY2_FLAGS_GUILTY;"'
ok abi_flags 'grep -q "PHX_AMDGPU_CTX_QUERY2_FLAGS_RESET  (1u << 0)" "$SRC/PhxNativeABI.h" && grep -q "PHX_AMDGPU_CTX_QUERY2_FLAGS_GUILTY (1u << 2)" "$SRC/PhxNativeABI.h"'
ok ws_loud 'body "void PhxBringup::recoverPoll" "$RC" | grep -q "THE LATCHED FAULT NAMES WINDOWSERVER"'
ok no_reset_paths '! grep -v "^//" "$RC" | grep -qiE "SOFT_RESET|VMID_RESET|DEQUEUE|Mode1|BACO|smuMessage|dalMessage|0xA980|0x30b3|0x3221"'
ok no_semaphore_regs '! grep -qiE "0x1A75[1-9a-f]|0x1A76[0-2]" "$SRC/PhxRecover.h" "$RC"'
ok gc_only_writes '[ "$(grep -o "e.wr(k[A-Za-z]*" "$SRC/PhxRecover.h" | sort -u | tr "\n" " ")" = "e.wr(kRegCntl e.wr(kRegSqCmd " ] && ! grep -q "wreg(" "$RC"'

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
