// SPDX-License-Identifier: MIT
// PhxDpStream.h: kext b117, multiport S2c: a DP picture on DP E or DP A from
// our kext, on a pipe nobody uses: OTG2 + OPP2 with the DPG solid colour (magenta) -> DIG4 (DP E) or DIG0 (DP A) FE -> BE ->
// PHY, no HUBP / DPP / plane / watermark / DET / clock change. Pipe 0 (the GOP's OTG0) and pipe 1 (our OTG1 / DIG3, the
// Acer) are never written: both fingerprints after every MMIO write and DMUB command, as in PhxDpRun.h (debug.phxdplink).
//
// Source of every step: the Linux v6.8 plug traces of the AOC on DP E / DP A (11-01), the 1080p60
// modeset Linux ran on OTG0:
//   OTG side = the DP E trace's window, cut into the host test's golden (Trace A's window is identical):
//     OTG_PIXEL_RATE_DIV OTGn field = 2 (K1 /1, K2 /2), ODM / OTG clock on (+ Linux's REG_WAITs on CLOCK_ON), DP_DTOn
//     = pclk / 2 over DPREFCLK, OTGn_PIXEL_RATE_CNTL DTO on, the timing, VTG, OTG_CONTROL (START_POINT_CNTL 1 for DP),
//     OPP pipe clock, DPG pattern, ODM source = OPPn, VTG + OTG master enable;
//   DIG/DP side = PhxDpLink.h enableStream (kDpE_enable / kDpA_enable, host-tested) with the FE fed by OTG2.
// Deliberate deviations from the trace (each checked by the host test): instance 2 instead of 0;
// no FAMS_DRR_UPDATE DMUB command (Linux's own firmware feature; the GOP build gets only TRANSMITTER_CONTROL);
// no FMT writes (the pipe-1 DPG stream never wrote FMT1 and showed its colour on the box); the RMW no-op
// VTG write of the previous mode's value dropped; OTG_H_TIMING_CNTL / OPP_PIPE_CONTROL written with their fields only
// (Linux's RMW carried read-only status bits); DPG colour magenta instead of black.
//
// Selector (root writes debug.phxdpstream): 0x10 + n = survey (reads + the DPCD caps read only), 0x20 + n = stream,
// 0x30 + n = undo, b119: 0x40 + n = desktop (S2d: DIG n fed from the GOP's OTG0, i.e. head 0's own desktop on that
// connector; no OTG / HUBP / clock write at all), 0x50 + n = its survey; n = 0 (DP A) or 4 (DP E). Anything else is refused.
// Refusal = zero writes. b119: a survey (0x1n / 0x5n) while something is applied reports the lit status (reads only,
// nothing latches) instead of refusing.
// Ladder: stream needs the LAST debug.phxdplink run of this boot to be a PASS on the same connector (that run loaded the
// PHY's C3 SRAM and proved the link), every S2b gate (PhxDpRun.h gates, on a copy), pipe 2 dark, DPREFCLK as assumed.
// Any non-PASS stream latches `failed` (no stream until reboot; undo allowed). Halted (pipe 0 / 1 changed) = nothing more,
// not even undo. Undo = Linux's DP unplug order (stream off, link off + D3 + DISABLE, BE source off, ODM source off,
// OTG master off, VTG off, wait OTG busy clear) then every pipe-2 register back to its survey value (reverse order,
// OTG_PIXEL_RATE_DIV only OTG2's field), DIGn FE back to its survey value.
// Kernel-free; glue phx_dpstream.cpp; host-tested (with mutations).
#pragma once
#include <stdint.h>
#include "PhxDpRun.h"

namespace phxdps {

using phxp2::Env;
using phxp2::State;
using phxp2::rd;

constexpr unsigned kOtg = 2;                     // the only OTG / OPP this file drives
constexpr uint32_t kPixelRateDiv = 0x0012F;      // OTG_PIXEL_RATE_DIV (shared: OTG0..3 K1/K2 fields)
constexpr uint32_t kMpccOppId0 = 0x09002, kMpccStride = 0x15;   // MPCCn_MPCC_OPP_ID (n = 0..3)
constexpr uint32_t kDto0Modulo = 0x00142;        // DP_DTO0_MODULO (the GOP's pipe-0 DTO: DPREFCLK check)

// ---- pipe n registers (absolute dwords; dcn_3_2_0_offset.h; every name checked per instance by the host test) ----
struct PipeRegs {
    uint32_t pixRate, dtoPhase, dtoModulo, odmInClk, otgClk, hTotal, hSyncA, hBlank, hSyncCntl, vTotal, vSyncA, vBlank,
             vSyncCntl, vtg, otgCtl, vstartup, vupdate, vready, odmFmt, hTimingCntl, oppPipe, dpgDim, dpgOffset, dpgB, dpgG,
             dpgR, dpgCtl, odmSrc, frame, odmGlobal, mpcOutMux, hubpCntl;
};
constexpr PipeRegs pipeRegs(unsigned n) {
    return PipeRegs{0x00140 + 4 * n, 0x00141 + 4 * n, 0x00142 + 4 * n, 0x04F8F + 0x10 * n, 0x05045 + 0x80 * n,
                    0x04FEA + 0x80 * n, 0x04FEC + 0x80 * n, 0x04FEB + 0x80 * n, 0x04FED + 0x80 * n, 0x04FEF + 0x80 * n,
                    0x04FF7 + 0x80 * n, 0x04FF6 + 0x80 * n, 0x04FF8 + 0x80 * n, 0x039EE + n, 0x05001 + 0x80 * n,
                    0x05046 + 0x80 * n, 0x05047 + 0x80 * n, 0x05048 + 0x80 * n, 0x04F8C + 0x10 * n, 0x04FEE + 0x80 * n,
                    0x04D4C + 0x5A * n, 0x04D16 + 0x5A * n, 0x04D1A + 0x5A * n, 0x04D19 + 0x5A * n, 0x04D18 + 0x5A * n,
                    0x04D17 + 0x5A * n, 0x04D14 + 0x5A * n, 0x04F8B + 0x10 * n, 0x0500C + 0x80 * n, 0x04F8A + 0x10 * n,
                    0x093D8 + 4 * n, 0x03AB3 + 0xDC * n};
}
static_assert(pipeRegs(0).hTotal == phxp2::kOtg0HTotal && pipeRegs(0).otgCtl == phxp2::kOtg0Control &&
              pipeRegs(0).frame == phxp2::kOtg0Frame && pipeRegs(0).odmGlobal == phxp2::kOdm0Global &&
              pipeRegs(0).hubpCntl == phxp2::kHubp0Cntl, "instance 0 = PhxPipe2.h's pipe-0 names");
static_assert(pipeRegs(1).otgClk == 0x050C5 && pipeRegs(1).oppPipe == 0x04DA6 && pipeRegs(1).dpgCtl == 0x04D6E &&
              pipeRegs(1).vtg == 0x039EF && pipeRegs(1).odmSrc == 0x04F9B && pipeRegs(1).pixRate == phxp2::kOtg1PixelRate &&
              pipeRegs(1).hubpCntl == phxp2::kHubp1Cntl && pipeRegs(1).frame == phxp2::kOtg1Frame,
              "instance 1 = the pipe-1 plan's addresses (phx_s2_plan_1080p60.inc)");
static_assert(pipeRegs(2).otgCtl == 0x05101 && pipeRegs(2).hTotal == 0x050EA && pipeRegs(2).dpgCtl == 0x04DC8 &&
              pipeRegs(2).oppPipe == 0x04E00 && pipeRegs(2).dtoPhase == 0x00149 && pipeRegs(2).vtg == 0x039F0 &&
              pipeRegs(2).hubpCntl == 0x03C6B && pipeRegs(2).mpcOutMux == 0x093E0, "instance 2 (offset.h)");
inline constexpr uint32_t divMask(unsigned n) { return 0x7u << (3 * n); }                  // OTGn K1 (1 bit) + K2 (2 bits)
inline constexpr uint32_t divDp(unsigned n) { return 0x2u << (3 * n); }                    // K1 /1, K2 /2 (trace 0xdb6 -> 0xdb2)
static_assert(divMask(2) == phxdpl::pixelRateDivMask(2) && divDp(2) == phxdpl::pixelRateDivDp(2), "= PhxDpLink.h");

// The registers the stream may write on pipe n (OTG_PIXEL_RATE_DIV is not here: only divWrite, OTGn's field only).
inline bool pipeWriteAllowed(unsigned n, uint32_t r) {
    if (n != kOtg) return false;
    const PipeRegs g = pipeRegs(n);
    const uint32_t ok[] = {g.pixRate, g.dtoPhase, g.dtoModulo, g.odmInClk, g.otgClk, g.hTotal, g.hSyncA, g.hBlank, g.hSyncCntl,
                           g.vTotal, g.vSyncA, g.vBlank, g.vSyncCntl, g.vtg, g.otgCtl, g.vstartup, g.vupdate, g.vready,
                           g.odmFmt, g.hTimingCntl, g.oppPipe, g.dpgDim, g.dpgOffset, g.dpgB, g.dpgG, g.dpgR, g.dpgCtl, g.odmSrc};
    for (uint32_t a : ok) if (a == r) return true;
    return false;
}

// ---- field values ------------------------------------------------------------------------------------------------------
constexpr uint32_t kClkOn = 0x3u;                       // ODMn_OPTC_INPUT_CLOCK_CONTROL / OTGn_OTG_CLOCK_CONTROL: CLK_EN | GATE_DIS
constexpr uint32_t kOdmInClkOn = 0x4u, kOtgClkOn = 0x100u, kOtgBusy = 0x10000u;   // CLK_ON status bits, OTG_BUSY
constexpr uint32_t kOtgCtlDp = 0x1200u;                 // OTG_DISABLE_POINT_CNTL 2 | OTG_START_POINT_CNTL 1 (DP; HDMI 0x200)
constexpr uint32_t kOtgMasterEn = 0x1u, kOtgMasterState = 0x10000u;
constexpr uint32_t kVtgEnable = 0x80000000u;
constexpr uint32_t kOppPipeClockEn = 0x1u, kOppPipeClockOn = 0x2u;
constexpr uint32_t kDpgEnHBars = 0x41u;                  // DPG_EN | DPG_MODE 4 (horizontal bars, both colours equal = solid)
constexpr uint32_t kDpgMagentaBR = 0xffc0ffc0u;          // the bring-up marker colour of the pipe-1 plan before b94 (B and R)
constexpr uint32_t kOdmSrcNone = 0xffff0000u;            // every segment source 0xf
inline constexpr uint32_t odmSrc(unsigned opp) { return 0xfff00000u | (opp << 16); }   // one segment, seg0 = OPPn
constexpr uint32_t kFifoErr = 0x30000000u;               // DIGn_DIG_FIFO_CTRL0 DIG_FIFO_ERROR
constexpr uint32_t kDioFifoErr = 0x4000u;                // OTGn_PIXEL_RATE_CNTL OTGn_DIO_FIFO_ERROR
constexpr uint32_t kDprefLoHz = 716000000u, kDprefHiHz = 717400000u;   // DP_DTO0_MODULO window (the GOP's DPREFCLK)

// The only timing: CEA 1080p60 at 148.5 MHz, the mode Linux lit the AOC with on DP E / DP A at RBR x4 (Trace E / A).
// VSTARTUP / VUPDATE / VREADY are Linux DML's values for it on OTG0 [F trace]; VTG FP2 and VCOUNT_INIT follow from them.
struct OtgTiming { uint32_t vstartup, vupdate, vready; };
constexpr OtgTiming kOtg1080p60 = {0x2c, 0x006a0226, 0x95};
inline bool timingSupported(const phxdpl::Timing &t) {
    const phxdpl::Timing &c = phxdpl::kCea1080p60;
    return t.hTotal == c.hTotal && t.hActive == c.hActive && t.hFront == c.hFront && t.hSync == c.hSync &&
           t.vTotal == c.vTotal && t.vActive == c.vActive && t.vFront == c.vFront && t.vSync == c.vSync &&
           t.pclkKhz == c.pclkKhz && t.hNeg == c.hNeg && t.vNeg == c.vNeg;
}

// ---- the step table -----------------------------------------------------------------------------------------------------
enum : uint8_t { kW = 1, kDiv = 2, kPoll = 3 };
struct Step { uint8_t op; uint32_t reg, val, mask, budgetUs; const char *name; };   // name: printf template, %u = n
static constexpr unsigned kMaxSteps = 40;
struct Colour { uint32_t b, g, r; };
constexpr Colour kMagenta = {kDpgMagentaBR, 0, kDpgMagentaBR};
// Linux optc1_program_timing / dcn20_enable_stream_timing order (the golden): K1/K2, clocks (+ waits), DTO, timing, VTG,
// OTG_CONTROL, VSTARTUP.., VTG, data format, H timing, OPP clock, DPG, ODM source, VTG + OTG enable.
inline unsigned buildSteps(unsigned n, const phxdpl::Timing &t, const OtgTiming &o, const Colour &c, Step *s) {
    const PipeRegs g = pipeRegs(n);
    const uint32_t hBlankStart = t.hTotal - t.hFront, hBlankEnd = hBlankStart - t.hActive;
    const uint32_t vBlankStart = t.vTotal - t.vFront, vBlankEnd = vBlankStart - t.vActive;
    const int32_t lineStart = static_cast<int32_t>(vBlankEnd) - static_cast<int32_t>(o.vstartup) + 1;
    const uint32_t fp2 = lineStart < 0 ? static_cast<uint32_t>(-lineStart) : 0;    // optc1_program_timing VTG0_FP2
    const uint32_t vtg = (vBlankStart << 16) | fp2;                                 // VTG_VCOUNT_INIT = v blank start
    unsigned k = 0;
    auto W = [&](uint32_t reg, uint32_t val, uint32_t mask, const char *name) { s[k++] = Step{kW, reg, val, mask, 0, name}; };
    auto P = [&](uint32_t reg, uint32_t bit, uint32_t us, const char *name) { s[k++] = Step{kPoll, reg, bit, bit, us, name}; };
    s[k++] = Step{kDiv, kPixelRateDiv, divDp(n), divMask(n), 0, "OTG_PIXEL_RATE_DIV(OTG%u K1/K2)"};
    W(g.odmInClk, kClkOn, 0x3, "ODM%u_OPTC_INPUT_CLOCK_CONTROL");
    P(g.odmInClk, kOdmInClkOn, 1000, "ODM%u_OPTC_INPUT_CLOCK_CONTROL(CLK_ON)");
    W(g.otgClk, kClkOn, 0x13, "OTG%u_OTG_CLOCK_CONTROL");
    P(g.otgClk, kOtgClkOn, 1000, "OTG%u_OTG_CLOCK_CONTROL(CLOCK_ON)");
    W(g.dtoPhase, phxdpl::dtoPhase(t.pclkKhz), 0xffffffffu, "DP_DTO%u_PHASE");
    W(g.dtoModulo, phxdpl::kDpRefClkHz, 0xffffffffu, "DP_DTO%u_MODULO");
    W(g.pixRate, phxdpl::kOtgPixelRateCntlDto, 0x333b, "OTG%u_PIXEL_RATE_CNTL");
    W(g.hTotal, t.hTotal - 1u, 0x7fff, "OTG%u_OTG_H_TOTAL");
    W(g.hSyncA, static_cast<uint32_t>(t.hSync) << 16, 0x7fff7fff, "OTG%u_OTG_H_SYNC_A");
    W(g.hBlank, (hBlankEnd << 16) | hBlankStart, 0x7fff7fff, "OTG%u_OTG_H_BLANK_START_END");
    W(g.hSyncCntl, t.hNeg ? 1u : 0u, 0x00030001, "OTG%u_OTG_H_SYNC_A_CNTL");
    W(g.vTotal, t.vTotal - 1u, 0x7fff, "OTG%u_OTG_V_TOTAL");
    W(g.vSyncA, static_cast<uint32_t>(t.vSync) << 16, 0x7fff7fff, "OTG%u_OTG_V_SYNC_A");
    W(g.vBlank, (vBlankEnd << 16) | vBlankStart, 0x7fff7fff, "OTG%u_OTG_V_BLANK_START_END");
    W(g.vSyncCntl, t.vNeg ? 1u : 0u, 0x101, "OTG%u_OTG_V_SYNC_A_CNTL");
    W(g.otgCtl, kOtgCtlDp, 0x00307301, "OTG%u_OTG_CONTROL");
    W(g.vstartup, o.vstartup, 0x3ff, "OTG%u_OTG_VSTARTUP_PARAM");
    W(g.vupdate, o.vupdate, 0x03ffffff, "OTG%u_OTG_VUPDATE_PARAM");
    W(g.vready, o.vready, 0xffff, "OTG%u_OTG_VREADY_PARAM");
    W(g.vtg, vtg, 0xffff7fff, "VTG%u_CONTROL");
    W(g.odmFmt, 0, 0x33, "ODM%u_OPTC_DATA_FORMAT_CONTROL");
    W(g.hTimingCntl, 0, 0x103, "OTG%u_OTG_H_TIMING_CNTL");
    W(g.oppPipe, kOppPipeClockEn, 0x11, "OPP_PIPE%u_OPP_PIPE_CONTROL");
    W(g.dpgDim, (static_cast<uint32_t>(t.hActive) << 16) | t.vActive, 0x3fff3fff, "DPG%u_DPG_DIMENSIONS");
    W(g.dpgOffset, 0, 0x3fff3fff, "DPG%u_DPG_OFFSET_SEGMENT");
    W(g.dpgB, c.b, 0xffffffffu, "DPG%u_DPG_COLOUR_B_CB");
    W(g.dpgG, c.g, 0xffffffffu, "DPG%u_DPG_COLOUR_G_Y");
    W(g.dpgR, c.r, 0xffffffffu, "DPG%u_DPG_COLOUR_R_CR");
    W(g.dpgCtl, kDpgEnHBars, 0x01ff3171, "DPG%u_DPG_CONTROL");
    W(g.dpgDim, (static_cast<uint32_t>(t.hActive) << 16) | t.vActive, 0x3fff3fff, "DPG%u_DPG_DIMENSIONS");
    W(g.odmSrc, odmSrc(n), 0xffff0303, "ODM%u_OPTC_DATA_SOURCE_SELECT");
    W(g.vtg, vtg | kVtgEnable, 0xffff7fff, "VTG%u_CONTROL");
    W(g.otgCtl, kOtgCtlDp | kOtgMasterEn, 0x00307301, "OTG%u_OTG_CONTROL");
    return k;
}

// ---- state ----------------------------------------------------------------------------------------------------------------
enum Res { kNone = 0, kRefused, kSurvey, kPass, kFailed, kTimeout, kHalted, kPostFail, kUndone, kUndoFail };
static const char *const kResName[] = {"none", "refused", "survey-ok", "PASS", "FAILED", "timeout", "HALTED", "POST-FAIL",
                                       "UNDONE", "UNDO-FAIL"};
enum Act { kActSurvey = 1, kActStream = 2, kActUndo = 3, kActDesk = 4, kActDeskSurvey = 5 };
static constexpr uint32_t kStatusWaitUs = 100000, kMeasureUs = 500000, kBusyWaitUs = 100000;

struct Stream {
    // boot state
    int failed;                // a stream run ended in anything but PASS: no stream run until reboot (undo allowed)
    int dig;                   // connector of the applied state (-1 = none)
    unsigned touched;          // pipe-2 steps [0, touched) may have changed the hardware
    int linkTouched;           // enableStream started (DIG FE/BE, DPn, PHY, sink may be on)
    int lit;                   // the last stream run passed, no undo since
    uint32_t runs, passes, refusals, undos;
    // the last run
    int sel, act, res, phase;  // phase: 0 gates, 1 pipe-2 steps, 2 link, 3 post, 4 undo-link, 5 undo-otg, 6 undo-restore
    const char *why, *lastRefusal;
    phxdpr::Run b;             // the S2b gates' baseline (fingerprints, OTG1 state, caps, link) + the run's io counters
    Step steps[kMaxSteps]; unsigned n;
    uint32_t before[kMaxSteps];   // each W / kDiv step's register just before its write
    int failStep;
    // pipe-2 survey (reads)
    uint32_t sOtgCtl, sOtgClk, sOdmIn, sOpp, sDpg, sPix, sDiv, sMpcOut, sHubp, sDto0Mod, sFeCntl;
    // stream
    int erc; phxdpl::Report rep;
    uint32_t pVid, pFifo, pPix, pOdm, pFe, pBeEn, pDiv, statusUs, hz0, hz1, hz2;
    uint8_t after[6], power; int auxRc;
    // undo
    int urc; unsigned restored;
    uint32_t uOtgCtl, uOtgClk, uBeEn, uLink, uVid, uDiv, uFe;
    // b119 S2d (DIG n fed from the GOP's OTG0) and the read-only lit status
    int kind;                  // what is applied: 0 none, kActStream (S2c, pipe 2), kActDesk (S2d, OTG0)
    int onePpc;                // the applied DIG side's pixel mode (S2d follows OTG0's: the GOP's = 1)
    phxdpl::Timing t0;         // OTG0's timing as read (S2d)
    uint32_t p0Pix, p0Phase, p0Mod, p0Hz, p0PclkKhz;   // OTG0_PIXEL_RATE_CNTL, DP_DTO0 phase / modulo, mode rate, nominal pclk
    int lsPipe0, lsPipe1;      // lit status: pipe 0 / pipe 1 still equal to the stream-time baseline (reported, never latched)
};

inline int refuse(Stream &s, const char *why) { s.res = kRefused; s.why = s.lastRefusal = why; s.refusals++; return kRefused; }
inline int endRun(Stream &s, int res, const char *why) {
    s.res = res; s.why = why;
    if (res == kPass) { s.passes++; s.lit = 1; }
    else if (res != kSurvey && res != kUndone) s.failed = 1;
    return res;
}

// One MMIO write of this file's own (pipe-2 list, or OTG_PIXEL_RATE_DIV with only OTGn's bits changed), then the guard.
inline bool pWr(const Env &e, State &st, Stream &s, uint32_t reg, uint32_t v) {
    if (st.halted) { s.b.blocked++; return false; }
    const bool div = reg == kPixelRateDiv && !((v ^ rd(e, kPixelRateDiv)) & ~divMask(kOtg));
    if (!div && !pipeWriteAllowed(kOtg, reg)) { s.b.strayWr++; return false; }
    e.io.wr(e.io.c, reg, v);
    s.b.mmioWr++;
    return phxdpr::guard(e, st, s.b);
}
inline bool otgFrames(const Env &e) { return phxp2::frameMoves(e, pipeRegs(kOtg).frame); }

// Pipe 2 dark and free (reads only).
inline const char *pipe2Gates(const Env &e, Stream &s, const phxdpl::Conn &k) {
    const PipeRegs g = pipeRegs(kOtg);
    s.sOtgCtl = rd(e, g.otgCtl); s.sOtgClk = rd(e, g.otgClk); s.sOdmIn = rd(e, g.odmInClk); s.sOpp = rd(e, g.oppPipe);
    s.sDpg = rd(e, g.dpgCtl); s.sPix = rd(e, g.pixRate); s.sDiv = rd(e, kPixelRateDiv); s.sMpcOut = rd(e, g.mpcOutMux);
    s.sHubp = rd(e, g.hubpCntl); s.sDto0Mod = rd(e, kDto0Modulo); s.sFeCntl = rd(e, phxdpl::regs(k.dig).feCntl);
    if (s.sOtgCtl & (kOtgMasterEn | kOtgMasterState)) return "pipe 2 busy: OTG2_OTG_CONTROL master enabled";
    if (s.sOtgClk & 1u) return "pipe 2 busy: OTG2_OTG_CLOCK_CONTROL OTG_CLOCK_EN set";
    if (s.sOdmIn & 2u) return "pipe 2 busy: ODM2_OPTC_INPUT_CLOCK_CONTROL OPTC_INPUT_CLK_EN set";
    if (s.sOpp & kOppPipeClockEn) return "pipe 2 busy: OPP_PIPE2 clock enabled";
    if (s.sDpg & 1u) return "pipe 2 busy: DPG2 enabled";
    if (s.sPix & 0x18u) return "pipe 2 busy: OTG2_PIXEL_RATE_CNTL DP / DTB DTO enabled";
    if ((s.sMpcOut & 0xfu) != 0xfu) return "pipe 2 busy: MPC_OUT2_MUX connected to an MPCC";
    for (unsigned m = 0; m < 4; m++) if ((rd(e, kMpccOppId0 + kMpccStride * m) & 0xfu) == kOtg) return "pipe 2 busy: an MPCC feeds OPP2";
    for (unsigned d = 0; d <= 4; d++) {
        if (d == k.dig) continue;
        const phxdpl::Regs o = phxdpl::regs(d);
        if ((rd(e, o.beEn) & 1u) && (rd(e, o.feCntl) & 7u) == kOtg) return "pipe 2 busy: another enabled DIG front end sources OTG2";
    }
    if (s.sDto0Mod < kDprefLoHz || s.sDto0Mod > kDprefHiHz) return "DP_DTO0_MODULO (the GOP's DPREFCLK) is not 716.0-717.4 MHz: the DTO2 value would be wrong";
    return nullptr;
}

// The S2b ladder + PhxDpRun.h's gates on a copy, at link rate `rate` (reads + the DPCD caps read). Fills s.b.
inline const char *commonGates(const Env &e, State &st, Stream &s, const phxdpr::Run &dpl, int restartDone, const phxdpr::Aux &aux,
                               const phxdpl::Conn &k, uint8_t rate) {
    if (dpl.res != phxdpr::kPass || dpl.dig != static_cast<int>(k.dig) || !dpl.c3Done[k.dig])
        return "needs a debug.phxdplink PASS on this connector as the last S2b run of this boot (it loads C3 and proves the link)";
    if (dpl.link.rate != rate || dpl.link.lanes != 4)
        return rate == phxdpl::kHbr ? "the last S2b PASS was not at HBR x4, the link this stream needs: run debug.phxdplink=0x10 + n first"
                                    : "the last S2b PASS was not at RBR x4, the link this stream needs: run debug.phxdplink=n first";
    phxdpr::Run g{};
    g.failed = dpl.failed;
    for (int i = 0; i < 5; i++) g.c3Done[i] = dpl.c3Done[i];
    g.sel = static_cast<int>(k.dig) | (rate == phxdpl::kHbr ? static_cast<int>(phxdpr::kSelHbr) : 0);
    g.dig = -1; g.pipe0Same = g.pipe1Same = 1;
    const phxdpl::Conn *kk = nullptr;
    const char *w = phxdpr::gates(e, st, g, restartDone, aux, &kk);
    s.b = g;
    if (w) return w;
    if (kk != &k) return "connector mismatch";
    return nullptr;
}

// Survey or stream (S2c): every gate, reads only (+ the DPCD caps read). Fills s.b with the S2b gates' baseline.
inline const char *gates(const Env &e, State &st, Stream &s, const phxdpr::Run &dpl, int restartDone, const phxdpr::Aux &aux,
                         const phxdpl::Conn &k) {
    if (s.act == kActStream || s.act == kActDesk) {
        if (s.failed) return "a debug.phxdpstream stream run failed in this boot: undo (0x30 + n) and reboot";
        if (s.touched || s.linkTouched) return "a stream is applied: undo (0x30 + n) first";
    }
    const char *w = commonGates(e, st, s, dpl, restartDone, aux, k, phxdpl::kRbr);
    if (w) return w;
    phxdpl::Sink sink{};
    phxdpl::Link need{};
    if (!phxdpl::parseCaps(s.b.caps, &sink) || !phxdpl::pickLink(sink, phxdpl::kCea1080p60.pclkKhz, &need) ||
        need.rate != s.b.link.rate || need.lanes != s.b.link.lanes)
        return "the sink's link for 1080p60 is not RBR x4 (Linux's)";
    return pipe2Gates(e, s, k);
}

// The DIG/DP Io: PhxDpRun.h's per-run callbacks (lists, guards, counters in s.b).
inline phxdpl::Io linkIo(phxdpr::Ctx &x) {
    return phxdpl::Io{&x, phxdpr::cRd, phxdpr::cWr, phxdpr::cDmub, phxdpr::cAuxRd, phxdpr::cAuxWr, phxdpr::cDelay};
}

inline int stream(const Env &e, State &st, Stream &s, const phxdpl::Conn &k, const phxdpr::Aux &aux) {
    const PipeRegs g = pipeRegs(kOtg);
    s.n = buildSteps(kOtg, phxdpl::kCea1080p60, kOtg1080p60, kMagenta, s.steps);
    s.dig = static_cast<int>(k.dig);
    s.kind = kActStream; s.onePpc = 0;
    // 1. pipe 2: timing, clocks, DPG
    s.phase = 1;
    for (unsigned i = 0; i < s.n; i++) {
        const Step &p = s.steps[i];
        s.failStep = static_cast<int>(i);
        if (p.op == kPoll) {
            uint32_t us = 0;
            while ((rd(e, p.reg) & p.mask) != p.val) {
                if (us >= p.budgetUs) return endRun(s, kFailed, "a clock did not come on within 1 ms (CLOCK_ON)");
                e.io.delayUs(e.io.c, 10); us += 10;
            }
            continue;
        }
        const uint32_t b = rd(e, p.reg);
        s.before[i] = b;
        const uint32_t v = p.op == kDiv ? ((b & ~p.mask) | (p.val & p.mask)) : p.val;
        s.touched = i + 1;
        if (!pWr(e, st, s, p.reg, v)) return endRun(s, st.halted ? kHalted : kFailed, st.halted ? "pipe 0 / pipe 1 changed after a pipe-2 write: STOP, halted until reboot" : "a pipe-2 write was dropped (not on the list)");
        const uint32_t rb = rd(e, p.reg);
        if (p.op == kDiv ? (rb != v) : ((rb ^ p.val) & p.mask) != 0) return endRun(s, kFailed, "pipe-2 read-back mismatch");
    }
    s.failStep = -1;
    if (!otgFrames(e)) return endRun(s, kFailed, "OTG2 frame counter not moving after the timing steps (no pixel clock): undo");
    // 2. DIG / DP: FE fed by OTG2, link setup, ENABLE, training, stream on (Linux's modeset link enable)
    s.phase = 2;
    s.linkTouched = 1;
    phxdpl::Sink sink{};
    phxdpl::parseCaps(s.b.caps, &sink);
    phxdpr::Ctx x{&e, &st, &s.b, &k, aux};
    const phxdpl::Io io = linkIo(x);
    phxdpl::Opts o;
    o.panelModeRead = true;   // the modeset path reads 0x10A (Trace E E6)
    o.attempts = 1;           // stop at the first failure
    s.erc = phxdpl::enableStream(io, k, sink, s.b.link, kOtg, phxdpl::kCea1080p60, o, &s.rep);
    if (st.halted) return endRun(s, kHalted, "pipe 0 / pipe 1 changed during the link enable: STOP, halted until reboot");
    if (st.dmub.dead) return endRun(s, kTimeout, "a TRANSMITTER_CONTROL timed out: debug.phxdmub_dead latched; undo (MMIO only), warm reboot");
    if (s.erc != phxdpl::kOk) return endRun(s, kFailed, phxdpl::rcName(s.erc));
    if (s.b.strayWr || s.b.dpcdStray) return endRun(s, kFailed, "a write off the lists was dropped (stray): not the planned sequence");
    // 3. post (reads, plus two DPCD reads)
    s.phase = 3;
    const phxdpl::Regs dr = phxdpl::regs(k.dig);
    for (s.statusUs = 0; !(rd(e, dr.vidStream) & phxdpl::kStreamStatus) && s.statusUs < kStatusWaitUs; s.statusUs += 1000) e.io.delayUs(e.io.c, 1000);
    const uint32_t f0 = rd(e, phxp2::kOtg0Frame), f1 = rd(e, phxp2::kOtg1Frame), f2 = rd(e, g.frame);
    e.io.delayUs(e.io.c, kMeasureUs);
    s.hz0 = ((rd(e, phxp2::kOtg0Frame) - f0) & 0xffffff) * (1000000u / kMeasureUs);
    s.hz1 = ((rd(e, phxp2::kOtg1Frame) - f1) & 0xffffff) * (1000000u / kMeasureUs);
    s.hz2 = ((rd(e, g.frame) - f2) & 0xffffff) * (1000000u / kMeasureUs);
    s.pVid = rd(e, dr.vidStream); s.pFifo = rd(e, dr.fifo); s.pFe = rd(e, dr.feCntl); s.pBeEn = rd(e, dr.beEn);
    s.pPix = rd(e, g.pixRate); s.pOdm = rd(e, g.odmGlobal); s.pDiv = rd(e, kPixelRateDiv);
    s.auxRc = phxdpr::cAuxRd(&x, 0x200, s.after, 6);
    if (!s.auxRc) s.auxRc = phxdpr::cAuxRd(&x, 0x600, &s.power, 1);
    if (!phxdpr::guard(e, st, s.b) || !phxdpr::framesMove(e, st, s.b)) return endRun(s, kHalted, "pipe 0 / pipe 1 changed after the stream: halted until reboot");
    if (!(s.pVid & phxdpl::kStreamStatus)) return endRun(s, kPostFail, "DPn_DP_VID_STREAM_CNTL STATUS never rose (100 ms): SNAP, then undo");
    if (s.hz2 < 55 || s.hz2 > 65) return endRun(s, kPostFail, "OTG2 not at 55-65 Hz");
    const uint32_t want0 = phxp2::p0ModeHz(e);
    if (!want0 || s.hz0 < want0 * 92 / 100 || s.hz0 > want0 * 109 / 100) return endRun(s, kPostFail, "OTG0 off its mode's rate");
    if (s.pFifo & kFifoErr) return endRun(s, kPostFail, "DIGn_DIG_FIFO_CTRL0 DIG_FIFO_ERROR set");
    if (s.pPix & kDioFifoErr) return endRun(s, kPostFail, "OTG2_PIXEL_RATE_CNTL DIO FIFO error set");
    if ((s.pDiv ^ s.sDiv) & ~divMask(kOtg)) return endRun(s, kPostFail, "OTG_PIXEL_RATE_DIV changed outside OTG2's field");
    if (s.auxRc || (s.after[2] & 0x77) != 0x77 || (s.after[3] & 0x77) != 0x77 || !(s.after[4] & 1) || s.power != 1)
        return endRun(s, kPostFail, "the sink does not report the 4 lanes locked + aligned in D0 after the stream");
    return endRun(s, kPass, "stream on: OTG2 1080p60 DPG -> DIG FE -> BE -> PHY, sink locked");
}

// The undo's own baseline: pipe 0 / pipe 1 as they are right before the undo (a pipe-1 mode switch or the pipe-0 engine may
// have changed them legitimately since the stream; the guard is about the undo's own writes). Reads only.
inline const char *rebase(const Env &e, phxdpr::Run &r) {
    const char *w = nullptr;
    if (!phxp2::pipe0Abs(e, &w)) return w;
    for (unsigned j = 0; j < phxp2::kNFp; j++) r.fp0[j] = rd(e, phxp2::kFp[j].reg);
    r.odm0 = rd(e, phxp2::kOdm0Global) & phxp2::kOdmUnderflowBits;
    for (unsigned j = 0; j < phxdpr::kNFp1; j++) r.fp1[j] = rd(e, phxdpr::kFp1[j].reg);
    r.odm1 = rd(e, phxp2::kOdm1Global) & phxp2::kOdmUnderflowBits;
    r.otg1Ctl = r.fp1[0];
    r.pipe0Same = r.pipe1Same = 1; r.pipeWhy = nullptr;
    return nullptr;
}

// Undo: Linux's DP unplug order (multiport-s2c 2.5), then the restore pass.
inline int undo(const Env &e, State &st, Stream &s, const phxdpl::Conn &k, const phxdpr::Aux &aux) {
    if (!s.touched && !s.linkTouched) return refuse(s, "nothing applied in this boot");
    if (st.halted) return refuse(s, "halted: pipe 0 / pipe 1 changed earlier in this boot: nothing more, not even undo (reboot)");
    if (s.dig != static_cast<int>(k.dig)) return refuse(s, "the applied stream is on the other connector");
    const char *w = rebase(e, s.b);
    if (w) return refuse(s, w);
    s.undos++;
    s.urc = 0; s.restored = 0;
    const PipeRegs g = pipeRegs(kOtg);
    const phxdpl::Regs dr = phxdpl::regs(k.dig);
    // 1. stream + link off (DIS_DEFER, stream disable, steer reset, D3, DISABLE when the ring is alive, BE source off, FIFO)
    if (s.linkTouched) {
        s.phase = 4;
        phxdpr::Ctx x{&e, &st, &s.b, &k, aux};
        const phxdpl::Io io = linkIo(x);
        phxdpl::Opts o;
        o.onePpc = s.onePpc != 0;   // the FIFO base the applied DIG side used
        phxdpl::Report r{};
        s.urc = phxdpl::disableStream(io, k, o, &r, !st.dmub.dead);   // an AUX error (cable pulled) is recorded, not a stop
        if (st.halted) return endRun(s, kHalted, "pipe 0 / pipe 1 changed during the undo: halted until reboot");
        phxdpr::cWr(&x, dr.feCntl, s.sFeCntl);                       // the FE back to its survey value
        if (st.halted) return endRun(s, kHalted, "pipe 0 / pipe 1 changed during the undo: halted until reboot");
    }
    // 2. pipe 2 off: ODM source off, OTG master off, VTG off, wait OTG_BUSY clear (the DP unplug trace's order)
    s.phase = 5;
    auto applied = [&](uint32_t reg) { for (unsigned i = 0; i < s.touched; i++) if (s.steps[i].op != kPoll && s.steps[i].reg == reg) return true; return false; };
    if (applied(g.odmSrc) && !pWr(e, st, s, g.odmSrc, kOdmSrcNone)) return endRun(s, st.halted ? kHalted : kUndoFail, "undo: ODM2 source off dropped");
    if (applied(g.otgCtl) && !pWr(e, st, s, g.otgCtl, rd(e, g.otgCtl) & ~(kOtgMasterEn | kOtgMasterState))) return endRun(s, st.halted ? kHalted : kUndoFail, "undo: OTG2 master off dropped");
    if (applied(g.vtg) && !pWr(e, st, s, g.vtg, rd(e, g.vtg) & ~kVtgEnable)) return endRun(s, st.halted ? kHalted : kUndoFail, "undo: VTG2 off dropped");
    if (applied(g.otgClk)) {
        uint32_t us = 0;
        while ((rd(e, g.otgClk) & kOtgBusy) && us < kBusyWaitUs) { e.io.delayUs(e.io.c, 1000); us += 1000; }
        if (rd(e, g.otgClk) & kOtgBusy) return endRun(s, kUndoFail, "undo: OTG2 still busy 100 ms after master off (nothing restored)");
    }
    // 3. restore pass: every written register back to its value before its first write, reverse order
    s.phase = 6;
    for (int i = static_cast<int>(s.touched) - 1; i >= 0; i--) {
        const Step &p = s.steps[i];
        if (p.op == kPoll) continue;
        bool first = true;
        for (int j = 0; j < i && first; j++) if (s.steps[j].op != kPoll && s.steps[j].reg == p.reg) first = false;
        if (!first) continue;
        const uint32_t v = p.op == kDiv ? ((rd(e, p.reg) & ~p.mask) | (s.before[i] & p.mask)) : s.before[i];
        if (!pWr(e, st, s, p.reg, v)) return endRun(s, st.halted ? kHalted : kUndoFail, "undo: a restore write was dropped");
        const uint32_t rb = rd(e, p.reg);
        if (p.op == kDiv ? rb != v : ((rb ^ v) & p.mask) != 0) return endRun(s, kUndoFail, "undo: restore read-back mismatch");
        s.restored++;
    }
    s.uOtgCtl = rd(e, g.otgCtl); s.uOtgClk = rd(e, g.otgClk); s.uBeEn = rd(e, dr.beEn); s.uLink = rd(e, dr.linkCntl);
    s.uVid = rd(e, dr.vidStream); s.uDiv = rd(e, kPixelRateDiv); s.uFe = rd(e, dr.feCntl);
    if (!phxdpr::guard(e, st, s.b)) return endRun(s, kHalted, "pipe 0 / pipe 1 changed after the undo: halted until reboot");
    if ((s.uOtgCtl & kOtgMasterEn) || (s.uOtgClk & 1u) || (s.uBeEn & 1u) || (s.uLink & phxdpl::kLinkTrained) ||
        (s.uVid & phxdpl::kStreamEnable) || s.uDiv != s.sDiv)
        return endRun(s, kUndoFail, "undo done but pipe 2 / the DIG is not dark again (reboot)");
    s.touched = 0; s.linkTouched = 0; s.lit = 0; s.dig = -1; s.kind = 0;
    return endRun(s, kUndone, s.urc ? "undo clean (the sink did not answer the D3 write: cable pulled?)" : "undo clean: stream, link and pipe 2 off, registers restored");
}

// ---- b119 S2d: DIG n fed from the GOP's OTG0 (head 0's desktop, whatever mode it runs) ------------------------------------
// OTG0's timing as programmed (reads only; optc1_program_timing's encoding, the inverse of buildSteps').
inline void readOtgTiming(const Env &e, unsigned n, phxdpl::Timing *t) {
    const PipeRegs g = pipeRegs(n);
    const uint32_t ht = (rd(e, g.hTotal) & 0x7fff) + 1, hs = (rd(e, g.hSyncA) >> 16) & 0x7fff, hb = rd(e, g.hBlank);
    const uint32_t vt = (rd(e, g.vTotal) & 0x7fff) + 1, vs = (rd(e, g.vSyncA) >> 16) & 0x7fff, vb = rd(e, g.vBlank);
    const uint32_t hbs = hb & 0x7fff, hbe = (hb >> 16) & 0x7fff, vbs = vb & 0x7fff, vbe = (vb >> 16) & 0x7fff;
    t->hTotal = static_cast<uint16_t>(ht); t->hActive = static_cast<uint16_t>(hbs - hbe); t->hFront = static_cast<uint16_t>(ht - hbs);
    t->hSync = static_cast<uint16_t>(hs);
    t->vTotal = static_cast<uint16_t>(vt); t->vActive = static_cast<uint16_t>(vbs - vbe); t->vFront = static_cast<uint16_t>(vt - vbs);
    t->vSync = static_cast<uint16_t>(vs);
    t->hNeg = (rd(e, g.hSyncCntl) & 1u) != 0; t->vNeg = (rd(e, g.vSyncCntl) & 1u) != 0;
    t->pclkKhz = 0;
}
constexpr uint32_t kDtoOn = 0x10u;          // OTGn_PIXEL_RATE_CNTL DP_DTOn_ENABLE
constexpr uint32_t kDivGop1ppc = 6u;        // OTG0's K1/K2 field as the GOP leaves it with DTO0 = the full pclk (read on the box)
constexpr uint32_t kDivLinux2ppc = 2u;      // Linux's (Trace E): DTO = pclk / 2
// Pipe 0 as the source: a known AOC mode, 1920x1080, clocked by DTO0 in one of the two consistent conventions, no enabled
// DIG fed by OTG0 yet (the AOC has left DP B), the link this pixel clock needs. Reads only.
inline const char *otg0Gates(const Env &e, Stream &s, const phxdpl::Conn &k, uint8_t *rateOut) {
    s.p0Hz = phxp2::p0ModeHz(e);
    if (!s.p0Hz) return "OTG0 is not at a known AOC mode (1080p60 0x897/0x464 or 1080p144 0x807/0x44b)";
    readOtgTiming(e, 0, &s.t0);
    const phxdpl::Timing &t = s.t0;
    if (t.hActive != 1920 || t.vActive != 1080 || t.hFront == 0 || t.vFront == 0 || !t.hSync || !t.vSync ||
        t.hFront + t.hSync >= t.hTotal - t.hActive || t.vFront + t.vSync >= t.vTotal - t.vActive || (rd(e, pipeRegs(0).hSyncA) & 0x7fff) || (rd(e, pipeRegs(0).vSyncA) & 0x7fff))
        return "OTG0's timing registers are not a 1920x1080 timing this file can describe in the MSA";
    s.p0PclkKhz = static_cast<uint32_t>((static_cast<uint64_t>(t.hTotal) * t.vTotal * s.p0Hz + 500u) / 1000u);
    s.p0Pix = rd(e, pipeRegs(0).pixRate); s.p0Phase = rd(e, pipeRegs(0).dtoPhase); s.p0Mod = rd(e, pipeRegs(0).dtoModulo);
    s.sDiv = rd(e, kPixelRateDiv); s.sFeCntl = rd(e, phxdpl::regs(k.dig).feCntl);
    if (!(s.p0Pix & kDtoOn)) return "OTG0_PIXEL_RATE_CNTL DP_DTO0_ENABLE is 0: OTG0 is not DTO-clocked";
    if (s.p0Mod < kDprefLoHz || s.p0Mod > kDprefHiHz) return "DP_DTO0_MODULO is not 716.0-717.4 MHz (DPREFCLK)";
    const uint64_t hz = static_cast<uint64_t>(s.p0PclkKhz) * 1000u, ph = s.p0Phase, f = s.sDiv & divMask(0);
    auto near = [](uint64_t a, uint64_t b) { return (a > b ? a - b : b - a) * 200u <= b; };   // within 0.5 %
    if (f == kDivGop1ppc && near(ph, hz)) s.onePpc = 1;
    else if (f == kDivLinux2ppc && near(ph * 2u, hz)) s.onePpc = 0;
    else return "OTG0's clocking is neither the GOP's (div field 6, DTO0 = pclk) nor Linux's (div field 2, DTO0 = pclk / 2)";
    s.t0.pclkKhz = s.p0PclkKhz;
    for (unsigned d = 0; d <= 4; d++) {
        if (d == k.dig) continue;
        const phxdpl::Regs o = phxdpl::regs(d);
        if ((rd(e, o.beEn) & 1u) && (rd(e, o.feCntl) & 7u) == 0) return "OTG0 already drives an enabled DIG (the AOC still on DP B?): move the cable first";
    }
    phxdpl::Sink sink{};
    phxdpl::Link need{};
    if (!phxdpl::parseCaps(s.b.caps, &sink) || !phxdpl::pickLink(sink, s.p0PclkKhz, &need) || need.lanes != 4)
        return "the sink cannot carry OTG0's pixel clock on 4 lanes";
    *rateOut = need.rate;
    return nullptr;
}
inline const char *deskGates(const Env &e, State &st, Stream &s, const phxdpr::Run &dpl, int restartDone, const phxdpr::Aux &aux,
                             const phxdpl::Conn &k) {
    if (s.act == kActDesk) {
        if (s.failed) return "a debug.phxdpstream stream run failed in this boot: undo (0x30 + n) and reboot";
        if (s.touched || s.linkTouched) return "a stream is applied: undo (0x30 + n) first";
    }
    // the caps first (the link depends on them), through the S2b gates at the rate the last S2b proved
    const char *w = commonGates(e, st, s, dpl, restartDone, aux, k, dpl.link.rate);
    if (w) return w;
    uint8_t rate = 0;
    if ((w = otg0Gates(e, s, k, &rate))) return w;
    if (rate != dpl.link.rate)
        return rate == phxdpl::kHbr ? "OTG0's pixel clock needs HBR x4: run debug.phxdplink=0x10 + n (HBR) first"
                                    : "OTG0's pixel clock fits RBR x4: run debug.phxdplink=n (RBR, Linux's 1080p60 link) first";
    return nullptr;
}
inline int desk(const Env &e, State &st, Stream &s, const phxdpl::Conn &k, const phxdpr::Aux &aux) {
    s.dig = static_cast<int>(k.dig);
    s.kind = kActDesk; s.n = 0;
    s.phase = 2;
    s.linkTouched = 1;
    phxdpl::Sink sink{};
    phxdpl::parseCaps(s.b.caps, &sink);
    phxdpr::Ctx x{&e, &st, &s.b, &k, aux};
    const phxdpl::Io io = linkIo(x);
    phxdpl::Opts o;
    o.panelModeRead = true;
    o.attempts = 1;
    o.onePpc = s.onePpc != 0;
    s.erc = phxdpl::enableStream(io, k, sink, s.b.link, 0, s.t0, o, &s.rep);
    if (st.halted) return endRun(s, kHalted, "pipe 0 / pipe 1 changed during the link enable: STOP, halted until reboot");
    if (st.dmub.dead) return endRun(s, kTimeout, "a TRANSMITTER_CONTROL timed out: debug.phxdmub_dead latched; undo (MMIO only), warm reboot");
    if (s.erc != phxdpl::kOk) return endRun(s, kFailed, phxdpl::rcName(s.erc));
    if (s.b.strayWr || s.b.dpcdStray) return endRun(s, kFailed, "a write off the lists was dropped (stray): not the planned sequence");
    s.phase = 3;
    const phxdpl::Regs dr = phxdpl::regs(k.dig);
    for (s.statusUs = 0; !(rd(e, dr.vidStream) & phxdpl::kStreamStatus) && s.statusUs < kStatusWaitUs; s.statusUs += 1000) e.io.delayUs(e.io.c, 1000);
    const uint32_t f0 = rd(e, phxp2::kOtg0Frame), f1 = rd(e, phxp2::kOtg1Frame);
    e.io.delayUs(e.io.c, kMeasureUs);
    s.hz0 = ((rd(e, phxp2::kOtg0Frame) - f0) & 0xffffff) * (1000000u / kMeasureUs);
    s.hz1 = ((rd(e, phxp2::kOtg1Frame) - f1) & 0xffffff) * (1000000u / kMeasureUs);
    s.hz2 = 0;
    s.pVid = rd(e, dr.vidStream); s.pFifo = rd(e, dr.fifo); s.pFe = rd(e, dr.feCntl); s.pBeEn = rd(e, dr.beEn);
    s.pPix = rd(e, pipeRegs(0).pixRate); s.pOdm = rd(e, phxp2::kOdm0Global); s.pDiv = rd(e, kPixelRateDiv);
    s.auxRc = phxdpr::cAuxRd(&x, 0x200, s.after, 6);
    if (!s.auxRc) s.auxRc = phxdpr::cAuxRd(&x, 0x600, &s.power, 1);
    if (!phxdpr::guard(e, st, s.b) || !phxdpr::framesMove(e, st, s.b)) return endRun(s, kHalted, "pipe 0 / pipe 1 changed after the stream: halted until reboot");
    if (!(s.pVid & phxdpl::kStreamStatus)) return endRun(s, kPostFail, "DPn_DP_VID_STREAM_CNTL STATUS never rose (100 ms): SNAP, then undo");
    if (s.hz0 < s.p0Hz * 92 / 100 || s.hz0 > s.p0Hz * 109 / 100) return endRun(s, kPostFail, "OTG0 off its mode's rate");
    if (s.pFifo & kFifoErr) return endRun(s, kPostFail, "DIGn_DIG_FIFO_CTRL0 DIG_FIFO_ERROR set");
    if (s.pDiv != s.sDiv) return endRun(s, kPostFail, "OTG_PIXEL_RATE_DIV changed");
    if (s.auxRc || (s.after[2] & 0x77) != 0x77 || (s.after[3] & 0x77) != 0x77 || !(s.after[4] & 1) || s.power != 1)
        return endRun(s, kPostFail, "the sink does not report the 4 lanes locked + aligned in D0 after the stream");
    return endRun(s, kPass, "stream on: OTG0 (head 0's desktop) -> DIG FE -> BE -> PHY, sink locked");
}

// Lit status (a survey while something is applied): reads only, the DPCD status read included; nothing latches.
inline int litStatus(const Env &e, Stream &s, const phxdpl::Conn &k, const phxdpr::Aux &aux) {
    if (s.dig != static_cast<int>(k.dig)) return refuse(s, "lit status: the applied stream is on the other connector");
    const phxdpl::Regs dr = phxdpl::regs(k.dig);
    const uint32_t f0 = rd(e, phxp2::kOtg0Frame), f1 = rd(e, phxp2::kOtg1Frame), f2 = rd(e, pipeRegs(kOtg).frame);
    e.io.delayUs(e.io.c, kMeasureUs);
    s.hz0 = ((rd(e, phxp2::kOtg0Frame) - f0) & 0xffffff) * (1000000u / kMeasureUs);
    s.hz1 = ((rd(e, phxp2::kOtg1Frame) - f1) & 0xffffff) * (1000000u / kMeasureUs);
    s.hz2 = ((rd(e, pipeRegs(kOtg).frame) - f2) & 0xffffff) * (1000000u / kMeasureUs);
    s.pVid = rd(e, dr.vidStream); s.pFifo = rd(e, dr.fifo); s.pFe = rd(e, dr.feCntl); s.pBeEn = rd(e, dr.beEn);
    s.pPix = rd(e, pipeRegs(s.kind == kActDesk ? 0 : kOtg).pixRate); s.pDiv = rd(e, kPixelRateDiv);
    s.pOdm = rd(e, s.kind == kActDesk ? phxp2::kOdm0Global : pipeRegs(kOtg).odmGlobal);
    s.auxRc = aux.rd(aux.c, k.aux, 0x200, s.after, 6);
    if (!s.auxRc) s.auxRc = aux.rd(aux.c, k.aux, 0x600, &s.power, 1);
    const char *w = nullptr;
    s.lsPipe0 = phxdpr::pipe0Same(e, s.b, &w) ? 1 : 0;
    s.lsPipe1 = phxdpr::pipe1Same(e, s.b, &w) ? 1 : 0;
    s.res = kSurvey;
    s.why = (s.pVid & phxdpl::kStreamStatus) ? "lit status (reads only): stream STATUS 1" : "lit status (reads only): stream STATUS 0";
    return kSurvey;
}

// The sysctl write. dpl = the debug.phxdplink run state (S2b), read only.
inline int run(const Env &e, State &st, Stream &s, const phxdpr::Run &dpl, int sel, int restartDone, const phxdpr::Aux &aux) {
    s.runs++;
    s.sel = sel; s.why = nullptr; s.res = kNone; s.phase = 0; s.failStep = -1; s.act = 0;
    if (sel < 0 || (static_cast<uint32_t>(sel) & ~0x7fu) || ((sel >> 4) & 7) == 0 || ((sel >> 4) & 7) > kActDeskSurvey)
        return refuse(s, "selector: 0x10 + n survey, 0x20 + n stream (S2c), 0x30 + n undo, 0x40 + n desktop (S2d: DIG n fed from OTG0), 0x50 + n its survey; n = 0 (DP A) or 4 (DP E)");
    const phxdpl::Conn *k = phxdpl::connByDig(static_cast<uint32_t>(sel) & 0xfu);
    if (!k) return refuse(s, "not a DP connector this file drives: DP A (0) or DP E (4) only (DP B = the GOP's pipe 0, HDMI D = pipe 1)");
    s.act = (sel >> 4) & 7;
    if (s.act == kActUndo) return undo(e, st, s, *k, aux);
    if ((s.act == kActSurvey || s.act == kActDeskSurvey) && (s.touched || s.linkTouched)) return litStatus(e, s, *k, aux);
    s.erc = 0; s.rep = phxdpl::Report{}; s.hz0 = s.hz1 = s.hz2 = 0;
    const bool deskAct = s.act == kActDesk || s.act == kActDeskSurvey;
    const char *w = deskAct ? deskGates(e, st, s, dpl, restartDone, aux, *k) : gates(e, st, s, dpl, restartDone, aux, *k);
    if (w) return refuse(s, w);
    if (s.act == kActSurvey || s.act == kActDeskSurvey) { s.res = kSurvey; s.why = "survey passed: every gate holds (reads only)"; return kSurvey; }
    return deskAct ? desk(e, st, s, *k, aux) : stream(e, st, s, *k, aux);
}

inline int stat(const Stream &s, char *b, int n) {
    int o = 0;
#define PHXDPS_OUT(...) do { if (o < n - 1) { const int k_ = snprintf(b + o, static_cast<size_t>(n - o), __VA_ARGS__); o += k_ > 0 ? k_ : 0; if (o > n - 1) o = n - 1; } } while (0)
    static const char *const act[] = {"-", "survey", "stream", "undo", "desktop", "desk-survey"};
    static const char *const ph[] = {"gates", "pipe2", "link", "post", "undo-link", "undo-otg", "undo-restore"};
    const unsigned d = static_cast<unsigned>(s.sel) & 0xfu;
    PHXDPS_OUT("result=%s sel=0x%x act=%s conn=%s phase=%s why=%s\n", kResName[s.res >= 0 && s.res <= kUndoFail ? s.res : 0], s.sel,
               act[s.act >= 0 && s.act <= kActDeskSurvey ? s.act : 0], d == 0 ? "DP-A" : d == 4 ? "DP-E" : "-", ph[s.phase >= 0 && s.phase <= 6 ? s.phase : 0],
               s.why ? s.why : "-");
    PHXDPS_OUT("boot: runs=%u passes=%u refusals=%u undos=%u failed=%d lit=%d touched=%u link_touched=%d applied_conn=%d last_refusal=%s\n",
               s.runs, s.passes, s.refusals, s.undos, s.failed, s.lit, s.touched, s.linkTouched, (s.touched || s.linkTouched) ? s.dig : -1, s.lastRefusal ? s.lastRefusal : "-");
    PHXDPS_OUT("kind=%s ppc=%d otg0: %ux%u total %ux%u front %u/%u sync %u/%u pclk_khz=%u hz=%u pix=0x%x dto=%u/%u | lit status: pipe0 same=%d pipe1 same=%d\n",
               s.kind == kActDesk ? "desktop(OTG0)" : s.kind == kActStream ? "stream(OTG2)" : "-", s.onePpc ? 1 : 2, s.t0.hActive, s.t0.vActive,
               s.t0.hTotal, s.t0.vTotal, s.t0.hFront, s.t0.vFront, s.t0.hSync, s.t0.vSync, s.p0PclkKhz, s.p0Hz, s.p0Pix, s.p0Phase, s.p0Mod,
               s.lsPipe0, s.lsPipe1);
    PHXDPS_OUT("pipe2 survey: otg_ctl=0x%x otg_clk=0x%x odm_in=0x%x opp=0x%x dpg=0x%x pix=0x%x div=0x%x mpc_out=0x%x hubp=0x%08x dto0_mod=%u fe=0x%x\n",
               s.sOtgCtl, s.sOtgClk, s.sOdmIn, s.sOpp, s.sDpg, s.sPix, s.sDiv, s.sMpcOut, s.sHubp, s.sDto0Mod, s.sFeCntl);
    PHXDPS_OUT("caps:"); for (int i = 0; i < 16; i++) PHXDPS_OUT(" %02x", s.b.caps[i]);
    PHXDPS_OUT(" | link rate=0x%02x lanes=%u\n", s.b.link.rate, s.b.link.lanes);
    char nm[48] = "-";
    if (s.failStep >= 0 && static_cast<unsigned>(s.failStep) < s.n) snprintf(nm, sizeof nm, s.steps[s.failStep].name, kOtg);
    PHXDPS_OUT("steps: n=%u applied=%u fail_step=%d(%s)\n", s.n, s.touched, s.failStep, nm);
    PHXDPS_OUT("train: rc=%s cr_loops=%u eq_loops=%u vs=%u pe=%u pattern=0x%02x status(202..207)=%02x %02x %02x %02x %02x %02x\n",
               phxdpl::rcName(s.erc), s.rep.crLoops, s.rep.eqLoops, s.rep.vs, s.rep.pe, s.rep.pattern, s.rep.status[0], s.rep.status[1],
               s.rep.status[2], s.rep.status[3], s.rep.status[4], s.rep.status[5]);
    PHXDPS_OUT("stream: vid_stream=0x%08x status_us=%u fifo=0x%08x fe=0x%08x be_en=0x%x otg2_pix=0x%x odm2=0x%x div=0x%x\n",
               s.pVid, s.statusUs, s.pFifo, s.pFe, s.pBeEn, s.pPix, s.pOdm, s.pDiv);
    PHXDPS_OUT("rates: otg0=%u otg1=%u otg2=%u Hz | sink: rc=%d after(200..205)=%02x %02x %02x %02x %02x %02x power(600)=%02x\n",
               s.hz0, s.hz1, s.hz2, s.auxRc, s.after[0], s.after[1], s.after[2], s.after[3], s.after[4], s.after[5], s.power);
    PHXDPS_OUT("io: mmio_wr=%u dmub_sent=%u ring_rc=%d(%s) aux_rd=%u aux_wr=%u aux_err=%u stray_wr=%u dpcd_stray=%u blocked=%u\n",
               s.b.mmioWr, s.b.dmubSent, s.b.ringRc, s.b.ringWhy ? s.b.ringWhy : phxdmub::rcName(s.b.ringRc), s.b.auxRdN, s.b.auxWrN,
               s.b.auxErr, s.b.strayWr, s.b.dpcdStray, s.b.blocked);
    PHXDPS_OUT("undo: rc=%s restored=%u otg_ctl=0x%x otg_clk=0x%x be_en=0x%x link=0x%x vid=0x%x div=0x%x fe=0x%x | pipe0 same=%d pipe1 same=%d%s%s\n",
               phxdpl::rcName(s.urc), s.restored, s.uOtgCtl, s.uOtgClk, s.uBeEn, s.uLink, s.uVid, s.uDiv, s.uFe, s.b.pipe0Same,
               s.b.pipe1Same, s.b.pipeWhy ? " changed=" : "", s.b.pipeWhy ? s.b.pipeWhy : "");
#undef PHXDPS_OUT
    return o;
}

}  // namespace phxdps
