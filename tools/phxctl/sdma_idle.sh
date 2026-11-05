#!/bin/sh
# SPDX-License-Identifier: MIT
# sdma_idle.sh (box, root, after navi31_up.sh): the idle-power (fan) fix. Our stack never uses the SDMA engines, but the RLC grants
# GFX coarse-grain clock gating only after both engines report idle (RLC_RLCS_SDMA_INT_STAT 0xEEF5 = 0x003). A halted,
# never-started engine stays "busy" (0x300) forever: PMFW GFX activity 100 %, ~150 W idle. This starts queue 0 of SDMA0
# and SDMA1 the way Linux sdma_v6_0_gfx_resume does (CNTL with CTXEMPTY, F32 unhalt, a 4 KiB ring with rptr writeback
# and wptr polling in VRAM 0xA1E0000 / 0xA1E8000, inside mesboot's free work area), with no work queued: enabling the
# ring is enough for the engine to report idle. Prints the RLC's SDMA state. PHXCTL overrides the phxctl path.
P=${PHXCTL:-$HOME/gpu/phxctl.bin}
w(){ "$P" wreg "$1" "$2" >/dev/null; }; v(){ "$P" vramw "$1" "$2" >/dev/null; }
h(){ printf '0x%x' "$1"; }
for N in 0 1; do
  o=$((N * 0x600)); F=$((0xf89a + N * 0x20)); VB=$((0xa1e0000 + N * 0x8000))
  a(){ h $(( $1 + o )); }
  RPTR=$((VB + 0x2000)); WPOLL=$((VB + 0x2010))
  v $(h $RPTR) 0; v $(h $((RPTR + 4))) 0; v $(h $WPOLL) 0; v $(h $((WPOLL + 4))) 0
  w $(a 0x127c) 0x10002440                                   # SDMA_CNTL: CTXEMPTY interrupt (Linux value)
  w $(a 0x1282) 0; w $(a 0x12e0) 0x00841814                  # SEM_WAIT_FAIL_TIMER, RB_CNTL (1024 dw, poll, wb, off)
  w $(a 0x12e3) 0; w $(a 0x12e4) 0; w $(a 0x12e5) 0; w $(a 0x12e6) 0
  w $(a 0x1313) $(h $WPOLL); w $(a 0x1312) 0x80              # WPTR_POLL_ADDR
  w $(a 0x12e8) 0x80; w $(a 0x12e9) $(h $RPTR)               # RPTR_ADDR (writeback)
  w $(a 0x12e1) $(h $(( (0x8000000000 + VB) >> 8 ))); w $(a 0x12e2) 0   # RB_BASE (MC >> 8)
  w $(a 0x1315) 1; w $(a 0x12e5) 0; w $(a 0x12e6) 0; w $(a 0x12f2) 0; w $(a 0x130b) 0; w $(a 0x1315) 0   # MINOR_PTR_UPDATE, doorbell off
  w $(a 0x129c) 0x2c000689                                   # UTCL1_CNTL (Linux value)
  w $(h $F) 0x08084000                                       # F32 unhalt
  w $(a 0x12e0) 0x00841815; w $(a 0x12ea) 0x00000101         # RB_ENABLE, IB_ENABLE
done
sleep 1
echo "sdma idle: RLC_RLCS_SDMA_INT_STAT $("$P" regs 0xeef5 | awk '{print $2}') (0x00000003 = both idle)"
