#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds libphxdec_hevc.dylib (x86_64, ad-hoc signed) against deps/ffmpeg-lib/prefix (libavcodec_phx, libavutil_phx)
# and the in-process test tool vtdec_test. Usage: [FFMPEG_PREFIX=<dir>] [VULKAN_HEADERS=<dir>] ./build.sh  -> build/
# FFMPEG_PREFIX = an install prefix of FFmpeg n7.1 + patches/ffmpeg built as libavcodec_phx / libavutil_phx (docs/BUILD.md).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd); OUT=$HERE/build
FF=${FFMPEG_PREFIX:-$ROOT/deps/ffmpeg-lib/prefix}; VKH=${VULKAN_HEADERS:-$ROOT/deps/Vulkan-Headers/include}
PFX="-ffile-prefix-map=$ROOT=/vermilion -fdebug-prefix-map=$ROOT=/vermilion -ffile-prefix-map=$HOME=/build -fdebug-prefix-map=$HOME=/build"
mkdir -p "$OUT"
xcrun clang -arch x86_64 -mmacosx-version-min=13.0 -O2 $PFX -Wall -dynamiclib -fvisibility=hidden \
  -install_name @loader_path/libphxdec_hevc.dylib -I"$FF/include" -I"$VKH" "$HERE/phxdec_hevc.c" "$HERE/phxdec_copy.c" -o "$OUT/libphxdec_hevc.dylib" \
  -L"$FF/lib" -lavcodec_phx -lavutil_phx -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework IOSurface -framework VideoToolbox
cp "$FF/lib/libavcodec_phx.61.dylib" "$FF/lib/libavutil_phx.59.dylib" "$OUT/"
xcrun clang -arch x86_64 -mmacosx-version-min=13.0 -O2 $PFX -fobjc-arc "$HERE/vtdec_test.m" -o "$OUT/vtdec_test" \
  -framework Foundation -framework AVFoundation -framework Metal -framework CoreMedia -framework CoreVideo -framework IOSurface -framework VideoToolbox
for f in "$OUT"/*.dylib "$OUT/vtdec_test"; do codesign -fs - "$f"; done
echo "built $OUT"
