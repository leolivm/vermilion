#!/bin/sh
# SPDX-License-Identifier: MIT
# metaltest.sh: the Metal-device probe the boot daemon runs (tools/metaltest/metaltest.m) -> $VERMILION_OUT/metaltest.
# x86_64, deployment target 26.0, Foundation + Metal. Install as ~/gpu/metaltest (docs/INSTALL.md, section 6).
# The binary is not signed, like the one the reference machine runs (an Intel Mac runs unsigned executables).
. "$(dirname "$0")/lib.sh"
xcrun clang -arch x86_64 -mmacosx-version-min=26.0 -fobjc-arc -O2 $PFX -o "$OUT/metaltest" "$ROOT/tools/metaltest/metaltest.m" \
  -framework Foundation -framework Metal
homecheck "$OUT/metaltest"; echo "built $OUT/metaltest"
