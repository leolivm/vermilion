#!/bin/sh
# SPDX-License-Identifier: MIT
# wm144_guard_test.sh: the stutter guard of tools/phxctl/wm144.sh, with a fake phxctl.bin and sudo.
# WM144=<file> tests another copy. Exit 0 = all cases pass.
HERE=$(cd "$(dirname "$0")" && pwd); WM=${WM144:-$HERE/../../tools/phxctl/wm144.sh}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; mkdir -p "$T/gpu" "$T/bin"
printf '#!/bin/sh\nshift; exec "$@"\n' > "$T/bin/sudo"
cat > "$T/gpu/phxctl.bin" <<'FAKE'
#!/bin/sh
case "$1" in
  regs) if [ "$2" = 0x39E5 ]; then echo "$2 0x00001002"; else [ "$REG" = fail ] && exit 1; echo "$2 $REG"; fi ;;
  wreg) echo "$2 $3" >> "$HOME/writes"; echo "$2 $3" ;;
esac
FAKE
chmod +x "$T/bin/sudo" "$T/gpu/phxctl.bin"; pass=0; fail=0
run() { rm -f "$T/writes"; HOME=$T PATH="$T/bin:$PATH" REG=$1 sh "$WM" "$2" > "$T/out" 2>&1; rc=$?; w=0; [ -f "$T/writes" ] && w=$(wc -l < "$T/writes" | tr -d ' '); }
check() { if eval "$2"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $1 (rc=$rc writes=$w): $(tail -1 "$T/out")"; fi; }
for m in linux linux2 linux144x2 linux144a60 linux60a165 linux144a165 gop; do
  run 0x00001000 $m; check "$m refused with stutter allowed" '[ $rc = 3 ] && [ $w = 0 ] && grep -q REFUSED "$T/out"'
  run 0x00001002 $m; check "$m writes with stutter off" '[ "$w" -gt 0 ] && ! grep -q REFUSED "$T/out"'
done
run fail linux; check "unreadable register refused" '[ $rc = 3 ] && [ $w = 0 ]'
run 0x00001000 show; check "show never refuses" '[ $w = 0 ] && ! grep -q REFUSED "$T/out"'
echo "wm144 guard: $pass passed, $fail failed"; [ $fail = 0 ]
