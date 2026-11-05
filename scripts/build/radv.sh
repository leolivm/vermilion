#!/bin/sh
# SPDX-License-Identifier: MIT
# radv.sh [apply]: RADV (Mesa Vulkan driver) with the Darwin backend -> $VERMILION_OUT/libvulkan_radeon.dylib.
#   radv.sh apply   apply patches/mesa (0001..0017, git am) to MESA_DIR, a clean Mesa checkout at f5cb8ee0
#                   (https://gitlab.freedesktop.org/mesa/mesa, f5cb8ee032adabef599ae892ec4e42d796b84da9)
#   radv.sh         configure once (meson, x86_64 cross file below) and build
# Needs meson, ninja, glslangValidator and python3 with mako, pyyaml, markupsafe, packaging.
. "$(dirname "$0")/lib.sh"
: "${MESA_DIR:?set MESA_DIR to a Mesa checkout at f5cb8ee0}"
if [ "${1:-}" = apply ]; then git -C "$MESA_DIR" am "$ROOT"/patches/mesa/*.patch; exit 0; fi
B="$MESA_DIR/build-x86_64"
if [ ! -d "$B" ]; then
  X="$OUT/mesa-x86_64-cross.ini"
  printf "[binaries]\nc = ['clang', '-arch', 'x86_64']\ncpp = ['clang++', '-arch', 'x86_64']\nobjc = ['clang', '-arch', 'x86_64']\nar = 'ar'\nstrip = 'strip'\n\n[built-in options]\nc_args = ['-mmacosx-version-min=14.0', '-ffile-prefix-map=%s=/mesa']\ncpp_args = ['-mmacosx-version-min=14.0', '-ffile-prefix-map=%s=/mesa']\nc_link_args = ['-mmacosx-version-min=14.0']\ncpp_link_args = ['-mmacosx-version-min=14.0']\n\n[host_machine]\nsystem = 'darwin'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n" "$MESA_DIR" "$MESA_DIR" > "$X"
  (cd "$MESA_DIR" && meson setup build-x86_64 -Dbuildtype=release -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= \
    -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dbuild-tests=false -Dvulkan-layers= \
    -Dtools= -Dvideo-codecs=all_free,h264dec,h265dec --cross-file="$X")
fi
ninja -C "$B" src/amd/vulkan/libvulkan_radeon.dylib
cp "$B/src/amd/vulkan/libvulkan_radeon.dylib" "$OUT/libvulkan_radeon.dylib"
sign "$OUT/libvulkan_radeon.dylib"
echo "built $OUT/libvulkan_radeon.dylib"
