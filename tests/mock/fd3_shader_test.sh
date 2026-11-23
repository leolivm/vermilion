#!/bin/bash
# SPDX-License-Identifier: MIT
# fd3_shader_test.sh: host test of submit_test fd3 / fd3ok (tools/phxctl/fd3_shader.h, H-FAULTDEF FD-3). NO hardware.
# 1) fd3_shader_test.c: LLVM golden encodings, an independent disassembly of the shader, a PM4 walk of the IB.
# 2) submit_test.c builds warning-free and wires the modes: fd3 gated like fd1/fd2, fd3ok not, the shader at IB +0x800,
#    the default probe VA 0x100008140. 3) Mutations of the header, each must be CAUGHT.
# Exit 0 iff all pass.   bash tests/mock/fd3_shader_test.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); TOOLS=${FD3_TOOLS:-$HERE/../../tools/phxctl}
T=$(mktemp -d /private/tmp/fd3.XXXXXX); PASSN=0; FAILN=0
ok() { if eval "$2"; then PASSN=$((PASSN + 1)); echo "PASS  $1"; else FAILN=$((FAILN + 1)); echo "FAIL  $1 ($2)"; fi; }
build() { xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I "$1" -o "$2" "$HERE/fd3_shader_test.c" 2> "$T/cc.log"; }

# 1. encodings, shader, IB
if build "$TOOLS" "$T/t"; then
  "$T/t" > "$T/out" 2>&1; rc=$?
  sed 's/^/  /' "$T/out"
  ok unit_all_pass '[ $rc = 0 ] && grep -q "RESULT: [0-9]* passed, 0 failed" "$T/out"'
else
  ok unit_build false; cat "$T/cc.log"
fi

# 2. submit_test wiring
ST=$TOOLS/submit_test.c
ok submit_test_builds 'xcrun clang -arch x86_64 -O2 -Wall -Wextra -Werror -o "$T/st" "$ST" -framework IOKit -framework CoreFoundation 2> "$T/st.log" || { cat "$T/st.log"; false; }'
ok fd3_gated 'grep -q "if (mode != 4 && (on != 1 || cntl != 0x1FFEu) && !force && !dry)" "$ST"'
ok fd3_default_va 'grep -q "if (mode == 3 && !va_set) hole = 0x100008140ull;" "$ST"'
ok shader_in_ib_page 'grep -q "SHVA = IBVA + 0x800" "$ST" && grep -q "uint32_t \*sh = p + 0x200" "$ST"'
ok fd3_needs_status 'grep -q "st_ok = regs_ok && cid == 8 && rw == 1 && vmid >= 8 && fva == (hole & ~0xFFFull)" "$ST"'
ok timeout_capped 'grep -q "if (timeout_ms > 2000) timeout_ms = 2000;" "$ST"'

# 3. mutations of the header: each must make the unit test fail
mut() {   # mut <name> <perl substitution>
  mkdir -p "$T/m/$1"; cp "$TOOLS/fd3_shader.h" "$T/m/$1/"
  perl -0pi -e "$2" "$T/m/$1/fd3_shader.h"
  if cmp -s "$TOOLS/fd3_shader.h" "$T/m/$1/fd3_shader.h"; then ok "mutation_$1_applied" false; return; fi
  if ! build "$T/m/$1" "$T/m/$1/t"; then ok "mutation_$1_builds" false; cat "$T/cc.log"; return; fi   # a build error is not a catch
  if "$T/m/$1/t" > "$T/m/$1/out" 2>&1; then ok "mutation_$1_caught" false; else ok "mutation_$1_caught" true; fi
}
mut store_opcode 's/G11_GLOBAL_STORE_B32 26u/G11_GLOBAL_STORE_B32 28u/'
mut flat_seg 's/\(2u << 16\)/(0u << 16)/'
mut vop1_shift 's/\(vdst << 17\)/(vdst << 16)/'
mut no_waitcnt 's/   s\[n\+\+\] = g11_sopk\(G11_S_WAITCNT_VSCNT, G11_NULL, 0\);\n   s\[n\+\+\] = g11_vop1\(G11_V_MOV_B32, 2/   s[n++] = g11_vop1(G11_V_MOV_B32, 2/'
mut swapped_user_data 's/\{\(uint32_t\)store_va, \(uint32_t\)\(store_va >> 32\), \(uint32_t\)mark_va, \(uint32_t\)\(mark_va >> 32\)\}/{(uint32_t)mark_va, (uint32_t)(mark_va >> 32), (uint32_t)store_va, (uint32_t)(store_va >> 32)}/'
mut pgm_shift 's/\(uint32_t\)\(shader_va >> 8\)/(uint32_t)(shader_va >> 12)/'
mut no_compute_bit 's/pkt3\(0x15, 4\) \| \(1u << 1\)/pkt3(0x15, 4)/'
mut wave32 's/#define FD3_INITIATOR \(1u \| \(1u << 2\)\)/#define FD3_INITIATOR (1u | (1u << 2) | (1u << 15))/'
mut pkt_count 's/return 0xC0000000u \| \(\(count - 1u\) << 16\)/return 0xC0000000u | (count << 16)/'
mut user_sgpr_3 's/#define FD3_RSRC2 \(4u << 1\)/#define FD3_RSRC2 (3u << 1)/'
mut endpgm_op 's/G11_S_ENDPGM 48u/G11_S_ENDPGM 49u/'
mut gcr_no_gl2 's/#define FD3_GCR 0xC3B1u/#define FD3_GCR 0x03B1u/'

rm -rf "$T"; echo "RESULT: $PASSN passed, $FAILN failed"; [ $FAILN = 0 ]
