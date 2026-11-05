// SPDX-License-Identifier: MIT
// PhxPipe2.h: the M7 S2 plan engine of kext b82:
// light the Acer (HDMI, DIG3 / PHY D / PLL3) on pipe 1 next to the AOC the GOP keeps on pipe 0. Kernel-free (stdint +
// snprintf, host-tested); the kext glue is phx_pipe2.cpp. The plan tables are generated from register captures of
// Linux amdgpu (phx_s2_plan_1080p60.inc).
//
// Ladder (debug.phxpipe2 = N, root, never armed at boot; each level needs the one below it passed in this boot):
//   1 survey  reads only: static check of every table entry (allow-list, DMUB allow-list), FB base, ring geometry, all
//             24 requirements, pipe 0 (OTG0 on, H_TOTAL 0x897, HUBP0 underflow 0, frame counter moving), the pipe-0
//             fingerprint baseline, every step's current value.
//   2 ping    S2.1: one QUERY_FEATURE_CAPS through the ring.
//   3 stream  S2.2: 62 writes + 3 DMUB commands, DPG solid colour on OTG1 / DIG3 / PHY D (black since b94, magenta before).
//   4 plane   S2.3: HUBP1 scans head 1's surface (VRAM 64 MiB, 1680x1050; b87 debug.phxpipe2_head1=1 before the plane:
//             1920x1080, applyAlt).
//   0 undo    the pipe-1 part of Linux's unplug for whatever was applied, then every plan register back to its value
//             before the first write (reverse order; kPhxS2NoRestore skipped). Pipe 1 dark again = the ladder restarts
//             at survey; otherwise reboot.
// Every write: on the pipe-1 allow-list (kAllow), read back under the table's mask, then the pipe-0 fingerprint
// re-read. Every DMUB command: PhxDmub.h submit, then the full pipe-0 guard (fingerprint + OTG0 frame counter moving).
// The engine stops at the first failure and records the step. Latches until reboot: `halted` (pipe 0 changed after we
// wrote something: nothing more, not even undo) and phxdmub `dead` (a command timed out: nothing more).
// SCDC1: an I2C step (PHX_S2_I2C) runs one SCDC operation over DC_I2C (PhxDdc.h) for HDMI 2.0 modes above 340 MHz; no
// 60/144 plan has one.
// b109: dmubRestart (debug.phxdmub_restart, the DMCUB firmware restart before any pipe-1 command; PhxDmub.h restart) and
// scdcFix (debug.phxscdc_fix, the sink's TMDS_Config re-written with the stream running), both run only when root writes them.
// b111: dmubRegTest (debug.phxdmub_regtest: one generic DMUB register write to the dark OTG1_OTG_H_TOTAL, restored
// by the host), root only, one shot.
// b113: dmubC3 (debug.phxc3, the cold-boot dark Acer: the REG_SEQ sub-tests 1-5 and mode 9, PHY D's prologue + SRAM load
// through the GOP firmware; the programs are PhxC3Load.h), root only, gated, one shot per mode.
#pragma once
#include <stdint.h>
#ifndef KERNEL
#include <stdio.h>
#endif
#include "PhxDmub.h"
#include "PhxC3Load.h"
#include "PhxDdc.h"

#ifndef PHX_S2_TYPES
#define PHX_S2_TYPES
enum { PHX_S2_W = 1, PHX_S2_DMUB = 2, PHX_S2_POLL = 3, PHX_S2_DELAY = 4, PHX_S2_CHECK = 5 };
struct phx_s2_step { uint8_t op, stage; uint16_t pad; uint32_t reg, val, mask; uint32_t dmub[16]; const char *name; };
struct phx_s2_req { uint8_t stage, cmp; uint16_t pad; uint32_t reg, mask, val; const char *name; };
#endif
#ifndef PHX_S2_ALT_TYPE
#define PHX_S2_ALT_TYPE
struct phx_s2_alt { uint32_t idx, reg, val; const char *name; };   // b87 head-1 plane values (phx_s2_plan_head1_1920.inc)
#endif
// SCDC1: one SCDC operation, a step of its own outside the generated
// 60/144 tables' enum. reg 0 (no MMIO register of the step itself); dmub[0] = kind (kI2cScdcTmds: Linux write_scdc_data
// with TMDS_Config = val; kI2cScdcCheck: SCDC register dmub[3] & mask == val), dmub[1] = DDC line (0 = DDC1), dmub[2] =
// 7-bit address (0x54 only), dmub[3] = SCDC offset (check), dmub[4] = flags (kI2cSoft: a failure is counted, the step
// passes: the undo after the TMDS output is off, where an unplugged sink has reset itself).
#ifndef PHX_S2_I2C_TYPE
#define PHX_S2_I2C_TYPE
enum { PHX_S2_I2C = 6 };
#endif

namespace phxp2 {

// Writes are allowed only inside these ranges: the pipe-1 blocks (OTG1/ODM1/VTG1, OPP1/FMT1/DPG1/OPPBUF1, MPCC1,
// MPCC_OGAM1/MCM1, MPC_OUT1, DPP1/CNVC1/DSCL1/CM1, HUBP1/HUBPREQ1/HUBPRET1/CURSOR0_1), DIG3/AFMT3/VPG3/DME3 and the
// PHY-D/PLL3 clock registers. Every range holds only those registers (the generator's selftest checks it against the Linux
// register names); OTG0/HUBP0/DIG1, the shared clocks (DENTIST, OTG_PIXEL_RATE_DIV), HUBBUB (watermarks, DET), the DMCUB
// registers and the interrupt routing are outside. No plan entry is marked shared: there is no exception path.
struct Range { uint32_t lo, hi; };
static const Range kAllow[] = {
    {0x00103, 0x00103},  // allow PHYPLLD_PIXCLK_RESYNC_CNTL
    {0x00144, 0x00144},  // allow OTG1_PIXEL_RATE_CNTL
    {0x00147, 0x00147},  // allow OTG1_PHYPLL_PIXEL_RATE_CNTL
    {0x00163, 0x00163},  // allow SYMCLKD_CLOCK_ENABLE
    {0x03515, 0x03515},  // allow PHYDSYMCLK_CLOCK_CNTL
    {0x039EF, 0x039EF},  // allow VTG1_CONTROL
    {0x03B81, 0x03C24},  // allow HUBP1_ .. HUBPREQ1_ .. HUBPRET1_ .. CURSOR0_1_
    {0x042F0, 0x04319},  // allow DPP_TOP1_ .. CNVC_CFG1_
    {0x04324, 0x043B9},  // allow DSCL1_ .. CM1_
    {0x04D56, 0x04DA6},  // allow FMT1_ .. DPG1_ .. OPPBUF1_ .. OPP_PIPE1_
    {0x04F9A, 0x04FA1},  // allow ODM1_
    {0x0506A, 0x050E1},  // allow OTG1_OTG_
    {0x05828, 0x058A6},  // allow VPG3_ .. AFMT3_ .. DME3_ .. DIG3_
    {0x09015, 0x09023},  // allow MPCC1_
    {0x09106, 0x0915D},  // allow MPCC_OGAM1_
    {0x093DC, 0x093DF},  // allow MPC_OUT1_MUX .. DENORM
    {0x093FE, 0x0940A},  // allow MPC_OUT1_CSC
    {0x094E3, 0x0956D},  // allow MPCC_MCM1_
};
inline bool forbidden(uint32_t reg) { return reg >= 0x1A751 && reg <= 0x1A762; }   // MMVM_INVALIDATE_ENG*_SEM
inline bool writeAllowed(uint32_t reg) {
    if (forbidden(reg)) return false;
    for (const Range &r : kAllow) if (reg >= r.lo && reg <= r.hi) return true;
    return false;
}
inline bool readAllowed(uint32_t reg) { return !forbidden(reg) && reg < 0x10000; }
// SCDC1: the only registers an I2C step may write, the DC_I2C engine block phxddc::ddcRead already drives for the EDID
// (DC_I2C_CONTROL 0x05358 .. DC_I2C_DATA 0x05372; the generator's selftest checks the range holds only DC_I2C registers).
static constexpr uint32_t kI2cLo = phxddc::kI2cControl, kI2cHi = phxddc::kI2cData;
inline bool i2cWriteAllowed(uint32_t reg) { return reg >= kI2cLo && reg <= kI2cHi; }
enum : uint32_t { kI2cScdcTmds = 1, kI2cScdcCheck = 2, kI2cSoft = 1 };

// Pipe 0 (the AOC, lit by the GOP) and the shared clock state: read before the first step (survey) and re-compared
// after every step; any change once something was written = halted.
struct Fp { uint32_t reg, mask; const char *name; };
static const Fp kFp[] = {
    {0x05001, 0xffffffffu, "OTG0_OTG_CONTROL"},
    {0x04FEA, 0xffffffffu, "OTG0_OTG_H_TOTAL"},
    {0x04FEF, 0xffffffffu, "OTG0_OTG_V_TOTAL"},
    {0x00140, 0x0000333bu, "OTG0_PIXEL_RATE_CNTL"},          // status/error fields masked
    {0x0012F, 0x00000007u, "OTG_PIXEL_RATE_DIV(OTG0)"},      // OTG1's K1/K2 may change
    {0x00124, 0x7f007f7fu, "DENTIST_DISPCLK_CNTL(dividers)"},
    {0x0564B, 0xfef7ffffu, "DIG1_DIG_FE_CNTL"},
    {0x05671, 0xffffffffu, "DIG1_DIG_BE_CNTL"},
    {0x05672, 0x00000001u, "DIG1_DIG_BE_EN_CNTL"},
    {0x09002, 0x0000000fu, "MPCC0_MPCC_OPP_ID"},
};
static constexpr unsigned kNFp = sizeof kFp / sizeof kFp[0];
static constexpr uint32_t kOtg0Control = 0x05001, kOtg0HTotal = 0x04FEA, kOtg0Frame = 0x0500C, kOtg1Control = 0x05081,
                          kOtg1Frame = 0x0508C, kHubp0Cntl = 0x03AB3, kHubp1Cntl = 0x03B8F, kOdm0Global = 0x04F8A,
                          kOdm1Global = 0x04F9A, kDig3FifoCtrl0 = 0x05851, kDig3BeEn = 0x05872, kDig3FeCntl = 0x0584B,
                          kDig3ClockPattern = 0x0584E, kSymclkd = 0x00163, kOtg1PixelRate = 0x00144;
static constexpr uint32_t kAocHTotal = 0x897;            // CEA 1080p60: the mode every cloned value was planned for
// b99: the AOC modes pipe 0 may be at (OTG0 H_TOTAL / V_TOTAL register values, rate). The GOP lights the first at every
// boot; only the pipe-0 engine (PhxPipe0.h, gated by debug.phxpipe0_enable) moves it to the second, and then re-baselines
// this engine's pipe-0 fingerprint itself (rebasePipe0) under the shared busy lock.
struct P0Mode { uint32_t hTotal, vTotal, hz; };
static const P0Mode kP0Modes[] = {{0x897, 0x464, 60}, {0x807, 0x44b, 144}};
static constexpr uint32_t kOtg0VTotalReg = 0x04FEF;
static constexpr uint32_t kUnderflow = 0x70000000u;      // HUBPn_DCHUBP_CNTL.HUBP_UNDERFLOW_STATUS
static constexpr uint32_t kOdmUnderflowBits = 0x2400u;   // OPTC_UNDERFLOW_OCCURRED_STATUS (sticky) | _CURRENT
static constexpr uint32_t kOdmSticky = 0x0400u, kOdmCurrent = 0x2000u;   // the two bits of kOdmUnderflowBits (dcn_3_2_0_sh_mask.h)

static constexpr unsigned kMaxSteps = 256, kMaxReq = 32;
static constexpr uint32_t kWDefer = 1, kWVerify = 2;   // W dmub[0]: read-back deferred / the unlock that verifies them
static constexpr uint32_t kLatchUs = 100000;            // the deferred registers must read back within 100 ms (6 frames)
enum Res { kDone = 0, kRefused = 1, kFailed = 2 };
static const char *const kLevelName[] = {"undo", "survey", "ping", "stream", "plane"};

struct Plan {
    const phx_s2_step *s; unsigned n;
    const phx_s2_req *req; unsigned nReq;
    const phx_s2_step *undo; unsigned nUndo;
    const uint32_t *noRestore; unsigned nNoRestore;
    uint32_t otg1Hz;   // b90: the OTG1 rate this plan sets (kPhxS2Otg1Hz; 0 = 60); OTG0 stays the GOP's 60 Hz
    // SCDC2: the DAL SetHardMinByFreq args {DISPCLK, DPPCLK} a switch to this plan sends when its DENTIST requirement is not
    // met (kPhxS2_165DalMin: Linux's 391/391 MHz at 165); null = the 331/331 MHz pair of the 144 plan
    const uint32_t *dalMin;
};
// b91: dal() = one DAL -> PMFW message (PhxBringup::dalMessage; returns RESP, 1 = OK, 0 = timeout, which latches
// dalDead); pmfwIdle() = DAL RESP and driver-mailbox RESP both non-zero (no message in flight). Either may be null.
struct Env {
    phxdmub::Io io; uint64_t vramBytes, fbMcBase; int dalDead;
    uint32_t (*dal)(void *c, uint32_t msg, uint32_t arg);
    int (*pmfwIdle)(void *c);
    int p0Pair;   // b103: the pipe-0 engine re-plans pipe 0 at 144 around a switch (phx_pipe0.cpp p0PairSwitch = phxp0::pairSwitch)
    // SCDC1: the DC_I2C engine's owner lock (the kext's edidBusy, shared with the EDID reads): take = 1 tries to take it
    // (1 = taken), take = 0 releases it. Null = every I2C step refused.
    int (*i2cLock)(void *c, int take);
};

struct State {
    int level;           // highest level passed this boot: 0 none, 1 survey, 2 ping, 3 stream, 4 plane
    int failed;          // a level or an undo failed: only survey (report) and undo remain
    int halted;          // pipe 0 / shared state changed after a write: nothing more until reboot
    unsigned touched;    // plan entries [0, touched) may have changed the hardware
    int lastLevel, lastRes, lastStep;   // lastStep: plan index, 1000 + undo index, -1 none
    char why[200];
    uint32_t before[kMaxSteps];         // each plan write's register value just before it
    uint32_t now[kMaxSteps];            // survey: each entry's register now
    uint32_t reqNow[kMaxReq], reqOk;    // last requirement check (bit i = kPhxS2Req[i] true)
    uint32_t reqChecked;                // bit i = checked
    int fpValid; uint32_t fp[kNFp], odm0Base, hubp0Base;
    unsigned stepsEqual, stepsW;
    uint32_t writes, rbMismatch, guards, undoRuns, runs;
    uint32_t otg0Hz, otg1Hz, post[6];   // post: DIG3 BE_EN, SYMCLKD, DIG3 FE, DIG3 CLOCK_PATTERN, DIG3 FIFO, OTG1 PIXEL_RATE
    phxdmub::St dmub;
    // b84: read-backs deferred to the unlock (W dmub[0] bit 0) of writes inside an OTG1 master-update-lock window:
    // double-buffered registers latch only at the VUPDATE after the unlock (box b83 22:29, DSCL1_LB_MEMORY_CTRL)
    struct Pend { uint32_t reg, val, mask; int id; } pend[kMaxSteps];
    unsigned nPend;
    // b91 mode switch: switching = between the undo of the old mode and the end (the b15 watchdog ignores pipe 1)
    const phx_s2_step *lit;   // the steps of the plan whose plane passed (null after an undo)
    int switching; uint32_t switchGen, switchOk, switchFell, switchDark, switchRefused, dalSent;
    // b97: stale ODM0 underflow stickies the switch preflight accepted into the pipe-0 baseline (count, the value seen)
    uint32_t p0StaleOk, p0StaleOdm0;
    uint32_t p0Rebases;   // b99: rebasePipe0 calls that passed
    // SCDC1: I2C steps run, soft failures passed over, last result (PhxDdc rc), the sink's version, the last SCDC byte
    // read, DC_I2C SW_STATUS, writes outside DC_I2C dropped by the step's Io (0 unless PhxDdc.h is broken)
    uint32_t i2cOps, i2cSoftFails, i2cSink, i2cLastVal, i2cStatus, i2cStray; int i2cLastRc;
};

inline uint32_t rd(const Env &e, uint32_t r) { return readAllowed(r) ? e.io.rd(e.io.c, r) : 0xdeadbeefu; }
inline void stepId(const Plan &p, int i, char *b, unsigned n) {
    if (i >= 1000 && static_cast<unsigned>(i - 1000) < p.nUndo) snprintf(b, n, "undo-%02d", i - 1000 + 1);
    else if (i >= 0 && static_cast<unsigned>(i) < p.n) snprintf(b, n, "S2.%u-%02u", p.s[i].stage, p.s[i].pad);
    else snprintf(b, n, "-");
}
inline int stageFirst(const Plan &p, unsigned stage) { for (unsigned i = 0; i < p.n; i++) if (p.s[i].stage == stage) return static_cast<int>(i); return -1; }
inline int stageEnd(const Plan &p, unsigned stage) { int e = -1; for (unsigned i = 0; i < p.n; i++) if (p.s[i].stage == stage) e = static_cast<int>(i) + 1; return e; }
inline bool noRestore(const Plan &p, uint32_t reg) { for (unsigned i = 0; i < p.nNoRestore; i++) if (p.noRestore[i] == reg) return true; return false; }

// Requirements of the stages in `mask` (bit s = stage s). Records now/ok; `why` = the first false one.
inline bool reqCheck(const Env &e, State &st, const Plan &p, unsigned mask, const char **why) {
    bool ok = true;
    for (unsigned i = 0; i < p.nReq && i < kMaxReq; i++) {
        const phx_s2_req &q = p.req[i];
        if (!(mask & (1u << q.stage))) continue;
        bool t;
        uint32_t v;
        if (q.cmp == 2) {
            const uint32_t w = rd(e, phxdmub::kInbox1Wptr), r = rd(e, phxdmub::kInbox1Rptr);
            v = w << 16 | (r & 0xffff);
            t = w == r;
        } else {
            v = rd(e, q.reg);
            // cmp 3 (b83): == val or == pad (PHYPLLD: the GOP's 0, or the 0x103 Linux's unplug and our undo leave)
            t = q.cmp == 1 ? (v & q.mask) <= q.val : ((v & q.mask) == q.val || (q.cmp == 3 && (v & q.mask) == q.pad));
        }
        st.reqNow[i] = v;
        st.reqChecked |= 1u << i;
        if (t) st.reqOk |= 1u << i; else st.reqOk &= ~(1u << i);
        if (!t && ok) { ok = false; if (why) *why = q.name; }
    }
    return ok;
}

// b99: the rate of the known AOC mode OTG0 runs (kP0Modes), 0 = none.
inline uint32_t p0ModeHz(const Env &e) {
    const uint32_t h = rd(e, kOtg0HTotal), v = rd(e, kOtg0VTotalReg);
    for (const P0Mode &m : kP0Modes) if (m.hTotal == h && m.vTotal == v) return m.hz;
    return 0;
}
// The absolute pipe-0 conditions every level starts from.
inline bool pipe0Abs(const Env &e, const char **why) {
    if (!(rd(e, kOtg0Control) & 1)) { *why = "OTG0 not enabled"; return false; }
    if (!p0ModeHz(e)) { *why = "OTG0 is not at a known AOC mode (0x897/0x464 1080p60 or, after the pipe-0 engine, 0x807/0x44b 1080p144)"; return false; }
    if (rd(e, kHubp0Cntl) & kUnderflow) { *why = "HUBP0 underflow status set"; return false; }
    return true;
}
// odm0Accept (b97, the switch preflight only): ODM0 underflow bits ignored on top of the baseline's
inline bool fpSame(const Env &e, const State &st, const char **why, uint32_t odm0Accept = 0) {
    if (!st.fpValid) { *why = "no pipe-0 baseline (run survey)"; return false; }
    for (unsigned k = 0; k < kNFp; k++)
        if ((rd(e, kFp[k].reg) & kFp[k].mask) != (st.fp[k] & kFp[k].mask)) { *why = kFp[k].name; return false; }
    if (rd(e, kOdm0Global) & kOdmUnderflowBits & ~(st.odm0Base | odm0Accept)) { *why = "ODM0 underflow bit newly set"; return false; }
    return pipe0Abs(e, why);
}
inline bool frameMoves(const Env &e, uint32_t reg) {
    const uint32_t f0 = rd(e, reg) & 0xffffff;
    for (unsigned t = 0; t < 30; t++) {          // up to 60 ms: > 3 frames at 60 Hz
        e.io.delayUs(e.io.c, 2000);
        if ((rd(e, reg) & 0xffffff) != f0) return true;
    }
    return false;
}
inline bool guardFull(const Env &e, State &st, const char **why) {
    st.guards++;
    if (!fpSame(e, st, why)) return false;
    if (!frameMoves(e, kOtg0Frame)) { *why = "OTG0 frame counter stopped"; return false; }
    return true;
}
inline void measureHz(const Env &e, State &st) {
    const uint32_t a0 = rd(e, kOtg0Frame), a1 = rd(e, kOtg1Frame);
    e.io.delayUs(e.io.c, 1000000);
    st.otg0Hz = (rd(e, kOtg0Frame) - a0) & 0xffffff;
    st.otg1Hz = (rd(e, kOtg1Frame) - a1) & 0xffffff;
}

inline int fail(State &st, int res, const char *what, const char *detail) {
    snprintf(st.why, sizeof st.why, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    st.lastRes = res;
    if (res == kFailed) st.failed = 1;
    return res;
}

// SCDC1: the Io of an I2C step: reads through the engine's rd (allow rules), writes only inside DC_I2C (any other is
// dropped and counted), delays through the engine.
struct I2cGate { const Env *e; uint32_t stray; };
inline uint32_t i2cGateRd(void *c, uint32_t r) { return rd(*static_cast<I2cGate *>(c)->e, r); }
inline void i2cGateWr(void *c, uint32_t r, uint32_t v) {
    I2cGate *g = static_cast<I2cGate *>(c);
    if (!i2cWriteAllowed(r)) { g->stray++; return; }
    g->e->io.wr(g->e->io.c, r, v);
}
inline void i2cGateDelay(void *c, uint32_t us) { const Env *e = static_cast<I2cGate *>(c)->e; e->io.delayUs(e->io.c, us); }
// The static rules of one I2C entry (staticCheck).
inline bool i2cStepOk(const phx_s2_step &s, const char **why) {
    if (s.dmub[0] != kI2cScdcTmds && s.dmub[0] != kI2cScdcCheck) { *why = "an I2C step of an unknown kind"; return false; }
    if (s.dmub[1] > 4) { *why = "an I2C step on a DDC line that does not exist"; return false; }
    if (s.dmub[2] != phxddc::kAddrScdc) { *why = "an I2C step not to SCDC (0x54)"; return false; }
    if (s.dmub[0] == kI2cScdcTmds && s.val != 0 && s.val != 1 && s.val != 3) { *why = "an SCDC TMDS_Config value not 0, 1 or 3"; return false; }
    if (s.dmub[0] == kI2cScdcCheck && (s.dmub[3] > 0xff || s.mask > 0xff || s.val > 0xff)) { *why = "an SCDC check outside one byte"; return false; }
    if (s.dmub[4] & ~kI2cSoft) { *why = "an I2C step with unknown flags"; return false; }
    return true;
}
// One I2C entry: under the DC_I2C lock, one PhxDdc.h SCDC call. An attempted step counts as applied (touched) even when
// it fails: the sink may have taken part of it, so the undo's TMDS_Config = 0 still runs.
inline bool execI2c(const Env &e, State &st, const phx_s2_step &s, int idx, bool plan, const char **why) {
    if (!i2cStepOk(s, why)) return false;
    if (!e.i2cLock) { *why = "I2C step without the DC_I2C lock (Env.i2cLock)"; return false; }
    if (!e.i2cLock(e.io.c, 1)) { *why = "DC_I2C busy (an EDID read holds it)"; return false; }
    I2cGate g{&e, 0};
    const phxddc::Io io{&g, i2cGateRd, i2cGateWr, i2cGateDelay};
    phxddc::Diag d{};
    uint8_t v = 0, sv = 0;
    int rc;
    if (s.dmub[0] == kI2cScdcTmds) rc = phxddc::scdcWriteTmdsConfig(io, s.dmub[1], static_cast<uint8_t>(s.val), &d, &sv);
    else rc = phxddc::scdcRead(io, s.dmub[1], static_cast<uint8_t>(s.dmub[3]), &v, &d);
    e.i2cLock(e.io.c, 0);
    if (plan) st.touched = static_cast<unsigned>(idx) + 1;
    st.i2cOps++; st.i2cLastRc = rc; st.i2cStatus = d.status; st.i2cStray += g.stray;
    if (s.dmub[0] == kI2cScdcTmds) st.i2cSink = sv; else st.i2cLastVal = v;
    if (g.stray) { *why = "an I2C step tried a write outside DC_I2C (dropped)"; return false; }
    const char *w = nullptr;
    if (rc != phxddc::kOk) w = rc == phxddc::kErrNack ? "SCDC NACK (no HDMI 2.0 sink answering at 0x54)" : rc == phxddc::kErrTimeout ? "SCDC timeout"
                               : rc == phxddc::kErrBusy ? "DC_I2C engine busy" : rc == phxddc::kErrNotGranted ? "DC_I2C owned by the hardware side" : "SCDC call refused";
    else if (s.dmub[0] == kI2cScdcCheck && (v & s.mask) != s.val) w = "SCDC check failed";
    if (!w) return true;
    if (s.dmub[4] & kI2cSoft) { st.i2cSoftFails++; return true; }
    *why = w;
    return false;
}
inline bool hasI2c(const Plan &p) {
    for (unsigned i = 0; i < p.n + p.nUndo; i++) if ((i < p.n ? p.s[i] : p.undo[i - p.n]).op == PHX_S2_I2C) return true;
    return false;
}

// One table entry. plan = true: an entry of kPhxS2 at index idx (records before/touched); false: an undo entry.
inline bool execOne(const Env &e, State &st, const Plan &, const phx_s2_step &s, int idx, bool plan, const char **why) {
    switch (s.op) {
    case PHX_S2_W: {
        if (!writeAllowed(s.reg)) { *why = "write outside the pipe-1 allow-list (not written)"; return false; }
        const uint32_t b = rd(e, s.reg);
        if (plan) st.before[idx] = b;
        e.io.wr(e.io.c, s.reg, s.val);
        st.writes++;
        if (plan) st.touched = static_cast<unsigned>(idx) + 1;
        if (s.dmub[0] & kWDefer) {   // double-buffered: compared after the unlock; a later write of the register replaces it
            unsigned k = 0;
            while (k < st.nPend && st.pend[k].reg != s.reg) k++;
            if (k == st.nPend) { if (st.nPend >= kMaxSteps) { *why = "too many deferred read-backs"; return false; } st.nPend++; }
            st.pend[k] = {s.reg, s.val, s.mask, plan ? idx : 1000 + idx};
            return true;
        }
        const uint32_t rb = rd(e, s.reg);
        if ((rb ^ s.val) & s.mask) { st.rbMismatch++; *why = "read-back mismatch"; return false; }
        if (s.dmub[0] & kWVerify) {   // the unlock: wait for the latch, then every deferred register under its mask
            for (uint32_t us = 0;; us += 2000) {
                unsigned bad = st.nPend;
                for (unsigned k = 0; k < st.nPend && bad == st.nPend; k++)
                    if ((rd(e, st.pend[k].reg) ^ st.pend[k].val) & st.pend[k].mask) bad = k;
                if (bad == st.nPend) break;
                if (us >= kLatchUs) {
                    st.rbMismatch++;
                    st.lastStep = st.pend[bad].id;
                    *why = "deferred read-back mismatch after the unlock (100 ms)";
                    st.nPend = 0;
                    return false;
                }
                e.io.delayUs(e.io.c, 2000);
            }
            st.nPend = 0;
        }
        return true;
    }
    case PHX_S2_DMUB: {
        const char *w = nullptr;
        const int rc = phxdmub::submit(e.io, st.dmub, e.vramBytes, s.dmub, &w);
        if (plan && (rc == phxdmub::kOk || rc == phxdmub::kTimeout)) st.touched = static_cast<unsigned>(idx) + 1;
        if (rc != phxdmub::kOk) { *why = w ? w : phxdmub::rcName(rc); return false; }
        return true;
    }
    case PHX_S2_POLL: {
        if (!readAllowed(s.reg)) { *why = "poll of a forbidden register"; return false; }
        uint32_t us = 0;
        const uint32_t budget = s.dmub[0] ? s.dmub[0] : 1000;
        while ((rd(e, s.reg) & s.mask) != s.val) {
            if (us >= budget) { *why = "poll timed out"; if (plan) st.touched = static_cast<unsigned>(idx) + 1; return false; }
            e.io.delayUs(e.io.c, 10);
            us += 10;
        }
        break;
    }
    case PHX_S2_DELAY: e.io.delayUs(e.io.c, s.val); break;
    case PHX_S2_I2C: return execI2c(e, st, s, idx, plan, why);
    case PHX_S2_CHECK:
        if (!readAllowed(s.reg) || (rd(e, s.reg) & s.mask) != s.val) { *why = "check failed"; if (plan) st.touched = static_cast<unsigned>(idx) + 1; return false; }
        break;
    default: *why = "unknown op"; return false;
    }
    if (plan) st.touched = static_cast<unsigned>(idx) + 1;
    return true;
}

// Runs plan entries [first, end); after each: fingerprint (writes), full guard (DMUB).
inline int execRange(const Env &e, State &st, const Plan &p, int first, int end) {
    for (int i = first; i < end; i++) {
        const char *w = nullptr;
        st.lastStep = i;
        if (!execOne(e, st, p, p.s[i], i, true, &w)) {
            char id[16]; stepId(p, st.lastStep, id, sizeof id);
            const phx_s2_step &fs = st.lastStep >= 0 && static_cast<unsigned>(st.lastStep) < p.n ? p.s[st.lastStep] : p.s[i];
            char d[96]; snprintf(d, sizeof d, "%s %s", id, fs.name);
            return fail(st, kFailed, w, d);
        }
        if (p.s[i].op == PHX_S2_DMUB ? !guardFull(e, st, &w) : !fpSame(e, st, &w)) {
            st.halted = 1;
            char id[16]; stepId(p, i, id, sizeof id);
            char d[120]; snprintf(d, sizeof d, "pipe 0 / shared state changed after %s %s: %s", id, p.s[i].name, w);
            return fail(st, kFailed, "HALTED", d);
        }
    }
    if (st.nPend) { st.nPend = 0; return fail(st, kFailed, "deferred read-backs never verified", "lock window without its unlock"); }
    return kDone;
}

// SCDC2: a plan's own DAL hard mins: exactly {DISPCLK, DPPCLK} SetHardMinByFreq args, 331-600 MHz (never below the 144 pair).
inline bool dalMinOk(const Plan &p) {
    if (!p.dalMin) return true;
    const uint32_t a = p.dalMin[0], b = p.dalMin[1];
    return a >> 16 == 8 && b >> 16 == 9 && (a & 0xffff) >= 331 && (a & 0xffff) <= 600 && (b & 0xffff) >= 331 && (b & 0xffff) <= 600;
}
inline bool staticCheck(const Plan &p, const char **why) {
    if (p.n > kMaxSteps || p.nReq > kMaxReq) { *why = "table larger than the engine"; return false; }
    if (!dalMinOk(p)) { *why = "the plan's DAL hard mins are not a DISPCLK + DPPCLK pair of 331-600 MHz"; return false; }
    unsigned last = 1;
    for (unsigned i = 0; i < p.n + p.nUndo; i++) {
        const phx_s2_step &s = i < p.n ? p.s[i] : p.undo[i - p.n];
        const char *w = nullptr;
        if (i < p.n) { if (s.stage < last || s.stage > 3) { *why = "plan stages out of order"; return false; } last = s.stage; }
        if (s.op == PHX_S2_W && !writeAllowed(s.reg)) { *why = i < p.n ? "a plan write is outside the allow-list" : "an undo write is outside the allow-list"; return false; }
        if ((s.op == PHX_S2_POLL || s.op == PHX_S2_CHECK) && !readAllowed(s.reg)) { *why = "a read of a forbidden register"; return false; }
        if (s.op == PHX_S2_DMUB && !phxdmub::cmdAllowed(s.dmub, &w)) { *why = w; return false; }
        if (s.op == PHX_S2_I2C && !i2cStepOk(s, &w)) { *why = w; return false; }
        if (s.op < PHX_S2_W || s.op > PHX_S2_I2C) { *why = "unknown op"; return false; }
    }
    for (unsigned list = 0; list < 2; list++) {   // b84: every deferred read-back is verified by an unlock of its own stage
        const phx_s2_step *L = list ? p.undo : p.s;
        const unsigned n = list ? p.nUndo : p.n;
        bool open = false;
        for (unsigned i = 0; i < n; i++) {
            if (i && L[i].stage != L[i - 1].stage && open) { *why = "deferred read-backs without an unlock in their stage"; return false; }
            if (L[i].op == PHX_S2_W && (L[i].dmub[0] & kWDefer)) open = true;
            if (L[i].op == PHX_S2_W && (L[i].dmub[0] & kWVerify)) open = false;
        }
        if (open) { *why = "deferred read-backs without an unlock"; return false; }
    }
    for (unsigned j = 0; j < p.nUndo; j++) {   // every gate names a plan step
        bool found = p.undo[j].stage == 0;
        for (unsigned i = 0; i < p.n && !found; i++) found = p.s[i].stage == p.undo[j].stage && p.s[i].pad == p.undo[j].pad;
        if (!found) { *why = "an undo gate names no plan step"; return false; }
    }
    return true;
}

// b87 (M7 S4b T4): head 1 at another size changes only plane values (the generated phx_s2_plan_head1_1920.inc:
// the pitch, both viewports, the recout). Copies p's steps into dst (cap entries) and patches those entries. Refuses an
// entry that is not a stage-3 write of the same register on the allow-list: the patched plan keeps every check of p.
inline bool applyAlt(phx_s2_step *dst, unsigned cap, const Plan &p, const phx_s2_alt *a, unsigned na, const char **why) {
    if (p.n > cap) { *why = "plan larger than the copy"; return false; }
    for (unsigned i = 0; i < p.n; i++) dst[i] = p.s[i];
    for (unsigned k = 0; k < na; k++) {
        const phx_s2_alt &x = a[k];
        if (x.idx >= p.n || dst[x.idx].op != PHX_S2_W || dst[x.idx].stage != 3 || dst[x.idx].reg != x.reg || !writeAllowed(x.reg)) {
            *why = "a head-1 entry is not a plane write of its register"; return false;
        }
        dst[x.idx].val = x.val;
    }
    return true;
}
// Whether the plane values may still change: no plane (stage 3) entry applied since the last undo.
inline bool planeUntouched(const State &st, const Plan &p) { const int f = stageFirst(p, 3); return f >= 0 && st.touched <= static_cast<unsigned>(f); }

inline int survey(const Env &e, State &st, const Plan &p) {
    const char *w = nullptr;
    phxdmub::Ring r;
    if (!staticCheck(p, &w)) return fail(st, kFailed, "survey: static check", w);
    st.stepsEqual = st.stepsW = 0;
    for (unsigned i = 0; i < p.n; i++) {
        const phx_s2_step &s = p.s[i];
        st.now[i] = (s.op == PHX_S2_W || s.op == PHX_S2_POLL || s.op == PHX_S2_CHECK) ? rd(e, s.reg) : 0;
        if (s.op == PHX_S2_W) { st.stepsW++; st.stepsEqual += st.now[i] == s.val; }
    }
    const bool fresh = st.touched == 0;
    const bool reqs = reqCheck(e, st, p, 0xe, &w);
    const char *rw = w;
    if (e.dalDead) return fail(st, kFailed, "survey", "debug.phxdal_dead = 1");
    if (st.dmub.dead) return fail(st, kFailed, "survey", "debug.phxdmub_dead = 1");
    if ((static_cast<uint64_t>(rd(e, phxdmub::kDcnVmFbBase) & 0xffffff) << 24) != e.fbMcBase) return fail(st, kFailed, "survey", "DCN FB base != the kext's FB base");
    if (!phxdmub::ringGeometry(e.io, e.vramBytes, &r, &w)) return fail(st, kFailed, "survey: ring", w);
    if (!fresh) { snprintf(st.why, sizeof st.why, "survey: report only (%u entries applied; requirements reflect them)", st.touched); st.lastRes = kDone; return kDone; }
    if (!reqs) return fail(st, kFailed, "survey: requirement false", rw);
    if (!pipe0Abs(e, &w)) return fail(st, kFailed, "survey: pipe 0", w);
    if (!frameMoves(e, kOtg0Frame)) return fail(st, kFailed, "survey: pipe 0", "OTG0 frame counter not moving");
    for (unsigned k = 0; k < kNFp; k++) st.fp[k] = rd(e, kFp[k].reg);
    st.odm0Base = rd(e, kOdm0Global) & kOdmUnderflowBits;
    st.hubp0Base = rd(e, kHubp0Cntl);
    st.fpValid = 1;
    st.failed = 0;   // nothing applied: an earlier failed survey or a refused level leaves nothing to undo
    if (st.level < 1) st.level = 1;
    snprintf(st.why, sizeof st.why, "survey passed: %u requirements, ring VRAM 0x%llx size 0x%x WPTR 0x%x, %u/%u writes already equal",
             p.nReq, static_cast<unsigned long long>(r.vram), r.size, r.wptr, st.stepsEqual, st.stepsW);
    st.lastRes = kDone;
    return kDone;
}

inline int undo(const Env &e, State &st, const Plan &p) {
    const char *w = nullptr;
    if (!st.touched) return fail(st, kRefused, "undo", "nothing applied in this boot");
    st.undoRuns++;
    for (unsigned j = 0; j < p.nUndo; j++) {
        const phx_s2_step &u = p.undo[j];
        if (u.stage) {
            int g = -1;
            for (unsigned i = 0; i < p.n && g < 0; i++) if (p.s[i].stage == u.stage && p.s[i].pad == u.pad) g = static_cast<int>(i);
            if (g < 0 || st.touched <= static_cast<unsigned>(g)) continue;
        }
        st.lastStep = 1000 + static_cast<int>(j);
        if (!execOne(e, st, p, u, static_cast<int>(j), false, &w)) {
            char id[16]; stepId(p, st.lastStep, id, sizeof id);
            const unsigned fj = st.lastStep >= 1000 ? static_cast<unsigned>(st.lastStep - 1000) : j;
            char d[96]; snprintf(d, sizeof d, "%s %s", id, fj < p.nUndo ? p.undo[fj].name : u.name);
            return fail(st, kFailed, w, d);
        }
        if (u.op == PHX_S2_DMUB ? !guardFull(e, st, &w) : !fpSame(e, st, &w)) {
            st.halted = 1;
            char d[120]; snprintf(d, sizeof d, "pipe 0 / shared state changed after undo-%02u %s: %s", j + 1, u.name, w);
            return fail(st, kFailed, "HALTED", d);
        }
    }
    unsigned restored = 0;
    for (int i = static_cast<int>(st.touched) - 1; i >= 0; i--) {   // restore pass: first write of each register, reverse order
        const phx_s2_step &s = p.s[i];
        if (s.op != PHX_S2_W || noRestore(p, s.reg)) continue;
        bool first = true;
        for (int k = 0; k < i && first; k++) if (p.s[k].op == PHX_S2_W && p.s[k].reg == s.reg) first = false;
        if (!first) continue;
        st.lastStep = i;
        const phx_s2_step rs = {PHX_S2_W, s.stage, s.pad, s.reg, st.before[i], s.mask, {0}, s.name};
        if (!execOne(e, st, p, rs, i, false, &w)) {
            char d[96], id[16]; stepId(p, i, id, sizeof id); snprintf(d, sizeof d, "restore of %s %s", id, s.name);
            return fail(st, kFailed, w, d);
        }
        if (!fpSame(e, st, &w)) {
            st.halted = 1;
            char d[120]; snprintf(d, sizeof d, "pipe 0 / shared state changed after restoring %s: %s", s.name, w);
            return fail(st, kFailed, "HALTED", d);
        }
        restored++;
    }
    if (!guardFull(e, st, &w)) { st.halted = 1; return fail(st, kFailed, "HALTED after undo", w); }
    if (!reqCheck(e, st, p, 0x6, &w))
        return fail(st, kFailed, "undo done but pipe 1 is not dark again (reboot)", w);
    st.level = 0; st.touched = 0; st.failed = 0; st.fpValid = 0; st.lit = nullptr;
    snprintf(st.why, sizeof st.why, "undo clean: %u registers restored, pipe 1 dark again; the ladder restarts at survey", restored);
    st.lastRes = kDone;
    return kDone;
}

// The sysctl write. Returns kDone / kRefused / kFailed; st.why says why.
inline int run(const Env &e, State &st, const Plan &p, int level) {
    st.runs++;
    st.nPend = 0;
    st.lastLevel = level;
    const char *w = nullptr;
    if (st.halted) return fail(st, kRefused, "halted", "pipe 0 changed earlier in this boot: reboot");
    if (st.dmub.dead) return fail(st, kRefused, "debug.phxdmub_dead", "a DMUB command timed out earlier in this boot: reboot (cold boot if anything misbehaves)");
    if (level == 1) return survey(e, st, p);
    if (level == 0) {
        if (e.dalDead) return fail(st, kRefused, "undo", "debug.phxdal_dead = 1");
        return undo(e, st, p);
    }
    if (level < 0 || level > 4) return fail(st, kRefused, "levels", "0 undo, 1 survey, 2 ping, 3 stream, 4 plane");
    if (e.dalDead) return fail(st, kRefused, kLevelName[level], "debug.phxdal_dead = 1");
    if (st.failed) return fail(st, kRefused, kLevelName[level], "a level failed in this boot: undo (0) or reboot");
    if (st.level != level - 1) {
        if (st.level >= level) snprintf(st.why, sizeof st.why, "%s already passed in this boot (reached: %s); undo (0) first to run it again", kLevelName[level], kLevelName[st.level]);
        else snprintf(st.why, sizeof st.why, "%s needs %s passed first (reached: %s)", kLevelName[level], kLevelName[level - 1], kLevelName[st.level]);
        st.lastRes = kRefused;
        return kRefused;
    }
    const unsigned stage = static_cast<unsigned>(level - 1);
    const int first = stageFirst(p, stage), end = stageEnd(p, stage);
    if (first < 0 || st.touched != static_cast<unsigned>(first)) return fail(st, kRefused, kLevelName[level], "plan position does not match the level");
    if (!reqCheck(e, st, p, 0x2 | (1u << stage), &w)) return fail(st, kRefused, "requirement false", w);
    if (!fpSame(e, st, &w)) {
        if (st.touched) { st.halted = 1; return fail(st, kFailed, "HALTED: pipe 0 changed before the level", w); }
        return fail(st, kRefused, "pipe 0 changed since the survey (re-run survey)", w);
    }
    const int r = execRange(e, st, p, first, end);
    if (r != kDone) return r;
    if (!guardFull(e, st, &w)) { st.halted = 1; return fail(st, kFailed, "HALTED at the end of the level", w); }
    if (level == 3 || level == 4) measureHz(e, st);
    if (level == 3) {
        st.post[0] = rd(e, kDig3BeEn); st.post[1] = rd(e, kSymclkd); st.post[2] = rd(e, kDig3FeCntl);
        st.post[3] = rd(e, kDig3ClockPattern); st.post[4] = rd(e, kDig3FifoCtrl0); st.post[5] = rd(e, kOtg1PixelRate);
        if (st.post[4] & 0x30000000u) return fail(st, kFailed, "stream post-check", "DIG3_DIG_FIFO_CTRL0 DIG_FIFO_ERROR set");
        if (st.post[5] & 0xc000u) return fail(st, kFailed, "stream post-check", "OTG1_PIXEL_RATE_CNTL DIO FIFO error set");
    }
    if (level == 4) {
        if (rd(e, kHubp1Cntl) & kUnderflow) return fail(st, kFailed, "plane post-check", "HUBP1 underflow");
        if (rd(e, kOdm1Global) & 0x400u) return fail(st, kFailed, "plane post-check", "OPTC1 underflow");
    }
    const uint32_t want1 = p.otg1Hz ? p.otg1Hz : 60, lo1 = want1 * 92 / 100, hi1 = want1 * 109 / 100;   // b90: per plan (60: 55-65, 144: 132-156)
    const uint32_t want0 = p0ModeHz(e) ? p0ModeHz(e) : 60, lo0 = want0 * 92 / 100, hi0 = want0 * 109 / 100;   // b99: OTG0's mode
    if ((level == 3 || level == 4) && (st.otg1Hz < lo1 || st.otg1Hz > hi1 || st.otg0Hz < lo0 || st.otg0Hz > hi0)) {
        char d[96]; snprintf(d, sizeof d, "OTG0 %u Hz, OTG1 %u Hz (want %u-%u and %u-%u)", st.otg0Hz, st.otg1Hz, lo0, hi0, lo1, hi1);
        return fail(st, kFailed, "frame rate post-check", d);
    }
    st.level = level;
    if (level == 4) st.lit = p.s;
    if (level == 2)
        snprintf(st.why, sizeof st.why, "ping passed: RPTR caught up in %u us, reply header 0x%08x (ret_status %u)", st.dmub.lastUs, st.dmub.reply[0], (st.dmub.reply[0] >> 16) & 1);
    else
        snprintf(st.why, sizeof st.why, "%s passed: OTG0 %u Hz, OTG1 %u Hz", kLevelName[level], st.otg0Hz, st.otg1Hz);
    st.lastRes = kDone;
    return kDone;
}

#define PHXP2_OUT(...) do { if (o < n - 1) { const int k_ = snprintf(b + o, static_cast<size_t>(n - o), __VA_ARGS__); o += k_ > 0 ? k_ : 0; if (o > n - 1) o = n - 1; } } while (0)

// ---- b91 (mode switch): two pieces the boot scripts did, in the kext
// OTG1's vblank onto OTG0's when both pipes run the same timing (port of tools/phxctl/m7_phase.sh; the root cause it
// fixes: a fixed offset larger than the ~45-line vblank never lets the DCN grant the DRAM p-state change
// and every PMFW main-loop message stalls). Shortens OTG1's V_TOTAL by 5 lines for n OTG1 frames until OTG1 trails OTG0 by
// <= tol lines (or leads by <= tol), restores V_TOTAL, then clears the OPTC1 underflow sticky the short frames set (the
// plane's check reads it and refused the plane). Writes only OTG1_OTG_V_TOTAL and ODM1_OPTC_INPUT_GLOBAL_CONTROL.
static constexpr uint32_t kOtg0Pos = 0x0500A, kOtg1Pos = 0x0508A, kOtg0VTotal = 0x04FEF, kOtg1VTotal = 0x0506F;
enum PhaseRes { kPhaseAligned = 0, kPhaseNotNeeded = 1, kPhaseFailed = 2 };
inline uint32_t phaseLag(const Env &e, uint32_t lines) {   // lines OTG1 trails OTG0, 0..lines-1
    const uint32_t a = (rd(e, kOtg0Pos) & 0x7fff) % lines, b = (rd(e, kOtg1Pos) & 0x7fff) % lines;
    return (a + lines - b) % lines;
}
inline bool waitOtg1Frames(const Env &e, uint32_t n) {
    const uint32_t f0 = rd(e, kOtg1Frame) & 0xffffff;
    for (uint32_t i = 0; i < n * 20 + 100; i++) {   // 1 ms polls; a 60 Hz frame is ~17
        if (((rd(e, kOtg1Frame) - f0) & 0xffffff) >= n) return true;
        e.io.delayUs(e.io.c, 1000);
    }
    return false;
}
inline int phaseAlign(const Env &e, State &st, uint32_t tol, unsigned *tries, uint32_t *lagOut, const char **why) {
    const uint32_t vt0 = rd(e, kOtg0VTotal), vt1 = rd(e, kOtg1VTotal);
    *tries = 0; *lagOut = 0;
    if (vt0 != vt1) return kPhaseNotNeeded;   // different timings: the phases drift, the vblanks meet periodically
    if (!(rd(e, kOtg1Control) & 1)) { *why = "OTG1 not running"; return kPhaseFailed; }
    const uint32_t lines = vt1 + 1;
    bool shifted = false, ok = false;
    for (unsigned t = 0; t <= 3; t++) {
        const uint32_t lag = phaseLag(e, lines);
        *lagOut = lag;
        if (lag <= tol || lag >= lines - tol) { ok = true; break; }
        if (t == 3) break;
        const uint32_t n = (lag + 2) / 5;
        e.io.wr(e.io.c, kOtg1VTotal, vt1 - 5);
        const bool counted = waitOtg1Frames(e, n);
        e.io.wr(e.io.c, kOtg1VTotal, vt1);
        shifted = true; *tries = t + 1;
        if (!counted) { *why = "OTG1 frame counter did not move"; break; }
        if (!fpSame(e, st, why)) { st.halted = 1; return kPhaseFailed; }
    }
    if (rd(e, kOtg1VTotal) != vt1) e.io.wr(e.io.c, kOtg1VTotal, vt1);
    if (shifted) {
        waitOtg1Frames(e, 2);
        const uint32_t v = rd(e, kOdm1Global);
        e.io.wr(e.io.c, kOdm1Global, (v & ~0x400u) | 0x1000u);
    }
    if (!ok && !*why) *why = "not aligned after 3 tries";
    return ok ? kPhaseAligned : kPhaseFailed;
}
// DET 9/9/0/0 = Linux's two-display split (18 / streams, dcn32_determine_det_override; port of m7_boot.sh det99, which
// keeps the PMFW answering at 144). DET2/3 -> 0 only while HUBP2/3 clocks are off, then DET0/1 -> 9; each write waits for
// SIZE_CURRENT; the CRB budget (DET sizes + COMPBUF) never exceeds 20; COMPBUF is never written. Own allow-list.
static constexpr uint32_t kCompbufCtrl = 0x0397A, kDetCtrl[4] = {0x0397B, 0x0397C, 0x0397D, 0x0397E},
                          kHubp2ClkCntl = 0x03C6C, kHubp3ClkCntl = 0x03D48;
inline bool detIs(const Env &e, const uint32_t want[4]) {
    for (unsigned k = 0; k < 4; k++) if ((rd(e, kDetCtrl[k]) & 0x1f) != want[k]) return false;
    return true;
}
inline bool detTwoPipe(const Env &e, State &st, const char **why) {
    static const uint32_t want[4] = {9, 9, 0, 0};
    if (detIs(e, want)) return true;
    if ((rd(e, kHubp2ClkCntl) & 1) || (rd(e, kHubp3ClkCntl) & 1)) { *why = "DET: HUBP2/3 clock on (pipes 2/3 in use)"; return false; }
    static const unsigned order[4] = {2, 3, 0, 1};
    for (unsigned i = 0; i < 4; i++) {
        const unsigned k = order[i];
        uint32_t sum = rd(e, kCompbufCtrl) & 0x1f;
        for (unsigned j = 0; j < 4; j++) sum += j == k ? want[k] : (rd(e, kDetCtrl[j]) & 0x1f);
        if (sum > 20) { *why = "DET: CRB budget would exceed 20 segments"; return false; }
        const uint32_t v = rd(e, kDetCtrl[k]);
        e.io.wr(e.io.c, kDetCtrl[k], (v & ~0x1fu) | want[k]);
        st.writes++;
        unsigned t = 0;
        while (((rd(e, kDetCtrl[k]) >> 8) & 0x1f) != want[k] && t++ < 50) e.io.delayUs(e.io.c, 1000);
        if (t > 50) { *why = "DET: SIZE_CURRENT never followed"; return false; }
    }
    if (rd(e, kCompbufCtrl) >> 31) { *why = "DET: COMPBUF config error"; return false; }
    return true;
}

// ---- b91 mode switch. Relights pipe 1 with another plan through the proven ladder: preflight (reads
// only) -> DET 9/9/0/0 and the DAL hard mins while the old mode is lit -> undo -> survey/ping/stream with the target ->
// phase (same V_TOTALs only) -> plane. A non-latched failure after the undo relights the previous plan (FellBack); a
// latch, or a fallback that fails too, leaves pipe 1 dark (Dark: head 1 offline until reboot). No DAL/SMU message
// after the undo. The caller selects both plans first (they must not share an alt-steps buffer).
enum SwRes { kSwDone = 0, kSwSame = 1, kSwRefused = 2, kSwFellBack = 3, kSwDark = 4 };
static const char *const kSwName[] = {"done", "same", "refused", "fell back", "dark"};
// b92 (a b91 switch failed on this requirement): the plans' plane requires DCHUBBUB_ARB_DRAM_STATE_CNTL & 3 == 2 (stutter
// forced off, the state the boot ladder runs in); the b15 two-pipe branch allows stutter (0x1000) after the boot. The
// switch forces it off from just before the undo and puts the saved value back once pipe 1 is clean for 100 ms; on dark
// it stays forced off (the safe side, what the b15 watchdog does on an underflow). Own allow-list: this one register.
static constexpr uint32_t kArbDramStateCntl = 0x039BC;
// The target's requirements that do not depend on pipe 1 being dark: checked in the preflight so that a refusal never
// reaches the undo (S2.1 DMCUB entries by stage, plus these registers).
static constexpr uint32_t kSwGlobalReq[] = {0x053D4 /*HPD0_DC_HPD_INT_STATUS*/, 0x05001 /*OTG0_OTG_CONTROL*/, 0x0564B /*DIG1_DIG_FE_CNTL*/,
                                            0x09002 /*MPCC0_MPCC_OPP_ID*/, 0x03555 /*DC_IP_REQUEST_CNTL*/, 0x03935 /*DCN_VM_FB_LOCATION_BASE*/};
inline bool swGlobalReqOk(const Env &e, const Plan &p, const char **why) {
    for (unsigned i = 0; i < p.nReq; i++) {
        const phx_s2_req &q = p.req[i];
        bool global = q.stage == 1;
        for (uint32_t r : kSwGlobalReq) global |= q.reg == r && q.stage != 1;
        if (!global) continue;
        bool t;
        if (q.cmp == 2) t = rd(e, phxdmub::kInbox1Wptr) == rd(e, phxdmub::kInbox1Rptr);
        else {
            const uint32_t v = rd(e, q.reg);
            t = q.cmp == 1 ? (v & q.mask) <= q.val : ((v & q.mask) == q.val || (q.cmp == 3 && (v & q.mask) == q.pad));
        }
        if (!t) { *why = q.name; return false; }
    }
    return true;
}
static constexpr uint32_t kDentist = 0x00124, kDalSetHardMinByFreq = 9, kDalMinDispclk331 = 0x0008014b, kDalMinDppclk331 = 0x0009014b;
struct Sw {
    const Plan *cur, *target;
    unsigned failAt;   // test only: a failure injected at stage n (5 stream of the target, 7 plane of the target)
    int res, stage;    // stage: 0 preflight, 1 DET, 2 DAL, 3 undo, 4 survey/ping, 5 stream, 6 phase, 7 plane, 8 stickies, 9 fallback
    uint32_t phaseLag; unsigned phaseTries;
    uint32_t arbSaved; bool arbForced;   // b92: ARB_DRAM_STATE_CNTL before the switch, and whether it is forced off now
    char why[160];
};
inline int swEnd(State &st, Sw &sw, int res, int stage, const char *what, const char *detail) {
    sw.res = res; sw.stage = stage;
    snprintf(sw.why, sizeof sw.why, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (res != kSwSame) st.switching = 0;
    if (res == kSwDone) st.switchOk++; else if (res == kSwFellBack) st.switchFell++;
    else if (res == kSwDark) st.switchDark++; else if (res == kSwRefused) st.switchRefused++;
    return res;
}
// The DENTIST dividers a plan's requirements accept (0 = no DENTIST requirement).
inline bool dentistFits(const Plan &p, uint32_t v) {
    for (unsigned i = 0; i < p.nReq; i++)
        if (p.req[i].reg == kDentist && p.req[i].cmp == 1 && (v & p.req[i].mask) > p.req[i].val) return false;
    return true;
}
// After the DAL hard mins: only the DENTIST entry of the pipe-0 baseline may differ (strict: every divider <= 0x33;
// a refused second message leaves DISPCLK raised and DPPCLK not, 0x547f3333). SCDC2: 0x33 stays the strict bound
// for every target (a raised clock never has a larger divider); a 165 target's own <= 0x2b (Linux's 0x2b792b2b) is then
// checked by dentistFits as a plain refusal before the undo, never a latch.
inline bool rebaseDentist(const Env &e, State &st, bool strict, const char **why) {
    if (!st.fpValid) { *why = "no pipe-0 baseline"; return false; }
    for (unsigned k = 0; k < kNFp; k++) {
        const uint32_t v = rd(e, kFp[k].reg);
        if (kFp[k].reg == kDentist) {
            if (strict && ((v & 0x7f) > 0x33 || ((v >> 8) & 0x7f) > 0x33 || ((v >> 24) & 0x7f) > 0x33)) { *why = "DENTIST dividers above 0x33 after the hard mins"; return false; }
            st.fp[k] = v;
        } else if ((v & kFp[k].mask) != (st.fp[k] & kFp[k].mask)) { *why = kFp[k].name; return false; }
    }
    if (!frameMoves(e, kOtg0Frame)) { *why = "OTG0 frame counter stopped"; return false; }
    return pipe0Abs(e, why);
}
// b99: after the pipe-0 engine changed OTG0's timing (done or fell back, under
// the shared busy lock), the only sanctioned change of this engine's pipe-0 baseline besides rebaseDentist. OTG0 H_TOTAL /
// V_TOTAL must read exactly hTotal / vTotal (what the pipe-0 engine verified), every other kFp entry (OTG0_OTG_CONTROL,
// OTG0_PIXEL_RATE_CNTL, OTG_PIXEL_RATE_DIV OTG0 bits, DENTIST, DIG1 FE/BE/BE_EN, MPCC0_OPP_ID) unchanged, no new ODM0 underflow
// bit, OTG0 counting at a known AOC mode. No baseline yet (pipe 1 never surveyed) = nothing to do. False = the caller latches
// halted, as any other pipe-0 change.
inline bool rebasePipe0(const Env &e, State &st, uint32_t hTotal, uint32_t vTotal, const char **why) {
    if (!st.fpValid) return true;
    uint32_t nv[kNFp];
    for (unsigned k = 0; k < kNFp; k++) {
        nv[k] = rd(e, kFp[k].reg);
        if (kFp[k].reg == kOtg0HTotal || kFp[k].reg == kOtg0VTotalReg) {
            if (nv[k] != (kFp[k].reg == kOtg0HTotal ? hTotal : vTotal)) { *why = "OTG0 totals are not what the pipe-0 engine reported"; return false; }
        } else if ((nv[k] & kFp[k].mask) != (st.fp[k] & kFp[k].mask)) { *why = kFp[k].name; return false; }
    }
    if (rd(e, kOdm0Global) & kOdmUnderflowBits & ~st.odm0Base) { *why = "ODM0 underflow bit newly set"; return false; }
    if (!frameMoves(e, kOtg0Frame)) { *why = "OTG0 frame counter stopped"; return false; }
    if (!pipe0Abs(e, why)) return false;
    for (unsigned k = 0; k < kNFp; k++) st.fp[k] = nv[k];
    st.p0Rebases++;
    return true;
}
// Survey, ping, stream, (phase), plane with p. failAt: test injection. Returns the stage that failed, 0 = all passed.
inline int swLight(const Env &e, State &st, Sw &sw, const Plan &p, bool inject, const char **why) {
    for (int lv = 1; lv <= 3; lv++) {
        if (inject && sw.failAt == 5 && lv == 3) { *why = "injected failure (stream)"; return 5; }
        if (run(e, st, p, lv) != kDone) { *why = st.why; return lv == 3 ? 5 : 4; }
    }
    if (rd(e, kOtg0VTotal) == rd(e, kOtg1VTotal)) {
        const char *w = nullptr;
        const int r = phaseAlign(e, st, 10, &sw.phaseTries, &sw.phaseLag, &w);
        if (r == kPhaseFailed) { *why = w; return 6; }
    }
    if (inject && sw.failAt == 7) { *why = "injected failure (plane)"; return 7; }
    if (run(e, st, p, 4) != kDone) { *why = st.why; return 7; }
    return 0;
}
inline void swArbRestore(const Env &e, State &st, Sw &sw) {
    if (!sw.arbForced) return;
    e.io.wr(e.io.c, kArbDramStateCntl, sw.arbSaved); st.writes++;
    sw.arbForced = false;
}
// b97 (a stale sticky refused every switch): the b15 watchdog's first pipe-0 underflow hit forces stutter off
// (ARB_DRAM_STATE_CNTL 0x1002), pins UCLK and exits; ODM0's OCCURRED sticky stays set for the rest of the boot, so every
// later switch was refused in the preflight ("ODM0 underflow bit newly set") and CG held the screens ~10 s for it. The
// preflight now accepts that sticky into the baseline when it is the ONLY pipe-0 difference, the watchdog's mitigation is
// in place (stutter forced off) and OCCURRED_CURRENT stays clear for kStaleWatchUs (>= 7 frames at 60 Hz). Reads only:
// pipe 0 is never written (the sticky stays set as the boot's evidence; the target's survey re-baselines it the same
// way). Anything else (stutter allowed, CURRENT seen, HUBP0's sticky, a fingerprint change) refuses as before.
static constexpr uint32_t kStaleWatchUs = 120000, kStaleStepUs = 4000;
inline bool p0StaleAccept(const Env &e, State &st, const char **why) {
    const char *w = nullptr;
    const uint32_t o = rd(e, kOdm0Global);
    if (!(o & kOdmSticky & ~st.odm0Base)) return false;                 // not the sticky: keep fpSame's reason
    if (o & kOdmCurrent) { *why = "ODM0 underflow sticky set and OCCURRED_CURRENT seen (underflow ongoing)"; return false; }
    if (!fpSame(e, st, &w, kOdmSticky)) { *why = w; return false; }      // something else of pipe 0 differs too
    if ((rd(e, kArbDramStateCntl) & 3) != 2) { *why = "ODM0 underflow sticky set with stutter allowed (the b15 watchdog has not acted)"; return false; }
    for (uint32_t t = 0; t <= kStaleWatchUs; t += kStaleStepUs) {
        if (rd(e, kOdm0Global) & kOdmCurrent) { *why = "ODM0 underflow sticky set and OCCURRED_CURRENT seen (underflow ongoing)"; return false; }
        if (t < kStaleWatchUs) e.io.delayUs(e.io.c, kStaleStepUs);
    }
    if (!fpSame(e, st, &w, kOdmSticky)) { *why = w; return false; }      // pipe 0 moved during the watch
    st.odm0Base |= kOdmSticky; st.p0StaleOk++; st.p0StaleOdm0 = o;
    return true;
}
inline int modeSwitch(const Env &e, State &st, Sw &sw) {
    sw.phaseLag = 0; sw.phaseTries = 0; sw.why[0] = 0; sw.arbForced = false; sw.arbSaved = 0;
    const char *w = nullptr;
    if (!sw.cur || !sw.target) return swEnd(st, sw, kSwRefused, 0, "preflight", "no plan");
    if (sw.target == sw.cur) return swEnd(st, sw, kSwSame, 0, "same mode", nullptr);
    // P0 preflight: reads only
    if (st.halted) return swEnd(st, sw, kSwRefused, 0, "preflight", "halted earlier in this boot");
    if (st.dmub.dead) return swEnd(st, sw, kSwRefused, 0, "preflight", "debug.phxdmub_dead");
    if (e.dalDead) return swEnd(st, sw, kSwRefused, 0, "preflight", "debug.phxdal_dead");
    if (st.level != 4 || st.failed || st.touched != sw.cur->n || st.lit != sw.cur->s) return swEnd(st, sw, kSwRefused, 0, "preflight", "pipe 1 is not lit with the current plan");
    if (!staticCheck(*sw.target, &w)) return swEnd(st, sw, kSwRefused, 0, "preflight: target plan", w);
    if (hasI2c(*sw.target) && !e.i2cLock) return swEnd(st, sw, kSwRefused, 0, "preflight", "the target has SCDC steps: no DC_I2C lock");
    // b99: the AOC-144 pipe-0 values are Linux's for one Acer mode each (one capture each); b103: the switch runs only when
    // the pipe-0 engine re-plans pipe 0 around it (Env.p0Pair: phx_pipe0.cpp p0PairSwitch = phxp0::pairSwitch, the safe order)
    if (p0ModeHz(e) == 144 && !e.p0Pair)
        return swEnd(st, sw, kSwRefused, 0, "preflight", "pipe 0 is at 1080p144 and its plan cannot follow pipe 1 (pipe-0 engine off: set pipe 0 back to 60 first)");
    if (!swGlobalReqOk(e, *sw.target, &w)) return swEnd(st, sw, kSwRefused, 0, "preflight: target requirement", w);
    if (!fpSame(e, st, &w)) {
        const char *w2 = w;
        if (!p0StaleAccept(e, st, &w2)) return swEnd(st, sw, kSwRefused, 0, "preflight: pipe 0", w2);
    }
    if (rd(e, kHubp1Cntl) & kUnderflow) return swEnd(st, sw, kSwRefused, 0, "preflight", "HUBP1 underflow set");
    if (rd(e, kOdm1Global) & 0x400u) return swEnd(st, sw, kSwRefused, 0, "preflight", "OPTC1 underflow set");
    static const uint32_t det99[4] = {9, 9, 0, 0};
    const bool needDet = !detIs(e, det99);
    if (needDet && ((rd(e, kHubp2ClkCntl) & 1) || (rd(e, kHubp3ClkCntl) & 1))) return swEnd(st, sw, kSwRefused, 0, "preflight", "HUBP2/3 clock on");
    const bool needDal = !dentistFits(*sw.target, rd(e, kDentist));
    if (needDal && (!e.dal || !e.pmfwIdle)) return swEnd(st, sw, kSwRefused, 0, "preflight", "the target needs the DAL hard mins: no DAL callback");
    if (e.pmfwIdle && !e.pmfwIdle(e.io.c)) return swEnd(st, sw, kSwRefused, 0, "preflight", "a PMFW message is in flight");
    // P1 DET while lit
    if (needDet && !detTwoPipe(e, st, &w)) {
        if (!fpSame(e, st, &w)) { st.halted = 1; return swEnd(st, sw, kSwDark, 1, "HALTED after the DET step", w); }
        return swEnd(st, sw, kSwRefused, 1, "DET", w);
    }
    // P2 hard mins while lit (never lowered), then re-baseline DENTIST only
    if (needDal) {
        const uint32_t args[2] = {sw.target->dalMin ? sw.target->dalMin[0] : kDalMinDispclk331, sw.target->dalMin ? sw.target->dalMin[1] : kDalMinDppclk331};
        for (uint32_t a : args) {
            st.dalSent++;
            const uint32_t r = e.dal(e.io.c, kDalSetHardMinByFreq, a);
            if (r != 1) {
                char d[64]; snprintf(d, sizeof d, "SetHardMinByFreq 0x%08x answered 0x%x", a, r);
                if (!rebaseDentist(e, st, false, &w)) { st.halted = 1; return swEnd(st, sw, kSwDark, 2, "HALTED after a DAL message", w); }
                return swEnd(st, sw, kSwRefused, 2, "DAL", d);
            }
        }
        if (!rebaseDentist(e, st, true, &w)) { st.halted = 1; return swEnd(st, sw, kSwDark, 2, "HALTED: re-baseline", w); }
        if (!dentistFits(*sw.target, rd(e, kDentist))) return swEnd(st, sw, kSwRefused, 2, "DAL", "DENTIST still above the target plan's dividers");
    }
    // P3 from here pipe 1 goes dark; stutter forced off for the relight (b92)
    st.switching = 1; st.switchGen++;
    const uint32_t arb = rd(e, kArbDramStateCntl);
    const bool arbForced = (arb & 3) != 2;
    if (arbForced) { e.io.wr(e.io.c, kArbDramStateCntl, (arb & ~3u) | 2u); st.writes++; e.io.delayUs(e.io.c, 1000); }
    sw.arbSaved = arb; sw.arbForced = arbForced;
    // P4 undo the current mode
    if (run(e, st, *sw.cur, 0) != kDone) return swEnd(st, sw, kSwDark, 3, "undo of the current mode", st.why);
    // P5-P8 the target
    int fs = swLight(e, st, sw, *sw.target, true, &w);
    if (!fs) {
        // P9 the switch's own stickies on pipe 1, then 100 ms clean
        const uint32_t o = rd(e, kOdm1Global);
        if (o & 0x400u) e.io.wr(e.io.c, kOdm1Global, (o & ~0x400u) | 0x1000u);
        e.io.delayUs(e.io.c, 100000);
        if ((rd(e, kHubp1Cntl) & kUnderflow) || (rd(e, kOdm1Global) & 0x400u)) { fs = 8; w = "pipe 1 underflow after the switch"; }
        else { swArbRestore(e, st, sw); return swEnd(st, sw, kSwDone, 7, kSwName[kSwDone], nullptr); }
    }
    char d[160]; snprintf(d, sizeof d, "%s", w ? w : "");
    if (st.halted || st.dmub.dead) return swEnd(st, sw, kSwDark, fs, "latched", d);
    // fallback: undo what the target left, relight the previous plan
    if ((st.touched || st.level) && run(e, st, *sw.target, 0) != kDone) return swEnd(st, sw, kSwDark, fs, "target failed and its undo failed", d);
    const int fb = swLight(e, st, sw, *sw.cur, false, &w);
    if (fb) {
        if (!st.halted && !st.dmub.dead) run(e, st, *sw.cur, 0);
        char d2[160]; snprintf(d2, sizeof d2, "%s; fallback: %s", d, w ? w : "");
        return swEnd(st, sw, kSwDark, 9, "fallback failed", d2);
    }
    e.io.delayUs(e.io.c, 100000);
    if (!(rd(e, kHubp1Cntl) & kUnderflow) && !(rd(e, kOdm1Global) & 0x400u)) swArbRestore(e, st, sw);
    return swEnd(st, sw, kSwFellBack, fs, "fell back to the previous mode", d);
}

// ---- b109 debug.phxdmub_restart: the preconditions this engine
// owns (P1-P4, P7) around phxdmub::restart, then the pipe-0 guard (step 18). Runs under the caller's display lock (gP2Busy).
// mode 1 = power-on chain only (DC_GPIO_GENERIC_A bit 8 = 1: 0x101 after a power-on, 0x1 warm, read on the box),
// 2 = any chain (test override), 3 = 2 + the GOP's 0x0C post-start command, 4 = 1 + 0x0C. One shot per boot.
static constexpr uint32_t kGpioGenericA = 0x05D89, kGpioPowerOn = 0x100;
static constexpr uint32_t kOtgDisablePoint = 0x300;   // OTG0_OTG_CONTROL OTG_DISABLE_POINT_CNTL (dcn_3_2_0_sh_mask.h)
inline int dmubRestart(const Env &e, State &st, phxdmub::RestartSt &rs, int mode, uint32_t *trace, uint32_t traceBytes) {
    using phxdmub::rsRefuse;
    if (rs.done) { rs.refusals++; rs.lastRefusal = "already run in this boot (one shot): reboot to run it again"; return phxdmub::kRsRefused; }
    { const uint32_t a = rs.attempts + 1, r = rs.refusals; rs = phxdmub::RestartSt{}; rs.attempts = a; rs.refusals = r; }
    rs.mode = mode;
    rs.gpioA = rd(e, kGpioGenericA);
    if (mode < 1 || mode > 4) return rsRefuse(rs, "values: 1 power-on chain, 2 any chain, 3 any chain + 0x0C, 4 power-on chain + 0x0C");
    if ((mode == 1 || mode == 4) && !(rs.gpioA & kGpioPowerOn)) return rsRefuse(rs, "DC_GPIO_GENERIC_A bit 8 = 0: a warm chain (2 or 3 override)");
    if (st.halted) return rsRefuse(rs, "halted: pipe 0 changed earlier in this boot");
    if (st.dmub.dead) return rsRefuse(rs, "debug.phxdmub_dead = 1");
    if (st.dmub.sent) return rsRefuse(rs, "a pipe-1 DMUB command was already sent in this boot");
    if (st.level || st.touched || st.failed) return rsRefuse(rs, "the pipe-1 ladder already ran in this boot (survey or more): the restart comes first");
    if (e.dalDead) return rsRefuse(rs, "debug.phxdal_dead = 1");
    if (!e.pmfwIdle) return rsRefuse(rs, "no PMFW idle check");
    const char *w = nullptr;
    if (!pipe0Abs(e, &w)) return rsRefuse(rs, w);
    uint32_t fp[kNFp];
    for (unsigned k = 0; k < kNFp; k++) fp[k] = rd(e, kFp[k].reg);
    const uint32_t odm0 = rd(e, kOdm0Global) & kOdmUnderflowBits;
    rs.otg0CtlPre = fp[0];
    if (!frameMoves(e, kOtg0Frame)) return rsRefuse(rs, "OTG0 frame counter not moving");
    if (!e.pmfwIdle(e.io.c)) return rsRefuse(rs, "a PMFW message is in flight");
    const int rc = phxdmub::restart(e.io, st.dmub, rs, e.vramBytes, mode >= 3, trace, traceBytes);
    if (rc == phxdmub::kRsRefused || rc == phxdmub::kRsNoAck) return rc;
    // 18: the pipe-0 guard against the P7 baseline, also after a dead firmware
    // b110 (seen on the box): the restarted GOP firmware sets OTG0's OTG_DISABLE_POINT_CNTL 2 -> 3 (0x11201 -> 0x11301)
    // with the AOC still scanning; that field only decides where a later OTG disable takes effect, so this guard skips it
    // (the stat records both values). Every other fingerprint bit still halts.
    w = nullptr;
    rs.otg0CtlPost = rd(e, kOtg0Control);
    for (unsigned k = 0; k < kNFp && !w; k++) {
        const uint32_t m = kFp[k].reg == kOtg0Control ? kFp[k].mask & ~kOtgDisablePoint : kFp[k].mask;
        if ((rd(e, kFp[k].reg) & m) != (fp[k] & m)) w = kFp[k].name;
    }
    if (!w && (rd(e, kOdm0Global) & kOdmUnderflowBits & ~odm0)) w = "ODM0 underflow bit newly set";
    if (!w && !pipe0Abs(e, &w)) {}
    if (!w && !frameMoves(e, kOtg0Frame)) w = "OTG0 frame counter stopped";
    if (w) {
        st.halted = 1; rs.guardWhy = w;
        if (rc == phxdmub::kRsOk) return phxdmub::rsEnd(rs, phxdmub::kRsHalted, 18, "pipe 0 / shared state changed across the restart");
        return rc;
    }
    rs.guard = 1;
    return rc;
}
inline int restartStat(const phxdmub::RestartSt &rs, char *b, int n) {
    int o = 0;
    PHXP2_OUT("rc=%s stage=%d(%s) mode=%d done=%d attempts=%u refusals=%u gpio_generic_a=0x%x why=%s\n", rs.attempts ? phxdmub::kRsName[rs.rc] : "none",
              rs.stage, phxdmub::kRsStage[rs.stage >= 0 && rs.stage <= 18 ? rs.stage : 0], rs.mode, rs.done, rs.attempts, rs.refusals, rs.gpioA, rs.why ? rs.why : "-");
    PHXP2_OUT("pre: scratch0=0x%x scratch1=0x%08x scratch7=0x%08x cntl=0x%08x | post: scratch0=0x%x scratch1=0x%08x scratch7=0x%08x cntl=0x%08x\n",
              rs.pre[0], rs.pre[1], rs.pre[2], rs.pre[3], rs.post[0], rs.post[1], rs.post[2], rs.post[3]);
    PHXP2_OUT("stop_fw: acked=%d ack_us=%u deaddead=%d halt_us=%u pwait_us=%u | boot_us=%u main_begin_pre=%u main_begin_post=%u boot_block=%u\n",
              rs.acked, rs.ackUs, rs.stopped, rs.haltUs, rs.pwaitUs, rs.bootUs, rs.mainBeginPre, rs.mainBeginPost, rs.bootBlock);
    PHXP2_OUT("windows: diff_mask=0x%x restored=%u zeroed_bytes=%u stray=%u faults=0x%x/0x%x/0x%x | trace_pre: vram=0x%llx bytes=%u wptr=0x%x\n",
              rs.diffMask, rs.restored, rs.zeroed, rs.stray, rs.fault[0], rs.fault[1], rs.fault[2], static_cast<unsigned long long>(rs.traceVram), rs.traceBytes, rs.traceWptr);
    PHXP2_OUT("init_0x0c: rc=%d us=%u | guard=%d%s%s | otg0_control pre=0x%08x post=0x%08x | last_refusal=%s\n", rs.initRc, rs.initUs, rs.guard,
              rs.guardWhy ? " " : "", rs.guardWhy ? rs.guardWhy : "", rs.otg0CtlPre, rs.otg0CtlPost, rs.lastRefusal ? rs.lastRefusal : "-");
    return o;
}

// ---- b109 debug.phxscdc_fix (seen on the box: the Acer at
// 165 lost TMDS_Config after the boot write was accepted). With pipe 1 lit by a plan that has an SCDC TMDS_Config step of a
// non-zero value (today the 165 plan) and OTG1 at that plan's totals: read the sink's SCDC; when TMDS_Config is not the
// step's value, run the step's own write (phxddc::scdcWriteTmdsConfig = Linux write_scdc_data: Sink_Version,
// Source_Version 1, TMDS_Config) with the stream running, as vc4_hdmi_scrambling_wq does, then read back. DC_I2C only
// (the I2C step's Io): no other register, no TX toggle, no DMUB command. One DC_I2C lock hold for read, write, read-back.
enum FixRes { kFixRewritten = 0, kFixSame = 1, kFixRefused = 2, kFixBusy = 3, kFixFailed = 4 };
static const char *const kFixName[] = {"rewritten", "already set", "refused", "busy", "FAILED"};
static constexpr uint32_t kOtg1HTotal = 0x0506A, kFixSettleUs = 10000;
struct ScdcFix {
    uint32_t runs, rewrites, same, refused, busy, fails;
    int lastRes, lastRc; uint32_t want, line, stray, status;
    phxddc::ScdcStatus before, after;
    const char *why;
};
inline const phx_s2_step *planScdcStep(const Plan &p) {
    for (unsigned i = 0; i < p.n; i++) if (p.s[i].op == PHX_S2_I2C && p.s[i].dmub[0] == kI2cScdcTmds && p.s[i].val) return &p.s[i];
    return nullptr;
}
inline bool planOtg1Totals(const Plan &p, uint32_t *h, uint32_t *v) {
    bool hh = false, vv = false;
    for (unsigned i = 0; i < p.n; i++) {
        if (p.s[i].op != PHX_S2_W) continue;
        if (p.s[i].reg == kOtg1HTotal) { *h = p.s[i].val; hh = true; }
        if (p.s[i].reg == kOtg1VTotal) { *v = p.s[i].val; vv = true; }
    }
    return hh && vv;
}
inline int fixEnd(ScdcFix &fx, int res, const char *why) {
    fx.lastRes = res; fx.why = why;
    if (res == kFixRewritten) fx.rewrites++; else if (res == kFixSame) fx.same++; else if (res == kFixRefused) fx.refused++;
    else if (res == kFixBusy) fx.busy++; else fx.fails++;
    return res;
}
inline int scdcFix(const Env &e, const State &st, const Plan &p, ScdcFix &fx) {
    fx.runs++; fx.lastRc = 0; fx.stray = 0; fx.status = 0; fx.before = fx.after = phxddc::ScdcStatus{};
    if (st.halted) return fixEnd(fx, kFixRefused, "halted earlier in this boot");
    if (st.switching) return fixEnd(fx, kFixRefused, "a pipe-1 mode switch is running");
    if (st.level != 4 || st.failed || !st.lit || st.lit != p.s) return fixEnd(fx, kFixRefused, "pipe 1 is not lit (plane passed) with the current plan");
    const phx_s2_step *s = planScdcStep(p);
    if (!s) return fixEnd(fx, kFixRefused, "the lit plan has no SCDC TMDS_Config step (not an HDMI 2.0 mode: 165 only)");
    uint32_t h = 0, v = 0;
    if (!planOtg1Totals(p, &h, &v) || rd(e, kOtg1HTotal) != h || rd(e, kOtg1VTotal) != v) return fixEnd(fx, kFixRefused, "OTG1 totals are not the lit plan's");
    if (!(rd(e, kOtg1Control) & 1)) return fixEnd(fx, kFixRefused, "OTG1 not enabled");
    if (!e.i2cLock) return fixEnd(fx, kFixRefused, "no DC_I2C lock (Env.i2cLock)");
    fx.want = s->val; fx.line = s->dmub[1];
    if (!e.i2cLock(e.io.c, 1)) return fixEnd(fx, kFixBusy, "DC_I2C busy (an EDID read holds it)");
    I2cGate g{&e, 0};
    const phxddc::Io io{&g, i2cGateRd, i2cGateWr, i2cGateDelay};
    phxddc::Diag d{};
    int res;
    const char *w;
    int rc = phxddc::scdcReadStatus(io, fx.line, &fx.before, &d);
    if (rc != phxddc::kOk) { res = kFixFailed; w = "SCDC status read failed (nothing written)"; }
    else if (fx.before.tmdsConfig == fx.want) { res = kFixSame; w = "TMDS_Config already the plan's value: nothing written"; fx.after = fx.before; }
    else {
        uint8_t sv = 0;
        rc = phxddc::scdcWriteTmdsConfig(io, fx.line, static_cast<uint8_t>(fx.want), &d, &sv);
        if (rc == phxddc::kOk) { e.io.delayUs(e.io.c, kFixSettleUs); rc = phxddc::scdcReadStatus(io, fx.line, &fx.after, &d); }
        if (rc != phxddc::kOk) { res = kFixFailed; w = "SCDC write or read-back failed"; }
        else if (fx.after.tmdsConfig != fx.want) { res = kFixFailed; w = "TMDS_Config read back differs after the write"; }
        else { res = kFixRewritten; w = "TMDS_Config re-written with the stream running"; }
    }
    e.i2cLock(e.io.c, 0);
    fx.lastRc = rc; fx.status = d.status; fx.stray = g.stray;
    if (g.stray) { res = kFixFailed; w = "a write outside DC_I2C (dropped)"; }
    return fixEnd(fx, res, w);
}
inline int fixStat(const ScdcFix &fx, char *b, int n) {
    int o = 0;
    PHXP2_OUT("last=%s why=%s runs=%u rewrites=%u same=%u refused=%u busy=%u fails=%u want=0x%02x line=%u rc=%d i2c_status=0x%x stray=%u\n",
              fx.runs ? kFixName[fx.lastRes] : "none", fx.why ? fx.why : "-", fx.runs, fx.rewrites, fx.same, fx.refused, fx.busy, fx.fails, fx.want,
              fx.line, fx.lastRc, fx.status, fx.stray);
    PHXP2_OUT("before: tmds_config=0x%02x scrambler=0x%02x flags0=0x%02x locked=%d | after: tmds_config=0x%02x scrambler=0x%02x flags0=0x%02x locked=%d\n",
              fx.before.tmdsConfig, fx.before.scrambler, fx.before.flags0, phxddc::scdcLocked(fx.before) ? 1 : 0, fx.after.tmdsConfig,
              fx.after.scrambler, fx.after.flags0, phxddc::scdcLocked(fx.after) ? 1 : 0);
    return o;
}

// ---- b111 debug.phxdmub_regtest (the one-write test): does the running GOP
// DMCUB firmware 0x07008742 execute a generic DMUB register write (DMUB_CMD__REG_SEQ_READ_MODIFY_WRITE, PhxDmub.h
// submitRegTest)? One shot per boot, root writes 1, under the caller's display lock (gP2Busy). Refused with zero register
// writes unless: not run this boot, not halted, no dead latch, no debug.phxdmub_restart this boot, no DMUB command sent
// and the pipe-1 ladder never ran, OTG1 disabled (OTG1_OTG_CONTROL OTG_MASTER_EN 0: the register is dark), pipe 0 at a
// known AOC mode with OTG0 counting, the DMCUB alive and idle as restart()'s P5 (CNTL enable + PWAIT, CNTL2 out of reset,
// SCRATCH0 0x2, SCRATCH1 0x07008742, GPINT 0), the DCN FB base the kext's, the ring geometry as planned and idle.
// Sequence: host reads OTG1_OTG_H_TOTAL (old); one type-1 command {0x0506A, mask 0xffffffff, old ^ 1} through the ring
// (RPTR <= 200 ms, phxdmub::put; a timeout latches debug.phxdmub_dead); host reads it again, polling up to 10 ms for the
// firmware's store; when it changed, the host writes old back directly (the pipe-1 allow-list write the plan uses) and
// reads back; then the pipe-0 fingerprint and OTG0 counting again (any change = halted). No other register is written.
enum RtRes { kRtNone = 0, kRtRefused, kRtWritten, kRtUnsupported, kRtUnexpected, kRtTimeout, kRtNotSent, kRtPipe0 };
static const char *const kRtName[] = {"none", "refused", "written", "unsupported", "unexpected", "timeout", "not-sent", "PIPE0-CHANGED"};
static constexpr uint32_t kRtValueUs = 10000, kRtStepUs = 10;
static_assert(phxdmub::kRegTestAddr == kOtg1HTotal, "the pinned T-C3-0 register is OTG1_OTG_H_TOTAL");
struct RegTest {
    int done, res, ringRc, timedOut, restoreOk, pipe0Same, restartDone;
    uint32_t attempts, refusals;
    uint32_t oldV, sentV, newV, restoredV, valueUs, consumedUs;
    uint32_t rptrBefore, rptrAfter, wptrBefore, wptrAfter;
    uint32_t otg1Ctl, otg0CtlPre, otg0CtlPost, otg0HPre, otg0HPost;
    uint32_t cmd[phxdmub::kCmdDw], reply[phxdmub::kCmdDw];
    const char *why, *lastRefusal, *pipe0Why;
};
inline int rtRefuse(RegTest &t, const char *why) { t.res = kRtRefused; t.why = t.lastRefusal = why; t.refusals++; return kRtRefused; }
inline int dmubRegTest(const Env &e, State &st, RegTest &t, int value, int restartDone) {
    using namespace phxdmub;
    if (t.done) { t.refusals++; t.lastRefusal = "already run in this boot (one shot): reboot to run it again"; return kRtRefused; }
    { const uint32_t a = t.attempts + 1, r = t.refusals; t = RegTest{}; t.attempts = a; t.refusals = r; }
    t.restartDone = restartDone;
    if (value != 1) return rtRefuse(t, "write 1 (anything else refused)");
    if (st.halted) return rtRefuse(t, "halted: pipe 0 changed earlier in this boot");
    if (st.dmub.dead) return rtRefuse(t, "debug.phxdmub_dead = 1");
    if (restartDone) return rtRefuse(t, "debug.phxdmub_restart ran in this boot");
    if (st.dmub.sent) return rtRefuse(t, "a DMUB command was already sent in this boot");
    if (st.level || st.touched || st.failed || st.switching) return rtRefuse(t, "the pipe-1 ladder already ran in this boot (survey or more)");
    t.otg1Ctl = rd(e, kOtg1Control);
    if (t.otg1Ctl & 1) return rtRefuse(t, "OTG1 enabled (OTG1_OTG_CONTROL OTG_MASTER_EN = 1): needs pipe 1 dark (boot without ~/gpu/phxboot-m7)");
    if (!writeAllowed(kRegTestAddr)) return rtRefuse(t, "0x0506A not on the pipe-1 allow-list");
    const char *w = nullptr;
    if (!pipe0Abs(e, &w)) return rtRefuse(t, w);
    const uint32_t cntl = rd(e, kCntl);
    if ((cntl & (kCntlEnable | kCntlPwait)) != (kCntlEnable | kCntlPwait)) return rtRefuse(t, "DMCUB not enabled and idle (CNTL bit 16 / bit 20)");
    if (rd(e, kCntl2) & kSoftReset) return rtRefuse(t, "DMCUB held in soft reset (CNTL2 bit 0)");
    if (rd(e, kScratch0) != 2) return rtRefuse(t, "SCRATCH0 is not 0x2 (mailbox_rdy only)");
    if (rd(e, kScratch1) != kFwVersion) return rtRefuse(t, "SCRATCH1 is not the GOP firmware 0x07008742");
    if (rd(e, kGpintIn1)) return rtRefuse(t, "GPINT_DATAIN1 not 0 (a GPINT in flight)");
    if ((static_cast<uint64_t>(rd(e, kDcnVmFbBase) & 0xffffff) << 24) != e.fbMcBase) return rtRefuse(t, "DCN FB base != the kext's FB base");
    Ring r;
    if (!ringGeometry(e.io, e.vramBytes, &r, &w)) return rtRefuse(t, w);
    if (r.wptr != r.rptr || r.wptr >= r.size || r.wptr % kCmdBytes) return rtRefuse(t, "ring not idle (INBOX1 WPTR != RPTR)");
    uint32_t fp[kNFp];
    for (unsigned k = 0; k < kNFp; k++) fp[k] = rd(e, kFp[k].reg);
    const uint32_t odm0 = rd(e, kOdm0Global) & kOdmUnderflowBits;
    if (!frameMoves(e, kOtg0Frame)) return rtRefuse(t, "OTG0 frame counter not moving");
    t.otg0CtlPre = fp[0]; t.otg0HPre = fp[1];

    t.done = 1;   // the one shot is spent from the first read of the target on
    t.oldV = rd(e, kRegTestAddr);
    t.sentV = t.oldV ^ 1u;
    regTestCmd(t.sentV, t.cmd);
    t.rptrBefore = rd(e, kInbox1Rptr); t.wptrBefore = rd(e, kInbox1Wptr);
    t.ringRc = submitRegTest(e.io, st.dmub, e.vramBytes, t.sentV, &w);
    t.rptrAfter = rd(e, kInbox1Rptr); t.wptrAfter = rd(e, kInbox1Wptr);
    t.timedOut = t.ringRc == kTimeout;
    if (t.ringRc == kOk) { t.consumedUs = st.dmub.lastUs; for (unsigned i = 0; i < kCmdDw; i++) t.reply[i] = st.dmub.reply[i]; }
    t.newV = rd(e, kRegTestAddr);
    if (t.ringRc == kOk)
        for (t.valueUs = 0; t.newV == t.oldV && t.valueUs < kRtValueUs; t.valueUs += kRtStepUs) { e.io.delayUs(e.io.c, kRtStepUs); t.newV = rd(e, kRegTestAddr); }
    if (t.newV != t.oldV) e.io.wr(e.io.c, kRegTestAddr, t.oldV);   // the host restore (allow-listed above)
    t.restoredV = rd(e, kRegTestAddr);
    t.restoreOk = t.restoredV == t.oldV;
    // pipe 0 after: the fingerprint (every bit), no new ODM0 underflow, still a known AOC mode, OTG0 counting
    w = nullptr;
    for (unsigned k = 0; k < kNFp && !w; k++) if ((rd(e, kFp[k].reg) & kFp[k].mask) != (fp[k] & kFp[k].mask)) w = kFp[k].name;
    if (!w && (rd(e, kOdm0Global) & kOdmUnderflowBits & ~odm0)) w = "ODM0 underflow bit newly set";
    if (!w && !pipe0Abs(e, &w)) {}
    if (!w && !frameMoves(e, kOtg0Frame)) w = "OTG0 frame counter stopped";
    t.otg0CtlPost = rd(e, kOtg0Control); t.otg0HPost = rd(e, kOtg0HTotal);
    t.pipe0Same = !w; t.pipe0Why = w;
    if (w) { st.halted = 1; t.res = kRtPipe0; t.why = "pipe 0 / shared state changed: STOP, halted until reboot"; }
    else if (t.timedOut) { t.res = kRtTimeout; t.why = "RPTR did not catch up within 200 ms: debug.phxdmub_dead latched, no retry"; }
    else if (t.ringRc != kOk) { t.res = kRtNotSent; t.why = rcName(t.ringRc); }
    else if (t.newV == t.sentV) { t.res = kRtWritten; t.why = t.restoreOk ? "the firmware executed the type-1 write; the host restored the old value"
                                                                            : "the firmware executed the type-1 write; the host restore did NOT read back the old value"; }
    else if (t.newV == t.oldV) { t.res = kRtUnsupported; t.why = "consumed, value unchanged: the GOP build did not execute the type-1 write"; }
    else { t.res = kRtUnexpected; t.why = t.restoreOk ? "consumed, the value changed to neither old nor old ^ 1; restored" : "consumed, unexpected value; restore did NOT read back"; }
    return t.res;
}
inline int regTestStat(const RegTest &t, char *b, int n) {
    int o = 0;
    PHXP2_OUT("result=%s why=%s done=%d attempts=%u refusals=%u restart_done=%d last_refusal=%s\n", kRtName[t.res >= 0 && t.res <= kRtPipe0 ? t.res : 0],
              t.why ? t.why : "-", t.done, t.attempts, t.refusals, t.restartDone, t.lastRefusal ? t.lastRefusal : "-");
    PHXP2_OUT("cmd: %08x %08x %08x %08x + 12 zero dwords | ring_rc=%d(%s) timeout=%d consumed_us=%u rptr_before=0x%x rptr_after=0x%x wptr_before=0x%x wptr_after=0x%x\n",
              t.cmd[0], t.cmd[1], t.cmd[2], t.cmd[3], t.ringRc, phxdmub::rcName(t.ringRc), t.timedOut, t.consumedUs, t.rptrBefore, t.rptrAfter, t.wptrBefore, t.wptrAfter);
    PHXP2_OUT("otg1_h_total(0x0506A): old=0x%x sent=0x%x new=0x%x value_us=%u restored=0x%x restore_ok=%d | otg1_control=0x%08x\n", t.oldV, t.sentV, t.newV,
              t.valueUs, t.restoredV, t.restoreOk, t.otg1Ctl);
    PHXP2_OUT("reply:");
    for (unsigned i = 0; i < phxdmub::kCmdDw; i++) PHXP2_OUT(" %08x", t.reply[i]);
    PHXP2_OUT("\n");
    PHXP2_OUT("pipe0: same=%d%s%s otg0_control pre=0x%08x post=0x%08x otg0_h_total pre=0x%x post=0x%x\n", t.pipe0Same, t.pipe0Why ? " changed=" : "",
              t.pipe0Why ? t.pipe0Why : "", t.otg0CtlPre, t.otg0CtlPost, t.otg0HPre, t.otg0HPost);
    return o;
}

// ---- b113 debug.phxc3 (the cold-boot dark Acer, C3): the fixed REG_SEQ programs of
// PhxC3Load.h through the GOP firmware's inbox (phxdmub::put), root only, under the caller's display lock (gP2Busy).
//   1..5 = the C3 sub-tests 1a1-1a5, in order, each once, each needing the previous one passed in this boot:
//     1 (1a1) type 3: 14 values 0x11..0x1E to the dark OTG1_OTG_H_TOTAL; PASS = it reads 0x1E, trace 0x8B p0 3, no 0x16/0xBB
//     2 (1a2) type 4 on 0x0506A bit 0 = its current value, timeout 1000 us; PASS = consumed <= 50 us, 0x8B p0 4, no 0x16
//     3 (1a3) type 4 on 0x0506A bit 0 = the other value, timeout 1000 us; PASS = consumed 900..5000 us (timeout honoured,
//             RPTR after execution)
//     4 (1a4) type 1 with 2 sequences, masks 0x2 and 0x4; PASS = old | 0x6 (the RMW keeps bits); SCRATCH5..8 recorded
//     5 (1a5) two type-4 reads of PHY D RESERVED13 bit 28 / bit 29 by the firmware (timeout 2000 us): recorded only
//   9 = the PHY D prologue + SRAM load (1475 commands), one shot, behind G2-G4 (b114 dropped G1, the kext-start
//     DC_GPIO_GENERIC_A bit 8 latch: it reads 0 on power-on chains too, seen on the box; debug.phxpoweron
//     is telemetry only); G2 PHY D dark (OTG1 / DIG3 BE + FE / SYMCLKD off) and a CW5 trace with exactly one
//        MAIN_BEGIN and no PHY D program (0xC0 / 0xC3 / 0xE8 / 0x10A with p0 3); G3 no DMUB command at all this boot, no
//        restart, no ladder, firmware 0x07008742 alive and idle, ring idle; G4 the pipe-0 baseline, the image CRC-32, and
//        DCCG_GATE_DISABLE_CNTL2 bit 27 = 0 (L3 restores it to 0).
// Every refusal happens before the first command: zero register writes (the refused run does not spend the shot). Each
// run stops at its first failure; any result but PASS latches `failed` (nothing more this boot), like the dead latch.
// Runtime stops in mode 9: a command off its table row (cmdOk), a ring timeout (debug.phxdmub_dead), L9 > 10 ms
// (FAIL-WAIT: PHY D's SRAM_INIT_DONE never rose; stopped before the SRAM address), a pipe-0 change after any command
// (halted). Host reads/writes: never the UNIPHY / RDPCS block; the only host register write is the 0x0506A restore of 1-4.
enum C3Res { kC3None = 0, kC3Refused, kC3Pass, kC3Fail, kC3Unsupported, kC3Timeout, kC3NotSent, kC3Wait, kC3Pipe0, kC3Shape };
static const char *const kC3Name[] = {"none", "refused", "PASS", "FAIL", "unsupported", "timeout", "not-sent", "FAIL-WAIT", "PIPE0-CHANGED", "shape-refused"};
static constexpr uint32_t kC3L9StopUs = 10000, kC3T2MaxUs = 50, kC3T3MinUs = 900, kC3T3MaxUs = 5000, kC3ScanMax = 64;
static constexpr uint32_t kScratch5 = 0x036A8;   // SCRATCH5..8 = 0x36A8..0x36AB
static constexpr uint32_t kTrCmd = 0x8a, kTrCmdType = 0x8b, kTrUnsupported = 0x16, kTrErr = 0xbb, kTrC3 = 0xc3;
struct C3 {
    int latched, powerOn; uint32_t gpioStart;         // latched at kext start (debug.phxpoweron = powerOn, -1 when not latched)
    const uint16_t *image;                            // null = the embedded kPhxC3Image (the kext never sets it; host-test seam)
    int done9, failed, passed; uint32_t attempts, refusals, sent;   // this boot: mode 9 spent, failure latch, last 1a passed, own commands
    int mode, res, ringRc, step, pipe0Same, restoreOk, total;
    uint32_t gpioLive, otg1Ctl, dig3Be, dig3Fe, symclkd, gateDis2, crc, trEntries, mainBegin, phyD;
    uint32_t nOk, nType[5], sumUs, maxUs, maxStep, l9Us, l10Us, us[2], trW[4];
    uint32_t oldV, wantV, newV, restoredV, valueUs, cond, rptr[2], wptr[2], scr[2][4];
    uint32_t tr8a, tr8b, tr16, trBB, trScanned;
    uint32_t cmd[phxdmub::kCmdDw];
    const char *why, *lastRefusal, *pipe0Why;
};
inline int c3Refuse(C3 &c, const char *why) { c.res = kC3Refused; c.why = c.lastRefusal = why; c.refusals++; return kC3Refused; }
inline int c3End(C3 &c, int res, const char *why) { c.res = res; c.why = why; if (res != kC3Pass) c.failed = 1; return res; }
// Pipe 0 against the baseline: every fingerprint bit, no new ODM0 underflow bit, still a known AOC mode.
inline bool c3P0Same(const Env &e, const uint32_t *fp, uint32_t odm0, const char **why) {
    for (unsigned k = 0; k < kNFp; k++) if ((rd(e, kFp[k].reg) & kFp[k].mask) != (fp[k] & kFp[k].mask)) { *why = kFp[k].name; return false; }
    if (rd(e, kOdm0Global) & kOdmUnderflowBits & ~odm0) { *why = "ODM0 underflow bit newly set"; return false; }
    return pipe0Abs(e, why);
}
// The firmware's trace entries written between two OUTBOX0_WPTR values (reads only; at most kC3ScanMax entries).
inline void c3TraceScan(const Env &e, C3 &c, uint32_t from, uint32_t to, uint32_t type) {
    phxdmub::Trace t;
    const char *w = nullptr;
    if (!phxdmub::traceGeometry(e.io, e.vramBytes, &t, &w) || to < from || (from & 15) || (to & 15) || static_cast<uint64_t>(to) + 16 > t.size) return;
    for (uint32_t o = from; o < to && c.trScanned < kC3ScanMax; o += 16) {
        const uint32_t code = e.io.mmRd(e.io.c, t.vram + 16u + o), p0 = e.io.mmRd(e.io.c, t.vram + 24u + o);
        c.trScanned++;
        c.tr8a += code == kTrCmd; c.tr8b += code == kTrCmdType && p0 == type; c.tr16 += code == kTrUnsupported; c.trBB += code == kTrErr;
    }
}
// One command: its table row (cmdOk), the dead latch, phxdmub::put. -1 = off its row (nothing sent).
inline int c3Put(const Env &e, State &st, C3 &c, const uint32_t *cmd, const phxc3::Shape *s, const char **w) {
    if (!s || !phxc3::cmdOk(cmd, *s, w)) { if (!s) *w = "no table row"; return -1; }
    if (st.dmub.dead) { *w = "debug.phxdmub_dead = 1"; return phxdmub::kDead; }
    const uint32_t s0 = st.dmub.sent;
    const int rc = phxdmub::put(e.io, st.dmub, e.vramBytes, cmd, w);
    c.sent += st.dmub.sent - s0;
    c.ringRc = rc;
    if (rc == phxdmub::kOk) {
        const uint32_t us = st.dmub.lastUs;
        c.nOk++; c.nType[(cmd[0] & 0xff) < 5 ? (cmd[0] & 0xff) : 0]++; c.sumUs += us;
        if (us > c.maxUs) { c.maxUs = us; c.maxStep = static_cast<uint32_t>(c.step); }
    }
    return rc;
}
// The gates every mode shares (G3 + the G2 OTG1 read + the G4 baseline). nullptr = pass; fp/odm0 get the baseline.
inline const char *c3Gates(const Env &e, State &st, C3 &c, int restartDone, uint32_t *fp, uint32_t *odm0) {
    using namespace phxdmub;
    if (st.halted) return "halted: pipe 0 changed earlier in this boot";
    if (st.dmub.dead) return "debug.phxdmub_dead = 1";
    if (restartDone) return "debug.phxdmub_restart ran in this boot";
    if (st.dmub.sent != c.sent) return "a DMUB command not from debug.phxc3 was sent in this boot (debug.phxdmub_regtest or the ladder)";
    if (st.level || st.touched || st.failed || st.switching) return "the pipe-1 ladder already ran in this boot (survey or more)";
    c.otg1Ctl = rd(e, kOtg1Control);
    if (c.otg1Ctl & 1) return "OTG1 enabled (OTG1_OTG_CONTROL OTG_MASTER_EN = 1): needs pipe 1 dark (boot without ~/gpu/phxboot-m7)";
    const char *w = nullptr;
    if (!pipe0Abs(e, &w)) return w;
    const uint32_t cntl = rd(e, kCntl);
    if ((cntl & (kCntlEnable | kCntlPwait)) != (kCntlEnable | kCntlPwait)) return "DMCUB not enabled and idle (CNTL bit 16 / bit 20)";
    if (rd(e, kCntl2) & kSoftReset) return "DMCUB held in soft reset (CNTL2 bit 0)";
    if (rd(e, kScratch0) != 2) return "SCRATCH0 is not 0x2 (mailbox_rdy only)";
    if (rd(e, kScratch1) != kFwVersion) return "SCRATCH1 is not the GOP firmware 0x07008742";
    if (rd(e, kGpintIn1)) return "GPINT_DATAIN1 not 0 (a GPINT in flight)";
    if ((static_cast<uint64_t>(rd(e, kDcnVmFbBase) & 0xffffff) << 24) != e.fbMcBase) return "DCN FB base != the kext's FB base";
    Ring r;
    if (!ringGeometry(e.io, e.vramBytes, &r, &w)) return w;
    if (r.wptr != r.rptr || r.wptr >= r.size || r.wptr % kCmdBytes) return "ring not idle (INBOX1 WPTR != RPTR)";
    for (unsigned k = 0; k < kNFp; k++) fp[k] = rd(e, kFp[k].reg);
    *odm0 = rd(e, kOdm0Global) & kOdmUnderflowBits;
    if (!frameMoves(e, kOtg0Frame)) return "OTG0 frame counter not moving";
    return nullptr;
}
// Mode 9's own gates: G2 (DIG3 / SYMCLKD dark, the trace), G4 (image CRC, DCCG bit 27). Reads only.
inline const char *c3Gates9(const Env &e, C3 &c) {
    using namespace phxdmub;
    c.dig3Be = rd(e, kDig3BeEn); c.dig3Fe = rd(e, kDig3FeCntl); c.symclkd = rd(e, kSymclkd);
    if (c.dig3Be & 1) return "G2: DIG3_DIG_BE_EN_CNTL bit 0 = 1 (PHY D's back end on)";
    if (c.dig3Fe & 7) return "G2: DIG3_DIG_FE_CNTL source / stream enable set";
    if (c.symclkd & 1) return "G2: SYMCLKD_CLOCK_ENABLE bit 0 = 1";
    Trace t;
    const char *w = nullptr;
    if (!traceGeometry(e.io, e.vramBytes, &t, &w)) return w;
    const unsigned n = traceEntries(t.size, t.wptr);
    if (n == ~0u) return "G2: trace OUTBOX0_WPTR outside the window";
    c.trEntries = n; c.trW[0] = t.wptr;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t code = e.io.mmRd(e.io.c, t.vram + 16u + 16u * i), p0 = e.io.mmRd(e.io.c, t.vram + 24u + 16u * i);
        c.mainBegin += code == kTrMainBegin;
        c.phyD += (code == kTrPllBegin || code == kTrC3 || code == kTrPllOff || code == kTrPll10a) && p0 == kTrPhyD;
    }
    if (c.mainBegin != 1) return "G2: the trace has not exactly one MAIN_BEGIN (wrapped, or a restart)";
    if (c.phyD) return "G2: PHY D already programmed in this firmware boot (trace 0xC0/0xC3/0xE8/0x10A with p0 3)";
    c.crc = phxc3::imageCrc(c.image ? c.image : kPhxC3Image, kPhxC3ImageWords);
    if (c.crc != kPhxC3ImageCrc32) return "G4: the embedded image fails its CRC-32";
    c.gateDis2 = rd(e, phxc3::kGateDis2);
    if (c.gateDis2 & phxc3::kGateDis2PhyD) return "G4: DCCG_GATE_DISABLE_CNTL2 bit 27 is not 0 (L3 restores it to 0)";
    return nullptr;
}
// Mode 9: L1..L1475, stop at the first failure.
inline int c3Load(const Env &e, State &st, C3 &c, const uint32_t *fp, uint32_t odm0) {
    using namespace phxc3;
    const char *w = nullptr;
    c.total = kLoadN;
    c.pipe0Same = 1;
    c.done9 = 1;   // the one shot is spent from the first command on
    for (unsigned i = 0; i < kLoadN; i++) {
        c.step = static_cast<int>(i);
        loadCmd(i, c.cmd, c.image ? c.image : kPhxC3Image);
        const Shape *s = shapeAt(kLoadShape, sizeof kLoadShape / sizeof kLoadShape[0], i);
        const int rc = c3Put(e, st, c, c.cmd, s, &w);
        if (rc < 0) return c3End(c, kC3Shape, w);
        if (rc == phxdmub::kTimeout) return c3End(c, kC3Timeout, "RPTR did not catch up within 200 ms: debug.phxdmub_dead latched; do not run m7, warm reboot");
        if (rc != phxdmub::kOk) return c3End(c, kC3NotSent, phxdmub::rcName(rc));
        if (i == kL9) c.l9Us = st.dmub.lastUs;
        if (i == kL10) c.l10Us = st.dmub.lastUs;
        if (!c3P0Same(e, fp, odm0, &w)) { st.halted = 1; c.pipe0Same = 0; c.pipe0Why = w; return c3End(c, kC3Pipe0, "pipe 0 / shared state changed: STOP, halted until reboot"); }
        if (i == kL9 && c.l9Us > kC3L9StopUs)
            return c3End(c, kC3Wait, "L9 (RESERVED13 bit 28 SRAM_INIT_DONE) took > 10 ms: it never rose; stopped before the SRAM address, do not run m7");
        if (s->gapUs) e.io.delayUs(e.io.c, s->gapUs);
        if (i == kL10) {
            c.trW[1] = rd(e, phxdmub::kOutbox0Wptr);
            if (!frameMoves(e, kOtg0Frame)) { st.halted = 1; c.pipe0Same = 0; c.pipe0Why = "OTG0 frame counter stopped"; return c3End(c, kC3Pipe0, "pipe 0 stopped after the prologue: STOP"); }
        }
        if (i == kLast - 1) c.trW[2] = rd(e, phxdmub::kOutbox0Wptr);
    }
    c.trW[3] = rd(e, phxdmub::kOutbox0Wptr);
    if (!frameMoves(e, kOtg0Frame)) { st.halted = 1; c.pipe0Same = 0; c.pipe0Why = "OTG0 frame counter stopped"; return c3End(c, kC3Pipe0, "pipe 0 stopped after the load: STOP"); }
    return c3End(c, kC3Pass, "loaded: prologue L1-L10, SRAM address, 20480 halfwords, RESERVED13 bit 29");
}
// Modes 1-5: the C3 sub-tests 1a1-1a5 (all on 0x0506A except 5). The host restores 0x0506A when it changed.
inline int c3Warm(const Env &e, State &st, C3 &c, int mode, const uint32_t *fp, uint32_t odm0) {
    using namespace phxc3;
    const char *w = nullptr;
    c.total = mode == 5 ? 2 : 1;
    const Shape *sh = mode == 1 ? kT1Shape : mode == 4 ? kT4Shape : mode == 5 ? kT5Shape : kT2Shape;
    const uint32_t type = mode == 1 ? kBurst : mode == 4 ? kRmw : kWait;
    if (mode == 4) for (unsigned k = 0; k < 4; k++) c.scr[0][k] = rd(e, kScratch5 + k);
    c.trW[0] = rd(e, phxdmub::kOutbox0Wptr);
    c.rptr[0] = rd(e, phxdmub::kInbox1Rptr); c.wptr[0] = rd(e, phxdmub::kInbox1Wptr);
    int rc = phxdmub::kOk;
    for (int k = 0; k < c.total && rc == phxdmub::kOk; k++) {
        c.step = k;
        if (mode == 1) t1Cmd(c.cmd); else if (mode == 4) t4Cmd(c.cmd); else if (mode == 5) t5Cmd(static_cast<unsigned>(k), c.cmd); else t23Cmd(c.cond, c.cmd);
        rc = c3Put(e, st, c, c.cmd, &sh[k], &w);
        if (rc == phxdmub::kOk) c.us[k] = st.dmub.lastUs;
    }
    c.rptr[1] = rd(e, phxdmub::kInbox1Rptr); c.wptr[1] = rd(e, phxdmub::kInbox1Wptr);
    if (mode != 5) {
        c.newV = rd(e, kOtg1HTotal);
        if (rc == phxdmub::kOk && (mode == 1 || mode == 4))
            for (c.valueUs = 0; c.newV == c.oldV && c.valueUs < kRtValueUs; c.valueUs += kRtStepUs) { e.io.delayUs(e.io.c, kRtStepUs); c.newV = rd(e, kOtg1HTotal); }
        if (c.newV != c.oldV) e.io.wr(e.io.c, kOtg1HTotal, c.oldV);   // the host restore (pipe-1 allow-list, checked before)
        c.restoredV = rd(e, kOtg1HTotal);
        c.restoreOk = c.restoredV == c.oldV;
    }
    c.trW[1] = rd(e, phxdmub::kOutbox0Wptr);
    if (rc != -1) c3TraceScan(e, c, c.trW[0], c.trW[1], type);
    if (mode == 4) for (unsigned k = 0; k < 4; k++) c.scr[1][k] = rd(e, kScratch5 + k);
    w = nullptr;
    const char *pw = nullptr;
    if (!c3P0Same(e, fp, odm0, &pw)) {}
    else if (!frameMoves(e, kOtg0Frame)) pw = "OTG0 frame counter stopped";
    c.pipe0Same = !pw; c.pipe0Why = pw;
    if (pw) { st.halted = 1; return c3End(c, kC3Pipe0, "pipe 0 / shared state changed: STOP, halted until reboot"); }
    if (rc == -1) return c3End(c, kC3Shape, "a command off its table row: nothing sent");
    if (rc == phxdmub::kTimeout) return c3End(c, kC3Timeout, "RPTR did not catch up within 200 ms: debug.phxdmub_dead latched, warm reboot");
    if (rc != phxdmub::kOk) return c3End(c, kC3NotSent, phxdmub::rcName(rc));
    if (mode != 5 && !c.restoreOk) return c3End(c, kC3Fail, "the host restore of 0x0506A did not read back");
    if (c.tr16) return c3End(c, kC3Unsupported, mode == 1 ? "trace 0x16: no type 3 in the GOP build (C3 stops: RESERVED20 needs a write with no read)"
                                              : mode == 4 ? "trace 0x16: no type 1 with 2 sequences" : "trace 0x16: no type 4 in the GOP build (L9/L10 become fixed waits: research first)");
    if (c.trBB) return c3End(c, kC3Fail, "trace 0xBB (a firmware error) after the command");
    switch (mode) {
    case 1:
        if (c.newV == c.oldV) return c3End(c, kC3Unsupported, "consumed, value unchanged: no type 3 (C3 stops: RESERVED20 needs a write with no read)");
        if (c.newV != c.wantV) return c3End(c, kC3Fail, "0x0506A is not the last burst value 0x1E");
        break;
    case 2:
        if (c.newV != c.oldV) return c3End(c, kC3Fail, "a type-4 wait changed 0x0506A");
        if (c.us[0] > kC3T2MaxUs) return c3End(c, kC3Fail, "a matching type-4 wait took > 50 us");
        break;
    case 3:
        if (c.newV != c.oldV) return c3End(c, kC3Fail, "a type-4 wait changed 0x0506A");
        if (c.us[0] < kC3T3MinUs) return c3End(c, kC3Fail, "consumed < 900 us: no timing signal (L9's stop rule needs the trace ticks)");
        if (c.us[0] > kC3T3MaxUs) return c3End(c, kC3Fail, "consumed > 5000 us: the 1000 us timeout is not honoured (never use type 4 on PHY D)");
        break;
    case 4:
        if (c.newV == c.oldV) return c3End(c, kC3Unsupported, "consumed, value unchanged: the type-1 sequences were not executed");
        if (c.newV == ((c.oldV & ~0x6u) | 0x4u)) return c3End(c, kC3Fail, "0x0506A = old | 0x4: the mask is ignored (L1-L10 cannot be expressed)");
        if (c.newV != c.wantV) return c3End(c, kC3Fail, "0x0506A is not old | 0x6");
        break;
    default: break;
    }
    if (mode != 5 && !c.tr8b) return c3End(c, kC3Fail, "no trace 0x8B with p0 = the command type after the command");
    c.passed = mode;
    return c3End(c, kC3Pass, mode == 5 ? "recorded (no criterion): consumed_us = the firmware's RESERVED13 bit 28 / bit 29 waits" : "the criterion of the sub-test holds");
}
inline int dmubC3(const Env &e, State &st, C3 &c, int mode, int restartDone) {
    if (c.failed) { c.refusals++; c.lastRefusal = "a debug.phxc3 run failed in this boot: nothing more until reboot"; return kC3Refused; }
    if (c.done9) { c.refusals++; c.lastRefusal = "mode 9 already ran in this boot (one shot): reboot to run it again"; return kC3Refused; }
    {   // a new run record; the boot's latches and counters stay
        const C3 k = c; c = C3{};
        c.image = k.image; c.latched = k.latched; c.powerOn = k.powerOn; c.gpioStart = k.gpioStart; c.passed = k.passed; c.sent = k.sent;
        c.attempts = k.attempts + 1; c.refusals = k.refusals; c.lastRefusal = k.lastRefusal;
    }
    c.mode = mode; c.step = -1;
    if ((mode < 1 || mode > 5) && mode != 9) return c3Refuse(c, "values: 1-5 the T-C3-1a sub-tests (in order), 9 the PHY D prologue + SRAM load");
    if (mode <= 5 && c.passed >= mode) return c3Refuse(c, "this sub-test already passed in this boot (each runs once)");
    if (mode <= 5 && c.passed != mode - 1) return c3Refuse(c, "the T-C3-1a sub-tests run in order 1, 2, 3, 4, 5: the previous one must pass first in this boot");
    if (mode == 9 && (c.passed || c.sent)) return c3Refuse(c, "G3: a T-C3-1a sub-test ran in this boot (mode 9 needs no DMUB command before it)");
    c.gpioLive = rd(e, kGpioGenericA);
    // b114: no G1. DC_GPIO_GENERIC_A bit 8 is 0 at kext start even after a 55-min power-off and rises later in the boot
    // (seen on the box), so it cannot tell a power-on chain; the latch stays as telemetry only (poweron=).
    uint32_t fp[kNFp], odm0 = 0;
    if (const char *g = c3Gates(e, st, c, restartDone, fp, &odm0)) return c3Refuse(c, g);
    if (mode == 9) {
        if (st.dmub.sent) return c3Refuse(c, "G3: a DMUB command was already sent in this boot");
        if (const char *g = c3Gates9(e, c)) return c3Refuse(c, g);
        return c3Load(e, st, c, fp, odm0);
    }
    if (!writeAllowed(phxc3::kOtg1HTotal)) return c3Refuse(c, "0x0506A not on the pipe-1 allow-list");
    if (mode != 5) {
        c.oldV = rd(e, phxc3::kOtg1HTotal);
        c.wantV = mode == 1 ? phxc3::kT1Last : mode == 4 ? (c.oldV | 0x6u) : c.oldV;
        c.cond = mode == 2 ? (c.oldV & 1u) : mode == 3 ? ((c.oldV & 1u) ^ 1u) : 0;
        if (mode == 1 && c.oldV == phxc3::kT1Last) return c3Refuse(c, "0x0506A already reads 0x1E: 1a1 could not tell a write");
        if (mode == 4 && (c.oldV & 0x6u)) return c3Refuse(c, "0x0506A bits 1/2 are not 0: 1a4 could not tell a kept bit");
    }
    return c3Warm(e, st, c, mode, fp, odm0);
}
inline int c3Stat(const C3 &c, char *b, int n) {
    int o = 0;
    char step[16];
    if (c.step < 0) snprintf(step, sizeof step, "-"); else if (c.mode == 9) snprintf(step, sizeof step, "L%d", c.step + 1); else snprintf(step, sizeof step, "cmd%d", c.step + 1);
    PHXP2_OUT("result=%s mode=%d step=%s why=%s\n", kC3Name[c.res >= 0 && c.res <= kC3Shape ? c.res : 0], c.mode, step, c.why ? c.why : "-");
    PHXP2_OUT("boot: attempts=%u refusals=%u failed=%d t_c3_1a_passed=%d mode9_done=%d own_dmub_commands=%u last_refusal=%s\n", c.attempts, c.refusals,
              c.failed, c.passed, c.done9, c.sent, c.lastRefusal ? c.lastRefusal : "-");
    PHXP2_OUT("gates: poweron=%d gpio_generic_a start=0x%x live=0x%x otg1_control=0x%08x dig3_be_en=0x%x dig3_fe=0x%x symclkd=0x%x "
              "dccg_gate_disable_cntl2=0x%08x image_crc=0x%08x/0x%08x trace_entries=%u main_begin=%u phyd_entries=%u\n", c.latched ? c.powerOn : -1, c.gpioStart, c.gpioLive,
              c.otg1Ctl, c.dig3Be, c.dig3Fe, c.symclkd, c.gateDis2, c.crc, kPhxC3ImageCrc32, c.trEntries, c.mainBegin, c.phyD);
    PHXP2_OUT("ring: loaded %u/%u (type1 %u type3 %u type4 %u) ring_rc=%d(%s) sum_us=%u max_us=%u at cmd %u | L9_us=%u L10_us=%u | trace wptr "
              "pre=0x%x prologue=0x%x load=0x%x end=0x%x\n", c.nOk, c.total, c.nType[1], c.nType[3], c.nType[4], c.ringRc, phxdmub::rcName(c.ringRc), c.sumUs,
              c.maxUs, c.maxStep + 1, c.l9Us, c.l10Us, c.trW[0], c.trW[1], c.trW[2], c.trW[3]);
    if (c.mode >= 1 && c.mode <= 5)
        PHXP2_OUT("1a: 0x0506A old=0x%x want=0x%x new=0x%x value_us=%u restored=0x%x restore_ok=%d cond=%u | consumed_us=%u,%u rptr 0x%x->0x%x wptr 0x%x->0x%x | "
                  "trace 8a=%u 8b(type)=%u 16=%u bb=%u scanned=%u | scratch5..8 pre=%x/%x/%x/%x post=%x/%x/%x/%x\n", c.oldV, c.wantV, c.newV, c.valueUs,
                  c.restoredV, c.restoreOk, c.cond, c.us[0], c.us[1], c.rptr[0], c.rptr[1], c.wptr[0], c.wptr[1], c.tr8a, c.tr8b, c.tr16, c.trBB, c.trScanned,
                  c.scr[0][0], c.scr[0][1], c.scr[0][2], c.scr[0][3], c.scr[1][0], c.scr[1][1], c.scr[1][2], c.scr[1][3]);
    PHXP2_OUT("last cmd:");
    for (unsigned i = 0; i < phxdmub::kCmdDw; i++) PHXP2_OUT(" %08x", c.cmd[i]);
    PHXP2_OUT("\npipe0: same=%d%s%s\n", c.pipe0Same, c.pipe0Why ? " changed=" : "", c.pipe0Why ? c.pipe0Why : "");
    return o;
}

// debug.phxpipe2_stat. live = read the key registers now (root).
inline int stat(const Env &e, const State &st, const Plan &p, char *b, int n, bool live) {
    int o = 0;
    char id[16];
    stepId(p, st.lastStep, id, sizeof id);
    PHXP2_OUT("level %d (%s) failed %d halted %d dmub_dead %d dal_dead %d applied %u/%u runs %u undo_runs %u\n", st.level,
              kLevelName[st.level >= 0 && st.level <= 4 ? st.level : 0], st.failed, st.halted, st.dmub.dead, e.dalDead, st.touched, p.n, st.runs, st.undoRuns);
    PHXP2_OUT("last: level %d (%s) result %s step %s: %s\n", st.lastLevel, kLevelName[st.lastLevel >= 0 && st.lastLevel <= 4 ? st.lastLevel : 0],
              st.lastRes == kDone ? "done" : st.lastRes == kRefused ? "refused" : "FAILED", id, st.why);
    PHXP2_OUT("dmub: sent %u consumed %u timeouts %u refused %u readback_fail %u last_rc %u (%s) last_hdr 0x%08x slot 0x%x last_us %u max_us %u\n",
              st.dmub.sent, st.dmub.consumed, st.dmub.timeouts, st.dmub.refused, st.dmub.readbackFails, st.dmub.lastRc,
              phxdmub::rcName(static_cast<int>(st.dmub.lastRc)), st.dmub.lastHdr, st.dmub.lastSlot, st.dmub.lastUs, st.dmub.maxUs);
    PHXP2_OUT("dmub reply:");
    for (unsigned i = 0; i < 8; i++) PHXP2_OUT(" %08x", st.dmub.reply[i]);
    PHXP2_OUT("\n");
    if (st.dmub.dead)
        PHXP2_OUT("dmub DEAD: SCRATCH0 0x%08x SCRATCH7 0x%08x INTERRUPT_STATUS 0x%08x CNTL 0x%08x WPTR 0x%x RPTR 0x%x\n", st.dmub.deadInfo[0],
                  st.dmub.deadInfo[1], st.dmub.deadInfo[2], st.dmub.deadInfo[3], st.dmub.deadInfo[4], st.dmub.deadInfo[5]);
    PHXP2_OUT("writes %u readback_mismatch %u guards %u; survey: %u/%u plan writes already equal; OTG0 %u Hz OTG1 %u Hz\n", st.writes,
              st.rbMismatch, st.guards, st.stepsEqual, st.stepsW, st.otg0Hz, st.otg1Hz);
    PHXP2_OUT("stream post: DIG3_BE_EN 0x%08x SYMCLKD 0x%08x DIG3_FE 0x%08x DIG3_CLOCK_PATTERN 0x%08x DIG3_FIFO0 0x%08x OTG1_PIXEL_RATE 0x%08x\n",
              st.post[0], st.post[1], st.post[2], st.post[3], st.post[4], st.post[5]);
    if (st.i2cOps || hasI2c(p))
        PHXP2_OUT("scdc: ops %u soft_fails %u last_rc %d sink_version %u last_read 0x%02x i2c_status 0x%x stray %u\n", st.i2cOps,
                  st.i2cSoftFails, st.i2cLastRc, st.i2cSink, st.i2cLastVal, st.i2cStatus, st.i2cStray);
    if (st.fpValid) {
        PHXP2_OUT("pipe-0 baseline:");
        for (unsigned k = 0; k < kNFp; k++) PHXP2_OUT(" %s=0x%x", kFp[k].name, st.fp[k]);
        PHXP2_OUT(" ODM0_underflow_bits=0x%x\n", st.odm0Base);
        if (st.p0StaleOk) PHXP2_OUT("switch preflight accepted a stale ODM0 underflow sticky %u time(s), last ODM0 0x%x\n", st.p0StaleOk, st.p0StaleOdm0);
        if (st.p0Rebases) PHXP2_OUT("pipe-0 baseline re-taken after %u pipe-0 engine mode set(s) (b99 rebasePipe0)\n", st.p0Rebases);
    }
    for (unsigned i = 0; i < p.nReq && i < kMaxReq; i++) {
        if (!(st.reqChecked & (1u << i))) continue;
        const phx_s2_req &q = p.req[i];
        PHXP2_OUT("REQ S2.%u %s %-32s now 0x%08x & 0x%08x %s 0x%08x", q.stage, st.reqOk & (1u << i) ? "ok  " : "FAIL", q.name, st.reqNow[i],
                  q.mask, q.cmp == 1 ? "<=" : "==", q.val);
        if (q.cmp == 3) PHXP2_OUT(" or 0x%08x", static_cast<uint32_t>(q.pad));
        PHXP2_OUT("\n");
    }
    if (live) {
        PHXP2_OUT("live: OTG0_CONTROL 0x%08x H_TOTAL 0x%x frame %u | OTG1_CONTROL 0x%08x frame %u | HUBP0_CNTL 0x%08x HUBP1_CNTL 0x%08x | "
                  "ODM0_GLOBAL 0x%08x ODM1_GLOBAL 0x%08x\n", rd(e, kOtg0Control), rd(e, kOtg0HTotal), rd(e, kOtg0Frame) & 0xffffff,
                  rd(e, kOtg1Control), rd(e, kOtg1Frame) & 0xffffff, rd(e, kHubp0Cntl), rd(e, kHubp1Cntl), rd(e, kOdm0Global), rd(e, kOdm1Global));
        PHXP2_OUT("live: DMCUB WPTR 0x%x RPTR 0x%x SCRATCH0 0x%08x | DIG3_BE_EN 0x%08x SYMCLKD 0x%08x DIG3_FIFO0 0x%08x OTG1_PIXEL_RATE 0x%08x\n",
                  rd(e, phxdmub::kInbox1Wptr), rd(e, phxdmub::kInbox1Rptr), rd(e, phxdmub::kScratch0), rd(e, kDig3BeEn), rd(e, kSymclkd),
                  rd(e, kDig3FifoCtrl0), rd(e, kOtg1PixelRate));
    }
    return o;
}

// debug.phxpipe2_plan: every entry with its value now (live) and at the survey.
inline int planDump(const Env &e, const State &st, const Plan &p, char *b, int n) {
    int o = 0;
    static const char *const op[] = {"?", "W", "DMUB", "POLL", "DELAY", "CHECK", "I2C"};
    for (unsigned i = 0; i < p.n; i++) {
        const phx_s2_step &s = p.s[i];
        char id[16]; stepId(p, static_cast<int>(i), id, sizeof id);
        const bool r = s.op == PHX_S2_W || s.op == PHX_S2_POLL || s.op == PHX_S2_CHECK;
        if (s.op == PHX_S2_DMUB)
            PHXP2_OUT("%3u %s %-5s %08x %08x %08x %08x %s\n", i, id, op[s.op], s.dmub[0], s.dmub[1], s.dmub[2], s.dmub[3], s.name);
        else if (s.op == PHX_S2_I2C)
            PHXP2_OUT("%3u %s %-5s kind %u line %u addr 0x%02x offset 0x%02x val 0x%02x mask 0x%02x flags %u %s%s\n", i, id, op[s.op], s.dmub[0],
                      s.dmub[1], s.dmub[2], s.dmub[3], s.val, s.mask, s.dmub[4], s.name, i < st.touched ? " [applied]" : "");
        else
            PHXP2_OUT("%3u %s %-5s 0x%05X now 0x%08x survey 0x%08x -> 0x%08x mask 0x%08x %s%s\n", i, id, op[s.op <= 5 ? s.op : 0], s.reg,
                      r ? rd(e, s.reg) : 0, st.now[i], s.val, s.mask, s.name, i < st.touched ? " [applied]" : "");
    }
    return o;
}

}  // namespace phxp2
