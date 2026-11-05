#!/bin/zsh
# SPDX-License-Identifier: MIT
# otgrate.sh [seconds]: OTG0/OTG1 totals and frame rate measured over a window (default 30 s), read-only.
# Reads OTGn H_TOTAL (0x4FEA + 0x80 n), V_TOTAL (+5) and the frame counter (+0x22, 24 bit) through phxctl.bin regs.
# 30 s gives about 0.03 Hz resolution (one frame per window); a 1 s window is only good to +-1 Hz.
W=${1:-30}
P=~/gpu/phxctl.bin
rd() { sudo -n $P regs $@ | awk '{print $2}'; }
a=($(rd 0x500C 0x508C)); t0=$(perl -MTime::HiRes=time -e 'printf "%.4f", time')
sleep $W
b=($(rd 0x500C 0x508C)); t1=$(perl -MTime::HiRes=time -e 'printf "%.4f", time')
tot=($(rd 0x4FEA 0x4FEF 0x506A 0x506F))
for i in 1 2; do
  n=$(( (b[i] - a[i]) & 0xffffff ))
  printf 'OTG%d H_TOTAL %s V_TOTAL %s frames %d dt %.4f fps %.3f\n' $((i - 1)) ${tot[2*i-1]} ${tot[2*i]} $n $(( t1 - t0 )) $(( n / (t1 - t0) ))
done
