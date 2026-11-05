// SPDX-License-Identifier: MIT
// PhxDpWatch.h: kext b120, multiport S3 step 0: a READ-ONLY watcher of
// the DP connectors' stream state, built to catch the intermittent dark AOC after a DP B replug (seen on the box:
// the GOP firmware re-enabled DIG1 / DP1, DP1_DP_VID_STREAM_CNTL = 0x201 = ENABLE 1 with DP_VID_STREAM_STATUS 0 for > 40 s;
// 1 of 5 firmware replugs). It counts plugs, unplugs, sink IRQs (RX_INT), stream starts and
// their STATUS latency, and records a "STUCK" event with a register snapshot when a stream is enabled on a trained link
// with HPD high for >= 1 s and STATUS stays 0 for >= 500 ms. It never writes anything: the Io has no write callback, and
// it never acks the sticky RX_INT bit (the GOP firmware may own it on DP B). The fix (Linux's FIFO reset + stream
// re-enable) is a later, separate step.
// Kernel-free; glue phx_dpwatch.cpp (debug.phxdpwatch / debug.phxdpwatch_stat, a 250 ms thread call); host-tested
// (synthetic timelines + replays of the box's own hpdwatch logs).
// b122: still reads only. (1) "fifo-err" events: a stream up (STATUS 1) with
// DIG_FIFO_ERROR != 0 for >= 500 ms is recorded, never acted on: the same bits were read on bright replugs
// (two 11-03 snapshots, the 11-04 14:59:59 P9 replug) and on the dark bounce of a hot-plug test, so it
// does not tell dark from bright. (2) wide snapshots (kSnapRegs: OTG0 status / counters, DIG1 FE / CRC / FIFO levels / BE,
// DP1 link / stream / steer / M / N / CRC / MSA, DCCG FIFO error detect, ODM0 / HUBP0 underflow): one at the first sample,
// one at each DP B plug edge and 2 s and 10 s after it (debug.phxdpwatch_snap), to find a register that does.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef KERNEL
#include <stdio.h>
#endif

namespace phxdpw {

struct RoIo { void *c; uint32_t (*rd)(void *c, uint32_t reg); };   // reads only: there is no write path

// The DP connectors of this board (DIG n = PHY n; HPD line): DP B = the GOP's AOC socket.
struct WConn { const char *name; uint8_t dig, hpd; };
static constexpr WConn kConns[] = {{"DP-B", 1, 2}, {"DP-A", 0, 3}, {"DP-E", 4, 4}};
static constexpr unsigned kN = sizeof kConns / sizeof kConns[0];

// Absolute dwords (dcn_3_2_0_offset.h; = PhxDpLink.h regs() / PhxHpd.h): DIGn stride 0x100, HPDn stride 8.
inline constexpr uint32_t hpdStatus(unsigned n) { return 0x053D4 + 8 * n; }
inline constexpr uint32_t digFe(unsigned d) { return 0x0554B + 0x100 * d; }
inline constexpr uint32_t digFifo(unsigned d) { return 0x05551 + 0x100 * d; }
inline constexpr uint32_t digBeCntl(unsigned d) { return 0x05571 + 0x100 * d; }
inline constexpr uint32_t digBeEn(unsigned d) { return 0x05572 + 0x100 * d; }
inline constexpr uint32_t dpLink(unsigned d) { return 0x055C8 + 0x100 * d; }
inline constexpr uint32_t dpVid(unsigned d) { return 0x055CC + 0x100 * d; }
constexpr uint32_t kOutbox0Wptr = 0x0369A;   // the GOP firmware's trace ring write pointer (PhxDmub.h)
static_assert(hpdStatus(2) == 0x053E4 && digFe(1) == 0x0564B && digBeEn(1) == 0x05672 && dpLink(1) == 0x056C8 &&
              dpVid(1) == 0x056CC && digFifo(1) == 0x05651 && digBeCntl(1) == 0x05671, "DP B = hpdwatch.sh / rt-snap addresses");
static_assert(digFifo(4) == 0x05951 && digBeCntl(4) == 0x05971 && dpVid(4) == 0x059CC, "DP E = PhxDpLink.h regs(4)");
// Every register this file reads (the host test checks the list against the MMVM semaphores and the sample function).
inline bool readAllowed(uint32_t r) {
    if (r >= 0x1A751 && r <= 0x1A762) return false;   // MMVM_INVALIDATE_ENG*_SEM (forbidden: never read)
    if (r == kOutbox0Wptr) return true;
    for (const WConn &k : kConns)
        if (r == hpdStatus(k.hpd) || r == digFe(k.dig) || r == digFifo(k.dig) || r == digBeCntl(k.dig) || r == digBeEn(k.dig) ||
            r == dpLink(k.dig) || r == dpVid(k.dig)) return true;
    return false;
}

// Field bits (dcn_3_2_0_sh_mask.h)
constexpr uint32_t kHpdSense = 0x2u, kHpdRxInt = 0x100u;          // DC_HPD_SENSE, DC_HPD_RX_INT_STATUS (sticky, never acked here)
constexpr uint32_t kBeOn = 0x1u, kLinkTrained = 0x10u;            // DIG_BE_EN_CNTL bit 0, DP_LINK_TRAINING_COMPLETE
constexpr uint32_t kVidEnable = 0x1u, kVidStatus = 0x10000u;       // DP_VID_STREAM_ENABLE, DP_VID_STREAM_STATUS
constexpr uint32_t kArmMs = 1000, kStuckMs = 500;                   // HPD high this long; STATUS 0 this long = stuck
constexpr uint32_t kFifoError = 0x30000000u;                       // b122 DIG_FIFO_ERROR (DIG1_DIG_FIFO_CTRL0, dcn_3_2_0_sh_mask.h)
constexpr uint32_t kFifoErrMs = 500;                               // b122: error set with the stream up this long = a fifo-err event

// b122: the wide snapshot, reads only. Every address = dcn_3_2_0_offset.h reg* + its base (DCN 0x34c0 / DCCG 0xc0);
// (rt) = already read on the box by the 11-03 snapshots.
struct SnapReg { uint32_t reg; const char *name; };
static constexpr SnapReg kSnapRegs[] = {
    {0x053E4, "HPD2_STATUS"},
    {0x05001, "OTG0_CONTROL"}, {0x05009, "OTG0_STATUS"}, {0x0500A, "OTG0_STATUS_POSITION"}, {0x0500C, "OTG0_FRAME_COUNT(rt)"},
    {0x0500D, "OTG0_VF_COUNT"}, {0x0500E, "OTG0_HV_COUNT"}, {0x04F8A, "ODM0_OPTC_INPUT_GLOBAL_CONTROL"}, {0x03AB3, "HUBP0_DCHUBP_CNTL"},
    {0x00124, "DENTIST_DISPCLK_CNTL"}, {0x0012F, "OTG_PIXEL_RATE_DIV(rt)"}, {0x00131, "DISPCLK_FREQ_CHANGE_CNTL(FIFO_ERRDET)"},
    {0x00141, "DP_DTO0_PHASE(rt)"}, {0x00142, "DP_DTO0_MODULO(rt)"},
    {0x0564B, "DIG1_FE_CNTL(rt)"}, {0x0564C, "DIG1_OUTPUT_CRC_CNTL"}, {0x0564D, "DIG1_OUTPUT_CRC_RESULT"},
    {0x05651, "DIG1_FIFO_CTRL0(rt)"}, {0x05652, "DIG1_FIFO_CTRL1(levels)"}, {0x05671, "DIG1_BE_CNTL(rt)"}, {0x05672, "DIG1_BE_EN(rt)"},
    {0x056C8, "DP1_LINK_CNTL(rt)"}, {0x056C9, "DP1_PIXEL_FORMAT(rt)"}, {0x056CB, "DP1_CONFIG(rt)"}, {0x056CC, "DP1_VID_STREAM_CNTL(rt)"},
    {0x056CD, "DP1_STEER_FIFO(rt)"}, {0x056CE, "DP1_MSA_MISC(rt)"}, {0x056D0, "DP1_VID_TIMING(rt)"}, {0x056D1, "DP1_VID_N(rt)"},
    {0x056D2, "DP1_VID_M(rt)"}, {0x056D3, "DP1_LINK_FRAMING_CNTL(rt)"}, {0x056D5, "DP1_VID_MSA_VBID"}, {0x056D6, "DP1_VID_INTERRUPT_CNTL"},
    {0x056D7, "DP1_DPHY_CNTL(rt)"}, {0x056D8, "DP1_DPHY_TRAINING_PATTERN_SEL(rt)"}, {0x056DE, "DP1_DPHY_SCRAM_CNTL(rt)"},
    {0x056DF, "DP1_DPHY_CRC_EN"}, {0x056E0, "DP1_DPHY_CRC_CNTL"}, {0x056E1, "DP1_DPHY_CRC_RESULT"}, {0x056E5, "DP1_DPHY_FAST_TRAINING_STATUS"},
    {0x0570C, "DP1_MSA_TIMING_PARAM1"}, {0x0570D, "DP1_MSA_TIMING_PARAM2"}, {0x0570E, "DP1_MSA_TIMING_PARAM3"}, {0x0570F, "DP1_MSA_TIMING_PARAM4"},
    {0x0369A, "DMCUB_OUTBOX0_WPTR"},
};
static constexpr unsigned kNSnap = sizeof kSnapRegs / sizeof kSnapRegs[0];
inline bool snapAllowed(uint32_t r) {
    if (r >= 0x1A751 && r <= 0x1A762) return false;   // MMVM_INVALIDATE_ENG*_SEM (forbidden: never read)
    for (const SnapReg &k : kSnapRegs) if (k.reg == r) return true;
    return false;
}
enum SnapTag : uint8_t { kSnapStart = 0, kSnapPlug, kSnapPlus2, kSnapPlus10, kSnapNow };
static const char *const kSnapTagName[] = {"start", "plug+0", "plug+2s", "plug+10s", "now"};
constexpr uint32_t kSnap2Ms = 2000, kSnap10Ms = 10000;
struct Snap { uint64_t tMs; uint32_t plug; uint8_t tag; uint32_t v[kNSnap]; };
static constexpr unsigned kSnapRing = 8;   // the start one + the last plugs' three (oldest dropped)

struct Raw { uint32_t hpd, fe, fifo, beCntl, beEn, link, vid; };
struct Sample { Raw c[kN]; uint32_t out0; };
inline uint32_t rd(const RoIo &io, uint32_t r) { return readAllowed(r) ? io.rd(io.c, r) : 0; }
inline void sample(const RoIo &io, Sample *s) {
    for (unsigned i = 0; i < kN; i++) {
        const WConn &k = kConns[i];
        s->c[i] = Raw{rd(io, hpdStatus(k.hpd)), rd(io, digFe(k.dig)), rd(io, digFifo(k.dig)), rd(io, digBeCntl(k.dig)),
                      rd(io, digBeEn(k.dig)), rd(io, dpLink(k.dig)), rd(io, dpVid(k.dig))};
    }
    s->out0 = rd(io, kOutbox0Wptr);
}

enum Kind : uint8_t { kEvNone = 0, kEvPlug, kEvUnplug, kEvRxInt, kEvStreamOn, kEvStatusUp, kEvStuck, kEvRecovered, kEvStreamOff,
                     kEvFixOk, kEvFixFail, kEvFixRefused,   // b121: PhxDpbFix.h (level 2)
                     kEvFifoErr, kEvFifoErrEnd };           // b122: observation only
static const char *const kKindName[] = {"-", "plug", "unplug", "rx-int", "stream-on", "status-up", "STUCK", "recovered", "stream-off",
                                        "FIX-OK", "FIX-FAIL", "fix-refused", "fifo-err", "fifo-err-cleared"};
struct Event { uint64_t tMs; uint8_t conn, kind; uint32_t dtMs; Raw r; uint32_t out0, out0QuietMs; };
static constexpr unsigned kRing = 24;

struct Conn {
    int init, hpd, rx, on, status, stuck;
    uint64_t hpdSinceMs, onSinceMs, lowSinceMs, stuckAtMs;
    uint32_t plugs, unplugs, rxInts, starts, statusUps, stuckN, recovered, stuckOff;   // stuckOff: stuck ended by stream off / unplug
    uint32_t lastLatMs, maxLatMs;
    Raw last;
    // b121 (level 2, DP B only): one fix try per plug edge
    int fixTried;
    uint32_t fixRuns, fixOk, fixFail, fixRefused, fixBusy, lastFixWaitUs;
    // b122 (observation): stream up + DIG_FIFO_ERROR != 0 for >= kFifoErrMs
    int ferr, ferrCond;
    uint64_t ferrSinceMs, ferrAtMs;
    uint32_t ferrN, ferrCleared, ferrOff;
};
struct Watch {
    uint32_t samples; uint64_t firstMs, lastMs;
    uint32_t out0; uint64_t out0ChangedMs; int out0Init;
    Conn c[kN];
    Event ring[kRing]; unsigned head, count; uint32_t events;
    // b122 snapshots (DP B): what is due (bit per tag) and since which plug edge
    unsigned snapDue; uint64_t snapPlugMs; uint32_t snapPlug;
    Snap snap[kSnapRing]; unsigned snapHead, snapCount; uint32_t snaps;
};

inline void push(Watch &w, uint64_t now, unsigned i, uint8_t kind, uint32_t dt, const Raw &r) {
    Event &e = w.ring[w.head];
    e = Event{now, static_cast<uint8_t>(i), kind, dt, r, w.out0, static_cast<uint32_t>(now - w.out0ChangedMs)};
    w.head = (w.head + 1) % kRing;
    if (w.count < kRing) w.count++;
    w.events++;
}
// A stream "on": the DIG back end on, the DP link trained and the stream enabled (what the firmware's re-enable leaves).
inline bool streamOn(const Raw &r) { return (r.beEn & kBeOn) && (r.link & kLinkTrained) && (r.vid & kVidEnable); }

// One sample at time `now` (ms, monotonic). Pure: no I/O.
inline void step(Watch &w, const Sample &s, uint64_t now) {
    if (!w.samples) w.firstMs = now;
    w.samples++; w.lastMs = now;
    if (!w.out0Init || s.out0 != w.out0) { w.out0 = s.out0; w.out0ChangedMs = now; w.out0Init = 1; }
    for (unsigned i = 0; i < kN; i++) {
        Conn &c = w.c[i];
        const Raw &r = s.c[i];
        const int hpd = (r.hpd & kHpdSense) != 0, rx = (r.hpd & kHpdRxInt) != 0, on = streamOn(r) && hpd, st = (r.vid & kVidStatus) != 0;
        if (!c.init) {   // the first sample: the state as found (a lit link counts as armed and started, no events)
            c.init = 1; c.hpd = hpd; c.rx = rx; c.on = on; c.status = st;
            c.hpdSinceMs = now >= kArmMs ? now - kArmMs : 0; c.onSinceMs = now; c.lowSinceMs = now; c.last = r;
            c.ferrCond = on && st && (r.fifo & kFifoError); c.ferrSinceMs = now;
            if (i == 0) w.snapDue |= 1u << kSnapStart;
            continue;
        }
        if (hpd != c.hpd) {
            c.hpd = hpd; c.hpdSinceMs = now;
            if (hpd) {
                c.plugs++; c.fixTried = 0; push(w, now, i, kEvPlug, 0, r);
                if (i == 0) { w.snapDue = (w.snapDue & (1u << kSnapStart)) | 1u << kSnapPlug | 1u << kSnapPlus2 | 1u << kSnapPlus10; w.snapPlugMs = now; w.snapPlug = c.plugs; }
            } else { c.unplugs++; push(w, now, i, kEvUnplug, 0, r); }
        }
        if (rx && !c.rx) { c.rxInts++; push(w, now, i, kEvRxInt, 0, r); }
        c.rx = rx;
        if (on && !c.on) { c.starts++; c.onSinceMs = now; c.lowSinceMs = now; push(w, now, i, kEvStreamOn, 0, r); c.status = 0; }
        if (on) {
            if (st && !c.status) {
                const uint32_t lat = static_cast<uint32_t>(now - c.lowSinceMs);
                c.statusUps++; c.lastLatMs = lat; if (lat > c.maxLatMs) c.maxLatMs = lat;
                if (c.stuck) { c.stuck = 0; c.recovered++; push(w, now, i, kEvRecovered, static_cast<uint32_t>(now - c.stuckAtMs), r); }
                else push(w, now, i, kEvStatusUp, lat, r);
            }
            if (!st && c.status) c.lowSinceMs = now;   // STATUS fell while the stream stays on
            c.status = st;
            if (!st && !c.stuck && now - c.hpdSinceMs >= kArmMs && now - c.lowSinceMs >= kStuckMs) {
                c.stuck = 1; c.stuckAtMs = now; c.stuckN++;
                push(w, now, i, kEvStuck, static_cast<uint32_t>(now - c.lowSinceMs), r);
            }
        } else if (c.on) {
            if (c.stuck) { c.stuck = 0; c.stuckOff++; }
            push(w, now, i, kEvStreamOff, static_cast<uint32_t>(now - c.onSinceMs), r);
            c.status = st;
        }
        // b122 fifo-err (observation only): the stream up with DIG_FIFO_ERROR set, continuously, after the 1 s arm
        const bool fc = on && st && (r.fifo & kFifoError);
        if (fc && !c.ferrCond) c.ferrSinceMs = now;
        c.ferrCond = fc;
        if (fc && !c.ferr && now - c.hpdSinceMs >= kArmMs && now - c.ferrSinceMs >= kFifoErrMs) {
            c.ferr = 1; c.ferrAtMs = now; c.ferrN++;
            push(w, now, i, kEvFifoErr, static_cast<uint32_t>(now - c.ferrSinceMs), r);
        } else if (!fc && c.ferr) {
            c.ferr = 0;
            if (on && st) { c.ferrCleared++; push(w, now, i, kEvFifoErrEnd, static_cast<uint32_t>(now - c.ferrAtMs), r); }
            else c.ferrOff++;
        }
        c.on = on;
        c.last = r;
    }
}

// b122: the snapshot due now (a tag), or -1. A plug edge is due in the same poll; +2 s / +10 s at the first poll at or
// after that time; an unplug before +10 s keeps them due (a bounce shows what follows); a new plug edge restarts them.
inline int snapWhich(const Watch &w, uint64_t now) {
    if (w.snapDue & (1u << kSnapStart)) return kSnapStart;
    if (w.snapDue & (1u << kSnapPlug)) return kSnapPlug;
    if ((w.snapDue & (1u << kSnapPlus2)) && now - w.snapPlugMs >= kSnap2Ms) return kSnapPlus2;
    if ((w.snapDue & (1u << kSnapPlus10)) && now - w.snapPlugMs >= kSnap10Ms) return kSnapPlus10;
    return -1;
}
inline void snapRead(const RoIo &io, Snap &s, uint8_t tag, uint32_t plug, uint64_t now) {
    s.tMs = now; s.tag = tag; s.plug = plug;
    for (unsigned k = 0; k < kNSnap; k++) s.v[k] = snapAllowed(kSnapRegs[k].reg) ? io.rd(io.c, kSnapRegs[k].reg) : 0;
}
// Takes every snapshot due now (reads only; at most one per tag per call).
inline unsigned snapPoll(const RoIo &io, Watch &w, uint64_t now) {
    unsigned n = 0;
    for (int t; (t = snapWhich(w, now)) >= 0; n++) {
        w.snapDue &= ~(1u << t);
        snapRead(io, w.snap[w.snapHead], static_cast<uint8_t>(t), t == kSnapStart ? 0 : w.snapPlug, now);
        w.snapHead = (w.snapHead + 1) % kSnapRing;
        if (w.snapCount < kSnapRing) w.snapCount++;
        w.snaps++;
    }
    return n;
}
inline int snapFormatOne(const Snap &s, uint64_t now, char *b, int n) {
    int o = 0;
#define PHXDPW_SOUT(...) do { if (o < n - 1) { const int k_ = snprintf(b + o, static_cast<size_t>(n - o), __VA_ARGS__); o += k_ > 0 ? k_ : 0; if (o > n - 1) o = n - 1; } } while (0)
    PHXDPW_SOUT("snap %s plug=%u t=-%llums:", kSnapTagName[s.tag <= kSnapNow ? s.tag : 0], s.plug, static_cast<unsigned long long>(now - s.tMs));
    for (unsigned k = 0; k < kNSnap; k++) PHXDPW_SOUT(" %05X=%08x", kSnapRegs[k].reg, s.v[k]);
    PHXDPW_SOUT("\n");
#undef PHXDPW_SOUT
    return o;
}
// debug.phxdpwatch_snap: the register legend, the ring (oldest first), then `fresh` (a read made now) when given.
inline int snapFormat(const Watch &w, const Snap *fresh, uint64_t now, char *b, int n) {
    int o = 0;
#define PHXDPW_SOUT(...) do { if (o < n - 1) { const int k_ = snprintf(b + o, static_cast<size_t>(n - o), __VA_ARGS__); o += k_ > 0 ? k_ : 0; if (o > n - 1) o = n - 1; } } while (0)
    PHXDPW_SOUT("dpwatch snap v1 (reads only) snaps=%u kept=%u regs=%u due=0x%x plug=%u\nregs:", w.snaps, w.snapCount, kNSnap, w.snapDue, w.snapPlug);
    for (unsigned k = 0; k < kNSnap; k++) PHXDPW_SOUT(" %05X=%s", kSnapRegs[k].reg, kSnapRegs[k].name);
    PHXDPW_SOUT("\n");
#undef PHXDPW_SOUT
    for (unsigned k = 0; k < w.snapCount && o < n - 1; k++) o += snapFormatOne(w.snap[(w.snapHead + kSnapRing - w.snapCount + k) % kSnapRing], now, b + o, n - o);
    if (fresh && o < n - 1) o += snapFormatOne(*fresh, now, b + o, n - o);
    return o;
}

// b121: whether level 2 should try the fix now: DP B (index 0) STUCK, not tried since its last plug, and the GOP firmware's
// trace ring quiet for >= 500 ms (its own re-enable is over). The caller (PhxDpbFix.h) re-checks every register first.
constexpr uint32_t kFixQuietMs = 500;
inline bool wantFix(const Watch &w, int level, uint64_t now) {
    const Conn &c = w.c[0];
    return level == 2 && c.stuck && !c.fixTried && now - w.out0ChangedMs >= kFixQuietMs;
}
// The fix's outcome into the watcher: counters + one event (kEvFixOk / kEvFixFail / kEvFixRefused); `busy` = the display
// lock was held (not a try: the next poll tries again).
inline void fixResult(Watch &w, uint64_t now, int kind, uint32_t waitUs, const Raw &r) {
    Conn &c = w.c[0];
    if (kind == kEvFixOk) { c.fixTried = 1; c.fixRuns++; c.fixOk++; c.lastFixWaitUs = waitUs; }
    else if (kind == kEvFixFail) { c.fixTried = 1; c.fixRuns++; c.fixFail++; c.lastFixWaitUs = waitUs; }
    else if (kind == kEvFixRefused) { c.fixTried = 1; c.fixRefused++; }
    else { c.fixBusy++; return; }
    push(w, now, 0, static_cast<uint8_t>(kind), waitUs, r);
}

inline int format(const Watch &w, int level, uint64_t now, char *b, int n) {
    int o = 0;
#define PHXDPW_OUT(...) do { if (o < n - 1) { const int k_ = snprintf(b + o, static_cast<size_t>(n - o), __VA_ARGS__); o += k_ > 0 ? k_ : 0; if (o > n - 1) o = n - 1; } } while (0)
    PHXDPW_OUT("dpwatch v1 level=%d samples=%u span_ms=%llu events=%u out0=0x%x out0_quiet_ms=%llu (reads only, nothing acked)\n", level,
               w.samples, static_cast<unsigned long long>(w.samples ? w.lastMs - w.firstMs : 0), w.events, w.out0,
               static_cast<unsigned long long>(w.samples ? now - w.out0ChangedMs : 0));
    for (unsigned i = 0; i < kN; i++) {
        const Conn &c = w.c[i];
        PHXDPW_OUT("%s: hpd=%d on=%d status=%d stuck=%d plugs=%u unplugs=%u rx_int=%u starts=%u status_up=%u STUCK=%u recovered=%u stuck_ended_off=%u "
                   "fifo_err=%d FIFO_ERR=%u fifo_err_cleared=%u fifo_err_ended_off=%u lat_ms last=%u max=%u | be_en=0x%x link=0x%x vid=0x%08x fe=0x%08x fifo=0x%08x be_cntl=0x%08x hpd_st=0x%08x\n",
                   kConns[i].name, c.hpd, c.on, c.status, c.stuck, c.plugs, c.unplugs, c.rxInts, c.starts, c.statusUps, c.stuckN, c.recovered,
                   c.stuckOff, c.ferr, c.ferrN, c.ferrCleared, c.ferrOff, c.lastLatMs, c.maxLatMs, c.last.beEn, c.last.link, c.last.vid, c.last.fe, c.last.fifo, c.last.beCntl, c.last.hpd);
        if (i == 0)
            PHXDPW_OUT("DP-B fix (level 2): tried_this_plug=%d runs=%u ok=%u fail=%u refused=%u busy_skips=%u last_wait_us=%u\n", c.fixTried,
                       c.fixRuns, c.fixOk, c.fixFail, c.fixRefused, c.fixBusy, c.lastFixWaitUs);
    }
    for (unsigned k = 0; k < w.count; k++) {
        const Event &e = w.ring[(w.head + kRing - w.count + k) % kRing];
        PHXDPW_OUT("ev t=-%llums %s %s dt_ms=%u vid=0x%08x be_en=0x%x link=0x%x fe=0x%08x fifo=0x%08x be_cntl=0x%08x out0=0x%x out0_quiet_ms=%u\n",
                   static_cast<unsigned long long>(now - e.tMs), kConns[e.conn].name, kKindName[e.kind], e.dtMs, e.r.vid, e.r.beEn, e.r.link,
                   e.r.fe, e.r.fifo, e.r.beCntl, e.out0, e.out0QuietMs);
    }
#undef PHXDPW_OUT
    return o;
}

}  // namespace phxdpw
