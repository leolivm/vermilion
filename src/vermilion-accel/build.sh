#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Builds VermilionAccel.kext (formerly PhxAccel.kext: the NullMoth accelerator in its AMD/Tahoe configuration) natively on macOS.
# Usage: ./build.sh <path-to-MacKernelSDK>
set -eu
MKSDK=${1:?usage: build.sh <MacKernelSDK>}
HERE=$(cd "$(dirname "$0")" && pwd)
# Keep build paths out of the binary (no home directory or checkout path in __FILE__ strings or debug info).
ROOT=$(cd "$HERE/../.." && pwd)
PFX="-ffile-prefix-map=$ROOT=/vermilion -fdebug-prefix-map=$ROOT=/vermilion -ffile-prefix-map=$HOME=/build -fdebug-prefix-map=$HOME=/build"
OUT="$HERE/build/VermilionAccel.kext"
rm -rf "$HERE/build"
mkdir -p "$OUT/Contents/MacOS"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -fapple-kext -mkernel -nostdinc -I"$MKSDK/Headers" -I"$HERE" \
  -I"$HERE/accel" -I"$HERE/accel/iofam" -I"$HERE/accel/re" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DNMX_ACCEL -DNM_TAHOE -DPHX_FB \
  -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 $PFX \
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -Wno-unused-variable \
  -c "$HERE/accel/nvrm-accel.cpp" -o "$HERE/build/accel.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -fapple-kext -mkernel -nostdinc -I"$MKSDK/Headers" -I"$HERE" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 $PFX \
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -c "$HERE/accel/phxfb.cpp" -o "$HERE/build/phxfb.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -fapple-kext -mkernel -nostdinc -I"$MKSDK/Headers" -I"$HERE" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 $PFX \
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -c "$HERE/accel/phxagdc.cpp" -o "$HERE/build/phxagdc.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -fapple-kext -mkernel -nostdinc -I"$MKSDK/Headers" -I"$HERE" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 $PFX \
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -c "$HERE/accel/phxsurfmap_k.cpp" -o "$HERE/build/phxsurfmap_k.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -fapple-kext -mkernel -nostdinc -I"$MKSDK/Headers" -I"$HERE" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 $PFX \
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -c "$HERE/accel/phxrotate_k.cpp" -o "$HERE/build/phxrotate_k.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -nostdlib -Xlinker -kext -L"$MKSDK/Library/x86_64" \
  -lkmodc++ -lkmod -lcc_kext "$HERE/build/accel.o" "$HERE/build/phxfb.o" "$HERE/build/phxagdc.o" "$HERE/build/phxsurfmap_k.o" "$HERE/build/phxrotate_k.o" -o "$OUT/Contents/MacOS/VermilionAccel"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
codesign --force --sign - "$OUT"
echo "built $OUT"
