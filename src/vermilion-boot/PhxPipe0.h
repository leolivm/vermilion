// SPDX-License-Identifier: MIT
// PhxPipe0.h: kext b99 pipe-0 (AOC on DP: OTG0 / HUBP0 / DIG1 / DP1, lit by the GOP) mode engine:
// an MMIO re-time of pipe 0 on the link the GOP trained (no DMUB command, no
// AUX, no PHY/PLL step, no DAL message).
// Kernel-free (stdint + snprintf); the kext glue is
// phx_pipe0.cpp; host-tested (with mutations).
//
// A plan (phx_p0_plan_<mode>.inc, generated from Linux register captures) is the TARGET value of every per-mode pipe-0 register;
// the engine owns the sequence, the same shell for a mode set and for its revert (Linux unplug/plug order of the AOC,
// as its unplug and plug register traces show it, minus
// every link/PHY/DIG step; RedNavi quiescePipe / programLive / setModeLive):
//   1 DPG0 black (colours 0, DPG_CONTROL 0x41) inside OTG0_OTG_MASTER_UPDATE_LOCK, read back after the unlock
//   2 HUBP0 blank (HUBP_BLANK_EN), wait HUBP_NO_OUTSTANDING_REQ (as hubp2_set_blank; not fatal, Linux only waits)
//   3 DP1 stream off (DP_VID_STREAM_ENABLE 0), poll DP_VID_STREAM_STATUS 0 <= 102 ms, DP_STEER_FIFO_RESET 1
//   4 OTG0 off (OTG_MASTER_EN 0), poll OTG_CURRENT_MASTER_EN_STATE 0 <= 200 ms, VTG0_ENABLE 0, poll OTG_BUSY 0 <= 100 ms
//   5 the plan's groups 1 DTO, 2 OTG0 timing, 3 frame position (VSTARTUP/VUPDATE/VREADY, VTG0 FP2/VCOUNT_INIT, DSCL0 OTG
//     blank), 4 HUBP0 DLG/TTU, each a field-masked read-modify-write
//   6 VTG0_ENABLE 1, OTG_MASTER_EN 1, poll OTG_CURRENT_MASTER_EN_STATE 1 <= 100 ms
//   7 group 5 MSA, then Linux v6.8 enc32_stream_encoder_dp_unblank from the steer FIFO on: DP_STEER_FIFO_RESET pulse,
//     DIG_SYMCLK_FE_ON, the DIG1 resync FIFO reset (b106), 100 us, DP_VID_STREAM_ENABLE 1, poll DP_VID_STREAM_STATUS 1
//     <= 200 ms (our check; Linux does not poll after the enable), the wait measured in the stat
//   8 HUBP0 unblank (when the snapshot had it unblanked), 2 OTG0 frames
//   9 DPG0 back to the snapshot (lock, colours, control, unlock, read back)
//  10 the ODM0 / HUBP0 underflow stickies the switch itself raised are cleared (OPTC_UNDERFLOW_CLEAR, HUBP_UNDERFLOW_CLEAR)
//  11 every double-buffered plan register reads back under its mask within 100 ms
// then verify: OTG0 totals, stream status 1, LINK_CNTL / DP_CONFIG unchanged, DP_VID_M within 1 % of the old M x
// new pclk / old pclk, OTG0 frames in 1 s = the plan rate +-1 % (min 1), HUBP0/ODM0 underflow clear in 100 samples over
// that second, HUBP0 blank and DPG0 as the snapshot, the inverse fingerprint unchanged, OTG1 moving when lit.
// setMode: survey (reads only; every refusal = zero writes) -> snapshot (every register the shell or the plan writes) ->
// shell(target) -> verify. A non-latched failure: shell(snapshot) -> verify(old) = FellBack; if that fails too: Dark
// (latched: no pipe-0 write until reboot; the GOP relights 1080p60 at every boot). Inverse fingerprint (pipe 1, DIG1/DIG3,
// the shared clocks, DET, stutter, watermarks, DP1 link) changed during the shell: Halted latch at once, then the revert
// of our own registers without that check (the console must not stay half-programmed), then FellBack or Dark.
// Gate: Env.enable (debug.phxpipe0_enable, default 0): every command refused before any register access.
// b101 macSet (below run()): the macOS path's one-call sequence (stutter off, UCLK hard min, round trip, set) for
// PhxAMDFB head 0's setDisplayMode and debug.phxpipe0=4.
// b103: two 144 plans with the same OTG0 timing, each planned next to one OTG1
// mode (1080p144 next to the Acer at 144, 1080p144a60 next to the Acer at 60; one Linux capture each). pick() chooses the plan
// for a rate by OTG1's totals (or by the totals the pipe-1 switch sets next, Env.otg1Next*); a set to the other pair's plan
// at the same timing (a "pair re-plan") runs the same shell; identify() prefers the plan every register already equals.
// Writes: only kAllowP0 registers, only inside each entry's field mask, never a kRefusedP0 register or a pipe-1 register
// (phxp2::writeAllowed); MMVM_INVALIDATE_ENG*_SEM is never read.
// b105: W() is one read-modify-write of the caller's field mask. b104's W
// compared a value the caller built from its own read against a second read and refused the write when any bit outside the
// allow mask differed, so a read-only status bit that moved between the two reads (HUBP0 HUBP_NO_OUTSTANDING_REQ at the
// stage-2 blank) refused a correct write: the fall-backs seen on the box. A refusal is
// now only a mask beyond the allow mask (a code error), named in the stat with its stage. pairSwitch(): the whole Acer
// switch in the safe order; a post-step that does not land switches pipe 1 back (the pair stays a planned one).
// b106: about 1 shell in 6 fell back with DP_VID_STREAM_STATUS still 0 200 ms
// after the enable (on the box). The hardware starts the stream at the next frame start, so 200 ms
// (>= 12 frames) is not a slow settle but a stalled path. Linux's unblank resets the DIG resync FIFO (DIG_FIFO_CTRL0) before
// every stream enable; b99-b105 stopped and restarted OTG0 without it. The shell now does that reset in Linux's order,
// writing only DIG_FIFO_RESET (DIG1_DIG_FIFO_CTRL0 left the refused DIG1 range), and the stat records the stream-status wait
// (last / max us), the stream failures and the DIG FIFO before the reset (DIG_FIFO_ERROR count).
// SCDC2 (11-01, from a Linux capture of this pair): a third 144 plan,
// 1080p144a165 = the AOC at 144 next to the Acer at 1080p165 (OTG1 0x81f/0x455). It differs from 1080p144 in 5 values
// (VUPDATE, VREADY, DST_AFTER_SCALER, PREFETCH, MIN_TTU_VBLANK) that Linux computed at DISPCLK / DPPCLK 391 MHz, so its
// DENTIST divider max is 0x2b (the other 144 plans 0x33): plans at one timing may differ in dentistMax (only the target's
// is read). In that capture Linux raised the clocks, then moved the Acer to 165, then re-planned pipe 0: pairPre never runs before
// a switch to 165 (the pipe-1 switch sends the hard mins itself) and never re-plans pipe 0 down to 1080p144 while the
// Acer runs at 165; pairPost follows OTG1 as before.
// b123: with Env.live (debug.phxpipe0_live = 1, default 0; b124 default 1) a pair re-plan whose
// differing values are all double-buffered group 3 / 4 registers runs live (liveShell: OTG0 lock, those writes, unlock,
// latch, then the same verify) instead of the shell, as Linux's dcn20_program_pipe; a failure reverts with the full shell.
#pragma once
#include <stdint.h>
#ifndef KERNEL
#include <stdio.h>
#endif
#include "PhxPipe2.h"

#ifndef PHX_P0_TYPES
#define PHX_P0_TYPES
struct phx_p0_info { uint32_t hz, hTotal, vTotal, pclkKhz, dentistMax, conv, ready, todo, pairHTotal, pairVTotal; const char *name; };
#endif

namespace phxp0 {

enum { kGDto = 1, kGOtg = 2, kGFrame = 3, kGHubp = 4, kGMsa = 5 };
static constexpr uint16_t kFTodo = 1, kFDerived = 2, kFDb = 4;   // phx_s2_step.pad of a plan entry (the generator's F_* flags)

// Every register phxp0 may write and the bits a write may change (the rest must stay as read). The plan generator's ALLOW is the same
// list (its selftest checks the names against dcn_3_2_0_offset.h).
struct Allow { uint32_t reg, mask; const char *name; };
static const Allow kAllowP0[] = {
    {0x00141, 0xffffffffu, "DP_DTO0_PHASE"},
    {0x0012F, 0x00000007u, "OTG_PIXEL_RATE_DIV (OTG0 K1/K2; Linux-convention plans only)"},
    {0x039EE, 0xffffffffu, "VTG0_CONTROL"},
    {0x03AB3, 0xf0000001u, "HUBP0_DCHUBP_CNTL (HUBP_BLANK_EN, HUBP_UNDERFLOW_CLEAR)"},
    {0x03AE9, 0xffffffffu, "HUBPREQ0_DCN_TTU_QOS_WM"},
    {0x03AEA, 0xffffffffu, "HUBPREQ0_DCN_GLOBAL_TTU_CNTL"},
    {0x03AEB, 0xffffffffu, "HUBPREQ0_DCN_SURF0_TTU_CNTL0"},
    {0x03AEC, 0xffffffffu, "HUBPREQ0_DCN_SURF0_TTU_CNTL1"},
    {0x03B03, 0xffffffffu, "HUBPREQ0_BLANK_OFFSET_0"},
    {0x03B04, 0xffffffffu, "HUBPREQ0_BLANK_OFFSET_1"},
    {0x03B05, 0xffffffffu, "HUBPREQ0_DST_DIMENSIONS"},
    {0x03B06, 0xffffffffu, "HUBPREQ0_DST_AFTER_SCALER"},
    {0x03B07, 0xffffffffu, "HUBPREQ0_PREFETCH_SETTINGS"},
    {0x03B09, 0xffffffffu, "HUBPREQ0_VBLANK_PARAMETERS_0"},
    {0x03B19, 0xffffffffu, "HUBPREQ0_PER_LINE_DELIVERY_PRE"},
    {0x03B1A, 0xffffffffu, "HUBPREQ0_PER_LINE_DELIVERY"},
    {0x03B1C, 0xffffffffu, "HUBPREQ0_REF_FREQ_TO_PIX_FREQ"},
    {0x041CF, 0xffffffffu, "DSCL0_OTG_H_BLANK"},
    {0x041D0, 0xffffffffu, "DSCL0_OTG_V_BLANK"},
    {0x04D14, 0xffffffffu, "DPG0_DPG_CONTROL"},
    {0x04D17, 0xffffffffu, "DPG0_DPG_COLOUR_R_CR"},
    {0x04D18, 0xffffffffu, "DPG0_DPG_COLOUR_G_Y"},
    {0x04D19, 0xffffffffu, "DPG0_DPG_COLOUR_B_CB"},
    {0x04F8A, 0x00001400u, "ODM0_OPTC_INPUT_GLOBAL_CONTROL (OPTC_UNDERFLOW_CLEAR)"},
    {0x04FEA, 0xffffffffu, "OTG0_OTG_H_TOTAL"},
    {0x04FEB, 0xffffffffu, "OTG0_OTG_H_BLANK_START_END"},
    {0x04FEC, 0xffffffffu, "OTG0_OTG_H_SYNC_A"},
    {0x04FED, 0xffffffffu, "OTG0_OTG_H_SYNC_A_CNTL"},
    {0x04FEF, 0xffffffffu, "OTG0_OTG_V_TOTAL"},
    {0x04FF6, 0xffffffffu, "OTG0_OTG_V_BLANK_START_END"},
    {0x04FF7, 0xffffffffu, "OTG0_OTG_V_SYNC_A"},
    {0x04FF8, 0xffffffffu, "OTG0_OTG_V_SYNC_A_CNTL"},
    {0x05001, 0x00000001u, "OTG0_OTG_CONTROL (OTG_MASTER_EN)"},
    {0x05046, 0xffffffffu, "OTG0_OTG_VSTARTUP_PARAM"},
    {0x05047, 0xffffffffu, "OTG0_OTG_VUPDATE_PARAM"},
    {0x05048, 0xffffffffu, "OTG0_OTG_VREADY_PARAM"},
    {0x0504A, 0x00000001u, "OTG0_OTG_MASTER_UPDATE_LOCK"},
    {0x05651, 0x00000002u, "DIG1_DIG_FIFO_CTRL0 (DIG_FIFO_RESET; b106)"},
    {0x056CC, 0x00000001u, "DP1_DP_VID_STREAM_CNTL (DP_VID_STREAM_ENABLE)"},
    {0x056CD, 0x00000001u, "DP1_DP_STEER_FIFO (DP_STEER_FIFO_RESET)"},
    {0x056D0, 0x00000c00u, "DP1_DP_VID_TIMING (DP_VID_N_MUL; Linux-convention plans only)"},
    {0x0570C, 0xffffffffu, "DP1_DP_MSA_TIMING_PARAM1"},
    {0x0570D, 0xffffffffu, "DP1_DP_MSA_TIMING_PARAM2"},
    {0x0570E, 0xffffffffu, "DP1_DP_MSA_TIMING_PARAM3"},
    {0x0570F, 0xffffffffu, "DP1_DP_MSA_TIMING_PARAM4"},
};
static constexpr unsigned kNAllow = sizeof kAllowP0 / sizeof kAllowP0[0];
inline bool linuxConvOnly(uint32_t r) { return r == 0x0012F || r == 0x056D0; }
// Never written by phxp0, whatever kAllowP0 says (the generator's REFUSED; its selftest checks every name inside).
static const phxp2::Range kRefusedP0[] = {
    {0x00100, 0x0012E},  // DCCG PHY PLLs, DS/GTC DTOs, DENTIST_DISPCLK_CNTL 0x124, DSCCLK
    {0x00130, 0x00140},  // DCCG clocks, OTG0_PIXEL_RATE_CNTL
    {0x00142, 0x00164},  // DP_DTO0_MODULO, OTG1-3 pixel rate / DTOs, DPPCLK DTOs, SYMCLKA-E
    {0x03512, 0x03516},  // PHY SYMCLK
    {0x03527, 0x03527}, {0x0358D, 0x0358D}, {0x0364E, 0x036F2},   // DMCUB
    {0x03620, 0x03620},  // DCIO
    {0x0392F, 0x039E5}, {0x039F2, 0x03A00},   // DCHUBBUB: watermarks, DET, ARB_DRAM_STATE_CNTL (their own steps)
    {0x03AA5, 0x03AB0},  // HUBP0 surface config / viewports
    {0x03AC7, 0x03AE7},  // HUBPREQ0 surface addresses (PhxAccel's flips own them)
    {0x05410, 0x05496},  // DP_AUX0-5
    {0x05628, 0x05650}, {0x05652, 0x056A6},   // AFMT1 / DME1 / DIG1 FE / BE (b106: all but DIG1_DIG_FIFO_CTRL0)
    {0x056C8, 0x056C8}, {0x056CB, 0x056CB},   // DP1_DP_LINK_CNTL, DP1_DP_CONFIG (the trained link)
    {0x05D2A, 0x06181},  // DCIO_UNIPHY A-E (PHY)
    {0x1A751, 0x1A762},  // MMVM_INVALIDATE_ENG*_SEM
};
inline bool refused(uint32_t r) {
    if (phxp2::forbidden(r) || r >= 0x10000) return true;
    for (const phxp2::Range &x : kRefusedP0) if (r >= x.lo && r <= x.hi) return true;
    return phxp2::writeAllowed(r);   // the pipe-1 engine's domain (OTG1/HUBP1/DIG3/PHY D ...)
}
// The bits phxp0 may change in r (0 = r is not writable).
inline uint32_t allowMask(uint32_t r) {
    if (refused(r)) return 0;
    for (const Allow &a : kAllowP0) if (a.reg == r) return a.mask;
    return 0;
}

static constexpr uint32_t kOtg0Control = 0x05001, kOtg0HTotal = 0x04FEA, kOtg0VTotal = 0x04FEF, kOtg0Frame = 0x0500C,
                          kOtg0Clock = 0x05045, kOtg0Lock = 0x0504A, kOtg0GlobalCtl0 = 0x0504F, kOtg0GlobalCtl2 = 0x05051,
                          kVtg0 = 0x039EE, kHubp0Cntl = 0x03AB3, kOdm0 = 0x04F8A, kStream = 0x056CC, kSteer = 0x056CD,
                          kLinkCntl = 0x056C8, kDpConfig = 0x056CB, kVidM = 0x056D2, kDpgCtl = 0x04D14, kDpgR = 0x04D17,
                          kDpgG = 0x04D18, kDpgB = 0x04D19, kDentist = 0x00124, kDet0 = 0x0397B, kArb = 0x039BC,
                          kOtg1Control = 0x05081, kOtg1Frame = 0x0508C, kDigFe = 0x0564B, kDigFifo = 0x05651;
static constexpr uint32_t kMasterEn = 1, kMasterEnState = 0x10000, kOtgBusy = 0x10000, kVtgEnable = 0x80000000u,
                          kLockStatus = 0x100, kStreamEn = 1, kStreamStatus = 0x10000, kSteerReset = 1, kHubpBlank = 1,
                          kHubpNoReq = 2, kHubpUnderflow = 0x70000000u, kHubpUnderflowClear = 0x80000000u,
                          kOdmSticky = 0x400, kOdmCurrent = 0x2000, kOdmClear = 0x1000, kDpgBlack = 0x41, kDpgMask = 0x01ff3171u;
// b106 DIG1_DIG_FE_CNTL DIG_SYMCLK_FE_ON; DIG1_DIG_FIFO_CTRL0 ENABLE / RESET / READ_START_LEVEL (the GOP's 0x1d = enabled,
// level 7, as Linux sets it), RESET_DONE, ERROR (W1C) (dcn_3_2_0_sh_mask.h)
static constexpr uint32_t kSymclkFeOn = 0x01000000u, kFifoReset = 2, kFifoCfg = 0x7f, kFifoGop = 0x1d, kFifoResetDone = 0x00100000u,
                          kFifoError = 0x30000000u;
static constexpr uint32_t kStreamOnBudgetUs = 200000;   // the stream-status poll after the enable (none in Linux)
static constexpr uint32_t kVidM60 = 0x468b;     // DP1_DP_VID_M at 148.5 MHz on the GOP's HBR x4 (read on the box; ~0.5 % downspread)
static constexpr uint32_t kVidM60Khz = 148500;

// What pipe 0 must never see change during a phxp0 shell: pipe 1, the encoders, the shared clocks / DET / stutter / the
// watermark set A, the trained DP1 link. Read before the first write and after every write.
struct Fp { uint32_t reg, mask; const char *name; };
static const Fp kInv[] = {
    {0x05081, 0xffffffffu, "OTG1_OTG_CONTROL"},
    {0x0506A, 0xffffffffu, "OTG1_OTG_H_TOTAL"},
    {0x0506F, 0xffffffffu, "OTG1_OTG_V_TOTAL"},
    {0x00144, 0x0000333bu, "OTG1_PIXEL_RATE_CNTL"},
    {0x0012F, 0x00000ff8u, "OTG_PIXEL_RATE_DIV(OTG1-3)"},
    {0x00124, 0x7f007f7fu, "DENTIST_DISPCLK_CNTL(dividers)"},
    {0x0397B, 0x0000001fu, "DCHUBBUB_DET0_CTRL"},
    {0x0397C, 0x0000001fu, "DCHUBBUB_DET1_CTRL"},
    {0x0397D, 0x0000001fu, "DCHUBBUB_DET2_CTRL"},
    {0x0397E, 0x0000001fu, "DCHUBBUB_DET3_CTRL"},
    {0x039BC, 0xffffffffu, "DCHUBBUB_ARB_DRAM_STATE_CNTL"},
    {0x039BE, 0xffffffffu, "DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A"},
    {0x039C1, 0xffffffffu, "DCHUBBUB_ARB_ALLOW_SR_ENTER_WATERMARK_A"},
    {0x039C2, 0xffffffffu, "DCHUBBUB_ARB_ALLOW_SR_EXIT_WATERMARK_A"},
    {0x039C3, 0xffffffffu, "DCHUBBUB_ARB_UCLK_PSTATE_CHANGE_WATERMARK_A"},
    {0x039C4, 0xffffffffu, "DCHUBBUB_ARB_FCLK_PSTATE_CHANGE_WATERMARK_A"},
    {0x09002, 0x0000000fu, "MPCC0_MPCC_OPP_ID"},
    {0x09017, 0x0000000fu, "MPCC1_MPCC_OPP_ID"},
    {0x0564B, 0xfef7ffffu, "DIG1_DIG_FE_CNTL"},
    {0x05671, 0xffffffffu, "DIG1_DIG_BE_CNTL"},
    {0x05672, 0x00000001u, "DIG1_DIG_BE_EN_CNTL"},
    {0x0584B, 0xfef7ffffu, "DIG3_DIG_FE_CNTL"},
    {0x05871, 0xffffffffu, "DIG3_DIG_BE_CNTL"},
    {0x05872, 0x00000001u, "DIG3_DIG_BE_EN_CNTL"},
    {0x056C8, 0xffffffffu, "DP1_DP_LINK_CNTL"},
    {0x056CB, 0xffffffffu, "DP1_DP_CONFIG"},
};
static constexpr unsigned kNInv = sizeof kInv / sizeof kInv[0];

struct Plan { const phx_s2_step *s; unsigned n; phx_p0_info info; };
static constexpr unsigned kMaxEntries = 48;

enum Res { kDone = 0, kSame = 1, kRefused = 2, kFellBack = 3, kDark = 4 };
static const char *const kResName[] = {"done", "same", "refused", "fell back", "dark"};
enum Cmd { kCmdSurvey = 1, kCmdSet = 2, kCmdRoundTrip = 3, kCmdMac = 4 };   // 4 = macSet (b101; the glue dispatches it, not run())
static const char *const kStageName[] = {"preflight", "dpg black", "hubp blank", "stream off", "otg off", "program", "otg on",
                                         "msa + stream on", "hubp unblank", "dpg restore", "stickies", "read-back", "verify"};
static constexpr int kStVerify = 12;

struct Env {
    phxdmub::Io io;
    int enable;        // debug.phxpipe0_enable (the kill switch)
    int p2Switching;   // the pipe-1 engine is between the undo and the end of a mode switch
    uint64_t (*nowUs)(void *c);   // monotonic clock for the verify window (IOSleep overshoots); null = the window is nominal
    uint32_t (*smu)(void *c, uint32_t msg, uint32_t arg);   // b101 macSet: one driver-mailbox SMU message, returns RESP (1 = OK, 0 = timeout); null = none
    uint32_t otg1NextH, otg1NextV;   // b103: the OTG1 totals the pipe-1 switch sets next (0 = none): a plan for them passes the pair check
    int solo;          // p0solo (debug.phxpipe0_solo, 0 default): with OTG1 dark, the 1080p144 plan is accepted (the AOC alone)
    int live;          // b123 (debug.phxpipe0_live, 0 default): a pair re-plan of double-buffered frame / HUBP registers runs live (liveShell)
};
#define PHXP0_HAS_LIVE 1
#define PHXP0_HAS_SOLO 1

struct Snap { uint32_t reg, val, mask; uint8_t group; uint16_t flags; };
struct State {
    int dark, halted, busy;   // latches: dark (a revert failed), halted (the inverse fingerprint moved); busy during a shell
    uint32_t gen;             // + 1 before the first write of every mode set (the b15 watchdog skips pipe 0 when it moves)
    int mode;                 // the mode the last survey identified (plan hz), 0 unknown
    int target, lastCmd, lastRes, lastStage;
    char why[200], warn[120];
    Snap snap[kMaxEntries]; unsigned nSnap;   // the revert table: every plan register before the first write
    Snap tgt[kMaxEntries];                     // the target being programmed
    int rtOk;                                  // a round trip (cmd 3) passed in this boot: real mode changes allowed
    uint32_t sDpg[4], sHubp, sStream, sLink, sCfg, sVidM, sPclk;   // the shell's own registers + link proof, before
    int invValid; uint32_t inv[kNInv];
    uint32_t odm0Base;
    uint32_t runs, surveys, done, same, refusedN, fell, darkN, haltedN, writes, rbMismatch, stickyClears, refusedWrites;
    uint32_t equal, nPlan;    // survey: plan registers already at the target value / plan entries
    uint32_t otg0Frames, vidM, shellUs, lockAcks;   // last verify: OTG0 frames in 1 s, DP_VID_M; last shell's delay sum
    uint32_t pubHTotal, pubVTotal;                  // what the last done / fell back left in OTG0 (rebasePipe0's target)
    int macBusy;                                    // b101: inside macSet (between its shells too): stat says busy=1
    uint32_t macRuns, uclkPins, arbForced, arbRestored;   // b101 macSet calls, UCLK hard mins sent, stutter forced off / written back
    const char *live;                                     // b103: the plan OTG0's registers equal at the last survey / set (null = none)
    uint32_t replans;                                     // b103: same-timing sets to the other pair's plan that ended done
    int curStage, curRevert;                              // b105: the shell stage running now (0 = none) and whether it is a revert
    uint32_t refReg, refMask, refAllow; int refStage, refRevert;   // b105: the last refused write (refReg 0 = none)
    char refWhy[112];
    uint32_t pairPostFails, pairRollbacks;                // b105 pairSwitch: post-steps that did not land / pipe-1 switches back
    uint32_t streamWaitUs, streamWaitMaxUs, streamFails;  // b106: the last stream-status wait after the enable, the longest that
                                                          // came up, the enables that did not come up within 200 ms
    uint32_t fifoResets, fifoErr, fifoPre, fifoPost;      // b106: DIG FIFO resets, of them with DIG_FIFO_ERROR set before, the
                                                          // last DIG1_DIG_FIFO_CTRL0 before the reset / after the stream poll
    uint32_t failRegs[5];                                 // b106: at the last stream-status timeout: DP_VID_STREAM_CNTL, DP_STEER_FIFO,
                                                          // DIG_FIFO_CTRL0 after / before the reset, DIG_FE_CNTL
    char s7Why[120];
    uint32_t liveReplans, liveFails, liveWrites;          // b123: live pair re-plans done / that failed (then the full-shell revert),
                                                          // plan writes of the last live re-plan
};

inline uint32_t rd(const Env &e, uint32_t r) { return phxp2::readAllowed(r) ? e.io.rd(e.io.c, r) : 0xdeadbeefu; }
inline void dly(const Env &e, State &st, uint32_t us) { e.io.delayUs(e.io.c, us); st.shellUs += us; }
// Polls (rd(r) & m) == want for up to budget us (10 us steps under 5 ms, else 1 ms; stepUs != 0: that step).
inline bool poll(const Env &e, State &st, uint32_t r, uint32_t m, uint32_t want, uint32_t budget, uint32_t stepUs = 0) {
    const uint32_t step = stepUs ? stepUs : budget > 5000 ? 1000 : 10;
    for (uint32_t us = 0;; us += step) {
        if ((rd(e, r) & m) == want) return true;
        if (us >= budget) return false;
        dly(e, st, step);
    }
}
inline bool frames(const Env &e, State &st, uint32_t n, uint32_t budgetUs) {
    const uint32_t f0 = rd(e, kOtg0Frame) & 0xffffff;
    for (uint32_t us = 0; us <= budgetUs; us += 1000) {
        if (((rd(e, kOtg0Frame) - f0) & 0xffffff) >= n) return true;
        dly(e, st, 1000);
    }
    return false;
}
inline bool moves(const Env &e, State &st, uint32_t reg) {   // up to 60 ms: > 3 frames at 60 Hz
    const uint32_t f0 = rd(e, reg) & 0xffffff;
    for (unsigned t = 0; t < 30; t++) { dly(e, st, 2000); if ((rd(e, reg) & 0xffffff) != f0) return true; }
    return false;
}
inline int setWhy(State &st, int res, int stage, const char *what, const char *detail) {
    snprintf(st.why, sizeof st.why, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
    st.lastRes = res; st.lastStage = stage;
    return res;
}

// ---- static check: what the generated table may contain
inline bool staticCheck(const Plan &p, const char **why) {
    if (!p.s || !p.n || p.n > kMaxEntries) { *why = "plan empty or larger than the engine"; return false; }
    if (p.info.hz < 24 || p.info.hz > 240 || !p.info.pclkKhz || !p.info.hTotal || !p.info.vTotal) { *why = "plan info not sane"; return false; }
    if (p.info.conv > 1) { *why = "unknown DTO convention"; return false; }
    unsigned last = kGDto, todo = 0;
    bool h = false, v = false;
    for (unsigned i = 0; i < p.n; i++) {
        const phx_s2_step &s = p.s[i];
        if (s.op != PHX_S2_W) { *why = "a pipe-0 plan holds only register writes (no DMUB / poll / delay / check entry)"; return false; }
        if (s.stage < last || s.stage > kGMsa) { *why = "plan groups out of order"; return false; }
        last = s.stage;
        const uint32_t am = allowMask(s.reg);
        if (!am) { *why = "a plan register is refused or not on the pipe-0 allow-list"; return false; }
        if (s.mask & ~am) { *why = "a plan mask exceeds the bits the allow-list lets phxp0 change"; return false; }
        if (!s.mask) { *why = "a plan entry with an empty mask"; return false; }
        if (linuxConvOnly(s.reg) && p.info.conv != 1) { *why = "OTG_PIXEL_RATE_DIV / DP_VID_TIMING in a GOP-convention plan"; return false; }
        if (s.reg == kOtg0Control || s.reg == kOtg0Lock || s.reg == kHubp0Cntl || s.reg == kStream || s.reg == kSteer ||
            s.reg == kOdm0 || s.reg == kDigFifo || (s.reg >= kDpgCtl && s.reg <= kDpgB)) { *why = "a plan entry writes a register the shell owns"; return false; }
        if (s.reg == kVtg0 && (s.mask & kVtgEnable)) { *why = "a plan entry writes VTG0_ENABLE (the shell's bit)"; return false; }
        for (unsigned k = 0; k < i; k++) if (p.s[k].reg == s.reg) { *why = "a register twice in one plan"; return false; }
        if (s.pad & kFTodo) todo++;
        if (s.reg == kOtg0HTotal) h = (s.val & s.mask) == p.info.hTotal;
        if (s.reg == kOtg0VTotal) v = (s.val & s.mask) == p.info.vTotal;
    }
    if (!h || !v) { *why = "plan info totals do not match its OTG0 H_TOTAL / V_TOTAL entries"; return false; }
    if (todo != p.info.todo || (p.info.ready && todo)) { *why = "plan TODO count / ready flag inconsistent"; return false; }
    return true;
}

// ---- writes: the only path to the hardware (allow-list + field mask; then the inverse fingerprint)
inline bool invSame(const Env &e, const State &st, const char **why) {
    for (unsigned k = 0; k < kNInv; k++)
        if ((rd(e, kInv[k].reg) & kInv[k].mask) != (st.inv[k] & kInv[k].mask)) { *why = kInv[k].name; return false; }
    return true;
}
// b105: one read-modify-write of the bits in `mask` to `val`; every other bit is written back as this read returns it, so a
// read-only status bit that moves between a caller's read and this one is not a change (b104 refused the write then).
// Refused before any access = `mask` reaches outside the register's allow mask (a code error: stat refused_write= names
// the register and the shell stage). checkInv = false only in the emergency revert after the inverse fingerprint moved.
inline bool W(const Env &e, State &st, uint32_t reg, uint32_t mask, uint32_t val, bool checkInv, const char **why) {
    const uint32_t am = allowMask(reg);
    if (!am || !mask || (mask & ~am)) {
        st.refusedWrites++; st.refReg = reg; st.refMask = mask; st.refAllow = am; st.refStage = st.curStage; st.refRevert = st.curRevert;
        snprintf(st.refWhy, sizeof st.refWhy, "write refused: 0x%05X mask 0x%08x outside the pipe-0 allow mask 0x%08x", reg, mask, am);
        *why = st.refWhy;
        return false;
    }
    const uint32_t old = rd(e, reg);
    e.io.wr(e.io.c, reg, (old & ~mask) | (val & mask));
    st.writes++;
    if (checkInv && st.invValid && !invSame(e, st, why)) { st.halted = 1; st.haltedN++; return false; }
    return true;
}
inline bool lock(const Env &e, State &st, bool on, bool checkInv, const char **why) {
    if (!W(e, st, kOtg0Lock, 1u, on ? 1u : 0u, checkInv, why)) return false;
    if (!on) return true;
    if (poll(e, st, kOtg0Lock, kLockStatus, kLockStatus, 1000)) { st.lockAcks++; return true; }
    W(e, st, kOtg0Lock, 1u, 0, checkInv, why);
    *why = "OTG0 UPDATE_LOCK_STATUS never 1";
    return false;
}

// DPG0 colours + control (black or the snapshot), inside the OTG0 lock when OTG0 runs; read back within 100 ms.
inline bool dpgSet(const Env &e, State &st, const uint32_t v[4], bool revert, bool checkInv, const char **why) {
    const bool running = rd(e, kOtg0Control) & kMasterEnState;
    bool locked = false;
    if (running) {
        locked = lock(e, st, true, checkInv, why);
        if (!locked && (!revert || st.halted)) return false;   // a revert goes on without the lock (OTG0 may be half off)
    }
    if (!W(e, st, kDpgR, ~0u, v[1], checkInv, why) || !W(e, st, kDpgG, ~0u, v[2], checkInv, why) || !W(e, st, kDpgB, ~0u, v[3], checkInv, why) ||
        !W(e, st, kDpgCtl, ~0u, v[0], checkInv, why)) { if (locked) W(e, st, kOtg0Lock, 1u, 0, false, why); return false; }
    if (locked && !W(e, st, kOtg0Lock, 1u, 0, checkInv, why)) return false;
    if (!poll(e, st, kDpgCtl, kDpgMask, v[0] & kDpgMask, 100000)) { st.rbMismatch++; *why = "DPG0_DPG_CONTROL did not latch within 100 ms"; return false; }
    return true;
}

// b106: the DIG1 resync FIFO reset of Linux v6.8 enc32_stream_encoder_dp_unblank (dcn32_dio_stream_encoder.c), between the
// steer FIFO release and the stream enable: wait DIG_SYMCLK_FE_ON, DIG_FIFO_RESET 1, wait DIG_FIFO_RESET_DONE 1,
// DIG_FIFO_RESET 0, wait RESET_DONE 0 (each wait 10 us steps <= 50 ms, Linux's REG_WAIT 10 x 5000; without the symbol clock
// 10 us instead, as enc32_reset_fifo). Linux goes on after a wait times out (REG_WAIT only logs), so this does too (warn=)
// and the stream-status poll decides. DIG_FIFO_ENABLE and READ_START_LEVEL stay as the survey found them (1 / 7 = the
// values Linux writes there). The read-modify-write writes DIG_FIFO_ERROR (W1C) back as read, which clears a set error as
// Linux's REG_UPDATE does (Linux trace 776.530722: 0x2000011d -> 0x0010011f). Same order on the box under Linux at every
// unblank in three captures (0x5651).
inline bool digFifoReset(const Env &e, State &st, bool checkInv, const char **why) {
    const bool sym = poll(e, st, kDigFe, kSymclkFeOn, kSymclkFeOn, 50000, 10);
    if (!sym) snprintf(st.warn, sizeof st.warn, "DIG_SYMCLK_FE_ON 0 for 50 ms before the DIG FIFO reset");
    st.fifoPre = rd(e, kDigFifo);
    if (st.fifoPre & kFifoError) st.fifoErr++;
    st.fifoResets++;
    for (uint32_t want = kFifoReset;; want = 0) {
        if (!W(e, st, kDigFifo, kFifoReset, want, checkInv, why)) return false;
        if (!sym) dly(e, st, 10);
        else if (!poll(e, st, kDigFifo, kFifoResetDone, want ? kFifoResetDone : 0, 50000, 10))
            snprintf(st.warn, sizeof st.warn, "DIG_FIFO_RESET_DONE not %d 50 ms after DIG_FIFO_RESET %d", want ? 1 : 0, want ? 1 : 0);
        if (!want) return true;
    }
}
// b106: polls like poll() and returns the time it waited: the clock when there is one (IOSleep overshoots), else the
// poll's own count.
inline bool pollTimed(const Env &e, State &st, uint32_t r, uint32_t m, uint32_t want, uint32_t budget, uint32_t *waitedUs) {
    const uint64_t t0 = e.nowUs ? e.nowUs(e.io.c) : 0;
    const uint32_t c0 = st.shellUs;
    const bool ok = poll(e, st, r, m, want, budget);
    const uint64_t d = e.nowUs ? e.nowUs(e.io.c) - t0 : st.shellUs - c0;
    *waitedUs = d > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(d);
    return ok;
}

// The shell. t = the entries to program (a plan, or the snapshot for the revert); n entries. Returns the stage that
// failed, 0 = all passed.
// b105: st.curStage follows the stage (W names it in a refusal); shellStages() resets it on every return.
static constexpr uint32_t kHubpCntlMask = kHubpUnderflow | kHubpUnderflowClear | kHubpBlank;   // HUBP0_DCHUBP_CNTL bits the shell writes (= its allow mask)
inline int shellStages(const Env &e, State &st, const Snap *t, unsigned n, bool revert, bool checkInv, const char **why);
// Every entry of t reads back under its mask within 100 ms (the double-buffered ones latch at a frame boundary).
inline bool latchWait(const Env &e, State &st, const Snap *t, unsigned n, const char **why) {
    for (uint32_t us = 0;; us += 2000) {
        unsigned bad = n;
        for (unsigned i = 0; i < n && bad == n; i++) if ((rd(e, t[i].reg) ^ t[i].val) & t[i].mask) bad = i;
        if (bad == n) return true;
        if (us >= 100000) { st.rbMismatch++; snprintf(st.warn, sizeof st.warn, "0x%05X reads 0x%08x, want 0x%08x & 0x%08x", t[bad].reg, rd(e, t[bad].reg), t[bad].val, t[bad].mask); *why = "a plan register did not latch within 100 ms"; return false; }
        dly(e, st, 2000);
    }
}
inline int shell(const Env &e, State &st, const Snap *t, unsigned n, bool revert, bool checkInv, const char **why) {
    st.curRevert = revert ? 1 : 0;
    const int r = shellStages(e, st, t, n, revert, checkInv, why);
    st.curStage = 0; st.curRevert = 0;
    return r;
}
inline int shellStages(const Env &e, State &st, const Snap *t, unsigned n, bool revert, bool checkInv, const char **why) {
    st.shellUs = 0;
    const char *w = nullptr;
    // 1 DPG0 black
    static const uint32_t black[4] = {kDpgBlack, 0, 0, 0};
    st.curStage = 1;
    if (!dpgSet(e, st, black, revert, checkInv, &w)) { *why = w; if (!revert || st.halted) return 1; }
    // 2 HUBP0 blank (the underflow status / clear bits written 0, as before)
    st.curStage = 2;
    uint32_t h = rd(e, kHubp0Cntl);
    if (!(h & kHubpBlank)) {
        if (!W(e, st, kHubp0Cntl, kHubpCntlMask, kHubpBlank, checkInv, &w)) { *why = w; return 2; }
        if (!poll(e, st, kHubp0Cntl, kHubpNoReq, kHubpNoReq, 20000)) snprintf(st.warn, sizeof st.warn, "HUBP_NO_OUTSTANDING_REQ not 1 after the blank (20 ms)");
    }
    // 3 stream off, steer FIFO reset
    st.curStage = 3;
    if (rd(e, kStream) & kStreamEn) {
        if (!W(e, st, kStream, kStreamEn, 0, checkInv, &w)) { *why = w; return 3; }
    }
    if (!poll(e, st, kStream, kStreamStatus, 0, 102000)) { *why = "DP_VID_STREAM_STATUS still 1 102 ms after the stream disable"; if (!revert) return 3; }
    if (!(rd(e, kSteer) & kSteerReset) && !W(e, st, kSteer, kSteerReset, kSteerReset, checkInv, &w)) { *why = w; return 3; }
    // 4 OTG0 off
    st.curStage = 4;
    if (rd(e, kOtg0Control) & kMasterEn) {
        if (!W(e, st, kOtg0Control, kMasterEn, 0, checkInv, &w)) { *why = w; return 4; }
    }
    if (!poll(e, st, kOtg0Control, kMasterEnState, 0, 200000)) { *why = "OTG_CURRENT_MASTER_EN_STATE still 1 200 ms after OTG_MASTER_EN 0"; if (!revert) return 4; }
    if ((rd(e, kVtg0) & kVtgEnable) && !W(e, st, kVtg0, kVtgEnable, 0, checkInv, &w)) { *why = w; return 4; }
    if (!poll(e, st, kOtg0Clock, kOtgBusy, 0, 100000)) { *why = "OTG_BUSY still 1 100 ms after the disable"; if (!revert) return 4; }
    // 5 the plan (groups 1-4), OTG0 off: field-masked read-modify-write; immediate read-back unless double-buffered
    st.curStage = 5;
    for (unsigned i = 0; i < n; i++) {
        if (t[i].group > kGHubp) continue;
        if (!W(e, st, t[i].reg, t[i].mask, t[i].val, checkInv, &w)) { *why = w; return 5; }
        if (!(t[i].flags & kFDb) && ((rd(e, t[i].reg) ^ t[i].val) & t[i].mask)) { st.rbMismatch++; *why = "read-back mismatch"; return 5; }
    }
    // 6 OTG0 on
    st.curStage = 6;
    if (!W(e, st, kVtg0, kVtgEnable, kVtgEnable, checkInv, &w)) { *why = w; return 6; }
    if (!W(e, st, kOtg0Control, kMasterEn, kMasterEn, checkInv, &w)) { *why = w; return 6; }
    if (!poll(e, st, kOtg0Control, kMasterEnState, kMasterEnState, 100000)) { *why = "OTG_CURRENT_MASTER_EN_STATE not 1 100 ms after OTG_MASTER_EN 1"; return 6; }
    // 7 MSA, then enc32_stream_encoder_dp_unblank from the steer FIFO on: steer FIFO pulse, the DIG FIFO reset (b106),
    // 100 us, stream on (the M/N part stays out: DP_VID_M_N_GEN_EN runs, the hardware measures M)
    st.curStage = 7;
    for (unsigned i = 0; i < n; i++) {
        if (t[i].group != kGMsa) continue;
        if (!W(e, st, t[i].reg, t[i].mask, t[i].val, checkInv, &w)) { *why = w; return 7; }
    }
    if (!(rd(e, kSteer) & kSteerReset) && !W(e, st, kSteer, kSteerReset, kSteerReset, checkInv, &w)) { *why = w; return 7; }
    dly(e, st, 10);
    if (!W(e, st, kSteer, kSteerReset, 0, checkInv, &w)) { *why = w; return 7; }
    if (!digFifoReset(e, st, checkInv, &w)) { *why = w; return 7; }
    dly(e, st, 100);
    if (!W(e, st, kStream, kStreamEn, kStreamEn, checkInv, &w)) { *why = w; return 7; }
    {
        uint32_t waited = 0;
        const bool up = pollTimed(e, st, kStream, kStreamStatus, kStreamStatus, kStreamOnBudgetUs, &waited);
        st.streamWaitUs = waited; st.fifoPost = rd(e, kDigFifo);
        if (!up) {
            st.streamFails++;
            st.failRegs[0] = rd(e, kStream); st.failRegs[1] = rd(e, kSteer); st.failRegs[2] = st.fifoPost; st.failRegs[3] = st.fifoPre;
            st.failRegs[4] = rd(e, kDigFe);
            snprintf(st.s7Why, sizeof st.s7Why, "DP_VID_STREAM_STATUS not 1 200 ms after the stream enable (%u us, DIG_FIFO_CTRL0 0x%x)", waited, st.fifoPost);
            *why = st.s7Why;
            return 7;
        }
        if (waited > st.streamWaitMaxUs) st.streamWaitMaxUs = waited;
    }
    // 8 HUBP0 unblank (as the snapshot), 2 frames
    st.curStage = 8;
    h = rd(e, kHubp0Cntl);
    if (!(st.sHubp & kHubpBlank) && (h & kHubpBlank) &&
        !W(e, st, kHubp0Cntl, kHubpCntlMask, 0, checkInv, &w)) { *why = w; return 8; }
    if (!frames(e, st, 2, 100000)) { *why = "OTG0 frame counter stopped after the enable"; return 8; }
    // 9 DPG0 as the snapshot
    st.curStage = 9;
    if (!dpgSet(e, st, st.sDpg, revert, checkInv, &w)) { *why = w; return 9; }
    // 10 the switch's own underflow stickies
    st.curStage = 10;
    const uint32_t o = rd(e, kOdm0);
    if ((o & kOdmSticky & ~st.odm0Base) && !(o & kOdmCurrent)) {
        if (!W(e, st, kOdm0, kOdmSticky | kOdmClear, kOdmClear, checkInv, &w)) { *why = w; return 10; }
        st.stickyClears++;
    }
    h = rd(e, kHubp0Cntl);
    if (h & kHubpUnderflow) {
        if (!W(e, st, kHubp0Cntl, kHubpUnderflow | kHubpUnderflowClear, kHubpUnderflowClear, checkInv, &w) ||
            !W(e, st, kHubp0Cntl, kHubpUnderflow | kHubpUnderflowClear, 0, checkInv, &w)) { *why = w; return 10; }
        st.stickyClears++;
    }
    // 11 the double-buffered plan registers latched
    st.curStage = 11;
    return latchWait(e, st, t, n, why) ? 0 : 11;
}

// b123: the live pair re-plan. A same-timing set to another pair's plan (the Acer
// changed mode next to the AOC at 144: 1080p144 <-> 1080p144a165 / 1080p144a60) changes only frame-position (group 3) and
// HUBP0 DLG/TTU (group 4) values; the full shell blanked the AOC for it (DPG black, HUBP blank, DP stream off, OTG0 off)
// at every Acer switch (seen on the box). Linux programs the same change on the lit pipe as dcn20_program_pipe
// does for an unchanged stream (program_global_sync + hubp_setup inside OTG_MASTER_UPDATE_LOCK, no blank_pixel_data:
// Linux v6.8 dcn20_hwseq.c; box captures of Linux, 47.268441-.268472 with the AOC at 60 and
// 201.368737-.368801 at 144: OTG0 lock, VUPDATE / VREADY / DST_AFTER_SCALER / PREFETCH / GLOBAL_TTU, unlock, ~60 us).
// liveFits: at least one entry differs, and every differing entry is group 3 or 4, double-buffered (kFDb) and not
// VTG0_CONTROL. liveShell: OTG0 lock (acknowledged), the differing entries only (field-masked read-modify-write), unlock,
// then every plan register latched within 100 ms (stage 11). No other register is written.
inline bool liveFits(const Env &e, const Snap *t, unsigned n) {
    unsigned d = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!((rd(e, t[i].reg) ^ t[i].val) & t[i].mask)) continue;
        if ((t[i].group != kGFrame && t[i].group != kGHubp) || !(t[i].flags & kFDb) || t[i].reg == kVtg0) return false;
        d++;
    }
    return d != 0;
}
inline int liveShell(const Env &e, State &st, const Snap *t, unsigned n, bool checkInv, const char **why) {
    st.shellUs = 0; st.curRevert = 0; st.liveWrites = 0;
    const char *w = nullptr;
    st.curStage = 5;
    if (!lock(e, st, true, checkInv, &w)) { *why = w; st.curStage = 0; return 5; }
    for (unsigned i = 0; i < n; i++) {
        if (!((rd(e, t[i].reg) ^ t[i].val) & t[i].mask)) continue;
        if (!W(e, st, t[i].reg, t[i].mask, t[i].val, checkInv, &w)) { *why = w; W(e, st, kOtg0Lock, 1u, 0, false, &w); st.curStage = 0; return 5; }
        st.liveWrites++;
    }
    if (!W(e, st, kOtg0Lock, 1u, 0, checkInv, &w)) { *why = w; st.curStage = 0; return 5; }
    st.curStage = 11;
    const int r = latchWait(e, st, t, n, why) ? 0 : 11;
    st.curStage = 0;
    return r;
}

// Expected DP_VID_M at pclk: the M measured before x pclk / pclk before (M_N_GEN_EN; the downspread cancels).
inline uint32_t expectM(const State &st, uint32_t pclkKhz) {
    return static_cast<uint32_t>((static_cast<uint64_t>(st.sVidM) * pclkKhz + st.sPclk / 2) / (st.sPclk ? st.sPclk : 1));
}
inline bool mWithin(uint32_t m, uint32_t want) { const uint32_t d = m > want ? m - want : want - m; return d * 100 <= want; }

inline bool verify(const Env &e, State &st, uint32_t hTotal, uint32_t vTotal, uint32_t hz, uint32_t pclkKhz, bool checkInv, const char **why) {
    if ((rd(e, kOtg0HTotal) & 0x7fff) != hTotal || (rd(e, kOtg0VTotal) & 0x7fff) != vTotal) { *why = "OTG0 totals are not the plan's"; return false; }
    if ((rd(e, kStream) & (kStreamEn | kStreamStatus)) != (kStreamEn | kStreamStatus)) { *why = "DP1 stream not enabled / status 0"; return false; }
    if ((rd(e, kDigFifo) & kFifoCfg) != kFifoGop) { *why = "DIG1_DIG_FIFO_CTRL0 not back at enabled, out of reset, level 7 (0x1d)"; return false; }
    if (rd(e, kLinkCntl) != st.sLink || rd(e, kDpConfig) != st.sCfg) { *why = "DP1 LINK_CNTL / DP_CONFIG changed"; return false; }
    if ((rd(e, kHubp0Cntl) & kHubpBlank) != (st.sHubp & kHubpBlank)) { *why = "HUBP0 blank state is not the snapshot's"; return false; }
    if ((rd(e, kDpgCtl) & kDpgMask) != (st.sDpg[0] & kDpgMask)) { *why = "DPG0_DPG_CONTROL is not the snapshot's"; return false; }
    const uint32_t wantM = expectM(st, pclkKhz);
    bool mOk = false;
    for (unsigned i = 0; i <= 50 && !mOk; i++) { st.vidM = rd(e, kVidM) & 0xffffff; mOk = mWithin(st.vidM, wantM); if (!mOk) dly(e, st, 2000); }
    if (!mOk) { snprintf(st.warn, sizeof st.warn, "DP_VID_M 0x%x, want 0x%x +-1 %%", st.vidM, wantM); *why = "DP_VID_M not within 1 % of the expected value"; return false; }
    // ~1 s: OTG0 frames, underflow sampled every 10 ms; the expected count follows the measured window (on the box:
    // 100 x IOSleep(10) took ~1.067 s, 64 frames at 60 Hz)
    const uint64_t t0 = e.nowUs ? e.nowUs(e.io.c) : 0;
    const uint32_t f0 = rd(e, kOtg0Frame) & 0xffffff;
    for (unsigned i = 0; i < 100; i++) {
        dly(e, st, 10000);
        if (rd(e, kHubp0Cntl) & kHubpUnderflow) { *why = "HUBP0 underflow after the switch"; return false; }
        if (rd(e, kOdm0) & (kOdmSticky | kOdmCurrent) & ~st.odm0Base) { *why = "OPTC0 underflow after the switch"; return false; }
    }
    st.otg0Frames = (rd(e, kOtg0Frame) - f0) & 0xffffff;
    const uint64_t winUs = e.nowUs ? e.nowUs(e.io.c) - t0 : 1000000;
    const uint32_t want = static_cast<uint32_t>((static_cast<uint64_t>(hz) * winUs + 500000) / 1000000);
    const uint32_t tol = (want / 100 ? want / 100 : 1) + 1;   // + 1: a frame boundary on either edge of the window
    if (st.otg0Frames + tol < want || st.otg0Frames > want + tol) { snprintf(st.warn, sizeof st.warn, "OTG0 %u frames in %llu us, want %u +-%u", st.otg0Frames, static_cast<unsigned long long>(winUs), want, tol); *why = "OTG0 frame rate is not the plan's"; return false; }
    if (checkInv && !invSame(e, st, why)) { st.halted = 1; st.haltedN++; return false; }
    if ((rd(e, kOtg1Control) & kMasterEnState) && !moves(e, st, kOtg1Frame)) { *why = "OTG1 frame counter stopped"; return false; }
    return true;
}

inline int refuse(State &st, const char *what, const char *detail) { st.refusedN++; return setWhy(st, kRefused, 0, what, detail); }
inline unsigned equalCount(const Env &e, const Plan &p) {
    unsigned k = 0;
    for (unsigned i = 0; i < p.n; i++) k += !((rd(e, p.s[i].reg) ^ p.s[i].val) & p.s[i].mask);
    return k;
}
// OTG0's plan by its totals; with two plans at the same timing (b103) the one every register already equals, else the
// first (the timing fields the callers use, hz / totals / pclk, are the same in both: staticCheck refuses otherwise).
inline const Plan *identify(const Env &e, const Plan *const *pl, unsigned np) {
    const uint32_t h = rd(e, kOtg0HTotal) & 0x7fff, v = rd(e, kOtg0VTotal) & 0x7fff;
    const Plan *first = nullptr;
    unsigned nt = 0;
    for (unsigned i = 0; i < np; i++) if (pl[i] && pl[i]->info.hTotal == h && pl[i]->info.vTotal == v) { if (!first) first = pl[i]; nt++; }
    if (nt < 2) return first;
    for (unsigned i = 0; i < np; i++)
        if (pl[i] && pl[i]->info.hTotal == h && pl[i]->info.vTotal == v && equalCount(e, *pl[i]) == pl[i]->n) return pl[i];
    return first;
}
// b103: p was planned for any OTG1 (pair 0/0), or OTG1 is lit at exactly p's pair (as it runs now, or as Env.otg1Next*
// says the pipe-1 switch sets it next). Reads OTG1 only.
// p0solo (the AOC alone): OTG1 dark and debug.phxpipe0_solo = 1
// accept the plan planned next to the Acer at 1080p144: its pipe-0 groups 1-3 and 5 are the AOC's own timing, and
// its HUBP0 DLG/TTU group (4) is the one Linux runs for this AOC mode with DET 9 (the one-display survey below asks DET0 9).
static constexpr uint32_t kSoloPairH = 0x807, kSoloPairV = 0x449;
inline bool pairFits(const Env &e, const Plan &p) {
    if (!p.info.pairHTotal) return true;
    if (!(rd(e, kOtg1Control) & kMasterEnState))
        return e.solo && !e.otg1NextH && p.info.pairHTotal == kSoloPairH && p.info.pairVTotal == kSoloPairV;
    const uint32_t h = e.otg1NextH ? e.otg1NextH : rd(e, 0x0506A), v = e.otg1NextH ? e.otg1NextV : rd(e, 0x0506F);
    return p.info.pairHTotal == h && p.info.pairVTotal == v;
}
// b103: the plan for `hz` next to OTG1 as it runs now (or, with Env.otg1Next*, as the pipe-1 switch will set it): a plan
// with pair 0/0 fits any OTG1, a pair plan only OTG1 lit at exactly its totals. None fits = the first plan with that rate
// (its survey then refuses with the pair reason). Gate off or a latch: no register read, the first plan with that rate.
inline const Plan *pick(const Env &e, const State &st, const Plan *const *pl, unsigned np, uint32_t hz) {
    const Plan *first = nullptr;
    for (unsigned i = 0; i < np && !first; i++) if (pl[i] && pl[i]->info.hz == hz) first = pl[i];
    if (!first || !e.enable || st.dark || st.halted) return first;
    for (unsigned i = 0; i < np; i++) if (pl[i] && pl[i]->info.hz == hz && pairFits(e, *pl[i])) return pl[i];
    return first;
}
// b103: plans that share a timing must share every timing field (identify() returns either one).
inline bool sameTimingConsistent(const Plan *const *pl, unsigned np, const char **why) {
    for (unsigned i = 0; i < np; i++)
        for (unsigned k = i + 1; k < np; k++) {
            const phx_p0_info &a = pl[i]->info, &b = pl[k]->info;
            if (a.hTotal != b.hTotal || a.vTotal != b.vTotal) continue;
            // SCDC2: dentistMax may differ (1080p144a165 needs 0x2b): only the target's is read (survey), never identify()'s
            if (a.hz != b.hz || a.pclkKhz != b.pclkKhz || a.conv != b.conv) { *why = "two plans at the same totals differ in rate / pclk / convention"; return false; }
            if (a.pairHTotal == b.pairHTotal && a.pairVTotal == b.pairVTotal) { *why = "two plans at the same totals planned next to the same OTG1 mode"; return false; }
            if (!a.pairHTotal || !b.pairHTotal) { *why = "a same-timing plan without an OTG1 pair (pick() could not tell them apart)"; return false; }
        }
    return true;
}
inline bool dentistFits(uint32_t d, uint32_t max) {
    return !max || ((d & 0x7f) <= max && ((d >> 8) & 0x7f) <= max && ((d >> 24) & 0x7f) <= max);
}

// Reads only. Every refusal leaves the hardware untouched; with the gate off nothing is even read.
inline int survey(const Env &e, State &st, const Plan *const *pl, unsigned np, const Plan &tg, const Plan **curOut) {
    st.surveys++;
    const char *w = nullptr;
    char d[120];
    if (!e.enable) return refuse(st, "debug.phxpipe0_enable = 0", "the pipe-0 engine is switched off (kill switch)");
    if (st.dark) return refuse(st, "dark", "a revert failed earlier in this boot: no pipe-0 write until reboot");
    if (st.halted) return refuse(st, "halted", "pipe 1 / shared state moved during a pipe-0 switch earlier in this boot: reboot");
    if (!staticCheck(tg, &w)) return refuse(st, "target plan", w);
    if (!sameTimingConsistent(pl, np, &w)) return refuse(st, "plans", w);
    if (!tg.info.ready) { snprintf(d, sizeof d, "%s holds %u TODO-CAPTURE entries (regenerate with the Linux capture)", tg.info.name, tg.info.todo); return refuse(st, "target plan not ready", d); }
    if (e.p2Switching) return refuse(st, "preflight", "the pipe-1 engine is switching modes");
    const uint32_t c = rd(e, kOtg0Control);
    if (!(c & kMasterEn) || !(c & kMasterEnState)) return refuse(st, "preflight", "OTG0 not running");
    const Plan *cur = identify(e, pl, np);
    if (!cur) { snprintf(d, sizeof d, "OTG0 totals 0x%x/0x%x are not a known AOC mode", rd(e, kOtg0HTotal) & 0x7fff, rd(e, kOtg0VTotal) & 0x7fff); return refuse(st, "preflight", d); }
    if (!staticCheck(*cur, &w)) return refuse(st, "current plan", w);
    if ((rd(e, kLinkCntl) & 0x110) != 0x110) return refuse(st, "preflight", "DP1 link not trained (DP1_DP_LINK_CNTL TRAINING_COMPLETE | LINK_STATUS)");
    if ((rd(e, kDpConfig) & 3) != 3) return refuse(st, "preflight", "DP1_DP_CONFIG is not 4 lanes");
    if ((rd(e, kStream) & (kStreamEn | kStreamStatus)) != (kStreamEn | kStreamStatus)) return refuse(st, "preflight", "DP1 stream not running");
    if ((rd(e, kDigFifo) & kFifoCfg) != kFifoGop) return refuse(st, "preflight", "DIG1_DIG_FIFO_CTRL0 is not the GOP's enabled, out of reset, read start level 7 (0x1d)");
    const uint32_t m = rd(e, kVidM) & 0xffffff;
    const uint32_t wantM = static_cast<uint32_t>((static_cast<uint64_t>(kVidM60) * cur->info.pclkKhz + kVidM60Khz / 2) / kVidM60Khz);
    if (!mWithin(m, wantM)) { snprintf(d, sizeof d, "DP_VID_M 0x%x, want 0x%x +-1 %% (the GOP's HBR x4 link at %u kHz)", m, wantM, cur->info.pclkKhz); return refuse(st, "preflight", d); }
    if (!dentistFits(rd(e, kDentist), tg.info.dentistMax)) { snprintf(d, sizeof d, "DENTIST dividers above 0x%x: DISPCLK/DPPCLK too low for %s (the two DAL hard mins first)", tg.info.dentistMax, tg.info.name); return refuse(st, "preflight", d); }
    const bool otg1 = rd(e, kOtg1Control) & kMasterEnState;
    // b103: the pair (pairFits); e.otg1Next* = the OTG1 totals the pipe-1 switch sets right after this call (the safe
    // order: the heavier pair's plan first)
    if (!pairFits(e, tg)) {
        snprintf(d, sizeof d, "%s was planned next to OTG1 at 0x%x/0x%x (the Acer at 1080p%s); OTG1 %s 0x%x/0x%x", tg.info.name, tg.info.pairHTotal,
                 tg.info.pairVTotal, tg.info.pairHTotal == 0x897 ? "60" : tg.info.pairHTotal == 0x81f ? "165" : "144", otg1 ? "at" : "off,", rd(e, 0x0506A), rd(e, 0x0506F));
        return refuse(st, "preflight", d);
    }
    if (otg1 && tg.info.hz > 60 && (rd(e, kDet0) & 0x1f) < 9) return refuse(st, "preflight", "DET0 below 9 segments with two pipes lit");
    if (!otg1 && tg.info.hz > 60 && (rd(e, kDet0) & 0x1f) < 9) return refuse(st, "preflight", "DET0 below 9 segments for 144 with one display (det.sh set 9 first)");
    if ((rd(e, kArb) & 3) != 2) return refuse(st, "preflight", "stutter allowed (DCHUBBUB_ARB_DRAM_STATE_CNTL & 3 != 2): force it off first (0x1002)");
    if (rd(e, kHubp0Cntl) & kHubpUnderflow) return refuse(st, "preflight", "HUBP0 underflow status set");
    if (rd(e, kOdm0) & (kOdmSticky | kOdmCurrent)) return refuse(st, "preflight", "ODM0 OPTC underflow sticky / current set");
    if (rd(e, kHubp0Cntl) & kHubpBlank) return refuse(st, "preflight", "HUBP0 blanked");
    if (rd(e, kDpgCtl) & 1) return refuse(st, "preflight", "DPG0 enabled");
    if (rd(e, kOtg0Lock) & 1) return refuse(st, "preflight", "OTG0_OTG_MASTER_UPDATE_LOCK held");
    if ((rd(e, kOtg0GlobalCtl2) >> 25) & 7) return refuse(st, "preflight", "OTG0 OTG_MASTER_UPDATE_LOCK_SEL is not OTG0");
    if (rd(e, kOtg0GlobalCtl0) & 0x80000000u) return refuse(st, "preflight", "OTG0 MASTER_UPDATE_LOCK_DB_EN set");
    if (!(rd(e, kVtg0) & kVtgEnable)) return refuse(st, "preflight", "VTG0 not enabled");
    if (!moves(e, st, kOtg0Frame)) return refuse(st, "preflight", "OTG0 frame counter not moving");
    if (otg1 && !moves(e, st, kOtg1Frame)) return refuse(st, "preflight", "OTG1 lit but its frame counter is not moving");
    for (unsigned k = 0; k < kNInv; k++) st.inv[k] = rd(e, kInv[k].reg);
    st.invValid = 1;
    st.odm0Base = rd(e, kOdm0) & (kOdmSticky | kOdmCurrent);
    st.equal = equalCount(e, tg); st.nPlan = tg.n; st.mode = static_cast<int>(cur->info.hz);
    st.live = equalCount(e, *cur) == cur->n ? cur->info.name : nullptr;
    *curOut = cur;
    snprintf(d, sizeof d, "OTG0 at %s, target %s: %u/%u plan registers already equal, DP_VID_M 0x%x", cur->info.name, tg.info.name, st.equal, tg.n, m);
    return setWhy(st, kDone, 0, "survey passed", d);
}

inline void snapshot(const Env &e, State &st, const Plan &tg, const Plan &cur) {
    for (unsigned i = 0; i < tg.n; i++) {
        st.snap[i] = {tg.s[i].reg, rd(e, tg.s[i].reg), tg.s[i].mask, tg.s[i].stage, tg.s[i].pad};
        st.tgt[i] = {tg.s[i].reg, tg.s[i].val, tg.s[i].mask, tg.s[i].stage, tg.s[i].pad};
    }
    st.nSnap = tg.n;
    st.sDpg[0] = rd(e, kDpgCtl); st.sDpg[1] = rd(e, kDpgR); st.sDpg[2] = rd(e, kDpgG); st.sDpg[3] = rd(e, kDpgB);
    st.sHubp = rd(e, kHubp0Cntl); st.sStream = rd(e, kStream); st.sLink = rd(e, kLinkCntl); st.sCfg = rd(e, kDpConfig);
    st.sVidM = rd(e, kVidM) & 0xffffff; st.sPclk = cur.info.pclkKhz;
}

// cmd 2 (force = false): set tg (same mode with every register equal = kSame, nothing written); cmd 3 (force): the round
// trip, the full shell with the values already there (OTG0 must be at tg with every plan register equal). A real mode
// change needs a passed round trip in this boot first.
inline int setMode(const Env &e, State &st, const Plan *const *pl, unsigned np, const Plan &tg, bool force) {
    st.warn[0] = 0;
    const Plan *cur = nullptr;
    const int r = survey(e, st, pl, np, tg, &cur);
    if (r != kDone) return r;
    const bool same = cur->info.hTotal == tg.info.hTotal && cur->info.vTotal == tg.info.vTotal && cur->info.hz == tg.info.hz;
    const bool allEq = st.equal == tg.n;
    if (!force && same && allEq) { st.same++; return setWhy(st, kSame, 0, "same mode", "every plan register already at the target value: nothing written"); }
    if (force && !(same && allEq)) return refuse(st, "round trip", "needs OTG0 at the target mode with every plan register equal (use 2 for a change)");
    if (!force && !same && !st.rtOk) return refuse(st, "mode change", "a round trip (3) must pass in this boot first");
    snapshot(e, st, tg, *cur);
    // b123: a same-timing pair re-plan of double-buffered frame / HUBP registers only = the live path (no blank)
    const bool live = e.live && !force && same && liveFits(e, st.tgt, tg.n);
    st.busy = 1; st.gen++;
    const char *w = nullptr;
    int fs = live ? liveShell(e, st, st.tgt, tg.n, true, &w) : shell(e, st, st.tgt, tg.n, false, true, &w);
    if (!fs && !verify(e, st, tg.info.hTotal, tg.info.vTotal, tg.info.hz, tg.info.pclkKhz, true, &w)) fs = kStVerify;
    char d[200];
    if (!fs) {
        st.busy = 0; st.done++; st.mode = static_cast<int>(tg.info.hz); st.pubHTotal = tg.info.hTotal; st.pubVTotal = tg.info.vTotal;
        st.live = tg.info.name;
        if (!force && same && &tg != cur) st.replans++;
        if (live) st.liveReplans++;
        if (force) st.rtOk = 1;
        snprintf(d, sizeof d, "%s -> %s%s: OTG0 %u frames/s, DP_VID_M 0x%x, %u writes, %u sticky clears, shell %u us", cur->info.name, tg.info.name,
                 live ? " (live, no blank)" : "", st.otg0Frames, st.vidM, st.writes, st.stickyClears, st.shellUs);
        return setWhy(st, kDone, 0, force ? "round trip done" : "done", d);
    }
    if (live) st.liveFails++;   // the revert below is the full shell with the snapshot, as for any other failed set
    snprintf(d, sizeof d, "%s%s: %s%s%s", live ? "live " : "", kStageName[fs], w ? w : "", st.warn[0] ? " | " : "", st.warn);
    const bool emerg = st.halted;   // the inverse fingerprint moved: revert our own registers without that check
    const char *w2 = nullptr;
    // a failure in stage 1 touched only the lock and DPG0: put DPG0 back; anything later: the whole shell with the snapshot
    int rs;
    if (fs == 1) { st.curStage = 9; st.curRevert = 1; rs = dpgSet(e, st, st.sDpg, true, !emerg, &w2) ? 0 : 9; st.curStage = 0; st.curRevert = 0; }
    else rs = shell(e, st, st.snap, st.nSnap, true, !emerg, &w2);
    if (!rs && !verify(e, st, cur->info.hTotal, cur->info.vTotal, cur->info.hz, cur->info.pclkKhz, !emerg, &w2)) rs = kStVerify;
    st.busy = 0;
    char d2[200];
    if (rs) {
        st.dark = 1; st.darkN++;
        snprintf(d2, sizeof d2, "%s; revert %s: %s", d, kStageName[rs], w2 ? w2 : "");
        return setWhy(st, kDark, fs, "DARK: no pipe-0 write until reboot (the GOP relights 1080p60 at boot)", d2);
    }
    st.fell++; st.mode = static_cast<int>(cur->info.hz); st.pubHTotal = cur->info.hTotal; st.pubVTotal = cur->info.vTotal;
    return setWhy(st, kFellBack, fs, st.halted ? "fell back, HALTED (pipe 1 / shared state moved during the switch)" : "fell back to the previous mode", d);
}

// The sysctl write: 1 survey, 2 set the target, 3 round trip.
inline int run(const Env &e, State &st, const Plan *const *pl, unsigned np, const Plan &tg, int cmd) {
    st.runs++; st.lastCmd = cmd; st.target = static_cast<int>(tg.info.hz); st.warn[0] = 0;
    if (cmd == kCmdSurvey) { const Plan *c = nullptr; return survey(e, st, pl, np, tg, &c); }
    if (cmd == kCmdSet || cmd == kCmdRoundTrip) return setMode(e, st, pl, np, tg, cmd == kCmdRoundTrip);
    return refuse(st, "commands", "1 survey, 2 set debug.phxpipe0_mode, 3 round trip at the current mode");
}

// ---- b101: the macOS path (PhxAMDFB head 0 setDisplayMode -> PhxBringup "phxPipe0SetMode", PhxSwitchABI.h; also
// debug.phxpipe0=4). One call does what the box steps T1 + T2
// (both passed) did by hand, in their order, while the old mode is still lit:
//   1 the gate and the latches: refused with zero register access, as every command
//   2 OTG0's mode by its totals (reads only); OTG0 already at the target's totals = kSame, nothing written (b103: unless
//     the target is a pair plan some register differs from = a pair re-plan, the same shell at the same timing)
//   3 stutter forced off when allowed: DCHUBBUB_ARB_DRAM_STATE_CNTL (v & ~3) | 2, the pipe-1 switch's own write (b92), 1 ms
//   4 the target's survey (reads only: the Acer-at-144 pair, DENTIST, DET0, stickies, link ...)
//   5 for a target above the current rate, the UCLK hard minimum (SMU SetHardMinByFreq PPCLK_UCLK = uclk.sh pin <MHz>;
//     uclkMhz 0 = not sent)
//   6 the round trip at the current mode when none passed in this boot (a mode change needs one)
//   7 the set
// Afterwards stutter is written back only when this call forced it and pipe 0 ends at 60 Hz (or at an unknown mode with
// nothing else written): at 144 it stays forced off (stutter at 144 + 144 is its own later test), on dark it stays off as
// after the pipe-1 switch. UCLK is never lowered here (b15_boot.sh aoc60 / a later UCLK test decides). out->rebase: a shell
// ended done or fell back, so the caller re-baselines the pipe-1 engine's pipe-0 fingerprint (phxp2::rebasePipe0) with
// st.pubHTotal / st.pubVTotal before it releases the display lock.
static constexpr uint32_t kSmuSetHardMinByFreq = 0x1b, kPpclkUclk = 2;
enum : uint32_t { kMacRoundTrip = 1, kMacStutterOff = 2, kMacUclkPin = 4 };   // = PhxSwitchABI.h PHX_P0SW_*
struct MacOut { int res, stage, rtRes; uint32_t curHz, flags; bool rebase; };
inline int macSet(const Env &e, State &st, const Plan *const *pl, unsigned np, const Plan &tg, uint32_t uclkMhz, MacOut *o) {
    *o = MacOut{kRefused, 0, -1, 0, 0, false};
    st.macRuns++;
    if (!e.enable || st.dark || st.halted) { o->res = run(e, st, pl, np, tg, kCmdSurvey); o->stage = st.lastStage; return o->res; }
    const Plan *cur = identify(e, pl, np);
    o->curHz = cur ? cur->info.hz : 0;
    if (cur && cur->info.hTotal == tg.info.hTotal && cur->info.vTotal == tg.info.vTotal && cur->info.hz == tg.info.hz &&
        (!tg.info.pairHTotal || equalCount(e, tg) == tg.n)) {
        st.runs++; st.lastCmd = kCmdSet; st.target = static_cast<int>(tg.info.hz); st.same++; st.mode = static_cast<int>(tg.info.hz);
        o->res = setWhy(st, kSame, 0, "same mode", "OTG0 already runs the target's timing: nothing written");
        return o->res;
    }
    st.macBusy = 1;
    const uint32_t arb = rd(e, kArb);
    const bool forced = (arb & 3) != 2;
    if (forced) { e.io.wr(e.io.c, kArb, (arb & ~3u) | 2u); st.writes++; st.arbForced++; e.io.delayUs(e.io.c, 1000); o->flags |= kMacStutterOff; }
    int r = run(e, st, pl, np, tg, kCmdSurvey);
    if (r == kDone && cur && tg.info.hz > cur->info.hz && uclkMhz) {
        char d[96];
        if (!e.smu) { r = refuse(st, "UCLK pin", "no SMU callback"); }
        else {
            st.uclkPins++;
            const uint32_t s = e.smu(e.io.c, kSmuSetHardMinByFreq, (kPpclkUclk << 16) | (uclkMhz & 0xffffu));
            if (s == 1) o->flags |= kMacUclkPin;
            else { snprintf(d, sizeof d, "SetHardMinByFreq UCLK %u MHz answered 0x%x", uclkMhz, s); r = refuse(st, "UCLK pin", d); }
        }
    }
    if (r == kDone && !st.rtOk) {
        o->flags |= kMacRoundTrip;
        r = o->rtRes = run(e, st, pl, np, *cur, kCmdRoundTrip);
        if (r == kDone || r == kFellBack) o->rebase = true;
    }
    bool set = false;
    if (r == kDone) { set = true; r = run(e, st, pl, np, tg, kCmdSet); if (r == kDone || r == kFellBack) o->rebase = true; }
    o->res = r; o->stage = st.lastStage;
    o->curHz = set && r == kDone ? tg.info.hz : cur ? cur->info.hz : 0;
    if (forced && r != kDark && o->curHz <= 60) { e.io.wr(e.io.c, kArb, arb); st.writes++; st.arbRestored++; o->flags &= ~kMacStutterOff; }
    st.macBusy = 0;
    return r;
}

// ---- b103: the pipe-1 switch with pipe 0 at 144. Each 144 plan is planned
// next to one Acer mode (1080p144 next to OTG1 at 144, 1080p144a60 next to OTG1 at 60, from Linux; they differ in
// VUPDATE width, VREADY, PREFETCH and MIN_TTU_VBLANK). The safe order keeps pipe 0 on the heavier pair's plan while pipe 1
// changes (values planned for more display bandwidth than runs, never fewer):
//   Acer 60 -> 144: pairPre = pipe 0 to 1080p144 (Env.otg1Next = the Acer's 144 totals), then the pipe-1 switch; anything
//                   but done / same = the caller refuses the pipe-1 switch (pipe 1 untouched)
//   Acer 144 -> 60: the pipe-1 switch first (pipe 0 keeps 1080p144 next to the lighter Acer), then pairPost
//   SCDC2, Acer -> 165 (from 144 or 60): no pairPre; the pipe-1 switch raises DISPCLK / DPPCLK (its DAL step) and lights
//                   165, then pairPost = pipe 0 to 1080p144a165 (its DENTIST max 0x2b holds only after those hard mins);
//                   the order Linux used in its capture (hard mins 201.3173, the Acer 201.359, pipe 0 201.3687-.3747)
//   SCDC2, Acer 165 -> 144 / 60: the switch first (pipe 0 keeps 1080p144a165 next to the lighter Acer), then pairPost
// pairPost after every switch that was tried (any result): pipe 0 to the plan for OTG1 as it runs then; refused / fell back
// = pipe 0 keeps the heavier pair's values. Pipe 0 at 60 (the 60 plan fits any OTG1), the gate off or a latch: kPairNothing
// with zero register access (the caller refuses a switch with pipe 0 at 144 and the gate off). Callers: phx_pipe0.cpp
// pairSwitch (b105; phx_pipe0.cpp p0PairSwitch from p2SwitchTo), under the display lock. *tg = the plan macSet ran for (null = none).
static constexpr uint32_t kOtg1H144 = 0x807, kOtg1V144 = 0x449;   // the Acer's 1080p144 (phx_s2_plan_1080p144.inc)
// SCDC2: the Acer's 1080p165 (2080 x 1110). The heaviest Acer mode: nothing is re-planned before a switch away from it
// (the switch first, pipe 0 keeps 1080p144a165, then pairPost) nor before a switch to it (Linux's order: clocks, Acer, pipe 0).
static constexpr uint32_t kOtg1H165 = 0x81f, kOtg1V165 = 0x455;
static constexpr int kPairNothing = -1;
inline int pairPre(const Env &e, State &st, const Plan *const *pl, unsigned np, uint32_t target1Hz, uint32_t uclkMhz, MacOut *o, const Plan **tg) {
    *o = MacOut{kRefused, 0, -1, 0, 0, false}; *tg = nullptr;
    if (!e.enable || st.dark || st.halted) return kPairNothing;
    const Plan *cur = identify(e, pl, np);
    if (!cur || cur->info.hz <= 60) return kPairNothing;
    if (target1Hz != 144 || (rd(e, 0x0506A) == kOtg1H144 && rd(e, 0x0506F) == kOtg1V144)) return kPairNothing;   // lighter: the switch first
    if (rd(e, 0x0506A) == kOtg1H165 && rd(e, 0x0506F) == kOtg1V165) return kPairNothing;                          // SCDC2: 165 -> 144 is lighter too
    Env en = e; en.otg1NextH = kOtg1H144; en.otg1NextV = kOtg1V144;
    *tg = pick(en, st, pl, np, cur->info.hz);
    return *tg ? macSet(en, st, pl, np, **tg, uclkMhz, o) : kPairNothing;
}
inline int pairPost(const Env &e, State &st, const Plan *const *pl, unsigned np, uint32_t uclkMhz, MacOut *o, const Plan **tg) {
    *o = MacOut{kRefused, 0, -1, 0, 0, false}; *tg = nullptr;
    if (!e.enable || st.dark || st.halted) return kPairNothing;
    const Plan *cur = identify(e, pl, np);
    if (!cur || cur->info.hz <= 60) return kPairNothing;
    const Plan *t = pick(e, st, pl, np, cur->info.hz);
    if (!t || !pairFits(e, *t) || equalCount(e, *t) == t->n) return kPairNothing;   // OTG1 at no planned mode, or already there
    *tg = t;
    return macSet(e, st, pl, np, *t, uclkMhz, o);
}

// b105: does the plan pipe 0 runs fit OTG1 (as it runs, or Env.otg1Next*)? 1 yes (or pipe 0 at a pair-free plan), 0 no
// (an unplanned pair: e.g. 1080p144 next to the Acer at 60 after a post-step that fell back), -1 unknown (gate off / a
// latch: no register read; OTG0 at no plan, or at a pair timing without every register of one plan).
inline int pairNow(const Env &e, const State &st, const Plan *const *pl, unsigned np) {
    if (!e.enable || st.dark || st.halted) return -1;
    const Plan *cur = identify(e, pl, np);
    if (!cur) return -1;
    if (!cur->info.pairHTotal) return 1;
    if (equalCount(e, *cur) != cur->n) return -1;
    return pairFits(e, *cur) ? 1 : 0;
}
// b105: the whole pipe-1 (Acer) switch with the pipe-0 steps of the safe order (phx_pipe2.cpp p2SwitchTo calls it through
// phx_pipe0.cpp p0PairSwitch for every real mode change, under the display lock). PairIo.sw runs the pipe-1 switch to hz
// and returns its phxp2::SwRes; PairIo.rebase re-baselines the pipe-1 engine's pipe-0 fingerprint after a pipe-0 shell
// (done or fell back: MacOut.rebase), before the next pipe-1 step reads it; PairIo.log (may be null) logs a pipe-0 step.
//   1 pairPre (the heavier pair's plan first): anything but nothing / done / same = the switch refused (pipe 1 untouched),
//     then pairPost (pipe 0 back to the plan for OTG1 as it runs)
//   2 the pipe-1 switch, 3 pairPost (pipe 0 follows OTG1, any switch result)
//   4 pipe 0 not on a plan for OTG1 as it runs now (pairNow 0: the post-step refused / fell back) after a done switch:
//     pipe 1 switched back to its previous mode when pipe 0's plan was made for it, so the pair is a planned one again
//     (the post-step's symmetric twin of step 1's refusal; b104 reported done and left 1080p144 next to the Acer at 60).
//     The result is then "fell back" (pipe 1 at its previous mode; PhxAccel measures the mode it runs), "dark" when the
//     switch back went dark. No retry: a pipe-0 shell that fell back already blanked the AOC twice.
//   Any other unplanned end (the switch itself fell back and the post-step failed; pipe 0's plan fits neither mode) keeps
//   the switch's result with the pipe-0 state in why.
// o->why = the pipe-0 part of the reason ("" = the pipe-1 switch's own why stands).
struct PairIo {
    void *ctx;
    int (*sw)(void *ctx, uint32_t hz);
    void (*rebase)(void *ctx);
    void (*log)(void *ctx, const char *when, int res, const Plan *tg, const MacOut &o);
};
struct PairSw { int res, preRes, postRes, backRes, after; bool ran, rolledBack; const Plan *preTg, *postTg; char why[160]; };
inline int pairSwitch(const Env &e, State &st, const Plan *const *pl, unsigned np, uint32_t from1Hz, uint32_t to1Hz, uint32_t uclkMhz,
                      const PairIo &io, PairSw *o) {
    *o = PairSw{phxp2::kSwRefused, kPairNothing, kPairNothing, -1, -1, false, false, nullptr, nullptr, {0}};
    const bool armed = e.enable && !st.dark && !st.halted;
    const uint32_t h0 = armed ? rd(e, 0x0506A) : 0, v0 = armed ? rd(e, 0x0506F) : 0;   // OTG1 before the switch
    MacOut mo{};
    o->preRes = pairPre(e, st, pl, np, to1Hz, uclkMhz, &mo, &o->preTg);
    if (mo.rebase && io.rebase) io.rebase(io.ctx);
    if (o->preRes != kPairNothing && io.log) io.log(io.ctx, "before the Acer switch", o->preRes, o->preTg, mo);
    if (o->preRes != kPairNothing && o->preRes != kDone && o->preRes != kSame) {
        snprintf(o->why, sizeof o->why, "pipe 0: to %s first: %s: %s", o->preTg ? o->preTg->info.name : "?", kResName[o->preRes], st.why);
        MacOut po{};
        o->postRes = pairPost(e, st, pl, np, uclkMhz, &po, &o->postTg);   // a fell-back / partial pipe-0 step: back to the plan for OTG1
        if (po.rebase && io.rebase) io.rebase(io.ctx);
        if (o->postRes != kPairNothing && io.log) io.log(io.ctx, "after the refused Acer switch", o->postRes, o->postTg, po);
        return o->res = phxp2::kSwRefused;
    }
    o->ran = true;
    const int r = io.sw(io.ctx, to1Hz);
    o->res = r;
    MacOut po{};
    o->postRes = pairPost(e, st, pl, np, uclkMhz, &po, &o->postTg);
    if (po.rebase && io.rebase) io.rebase(io.ctx);
    if (o->postRes != kPairNothing && io.log) io.log(io.ctx, "after the Acer switch", o->postRes, o->postTg, po);
    o->after = pairNow(e, st, pl, np);
    if (o->after != 0) return r;
    st.pairPostFails++;
    const Plan *cur = identify(e, pl, np);
    const char *cn = cur ? cur->info.name : "?";
    const uint32_t h1 = rd(e, 0x0506A), v1 = rd(e, 0x0506F);
    char d[96];
    snprintf(d, sizeof d, "re-plan to %s %s", o->postTg ? o->postTg->info.name : "?", o->postRes >= 0 ? kResName[o->postRes] : "not run");
    Env eb = e; eb.otg1NextH = h0; eb.otg1NextV = v0;
    if (r != phxp2::kSwDone || !h0 || pairNow(eb, st, pl, np) != 1) {
        snprintf(o->why, sizeof o->why, "pipe 0 on %s next to OTG1 0x%x/0x%x (unplanned pair): %s", cn, h1, v1, d);
        return r == phxp2::kSwDone ? (o->res = phxp2::kSwFellBack) : r;
    }
    o->rolledBack = true; st.pairRollbacks++;
    o->backRes = io.sw(io.ctx, from1Hz);
    const int now = pairNow(e, st, pl, np);
    snprintf(o->why, sizeof o->why, "pipe 0: %s (stays on %s): pipe 1 switched back to %u Hz: %s%s", d, cn, from1Hz,
             o->backRes >= 0 && o->backRes <= phxp2::kSwDark ? phxp2::kSwName[o->backRes] : "?", now == 1 ? "" : " (pair still unplanned)");
    return o->res = o->backRes == phxp2::kSwDark ? phxp2::kSwDark : phxp2::kSwFellBack;
}

#define PHXP0_OUT(...) do { if (o < n - 1) { const int k_ = snprintf(b + o, static_cast<size_t>(n - o), __VA_ARGS__); o += k_ > 0 ? k_ : 0; if (o > n - 1) o = n - 1; } } while (0)
// debug.phxpipe0_stat: no register access (the b15 watchdog reads it while a switch runs). Starts with busy= gen= like
// debug.phxpipe2_switch_stat (b15_boot.sh sw() parses both).
inline int stat(const State &st, int enable, int modeSel, char *b, int n) {
    int o = 0;
    const int lr = st.lastRes >= 0 && st.lastRes <= kDark ? st.lastRes : kRefused;
    PHXP0_OUT("busy=%d gen=%u enable=%d mode=%d target=%d select=%d last=%s cmd=%d stage=%s dark=%d halted=%d rt_ok=%d runs=%u surveys=%u done=%u "
              "same=%u refused=%u fell=%u darkn=%u writes=%u rb_mismatch=%u refused_writes=%u sticky_clears=%u lock_acks=%u equal=%u/%u "
              "frames=%u vid_m=0x%x shell_us=%u mac=%u uclk_pins=%u arb_forced=%u arb_restored=%u plan=%s replans=%u live_replans=%u live_fails=%u live_writes=%u "
              "pair_post_fails=%u pair_rollbacks=%u stream_wait_us=%u stream_wait_max_us=%u stream_fails=%u dig_fifo_resets=%u "
              "dig_fifo_err=%u dig_fifo_pre=0x%08x dig_fifo_post=0x%08x stream_fail=0x%x/0x%x/0x%x/0x%x/0x%x refused_write=",
              st.busy || st.macBusy ? 1 : 0, st.gen, enable, st.mode, st.target, modeSel, st.runs ? kResName[lr] : "none", st.lastCmd,
              kStageName[st.lastStage >= 0 && st.lastStage <= kStVerify ? st.lastStage : 0], st.dark, st.halted, st.rtOk, st.runs, st.surveys,
              st.done, st.same, st.refusedN, st.fell, st.darkN, st.writes, st.rbMismatch, st.refusedWrites, st.stickyClears, st.lockAcks,
              st.equal, st.nPlan, st.otg0Frames, st.vidM, st.shellUs, st.macRuns, st.uclkPins, st.arbForced, st.arbRestored,
              st.live ? st.live : "?", st.replans, st.liveReplans, st.liveFails, st.liveWrites, st.pairPostFails, st.pairRollbacks, st.streamWaitUs, st.streamWaitMaxUs, st.streamFails,
              st.fifoResets, st.fifoErr, st.fifoPre, st.fifoPost, st.failRegs[0], st.failRegs[1], st.failRegs[2], st.failRegs[3], st.failRegs[4]);
    // b105: the last refused write, its register, mask and the shell stage it was in
    if (st.refReg) PHXP0_OUT("0x%05X/0x%08x@%s%s", st.refReg, st.refMask, kStageName[st.refStage >= 0 && st.refStage <= kStVerify ? st.refStage : 0], st.refRevert ? "(revert)" : "");
    else PHXP0_OUT("none");
    PHXP0_OUT(" why=%s%s%s", st.why, st.warn[0] ? " warn=" : "", st.warn);
    return o;
}
// debug.phxpipe0_plan (root, reads only): every entry of the plans with its register now.
inline int planDump(const Env &e, const Plan *const *pl, unsigned np, char *b, int n) {
    int o = 0;
    for (unsigned k = 0; k < np; k++) {
        const Plan &p = *pl[k];
        PHXP0_OUT("plan %s: %u Hz totals 0x%x/0x%x pclk %u kHz ready %u todo %u pair 0x%x/0x%x\n", p.info.name, p.info.hz, p.info.hTotal, p.info.vTotal,
                  p.info.pclkKhz, p.info.ready, p.info.todo, p.info.pairHTotal, p.info.pairVTotal);
        for (unsigned i = 0; i < p.n; i++) {
            const uint32_t v = rd(e, p.s[i].reg);
            PHXP0_OUT("  g%u 0x%05X now 0x%08x -> 0x%08x mask 0x%08x %s%s %s\n", p.s[i].stage, p.s[i].reg, v, p.s[i].val, p.s[i].mask,
                      (v ^ p.s[i].val) & p.s[i].mask ? "DIFF" : "same", p.s[i].pad & kFTodo ? " TODO" : "", p.s[i].name);
        }
    }
    return o;
}

}  // namespace phxp0
