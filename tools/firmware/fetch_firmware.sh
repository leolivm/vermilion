#!/bin/sh
# SPDX-License-Identifier: MIT
# fetch_firmware.sh [dest]: download the AMD Navi 31 microcode listed in fw.lock from linux-firmware (amdgpu/) and
# check each file's md5 against fw.lock. Default dest: $HOME/gpu/fw/navi31, the directory the box scripts read.
# The files are AMD's, redistributable in binary form under LICENSE.amdgpu (LICENSES/LICENSE.amdgpu.txt); this
# repository does not ship them. LINUX_FIRMWARE_REF picks the linux-firmware git ref (default main); a file whose md5
# differs from fw.lock is rejected, since the bring-up offsets and sizes were measured on those revisions.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-$HOME/gpu/fw/navi31}
REF=${LINUX_FIRMWARE_REF:-main}
BASES=${LINUX_FIRMWARE_URL:-"https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$REF/amdgpu https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/amdgpu?h=$REF"}
mkdir -p "$DEST"
grep -v '^#' "$HERE/fw.lock" | while read -r f sum size; do
  [ -n "$f" ] || continue
  if [ -f "$DEST/$f" ] && [ "$(md5 -q "$DEST/$f")" = "$sum" ]; then echo "ok      $f"; continue; fi
  got=0
  for b in $BASES; do
    case $b in *\?h=*) url="${b%%\?h=*}/$f?h=${b#*\?h=}";; *) url="$b/$f";; esac
    if curl -fsSL --retry 3 -o "$DEST/$f.tmp" "$url" && [ "$(md5 -q "$DEST/$f.tmp")" = "$sum" ]; then
      mv "$DEST/$f.tmp" "$DEST/$f"; echo "fetched $f"; got=1; break
    fi
    rm -f "$DEST/$f.tmp"
  done
  [ $got = 1 ] || { echo "FAILED  $f: no copy with md5 $sum at ref $REF (try LINUX_FIRMWARE_REF=<older commit>)" >&2; exit 1; }
done
echo "firmware in $DEST matches fw.lock"
