// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxhp0.h (PhxAccel hp0): hot-plug for head 0 (the AOC on a DP connector).
//
// Head 0 is PhxAMDFB's console framebuffer: whatever OTG0 drives. The GOP firmware owns DP B's link: on a pull it turns
// DIG1 / DP1 off by itself and on a replug it retrains and restores them, while OTG0 keeps scanning (seen
// on AOC pulls and replugs). So macOS only has to be told: head 0 reports its connection offline when
// every DP connector that can carry OTG0 has had HPD low for 6 s, and online again once one of them has had HPD high for
// 1 s AND carries a running stream fed from OTG0 (DIG back end on, link trained, stream enabled, STATUS up, FE source
// OTG0) for 500 ms, and (opts bit 0) the sink's EDID block 0 equals the one head 0 serves. A dark replug (the firmware's
// 1-in-5 "stream on, STATUS never", S3 detector) therefore never brings a black display back as the main one.
//
// Kernel-free (stdint + snprintf) so a host test runs it. The glue (phxhotplug0.cpp,
// part of phxfb.cpp's translation unit) only supplies the register reads and the head calls. Reads only: there is no write
// callback in RoIo, and readAllowed() names every register this file may read (never the MMVM semaphores).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef KERNEL
#include <stdio.h>
#include <string.h>
#endif

namespace phxhp0 {

// ---- registers (absolute BAR5 dwords = DCN segment 0x34C0 + ref/linux/dcn_3_2_0_offset.h) ---------------------------
constexpr uint32_t hpdStatus(unsigned n) { return 0x053D4u + 8u * n; }     // HPDn_DC_HPD_INT_STATUS
constexpr uint32_t digFe(unsigned d) { return 0x0554Bu + 0x100u * d; }     // DIGn_DIG_FE_CNTL (regDIG0_DIG_FE_CNTL 0x208b)
constexpr uint32_t digBeEn(unsigned d) { return 0x05572u + 0x100u * d; }   // DIGn_DIG_BE_EN_CNTL
constexpr uint32_t dpLink(unsigned d) { return 0x055C8u + 0x100u * d; }    // DPn_DP_LINK_CNTL
constexpr uint32_t dpVid(unsigned d) { return 0x055CCu + 0x100u * d; }     // DPn_DP_VID_STREAM_CNTL (regDP0_.. 0x210c)
static_assert(hpdStatus(2) == 0x053E4 && digFe(1) == 0x0564B && digBeEn(1) == 0x05672 && dpLink(1) == 0x056C8 &&
              dpVid(1) == 0x056CC, "DP B = PhxDpWatch.h / hpdwatch.sh addresses");
static_assert(digFe(4) == 0x0594B && dpVid(4) == 0x059CC, "DP E = PhxDpWatch.h");
constexpr uint32_t kSenseDelayed = 1u << 4;                 // DC_HPD_SENSE_DELAYED (Linux decides on it; PhxHpd.h too)
constexpr uint32_t kBeOn = 1u, kLinkTrained = 1u << 4;      // DIG_BE_EN_CNTL bit 0, DP_LINK_TRAINING_COMPLETE
constexpr uint32_t kVidEnable = 1u, kVidStatus = 1u << 16;  // DP_VID_STREAM_ENABLE, DP_VID_STREAM_STATUS
constexpr uint32_t kFeSourceMask = 7u;                      // DIG_SOURCE_SELECT = the OTG instance (0 = OTG0 = head 0)
constexpr uint32_t kMaxReg = 0x059CCu;                      // the highest register read (the glue checks the BAR length)

// The DP connectors that can carry OTG0 on this board (PhxDdc.h kNavi31Board + PhxDpWatch.h kConns): DP B is the GOP's,
// DP A / DP E are ours (PhxBringup S2d feeds DIG0 / DIG4 from OTG0). auxSrc = phxddc::connSource (0x20 + AUX line).
struct Conn { uint8_t hpd, dig, auxSrc; const char *name; };
constexpr Conn kConns[] = {{2, 1, 0x22, "DP-B"}, {3, 0, 0x23, "DP-A"}, {4, 4, 0x24, "DP-E"}};
constexpr unsigned kN = sizeof kConns / sizeof kConns[0];

inline bool readAllowed(uint32_t r) {
    if (r >= 0x1A751 && r <= 0x1A762) return false;   // MMVM_INVALIDATE_ENG*_SEM (forbidden, never read)
    for (const Conn &k : kConns)
        if (r == hpdStatus(k.hpd) || r == digFe(k.dig) || r == digBeEn(k.dig) || r == dpLink(k.dig) || r == dpVid(k.dig))
            return true;
    return false;
}

struct RoIo { void *c; uint32_t (*rd)(void *c, uint32_t dword); };   // reads only, on purpose
struct Raw { uint32_t hpd, fe, beEn, link, vid; };
struct Sample { Raw c[kN]; bool bad; unsigned reads; };
inline void sample(const RoIo &io, Sample *s) {
    *s = Sample{};
    auto get = [&](uint32_t r) -> uint32_t {
        if (!readAllowed(r)) return 0;
        const uint32_t v = io.rd(io.c, r);
        s->reads++;
        if (v == 0xffffffffu) s->bad = true;
        return v;
    };
    for (unsigned i = 0; i < kN; i++) {
        const Conn &k = kConns[i];
        s->c[i] = Raw{get(hpdStatus(k.hpd)), get(digFe(k.dig)), get(digBeEn(k.dig)), get(dpLink(k.dig)), get(dpVid(k.dig))};
    }
}
inline bool hpdHigh(const Raw &r) { return (r.hpd & kSenseDelayed) != 0; }
// a running stream fed from OTG0 on this connector (what the firmware's good replug, or S2d on DP A / E, leaves)
inline bool litFromOtg0(const Raw &r) {
    return hpdHigh(r) && (r.beEn & kBeOn) && (r.link & kLinkTrained) && (r.vid & kVidEnable) && (r.vid & kVidStatus) &&
           (r.fe & kFeSourceMask) == 0;
}

// ---- the policy --------------------------------------------------------------------------------------------------------
enum : int { kLvlOff = 0, kLvlObserve = 1, kLvlAct = 2, kLvlMax = 2 };
constexpr uint32_t kPlugMs = 1000, kUnplugMs = 6000, kLitMs = 500, kEdidRetryMs = 2000;
constexpr unsigned kEdidTries = 3;
enum : uint32_t { kOptEdid = 1u << 0 };   // compare EDID block 0 before online (default on)
constexpr uint32_t kOptsDefault = kOptEdid;
// own: who took head 0 offline
enum : int { kOwnNone = 0, kOwnHotplug = 1, kOwnManual = 2 };
enum : uint8_t { kEvNone = 0, kEvPlug, kEvUnplug, kEvGlitch, kEvOffline, kEvOnline, kEvRefused, kEvFail, kEvManualOff,
                 kEvManualOn, kEvLit, kEvDark };
static const char *const kEvName[] = {"-", "plug", "unplug", "glitch", "OFFLINE", "ONLINE", "refused", "FAIL", "manual-off",
                                      "manual-on", "lit", "dark"};
struct Event { uint64_t tMs; uint8_t kind, conn; int32_t detail; };
constexpr unsigned kRing = 16;

// What the policy asks of PhxAMDFB (phxhotplug0.cpp; stubs in the host test). Each answer is read fresh per poll.
struct Glue {
    void *c;
    int (*head0Online)(void *c);                 // 1 online, 0 offline, -1 no head 0
    int (*head1Online)(void *c);                 // 1 = head 1 started AND reports a monitor
    bool (*offerOpen)(void *c);                  // head 0's offer gate open (phxoffer.h; closed only for the first WS)
    bool (*switching)(void *c);                  // a head-0 setDisplayMode is in flight
    int (*setHead0)(void *c, bool on);           // errno: 0 done, EBUSY = the other head is not online (phxfb.cpp guard)
    int (*readEdid)(void *c, uint32_t src, uint8_t out[256], uint32_t *len);   // 0 = read ok (live, PhxBringup phxEdidRead)
    uint32_t (*servedEdid)(void *c, uint8_t out[256]);                       // bytes head 0 serves (phx-edid)
};

struct Hp0 {
    int level = kLvlOff, latched = 0;
    uint32_t opts = kOptsDefault;
    int init = 0, present = 0, cand = 0;
    uint64_t candSinceMs = 0, lastMs = 0;
    int lit = 0, litConn = -1;
    uint64_t litSinceMs = 0;
    int own = kOwnNone;
    int edgeRefused = 0;              // a refusal already counted for this settled edge
    unsigned edidTries = 0;
    uint64_t edidNextMs = 0;
    uint32_t polls = 0, bad = 0, plugs = 0, unplugs = 0, glitches = 0, offlines = 0, onlines = 0, refused = 0, holds = 0,
             fails = 0, edidReads = 0, edidBad = 0, manual = 0, darkWaits = 0;
    char why[112] = {};
    Event ring[kRing] = {};
    unsigned head = 0, count = 0;
};

#ifdef KERNEL
#define PHXHP0_FMT(buf, n, ...) snprintf(buf, n, __VA_ARGS__)
#else
#define PHXHP0_FMT(buf, n, ...) snprintf(buf, n, __VA_ARGS__)
#endif
#define PHXHP0_WHY(h, ...) PHXHP0_FMT((h).why, sizeof (h).why, __VA_ARGS__)

inline void push(Hp0 &h, uint64_t now, uint8_t kind, int conn, int32_t detail) {
    h.ring[h.head] = Event{now, kind, static_cast<uint8_t>(conn < 0 ? 0xff : conn), detail};
    h.head = (h.head + 1) % kRing;
    if (h.count < kRing) h.count++;
}
inline int fail(Hp0 &h, uint64_t now, int err) {
    h.fails++; h.latched = 1; h.level = kLvlObserve;
    push(h, now, kEvFail, h.litConn, err);
    return err;
}
// The level write (debug.phxhotplug0). 0 off, 1 observe, 2 act; 2 is refused once a failure latched this boot.
enum : int { kSetOk = 0, kSetBad = 1, kSetLatched = 2 };
inline int setLevel(Hp0 &h, int v) {
    if (v < kLvlOff || v > kLvlMax) return kSetBad;
    if (v == kLvlAct && h.latched) return kSetLatched;
    h.level = v;
    return kSetOk;
}
inline bool refuseOnce(Hp0 &h, uint64_t now, int code) {
    if (h.edgeRefused) return false;
    h.edgeRefused = 1; h.refused++;
    push(h, now, kEvRefused, h.litConn, code);
    return true;
}
enum : int { kRefOffer = 1, kRefLastHead = 2, kRefEdidDiff = 3, kRefEdidNone = 4 };

// Guards for taking head 0 offline, shared by the policy and the manual sysctl. 0 = may; else the refusal code / hold.
enum : int { kGoOk = 0, kGoHold = -1 };
inline int offlineGuard(const Glue &g) {
    if (!g.offerOpen(g.c)) return kRefOffer;          // the first WS's era: no display-set change before the daemon restart
    if (g.switching(g.c)) return kGoHold;             // a head-0 switch is in flight: the next poll
    if (g.head1Online(g.c) != 1) return kRefLastHead;   // never leave macOS with no online display [hotplug-design 2b]
    return kGoOk;
}

// The manual switch (debug.phxhead0_online = 0 / 1, root; box-tested without a cable). errno.
inline int manual(Hp0 &h, uint64_t now, bool on, const Glue &g) {
    const int cur = g.head0Online(g.c);
    if (cur < 0) return 6;   // ENXIO
    if (on) {
        if (cur == 1) { h.own = kOwnNone; return 0; }
        const int e = g.setHead0(g.c, true);
        if (e) return e;
        h.own = kOwnNone; h.manual++;
        push(h, now, kEvManualOn, -1, 0);
        PHXHP0_WHY(h, "manual: head 0 online (debug.phxhead0_online=1)");
        return 0;
    }
    if (cur == 0) return 0;
    const int gd = offlineGuard(g);
    if (gd == kGoHold) return 16;                       // EBUSY
    if (gd == kRefOffer) return 1;                      // EPERM: the offer gate is still closed
    if (gd == kRefLastHead) return 16;                  // EBUSY: head 1 is not online
    const int e = g.setHead0(g.c, false);
    if (e) return e;
    h.own = kOwnManual; h.manual++;
    push(h, now, kEvManualOff, -1, 0);
    PHXHP0_WHY(h, "manual: head 0 offline (debug.phxhead0_online=0)");
    return 0;
}

inline bool edidSame(const uint8_t *a, uint32_t la, const uint8_t *b, uint32_t lb) {
    if (la < 128 || lb < 128) return false;
    for (unsigned i = 0; i < 128; i++) if (a[i] != b[i]) return false;
    return true;
}

// One poll at `now` (ms, monotonic). Pure apart from the glue calls. Returns the event kind of an action taken (or kEvNone).
inline int poll(Hp0 &h, const Sample &s, uint64_t now, const Glue &g) {
    if (h.level == kLvlOff) return kEvNone;
    h.polls++; h.lastMs = now;
    if (s.bad) { h.bad++; return kEvNone; }
    int presentRaw = 0, litRaw = 0, litConn = -1;
    for (unsigned i = 0; i < kN; i++) {
        if (hpdHigh(s.c[i])) presentRaw = 1;
        if (!litRaw && litFromOtg0(s.c[i])) { litRaw = 1; litConn = static_cast<int>(i); }
    }
    if (!h.init) {   // the state as found: no edge, no action; a line found low must still stay low 6 s before an unplug
        h.init = 1; h.present = 1; h.cand = presentRaw; h.candSinceMs = now;
        h.lit = litRaw; h.litConn = litConn; h.litSinceMs = now;
        return kEvNone;
    }
    // stream tracking (the lit connector and since when)
    if (litRaw != h.lit || (litRaw && litConn != h.litConn)) {
        h.lit = litRaw; h.litConn = litConn; h.litSinceMs = now;
        push(h, now, litRaw ? kEvLit : kEvDark, litConn, 0);
    }
    // debounce of "a monitor on a head-0 DP connector"
    if (presentRaw != h.cand) {
        if (presentRaw == h.present) { h.glitches++; push(h, now, kEvGlitch, -1, static_cast<int32_t>(now - h.candSinceMs)); }
        h.cand = presentRaw; h.candSinceMs = now;
    }
    if (h.cand != h.present && now - h.candSinceMs >= (h.cand ? kPlugMs : kUnplugMs)) {
        h.present = h.cand;
        h.edgeRefused = 0; h.edidTries = 0; h.edidNextMs = 0;
        if (h.present) h.plugs++; else h.unplugs++;
        push(h, now, h.present ? kEvPlug : kEvUnplug, -1, 0);
    }
    if (h.level < kLvlAct) return kEvNone;

    const int on = g.head0Online(g.c);
    if (on < 0) return kEvNone;
    if (on == 1 && h.own != kOwnNone) h.own = kOwnNone;   // online again by someone else: no longer ours
    if (!h.present && h.cand == 0 && on == 1) {           // settled unplug, head 0 still online
        const int gd = offlineGuard(g);
        if (gd == kGoHold) { h.holds++; return kEvNone; }
        if (gd != kGoOk) {
            if (refuseOnce(h, now, gd))
                PHXHP0_WHY(h, "unplug: head 0 stays online (%s)", gd == kRefOffer ? "offer gate closed: first WindowServer"
                                                                                   : "head 1 not online: the last display");
            return kEvNone;   // a later poll tries again (head 1 may come back)
        }
        const int e = g.setHead0(g.c, false);
        if (e) { PHXHP0_WHY(h, "unplug: head 0 offline FAILED (errno %d): latched to observe", e); fail(h, now, e); return kEvFail; }
        h.own = kOwnHotplug; h.offlines++;
        push(h, now, kEvOffline, -1, 0);
        PHXHP0_WHY(h, "unplug: head 0 offline (macOS drops the AOC; OTG0 keeps scanning)");
        return kEvOffline;
    }
    if (h.present && on == 0 && h.own == kOwnHotplug) {   // settled plug, head 0 offline by us
        if (!h.lit) { h.darkWaits++; PHXHP0_WHY(h, "plug: waiting for a running stream from OTG0 (firmware retrain / dark replug)"); return kEvNone; }
        if (now - h.litSinceMs < kLitMs) return kEvNone;
        if (h.opts & kOptEdid) {
            if (h.edidTries >= kEdidTries) return kEvNone;   // refused for this edge (counted once)
            if (now < h.edidNextMs) return kEvNone;
            uint8_t served[256], live[256];
            uint32_t ls = g.servedEdid(g.c, served), ll = 0;
            const int rc = g.readEdid(g.c, kConns[h.litConn].auxSrc, live, &ll);
            h.edidReads++; h.edidTries++; h.edidNextMs = now + kEdidRetryMs;
            if (rc != 0) {
                h.edidBad++;
                if (h.edidTries >= kEdidTries && refuseOnce(h, now, kRefEdidNone))
                    PHXHP0_WHY(h, "plug: no EDID on %s after %u reads (rc %d): head 0 stays offline until the next plug", kConns[h.litConn].name, kEdidTries, rc);
                return kEvNone;
            }
            if (!edidSame(served, ls, live, ll)) {
                h.edidTries = kEdidTries;
                if (refuseOnce(h, now, kRefEdidDiff))
                    PHXHP0_WHY(h, "plug: a different monitor on %s (EDID block 0 differs): head 0 stays offline", kConns[h.litConn].name);
                return kEvNone;
            }
        }
        const int e = g.setHead0(g.c, true);
        if (e) { PHXHP0_WHY(h, "plug: head 0 online FAILED (errno %d): latched to observe", e); fail(h, now, e); return kEvFail; }
        h.own = kOwnNone; h.onlines++;
        push(h, now, kEvOnline, h.litConn, 0);
        PHXHP0_WHY(h, "plug: head 0 online on %s (stream up %llu ms)", kConns[h.litConn].name,
                   static_cast<unsigned long long>(now - h.litSinceMs));
        return kEvOnline;
    }
    return kEvNone;
}

inline int format(const Hp0 &h, const Sample *s, uint64_t now, int head0On, char *b, size_t n) {
    size_t len = 0;
    auto out = [&](int k) { if (k > 0) len += static_cast<size_t>(k); if (len > n) len = n; };
    out(PHXHP0_FMT(b, n, "hp0 v1 level=%d latched=%d opts=0x%x now_ms=%llu head0=%d own=%d present=%d cand=%d lit=%d lit_conn=%s "
                         "polls=%u bad=%u plugs=%u unplugs=%u glitches=%u offline=%u online=%u refused=%u holds=%u fails=%u "
                         "edid_reads=%u edid_bad=%u dark_waits=%u manual=%u\nwhy %s\n",
                   h.level, h.latched, h.opts, static_cast<unsigned long long>(now), head0On, h.own, h.present, h.cand, h.lit,
                   h.litConn >= 0 && h.litConn < static_cast<int>(kN) ? kConns[h.litConn].name : "-", h.polls, h.bad, h.plugs,
                   h.unplugs, h.glitches, h.offlines, h.onlines, h.refused, h.holds, h.fails, h.edidReads, h.edidBad, h.darkWaits,
                   h.manual, h.why[0] ? h.why : "-"));
    if (s)
        for (unsigned i = 0; i < kN && len < n; i++)
            out(PHXHP0_FMT(b + len, n - len, "%s hpd=0x%08x fe=0x%08x be_en=0x%08x link=0x%08x vid=0x%08x high=%d lit=%d\n",
                           kConns[i].name, s->c[i].hpd, s->c[i].fe, s->c[i].beEn, s->c[i].link, s->c[i].vid,
                           hpdHigh(s->c[i]) ? 1 : 0, litFromOtg0(s->c[i]) ? 1 : 0));
    for (unsigned k = 0; k < h.count && len < n; k++) {
        const Event &e = h.ring[(h.head + kRing - h.count + k) % kRing];
        out(PHXHP0_FMT(b + len, n - len, "ev%u t=%llu %s conn=%s detail=%d\n", k, static_cast<unsigned long long>(e.tMs),
                       e.kind < sizeof kEvName / sizeof kEvName[0] ? kEvName[e.kind] : "?",
                       e.conn < kN ? kConns[e.conn].name : "-", static_cast<int>(e.detail)));
    }
    return static_cast<int>(len < n ? len : n - 1);
}

// ---- the cross-head guard (phxfb.cpp phxFbSetHeadOnline, every caller: hot-plug, debug.phxhead2, debug.phxhead0_online)
// A head may go offline only while another head stays online: macOS never sees zero online framebuffers from us (that
// IOGraphics path, fOnlineMask 0, is untested here). head1Started = head 1 exists.
inline bool mayGoOffline(unsigned head, bool head0On, bool head1Started, bool head1On) {
    if (head == 0) return head1Started && head1On;
    return head0On;
}

}  // namespace phxhp0
