#!/bin/sh
# SPDX-License-Identifier: MIT
# linkgate_exports.sh [KC dir] [out dir]: the symbol lists the kext link gate (scripts/build/linkgate.py) checks against,
# from the kernel collections of the target macOS 26.7.1 (/System/Library/KernelCollections of that machine, copied read-only).
#   <bundle id>.exports = one symbol per line:
#   - com.apple.kpi.* : the kernel's symbol sets (`ipsw kernel symbolsets BootKernelExtensions.kc`), i.e. what kmutil lets a
#     third-party kext bind (e.g. _thread_call_cancel_wait is only in com.apple.kpi.private);
#   - family kexts (IOPCIFamily, IOSurface, IOHIDFamily (PhxBringup), IOGraphicsFamily, IOAcceleratorFamily2, AppleGraphicsDeviceControl): the
#     defined external symbols of their KC extract (`ipsw kernel extract`, `nm -gU`).
# Needs ipsw (brew). Run again after a macOS update with the new KCs. Output goes to build/linkgate (Apple-derived symbol lists: never commit them).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KC=${1:-/System/Library/KernelCollections}
OUT=${2:-$ROOT/build/linkgate}
command -v ipsw > /dev/null || { echo "linkgate_exports: ipsw not found (brew install blacktop/tap/ipsw)" >&2; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$OUT"
ipsw kernel symbolsets "$KC/BootKernelExtensions.kc" --no-color > "$T/ss.txt"
python3 - "$T/ss.txt" "$OUT" <<'EOF'
import re, sys
cur, sets = None, {}
for line in open(sys.argv[1]):
    line = line.rstrip('\n')
    m = re.match(r'^(com\.apple\.[A-Za-z0-9_.-]+): \(', line)
    if m: cur = m.group(1); sets[cur] = []; continue
    if cur and line.startswith('_'): sets[cur].append(line.strip())
for k, v in sets.items():
    open(f"{sys.argv[2]}/{k}.exports", "w").write("\n".join(sorted(set(v))) + "\n")
    print(f"{k}: {len(set(v))}")
EOF
for spec in "BootKernelExtensions.kc com.apple.iokit.IOPCIFamily" "BootKernelExtensions.kc com.apple.iokit.IOSurface" \
            "BootKernelExtensions.kc com.apple.iokit.IOHIDFamily" \
            "SystemKernelExtensions.kc com.apple.iokit.IOGraphicsFamily" "SystemKernelExtensions.kc com.apple.iokit.IOAcceleratorFamily2" \
            "SystemKernelExtensions.kc com.apple.AppleGraphicsDeviceControl"; do
  set -- $spec
  rm -rf "$T/x"; mkdir -p "$T/x"
  ipsw kernel extract "$KC/$1" "$2" -o "$T/x" --no-color > /dev/null 2>&1 || { echo "linkgate_exports: extract $2 failed" >&2; exit 1; }
  f=$(find "$T/x" -type f | head -1)
  [ -n "$f" ] || { echo "linkgate_exports: no file for $2" >&2; exit 1; }
  nm -gU "$f" | awk '{print $NF}' | sort -u > "$OUT/$2.exports"
  echo "$2: $(wc -l < "$OUT/$2.exports" | tr -d ' ')"
done
