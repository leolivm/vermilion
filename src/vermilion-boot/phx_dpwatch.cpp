// SPDX-License-Identifier: MIT
// phx_dpwatch.cpp: kext b120, multiport S3 step 0: debug.phxdpwatch +
// debug.phxdpwatch_stat, the read-only DP stream watcher of PhxDpWatch.h. b121:
// level 2 = the same watcher + PhxDpbFix.h on a DP B STUCK (once per plug edge, under gP2Busy try-only, writes only DP1
// stream enable / steer reset / DIG1 FIFO reset bits; a pipe-0/1 change = halted and the watcher drops to level 1).
//   debug.phxdpwatch (int): root writes 1 = start a 250 ms poll (thread call) of DP B / DP A / DP E (HPD, DIG BE / FE /
//     FIFO, DP link / stream, OUTBOX0_WPTR), 0 = stop it (default; nothing runs at boot or until root writes it). The
//     counters and the event ring are kept across a stop and cleared by the next 1. Boot-arg -phxdpwatchoff refuses every
//     write. Reads return the level.
//   debug.phxdpwatch_stat (string): the counters, the per-connector state and the last 24 events; while the poll is off it
//     shows one fresh sample (reads only) instead.
// Levels 0 / 1: the poll never writes a register, never acks an interrupt, never takes gP2Busy or the AUX lock and calls no
// other kext. Level 2 adds only the PhxDpbFix.h call above (no AUX lock either).
// b122: reads only, no new write: fifo-err events in the stat and
// debug.phxdpwatch_snap (string): the wide DP B snapshots PhxDpWatch.h takes in the poll (first sample, every DP B plug edge,
// 2 s and 10 s after it) plus one fresh snapshot read at the call (also while the poll is off). The level-2 fix is b121's.
// Part of the PhxBringup.cpp translation unit, included after phx_dpstream.cpp; not compiled on its own.

#include "PhxDpWatch.h"
#include "PhxDpbFix.h"   // b121

static phxdpw::Watch gDpw;        // under gDpwLock
static IOLock *gDpwLock;
static thread_call_t gDpwCall;
static int gDpwLevel, gDpwInit;
static phxdpf::Fix gDpf;          // b121: the last fix try (under gDpwLock)
static constexpr uint32_t kDpwPollMs = 250;

static uint32_t dpwRd(void *c, uint32_t r) { return static_cast<const PhxBringup *>(c)->p2Rd(r); }
static uint64_t dpwNowMs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ull;
}
static void dpwArm() { uint64_t d; clock_interval_to_deadline(kDpwPollMs, kMillisecondScale, &d); thread_call_enter_delayed(gDpwCall, d); }

static bool dpwSetup() {
    if (gDpwInit) return true;
    IOLock *l = IOLockAlloc();
    if (l && !OSCompareAndSwapPtr(nullptr, l, &gDpwLock)) IOLockFree(l);
    if (!gDpwLock) return false;
    IOLockLock(gDpwLock);
    if (!gDpwInit) {
        gDpwCall = thread_call_allocate([](thread_call_param_t, thread_call_param_t) {
            IOLockLock(gDpwLock);
            bool again = false;
            if (gDpwLevel && gPhx) {
                const phxdpw::RoIo io{gPhx, dpwRd};
                phxdpw::Sample s{};
                phxdpw::sample(io, &s);
                const uint64_t now = dpwNowMs();
                phxdpw::step(gDpw, s, now);
                phxdpw::snapPoll(io, gDpw, now);   // b122: the wide snapshots due now (reads only)
                // b121 level 2: the fix, DP B only, once per plug, under the display lock (try only)
                if (phxdpw::wantFix(gDpw, gDpwLevel, now)) {
                    const bool lockOk = p2Up() && OSCompareAndSwap(0, 1, &gP2Busy);
                    const int r = phxdpf::run(p2Env(), gP2, gDpf, lockOk);
                    if (lockOk) OSCompareAndSwap(1, 0, &gP2Busy);
                    const int kind = r == phxdpf::kFixed ? phxdpw::kEvFixOk : r == phxdpf::kRefused ? phxdpw::kEvFixRefused
                                   : r == phxdpf::kBusy ? phxdpw::kEvNone : phxdpw::kEvFixFail;
                    phxdpw::fixResult(gDpw, now, kind, gDpf.waitUs, s.c[0]);
                    if (r == phxdpf::kHalted) gDpwLevel = 1;   // a pipe-0 / pipe-1 change: observe only from now on
                    if (r != phxdpf::kBusy)
                        IOLog("PhxBringup: debug.phxdpwatch DP-B STUCK -> %s: %s (wait %u us, writes %u)\n", phxdpf::kResName[r], gDpf.why ? gDpf.why : "-",
                              gDpf.waitUs, gDpf.writes);
                }
                again = true;
            }
            IOLockUnlock(gDpwLock);
            if (again) dpwArm();
        }, nullptr);
        if (gDpwCall) gDpwInit = 1;
    }
    IOLockUnlock(gDpwLock);
    return gDpwInit != 0;
}

static int phxDpWatchSysctl SYSCTL_HANDLER_ARGS {
    int v = gDpwLevel;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;
    char a[8];
    if (PE_parse_boot_argn("-phxdpwatchoff", a, sizeof a)) return EPERM;
    if (v < 0 || v > 2) return EINVAL;   // b121: 2 = observe + the DP B fix
    if (!gPhx || !dpwSetup()) return ENXIO;
    IOLockLock(gDpwLock);
    const int was = gDpwLevel;
    if (v && !was) { gDpw = phxdpw::Watch{}; gDpf = phxdpf::Fix{}; }
    gDpwLevel = v;
    IOLockUnlock(gDpwLock);
    if (v && !was) dpwArm();
    else if (!v && was) thread_call_cancel_wait(gDpwCall);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdpwatch, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxDpWatchSysctl, "I",
            "b120/b121 root: 1 = start the read-only 250 ms DP stream watcher (DP B/A/E: HPD, DIG, DP link/stream; STUCK = stream on, STATUS 0 >= 500 ms), 2 = also the DP B fix on a STUCK (once per plug: DIG1 FIFO reset + stream re-enable), 0 = stop (default)");

static int phxDpWatchStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 8192;   // b122: 6144 -> 8192 (the fifo-err fields)
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len = 0;
    const uint64_t now = dpwNowMs();
    if (!gPhx) len = snprintf(b, kN, "no PhxBringup\n");
    else if (!gDpwInit || !gDpwLevel) {   // off: one fresh sample, reads only (into a scratch watcher; gDpw is kept)
        phxdpw::Watch *one = static_cast<phxdpw::Watch *>(IOMalloc(sizeof(phxdpw::Watch)));   // per call: readers may overlap
        if (!one) { IOFree(b, kN); return ENOMEM; }
        *one = phxdpw::Watch{};
        const phxdpw::RoIo io{gPhx, dpwRd};
        phxdpw::Sample s{};
        phxdpw::sample(io, &s);
        phxdpw::step(*one, s, now);
        len = snprintf(b, kN, "poll off: one fresh sample follows (root: sysctl -w debug.phxdpwatch=1 to start the poll)\n");
        if (len >= 0 && len < kN) len += phxdpw::format(*one, 0, now, b + len, kN - len);
        IOFree(one, sizeof(phxdpw::Watch));
        if (gDpwInit && gDpw.samples && len >= 0 && len < kN) {
            IOLockLock(gDpwLock);
            len += snprintf(b + len, static_cast<size_t>(kN - len), "last poll (stopped):\n");
            if (len < kN) len += phxdpw::format(gDpw, 0, now, b + len, kN - len);
            IOLockUnlock(gDpwLock);
        }
    } else {
        IOLockLock(gDpwLock);
        len = phxdpw::format(gDpw, gDpwLevel, now, b, kN);
        if (len >= 0 && len < kN && gDpf.res) len += phxdpf::stat(gDpf, b + len, kN - len);
        IOLockUnlock(gDpwLock);
    }
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdpwatch_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDpWatchStatSysctl, "A",
            "b120 the DP stream watcher: counters, per-connector state, the last 24 events (STUCK with its register snapshot); off = one fresh sample");

// b122: the snapshots + a fresh one (reads only; the fresh one also while the poll is off).
static int phxDpWatchSnapSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 12288;
    char *b = static_cast<char *>(IOMalloc(kN));
    phxdpw::Snap *fresh = static_cast<phxdpw::Snap *>(IOMalloc(sizeof(phxdpw::Snap)));
    if (!b || !fresh) { if (b) IOFree(b, kN); if (fresh) IOFree(fresh, sizeof(phxdpw::Snap)); return ENOMEM; }
    int len = 0;
    const uint64_t now = dpwNowMs();
    if (!gPhx) len = snprintf(b, kN, "no PhxBringup\n");
    else {
        const phxdpw::RoIo io{gPhx, dpwRd};
        phxdpw::snapRead(io, *fresh, phxdpw::kSnapNow, 0, now);
        if (gDpwInit) {
            IOLockLock(gDpwLock);
            fresh->plug = gDpw.snapPlug;
            len = phxdpw::snapFormat(gDpw, fresh, now, b, kN);
            IOLockUnlock(gDpwLock);
        } else {
            phxdpw::Watch *none = static_cast<phxdpw::Watch *>(IOMalloc(sizeof(phxdpw::Watch)));
            if (none) { *none = phxdpw::Watch{}; len = phxdpw::snapFormat(*none, fresh, now, b, kN); IOFree(none, sizeof(phxdpw::Watch)); }
        }
    }
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(fresh, sizeof(phxdpw::Snap));
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdpwatch_snap, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDpWatchSnapSysctl, "A",
            "b122 the DP B wide register snapshots (reads only): watcher start, each DP B plug edge, +2 s, +10 s, and one read now");

// PhxBringup::stop: no poll after the provider goes.
static void phxDpWatchStop() {
    if (!gDpwInit) return;
    IOLockLock(gDpwLock);
    gDpwLevel = 0;
    IOLockUnlock(gDpwLock);
    thread_call_cancel_wait(gDpwCall);
}
