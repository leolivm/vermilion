// SPDX-License-Identifier: MIT
// phx_pipe2.cpp: b82 (M7 S2): the pipe-1 ladder
// debug.phxpipe2 (0 undo, 1 survey, 2 ping, 3 stream, 4 plane; root, never armed at boot), debug.phxpipe2_stat,
// debug.phxpipe2_plan, debug.phxdmub_ring, debug.phxdmub_dead, and the indirect VRAM window (MM_INDEX/MM_DATA) they use.
// The engine is PhxPipe2.h + PhxDmub.h (kernel-free, host-tested); the plan is
// phx_s2_plan_1080p60.inc (generated from a Linux register capture). Nothing here runs unless root writes debug.phxpipe2 or reads
// debug.phxdmub_ring. b109: debug.phxdmub_restart(_stat) / debug.phxdmub_trace_pre and debug.phxscdc_fix(_stat), at the end;
// b111: debug.phxdmub_regtest(_stat) after them; b113: debug.phxc3(_stat) and debug.phxpoweron at the end.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

#include "PhxPipe2.h"
#include "PhxSwitchABI.h"
#include "phx_s2_plan_1080p60.inc"
#include "phx_s2_plan_head1_1920.inc"
#include "phx_s2_plan_1080p144.inc"
#include "phx_s2_plan_head1_1920_144.inc"
#include "phx_s2_plan_1080p165.inc"          // b108 SCDC2: Linux's 11-01 capture (mode 1080p165)
#include "phx_s2_plan_head1_1920_165.inc"

// The BIF index pair is shared state: each index + data triple runs under gMmIdxLock (phx_globals.cpp) with interrupts
// off, so no other user of the pair (ReadRegs/WriteReg of BAR5 dwords 0-6 take the same lock; nothing else in the kext
// uses the pair) can move MM_INDEX between our index write and our data access. Offsets are VRAM byte offsets, dword
// aligned, inside VRAM (the engine keeps them inside the DMCUB ring).
uint32_t PhxBringup::mmIdxRead(uint64_t off) {
    if (!mmio || !gMmIdxLock || (off & 3) || off + 4 > fbSize) return 0xffffffffu;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(gMmIdxLock);
    wreg(phxdmub::kMmIndexHi, phxdmub::mmIndexHi(off));
    wreg(phxdmub::kMmIndex, phxdmub::mmIndexLo(off));
    const uint32_t v = rreg(phxdmub::kMmData);
    IOSimpleLockUnlockEnableInterrupt(gMmIdxLock, s);
    return v;
}

void PhxBringup::mmIdxWrite(uint64_t off, uint32_t v) {
    if (!mmio || !gMmIdxLock || (off & 3) || off + 4 > fbSize) return;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(gMmIdxLock);
    wreg(phxdmub::kMmIndexHi, phxdmub::mmIndexHi(off));
    wreg(phxdmub::kMmIndex, phxdmub::mmIndexLo(off));
    wreg(phxdmub::kMmData, v);
    IOSimpleLockUnlockEnableInterrupt(gMmIdxLock, s);
}

uint32_t PhxBringup::p2Rd(uint32_t reg) const {
    if (!mmio || (static_cast<uint64_t>(reg) + 1) * 4 > mmio->getLength()) return 0xffffffffu;
    return rreg(reg);
}

void PhxBringup::p2Wr(uint32_t reg, uint32_t v) const {
    if (!mmio || (static_cast<uint64_t>(reg) + 1) * 4 > mmio->getLength()) return;
    wreg(reg, v);
}

static phxp2::State gP2;
static volatile UInt32 gP2Busy;   // one engine run or ring dump at a time
static const phxp2::Plan kP2Plan = {kPhxS2, sizeof kPhxS2 / sizeof kPhxS2[0], kPhxS2Req, sizeof kPhxS2Req / sizeof kPhxS2Req[0],
                                    kPhxS2Undo, sizeof kPhxS2Undo / sizeof kPhxS2Undo[0], kPhxS2NoRestore,
                                    sizeof kPhxS2NoRestore / sizeof kPhxS2NoRestore[0], kPhxS2Otg1Hz, nullptr};
// b90: the 1080p144 plan (mode 1080p144: GOP 144 frame group + hubp_dlg rate group; needs DENTIST <= 0x33
// for DISPCLK and DPPCLK = the two DAL hard mins BEFORE the survey)
static const phxp2::Plan kP2Plan144 = {kPhxS2_144, sizeof kPhxS2_144 / sizeof kPhxS2_144[0], kPhxS2_144Req,
                                       sizeof kPhxS2_144Req / sizeof kPhxS2_144Req[0], kPhxS2_144Undo,
                                       sizeof kPhxS2_144Undo / sizeof kPhxS2_144Undo[0], kPhxS2_144NoRestore,
                                       sizeof kPhxS2_144NoRestore / sizeof kPhxS2_144NoRestore[0], kPhxS2_144Otg1Hz, nullptr};
// b108 SCDC2: the 1080p165 plan (HDMI 2.0: SCDC steps, scrambling; DAL hard mins DISPCLK/DPPCLK 391 MHz, DENTIST <= 0x2b)
static const phxp2::Plan kP2Plan165 = {kPhxS2_165, sizeof kPhxS2_165 / sizeof kPhxS2_165[0], kPhxS2_165Req,
                                       sizeof kPhxS2_165Req / sizeof kPhxS2_165Req[0], kPhxS2_165Undo,
                                       sizeof kPhxS2_165Undo / sizeof kPhxS2_165Undo[0], kPhxS2_165NoRestore,
                                       sizeof kPhxS2_165NoRestore / sizeof kPhxS2_165NoRestore[0], kPhxS2_165Otg1Hz, kPhxS2_165DalMin};
static constexpr unsigned kP2N60 = sizeof kPhxS2 / sizeof kPhxS2[0], kP2N144 = sizeof kPhxS2_144 / sizeof kPhxS2_144[0],
                          kP2N165 = sizeof kPhxS2_165 / sizeof kPhxS2_165[0];
static constexpr unsigned kP2MaxSteps = kP2N60 > kP2N144 ? (kP2N60 > kP2N165 ? kP2N60 : kP2N165) : (kP2N144 > kP2N165 ? kP2N144 : kP2N165);
// b91: two alt-steps buffers, so a mode switch can prepare the target while the current plan (maybe the other alt) is lit
static phx_s2_step gP2AltSteps[2][kP2MaxSteps];
static phxp2::Plan gP2AltPlan[2];
static const phxp2::Plan *gP2Plan = &kP2Plan;   // switched only under gP2Busy
static int gP2Head1;                            // 0 = the mode's plan (head 1 1680x1050), 1 = its 1920x1080 values
static int gP2Mode = 60;                        // b90: 60 = kP2Plan, 144 = kP2Plan144, b108 165 = kP2Plan165 (debug.phxpipe2_mode, before the survey)
// The plan for (mode, head 1 size) in the alt buffer the current plan does not use, under gP2Busy. Null = the head-1
// values do not fit the mode's plan.
static const phxp2::Plan *p2Build(int mode, int head1, const char **why) {
    const phxp2::Plan *base = mode == 165 ? &kP2Plan165 : mode == 144 ? &kP2Plan144 : &kP2Plan;
    if (!head1) return base;
    const unsigned slot = gP2Plan == &gP2AltPlan[0] ? 1 : 0;
    const phx_s2_alt *a = mode == 165 ? kPhxS2_165Head1Alt : mode == 144 ? kPhxS2_144Head1Alt : kPhxS2Head1Alt;
    const unsigned na = mode == 165 ? sizeof kPhxS2_165Head1Alt / sizeof kPhxS2_165Head1Alt[0]
                      : mode == 144 ? sizeof kPhxS2_144Head1Alt / sizeof kPhxS2_144Head1Alt[0] : sizeof kPhxS2Head1Alt / sizeof kPhxS2Head1Alt[0];
    if (!phxp2::applyAlt(gP2AltSteps[slot], kP2MaxSteps, *base, a, na, why)) return nullptr;
    gP2AltPlan[slot] = *base; gP2AltPlan[slot].s = gP2AltSteps[slot];
    return &gP2AltPlan[slot];
}
// Select it as the plan the ladder runs, under gP2Busy. False = nothing changed.
static bool p2Select(int mode, int head1, const char **why) {
    const phxp2::Plan *p = p2Build(mode, head1, why);
    if (!p) return false;
    gP2Plan = p; gP2Mode = mode; gP2Head1 = head1 ? 1 : 0;
    return true;
}
static uint32_t p2IoRd(void *c, uint32_t r) { return static_cast<PhxBringup *>(c)->p2Rd(r); }
static void p2IoWr(void *c, uint32_t r, uint32_t v) { static_cast<PhxBringup *>(c)->p2Wr(r, v); }
static uint32_t p2IoMmRd(void *c, uint64_t off) { return static_cast<PhxBringup *>(c)->mmIdxRead(off); }
static void p2IoMmWr(void *c, uint64_t off, uint32_t v) { static_cast<PhxBringup *>(c)->mmIdxWrite(off, v); }
static void p2IoDelay(void *, uint32_t us) {
    if (us >= 1000) IOSleep(us / 1000);
    if (us % 1000) IODelay(us % 1000);
}
// b91 mode switch: one DAL message (RESP, 0 = timeout, which latches gDalDead in dalMessage); the PMFW is idle when
// neither the DAL mailbox nor the driver mailbox has a message in flight (RESP != 0 on both).
static uint32_t p2Dal(void *c, uint32_t msg, uint32_t arg) {
    uint32_t resp = 0, after = 0;
    static_cast<PhxBringup *>(c)->dalMessage(msg, arg, &resp, &after);
    return resp;
}
static int p2PmfwIdle(void *c) {
    const PhxBringup *b = static_cast<PhxBringup *>(c);
    for (unsigned i = 0; i < 20; i++) {   // up to 200 ms
        if (b->p2Rd(0x16274) != 0 && b->p2Rd(PHX_SMU_RESP_REG) != 0) return 1;
        IOSleep(10);
    }
    return 0;
}
// SCDC1: the DC_I2C engine is shared with the EDID reads (debug.phxedid, phxEdidRead, the hot-plug policy): an I2C plan
// step takes their edidBusy, waiting for it at most ~100 ms like edidFetch.
static int p2I2cLock(void *c, int take) {
    PhxBringup *b = static_cast<PhxBringup *>(c);
    if (!take) { OSCompareAndSwap(1, 0, &b->edidBusy); return 1; }
    for (unsigned i = 0; i < 20; i++) {
        if (OSCompareAndSwap(0, 1, &b->edidBusy)) return 1;
        IOSleep(5);
    }
    return 0;
}
// b103: the pipe-0 pair hooks of the switch (phx_pipe0.cpp; b105:
// the whole switch in phxp0::pairSwitch)
static int p0PairArmed();
static bool p0PairGate(char *why, size_t n);
static int p0PairSwitch(int from1Hz, int to1Hz, int (*sw)(void *, uint32_t), char *why, size_t n, bool *ran);
static void p0PairCounts(uint32_t *postFails, uint32_t *rollbacks);
static phxp2::Env p2Env() {
    phxp2::Env e{};
    e.io = {gPhx, p2IoRd, p2IoWr, p2IoMmRd, p2IoMmWr, p2IoDelay};
    e.vramBytes = gPhx->p2VramBytes();
    e.fbMcBase = gPhx->vramMcBase();
    e.dalDead = gDalDead;
    e.dal = p2Dal;
    e.pmfwIdle = p2PmfwIdle;
    e.p0Pair = p0PairArmed();
    e.i2cLock = p2I2cLock;
    return e;
}
static bool p2Up() { return gPhx && gPhx->p2Ready() && gMmIdxLock && gPhx->p2VramBytes(); }

static int phxPipe2Sysctl SYSCTL_HANDLER_ARGS {
    int v = gP2.level;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;                    // writes the display engine: root only
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    const int r = phxp2::run(p2Env(), gP2, *gP2Plan, v);
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r == phxp2::kDone ? 0 : r == phxp2::kRefused ? EINVAL : EIO;   // debug.phxpipe2_stat says why
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe2Sysctl, "I",
            "b82 M7 S2 pipe-1 ladder (root): 1 survey, 2 ping, 3 stream, 4 plane, 0 undo; reads the level passed");

static int phxPipe2Head1Sysctl SYSCTL_HANDLER_ARGS {
    int v = gP2Head1;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 0 && v != 1) return EINVAL;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    int r = 0;
    if (!phxp2::planeUntouched(gP2, *gP2Plan)) r = EBUSY;   // the plane is (partly) applied: undo (0) first
    else {
        const char *w = nullptr;
        if (!p2Select(gP2Mode, v, &w)) { IOLog("PhxBringup: debug.phxpipe2_head1=%d refused: %s\n", v, w); r = EINVAL; }
    }
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r;
}

// b90: the pipe-1 mode, 60 or 144 Hz. Only before the survey (level 0, nothing touched): the survey checks the chosen
// plan's requirements (DENTIST <= 0x33 at 144) and records the pipe-0 baseline the later levels compare with.
static int phxPipe2ModeSysctl SYSCTL_HANDLER_ARGS {
    int v = gP2Mode;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 60 && v != 144 && v != 165) return EINVAL;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    int r = 0;
    const char *w = nullptr;
    if (gP2.level != 0 || gP2.touched != 0) r = EBUSY;   // after the survey or with anything applied: undo (0) first
    else if (!p2Select(v, gP2Head1, &w)) { IOLog("PhxBringup: debug.phxpipe2_mode=%d refused: %s\n", v, w); r = EINVAL; }
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2_mode, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe2ModeSysctl, "I",
            "b90 pipe-1 mode (root, before the survey): 60 = 1080p60 (default), 144 = 1080p144, b108 165 = 1080p165 HDMI 2.0 (both need DISPCLK/DPPCLK hard mins first)");
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2_head1, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe2Head1Sysctl, "I",
            "b87 pipe-1 plane size (root, before the plane): 0 head 1 1680x1050, 1 head 1 1920x1080x7680");

// b91: switch the lit pipe-1 mode, 60 <-> 144 (b108: <-> 165), keeping the head-1
// size. Reads the current mode. 0 done (or same), EINVAL refused (nothing touched), EIO fell back to the previous mode,
// ENXIO dark (pipe 1 off until reboot); debug.phxpipe2_switch_stat says why.
static phxp2::Sw gP2Sw;
// b105: one pipe-1 switch to hz (phxp0::pairSwitch's callback, also its switch back), under gP2Busy. Returns the SwRes.
static int p2SwOne(void *, uint32_t hz) {
    const char *w = nullptr;
    const phxp2::Plan *t = static_cast<int>(hz) == gP2Mode ? gP2Plan : p2Build(static_cast<int>(hz), gP2Head1, &w);
    if (!t) {
        snprintf(gP2Sw.why, sizeof gP2Sw.why, "target plan: %s", w ? w : "?");
        gP2Sw.res = phxp2::kSwRefused; gP2Sw.stage = 0; gP2.switchRefused++;
        return gP2Sw.res;
    }
    gP2Sw.cur = gP2Plan; gP2Sw.target = t; gP2Sw.failAt = 0;
    const int sr = phxp2::modeSwitch(p2Env(), gP2, gP2Sw);
    if (sr == phxp2::kSwDone) { gP2Plan = t; gP2Mode = static_cast<int>(hz); }
    return sr;
}
// One switch under gP2Busy. Returns the errno the sysctl gives; -1 in *res = not run (busy / not ready).
static int p2SwitchTo(int v, int *res) {
    *res = -1;
    if (v != 60 && v != 144 && v != 165) return EINVAL;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    char pw[160] = "";
    const char *w = nullptr;
    int sr;
    if (v == gP2Mode || !p2Build(v, gP2Head1, &w)) sr = p2SwOne(nullptr, static_cast<uint32_t>(v));   // same mode / no target plan: as before
    else if (!p0PairGate(pw, sizeof pw)) {   // pipe 0 at 144 with the pipe-0 engine off / latched
        snprintf(gP2Sw.why, sizeof gP2Sw.why, "pipe 0: %s", pw);
        gP2Sw.res = sr = phxp2::kSwRefused; gP2Sw.stage = 0; gP2.switchRefused++;
    } else {
        // b103/b105: the safe order around the switch (phxp0::pairSwitch): pipe 0 at 144 follows OTG1; a post-step that
        // does not land switches pipe 1 back; the pipe-0 reason replaces the switch's own why
        bool ran = false;
        sr = p0PairSwitch(gP2Mode, v, p2SwOne, pw, sizeof pw, &ran);
        if (!ran) { gP2Sw.stage = 0; gP2.switchRefused++; }   // the pre-step refused it: pipe 1 untouched
        if (pw[0]) { gP2Sw.res = sr; strlcpy(gP2Sw.why, pw, sizeof gP2Sw.why); }
    }
    const int r = sr == phxp2::kSwDone || sr == phxp2::kSwSame ? 0 : sr == phxp2::kSwRefused ? EINVAL : sr == phxp2::kSwFellBack ? EIO : ENXIO;
    *res = gP2Sw.res;
    IOLog("PhxBringup: pipe-1 switch to %d: %s stage %d: %s\n", v, phxp2::kSwName[gP2Sw.res], gP2Sw.stage, gP2Sw.why);
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r;
}
static int phxPipe2SwitchSysctl SYSCTL_HANDLER_ARGS {
    int v = gP2Mode;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    int res;
    return p2SwitchTo(v, &res);
}
// b93: the same switch for PhxAccel's head-1 setDisplayMode (PhxSwitchABI.h; dispatched in phx_client.cpp).
static IOReturn phxPipe2SetModeFn(phx_pipe2_setmode *m) {
    if (!m || m->version != PHX_PIPE2_SETMODE_VERSION) return kIOReturnBadArgument;
    int res;
    const int e = p2SwitchTo(static_cast<int>(m->target_hz), &res);
    m->result = res;
    m->stage = res >= 0 ? gP2Sw.stage : 0;
    m->cur_hz = static_cast<uint32_t>(gP2Mode);
    strlcpy(m->why, res >= 0 ? gP2Sw.why : e == EBUSY ? "busy" : e == ENXIO ? "PhxBringup not ready" : "bad target", sizeof m->why);
    return res >= 0 ? kIOReturnSuccess : e == EBUSY ? kIOReturnBusy : kIOReturnNotReady;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2_switch, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe2SwitchSysctl, "I",
            "b91 switch the lit pipe-1 mode (root): 60, 144 or (b108) 165; reads the current mode");
static int phxPipe2SwitchStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    char b[448];
    uint32_t p0Fail = 0, p0Back = 0;
    p0PairCounts(&p0Fail, &p0Back);   // b105: post-steps that did not land / pipe-1 switches back (phxp0::pairSwitch)
    int len = snprintf(b, sizeof b, "busy=%d gen=%u mode=%d head1=%d last=%s stage=%d phase_lag=%u phase_tries=%u ok=%u fell=%u dark=%u refused=%u dal=%u p0stale=%u p0post_fail=%u p0back=%u why=%s",
                       gP2.switching, gP2.switchGen, gP2Mode, gP2Head1, gP2.switchGen || gP2.switchRefused || gP2Sw.why[0] ? phxp2::kSwName[gP2Sw.res] : "none", gP2Sw.stage, gP2Sw.phaseLag, gP2Sw.phaseTries,
                       gP2.switchOk, gP2.switchFell, gP2.switchDark, gP2.switchRefused, gP2.dalSent, gP2.p0StaleOk, p0Fail, p0Back, gP2Sw.why);
    if (len < 0) len = 0;
    if (len > static_cast<int>(sizeof b) - 1) len = sizeof b - 1;
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2_switch_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxPipe2SwitchStatSysctl, "A",
            "b91 last pipe-1 mode switch: busy (b15 watchdog ignores pipe 1), result, stage, counters, why");

// b91: the boot scripts' two pipe-1 helpers in the kext (m7_phase.sh, m7_boot.sh det99). debug.phxpipe2_phase=1 aligns
// OTG1's vblank with OTG0's (same V_TOTALs only; reads the last lag in lines, -1 = never run);
// debug.phxpipe2_det=9 sets DET 9/9/0/0 (reads DET1's size).
static int gP2PhaseLag = -1;
static int phxPipe2PhaseSysctl SYSCTL_HANDLER_ARGS {
    int v = gP2PhaseLag;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 1) return EINVAL;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    int r = 0;
    if (gP2.halted || gP2.level < 3) r = EINVAL;   // needs pipe 1 streaming and a pipe-0 baseline
    else {
        unsigned tries = 0; uint32_t lag = 0; const char *w = nullptr;
        const int res = phxp2::phaseAlign(p2Env(), gP2, 10, &tries, &lag, &w);
        gP2PhaseLag = static_cast<int>(lag);
        IOLog("PhxBringup: debug.phxpipe2_phase: %s, lag %u, tries %u%s%s\n", res == phxp2::kPhaseAligned ? "aligned" : res == phxp2::kPhaseNotNeeded ? "not needed" : "FAILED",
              lag, tries, w ? ": " : "", w ? w : "");
        r = res == phxp2::kPhaseFailed ? EIO : 0;
    }
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2_phase, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe2PhaseSysctl, "I",
            "b91 1 = align OTG1's vblank with OTG0's (root, pipe 1 streaming); reads the last lag in lines");
static int phxPipe2DetSysctl SYSCTL_HANDLER_ARGS {
    int v = gPhx && p2Up() ? static_cast<int>(gPhx->p2Rd(0x0397C) & 0x1f) : -1;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 9) return EINVAL;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    const char *w = nullptr;
    const bool ok = phxp2::detTwoPipe(p2Env(), gP2, &w);
    if (!ok) IOLog("PhxBringup: debug.phxpipe2_det=9 refused: %s\n", w ? w : "?");
    OSCompareAndSwap(1, 0, &gP2Busy);
    return ok ? 0 : EIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2_det, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxPipe2DetSysctl, "I",
            "b91 9 = DET 9/9/0/0, the two-display split (root; HUBP2/3 clocks off); reads DET1's size");

// b107 SCDC1: the Acer's SCDC as the sink sees the link, read over DDC1
// (the board's HDMI connector) with phxddc::scdcReadStatus: reads only (each transfer writes just the offset pointer), no
// SCDC register written, under edidBusy like the EDID reads; never touches the plan engine (no gP2Busy). Root only.
// "rc= sink_version= tmds_config= scrambler= flags0= locked= err0= err1= err2= status= step=" (err -1 = not valid).
static int phxScdcSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    if (proc_suser(req->p)) return EPERM;   // it drives the DC_I2C engine
    if (!gPhx || !gPhx->p2Ready()) return ENXIO;
    if (!p2I2cLock(gPhx, 1)) return EBUSY;
    phxp2::Env e{};
    e.io = {gPhx, p2IoRd, p2IoWr, p2IoMmRd, p2IoMmWr, p2IoDelay};
    phxp2::I2cGate g{&e, 0};
    const phxddc::Io io{&g, phxp2::i2cGateRd, phxp2::i2cGateWr, phxp2::i2cGateDelay};
    phxddc::Diag d{};
    phxddc::ScdcStatus s{};
    const int rc = phxddc::scdcReadStatus(io, 0, &s, &d);
    p2I2cLock(gPhx, 0);
    char b[256];
    int len = snprintf(b, sizeof b, "rc=%d sink_version=%u tmds_config=0x%02x scrambler=0x%02x flags0=0x%02x locked=%d err0=%d err1=%d err2=%d status=0x%x step=0x%02x stray=%u",
                       rc, s.sinkVersion, s.tmdsConfig, s.scrambler, s.flags0, phxddc::scdcLocked(s) ? 1 : 0, phxddc::scdcErrCount(s, 0),
                       phxddc::scdcErrCount(s, 1), phxddc::scdcErrCount(s, 2), d.status, d.step, g.stray);
    if (len < 0) len = 0;
    if (len > static_cast<int>(sizeof b) - 1) len = sizeof b - 1;
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxscdc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxScdcSysctl, "A",
            "b107 SCDC of the HDMI sink on DDC1 (root, reads only): version, TMDS_Config, scrambler, lock flags, error counters");

static int phxPipe2StatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    char *b = static_cast<char *>(IOMalloc(8192));
    if (!b) return ENOMEM;
    const bool live = p2Up() && !proc_suser(req->p);   // the register section only for root
    phxp2::Env ne{};
    ne.dalDead = gDalDead;
    int len = gPhx ? phxp2::stat(live ? p2Env() : ne, gP2, *gP2Plan, b, 8192, live) : snprintf(b, 8192, "no PhxBringup");
    if (len < 0) len = 0;
    if (len > 8191) len = 8191;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, 8192);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxPipe2StatSysctl, "A",
            "b82 pipe-1 ladder: level, last step, failure, latches, DMUB counters, requirements, live registers (root)");

static int phxPipe2PlanSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    static constexpr int kN = 32768;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len = phxp2::planDump(p2Env(), gP2, *gP2Plan, b, kN);
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpipe2_plan, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxPipe2PlanSysctl, "A",
            "b82 pipe-1 plan: every entry, its register now and at the survey (root, reads only)");

// The GOP's ring history: 'PDMR', ring VRAM lo/hi, size, WPTR, RPTR, rc, 0, then the ring. Reads only, through the
// indirect window (it writes MM_INDEX/MM_INDEX_HI under gMmIdxLock); allowed after a DMUB timeout (diagnosis).
static int phxDmubRingSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    const phxp2::Env e = p2Env();
    phxdmub::Ring r{};
    const char *why = nullptr;
    if (!req->oldptr) {   // size query: registers only
        const bool ok = phxdmub::ringGeometry(e.io, e.vramBytes, &r, &why);
        return SYSCTL_OUT(req, nullptr, 32 + (ok ? r.size : 0));
    }
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    uint32_t *buf = static_cast<uint32_t *>(IOMalloc(phxdmub::kWinBytes));
    if (!buf) { OSCompareAndSwap(1, 0, &gP2Busy); return ENOMEM; }
    const bool ok = phxdmub::ringRead(e.io, e.vramBytes, &r, buf, phxdmub::kWinBytes, &why);
    OSCompareAndSwap(1, 0, &gP2Busy);
    const uint32_t hdr[8] = {0x524d4450u, static_cast<uint32_t>(r.vram), static_cast<uint32_t>(r.vram >> 32), r.size, r.wptr, r.rptr, ok ? 0u : 1u, 0};
    int err = SYSCTL_OUT(req, hdr, sizeof hdr);
    if (!err && ok) err = SYSCTL_OUT(req, buf, r.size);
    IOFree(buf, phxdmub::kWinBytes);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdmub_ring, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDmubRingSysctl, "S",
            "b82 DMCUB inbox1 ring dump (root, reads only through MM_INDEX/MM_DATA): 'PDMR', vram lo/hi, size, wptr, rptr, rc, 0, ring");

// b88: the firmware's trace buffer (window CW5): 'PDMT', vram lo/hi, size, OUTBOX0_WPTR, SCRATCH0, DMCUB_CNTL, rc, then
// the window. Reads only, through the indirect window; allowed after a DMUB timeout (that is when it matters).
static int phxDmubTraceSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    const phxp2::Env e = p2Env();
    phxdmub::Trace t{};
    const char *why = nullptr;
    if (!req->oldptr) {   // size query: registers only
        const bool ok = phxdmub::traceGeometry(e.io, e.vramBytes, &t, &why);
        return SYSCTL_OUT(req, nullptr, 32 + (ok ? t.size : 0));
    }
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    uint32_t *buf = static_cast<uint32_t *>(IOMalloc(phxdmub::kWinBytes));
    if (!buf) { OSCompareAndSwap(1, 0, &gP2Busy); return ENOMEM; }
    const bool ok = phxdmub::traceRead(e.io, e.vramBytes, &t, buf, phxdmub::kWinBytes, &why);
    OSCompareAndSwap(1, 0, &gP2Busy);
    const uint32_t hdr[8] = {0x544d4450u, static_cast<uint32_t>(t.vram), static_cast<uint32_t>(t.vram >> 32), t.size, t.wptr,
                             gPhx->p2Rd(phxdmub::kScratch0), gPhx->p2Rd(phxdmub::kCntl), ok ? 0u : 1u};
    int err = SYSCTL_OUT(req, hdr, sizeof hdr);
    if (!err && ok) err = SYSCTL_OUT(req, buf, t.size);
    IOFree(buf, phxdmub::kWinBytes);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdmub_trace, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDmubTraceSysctl, "S",
            "b88 DMCUB trace buffer dump (root, reads only through MM_INDEX/MM_DATA): 'PDMT', vram lo/hi, size, outbox0 wptr, scratch0, cntl, rc, window");

static int phxDmubDeadSysctl SYSCTL_HANDLER_ARGS {
    int v = gP2.dmub.dead;
    if (req->newptr) return EPERM;
    return sysctl_handle_int(oidp, &v, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, phxdmub_dead, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDmubDeadSysctl, "I",
            "b82: 1 = a DMUB command timed out; DMUB and debug.phxpipe2 refused until reboot");

// b109 (cold-boot dark Acer, candidate C1): restart the GOP DMCUB firmware without an
// image reload or a PSP message, one shot per boot, before any pipe-1 DMUB command. Root writes 1 (power-on chain only:
// DC_GPIO_GENERIC_A bit 8), 2 (any chain), 3 (any chain + the GOP's 0x0C post-start command) or 4 (power-on chain + 0x0C);
// reads the last result (-1 never, else phxdmub::RsRc). Under gP2Busy like the ladder. debug.phxdmub_restart_stat says
// what happened; debug.phxdmub_trace_pre is the CW5 trace copied just before the first write.
static phxdmub::RestartSt gDmubRs;
static uint32_t gDmubTracePre[phxdmub::kWinBytes / 4];
static int phxDmubRestartSysctl SYSCTL_HANDLER_ARGS {
    int v = gDmubRs.attempts ? gDmubRs.rc : -1;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    const int rc = phxp2::dmubRestart(p2Env(), gP2, gDmubRs, v, gDmubTracePre, sizeof gDmubTracePre);
    OSCompareAndSwap(1, 0, &gP2Busy);
    IOLog("PhxBringup: debug.phxdmub_restart=%d: %s stage %d: %s\n", v, phxdmub::kRsName[rc >= 0 && rc <= phxdmub::kRsHalted ? rc : 1], gDmubRs.stage,
          gDmubRs.why ? gDmubRs.why : "-");
    return rc == phxdmub::kRsOk ? 0 : rc == phxdmub::kRsRefused ? EINVAL : rc == phxdmub::kRsNoAck ? ETIMEDOUT : EIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdmub_restart, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxDmubRestartSysctl, "I",
            "b109 DMCUB firmware restart, one shot (root): 1 power-on chain, 2 any chain, 3 = 2 + 0x0C, 4 = 1 + 0x0C; reads the last rc");
static int phxDmubRestartStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 2048;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len = phxp2::restartStat(gDmubRs, b, kN);
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdmub_restart_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDmubRestartStatSysctl, "A",
            "b109 last debug.phxdmub_restart: result, stage, why, SCRATCH0/1/7 + CNTL before/after, timings, MAIN_BEGIN, guard");
// 'PDMT' (as debug.phxdmub_trace, one decoder reads both), vram lo/hi, size, OUTBOX0_WPTR, SCRATCH0 and
// CNTL before the restart, rc (1 = no copy), then the copy. Root; memory only, no register access.
static int phxDmubTracePreSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    if (proc_suser(req->p)) return EPERM;
    if (!req->oldptr) return SYSCTL_OUT(req, nullptr, 32 + gDmubRs.traceBytes);
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    const uint32_t hdr[8] = {0x544d4450u, static_cast<uint32_t>(gDmubRs.traceVram), static_cast<uint32_t>(gDmubRs.traceVram >> 32), gDmubRs.traceBytes,
                             gDmubRs.traceWptr, gDmubRs.pre[0], gDmubRs.pre[3], gDmubRs.traceBytes ? 0u : 1u};
    int err = SYSCTL_OUT(req, hdr, sizeof hdr);
    if (!err && gDmubRs.traceBytes) err = SYSCTL_OUT(req, gDmubTracePre, gDmubRs.traceBytes);
    OSCompareAndSwap(1, 0, &gP2Busy);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdmub_trace_pre, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDmubTracePreSysctl, "S",
            "b109 the DMCUB trace (CW5) copied by debug.phxdmub_restart before its first write: 'PDMT' header as debug.phxdmub_trace, then the window");

// b109: re-write the sink's SCDC TMDS_Config with the stream running when the
// lit plan has an SCDC step (165) and the sink lost it (phxp2::scdcFix: DC_I2C only, no TX toggle, no DMUB command).
// Root writes 1 (anything else refused); reads the number of re-writes. debug.phxscdc_fix_stat: last result + counters.
static phxp2::ScdcFix gScdcFix;
static int phxScdcFixSysctl SYSCTL_HANDLER_ARGS {
    int v = static_cast<int>(gScdcFix.rewrites);
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 1) return EINVAL;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    const int r = phxp2::scdcFix(p2Env(), gP2, *gP2Plan, gScdcFix);
    OSCompareAndSwap(1, 0, &gP2Busy);
    IOLog("PhxBringup: debug.phxscdc_fix: %s: %s (TMDS_Config 0x%02x -> 0x%02x)\n", phxp2::kFixName[r], gScdcFix.why ? gScdcFix.why : "-",
          gScdcFix.before.tmdsConfig, gScdcFix.after.tmdsConfig);
    return r == phxp2::kFixRewritten || r == phxp2::kFixSame ? 0 : r == phxp2::kFixRefused ? EINVAL : r == phxp2::kFixBusy ? EBUSY : EIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxscdc_fix, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxScdcFixSysctl, "I",
            "b109 1 = re-write the sink's SCDC TMDS_Config when pipe 1 is lit at 165 and the sink lost it (root, DC_I2C only); reads the re-write count");
static int phxScdcFixStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    char b[448];
    int len = phxp2::fixStat(gScdcFix, b, sizeof b);
    if (len < 0) len = 0;
    if (len > static_cast<int>(sizeof b) - 1) len = sizeof b - 1;
    b[len] = 0;
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxscdc_fix_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxScdcFixStatSysctl, "A",
            "b109 last debug.phxscdc_fix: result, why, counters, SCDC TMDS_Config / scrambler / lock before and after");

// b111: does the GOP DMCUB firmware execute a generic DMUB register
// write? Root writes 1 (anything else refused), one shot per boot: one DMUB_CMD__REG_SEQ_READ_MODIFY_WRITE of old ^ 1 to
// the dark OTG1_OTG_H_TOTAL (0x0506A, pinned in PhxDmub.h), read back, restored by a host write (phxp2::dmubRegTest).
// Reads the last result (-1 never, else phxp2::RtRes). Under gP2Busy like the ladder; refused after debug.phxdmub_restart.
// debug.phxdmub_regtest_stat: the result, the command, RPTR before/after, the values, the reply, pipe 0 before/after.
static phxp2::RegTest gDmubRt;
static int phxDmubRegTestSysctl SYSCTL_HANDLER_ARGS {
    int v = gDmubRt.attempts ? gDmubRt.res : -1;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    const int r = phxp2::dmubRegTest(p2Env(), gP2, gDmubRt, v, gDmubRs.done);
    OSCompareAndSwap(1, 0, &gP2Busy);
    IOLog("PhxBringup: debug.phxdmub_regtest=%d: %s: %s (0x0506A old 0x%x new 0x%x restored 0x%x)\n", v, phxp2::kRtName[r >= 0 && r <= phxp2::kRtPipe0 ? r : 0],
          gDmubRt.why ? gDmubRt.why : "-", gDmubRt.oldV, gDmubRt.newV, gDmubRt.restoredV);
    return r == phxp2::kRtWritten || r == phxp2::kRtUnsupported || r == phxp2::kRtUnexpected ? 0 : r == phxp2::kRtRefused ? EINVAL
         : r == phxp2::kRtTimeout ? ETIMEDOUT : EIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdmub_regtest, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxDmubRegTestSysctl, "I",
            "b111 T-C3-0 one DMUB REG_SEQ write of old^1 to the dark OTG1_OTG_H_TOTAL, host-restored, one shot (root, write 1); reads the last result");
static int phxDmubRegTestStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 1536;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len = phxp2::regTestStat(gDmubRt, b, kN);
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdmub_regtest_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDmubRegTestStatSysctl, "A",
            "b111 last debug.phxdmub_regtest: result, why, command, RPTR/WPTR before/after, 0x0506A old/new/restored, DMUB reply, OTG0 before/after");

// b113 (the cold-boot dark Acer, C3): debug.phxc3, the fixed DMUB REG_SEQ programs of
// PhxC3Load.h through the GOP firmware (phxp2::dmubC3). Root writes 1-5 (the 1a1-1a5 sub-tests on the dark OTG1_OTG_H_TOTAL,
// in order, each once) or 9 (PHY D's prologue + SRAM load, 1475 commands, one shot, gates G1-G4); anything else refused.
// Reads the last result (-1 never, else phxp2::C3Res). Under gP2Busy like the ladder; refused after debug.phxdmub_restart.
// debug.phxpoweron: DC_GPIO_GENERIC_A bit 8 latched once at kext start (phxC3LatchPowerOn from start()), -1 not latched.
// Telemetry only since b114: it read 0 at kext start on power-on chains too (seen on the box). debug.phxc3_stat: the last run.
static phxp2::C3 gC3;
static void phxC3LatchPowerOn(uint32_t gpioA) {
    if (gC3.latched || gpioA == 0xffffffffu) return;
    gC3.gpioStart = gpioA;
    gC3.powerOn = (gpioA & phxp2::kGpioPowerOn) ? 1 : 0;
    gC3.latched = 1;
}
static int phxC3Sysctl SYSCTL_HANDLER_ARGS {
    int v = gC3.attempts ? gC3.res : -1;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    const int r = phxp2::dmubC3(p2Env(), gP2, gC3, v, gDmubRs.done);
    OSCompareAndSwap(1, 0, &gP2Busy);
    IOLog("PhxBringup: debug.phxc3=%d: %s step %d: %s (loaded %u/%d)\n", v, phxp2::kC3Name[r >= 0 && r <= phxp2::kC3Shape ? r : 0], gC3.step + 1,
          r == phxp2::kC3Refused ? (gC3.lastRefusal ? gC3.lastRefusal : "-") : (gC3.why ? gC3.why : "-"), gC3.nOk, gC3.total);
    return r == phxp2::kC3Pass ? 0 : r == phxp2::kC3Refused ? EINVAL : r == phxp2::kC3Timeout ? ETIMEDOUT : EIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxc3, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxC3Sysctl, "I",
            "b113 B-22 T-C3-1 (root): 1-5 the T-C3-1a REG_SEQ sub-tests in order, 9 the PHY D prologue + SRAM load (power-on chain only); reads the last result");
static int phxC3StatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 3072;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len = phxp2::c3Stat(gC3, b, kN);
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxc3_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxC3StatSysctl, "A",
            "b113 last debug.phxc3: result, mode, step, why, gates (G1 latched / live GPIO, G2 trace, G4 CRC), commands loaded, L9/L10 us, trace wptr per phase, 1a values, pipe 0");
static int phxPowerOnSysctl SYSCTL_HANDLER_ARGS {
    int v = gC3.latched ? gC3.powerOn : -1;
    if (req->newptr) return EPERM;
    return sysctl_handle_int(oidp, &v, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, phxpoweron, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxPowerOnSysctl, "I",
            "b113 DC_GPIO_GENERIC_A bit 8 latched at kext start: 1 power-on chain, 0 warm, -1 not latched (debug.phxc3=9 gate G1)");
