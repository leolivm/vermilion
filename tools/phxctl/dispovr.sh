#!/bin/sh
# SPDX-License-Identifier: MIT
# dispovr.sh [--check] (box, root; rotation rot15, 11-03): a display override per
# connected display so CoreDisplay's default mode at a display set it never saw is the HIGHEST rate, not the one under 86 Hz.
# CoreDisplay 26.7.1 IOFBRebuild breaks a same-size tie with the override's default-resolution refresh (0x7ff80535e7dc);
# __IODisplayCreateInfoDictionary opens <dir>/DisplayVendorID-<vid hex>/DisplayProductID-<pid hex> with NO .plist suffix
# (_readPlist = open() of that exact path). default-resolution = 0 x 0 (sizes untouched: different sizes still compare by
# area) @ 255 Hz (16.16 big-endian 00ff0000) = above every mode, so the highest refresh wins (phxrot::kOverrideDesire).
# Only files this tool wrote (key phx-override) are ever replaced; another override for the display is left alone.
# The override is read when a WindowServer first meets the display: run it before the boot daemon's WindowServer restart.
# Exit 0 = every connected display has a current override (or one we must not touch); --check writes nothing.
set -u
DIR=${PHX_OVR_DIR:-/Library/Displays/Contents/Resources/Overrides}
DATA=AAAAAAAAAAAA/wAA
CHECK=0; [ "${1:-}" = --check ] && CHECK=1
IOREG=${PHX_OVR_IOREG:-ioreg}
n=0; w=0; k=0
for pair in $($IOREG -lw0 -r -c AppleDisplay | awk -F' = ' '/"DisplayVendorID"/{v=$2} /"DisplayProductID"/{p=$2} /"DisplayProductID"|"DisplayVendorID"/{ if (v != "" && p != "") { print v ":" p; v = ""; p = "" } }' | sort -u); do
    v=${pair%%:*}; p=${pair##*:}
    case "$v$p" in *[!0-9]*|"") continue;; esac
    n=$((n + 1))
    f=$(printf '%s/DisplayVendorID-%x/DisplayProductID-%x' "$DIR" "$v" "$p")
    if [ -e "$f" ] && ! plutil -extract phx-override raw -o - "$f" >/dev/null 2>&1; then
        echo "dispovr: $f exists and is not ours: left alone"; k=$((k + 1)); continue; fi
    cur=$(plutil -extract default-resolution raw -o - "$f" 2>/dev/null)
    if [ "$cur" = "$DATA" ]; then echo "dispovr: $f current"; continue; fi
    if [ "$CHECK" = 1 ]; then echo "dispovr: $f missing or stale (--check: not written)"; continue; fi
    mkdir -p "$(dirname "$f")" || exit 2
    t="$f.new.$$"
    printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">\n<dict>\n\t<key>DisplayProductID</key>\n\t<integer>%s</integer>\n\t<key>DisplayVendorID</key>\n\t<integer>%s</integer>\n\t<key>default-resolution</key>\n\t<data>%s</data>\n\t<key>phx-override</key>\n\t<string>rotation rot15: highest rate as the default at a new display set</string>\n</dict>\n</plist>\n' "$p" "$v" "$DATA" > "$t" \
        && plutil -lint "$t" >/dev/null && chmod 644 "$t" && mv -f "$t" "$f" || { rm -f "$t"; echo "dispovr: writing $f FAILED"; exit 2; }
    echo "dispovr: $f written (vendor $v product $p, default-resolution 0x0 @255 Hz)"; w=$((w + 1))
done
echo "dispovr: $n display(s), $w written, $k foreign override(s) left alone"
exit 0
