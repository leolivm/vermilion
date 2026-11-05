# SPDX-License-Identifier: MIT
# Shared helpers for scripts/build/*.sh (sourced; POSIX sh). No machine-specific paths: everything comes from the
# environment or from the checkout.
#   VERMILION_OUT   output dir (default <repo>/build)
#   MKSDK           MacKernelSDK checkout (https://github.com/acidanthera/MacKernelSDK), needed by kext.sh / accel.sh
#   VULKAN_HEADERS  Vulkan-Headers include dir (plugin.sh, phxdec.sh)
#   FFMPEG_PREFIX   FFmpeg n7.1 + patches/ffmpeg install prefix (phxdec.sh)
#   MESA_DIR        Mesa checkout at f5cb8ee0 (radv.sh)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${VERMILION_OUT:-$ROOT/build}
# Path-neutral binaries: the checkout path and $HOME never end up in __FILE__ strings or debug info.
PFX="-ffile-prefix-map=$ROOT=/vermilion -fdebug-prefix-map=$ROOT=/vermilion -ffile-prefix-map=$HOME=/build -fdebug-prefix-map=$HOME=/build"
mkdir -p "$OUT"
sign() { for f in "$@"; do codesign -fs - "$f"; done; }
# homecheck <file> [path ...]: fail when the binary still names the checkout, the home directory or one of the extra
# paths. It greps the raw file (load commands included: `strings` skips the install name and LC_RPATH).
homecheck() {
  f=$1; shift
  for p in "$HOME" "$ROOT" "$@"; do
    if LC_ALL=C grep -a -q -F -e "$p" "$f"; then echo "$f: still names a build path (\$HOME, the checkout or $p)" >&2; exit 1; fi
  done
}

