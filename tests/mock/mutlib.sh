# SPDX-License-Identifier: MIT
# mutlib.sh: sourced by the shell mutation runners (relight_mut.sh, watchsrv_mut.sh, bootflags_mut.sh, wsprefs_mut.sh,
# bootrate_mut.sh). 11-04: several mutations checked at once, each one's mock run as small as it can be.
#   mut_make <src> <dst> <old> <new>  dst = src with <old> replaced by <new>; status 1 if <old> is not there exactly once
#                                     (the check would be vacuous: the runner reports it BAD)
#   mut_bg <cmd> [args]               one mutation check in the background (<cmd> prints its one CAUGHT / MISSED / BAD
#                                     line), at most PAR (default 2) at once; the lines come out in mutation order
#   mut_end                           waits for the rest; CAUGHT / MISSED / BAD = the counts of those lines
# PAR=1 = one mutation after the other (the old behaviour). The runners give the mock they call PAR=1 themselves, so
# PAR jobs is the most that ever runs.
MUTD=$(mktemp -d /private/tmp/mutlib.XXXXXX); MUTI=0; MUTO=1; MUTP=""
mut_make() {
  python3 -c 'import sys; s=open(sys.argv[1]).read(); o=sys.argv[3]; n=sys.argv[4]
if s.count(o) != 1: sys.exit(1)
open(sys.argv[2], "w").write(s.replace(o, n))' "$@"; }
mut_out1() { set -- $MUTP; wait "$1"; shift; MUTP="$*"; cat "$MUTD/$MUTO"; MUTO=$((MUTO + 1)); }
mut_bg() { MUTI=$((MUTI + 1)); "$@" > "$MUTD/$MUTI" 2>&1 & MUTP="$MUTP $!"
  set -- $MUTP; [ $# -ge "${PAR:-2}" ] && mut_out1; return 0; }
mut_end() { while [ -n "${MUTP// /}" ]; do mut_out1; done
  CAUGHT=$(cat "$MUTD"/* 2>/dev/null | grep -c '^CAUGHT'); MISSED=$(cat "$MUTD"/* 2>/dev/null | grep -c '^MISSED')
  BAD=$(cat "$MUTD"/* 2>/dev/null | grep -c '^BAD'); rm -rf "$MUTD"; }
