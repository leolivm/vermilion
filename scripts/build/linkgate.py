#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""linkgate.py <kext bundle> [exports dir]: fail (exit 1) when the kext binary imports a symbol that none of the libraries
its Info.plist OSBundleLibraries names exports on the target macOS (the lists from scripts/build/linkgate_exports.sh).
This is the check kmutil makes at load time ("Failed to bind '_x' ... could not find a kext which exports this symbol");
host tests never link against the kernel KPI, so only this catches it before the box.
Exit 2 = the gate cannot run (no exports list for a listed library, no binary)."""
import os
import plistlib
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    kext = sys.argv[1].rstrip('/')
    exp = sys.argv[2] if len(sys.argv) > 2 else os.environ.get('LINKGATE_EXPORTS') or os.path.join(ROOT, 'build', 'linkgate')
    with open(os.path.join(kext, 'Contents/Info.plist'), 'rb') as f:
        info = plistlib.load(f)
    binary = os.path.join(kext, 'Contents/MacOS', info.get('CFBundleExecutable', ''))
    if not os.path.isfile(binary):
        print(f'linkgate: no binary {binary}')
        return 2
    libs = sorted(info.get('OSBundleLibraries', {}))
    allowed, missing_lists = set(), []
    for lib in libs:
        p = os.path.join(exp, lib + '.exports')
        if not os.path.isfile(p):
            missing_lists.append(lib)
            continue
        allowed.update(l.strip() for l in open(p) if l.strip())
    if missing_lists:
        print('linkgate: no exports list for ' + ', '.join(missing_lists) + f' in {exp} (run scripts/build/linkgate_exports.sh)')
        return 2
    out = subprocess.run(['nm', '-u', binary], capture_output=True, text=True, check=True).stdout
    undef = sorted({l.strip() for l in out.splitlines() if l.strip()})
    bad = [s for s in undef if s not in allowed]
    name = os.path.basename(kext)
    if bad:
        print(f'linkgate: FAIL {name}: {len(bad)} of {len(undef)} imported symbols are exported by none of {", ".join(libs)}:')
        for s in bad:
            print('  ' + s)
        return 1
    print(f'linkgate: PASS {name}: all {len(undef)} imported symbols are exported by its OSBundleLibraries ({len(libs)} libraries)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
