// SPDX-License-Identifier: MIT
// PhxDpbFix.h: kext b121, multiport S3 step 1: the gated fix for the dark
// AOC after a DP B replug. On a DP B STUCK of the read-only watcher (PhxDpWatch.h: HPD2 high >= 1 s, DIG1 BE on, DP1 link
// trained, DP1 stream enabled, DP_VID_STREAM_STATUS 0 >= 500 ms, as seen on the box), at debug.phxdpwatch = 2 only, once per
// plug edge, with the GOP firmware's trace ring quiet >= 500 ms, it runs Linux v6.8's stream re-enable with the DIG resync
// FIFO reset (enc1_stream_encoder_dp_blank's disable + enc32_stream_encoder_dp_unblank's steer pulse / enc32_reset_fifo /
// enable), exactly the b106 pipe-0 shell's stages 3 and 7 without the OTG / HUBP / DPG / MSA parts (PhxPipe0.h; box 22/22
// on the same register signature; for the replug case the FIFO is a hypothesis):
//   DP1 VID_STREAM_ENABLE 0, wait STATUS 0 (<= 102 ms), DP1 STEER_FIFO_RESET 1, 10 us, 0, wait DIG_SYMCLK_FE_ON (<= 50 ms),
//   DIG1 DIG_FIFO_RESET 1, wait RESET_DONE 1, RESET 0, wait RESET_DONE 0 (<= 50 ms each), 100 us, VID_STREAM_ENABLE 1, wait
//   STATUS 1 (<= 200 ms).
// Writes: only those three registers, only those bits, as read-modify-writes (FixGate; anything else is dropped and counted).
// Before the first write: the display lock (gP2Busy, try only: held = skip this poll), no halted / dmub latch, every
// STUCK register re-read (HPD2, DIG1 BE_EN / FE source OTG0, DP1 link, stream ENABLE 1 + STATUS 0), pipe 0 absolute (OTG0 at
// a known AOC mode, frames moving) and a fresh pipe-0 / pipe-1 baseline; after every write the pipe-0 fingerprint (PhxPipe2.h
// kFp: OTG0 / DTO0 / DENTIST / DIG1 FE / BE / BE_EN / MPCC0) and the pipe-1 one (PhxDpRun.h kFp1): a change = halted (shared
// latch: no display write by any engine until reboot) and the watcher drops to observe. No DMUB, AUX, DPCD, SMU or DAL.
// Kernel-free; glue phx_dpwatch.cpp; host-tested (with mutations).
#pragma once
#include <stdint.h>
#include "PhxDpStream.h"
#include "PhxDpWatch.h"

namespace phxdpf {

using phxp2::Env;
using phxp2::State;
using phxp2::rd;

constexpr uint32_t kHpd2 = phxdpw::hpdStatus(2), kDig1Fe = phxdpw::digFe(1), kDig1Fifo = phxdpw::digFifo(1),
                   kDig1BeEn = phxdpw::digBeEn(1), kDp1Link = phxdpw::dpLink(1), kDp1Vid = phxdpw::dpVid(1),
                   kDp1Steer = phxdpw::dpVid(1) + 1;
static_assert(kDp1Steer == 0x056CD && kDig1Fifo == 0x05651 && kDp1Vid == 0x056CC, "DP1_DP_STEER_FIFO / DIG1 FIFO / DP1 stream (rt-snap)");
constexpr uint32_t kVidEn = 0x1u, kVidStatus = 0x10000u, kSteerReset = 0x1u, kFifoReset = 0x2u, kFifoResetDone = 0x100000u,
                   kFifoError = 0x30000000u, kSymclkFeOn = 0x01000000u;
// (the host test checks these against PhxPipe0.h phxp0::, the b106 shell that proved them on the box)
constexpr uint32_t kStatusOffUs = 102000, kFeOnUs = 50000, kDoneUs = 50000, kStatusOnUs = 200000;

// The only writes: (register, the bits that may change).
inline uint32_t allowedBits(uint32_t r) {
    if (r == kDp1Vid) return kVidEn;
    if (r == kDp1Steer) return kSteerReset;
    if (r == kDig1Fifo) return kFifoReset;
    return 0;
}

enum Res { kNone = 0, kFixed, kFailed, kRefused, kBusy, kHalted };
static const char *const kResName[] = {"none", "FIX-OK", "FIX-FAIL", "refused", "busy", "HALTED"};
struct Fix {
    int res; const char *why;
    phxdpr::Run b;          // the fresh pipe-0 / pipe-1 baseline + io counters (blocked / stray / writes)
    uint32_t pre[5];         // DP1 stream, steer, DIG1 FIFO, DIG1 FE, DP1 link before
    uint32_t post[3];        // DP1 stream, DIG1 FIFO, steer after
    uint32_t waitUs, writes, stray;
    int warnFeOff, warnDone;
};

inline bool W(const Env &e, State &st, Fix &f, uint32_t r, uint32_t bits, uint32_t val) {
    if (st.halted) { f.b.blocked++; return false; }
    const uint32_t ok = allowedBits(r);
    if (!ok || (bits & ~ok)) { f.stray++; return false; }
    const uint32_t v = (rd(e, r) & ~bits) | (val & bits);   // read-modify-write; DIG_FIFO_ERROR (W1C) written back as read (= b106)
    e.io.wr(e.io.c, r, v);
    f.writes++;
    return phxdpr::guard(e, st, f.b);
}
inline bool poll(const Env &e, uint32_t r, uint32_t m, uint32_t want, uint32_t budgetUs, uint32_t *waited) {
    uint32_t us = 0;
    while ((rd(e, r) & m) != want) {
        if (us >= budgetUs) { if (waited) *waited = us; return false; }
        e.io.delayUs(e.io.c, 10); us += 10;
    }
    if (waited) *waited = us;
    return true;
}

// Preconditions, reads only: the STUCK state as the watcher saw it, still true now, and pipe 0 sane.
inline const char *gates(const Env &e, const State &st, Fix &f) {
    if (st.halted) return "halted: pipe 0 / pipe 1 changed earlier in this boot";
    if (st.dmub.dead) return "debug.phxdmub_dead = 1";
    if (st.switching) return "a pipe-1 mode switch is in progress";
    f.pre[0] = rd(e, kDp1Vid); f.pre[1] = rd(e, kDp1Steer); f.pre[2] = rd(e, kDig1Fifo); f.pre[3] = rd(e, kDig1Fe); f.pre[4] = rd(e, kDp1Link);
    if (!(rd(e, kHpd2) & phxdpw::kHpdSense)) return "HPD2 low (the AOC left DP B)";
    if (!(rd(e, kDig1BeEn) & 1u)) return "DIG1 back end off";
    if ((f.pre[4] & 0x10u) != 0x10u) return "DP1 link not trained";
    if ((f.pre[0] & (kVidEn | kVidStatus)) != kVidEn) return "DP1 stream not 'enabled with STATUS 0' any more";
    if ((f.pre[3] & 7u) != 0) return "DIG1 FE not fed by OTG0";
    if (f.pre[2] & kFifoReset) return "DIG1 FIFO already in reset";
    const char *w = phxdps::rebase(e, f.b);   // pipe 0 absolute (OTG0 on, a known AOC mode, no HUBP0 underflow) + the baseline
    if (w) return w;
    if (!phxp2::frameMoves(e, phxp2::kOtg0Frame)) return "OTG0 frame counter not moving";
    return nullptr;
}

// One try. lockOk = the caller took gP2Busy (false = held by someone else: kBusy, nothing read or written).
inline int run(const Env &e, State &st, Fix &f, bool lockOk) {
    f = Fix{};
    if (!lockOk) { f.res = kBusy; f.why = "display lock (gP2Busy) held: the next poll tries again"; return kBusy; }
    const char *w = gates(e, st, f);
    if (w) { f.res = kRefused; f.why = w; return kRefused; }
    auto halt = [&](const char *why) { f.res = kHalted; f.why = why; return static_cast<int>(kHalted); };
    // stream off (enc1_stream_encoder_dp_blank / the b106 shell stage 3)
    if (!W(e, st, f, kDp1Vid, kVidEn, 0)) return halt("pipe 0 / pipe 1 changed after the stream disable");
    poll(e, kDp1Vid, kVidStatus, 0, kStatusOffUs, nullptr);
    if (!(rd(e, kDp1Steer) & kSteerReset) && !W(e, st, f, kDp1Steer, kSteerReset, kSteerReset)) return halt("pipe 0 / pipe 1 changed after the steer reset");
    // unblank (enc32_stream_encoder_dp_unblank, the b106 shell stage 7 from the steer FIFO on)
    e.io.delayUs(e.io.c, 10);
    if (!W(e, st, f, kDp1Steer, kSteerReset, 0)) return halt("pipe 0 / pipe 1 changed after the steer release");
    if (!poll(e, kDig1Fe, kSymclkFeOn, kSymclkFeOn, kFeOnUs, nullptr)) f.warnFeOff = 1;
    for (uint32_t want = kFifoReset;; want = 0) {
        if (!W(e, st, f, kDig1Fifo, kFifoReset, want)) return halt("pipe 0 / pipe 1 changed during the DIG1 FIFO reset");
        if (f.warnFeOff) e.io.delayUs(e.io.c, 10);
        else if (!poll(e, kDig1Fifo, kFifoResetDone, want ? kFifoResetDone : 0, kDoneUs, nullptr)) f.warnDone = 1;
        if (!want) break;
    }
    e.io.delayUs(e.io.c, 100);
    if (!W(e, st, f, kDp1Vid, kVidEn, kVidEn)) return halt("pipe 0 / pipe 1 changed after the stream enable");
    const bool up = poll(e, kDp1Vid, kVidStatus, kVidStatus, kStatusOnUs, &f.waitUs);
    f.post[0] = rd(e, kDp1Vid); f.post[1] = rd(e, kDig1Fifo); f.post[2] = rd(e, kDp1Steer);
    if (!phxdpr::guard(e, st, f.b)) return halt("pipe 0 / pipe 1 changed after the fix");
    f.res = up ? kFixed : kFailed;
    f.why = up ? "DP1 stream STATUS 1 after the FIFO reset + re-enable" : "DP1 stream STATUS still 0 200 ms after the re-enable (not the FIFO: H-S2B-BESEL / D3?)";
    return f.res;
}

inline int stat(const Fix &f, char *b, int n) {
    return snprintf(b, static_cast<size_t>(n),
                    "last fix: %s why=%s pre vid=0x%08x steer=0x%08x fifo=0x%08x fe=0x%08x link=0x%x post vid=0x%08x fifo=0x%08x steer=0x%08x "
                    "wait_us=%u writes=%u stray=%u blocked=%u fe_off_warn=%d done_warn=%d pipe0 same=%d pipe1 same=%d%s%s\n",
                    kResName[f.res >= 0 && f.res <= kHalted ? f.res : 0], f.why ? f.why : "-", f.pre[0], f.pre[1], f.pre[2], f.pre[3], f.pre[4],
                    f.post[0], f.post[1], f.post[2], f.waitUs, f.writes, f.stray, f.b.blocked, f.warnFeOff, f.warnDone, f.b.pipe0Same,
                    f.b.pipe1Same, f.b.pipeWhy ? " changed=" : "", f.b.pipeWhy ? f.b.pipeWhy : "");
}

}  // namespace phxdpf
