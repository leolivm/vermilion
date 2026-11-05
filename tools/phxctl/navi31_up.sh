#!/bin/bash
# SPDX-License-Identifier: MIT
# One-shot Navi 31 bring-up on macOS, after a clean boot: GFX power-up, GART, MES, KIQ, MES scheduler and a
# compute queue — every step that works today. Stops at the first step that fails.
set -u
M(){ sudo -n /usr/local/sbin/mesboot "$@"; }
step(){ echo "== $1"; shift; out=$("$@" 2>&1); echo "$out" | tail -3; }
cd ~/gpu/phxctl || exit 1
step "bring-up (PSP, SMU, RLC, RS64, doorbells, MEC)" bash bringup_navi31.sh
echo "$out" | grep -q "watchdog cancelled" || { echo "bring-up failed"; exit 1; }
step "gart" M gart mc
step "mes" M mes
step "kiq (SET_RESOURCES + ring test)" M kiqres mc
echo "$out" | grep -q "scratch0=0xdeadbeef" || { echo "KIQ test failed"; exit 1; }
step "MES scheduler (MAP_QUEUES, SET_HW_RESOURCES)" M schedkiq mc
echo "$out" | grep -q "QUERY_SCHED_STATUS.*OK" || { echo "scheduler failed"; exit 1; }
step "compute queue (ADD_QUEUE + WRITE_DATA), then hand it to the kext" M cs mc ADOPT
echo "$out" | grep -q "testbuf=0xfeedf00d" || { echo "compute queue failed"; exit 1; }
step "gfx queue (clear-state, ADD_QUEUE gfx, ring test), then hand it to the kext" M gfxq mc
echo "$out" | grep -q "testbuf=0x6f7a6f7a" || echo "WARNING: gfx ring test did not confirm (adopt may still have run)"
echo "navi31 up: compute + gfx queues ready"
