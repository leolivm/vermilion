// SPDX-License-Identifier: MIT
// phx_pipe0.cpp: b99 pipe-0 (AOC, DP) mode engine glue (engine PhxPipe0.h,
// plans phx_p0_plan_1080p60.inc / phx_p0_plan_1080p144.inc generated from Linux register captures;
// host-tested). Nothing here runs at boot and nothing touches a register unless root sets
// debug.phxpipe0_enable=1 first:
//   debug.phxpipe0_enable  kill switch, default 0: every command refused before any register access
//   debug.phxpipe0_mode    the target, 60 or 144 (no hardware access)
//   debug.phxpipe0         root: 1 survey (reads only), 2 set the target mode, 3 round trip at the current mode (the full
//                          shell with the values already there; a real mode change needs one passed in this boot);
//                          reads the last result (0 done, 1 same, 2 refused, 3 fell back, 4 dark)
//   debug.phxpipe0_stat    busy= gen= ... why= (no register access: the b15 watchdog reads it during a switch)
//   debug.phxpipe0_plan    root, reads only: both plans with every register now
//   debug.phxpipe0_dead    the dark latch (RO)
// b101: the macOS path. PhxAMDFB head 0's setDisplayMode calls the platform
// function "phxPipe0SetMode" (PhxSwitchABI.h; property phx-pipe0-mode-fn), which runs phxp0::macSet: stutter forced off,
// the UCLK hard minimum (debug.phxpipe0_uclk MHz, default 456) before a set above 60, the round trip at the current mode
// when none passed in this boot, then the set; debug.phxpipe0=4 runs the same sequence for debug.phxpipe0_mode from a
// shell (no PhxAccel). debug.phxpipe0_modes=1 (root) publishes phx-pipe0-modes = true on this service: PhxAMDFB head 0
// then lists the AOC's 60 and 144 Hz modes when it starts (the boot daemon sets it before the claim). The kill switch
// debug.phxpipe0_enable still gates every register access of every path.
// b103: a third plan, phx_p0_plan_1080p144a60.inc = the AOC at 144 next to the
// Acer at 60 (from Linux); every path picks the plan for its rate by OTG1's totals (phxp0::pick), and the pipe-1 switch
// (p2SwitchTo, phx_pipe2.cpp) runs the switch through p0PairSwitch so that pipe 0 at 144 follows the Acer's mode in the safe order.
// b105: p2SwitchTo runs the whole Acer switch through p0PairSwitch
// (phxp0::pairSwitch): a post-step that does not land switches pipe 1 back, and pipe2_switch_stat says why ("pipe 0: ...").
// Every command runs under gP2Busy (the display lock the pipe-1 ladder, its mode switch and head-1 gamma take), so the
// two engines never interleave; a done or fell-back mode set then re-baselines the pipe-1 engine's pipe-0 fingerprint
// (phxp2::rebasePipe0) under the same lock; a refused re-baseline latches the pipe-1 engine halted, as any other pipe-0
// change does. Head-0 flips (PhxAccel) are not paused here: the box procedure turns them off first (debug.phxfb_flip=0).
// SCDC2: a fourth plan, phx_p0_plan_1080p144a165.inc = the AOC at 144 next to
// the Acer at 1080p165 (from Linux, DENTIST max 0x2b); pairSwitch: a switch to or from 165 never re-plans pipe 0 first,
// pairPost follows OTG1 (PhxPipe0.h). p0PairGate is unchanged: pipe 0 at 144 needs the armed engine whatever the Acer runs.
// b123: debug.phxpipe0_live (root, default 0; b124 default 1): 1 = PhxPipe0.h liveShell for a pair
// re-plan (no AOC blank at an Acer switch); stat live_replans= live_fails= live_writes=.
// Part of the PhxBringup.cpp translation unit (included there after phx_gamma.cpp); not compiled on its own.

#include "PhxPipe0.h"
#include "phx_p0_plan_1080p60.inc"
#include "phx_p0_plan_1080p144.inc"
#include "phx_p0_plan_1080p144a60.inc"
#include "phx_p0_plan_1080p144a165.inc"   // SCDC2: the AOC at 144 next to the Acer at 1080p165 (from Linux)

static phxp0::State gP0;
static int gP0Enable;          // debug.phxpipe0_enable
static int gP0Mode = 60;       // debug.phxpipe0_mode
static int gP0Modes;           // b101 debug.phxpipe0_modes (property phx-pipe0-modes for PhxAMDFB head 0's mode list)
static int gP0UclkMhz = 456;   // b101 debug.phxpipe0_uclk: the UCLK hard minimum macSet sends before a set above 60 (0 = none)
static int gP0Solo;            // p0solo debug.phxpipe0_solo: 1 = the AOC alone (OTG1 dark) may take the 1080p144 plan (PhxPipe0.h pairFits)
static int gP0Live = 1;        // b123 debug.phxpipe0_live (b124: default 1, passed on the box): 1 = a pair re-plan of double-buffered frame / HUBP registers runs live (PhxPipe0.h liveShell)
static const phxp0::Plan kP0Plan60 = {kPhxP0_1080p60, sizeof kPhxP0_1080p60 / sizeof kPhxP0_1080p60[0], kPhxP0_1080p60Info};
static const phxp0::Plan kP0Plan144 = {kPhxP0_1080p144, sizeof kPhxP0_1080p144 / sizeof kPhxP0_1080p144[0], kPhxP0_1080p144Info};
static const phxp0::Plan kP0Plan144a60 = {kPhxP0_1080p144a60, sizeof kPhxP0_1080p144a60 / sizeof kPhxP0_1080p144a60[0], kPhxP0_1080p144a60Info};
static const phxp0::Plan kP0Plan144a165 = {kPhxP0_1080p144a165, sizeof kPhxP0_1080p144a165 / sizeof kPhxP0_1080p144a165[0], kPhxP0_1080p144a165Info};
static const phxp0::Plan *const kP0Plans[] = {&kP0Plan60, &kP0Plan144, &kP0Plan144a60, &kP0Plan144a165};
static constexpr unsigned kP0NPlans = sizeof kP0Plans / sizeof kP0Plans[0];

static uint64_t p0NowUs(void *) {
    uint64_t t, ns;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000;
}

// b101: one driver-mailbox SMU message (the path of phxctl smu / uclk.sh); RESP, 0 = no answer or not sent
static uint32_t p0Smu(void *c, uint32_t msg, uint32_t arg) {
    uint32_t resp = 0, after = 0;
    return static_cast<PhxBringup *>(c)->smuMessage(msg, arg, &resp, &after) == kIOReturnSuccess ? resp : 0;
}

static phxp0::Env p0Env() {
    phxp0::Env e{};
    e.io = {gPhx, p2IoRd, p2IoWr, p2IoMmRd, p2IoMmWr, p2IoDelay};
    e.enable = gP0Enable;
    e.solo = gP0Solo;
    e.live = gP0Live;
    e.p2Switching = gP2.switching;
    e.nowUs = p0NowUs;
    e.smu = p0Smu;
    return e;
}

// After a done or fell-back shell, under gP2Busy: the pipe-1 engine's pipe-0 baseline follows OTG0's new totals.
static void p0Rebase() {
    const char *w = nullptr;
    if (!phxp2::rebasePipe0(p2Env(), gP2, gP0.pubHTotal, gP0.pubVTotal, &w)) {
        gP2.halted = 1;
        IOLog("PhxBringup: pipe-0 mode set: the pipe-1 engine's pipe-0 re-baseline refused (%s): pipe-1 engine halted\n", w ? w : "?");
    }
}
static const char *p0ResName(int r) { return phxp0::kResName[r >= 0 && r <= phxp0::kDark ? r : phxp0::kRefused]; }
// b103: the plan for targetHz next to OTG1 (as it runs, or as e.otg1Next* says it will), under gP2Busy.
static const phxp0::Plan &p0Pick(const phxp0::Env &e, uint32_t targetHz) {
    const phxp0::Plan *p = phxp0::pick(e, gP0, kP0Plans, kP0NPlans, targetHz == 144 ? 144u : 60u);
    return p ? *p : kP0Plan60;
}
// macSet for target_hz with the plan for OTG1's mode, the caller holds gP2Busy.
static int p0MacRun(uint32_t targetHz, phxp0::MacOut *out, const char *who) {
    const phxp0::Env e = p0Env();
    const phxp0::Plan &tg = p0Pick(e, targetHz);
    const int r = phxp0::macSet(e, gP0, kP0Plans, kP0NPlans, tg, static_cast<uint32_t>(gP0UclkMhz), out);
    if (out->rebase) p0Rebase();
    IOLog("PhxBringup: pipe-0 %s to %u (%s): %s stage %d flags 0x%x (round trip %d) now %u Hz: %s\n", who, targetHz, tg.info.name, p0ResName(r), out->stage,
          out->flags, out->rtRes, out->curHz, gP0.why);
    return r;
}
// b101: macSet for target_hz under gP2Busy (taken here). Returns the result (-1 = not run: busy / not ready) and fills out.
static int p0MacLocked(uint32_t targetHz, phxp0::MacOut *out, int *err) {
    *err = 0;
    if (!p2Up()) { *err = ENXIO; return -1; }
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) { *err = EBUSY; return -1; }
    const int r = p0MacRun(targetHz, out, "macOS path");
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r;
}

// b103: the pipe-1 switch with the AOC at 144 (p2SwitchTo, under gP2Busy): the safe order of PhxPipe0.h pairSwitch
// (Acer 60 -> 144: pipe 0 to 1080p144 first, a pipe-0 refusal refuses the pipe-1 switch; Acer 144 -> 60: the switch
// first, then pipe 0 to 1080p144a60; b105: that post-step not landing = pipe 1 switched back). The pipe-1 engine refuses
// a switch with pipe 0 at 144 while this is not armed (phxp2::Env.p0Pair = debug.phxpipe0_enable and no pipe-0 latch).
static int p0PairArmed() { return gP0Enable && !gP0.dark && !gP0.halted; }
static void p0PairLog(void *, const char *when, int r, const phxp0::Plan *tg, const phxp0::MacOut &o) {
    IOLog("PhxBringup: pipe-0 pair re-plan %s to %s: %s stage %d flags 0x%x now %u Hz: %s\n", when, tg ? tg->info.name : "?", p0ResName(r),
          o.stage, o.flags, o.curHz, gP0.why);
}
static void p0PairRebase(void *) { p0Rebase(); }
// Pipe 0 at 144 with the engine not armed: the switch is refused before anything (false, why filled).
static bool p0PairGate(char *why, size_t n) {
    if (phxp2::p0ModeHz(p2Env()) != 144 || p0PairArmed()) return true;
    snprintf(why, n, "pipe 0 at 1080p144 and %s: its plan cannot follow pipe 1", !gP0Enable ? "debug.phxpipe0_enable = 0" : "the pipe-0 engine latched (dark / halted)");
    return false;
}
// The whole switch; sw = one pipe-1 switch (phx_pipe2.cpp p2SwOne). Returns the phxp2::SwRes; why = the pipe-0 part ("" = none).
static int p0PairSwitch(int from1Hz, int to1Hz, int (*sw)(void *, uint32_t), char *why, size_t n, bool *ran) {
    phxp0::PairSw o{};
    const phxp0::PairIo io = {nullptr, sw, p0PairRebase, p0PairLog};
    const int r = phxp0::pairSwitch(p0Env(), gP0, kP0Plans, kP0NPlans, static_cast<uint32_t>(from1Hz), static_cast<uint32_t>(to1Hz),
                                    static_cast<uint32_t>(gP0UclkMhz), io, &o);
    strlcpy(why, o.why, n);
    *ran = o.ran;
    if (o.why[0]) IOLog("PhxBringup: pipe-1 switch %d -> %d with pipe 0: %s\n", from1Hz, to1Hz, o.why);
    return r;
}
static void p0PairCounts(uint32_t *postFails, uint32_t *rollbacks) { *postFails = gP0.pairPostFails; *rollbacks = gP0.pairRollbacks; }

// b101: PhxAMDFB head 0 setDisplayMode (PhxSwitchABI.h; dispatched in phx_client.cpp).
static IOReturn phxPipe0SetModeFn(phx_pipe0_setmode *m) {
    if (!m || m->version != PHX_PIPE0_SETMODE_VERSION) return kIOReturnBadArgument;
    m->result = -1; m->stage = 0; m->cur_hz = 0; m->flags = 0; m->why[0] = 0;
    if (m->target_hz != 60 && m->target_hz != 144) { strlcpy(m->why, "bad target (60 or 144)", sizeof m->why); return kIOReturnBadArgument; }
    phxp0::MacOut o{};
    int err = 0;
    const int r = p0MacLocked(m->target_hz, &o, &err);
    if (r < 0) {
        strlcpy(m->why, err == EBUSY ? "busy (another display-engine command runs)" : "PhxBringup not ready", sizeof m->why);
        return err == EBUSY ? kIOReturnBusy : kIOReturnNotReady;
    }
    m->result = r; m->stage = o.stage; m->cur_hz = o.curHz; m->flags = o.flags;
    snprintf(m->why, sizeof m->why, "%s%s", o.rtRes >= 0 && o.rtRes != phxp0::kDone ? "round trip: " : "", gP0.why);
    return kIOReturnSuccess;
}

static int phxPipe0Sysctl SYSCTL_HANDLER_ARGS {
    int v = gP0.lastRes;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    int r;
    if (v == phxp0::kCmdMac) {   // b101: the macOS path's sequence (stutter off, UCLK pin, round trip if needed, set)
        phxp0::MacOut o{};
        int busyErr = 0;
        r = p0MacLocked(static_cast<uint32_t>(gP0Mode), &o, &busyErr);
        if (r < 0) return busyErr;
    } else {
        if (!p2Up()) return ENXIO;
        if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
        const phxp0::Plan &tg = p0Pick(p0Env(), static_cast<uint32_t>(gP0Mode));
        r = phxp0::run(p0Env(), gP0, kP0Plans, kP0NPlans, tg, v);
        if ((r == phxp0::kDone || r == phxp0::kFellBack) && v != phxp0::kCmdSurvey) p0Rebase();
        IOLog("PhxBringup: debug.phxpipe0=%d target %d: %s stage %d: %s\n", v, gP0Mode, p0ResName(r), gP0.lastStage, gP0.why);
        OSCompareAndSwap(1, 0, &gP2Busy);
    }
    return r == phxp0::kDone || r == phxp0::kSame ? 0 : r == phxp0::kRefused ? EINVAL : r == phxp0::kFellBack ? EIO : ENXIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe0Sysctl, "I",
            "b99 pipe-0 (AOC) mode engine (root; needs debug.phxpipe0_enable=1): 1 survey, 2 set debug.phxpipe0_mode, 3 round trip, 4 (b101) the macOS path's sequence to debug.phxpipe0_mode; reads the last result");

static int phxPipe0EnableSysctl SYSCTL_HANDLER_ARGS {
    int v = gP0Enable;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 0 && v != 1) return EINVAL;
    gP0Enable = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_enable, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe0EnableSysctl, "I",
            "b99 pipe-0 engine kill switch (root): 0 off (default, every command refused before any register access), 1 on");

static int phxPipe0ModeSysctl SYSCTL_HANDLER_ARGS {
    int v = gP0Mode;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 60 && v != 144) return EINVAL;
    gP0Mode = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_mode, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe0ModeSysctl, "I",
            "b99 pipe-0 target mode (root): 60 (default) or 144; applied by debug.phxpipe0=2");

// b101: PhxAMDFB head 0 reads the property phx-pipe0-modes once, when it starts (macOS caches a framebuffer's mode list).
static int phxPipe0ModesSysctl SYSCTL_HANDLER_ARGS {
    int v = gP0Modes;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 0 && v != 1) return EINVAL;
    if (!gPhx) return ENXIO;
    gP0Modes = v;
    gPhx->setProperty("phx-pipe0-modes", v ? kOSBooleanTrue : kOSBooleanFalse);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_modes, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe0ModesSysctl, "I",
            "b101 (root): 1 = PhxAMDFB head 0 lists the AOC's 60 and 144 Hz modes (read when head 0 starts: set before the claim)");
static int phxPipe0UclkSysctl SYSCTL_HANDLER_ARGS {
    int v = gP0UclkMhz;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 0 && (v < 96 || v > 2500)) return EINVAL;
    gP0UclkMhz = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_uclk, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe0UclkSysctl, "I",
            "b101 (root): UCLK hard minimum in MHz the macOS path sends before a pipe-0 set above 60 Hz (default 456 = uclk.sh pin 456; 0 = none)");

// p0solo: the boot daemon's b15_boot.sh aocboot1 / the one-display
// branch write 1 when only the AOC is lit; 0 (default) = the pair rule as before (no 144 with OTG1 dark).
static int phxPipe0SoloSysctl SYSCTL_HANDLER_ARGS {
    int v = gP0Solo;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 0 && v != 1) return EINVAL;
    gP0Solo = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_solo, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe0SoloSysctl, "I",
            "p0solo (root): 1 = with OTG1 dark (the AOC alone) the 1080p144 plan may be set (needs DET0 9); 0 (default) = 144 only next to the Acer");

// b123: 1 = an Acer switch next to the AOC at 144 re-plans pipe 0 (1080p144 <->
// 1080p144a165 / 1080p144a60) live under the OTG0 lock, as Linux, instead of the blanking shell; 0 (default) = b122.
static int phxPipe0LiveSysctl SYSCTL_HANDLER_ARGS {
    int v = gP0Live;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 0 && v != 1) return EINVAL;
    gP0Live = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_live, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe0LiveSysctl, "I",
            "b123 (root): 1 = a same-timing pipe-0 pair re-plan of double-buffered registers runs live (no AOC blank); 0 (default) = the full shell");

static int phxPipe0StatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    char b[1536];   // b105: + refused_write= pair_post_fails= pair_rollbacks=; b106: + stream_wait / dig_fifo fields (worst case 1252 B)
    int len = phxp0::stat(gP0, gP0Enable, gP0Mode, b, sizeof b);
    if (len < 0) len = 0;
    if (len > static_cast<int>(sizeof b) - 1) len = sizeof b - 1;
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxPipe0StatSysctl, "A",
            "b99 pipe-0 engine: busy (the b15 watchdog skips pipe 0), gen, mode, last result, stage, latches, counters, why");

static int phxPipe0PlanSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    static constexpr int kN = 16384;   // b103: three plans
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) { IOFree(b, kN); return EBUSY; }
    int len = phxp0::planDump(p0Env(), kP0Plans, kP0NPlans, b, kN);
    OSCompareAndSwap(1, 0, &gP2Busy);
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_plan, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxPipe0PlanSysctl, "A",
            "b99 pipe-0 plans (b103: 1080p60, 1080p144, 1080p144a60; SCDC2: 1080p144a165): every entry with its register now (root, reads only)");

static int phxPipe0DeadSysctl SYSCTL_HANDLER_ARGS {
    int v = gP0.dark;
    if (req->newptr) return EPERM;
    return sysctl_handle_int(oidp, &v, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe0_dead, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxPipe0DeadSysctl, "I",
            "b99: 1 = a pipe-0 revert failed (dark): no pipe-0 write until reboot");
