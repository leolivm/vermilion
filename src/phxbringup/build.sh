#!/bin/sh
# SPDX-License-Identifier: MIT OR GPL-2.0-only
# Builds VermilionBoot.kext (formerly PhxBringup.kext; IOKit class PhxBringup) natively on macOS with the Command Line Tools.
# Usage: ./build.sh <path-to-MacKernelSDK>
set -eu
MKSDK=${1:?usage: build.sh <MacKernelSDK>}
HERE=$(cd "$(dirname "$0")" && pwd)
# Keep build paths out of the binary (no home directory or checkout path in __FILE__ strings or debug info).
ROOT=$(cd "$HERE/../.." && pwd)
PFX="-ffile-prefix-map=$ROOT=/vermilion -fdebug-prefix-map=$ROOT=/vermilion -ffile-prefix-map=$HOME=/build -fdebug-prefix-map=$HOME=/build"
OUT="$HERE/build/VermilionBoot.kext"
rm -rf "$HERE/build"
mkdir -p "$OUT/Contents/MacOS"
[ -f "$HERE/phx_c3_image.inc" ] || { echo "missing phx_c3_image.inc: run tools/firmware/c3_image.py (needs linux-firmware dcn_3_2_0_dmcub.bin, tools/firmware/fetch_firmware.sh)" >&2; exit 1; }

CFLAGS="-arch x86_64 -mmacosx-version-min=11.0 -mkernel -fno-builtin -fno-common -nostdinc
  -I$MKSDK/Headers -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -Os -Wall $PFX"

clang -x c $CFLAGS -c "$HERE/kmod_info.c" -o "$HERE/build/kmod_info.o"
clang++ $CFLAGS -std=c++17 -fapple-kext -fno-rtti -fno-exceptions \
  -c "$HERE/PhxBringup.cpp" -o "$HERE/build/PhxBringup.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -nostdlib -Xlinker -kext \
  -L"$MKSDK/Library/x86_64" -lkmod -lcc_kext \
  "$HERE/build/kmod_info.o" "$HERE/build/PhxBringup.o" \
  -o "$OUT/Contents/MacOS/VermilionBoot"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
echo "built $OUT"
