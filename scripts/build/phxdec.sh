#!/bin/sh
# SPDX-License-Identifier: MIT
# phxdec.sh: the HEVC/AV1 VideoToolbox decoder libphxdec_hevc.dylib + vtdec_test (src/video-decoder) -> $VERMILION_OUT.
# Links libavcodec_phx / libavutil_phx from FFMPEG_PREFIX (FFmpeg n7.1 + patches/ffmpeg, LGPL-2.1-or-later).
. "$(dirname "$0")/lib.sh"
sh "$ROOT/src/video-decoder/build.sh"
cp "$ROOT/src/video-decoder/build/libphxdec_hevc.dylib" "$OUT/"
homecheck "$OUT/libphxdec_hevc.dylib"
echo "built $OUT/libphxdec_hevc.dylib"
