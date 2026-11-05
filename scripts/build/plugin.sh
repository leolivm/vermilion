#!/bin/sh
# SPDX-License-Identifier: MIT
# plugin.sh: the Metal driver plugin NVMTLDriver (src/metal-plugin; NVMTLDevice.m includes the rest) -> $VERMILION_OUT,
# ad-hoc signed (WindowServer refuses an unsigned plugin and falls back to the firmware framebuffer).
# Deployment target 26.0 (macOS Tahoe 26.7.1); an API newer than 26 used without @available is a build error.
# PLUGIN_DEFS: extra -D flags, e.g. PLUGIN_DEFS=-DNVMTL_DIAG for the measurement build.
. "$(dirname "$0")/lib.sh"
VKH=${VULKAN_HEADERS:-$ROOT/deps/Vulkan-Headers/include}
S="$ROOT/src/metal-plugin"
(cd "$S" && xcrun clang -arch x86_64 -mmacosx-version-min=26.0 -Werror=unguarded-availability-new -bundle -fobjc-arc -O2 $PFX \
  -Wno-protocol -Wno-incomplete-implementation ${PLUGIN_DEFS:-} -I "$VKH" -o "$OUT/NVMTLDriver" NVMTLDevice.m \
  -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation)
sign "$OUT/NVMTLDriver"; homecheck "$OUT/NVMTLDriver"
echo "built $OUT/NVMTLDriver"
