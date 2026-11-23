#!/bin/bash
# SPDX-License-Identifier: MIT
# faultdef_test.sh: FD-0, the host test of debug.phxfaultdef (kext b126, src/vermilion-boot/PhxFaultDef.h). NO
# hardware. 1) faultdef_test.cpp runs the header's enable / disable / page scan against a register recorder (write
# order, CNTL 0x2 -> 0x1FFE, refusals that write nothing, teardown in reverse, never MMHUB). 2) Source checks on the
# kext glue: boot default 0, claimAccel only behind the boot-arg phxfaultdef=1 (b127), not in start, sysctls
# registered and unregistered, the GC-only flush, the page freed only after the bits read back off. 3) Mutations of the header, each must be CAUGHT.
# Exit 0 iff all pass.   bash tests/mock/faultdef_test.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); SRC=${FD_SRC:-$HERE/../../src/vermilion-boot}
T=$(mktemp -d /private/tmp/faultdef.XXXXXX); PASSN=0; FAILN=0
ok() { if eval "$2"; then PASSN=$((PASSN + 1)); echo "PASS  $1"; else FAILN=$((FAILN + 1)); echo "FAIL  $1 ($2)"; fi; }
build() { xcrun clang++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I "$1" -o "$2" "$HERE/faultdef_test.cpp" 2> "$T/cc.log"; }

# 1. the sequence
if build "$SRC" "$T/t"; then
  "$T/t" > "$T/out" 2>&1; rc=$?
  sed 's/^/  /' "$T/out"
  ok unit_all_pass '[ $rc = 0 ] && grep -q "RESULT: [0-9]* passed, 0 failed" "$T/out"'
else
  ok unit_build false; cat "$T/cc.log"
fi

# 2. the kext glue (text checks: the sources are one translation unit built only for the kernel)
VM=$SRC/phx_vm.cpp; SY=$SRC/phx_sysctl.cpp; CO=$SRC/phx_core.cpp; CL=$SRC/phx_client.cpp
fd_body() { awk '/^int PhxBringup::faultDefCtl/,/^}/' "$VM"; }
ok boot_default_off 'grep -q "^static int gFdOn = 0" "$SY" && grep -q "^static unsigned int gFdFill = 0" "$SY"'
# b127: claimAccel turns it on only behind the boot-arg phxfaultdef=1 (gFdBoot), start never
ok not_in_claim_or_start '[ "$(awk "/^IOReturn PhxBringup::claimAccel/,/^}/" "$VM" | grep -c faultDef)" = 1 ] && awk "/^IOReturn PhxBringup::claimAccel/,/^}/" "$VM" | grep -B1 "faultDefCtl(1)" | grep -q "if (gFdBoot == 1 && !gFdOn)" && ! awk "/^bool PhxBringup::start/,/^}/" "$CO" | grep -q faultDef'
ok sysctls_registered '[ "$(grep -c "sysctl_register_oid(&sysctl__debug_phxfaultdef" "$CL")" = 3 ] && [ "$(grep -o "sysctl_unregister_oid(&sysctl__debug_phxfaultdef[a-z_]*)" "$CO" | wc -l | tr -d " ")" = 3 ]'
ok gc_flush_only '! fd_body | grep -q "vmFlushTlb\|mmhubFlush\|kMm" && ! awk "/^void PhxBringup::faultDefQuiesce/,/^}/" "$VM" | grep -q "vmFlushTlb\|mmhubFlush\|kMm"'
ok no_semaphore_regs '! grep -qi "0x1A75[1-9a-f]\|0x1A76[0-2]" "$SRC/PhxFaultDef.h"'
ok idle_under_gpulock 'fd_body | awk "/IOLockLock\(gpuLock\)/{l=1} l && /gpuRetired\(\) >= gSeq/{print; exit}" | grep -q gpuRetired'
ok free_after_bits_off 'fd_body | awk "/phxfd::disable/{d=NR} /drop = gFdPage/{f=NR} END{exit !(d && f && d < f)}" && fd_body | grep -q "if (!gFdOn && gFdPage)"'
ok stop_tears_down 'awk "/^void PhxBringup::stop/,/^}/" "$CO" | grep -q "faultDefCtl(0)" && awk "/^void PhxBringup::stop/,/^}/" "$CO" | grep -q "gFdPage && !gFdOn"'
ok page_mask_48bit 'fd_body | grep -q "inTaskWithPhysicalMask" && fd_body | grep -q "phxfd::kPageMask" && fd_body | grep -q "kIOMemoryMapperNone"'

# 3. mutations of the header: each must make the unit test fail
mut() {   # mut <name> <perl substitution>
  mkdir -p "$T/m/$1"; cp "$SRC/PhxFaultDef.h" "$T/m/$1/"
  perl -0pi -e "$2" "$T/m/$1/PhxFaultDef.h"
  if cmp -s "$SRC/PhxFaultDef.h" "$T/m/$1/PhxFaultDef.h"; then ok "mutation_$1_applied" false; return; fi
  if build "$T/m/$1" "$T/m/$1/t" && "$T/m/$1/t" > "$T/m/$1/out" 2>&1; then ok "mutation_$1_caught" false; else ok "mutation_$1_caught" true; fi
}
mut cntl_first 's/(    e\.wr\(kRegDefLo, lo\);\n    e\.wr\(kRegDefHi, hi\);\n)(.*?)(    e\.wr\(kRegCntl, cntl \| kEnableDefault\);\n)/$3$1$2/s'
mut crash_bits 's/kEnableDefault = 0x00001FFCu/kEnableDefault = 0xFFFFFFFCu/'
mut no_readback 's/if \(e\.rd\(kRegDefLo\) != lo \|\| \(e\.rd\(kRegDefHi\) & 0xFu\) != hi\)/if (false)/'
mut teardown_addr_first 's/(template <class E> int disable.*?\n)(    e\.wr\(kRegCntl, kCntlBoot\);\n)(    e\.wr\(kRegDefLo, kDefLoBoot\);\n    e\.wr\(kRegDefHi, kDefHiBoot\);\n)/$1$3$2/s'
mut no_page_check 's/    if \(!pa\) return kNoPage;\n//'
mut not_boot_ignored 's/if \(cntl != kCntlBoot\) return kNotBoot;/(void)0;/'
mut keep_on_stuck 's/return \(e\.rd\(kRegCntl\) & kEnableDefault\) \? kCntlMismatch : kOk;/return kOk;/'
mut flush_mmhub 's/kRegInvReq = 0x291C/kRegInvReq = 0x1A774/'

rm -rf "$T"; echo "RESULT: $PASSN passed, $FAILN failed"; [ $FAILN = 0 ]
