// SPDX-License-Identifier: MIT
// PhxHpd.h (kext b102: hot-plug steps K1 + A2)
//
// Hot-plug detect (HPD) state of the five DCN 3.2 HPD blocks, read only, the software debounce, and the A2 policy (head 1
// offline / online on a settled unplug / plug of its connector; section "hot-plug policy" below).
// Kernel-free (stdint + snprintf) so a host test runs it; the kext glue is phx_hpd.cpp.
//
// Register offsets: absolute BAR5 dwords = DCN BASE_IDX 2 segment 0x34C0 (the card's IP discovery table, as PhxDdc.h) + the
// offsets of Linux's dcn_3_2_0_offset.h (regHPD0_DC_HPD_INT_STATUS 0x1f14 ... regHPD4_DC_HPD_TOGGLE_FILT_CNTL 0x1f38,
// stride 8; regDC_GPIO_HPD_MASK 0x28f4 .. _Y 0x28f7; regHPD_INTERRUPT_DEST 0x0161). Fields: dcn_3_2_0_sh_mask.h.
//
// The reader interface has no write callback on purpose: nothing in this header can write a register. Linux reads
// DC_HPD_SENSE_DELAYED to decide the connection state (dc/gpio/hw_hpd.c get_value, GPIO_MODE_INTERRUPT) and toggles
// DC_HPD_INT_POLARITY after each interrupt (dc/irq/dcn32/irq_service_dcn32.c hpd_ack); we never ack or re-arm anything.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef KERNEL
#include <stdio.h>
#endif
#include "PhxDdc.h"
#include "PhxEdid.h"

namespace phxhpd {

constexpr unsigned kNumHpd = 5;   // HPD0..HPD4 on DCN 3.2 (dcn_3_2_0_offset.h has no HPD5 block)
constexpr uint32_t kHpd0IntStatus = 0x34C0 + 0x1f14;   // 0x53D4
constexpr uint32_t kHpdStride = 8;
enum : uint32_t { kOffIntStatus = 0, kOffIntControl = 1, kOffControl = 2, kOffFastTrain = 3, kOffToggleFilt = 4 };
constexpr uint32_t hpdReg(unsigned n, uint32_t off) { return kHpd0IntStatus + kHpdStride * n + off; }
constexpr uint32_t kGpioHpdMask = 0x34C0 + 0x28f4, kGpioHpdA = 0x34C0 + 0x28f5, kGpioHpdEn = 0x34C0 + 0x28f6,
                   kGpioHpdY = 0x34C0 + 0x28f7;   // 0x5DB4..0x5DB7
constexpr uint32_t kHpdIntDest = 0x34C0 + 0x0161;  // 0x3621, DOUT_IHC_HPDn_INTERRUPT_DEST bits 0-5, _RX_ bits 8-13

// HPDn_DC_HPD_INT_STATUS
constexpr uint32_t kStIntStatus = 1u << 0, kStSense = 1u << 1, kStSenseDelayed = 1u << 4, kStRxIntStatus = 1u << 8;
constexpr uint32_t stConTimer(uint32_t v) { return (v >> 12) & 0xff; }
constexpr uint32_t stDisconTimer(uint32_t v) { return (v >> 24) & 0xff; }
// HPDn_DC_HPD_INT_CONTROL
constexpr uint32_t kIcAck = 1u << 0, kIcPolarity = 1u << 8, kIcEn = 1u << 16, kIcRxAck = 1u << 20, kIcRxEn = 1u << 24;
// HPDn_DC_HPD_CONTROL
constexpr uint32_t ctlConnTimer(uint32_t v) { return v & 0x1fff; }
constexpr uint32_t ctlRxIntTimer(uint32_t v) { return (v >> 16) & 0x3ff; }
constexpr uint32_t kCtlEn = 1u << 28;
// HPDn_DC_HPD_TOGGLE_FILT_CNTL (Linux program_hpd_filter writes ms / 10: HDMI 500/100 ms, DP 80/0 ms)
constexpr uint32_t filtConnect(uint32_t v) { return v & 0xff; }
constexpr uint32_t filtDisconnect(uint32_t v) { return (v >> 20) & 0xff; }
// DC_GPIO_HPD_Y: the raw pins, GPIO names 1-based. HPD block n <-> GPIO pin n+1 ([I], RX4Darwin's verified rule on its
// card: DIG_HPD_SELECT n = GPIO HPD n+1); pins 1-4 at bits 0/8/16/24, pin 5 at bit 26 (sh_mask DC_GPIO_HPD5_Y).
constexpr uint32_t gpioYBit(unsigned n) { return n < 4 ? 8u * n : 26u; }

// Read-only access: a context and a read callback, nothing else.
struct RoIo {
    void *ctx;
    uint32_t (*rd)(void *ctx, uint32_t dword);
};

struct Pin { uint32_t status, intControl, control, fastTrain, toggleFilt; };
struct Sample {
    Pin pin[kNumHpd];
    uint32_t gpioMask, gpioA, gpioEn, gpioY, intDest;
    unsigned reads;   // registers read for this sample
    bool bad;         // a read gave 0xffffffff (device gone / BAR not mapped): nothing below is meaningful
};

// The registers sample() reads, in order. The host test checks that this is all it reads.
inline bool isSampleReg(uint32_t r) {
    if (r >= hpdReg(0, 0) && r <= hpdReg(kNumHpd - 1, kOffToggleFilt) && ((r - kHpd0IntStatus) % kHpdStride) <= kOffToggleFilt)
        return true;
    return r == kGpioHpdMask || r == kGpioHpdA || r == kGpioHpdEn || r == kGpioHpdY || r == kHpdIntDest;
}

inline void sample(const RoIo &io, Sample *s) {
    *s = Sample{};
    auto get = [&](uint32_t r) {
        const uint32_t v = io.rd(io.ctx, r);
        s->reads++;
        if (v == 0xffffffffu) s->bad = true;
        return v;
    };
    for (unsigned n = 0; n < kNumHpd; n++) {
        Pin &p = s->pin[n];
        p.status = get(hpdReg(n, kOffIntStatus));
        p.intControl = get(hpdReg(n, kOffIntControl));
        p.control = get(hpdReg(n, kOffControl));
        p.fastTrain = get(hpdReg(n, kOffFastTrain));
        p.toggleFilt = get(hpdReg(n, kOffToggleFilt));
    }
    s->gpioMask = get(kGpioHpdMask);
    s->gpioA = get(kGpioHpdA);
    s->gpioEn = get(kGpioHpdEn);
    s->gpioY = get(kGpioHpdY);
    s->intDest = get(kHpdIntDest);
}

// bit n = HPDn: raw SENSE, SENSE_DELAYED (what Linux decides on), and the GPIO pin.
inline uint32_t senseMask(const Sample &s) { uint32_t m = 0; for (unsigned n = 0; n < kNumHpd; n++) if (s.pin[n].status & kStSense) m |= 1u << n; return m; }
inline uint32_t delayedMask(const Sample &s) { uint32_t m = 0; for (unsigned n = 0; n < kNumHpd; n++) if (s.pin[n].status & kStSenseDelayed) m |= 1u << n; return m; }
inline uint32_t gpioMaskOf(const Sample &s) { uint32_t m = 0; for (unsigned n = 0; n < kNumHpd; n++) if ((s.gpioY >> gpioYBit(n)) & 1u) m |= 1u << n; return m; }
// The int sysctl value: bits 4:0 SENSE_DELAYED, 12:8 raw SENSE, 20:16 GPIO pins, bit 31 = bad sample.
inline uint32_t packed(const Sample &s) {
    if (s.bad) return 1u << 31;
    return delayedMask(s) | senseMask(s) << 8 | gpioMaskOf(s) << 16;
}

// ---- debounce (step 2+; host-tested now) -----------------------------------------------------------------------------
// Thresholds: a connect = high for connectMs; a disconnect = low for disconnectMs. Defaults after RX4Darwin's design,
// verified on its card (docs/second-pipe.md "Hot-plug": a second of high is a plug, three seconds of low an unplug on
// HDMI, six on DisplayPort because its test monitor drops HPD up to 3.7 s when it wakes). Linux's hardware filter is far
// shorter (HDMI 500/100 ms, DP 80/0 ms, link_hpd.c program_hpd_filter) because Linux re-detects on every edge; we act
// only on a settled state, and each action is a pipe light / undo of ~0.1-3 s.
struct Thresh { uint32_t connectMs, disconnectMs; };
constexpr Thresh kHdmi{1000, 3000}, kDp{1000, 6000};
inline Thresh threshFor(uint8_t connType) { return connType == phxddc::kConnDp ? kDp : kHdmi; }

enum : uint8_t { kUnknown = 0, kLow = 1, kHigh = 2 };
enum Event : int { kNoEvent = 0, kPlugged = 1, kUnplugged = 2 };
struct Deb {
    uint8_t state;        // settled: kUnknown before the first step
    uint8_t cand;         // the level waiting to settle (== state when nothing is pending)
    uint64_t candSinceMs;
    uint32_t plugs, unplugs, glitches;   // glitches: a pending change that went back before it settled
    uint64_t lastEventMs;
};
// One poll. holdUnplug (display asleep, mode switch or relight running): a low level never settles into an unplug and
// its timer restarts, so an unplug needs the full disconnect time after the hold ends (a monitor that drops HPD while
// it sleeps or retrains is not taken for an unplug). A plug is never held.
inline Event step(Deb &d, bool high, uint64_t nowMs, const Thresh &t, bool holdUnplug = false) {
    const uint8_t lvl = high ? kHigh : kLow;
    if (d.state == kUnknown) { d.state = d.cand = lvl; d.candSinceMs = nowMs; return kNoEvent; }   // boot state: no event
    if (lvl == d.state) {
        if (d.cand != d.state) d.glitches++;
        d.cand = d.state;
        return kNoEvent;
    }
    if (d.cand != lvl) { d.cand = lvl; d.candSinceMs = nowMs; return kNoEvent; }
    if (!high && holdUnplug) { d.candSinceMs = nowMs; return kNoEvent; }
    const uint64_t need = high ? t.connectMs : t.disconnectMs;
    if (nowMs - d.candSinceMs < need) return kNoEvent;
    d.state = lvl;
    d.lastEventMs = nowMs;
    if (high) { d.plugs++; return kPlugged; }
    d.unplugs++;
    return kUnplugged;
}

// ---- report ----------------------------------------------------------------------------------------------------------
#ifndef PHXHPD_OUT
#define PHXHPD_OUT(...) do { if (o < n) { const int k_ = snprintf(b + o, static_cast<size_t>(n - o), __VA_ARGS__); if (k_ > 0) o += k_; } } while (0)
#endif
inline const char *connName(const phxddc::Connector *c) { return !c ? "none" : c->type == phxddc::kConnDp ? "DP" : "HDMI"; }
// debug.phxhpd: one summary line, then one line per HPD block with its decoded fields and the board connector on it.
inline int format(const Sample &s, const phxddc::Connector *t, unsigned nt, char *b, int n) {
    int o = 0;
    if (!b || n <= 0) return 0;
    b[0] = 0;
    PHXHPD_OUT("hpd v1 bad=%d delayed=0x%02x sense=0x%02x gpio=0x%02x gpio_y=0x%08x gpio_en=0x%08x gpio_mask=0x%08x gpio_a=0x%08x int_dest=0x%08x reads=%u\n",
               s.bad ? 1 : 0, delayedMask(s), senseMask(s), gpioMaskOf(s), s.gpioY, s.gpioEn, s.gpioMask, s.gpioA, s.intDest, s.reads);
    for (unsigned i = 0; i < kNumHpd; i++) {
        const Pin &p = s.pin[i];
        const phxddc::Connector *c = nullptr;
        for (unsigned j = 0; t && j < nt; j++) if (t[j].hpd == i) { c = &t[j]; break; }
        char via[16] = "-";
        if (c) snprintf(via, sizeof via, c->type == phxddc::kConnDp ? "AUX%u" : "DDC%u", c->type == phxddc::kConnDp ? c->line : c->line + 1u);
        PHXHPD_OUT("hpd%u sense=%u delayed=%u int=%u rx=%u con_t=%u dis_t=%u status=0x%08x | int_en=%u pol=%s rx_en=%u int_ctl=0x%08x | "
                   "hpd_en=%u conn_timer=%u rx_timer=%u ctl=0x%08x | filt_con=%u filt_dis=%u filt=0x%08x | gpio=%u | conn=%s path=%d %s\n",
                   i, (p.status & kStSense) ? 1 : 0, (p.status & kStSenseDelayed) ? 1 : 0, (p.status & kStIntStatus) ? 1 : 0,
                   (p.status & kStRxIntStatus) ? 1 : 0, stConTimer(p.status), stDisconTimer(p.status), p.status,
                   (p.intControl & kIcEn) ? 1 : 0, (p.intControl & kIcPolarity) ? "connect" : "disconnect", (p.intControl & kIcRxEn) ? 1 : 0, p.intControl,
                   (p.control & kCtlEn) ? 1 : 0, ctlConnTimer(p.control), ctlRxIntTimer(p.control), p.control,
                   filtConnect(p.toggleFilt), filtDisconnect(p.toggleFilt), p.toggleFilt,
                   (s.gpioY >> gpioYBit(i)) & 1u, connName(c), c ? static_cast<int>(c->path) : -1, via);
    }
    return o < n ? o : n - 1;
}

// ---- hot-plug policy, step A2 (head 1 / pipe 1 only) -----------------------------------------------------------------
// Kill switch debug.phxhotplug: 0 off (no poll, no read; default), 1 observe (poll every 250 ms, debounce every connector,
// count, keep an event ring; no action), 2 act on head 1:
//   - a settled unplug of head 1's connector (the board's HDMI connector = the Acer, HPD0) while head 1 is online takes
//     head 1 offline the debug.phxhead2=0 way (PhxAccel phxFbSetHeadOnline: kConnectionCheckEnable answers 0 and the
//     connect interrupt tells IOFramebuffer; box A1: macOS drops the Acer, WindowServer survives);
//   - a settled plug brings head 1 back the debug.phxhead2=1 way only when (a) this policy took it offline, (b) the
//     monitor's EDID block 0 equals the one head 1 serves (the same monitor; a different one = step A5, refused here),
//     (c) pipe 1 is still lit (box R2: with the Acer pulled OTG1 keeps scanning and the picture returns by itself).
// Pipe 1 is never written by this policy: no undo on unplug, no relight on plug. A dark pipe 1 at the plug keeps head 1
// offline with a reason (a relight, p2Relight, is not written yet). Head 0 is never acted on
// (level 3, after A6). Every action runs under the display lock (gP2Busy, try only: busy = the next poll tries again)
// and never while the lock is held, a low level on head 1's connector does not settle (the debounce hold, so a mode
// switch's own blank is never an unplug). A failed action drops level 2 to 1 for the rest of the boot (latched).
enum : int { kLvlOff = 0, kLvlObserve = 1, kLvlHead1 = 2, kLvlMax = 2 };
// head 1 as PhxAccel reports it (phx_head1_hotplug.state); kH1Unknown = no answer (PhxAccel without phxHead1Hotplug)
enum : int { kH1Unknown = -1, kH1None = 0, kH1Online = 1, kH1Offline = 2 };
// what a poll did (the event ring's act, poll()'s return)
enum : int { kAcNone = 0, kAcOffline = 1, kAcOnline = 2, kAcRefused = 3, kAcFailed = 4, kAcBusy = 5, kAcCancel = 6, kAcSkip = 7,
             kAcWant = 8 };
inline const char *acName(int a) {
    static const char *const k[] = {"none", "offline", "online", "refused", "FAILED", "busy", "cancel", "skip", "want"};
    return a >= 0 && a <= kAcWant ? k[a] : "?";
}
constexpr unsigned kEdidTries = 3;              // the read at the settled plug (1 s high), then +2 s and +4 s (1, 3, 7 s: RX4Darwin)
constexpr uint32_t kEdidRetryMs[kEdidTries] = {0, 2000, 4000};
constexpr unsigned kRing = 16;

// The kext side: every hardware or PhxAccel access the policy makes goes through these (host test = a model).
struct Glue {
    void *c;
    int (*head1)(void *c, uint8_t edid[256], uint32_t *edidLen);   // kH1*; edid (may be null) = what head 1 serves
    int (*setHead1)(void *c, bool on);                              // the debug.phxhead2 path; kH1* after the call
    bool (*tryLock)(void *c);                                       // the display lock, try only
    void (*unlock)(void *c);
    bool (*busy)(void *c);                                          // another display operation holds the lock / switches
    bool (*pipe1Lit)(void *c, const char **why);                    // under the lock
    int (*readEdid)(void *c, uint32_t src, uint8_t out[256], uint32_t *len);   // 0 = read done
};

struct Ev { uint64_t ms; uint8_t hpd, ev, act; int8_t h1; };
struct Hot {
    int level, latched;                    // latched: a failed action dropped 2 to 1 for this boot
    int h1Hpd; uint32_t h1Src;             // head 1's connector (-1 = none on this board) and its EDID source
    uint8_t h1Type;
    Deb deb[kNumHpd];
    int pend;                              // kAcOffline / kAcOnline decided, not run yet
    uint64_t pendMs, edidNextMs;
    unsigned edidTries;
    int edidOk;                            // the plug's EDID matched (kept while the lock is busy)
    int own;                               // head 1 is offline because this policy took it offline
    uint32_t polls, bad, holds, busy, offlines, onlines, refused, fails, cancels, edidReads, edidBad;
    uint64_t lastMs;
    char why[160];
    Ev ring[kRing]; unsigned nRing;
};

// head 1's connector: the board's HDMI connector (every pipe-1 plan drives DIG3 / UNIPHY D in HDMI mode and requires
// HPD0, phx_s2_plan_*.inc kPhxS2Req). Returns its index in the table, -1 = none.
inline int head1Connector(const phxddc::Connector *t, unsigned n) {
    for (unsigned i = 0; t && i < n; i++) if (t[i].type == phxddc::kConnHdmi && t[i].hpd < kNumHpd) return static_cast<int>(i);
    return -1;
}
inline const phxddc::Connector *connOfHpd(const phxddc::Connector *t, unsigned n, unsigned hpd) {
    for (unsigned i = 0; t && i < n; i++) if (t[i].hpd == hpd) return &t[i];
    return nullptr;
}
inline void init(Hot &h, const phxddc::Connector *t, unsigned n) {
    h = Hot{};
    const int i = head1Connector(t, n);
    h.h1Hpd = i < 0 ? -1 : t[i].hpd;
    h.h1Src = i < 0 ? 0 : phxddc::connSource(t[i]);
    h.h1Type = i < 0 ? phxddc::kConnHdmi : t[i].type;
}
inline void note(Hot &h, uint64_t ms, int hpd, int ev, int act, int h1) {
    Ev &e = h.ring[h.nRing % kRing];
    e.ms = ms; e.hpd = static_cast<uint8_t>(hpd); e.ev = static_cast<uint8_t>(ev); e.act = static_cast<uint8_t>(act);
    e.h1 = static_cast<int8_t>(h1);
    h.nRing++;
}
#ifndef PHXHPD_WHY
#define PHXHPD_WHY(h, ...) snprintf((h).why, sizeof (h).why, __VA_ARGS__)
#endif

// debug.phxhotplug writes. Leaving 0 restarts the debounce (its first poll is the boot state: no event), so turning the
// policy on never acts on what happened while it was off.
enum : int { kSetOk = 0, kSetBad = 1, kSetLatched = 2, kSetNoHead1 = 3 };
inline int setLevel(Hot &h, int v) {
    if (v < kLvlOff || v > kLvlMax) return kSetBad;
    if (v == kLvlHead1 && h.latched) return kSetLatched;
    if (v == kLvlHead1 && h.h1Hpd < 0) return kSetNoHead1;
    if (h.level == kLvlOff && v != kLvlOff) { for (Deb &d : h.deb) d = Deb{}; }
    if (v < kLvlHead1) { h.pend = kAcNone; h.edidOk = 0; }
    h.level = v;
    return kSetOk;
}
inline int fail(Hot &h, uint64_t now, int h1) {
    h.fails++; h.pend = kAcNone; h.edidOk = 0;
    h.latched = 1;
    if (h.level > kLvlObserve) h.level = kLvlObserve;
    note(h, now, h.h1Hpd, kNoEvent, kAcFailed, h1);
    return kAcFailed;
}
inline int refuse(Hot &h, uint64_t now, int h1) {
    h.refused++; h.pend = kAcNone; h.edidOk = 0;
    note(h, now, h.h1Hpd, kNoEvent, kAcRefused, h1);
    return kAcRefused;
}
inline bool sameBlock0(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl) {
    if (phxedid::check(a, al) != phxedid::kLive || phxedid::check(b, bl) != phxedid::kLive) return false;
    for (unsigned i = 0; i < 128; i++) if (a[i] != b[i]) return false;
    return true;
}

// A settled edge on head 1's connector at level 2: what to do (run by runPend, this poll or a later one).
inline int decide(Hot &h, Event ev, uint64_t now, int h1) {
    if (h1 == kH1Online) h.own = 0;   // online again by someone else (debug.phxhead2=1): no longer ours
    if (ev == kUnplugged) {
        if (h.pend == kAcOnline) { h.pend = kAcNone; h.edidOk = 0; h.cancels++; }
        if (h1 == kH1Online) { h.pend = kAcOffline; h.pendMs = now; return kAcWant; }
        PHXHPD_WHY(h, "unplug: head 1 not online (state %d): nothing to do", h1);
        return kAcSkip;
    }
    if (h.pend == kAcOffline) {   // the unplug never ran (lock busy) and the monitor is back: nothing changed
        h.pend = kAcNone; h.cancels++;
        PHXHPD_WHY(h, "plug: the pending unplug never ran (lock busy): cancelled, head 1 untouched");
        return kAcCancel;
    }
    if (h.own && h1 == kH1Offline) { h.pend = kAcOnline; h.pendMs = now; h.edidTries = 0; h.edidOk = 0; h.edidNextMs = now; return kAcWant; }
    if (h1 == kH1Offline) PHXHPD_WHY(h, "plug: head 1 offline but not by hot-plug (debug.phxhead2=0 or a dark mode switch): left alone");
    else PHXHPD_WHY(h, "plug: head 1 state %d: nothing to do", h1);
    return kAcSkip;
}

inline int runPend(Hot &h, uint64_t now, const Glue &g) {
    if (h.pend == kAcOffline) {
        if (h.deb[h.h1Hpd].state != kLow) { h.pend = kAcNone; h.cancels++; return kAcCancel; }
        if (h.deb[h.h1Hpd].cand != kLow) return kAcNone;   // the line is high again: wait (a settled plug cancels)
        if (!g.tryLock(g.c)) { h.busy++; return kAcBusy; }
        const int h1 = g.head1(g.c, nullptr, nullptr);
        if (h1 != kH1Online) {
            g.unlock(g.c);
            h.pend = kAcNone;
            PHXHPD_WHY(h, "unplug: head 1 state %d when the lock was free: nothing to do", h1);
            return kAcSkip;
        }
        const int after = g.setHead1(g.c, false);
        g.unlock(g.c);
        if (after != kH1Offline) { PHXHPD_WHY(h, "unplug: PhxAccel left head 1 in state %d (want offline %d)", after, kH1Offline); return fail(h, now, after); }
        h.pend = kAcNone; h.own = 1; h.offlines++; h.lastMs = now;
        PHXHPD_WHY(h, "unplug: head 1 offline (macOS drops the display; pipe 1 left lit)");
        note(h, now, h.h1Hpd, kNoEvent, kAcOffline, after);
        return kAcOffline;
    }
    if (h.pend != kAcOnline) return kAcNone;
    if (h.deb[h.h1Hpd].state != kHigh) { h.pend = kAcNone; h.edidOk = 0; h.cancels++; return kAcCancel; }
    if (h.deb[h.h1Hpd].cand != kHigh) return kAcNone;     // the line is low again: wait (a settled unplug cancels)
    if (!h.edidOk) {
        if (now < h.edidNextMs) return kAcNone;
        static uint8_t known[256], live[256];   // one poll at a time (the kext runs poll() under its own lock)
        uint32_t kl = 0, ll = 0;
        const int h1 = g.head1(g.c, known, &kl);
        if (!h.own || h1 != kH1Offline) {
            h.pend = kAcNone;
            PHXHPD_WHY(h, "plug: head 1 state %d before the EDID read: left alone", h1);
            return kAcSkip;
        }
        h.edidReads++;
        const int rc = g.readEdid(g.c, h.h1Src, live, &ll);
        const int chk = rc ? phxedid::kWhyReadFailed : phxedid::check(live, ll);
        if (chk != phxedid::kLive) {
            h.edidBad++;
            if (++h.edidTries >= kEdidTries) {
                PHXHPD_WHY(h, "plug: no valid EDID on source 0x%02x after %u reads (rc %d, check %d): head 1 stays offline", h.h1Src, h.edidTries, rc, chk);
                return refuse(h, now, h1);
            }
            h.edidNextMs = now + kEdidRetryMs[h.edidTries];
            return kAcNone;
        }
        if (!sameBlock0(known, kl, live, ll)) {
            PHXHPD_WHY(h, "plug: a different monitor (EDID mfg %02x%02x product %02x%02x, head 1 serves %02x%02x %02x%02x, %u B): stays offline (step A5)",
                       live[8], live[9], live[11], live[10], known[8], known[9], known[11], known[10], kl);
            return refuse(h, now, h1);
        }
        h.edidOk = 1;
    }
    if (!g.tryLock(g.c)) { h.busy++; return kAcBusy; }
    const char *w = nullptr;
    if (!g.pipe1Lit(g.c, &w)) {
        g.unlock(g.c);
        PHXHPD_WHY(h, "plug: pipe 1 is not lit (%s): head 1 stays offline (no relight in A2)", w ? w : "?");
        return refuse(h, now, kH1Offline);
    }
    const int h1 = g.head1(g.c, nullptr, nullptr);
    if (!h.own || h1 != kH1Offline) {
        g.unlock(g.c);
        h.pend = kAcNone; h.edidOk = 0;
        PHXHPD_WHY(h, "plug: head 1 state %d under the lock: left alone", h1);
        return kAcSkip;
    }
    const int after = g.setHead1(g.c, true);
    g.unlock(g.c);
    if (after != kH1Online) { PHXHPD_WHY(h, "plug: PhxAccel left head 1 in state %d (want online %d)", after, kH1Online); return fail(h, now, after); }
    h.pend = kAcNone; h.edidOk = 0; h.own = 0; h.onlines++; h.lastMs = now;
    PHXHPD_WHY(h, "plug: the same monitor, pipe 1 lit: head 1 online");
    note(h, now, h.h1Hpd, kNoEvent, kAcOnline, after);
    return kAcOnline;
}

// One 250 ms poll: debounce every connector of the board on SENSE_DELAYED (what Linux decides on), log the settled edges,
// and at level 2 decide and run head 1's action. Returns what it did (kAc*).
inline int poll(Hot &h, const Sample &s, uint64_t now, const phxddc::Connector *t, unsigned nt, const Glue &g) {
    if (h.level == kLvlOff) return kAcNone;
    h.polls++;
    if (s.bad) { h.bad++; return kAcSkip; }
    const uint32_t dm = delayedMask(s);
    const bool hold = g.busy && g.busy(g.c);
    int done = kAcNone;
    for (unsigned n = 0; n < kNumHpd; n++) {
        const phxddc::Connector *c = connOfHpd(t, nt, n);
        if (!c) continue;
        const bool hi = (dm >> n) & 1u, isH1 = static_cast<int>(n) == h.h1Hpd;
        if (isH1 && hold && !hi && h.deb[n].state == kHigh) h.holds++;
        const Event ev = step(h.deb[n], hi, now, threshFor(c->type), isH1 && hold);
        if (ev == kNoEvent) continue;
        int act = kAcNone, h1 = kH1Unknown;
        if (isH1 && h.level >= kLvlHead1) { h1 = g.head1(g.c, nullptr, nullptr); act = decide(h, ev, now, h1); }
        note(h, now, static_cast<int>(n), ev, act, h1);
        if (act == kAcCancel) done = act;
    }
    if (h.level >= kLvlHead1 && h.pend != kAcNone) done = runPend(h, now, g);
    return done;
}

// debug.phxhotplug_stat: one summary line, one line per connector, then the event ring (oldest first).
inline int formatHot(const Hot &h, uint64_t now, const phxddc::Connector *t, unsigned nt, char *b, int n) {
    int o = 0;
    if (!b || n <= 0) return 0;
    b[0] = 0;
    static const char *const kLv[] = {"?", "low", "high"};
    PHXHPD_OUT("hotplug v1 level=%d latched=%d now_ms=%llu h1_hpd=%d h1_src=0x%02x polls=%u bad=%u holds=%u busy=%u offline=%u online=%u "
               "refused=%u fails=%u cancels=%u edid_reads=%u edid_bad=%u own=%d pend=%s why=%s\n",
               h.level, h.latched, static_cast<unsigned long long>(now), h.h1Hpd, h.h1Src, h.polls, h.bad, h.holds, h.busy, h.offlines,
               h.onlines, h.refused, h.fails, h.cancels, h.edidReads, h.edidBad, h.own, acName(h.pend), h.why[0] ? h.why : "-");
    for (unsigned i = 0; i < kNumHpd; i++) {
        const phxddc::Connector *c = connOfHpd(t, nt, i);
        if (!c) continue;
        const Deb &d = h.deb[i];
        PHXHPD_OUT("hpd%u conn=%s%s state=%s cand=%s plugs=%u unplugs=%u glitches=%u\n", i, connName(c),
                   static_cast<int>(i) == h.h1Hpd ? " head1" : "", kLv[d.state < 3 ? d.state : 0], kLv[d.cand < 3 ? d.cand : 0], d.plugs,
                   d.unplugs, d.glitches);
    }
    static const char *const kEv[] = {"-", "plugged", "unplugged"};
    const unsigned first = h.nRing > kRing ? h.nRing - kRing : 0;
    for (unsigned k = first; k < h.nRing; k++) {
        const Ev &e = h.ring[k % kRing];
        PHXHPD_OUT("ev %u t_ms=%llu hpd%u %s act=%s h1=%d\n", k, static_cast<unsigned long long>(e.ms), e.hpd, kEv[e.ev < 3 ? e.ev : 0],
                   acName(e.act), e.h1);
    }
    return o < n ? o : n - 1;
}

}  // namespace phxhpd
