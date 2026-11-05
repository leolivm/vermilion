#!/bin/bash
# SPDX-License-Identifier: MIT
# ptwalk.sh: walk a GPU VMID's 4-level page table in VRAM (read through /usr/local/sbin/phxctl) down to one VA, printing
# every level, to tell a never-mapped page from a stale TLB/HDP read or a corrupted table at a VM fault.
# Usage: sudo bash ptwalk.sh [vmid] [va]   (defaults: the VMID and VA latched in GCVM_L2_PROTECTION_FAULT_STATUS/ADDR)
set -u
P=/usr/local/sbin/phxctl
reg() { "$P" regs "$1" | awk '{print $2}'; }
st=$(reg 0x2828); lo=$(reg 0x2829); hi=$(reg 0x282a)
vmid=${1:-$(( (st >> 20) & 15 ))}
va=${2:-$(( ((hi & 0xf) << 44) | (lo << 12) ))}
printf 'fault status %s -> VMID %d, walking VA 0x%x in VMID %d\n' "$st" $(( (st >> 20) & 15 )) "$va" "$vmid"
blo=$(reg $(printf '0x%x' $((0x2953 + 2 * vmid)))); bhi=$(reg $(printf '0x%x' $((0x2954 + 2 * vmid))))
base=$(( ((bhi << 32) | blo) & 0x0000FFFFFFFFF000 ))
printf 'CONTEXT%d page-table base 0x%x (raw lo %s hi %s)\n' "$vmid" "$base" "$blo" "$bhi"
tbl=$base
for shift in 39 30 21 12; do
  idx=$(( (va >> shift) & 511 )); off=$(( tbl + idx * 8 ))
  w=$("$P" vram $(printf '0x%x' $off) 2 | sed 's/.*: *//')
  e0=0x$(echo $w | awk '{print $1}'); e1=0x$(echo $w | awk '{print $2}')
  e=$(( (e1 << 32) | e0 ))
  lvl=$(( (39 - shift) / 9 ))
  printf 'level %d (bits %d..%d) idx %3d at VRAM 0x%x: entry 0x%016x  valid=%d system=%d\n' $lvl $((shift + 8)) $shift $idx $off $e $((e & 1)) $(( (e >> 1) & 1 ))
  # neighbours, to see whether the whole table is empty or only this entry
  if [ $(( e & 1 )) = 0 ]; then
    n=$("$P" vram $(printf '0x%x' $(( tbl + ((idx & ~7) * 8) ))) 16 | sed 's/^[^:]*: *//' | tr '\n' ' ')
    echo "  NOT VALID - entries $((idx & ~7))..$(((idx & ~7) + 7)) of this table: $n"
    exit 0
  fi
  [ $shift = 12 ] && { printf '  PTE valid -> system page 0x%x (the table maps this VA)\n' $(( e & 0x0000FFFFFFFFF000 )); exit 0; }
  tbl=$(( e & 0x0000FFFFFFFFF000 ))
done
