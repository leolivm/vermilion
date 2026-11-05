#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# smumetrics.py [n=3] [interval=2]: SMU v13.0.0 metrics (TransferTableSmu2Dram table 5 -> VRAM 0x9000000; layout =
# Linux SmuMetrics_t, smu13_driver_if_v13_0_0.h). Run on the box with sudo. Power: offset 116 = AverageSocketPower (what Linux
# amdgpu reports as power1_average, smu_v13_0_0_ppt.c), offset 118 = AverageTotalBoardPower (10-30: both printed).
import os, struct, subprocess, sys, time
P = os.path.expanduser('~/gpu/phxctl.bin')
n = int(sys.argv[1]) if len(sys.argv) > 1 else 3; dt = float(sys.argv[2]) if len(sys.argv) > 2 else 2
for i in range(n):
    subprocess.run([P, 'smu', '0x12', '5'], check=True, capture_output=True)
    out = subprocess.run([P, 'vram', '0x9000000', '98'], check=True, capture_output=True, text=True).stdout
    w = [int(x, 16) for l in out.splitlines() if ':' in l for x in l.split(':')[1].split()]
    b = b''.join(struct.pack('<I', x) for x in w)
    u16 = lambda o: struct.unpack_from('<H', b, o)[0]
    t = struct.unpack_from('<13H', b, 120)
    if os.environ.get('SMU_CLOCKS'):   # CurrClock[13]: gfx soc uclk fclk dclk0 vclk0 dclk1 vclk1 disp dpp dpref dcef dtb (MHz)
        c = struct.unpack_from('<13I', b, 0)
        print(f"{time.strftime('%H:%M:%S')} clk gfx {c[0]} soc {c[1]} uclk {c[2]} fclk {c[3]} vclk0 {c[5]} disp {c[8]} dcef {c[11]} act {u16(104)}% socket {u16(116)} board {u16(118)} W", flush=True)
        if i + 1 < n: time.sleep(dt)
        continue
    print(f"{time.strftime('%H:%M:%S')} act gfx {u16(104)}% gfxclk avg {u16(56)} MHz (cur {struct.unpack_from('<I', b, 0)[0]}) "
          f"socket {u16(116)} W board {u16(118)} W edge {t[0]} hot {t[1]} mem {t[4]} C fan {b[150]}%", flush=True)
    if i + 1 < n: time.sleep(dt)
