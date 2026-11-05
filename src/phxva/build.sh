#!/bin/sh
# SPDX-License-Identifier: MIT
# build.sh: builds build/PhxVA.bundle (x86_64, ad-hoc signed). Install: copy it to /Library/GPUBundles/PhxVA.bundle.
set -e
cd "$(dirname "$0")"
rm -rf build/PhxVA.bundle; mkdir -p build/PhxVA.bundle/Contents/MacOS
cp Info.plist build/PhxVA.bundle/Contents/
xcrun clang -arch x86_64 -bundle -O2 -ffile-prefix-map="$(cd ../.. && pwd)"=/vermilion -ffile-prefix-map="$HOME"=/build -o build/PhxVA.bundle/Contents/MacOS/PhxVA PhxVA.c
codesign -fs - build/PhxVA.bundle
echo built build/PhxVA.bundle
