// SPDX-License-Identifier: MIT
// phx_gamma.cpp: b98 head-1 gamma on MPCC_OGAM1:
// the platform function "phxPipe2SetGamma" PhxAMDFB head 1 calls from
// setGammaTable (PhxGamma.h ABI, property phx-pipe2-gamma-fn), and the sysctls
//   debug.phxgamma       kill switch, default 0: the table is kept and counted, nothing is written. 1 (root) = writes on,
//                        the last table is applied at once; 1 -> 0 = one CONTROL = 0 write (bypass), the instant rollback.
//   debug.phxgamma_test  root, independent of the kill switch: 0 bypass, 1 identity table (written, not bypassed),
//                        2 half (a 50 % fade), 3 warm (R 1.0, G 0.8, B 0.6, Night-Shift-like); reads the last result.
//   debug.phxgamma_stat  counters, the last call, and (root) OGAM1 CONTROL / MEM_PWR_CTRL / LUT_CONTROL / UPDATE_LOCK_SEL.
// Runs under gP2Busy (never during a ladder level or a mode switch: Busy), only while pipe 1 is lit by the ladder (level 4,
// not failed / halted / switching). Writes only MPCC_OGAM1 and MPCC1_MPCC_MEM_PWR_CTRL, each also through
// phxp2::writeAllowed; the pipe-0 fingerprint is compared before (refuse) and after (halted, as every other pipe-1 write).
// Never from interrupt context: PhxAMDFB calls it on the WindowServer thread or its retry thread call.
// Part of the PhxBringup.cpp translation unit (included there after phx_pipe2.cpp); not compiled on its own.

#include "PhxGamma.h"

static int gGammaOn;                       // debug.phxgamma
static int gGammaTest = -1;                // debug.phxgamma_test: the last test written (-1 none)
static phxgamma::Stats gGammaSt;
static uint32_t gGammaCalls, gGammaOff, gGammaIdentity, gGammaNotLit, gGammaEngineBusy, gGammaP0Sticky, gGammaUs, gGammaMaxUs;
static char gGammaWhy[64];
// the last table PhxAMDFB sent (applied when debug.phxgamma goes 1), the PWL and the register image: static, all under
// gP2Busy (the image is ~8 KB, too large for a kernel stack)
static uint16_t gGammaLast[3][phxgamma::kN];
static uint32_t gGammaLastMode;
static int gGammaLastValid;
static phxgamma::Pwl gGammaPwl;
static phxgamma::Image gGammaImg;
static bool gGammaAcceptSticky;            // the pre-check accepted a stale ODM0 sticky: the post-check accepts the same

// b97's case: the b15 watchdog's first pipe-0 underflow hit leaves ODM0's OCCURRED sticky set for the rest of the boot.
// Gamma accepts that sticky (and only it) when stutter is forced off and OCCURRED_CURRENT is clear, decided before the
// writes and kept for the post-check; the baseline itself (odm0Base) is not changed here.
static bool gamP0Same(void *, int after, const char **why) {
    const phxp2::Env e = p2Env();
    if (!after) {
        gGammaAcceptSticky = false;
        if (phxp2::fpSame(e, gP2, why)) return true;
        const char *w = nullptr;
        const uint32_t o = phxp2::rd(e, phxp2::kOdm0Global);
        if ((o & phxp2::kOdmSticky & ~gP2.odm0Base) && !(o & phxp2::kOdmCurrent) &&
            (phxp2::rd(e, phxp2::kArbDramStateCntl) & 3) == 2 && phxp2::fpSame(e, gP2, &w, phxp2::kOdmSticky)) {
            gGammaAcceptSticky = true; gGammaP0Sticky++;
            return true;
        }
        return false;
    }
    return phxp2::fpSame(e, gP2, why, gGammaAcceptSticky ? phxp2::kOdmSticky : 0);
}
static uint32_t gamRd(void *c, uint32_t r) { return static_cast<PhxBringup *>(c)->p2Rd(r); }
static void gamWr(void *c, uint32_t r, uint32_t v) {
    if (!phxgamma::writable(r) || !phxp2::writeAllowed(r)) return;   // PhxGamma.h refuses these first; never reached
    static_cast<PhxBringup *>(c)->p2Wr(r, v);
}

// One update under gP2Busy. mode: phxgamma::Mode; t: the table (ignored for bypass).
static int gamRun(uint32_t mode, const uint16_t t[3][phxgamma::kN], const char **why) {
    if (gP2.level != 4 || gP2.failed || gP2.halted || gP2.switching || !gP2.lit || !gP2.fpValid || gP2.dmub.dead) {
        gGammaNotLit++; gGammaSt.lastRes = phxgamma::kNotLit;
        *why = gP2.halted ? "pipe-1 engine halted" : "pipe 1 not lit by the ladder";
        return phxgamma::kNotLit;
    }
    bool bypass = mode == phxgamma::kModeBypass;
    if (mode == phxgamma::kModeAuto && phxgamma::isIdentity(t)) { bypass = true; gGammaIdentity++; }
    if (!bypass) phxgamma::build(t, gGammaPwl);
    const phxgamma::Io io = {gPhx, gamRd, gamWr, p2IoDelay, gamP0Same};
    const uint64_t t0 = mach_absolute_time();
    const int r = phxgamma::apply(io, gGammaSt, bypass ? nullptr : &gGammaPwl, gGammaImg, why);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    gGammaUs = static_cast<uint32_t>(ns / 1000);
    if (gGammaUs > gGammaMaxUs) gGammaMaxUs = gGammaUs;
    if (r == phxgamma::kHalted) gP2.halted = 1;   // pipe 0 moved after our writes: nothing more on pipe 1 until reboot
    static uint32_t logged;   // a fade is ~60 updates a second: the first 8, then refusals and failures only
    if (r == phxgamma::kRefused || r == phxgamma::kFailed || r == phxgamma::kHalted || ((r == phxgamma::kApplied || r == phxgamma::kBypass) && logged++ < 8))
        IOLog("PhxBringup: gamma %s bank %u CONTROL 0x%x -> 0x%x, %u writes, %u us: %s\n", phxgamma::kResName[r], gGammaSt.lastBank,
              gGammaSt.lastCtlBefore, gGammaSt.lastCtlAfter, gGammaSt.lastWrites, gGammaUs, *why ? *why : "");
    return r;
}
static void gamKeep(int r, const char *why) { gGammaSt.lastRes = r; strlcpy(gGammaWhy, why ? why : "", sizeof gGammaWhy); }

static IOReturn phxPipe2SetGammaFn(phx_pipe2_gamma *g) {
    if (!g || g->version != PHX_PIPE2_GAMMA_VERSION || g->mode > phxgamma::kModeTable) return kIOReturnBadArgument;
    OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&gGammaCalls));
    g->bank = 0; g->control = 0;
    if (!p2Up()) { g->result = phxgamma::kNotLit; strlcpy(g->why, "PhxBringup not ready", sizeof g->why); return kIOReturnSuccess; }
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) {
        gGammaEngineBusy++;
        g->result = phxgamma::kBusy; strlcpy(g->why, "pipe-1 engine busy", sizeof g->why);
        return kIOReturnBusy;
    }
    memcpy(gGammaLast, g->table, sizeof gGammaLast);
    gGammaLastMode = g->mode; gGammaLastValid = 1;
    const char *w = nullptr;
    int r;
    if (!gGammaOn) { gGammaOff++; r = phxgamma::kOff; w = "debug.phxgamma=0: kept, not written"; gGammaSt.lastRes = r; }
    else r = gamRun(g->mode, gGammaLast, &w);
    gamKeep(r, w);
    g->result = r; g->bank = gGammaSt.lastBank; g->control = gGammaSt.lastCtlAfter;
    strlcpy(g->why, w ? w : "", sizeof g->why);
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r == phxgamma::kBusy ? kIOReturnBusy : kIOReturnSuccess;
}

static int phxGammaSysctl SYSCTL_HANDLER_ARGS {
    int v = gGammaOn;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 0 && v != 1) return EINVAL;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    const int was = gGammaOn;
    gGammaOn = v;
    int r = phxgamma::kSame;
    const char *w = "unchanged";
    if (p2Up() && v != was) {
        if (v) r = gGammaLastValid ? gamRun(gGammaLastMode, gGammaLast, &w) : phxgamma::kSame;
        else r = gamRun(phxgamma::kModeBypass, gGammaLast, &w);
        if (v && !gGammaLastValid) w = "on; no table yet";
        gamKeep(r, w);
    }
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r == phxgamma::kBusy ? EBUSY : (r == phxgamma::kFailed || r == phxgamma::kHalted) ? EIO : 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxgamma, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxGammaSysctl, "I",
            "b98 head-1 gamma writes (root): 0 off (default: kept and counted only; 1 -> 0 writes bypass), 1 on (applies the last table)");

static int phxGammaTestSysctl SYSCTL_HANDLER_ARGS {
    int v = gGammaTest;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (proc_suser(req->p)) return EPERM;
    if (v < 0 || v > 3) return EINVAL;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    static uint16_t t[3][phxgamma::kN];   // under gP2Busy
    static const uint32_t scale[4][3] = {{0, 0, 0}, {256, 256, 256}, {128, 128, 128}, {256, 205, 154}};   // / 256
    for (unsigned c = 0; c < 3; c++)
        for (unsigned i = 0; i < phxgamma::kN; i++) t[c][i] = static_cast<uint16_t>(i * 257u * scale[v][c] / 256u);
    gGammaTest = v;
    const char *w = nullptr;
    const int r = gamRun(v ? phxgamma::kModeTable : phxgamma::kModeBypass, t, &w);
    gamKeep(r, w);
    OSCompareAndSwap(1, 0, &gP2Busy);
    return r == phxgamma::kApplied || r == phxgamma::kBypass || r == phxgamma::kSame ? 0 : r == phxgamma::kBusy ? EBUSY : EIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxgamma_test, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxGammaTestSysctl, "I",
            "b98 head-1 gamma test tables (root, ignores debug.phxgamma): 0 bypass, 1 identity table, 2 half, 3 warm");

static int phxGammaStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    char b[640];
    const phxgamma::Stats &s = gGammaSt;
    const int lr = s.lastRes >= 0 && s.lastRes <= phxgamma::kHalted ? s.lastRes : phxgamma::kOff;
    int len = snprintf(b, sizeof b,
                       "on=%d calls=%u off=%u applied=%u bypass=%u same=%u identity=%u busy=%u engine_busy=%u not_lit=%u refused=%u "
                       "failed=%u halted=%u pwr_fail=%u rb_fail=%u rb_reg=0x%x p0sticky=%u writes=%u pwr_wait_us=%u max=%u us=%u max_us=%u "
                       "test=%d last=%s bank=%u ctl=0x%x->0x%x last_writes=%u why=%s",
                       gGammaOn, gGammaCalls, gGammaOff, s.applied, s.bypass, s.same, gGammaIdentity, s.busy, gGammaEngineBusy, gGammaNotLit,
                       s.refused, s.failed, s.halted, s.pwrFail, s.rbFail, s.lastRbReg, gGammaP0Sticky, s.writes, s.pwrWaitUs, s.pwrWaitMax,
                       gGammaUs, gGammaMaxUs, gGammaTest, gGammaCalls || gGammaTest >= 0 ? phxgamma::kResName[lr] : "none", s.lastBank,
                       s.lastCtlBefore, s.lastCtlAfter, s.lastWrites, gGammaWhy);
    if (len < 0) len = 0;
    if (len > static_cast<int>(sizeof b) - 1) len = sizeof b - 1;
    if (p2Up() && !proc_suser(req->p) && len < static_cast<int>(sizeof b) - 1) {   // the registers: root only (reads)
        const int k = snprintf(b + len, sizeof b - static_cast<size_t>(len), " | live CONTROL=0x%x MEM_PWR_CTRL=0x%x LUT_CONTROL=0x%x UPDATE_LOCK_SEL=0x%x",
                               gPhx->p2Rd(phxgamma::kControl), gPhx->p2Rd(phxgamma::kMemPwr), gPhx->p2Rd(phxgamma::kLutControl),
                               gPhx->p2Rd(phxgamma::kLockSel));
        if (k > 0) len += k;
        if (len > static_cast<int>(sizeof b) - 1) len = sizeof b - 1;
    }
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxgamma_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxGammaStatSysctl, "A",
            "b98 head-1 gamma: kill switch, counters, last result, live OGAM1 registers (root)");
