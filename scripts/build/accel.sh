#!/bin/sh
# SPDX-License-Identifier: MIT
# accel.sh: VermilionAccel.kext (src/vermilion-accel, formerly PhxAccel.kext: PhxAMDFB framebuffer + NMXAccel accelerator + PhxAGDC) -> $VERMILION_OUT,
# ad-hoc signed by its build.sh. When build/linkgate holds export lists (scripts/build/linkgate_exports.sh), the link gate
# runs too: every imported symbol must be exported by a library in its OSBundleLibraries, or kmutil refuses the load.
. "$(dirname "$0")/lib.sh"
: "${MKSDK:?set MKSDK to a MacKernelSDK checkout}"
S="$ROOT/src/vermilion-accel"
(cd "$S" && sh ./build.sh "$MKSDK")
if [ -d "${LINKGATE_EXPORTS:-$ROOT/build/linkgate}" ]; then python3 "$ROOT/scripts/build/linkgate.py" "$S/build/VermilionAccel.kext"
else echo "link gate skipped: no export lists (scripts/build/linkgate_exports.sh)"; fi
rm -rf "$OUT/VermilionAccel.kext"; cp -R "$S/build/VermilionAccel.kext" "$OUT/VermilionAccel.kext"
homecheck "$OUT/VermilionAccel.kext/Contents/MacOS/VermilionAccel"
echo "built $OUT/VermilionAccel.kext (/Library/Extensions + Auxiliary KC; docs/INSTALL.md)"
