#!/bin/sh
# SPDX-License-Identifier: MIT
# sysvol_install.sh <src> <dest dir under /System> | --remove <path under /System>
# Copies <src> into the system volume (or removes a path there) and blesses a new boot snapshot, the way OCLP root
# patches do: mount the live APFS system volume read-write at /System/Volumes/Update/mnt1, change it, then
# bless --create-snapshot. Needs csrutil authenticated-root disabled. The change is live after the next reboot.
# Example: sudo sh sysvol_install.sh ~/gpu/b8/PhxVideoDecoder.bundle /System/Library/Video/Plug-Ins
set -eu
MNT=/System/Volumes/Update/mnt1
DEV=$(diskutil info / | awk -F': *' '/Device Node/ {print $2}' | sed 's/s[0-9]*$//')
[ -n "$DEV" ] || { echo "no system volume device"; exit 1; }
csrutil authenticated-root status | grep -q disabled || { echo "authenticated root is enabled"; exit 1; }
mkdir -p "$MNT"
mount | grep -q " on $MNT " || mount -o nobrowse -t apfs "$DEV" "$MNT"
if [ "$1" = "--remove" ]; then
    T="$MNT$2"
    [ -e "$T" ] && rm -rf "${T:?}"
    echo "removed $2"
else
    SRC=$1; DST="$MNT$2"
    [ -d "$DST" ] || { echo "no $2 on the system volume"; exit 1; }
    B="$DST/$(basename "$SRC")"
    [ -e "$B" ] && rm -rf "${B:?}"
    cp -R "$SRC" "$DST/"
    chown -R root:wheel "$B"
    chmod -R go-w "$B"
    echo "installed $2/$(basename "$SRC")"
fi
bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot
echo "bless rc $?"
umount "$MNT" || diskutil unmount "$MNT" || true
echo "done: reboot to boot the new snapshot"
