#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds PhxVideoDecoder.bundle (static VideoToolbox decoder plug-in, x86_64, ad-hoc signed) -> ../build/
# Install (box): /Library/Video/Plug-Ins (non-sandboxed clients) or /System/Library/Video/Plug-Ins (sealed volume, Safari).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd); B=$HERE/../build/PhxVideoDecoder.bundle
mkdir -p "$B/Contents/MacOS"
cp "$HERE/Info.plist" "$B/Contents/Info.plist"
plutil -lint "$B/Contents/Info.plist"
xcrun clang -arch x86_64 -mmacosx-version-min=13.0 -O2 -ffile-prefix-map=$ROOT=/vermilion -fdebug-prefix-map=$ROOT=/vermilion -ffile-prefix-map=$HOME=/build -fdebug-prefix-map=$HOME=/build -Wall -bundle -fvisibility=hidden "$HERE/phxvt_stub.c" \
  -o "$B/Contents/MacOS/PhxVideoDecoder" -framework CoreFoundation
codesign -fs - "$B"
echo "built $B"
