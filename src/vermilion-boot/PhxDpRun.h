// SPDX-License-Identifier: MIT
// PhxDpRun.h: kext b116, multiport S2b: the
// engine of debug.phxdplink = the train-only DP link check of Linux dp_verify_link_cap on DP A or DP E, from our kext:
//   1. gates (reads only; a refusal writes nothing),
//   2. the PHY's C3 SRAM load (PhxC3Phy.h) when debug.phxdplink did not load that PHY yet in this boot,
//   3. PhxDpLink.h verifyLink: D3, DP_CONFIG / scrambler, TRANSMITTER_CONTROL ENABLE, SET_POWER D0, link config, clock
//      recovery + channel equalisation over AUX with SETUP_VSEMPH per lane, pattern off, status, then link off again
//      (D3, TRANSMITTER_CONTROL DISABLE, pattern / link / internal registers 0),
//   4. post checks: DIGn back end and DPn link off, pipe 0 and pipe 1 as before.
// No OTG, no stream encoder front end, no clock, no plane: nothing of pipe 0 (GOP, OTG0) or pipe 1 (OTG1 / DIG3) is written.
// Kernel-free; the kext glue is phx_dplink.cpp; host-tested (with mutations).
//
// Selector (root writes debug.phxdplink): bits 3:0 = the DIG = the PHY (0 = DP A, 4 = DP E); bit 4 = HBR x4 (Linux's
// detection rate) instead of RBR x4 (the modeset rate for 1080p60). Anything else is refused.
//
// Every write goes through a per-run Io: MMIO only to the connector's DIG/DP registers (phxdpl::writeAllowed), DMUB only
// through phxdmub::submitWith with the connector's exact command shapes (phxdpl::txAllowed) or the C3 table check
// (phxc3p::cmdOkPhy), DPCD writes only 0x100-0x108 / 0x600, AUX register writes only the engine's block and its pad
// (auxWriteAllowed, the glue). After every MMIO write and every DMUB command: the pipe-0 fingerprint (PhxPipe2 kFp, ODM0
// underflow, pipe0Abs) and the pipe-1 fingerprint (kFp1, ODM1 underflow); a change = `halted` (PhxPipe2 State: nothing
// more until reboot, not even the link off); the OTG0 (and a running OTG1) frame counter after the C3 prologue, the C3 load
// and the check. A ring timeout = debug.phxdmub_dead (no DISABLE after it). Any result but PASS latches `failed`.
#pragma once
#include <stdint.h>
#include "PhxPipe2.h"
#include "PhxDpLink.h"
#include "PhxC3Phy.h"

namespace phxdpr {

using phxp2::Env;
using phxp2::State;
using phxp2::rd;

enum Res { kNone = 0, kRefused, kPass, kC3Fail, kTrainFail, kTimeout, kNotSent, kHalted, kPostFail };
static const char *const kResName[] = {"none", "refused", "PASS", "C3-FAIL", "TRAIN-FAIL", "timeout", "not-sent", "HALTED", "POST-FAIL"};
static constexpr uint32_t kSelDig = 0x0f, kSelHbr = 0x10;
static constexpr uint32_t kC3L9StopUs = 10000;
static constexpr uint32_t kSymclk0 = 0x00160;   // SYMCLKA_CLOCK_ENABLE (BASE_IDX 1); + n (SYMCLKD = 0x163 = phxp2::kSymclkd)
static_assert(kSymclk0 + 3 == phxp2::kSymclkd, "SYMCLKn");

// Pipe 1 (OTG1 / DIG3 / PHY D, the Acer): whatever state it is in (lit or dark) must not change during a run.
struct Fp1 { uint32_t reg, mask; const char *name; };
static const Fp1 kFp1[] = {
    {0x05081, 0xffffffffu, "OTG1_OTG_CONTROL"},
    {0x0506A, 0xffffffffu, "OTG1_OTG_H_TOTAL"},
    {0x0506F, 0xffffffffu, "OTG1_OTG_V_TOTAL"},
    {0x00144, 0x0000333bu, "OTG1_PIXEL_RATE_CNTL"},
    {0x0012F, 0x0000003fu, "OTG_PIXEL_RATE_DIV(OTG0+OTG1)"},
    {0x0584B, 0xfef7ffffu, "DIG3_DIG_FE_CNTL"},
    {0x05871, 0xffffffffu, "DIG3_DIG_BE_CNTL"},
    {0x05872, 0x00000001u, "DIG3_DIG_BE_EN_CNTL"},
    {0x09017, 0x0000000fu, "MPCC1_MPCC_OPP_ID"},
};
static constexpr unsigned kNFp1 = sizeof kFp1 / sizeof kFp1[0];

// The glue's AUX path (phxddc::aux under the EDID/AUX lock): 0 = ACK with n bytes.
struct Aux {
    void *c;
    int (*rd)(void *c, unsigned eng, uint32_t addr, uint8_t *buf, unsigned n);
    int (*wr)(void *c, unsigned eng, uint32_t addr, const uint8_t *buf, unsigned n);
};
// The AUX register writes the glue's AUX Io lets through: engine `eng`'s block (CONTROL .. SW_DATA) and its DDC pad
// (DDC<eng+1>_MASK, DC_GPIO_AUX_CTRL_5: PhxDdc.h auxPadSetup, unchanged since b115).
inline bool auxWriteAllowed(unsigned eng, uint32_t reg) {
    if (eng > 4) return false;
    const uint32_t b = phxddc::kAux0 + phxddc::kAuxStride * eng;
    return (reg >= b && reg < b + 7) || reg == phxddc::padMaskReg(eng) || reg == phxddc::kAuxCtrl5;
}

struct Run {
    int failed;                  // latch: a run ended in anything but PASS (refusals do not latch)
    uint8_t c3Done[5];           // PHY n loaded by debug.phxdplink in this boot
    uint32_t attempts, refusals, passes;
    int sel, dig, res, phase;    // phase: 0 gates, 1 C3, 2 training, 3 post
    const char *why, *lastRefusal, *pipeWhy, *ringWhy;
    int ringRc;
    // gates
    uint32_t hpdStatus, beEn, beCntl, linkCntl, vidStream, symclk, gateDis2, cntl, scr0, scr1, otg1Ctl, crc;
    uint32_t trEntries, trMainBegin, trPhyN; int trOk;
    uint8_t caps[16]; int capsRc;
    phxdpl::Link link;
    // C3
    int c3Ran; uint32_t c3Ok, c3Step, c3SumUs, c3MaxUs, c3L9Us;
    // training
    phxdpl::Report rep; int vrc;
    uint32_t dmubSent, mmioWr, strayWr, blocked, auxRdN, auxWrN, auxErr, dpcdStray;
    // post
    uint32_t postBeEn, postLink, postStream;
    int pipe0Same, pipe1Same;
    uint32_t fp0[phxp2::kNFp], fp1[kNFp1], odm0, odm1;
    uint32_t otg0Frames, otg1Frames;
};

inline int refuse(Run &r, const char *why) { r.res = kRefused; r.why = r.lastRefusal = why; r.refusals++; return kRefused; }
inline int end(Run &r, int res, const char *why) { r.res = res; r.why = why; if (res != kPass) r.failed = 1; else r.passes++; return res; }

inline bool pipe1Same(const Env &e, const Run &r, const char **why) {
    for (unsigned k = 0; k < kNFp1; k++) if ((rd(e, kFp1[k].reg) & kFp1[k].mask) != (r.fp1[k] & kFp1[k].mask)) { *why = kFp1[k].name; return false; }
    if (rd(e, phxp2::kOdm1Global) & phxp2::kOdmUnderflowBits & ~r.odm1) { *why = "ODM1 underflow bit newly set"; return false; }
    return true;
}
inline bool pipe0Same(const Env &e, const Run &r, const char **why) {
    for (unsigned k = 0; k < phxp2::kNFp; k++)
        if ((rd(e, phxp2::kFp[k].reg) & phxp2::kFp[k].mask) != (r.fp0[k] & phxp2::kFp[k].mask)) { *why = phxp2::kFp[k].name; return false; }
    if (rd(e, phxp2::kOdm0Global) & phxp2::kOdmUnderflowBits & ~r.odm0) { *why = "ODM0 underflow bit newly set"; return false; }
    return phxp2::pipe0Abs(e, why);
}
// After a write or a command: both fingerprints; a change = halted (nothing more this boot).
inline bool guard(const Env &e, State &st, Run &r) {
    const char *w = nullptr;
    if (!pipe0Same(e, r, &w)) { st.halted = 1; r.pipe0Same = 0; r.pipeWhy = w; return false; }
    if (!pipe1Same(e, r, &w)) { st.halted = 1; r.pipe1Same = 0; r.pipeWhy = w; return false; }
    return true;
}
inline bool framesMove(const Env &e, State &st, Run &r) {
    if (!phxp2::frameMoves(e, phxp2::kOtg0Frame)) { st.halted = 1; r.pipe0Same = 0; r.pipeWhy = "OTG0 frame counter stopped"; return false; }
    if ((r.otg1Ctl & 1) && !phxp2::frameMoves(e, phxp2::kOtg1Frame)) { st.halted = 1; r.pipe1Same = 0; r.pipeWhy = "OTG1 frame counter stopped"; return false; }
    return true;
}

// ---- gates (reads only, plus the caps read over AUX: AUX engine registers only) ---------------------------------------
inline const char *gates(const Env &e, State &st, Run &r, int restartDone, const Aux &aux, const phxdpl::Conn **conn) {
    using namespace phxdmub;
    if (r.sel < 0 || (static_cast<uint32_t>(r.sel) & ~(kSelDig | kSelHbr))) return "selector: 0 = DP A, 4 = DP E (RBR x4); + 0x10 = HBR x4";
    const phxdpl::Conn *k = phxdpl::connByDig(static_cast<uint32_t>(r.sel) & kSelDig);
    if (!k) return "not a DP connector this check drives: DP A (0) or DP E (4) only (DP B = the GOP's pipe 0, HDMI D = pipe 1)";
    *conn = k;
    r.dig = k->dig;
    if (r.failed) return "a debug.phxdplink run failed in this boot: nothing more until reboot";
    if (st.halted) return "halted: pipe 0 / pipe 1 changed earlier in this boot";
    if (st.dmub.dead) return "debug.phxdmub_dead = 1";
    if (restartDone) return "debug.phxdmub_restart ran in this boot";
    if (st.switching) return "a pipe-1 mode switch is in progress";
    r.hpdStatus = rd(e, phxddc::kHpd0IntStatus + phxddc::kHpdStride * k->hpd);
    if (!(r.hpdStatus & 2u)) return "no sink: HPD sense low on this connector";
    const phxdpl::Regs g = phxdpl::regs(k->dig);
    r.beEn = rd(e, g.beEn); r.beCntl = rd(e, g.beCntl); r.linkCntl = rd(e, g.linkCntl); r.vidStream = rd(e, g.vidStream);
    r.symclk = rd(e, kSymclk0 + k->dig);
    if (r.beEn & 1u) return "lit: DIGn_DIG_BE_EN_CNTL bit 0 = 1 (the back end is on)";
    if (r.linkCntl & phxdpl::kLinkTrained) return "lit: DPn_DP_LINK_CNTL training complete";
    if (r.vidStream & phxdpl::kStreamEnable) return "lit: DPn_DP_VID_STREAM_CNTL stream enabled";
    for (unsigned d = 0; d <= 4; d++) {
        const phxdpl::Regs o = phxdpl::regs(d);
        if ((rd(e, o.beEn) & 1u) && ((rd(e, o.beCntl) >> 28) & 7u) == k->hpd) return "lit: another enabled DIG back end selects this connector's HPD";
    }
    r.cntl = rd(e, kCntl);
    if ((r.cntl & (kCntlEnable | kCntlPwait)) != (kCntlEnable | kCntlPwait)) return "DMCUB not enabled and idle (CNTL bit 16 / bit 20)";
    if (rd(e, kCntl2) & kSoftReset) return "DMCUB held in soft reset (CNTL2 bit 0)";
    r.scr0 = rd(e, kScratch0); r.scr1 = rd(e, kScratch1);
    if (!(r.scr0 & 2u)) return "SCRATCH0 bit 1 (mailbox ready) not set";
    if (r.scr1 != kFwVersion) return "SCRATCH1 is not the GOP firmware 0x07008742";
    if (rd(e, kGpintIn1)) return "GPINT_DATAIN1 not 0 (a GPINT in flight)";
    if ((static_cast<uint64_t>(rd(e, kDcnVmFbBase) & 0xffffff) << 24) != e.fbMcBase) return "DCN FB base != the kext's FB base";
    Ring ring;
    const char *w = nullptr;
    if (!ringGeometry(e.io, e.vramBytes, &ring, &w)) return w;
    if (ring.wptr != ring.rptr || ring.wptr >= ring.size || ring.wptr % kCmdBytes) return "ring not idle (INBOX1 WPTR != RPTR)";
    if (!phxp2::pipe0Abs(e, &w)) return w;
    for (unsigned j = 0; j < phxp2::kNFp; j++) r.fp0[j] = rd(e, phxp2::kFp[j].reg);
    r.odm0 = rd(e, phxp2::kOdm0Global) & phxp2::kOdmUnderflowBits;
    for (unsigned j = 0; j < kNFp1; j++) r.fp1[j] = rd(e, kFp1[j].reg);
    r.odm1 = rd(e, phxp2::kOdm1Global) & phxp2::kOdmUnderflowBits;
    r.otg1Ctl = r.fp1[0];
    if (!phxp2::frameMoves(e, phxp2::kOtg0Frame)) return "OTG0 frame counter not moving";
    if ((r.otg1Ctl & 1) && !phxp2::frameMoves(e, phxp2::kOtg1Frame)) return "OTG1 enabled but its frame counter is not moving";
    if (!r.c3Done[k->dig]) {
        r.gateDis2 = rd(e, phxc3::kGateDis2);
        if (r.gateDis2 & phxc3p::gateBit(k->dig)) return "C3 gate: DCCG_GATE_DISABLE_CNTL2 PHYnSYMCLK_GATE_DISABLE is set (the load restores it to 0)";
        r.crc = phxc3::imageCrc(kPhxC3Image, kPhxC3ImageWords);
        if (r.crc != kPhxC3ImageCrc32) return "C3 gate: the embedded image fails its CRC-32";
        Trace t;   // recorded only: a PHY n program in the firmware trace means the GOP set PHY n up (a reload is harmless)
        if (traceGeometry(e.io, e.vramBytes, &t, &w)) {
            const unsigned n = traceEntries(t.size, t.wptr);
            if (n != ~0u) {
                r.trOk = 1; r.trEntries = n;
                for (unsigned i = 0; i < n; i++) {
                    const uint32_t code = e.io.mmRd(e.io.c, t.vram + 16u + 16u * i), p0 = e.io.mmRd(e.io.c, t.vram + 24u + 16u * i);
                    r.trMainBegin += code == kTrMainBegin;
                    r.trPhyN += (code == kTrPllBegin || code == phxp2::kTrC3 || code == kTrPllOff || code == kTrPll10a) && p0 == k->dig;
                }
            }
        }
    }
    // the sink's caps (AUX only: no display register is written by this)
    r.capsRc = aux.rd(aux.c, k->aux, 0, r.caps, 16);
    if (r.capsRc) return "DPCD caps 0x000 not readable over AUX";
    phxdpl::Sink s{};
    if (!phxdpl::parseCaps(r.caps, &s)) return "DPCD caps not a DP sink's";
    r.link = phxdpl::Link{(static_cast<uint32_t>(r.sel) & kSelHbr) ? phxdpl::kHbr : phxdpl::kRbr, 4};
    if (s.maxLanes < 4 || s.maxRate < r.link.rate) return "the sink cannot take this link (4 lanes at RBR / HBR)";
    return nullptr;
}

// ---- the per-run Io for PhxDpLink (MMIO / DMUB / AUX through the lists and the guard) ---------------------------------
struct Ctx { const Env *e; State *st; Run *r; const phxdpl::Conn *k; Aux aux; };
inline uint32_t cRd(void *c, uint32_t reg) { Ctx &x = *static_cast<Ctx *>(c); return rd(*x.e, reg); }
inline void cWr(void *c, uint32_t reg, uint32_t v) {
    Ctx &x = *static_cast<Ctx *>(c);
    if (x.st->halted) { x.r->blocked++; return; }
    if (!phxdpl::writeAllowed(*x.k, reg)) { x.r->strayWr++; return; }
    x.e->io.wr(x.e->io.c, reg, v);
    x.r->mmioWr++;
    guard(*x.e, *x.st, *x.r);
}
inline bool txCheck(const uint32_t *c, const void *ctx, const char **why) { return phxdpl::txAllowed(c, *static_cast<const phxdpl::Conn *>(ctx), why); }
inline int cDmub(void *c, const uint32_t cmd[16]) {
    Ctx &x = *static_cast<Ctx *>(c);
    if (x.st->halted) { x.r->blocked++; return -1; }
    const char *w = nullptr;
    const int rc = phxdmub::submitWith(x.e->io, x.st->dmub, x.e->vramBytes, cmd, txCheck, x.k, &w);
    x.r->ringRc = rc;
    if (rc != phxdmub::kOk) { x.r->ringWhy = w ? w : phxdmub::rcName(rc); return -1; }
    x.r->dmubSent++;
    return guard(*x.e, *x.st, *x.r) ? 0 : -1;
}
inline int cAuxRd(void *c, uint32_t a, uint8_t *b, unsigned n) {
    Ctx &x = *static_cast<Ctx *>(c);
    x.r->auxRdN++;
    const int rc = x.aux.rd(x.aux.c, x.k->aux, a, b, n);
    if (rc) x.r->auxErr++;
    return rc;
}
inline int cAuxWr(void *c, uint32_t a, const uint8_t *b, unsigned n) {
    Ctx &x = *static_cast<Ctx *>(c);
    if (x.st->halted) { x.r->blocked++; return -1; }
    if (!phxdpl::dpcdWriteAllowed(a, n)) { x.r->dpcdStray++; return -1; }
    x.r->auxWrN++;
    const int rc = x.aux.wr(x.aux.c, x.k->aux, a, b, n);
    if (rc) x.r->auxErr++;
    return rc;
}
inline void cDelay(void *c, uint32_t us) { Ctx &x = *static_cast<Ctx *>(c); x.e->io.delayUs(x.e->io.c, us); }

// ---- C3 for PHY n ----------------------------------------------------------------------------------------------------
struct C3Ctx { unsigned i, n; };
inline bool c3Check(const uint32_t *c, const void *ctx, const char **why) {
    const C3Ctx &k = *static_cast<const C3Ctx *>(ctx);
    return phxc3p::cmdOkPhy(c, k.i, k.n, why);
}
inline int c3Phy(const Env &e, State &st, Run &r, unsigned n) {
    r.phase = 1;
    r.c3Ran = 1;
    uint32_t cmd[phxdmub::kCmdDw];
    for (unsigned i = 0; i < phxc3::kLoadN; i++) {
        r.c3Step = i;
        if (!phxc3p::loadCmdPhy(i, n, cmd)) return end(r, kC3Fail, "no command for this step");
        const C3Ctx k{i, n};
        const char *w = nullptr;
        const int rc = phxdmub::submitWith(e.io, st.dmub, e.vramBytes, cmd, c3Check, &k, &w);
        r.ringRc = rc;
        if (rc == phxdmub::kTimeout) { r.ringWhy = w; return end(r, kTimeout, "C3: RPTR did not catch up within 200 ms: debug.phxdmub_dead latched, warm reboot"); }
        if (rc != phxdmub::kOk) { r.ringWhy = w ? w : phxdmub::rcName(rc); return end(r, kNotSent, "C3: a command was not sent (shape or ring)"); }
        r.c3Ok++;
        const uint32_t us = st.dmub.lastUs;
        r.c3SumUs += us;
        if (us > r.c3MaxUs) r.c3MaxUs = us;
        if (!guard(e, st, r)) return end(r, kHalted, "pipe 0 / pipe 1 changed during the C3 load: STOP, halted until reboot");
        if (i == phxc3::kL9) {
            r.c3L9Us = us;
            if (us > kC3L9StopUs) return end(r, kC3Fail, "C3 L9 (RESERVED13 bit 28 SRAM_INIT_DONE) took > 10 ms: it never rose; stopped before the SRAM address");
        }
        if (i == phxc3::kProN - 1 && !framesMove(e, st, r)) return end(r, kHalted, "a frame counter stopped after the C3 prologue: STOP");
    }
    if (!framesMove(e, st, r)) return end(r, kHalted, "a frame counter stopped after the C3 load: STOP");
    r.c3Done[n] = 1;
    return kNone;
}

// ---- the run ---------------------------------------------------------------------------------------------------------
inline int run(const Env &e, State &st, Run &r, int sel, int restartDone, const Aux &aux) {
    const int failed = r.failed;
    uint8_t c3[5];
    for (int i = 0; i < 5; i++) c3[i] = r.c3Done[i];
    const uint32_t att = r.attempts + 1, refs = r.refusals, passes = r.passes;
    const char *lastRef = r.lastRefusal;
    r = Run{};
    r.failed = failed; r.attempts = att; r.refusals = refs; r.passes = passes; r.lastRefusal = lastRef;
    for (int i = 0; i < 5; i++) r.c3Done[i] = c3[i];
    r.sel = sel; r.dig = -1; r.pipe0Same = r.pipe1Same = 1;
    const phxdpl::Conn *k = nullptr;
    const char *g = gates(e, st, r, restartDone, aux, &k);
    if (g) return refuse(r, g);
    if (!r.c3Done[k->dig]) {
        const int c = c3Phy(e, st, r, k->dig);
        if (c != kNone) return c;
    }
    r.phase = 2;
    phxdpl::Sink s{};
    phxdpl::parseCaps(r.caps, &s);
    Ctx x{&e, &st, &r, k, aux};
    const phxdpl::Io io{&x, cRd, cWr, cDmub, cAuxRd, cAuxWr, cDelay};
    phxdpl::Opts o;
    o.esiRead = true;      // Linux's detection read ESI (Trace E 3485.039)
    o.attempts = 1;        // stop at the first failure
    r.vrc = phxdpl::verifyLink(io, *k, s, r.link, o, &r.rep);
    if (st.halted) return end(r, kHalted, "pipe 0 / pipe 1 changed during the training: STOP, halted until reboot (no link off sent)");
    if (st.dmub.dead) return end(r, kTimeout, "a TRANSMITTER_CONTROL timed out: debug.phxdmub_dead latched, no DISABLE sent; warm reboot");
    r.phase = 3;
    const phxdpl::Regs gr = phxdpl::regs(k->dig);
    r.postBeEn = rd(e, gr.beEn); r.postLink = rd(e, gr.linkCntl); r.postStream = rd(e, gr.vidStream);
    if (r.vrc != phxdpl::kOk) return end(r, kTrainFail, phxdpl::rcName(r.vrc));
    if (r.strayWr || r.dpcdStray) return end(r, kPostFail, "a write off the lists was dropped (stray): the sequence is not the planned one");
    if ((r.postBeEn & 1u) || (r.postLink & phxdpl::kLinkTrained) || (r.postStream & phxdpl::kStreamEnable))
        return end(r, kPostFail, "DIGn back end / DPn link not off again after the check");
    if (!guard(e, st, r) || !framesMove(e, st, r)) return end(r, kHalted, "pipe 0 / pipe 1 changed after the check: halted until reboot");
    return end(r, kPass, "trained (CR + EQ done, aligned) and turned off again");
}

inline int stat(const Run &r, char *b, int n) {
    int o = 0;
#define PHXDPR_OUT(...) do { if (o < n - 1) { const int k_ = snprintf(b + o, static_cast<size_t>(n - o), __VA_ARGS__); o += k_ > 0 ? k_ : 0; if (o > n - 1) o = n - 1; } } while (0)
    static const char *const ph[] = {"gates", "c3", "training", "post"};
    PHXDPR_OUT("result=%s sel=0x%x conn=%s phase=%s why=%s\n", kResName[r.res >= 0 && r.res <= kPostFail ? r.res : 0], r.sel,
               r.dig == 0 ? "DP-A" : r.dig == 4 ? "DP-E" : "-", ph[r.phase >= 0 && r.phase <= 3 ? r.phase : 0], r.why ? r.why : "-");
    PHXDPR_OUT("boot: attempts=%u passes=%u refusals=%u failed=%d c3_done A=%d E=%d last_refusal=%s\n", r.attempts, r.passes, r.refusals,
               r.failed, r.c3Done[0], r.c3Done[4], r.lastRefusal ? r.lastRefusal : "-");
    PHXDPR_OUT("gates: hpd_status=0x%x be_en=0x%x be_cntl=0x%08x link_cntl=0x%x vid_stream=0x%x symclk=0x%x dccg_gate_dis2=0x%08x "
               "dmcub_cntl=0x%x scratch0=0x%x scratch1=0x%08x otg1_ctl=0x%08x image_crc=0x%08x trace_ok=%d entries=%u main_begin=%u phy_entries=%u\n",
               r.hpdStatus, r.beEn, r.beCntl, r.linkCntl, r.vidStream, r.symclk, r.gateDis2, r.cntl, r.scr0, r.scr1, r.otg1Ctl, r.crc,
               r.trOk, r.trEntries, r.trMainBegin, r.trPhyN);
    PHXDPR_OUT("caps: rc=%d", r.capsRc);
    for (int i = 0; i < 16; i++) PHXDPR_OUT(" %02x", r.caps[i]);
    PHXDPR_OUT(" | link rate=0x%02x lanes=%u\n", r.link.rate, r.link.lanes);
    PHXDPR_OUT("c3: ran=%d loaded=%u/%u last_step=L%u sum_us=%u max_us=%u L9_us=%u\n", r.c3Ran, r.c3Ok, r.c3Ran ? phxc3::kLoadN : 0,
               r.c3Step + 1, r.c3SumUs, r.c3MaxUs, r.c3L9Us);
    PHXDPR_OUT("train: rc=%s cr_loops=%u eq_loops=%u vs=%u pe=%u pattern=0x%02x status(202..207)=%02x %02x %02x %02x %02x %02x after(200..205)="
               "%02x %02x %02x %02x %02x %02x delay_us=%u\n", phxdpl::rcName(r.vrc), r.rep.crLoops, r.rep.eqLoops, r.rep.vs, r.rep.pe,
               r.rep.pattern, r.rep.status[0], r.rep.status[1], r.rep.status[2], r.rep.status[3], r.rep.status[4], r.rep.status[5],
               r.rep.after[0], r.rep.after[1], r.rep.after[2], r.rep.after[3], r.rep.after[4], r.rep.after[5], r.rep.delayUs);
    PHXDPR_OUT("io: mmio_wr=%u dmub_sent=%u ring_rc=%d(%s) aux_rd=%u aux_wr=%u aux_err=%u stray_wr=%u dpcd_stray=%u blocked=%u\n", r.mmioWr,
               r.dmubSent, r.ringRc, r.ringWhy ? r.ringWhy : phxdmub::rcName(r.ringRc), r.auxRdN, r.auxWrN, r.auxErr, r.strayWr, r.dpcdStray, r.blocked);
    PHXDPR_OUT("post: be_en=0x%x link_cntl=0x%x vid_stream=0x%x | pipe0 same=%d pipe1 same=%d%s%s\n", r.postBeEn, r.postLink, r.postStream,
               r.pipe0Same, r.pipe1Same, r.pipeWhy ? " changed=" : "", r.pipeWhy ? r.pipeWhy : "");
#undef PHXDPR_OUT
    return o;
}

}  // namespace phxdpr
