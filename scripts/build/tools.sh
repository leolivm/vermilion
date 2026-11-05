#!/bin/sh
# SPDX-License-Identifier: MIT
# tools.sh [phxctl|mesboot|vcn_write_test|submit_test ...]: the box command-line tools (tools/phxctl) -> $VERMILION_OUT.
# Default: all four. x86_64, -O2, IOKit + CoreFoundation. Install layout: phxctl as ~/gpu/phxctl.bin and
# /usr/local/sbin/phxctl, mesboot as /usr/local/sbin/mesboot (docs/INSTALL.md).
. "$(dirname "$0")/lib.sh"
[ $# -gt 0 ] || set -- phxctl mesboot vcn_write_test submit_test
for t in "$@"; do
  s="$ROOT/tools/phxctl/$t.c"; [ -f "$s" ] || { echo "unknown tool: $t" >&2; exit 2; }
  xcrun clang -arch x86_64 -O2 $PFX -o "$OUT/$t" "$s" -framework IOKit -framework CoreFoundation
  homecheck "$OUT/$t"; echo "built $OUT/$t"
done
