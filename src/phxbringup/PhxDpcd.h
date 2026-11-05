// SPDX-License-Identifier: MIT
// PhxDpcd.h: kext b112, multiport stage S1a.
// b115: the AUX pad setup of PhxDdc.h is let through the Gate (exact one-bit RMWs only) and shown in the stat.
// One read-only DPCD snapshot of the DP sink on AUX engine n, for debug.phxdpcd (phx_dpcd.cpp). Kernel-free so a host
// test runs it against a model of the AUX engine and a sink.
//
// What it reads (DPCD native reads only, phxddc::dpcdRead, the PhxDdc.h AUX engine code the EDID reads use):
//   0x0000-0x000F receiver capabilities, 0x2200-0x220F extended receiver capabilities (DPCD 1.4),
//   0x0100-0x0108 link configuration, 0x0200-0x0207 sink count / IRQ vector / lane status / adjust requests.
// 0x2200 is read whatever 0x000E bit 7 says: a sink without the extended field ACKs zeros there (the Acer on DP B,
// read under Linux on 11-01; its 0x000E = 0x00). None of the four
// ranges is clear-on-read in the DP spec (the clear-on-read symbol error counters are 0x210-0x217, not read here) [F/I].
//
// What it never does: a DPCD or I2C-over-AUX write (the only AUX action is 0x90 = native read), link training, a DIG,
// PHY, OTG, HPD or clock register write, a DMUB/SMU/PSP/DAL message. The only registers written are the selected AUX
// engine's own block (CONTROL .. SW_DATA, 7 dwords) and, since b115, its DDC pad (PhxDdc.h auxPadSetup), exactly as
// auxOnce does for the EDID: SW arbitration request/release, SW_DONE_ACK, SW_CONTROL length, SW_DATA bytes, AUX_SW_GO,
// AUX_EN when it was clear, and the pad in AUX mode when it was not (DC_GPIO_DDC<n+1>_MASK bit 16 set, then
// DC_GPIO_AUX_CTRL_5 DDC_PAD<n+1>_I2CMODE cleared). The Gate drops and counts (stray_wr) any other write, and a pad
// write that changes anything but that one bit of that one register (it re-reads the register to check). AUX_EN is put
// back to 0 afterwards when it was 0 before; the pad stays in AUX mode, as Linux leaves it (PhxDdc.h, "DP AUX pad").
// Registers read: that AUX block, its DDC<n+1>_MASK, DC_GPIO_AUX_CTRL_5, HPDn_DC_HPD_INT_STATUS, DIG0-4
// BE_EN/BE_CNTL, OTG0/OTG1 CONTROL (a read outside that list is refused by the Gate and counted). None is in the
// forbidden MMHUB semaphore range.
//
// Refusals (decided before any register write; refused = zero writes):
//   - the selector is not 0-4 or 0x10 + (0-4);
//   - no DP connector of this board uses AUX n (phxddc::boardConnectors: AUX0 / AUX1 have none);
//   - the connector is lit (an enabled DIG back end selects its HPD) and the selector has no 0x10 (see "Lit engines");
//   - its HPD does not sense a sink (no AUX traffic on an empty connector, as Linux).
// AUX ownership is checked per transaction by auxOnce (ARB REG_RW_CNTL_STATUS 2 = the DMCU owns it: not driven).
//
// Lit engines (selector 0x10 + n). Reading DPCD on the engine of a lit DP pipe is harmless for these reasons, so it is
// allowed, but only on an explicit request:
//   - the same engine code already runs on the lit pipe-0 engine at every boot: phxEdidRead follows the DIG lit on OTG0
//     (b80, phxddc::scanoutSource) and reads the AOC's EDID over AUX2 while the GOP drives DP B (proven
//     on the box); a DPCD read differs only in the request's command nibble (0x90);
//   - Linux reads the same DPCD ranges on lit links: the AOC on DP A / DP E / DP B with the stream up
//     (Linux snapshots of 10-29 and 11-01) and in its plug
//     sequence (Linux plug trace 216.459-216.465: 0x0000 x16, 0x2200 x16);
//   - the reads change no sink state (above) and no source link register; the ARB check keeps it off an engine the
//     firmware holds at that moment (the GOP retrains DP B by itself on a replug, seen on the box).
//
// Time bound: every delay goes through the Gate, which sums it. Once kBudgetUs is used, the next transaction's first
// ARB read returns all-ones, which auxOnce refuses before any write (kErrArg); the run stops there with kErrBudget.
// Worst case = kBudgetUs + one transaction (auxOnce: <= 1 ms idle wait + <= 20 ms DONE wait) + one aux() retry delay
// (<= 1 ms), plus register access time; a normal run is a few ms (the host test checks the bound).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef KERNEL
#include <stdio.h>
#endif
#include "PhxDdc.h"

namespace phxdpcd {

struct Range { uint32_t addr; uint8_t n; const char *name; };
static const Range kRanges[] = {{0x0000, 16, "caps"}, {0x2200, 16, "ext"}, {0x0100, 9, "link"}, {0x0200, 8, "status"}};
constexpr unsigned kNRanges = sizeof kRanges / sizeof kRanges[0];
constexpr uint32_t kSelLitOk = 0x10;          // selector bit: also read when the connector is lit
constexpr uint32_t kBudgetUs = 100000;        // delay budget of one run (see the header)
constexpr int kErrBudget = -20;               // the run stopped: delay budget used up
constexpr int kNotRun = -21;                  // this range was not attempted (an earlier one stopped the run)

enum : int { kRan = 0, kRefArg = 1, kRefNoConn = 2, kRefLit = 3, kRefNoSink = 4 };
static const char *const kRefName[] = {"ran", "selector", "no-dp-connector", "lit", "no-sink"};

// The registers of the fingerprint (read before and after; a read-only run must leave them equal):
// DIG0-4 DIG_BE_EN_CNTL, DIG0-4 DIG_BE_CNTL, OTG0_OTG_CONTROL, OTG1_OTG_CONTROL.
constexpr uint32_t kOtg0Control = 0x05001, kOtg1Control = 0x05081;
constexpr unsigned kFpN = 12;
inline uint32_t fpReg(unsigned i) {
    return i < 5 ? phxddc::kDig0BeEnCntl + phxddc::kDigStride * i
         : i < 10 ? phxddc::kDig0BeCntl + phxddc::kDigStride * (i - 5)
         : i == 10 ? kOtg0Control : kOtg1Control;
}
inline uint32_t auxBase(unsigned eng) { return phxddc::kAux0 + phxddc::kAuxStride * eng; }
constexpr uint32_t kAuxRegs = 7;              // CONTROL +0 .. SW_DATA +6 (PhxDdc.h auxOnce)

// The Io one run uses: writes only inside AUX engine `eng`'s block, reads only from the list in the header, every delay
// summed against the budget (see "Time bound").
struct Gate {
    phxddc::Io in;
    unsigned eng;
    uint32_t budgetUs, usedUs;
    uint32_t strayWr, strayRd;
    bool inTxn, closed;
    uint32_t padWr;                                   // b115: pad writes let through (DDC<n+1>_MASK, AUX_CTRL_5)
};
inline bool gateReadOk(const Gate &g, uint32_t r) {
    if (r >= auxBase(g.eng) && r < auxBase(g.eng) + kAuxRegs) return true;
    if (r == phxddc::padMaskReg(g.eng) || r == phxddc::kAuxCtrl5) return true;
    for (unsigned n = 0; n < 5; n++) if (r == phxddc::kHpd0IntStatus + phxddc::kHpdStride * n) return true;
    for (unsigned i = 0; i < kFpN; i++) if (r == fpReg(i)) return true;
    return false;
}
inline uint32_t gateRd(void *c, uint32_t r) {
    Gate *g = static_cast<Gate *>(c);
    if (!gateReadOk(*g, r)) { g->strayRd++; return 0xFFFFFFFFu; }
    if (r == auxBase(g->eng) + 2 && !g->inTxn && g->usedUs >= g->budgetUs) { g->closed = true; return 0xFFFFFFFFu; }
    return g->in.rd(g->in.ctx, r);
}
inline void gateWr(void *c, uint32_t r, uint32_t v) {
    Gate *g = static_cast<Gate *>(c);
    const uint32_t b = auxBase(g->eng);
    if (r == phxddc::padMaskReg(g->eng) || r == phxddc::kAuxCtrl5) {   // b115: only "set AUX_PAD mode" / "clear own I2CMODE"
        const bool mask = r == phxddc::padMaskReg(g->eng);
        const uint32_t bit = mask ? phxddc::kAuxPadMode : phxddc::padI2cBit(g->eng);
        const uint32_t cur = g->in.rd(g->in.ctx, r);
        const bool ok = phxddc::auxPadOwned(g->eng) && (cur ^ v) == bit && ((v & bit) != 0) == mask;
        if (!ok) { g->strayWr++; return; }
        g->padWr++;
        g->in.wr(g->in.ctx, r, v);
        return;
    }
    if (r < b || r >= b + kAuxRegs) { g->strayWr++; return; }
    if (r == b + 2) {                                   // ARB: SW_USE_AUX_REG_REQ 16 / SW_DONE_USING_AUX_REG 17
        if (v & (1u << 17)) g->inTxn = false;
        else if (v & (1u << 16)) g->inTxn = true;
    }
    g->in.wr(g->in.ctx, r, v);
}
inline void gateDelay(void *c, uint32_t us) {
    Gate *g = static_cast<Gate *>(c);
    g->usedUs += us;
    g->in.delayUs(g->in.ctx, us);
}

struct Read {
    int rc;              // PhxDdc.h result, kErrBudget, or kNotRun
    uint8_t got;         // bytes valid in b
    uint8_t b[16];
    uint32_t status;     // the last AUX_SW_STATUS
    uint32_t retries;    // AUX defers retried
};
struct Result {
    uint32_t sel;
    int eng;
    int refused;          // kRan or a kRef*
    int path, hpd;        // the connector (-1 = none)
    bool sensed, lit;
    uint32_t litHpd;      // bit n = an enabled DIG back end selects HPD n
    uint32_t ctlBefore, ctlAfter, arbBefore;
    bool auxEnRestored;
    Read r[kNRanges];
    uint32_t fpBefore[kFpN], fpAfter[kFpN];
    bool fpSame;
    uint32_t strayWr, strayRd, usedUs;
    bool budgetHit;
    uint32_t padMaskBefore, padMaskAfter, auxCtrl5Before, auxCtrl5After, padWr;   // b115: the engine's DDC pad
};

// The DP connector of this board on AUX engine `eng` (nullptr = none).
inline const phxddc::Connector *dpConnectorOfAux(unsigned eng) {
    unsigned n = 0;
    const phxddc::Connector *t = phxddc::boardConnectors(&n);
    for (unsigned i = 0; t && i < n; i++) if (t[i].type == phxddc::kConnDp && t[i].line == eng) return &t[i];
    return nullptr;
}
// The board's socket names per display path (the VBIOS object table, paths 0-3; the bracket names from plug tests at
// each socket: "first DP" = DP A, "second DP" = DP E, the AOC's usual socket = DP B).
inline const char *pathName(int path) {
    static const char *const k[] = {"DP-A(first DP socket)", "DP-E(second DP socket)", "DP-B(AOC socket)", "HDMI-D"};
    return path >= 0 && path < 4 ? k[path] : "-";
}
// bit n = an enabled DIG back end (DIG_BE_EN_CNTL bit 0) whose DIG_BE_CNTL DIG_HPD_SELECT (30:28) = n, any DIG_MODE.
inline uint32_t litHpdMask(const uint32_t fp[kFpN]) {
    uint32_t m = 0;
    for (unsigned d = 0; d < 5; d++)
        if (fp[d] & 1u) { const unsigned h = (fp[5 + d] >> 28) & 7u; if (h < 5) m |= 1u << h; }
    return m;
}
inline bool stops(int rc) {   // the sink or the engine is not usable: no point in the next range
    return rc == phxddc::kErrTimeout || rc == phxddc::kErrHpd || rc == phxddc::kErrNotGranted || rc == phxddc::kErrArg ||
           rc == kErrBudget;
}

// One run. Returns kRan or the refusal (also in res->refused). See the header for every rule.
inline int run(const phxddc::Io &io, uint32_t sel, Result *res, uint32_t budgetUs = kBudgetUs) {
    *res = Result{};
    res->sel = sel; res->eng = -1; res->path = res->hpd = -1;
    for (unsigned i = 0; i < kNRanges; i++) res->r[i].rc = kNotRun;
    const unsigned eng = sel & 0xFu;
    if ((sel & ~(kSelLitOk | 0xFu)) || eng > 4) return res->refused = kRefArg;
    res->eng = static_cast<int>(eng);
    const phxddc::Connector *c = dpConnectorOfAux(eng);
    if (!c) return res->refused = kRefNoConn;
    res->path = c->path; res->hpd = c->hpd;
    Gate g{io, eng, budgetUs, 0, 0, 0, false, false, 0};
    const phxddc::Io gio{&g, gateRd, gateWr, gateDelay};
    for (unsigned i = 0; i < kFpN; i++) res->fpBefore[i] = gateRd(&g, fpReg(i));
    res->litHpd = litHpdMask(res->fpBefore);
    res->lit = ((res->litHpd >> c->hpd) & 1u) != 0;
    res->sensed = phxddc::hpdSensed(gio, c->hpd);
    if (res->lit && !(sel & kSelLitOk)) res->refused = kRefLit;
    else if (!res->sensed) res->refused = kRefNoSink;
    if (res->refused) { res->strayRd = g.strayRd; return res->refused; }
    const uint32_t ctl = auxBase(eng), arb = auxBase(eng) + 2;
    res->ctlBefore = gateRd(&g, ctl);
    res->arbBefore = gateRd(&g, arb);
    res->padMaskBefore = gateRd(&g, phxddc::padMaskReg(eng));
    res->auxCtrl5Before = gateRd(&g, phxddc::kAuxCtrl5);
    bool stop = false;
    for (unsigned i = 0; i < kNRanges && !stop; i++) {
        Read &r = res->r[i];
        phxddc::Diag d{};
        r.rc = phxddc::dpcdRead(gio, eng, kRanges[i].addr, r.b, kRanges[i].n, &d);
        if (g.closed) { r.rc = kErrBudget; res->budgetHit = true; }
        r.got = r.rc == phxddc::kOk ? kRanges[i].n : 0;
        r.status = d.status; r.retries = d.retries;
        stop = stops(r.rc);
    }
    if (!(res->ctlBefore & 1u)) {                       // AUX_EN was clear: put it back
        const uint32_t now = gateRd(&g, ctl);
        if (now & 1u) { gateWr(&g, ctl, now & ~1u); res->auxEnRestored = true; }
    }
    res->ctlAfter = gateRd(&g, ctl);
    res->padMaskAfter = gateRd(&g, phxddc::padMaskReg(eng));
    res->auxCtrl5After = gateRd(&g, phxddc::kAuxCtrl5);
    res->padWr = g.padWr;
    for (unsigned i = 0; i < kFpN; i++) res->fpAfter[i] = gateRd(&g, fpReg(i));
    res->fpSame = true;
    for (unsigned i = 0; i < kFpN; i++) if (res->fpAfter[i] != res->fpBefore[i]) res->fpSame = false;
    res->strayWr = g.strayWr; res->strayRd = g.strayRd; res->usedUs = g.usedUs;
    return kRan;
}

// The range read at addr (nullptr = not read or failed).
inline const uint8_t *bytesAt(const Result &r, uint32_t addr) {
    for (unsigned i = 0; i < kNRanges; i++) if (kRanges[i].addr == addr && r.r[i].rc == phxddc::kOk) return r.r[i].b;
    return nullptr;
}

// The stat text: one header line, one line per range, a decode line (fields of the ranges that were read).
inline int format(const Result &r, char *b, size_t n) {
    if (!b || !n) return 0;
    size_t at = 0;
#define PHXDPCD_PUT(...) do { if (at < n) { const int k_ = snprintf(b + at, n - at, __VA_ARGS__); if (k_ > 0) at += static_cast<size_t>(k_); } } while (0)
    const int ref = r.refused >= 0 && r.refused <= kRefNoSink ? r.refused : 0;
    PHXDPCD_PUT("sel=0x%02x eng=%d result=%s conn=%s path=%d hpd=%d sensed=%d lit=%d lit_hpd=0x%02x", r.sel, r.eng, kRefName[ref],
        pathName(r.path), r.path, r.hpd, r.sensed ? 1 : 0, r.lit ? 1 : 0, r.litHpd);
    if (r.refused == kRan)
        PHXDPCD_PUT(" aux_ctl=0x%08x->0x%08x aux_en_restored=%d arb=0x%08x used_us=%u budget_hit=%d stray_wr=%u stray_rd=%u fp_same=%d",
            r.ctlBefore, r.ctlAfter, r.auxEnRestored ? 1 : 0, r.arbBefore, r.usedUs, r.budgetHit ? 1 : 0, r.strayWr, r.strayRd,
            r.fpSame ? 1 : 0);
    PHXDPCD_PUT("\n");
    if (r.refused != kRan) return static_cast<int>(at < n ? at : n - 1);
    // b115: the DDC pad of the engine (PhxDdc.h auxPadSetup): already in AUX mode (nothing written), or before->after
    if (r.padMaskBefore & phxddc::kAuxPadMode)
        PHXDPCD_PUT("pad: already aux ddc%d_mask 0x%x aux_ctrl5 0x%x pad_wr=%u\n", r.eng + 1, r.padMaskBefore, r.auxCtrl5Before, r.padWr);
    else
        PHXDPCD_PUT("pad: ddc%d_mask 0x%x->0x%x aux_ctrl5 0x%x->0x%x pad_wr=%u\n", r.eng + 1, r.padMaskBefore, r.padMaskAfter,
                    r.auxCtrl5Before, r.auxCtrl5After, r.padWr);
    for (unsigned i = 0; i < kNRanges; i++) {
        const Read &x = r.r[i];
        PHXDPCD_PUT("%-6s 0x%04x rc=%d defers=%u status=0x%08x:", kRanges[i].name, kRanges[i].addr, x.rc, x.retries, x.status);
        for (unsigned j = 0; j < x.got; j++) PHXDPCD_PUT(" %02x", x.b[j]);
        PHXDPCD_PUT("\n");
    }
    const uint8_t *caps = bytesAt(r, 0x0000), *ext = bytesAt(r, 0x2200), *link = bytesAt(r, 0x0100), *st = bytesAt(r, 0x0200);
    PHXDPCD_PUT("decode");
    if (caps) PHXDPCD_PUT(" rev=%u.%u max_rate=0x%02x max_lanes=%u tps3=%u enh_frame=%u ext_caps=%u", caps[0] >> 4, caps[0] & 0xF, caps[1],
                  caps[2] & 0x1F, (caps[2] >> 6) & 1, caps[2] >> 7, caps[0xE] >> 7);
    if (ext) PHXDPCD_PUT(" ext_rev=%u.%u ext_max_rate=0x%02x", ext[0] >> 4, ext[0] & 0xF, ext[1]);
    if (link) PHXDPCD_PUT(" link_rate=0x%02x lane_count=%u enh=%u tps=0x%02x", link[0], link[1] & 0x1F, link[1] >> 7, link[2]);
    if (st) PHXDPCD_PUT(" sink_count=%u irq=0x%02x lane01=0x%02x lane23=0x%02x align=0x%02x sink_status=0x%02x", st[0] & 0x3F, st[1], st[2],
                st[3], st[4], st[5]);
    PHXDPCD_PUT("\n");
    if (!r.fpSame)
        for (unsigned i = 0; i < kFpN; i++)
            if (r.fpBefore[i] != r.fpAfter[i]) PHXDPCD_PUT("fp_changed 0x%05x 0x%08x->0x%08x\n", fpReg(i), r.fpBefore[i], r.fpAfter[i]);
    return static_cast<int>(at < n ? at : n - 1);
#undef PHXDPCD_PUT
}

}  // namespace phxdpcd
