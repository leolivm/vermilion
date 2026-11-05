// SPDX-License-Identifier: MIT
// phx_hpd.cpp: kext b102, hot-plug steps K1 + A2.
//   K1: debug.phxhpd (string, root) and debug.phxhpd_sense (int) report the live HPD state of the five HPD blocks. Reads
//       only (PhxHpd.h has no write path); each read of either sysctl = one fresh sample.
//   A2: debug.phxhotplug (root; 0 off = default, 1 observe, 2 act on head 1) runs a 250 ms poll (thread call) of the same
//       sample through phxhpd::poll: debounce (HDMI 1 s high = plug, 3 s low = unplug; DP 1 s / 6 s), counters and an
//       event ring (debug.phxhotplug_stat; our IOLog never reaches the unified log), and at level 2 head 1 offline / online
//       through PhxAccel's "phxHead1Hotplug" (PhxSwitchABI.h phx_head1_hotplug: the debug.phxhead2=0 / =1 path) under the
//       display lock gP2Busy (try only). No register is written here: pipe 1 is neither undone nor relit, head 0 is never
//       acted on. Boot-arg -phxhotplugoff refuses every write of debug.phxhotplug.
// Part of the PhxBringup.cpp translation unit, included after phx_pipe0.cpp (it uses gP2 / gP2Busy / p2Up of phx_pipe2.cpp);
// not compiled on its own.

static uint32_t phxHpdRd(void *c, uint32_t r) { return static_cast<const PhxBringup *>(c)->p2Rd(r); }

static bool phxHpdSample(phxhpd::Sample *s) {
    if (!gPhx) return false;
    const phxhpd::RoIo io{gPhx, phxHpdRd};
    phxhpd::sample(io, s);
    return true;
}

static int phxHpdSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    if (proc_suser(req->p)) return EPERM;   // register values: root only, as debug.phxpipe2_stat's register section
    static constexpr int kN = 4096;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len;
    phxhpd::Sample s{};
    if (!phxHpdSample(&s)) len = snprintf(b, kN, "no PhxBringup\n");
    else {
        unsigned nt = 0;
        const phxddc::Connector *t = phxddc::boardConnectors(&nt);
        len = phxhpd::format(s, t, nt, b, kN);
        // context for the plug tests: which pipes run and what the pipe-1 ladder / switch is doing
        if (len >= 0 && len < kN)
            len += snprintf(b + len, static_cast<size_t>(kN - len),
                            "ctx otg0_ctl=0x%08x otg1_ctl=0x%08x otg0_frame=0x%06x dig1_be_en=0x%08x dig3_be_en=0x%08x p2_level=%d p2_switching=%d p2_halted=%d dmub_dead=%d\n",
                            gPhx->p2Rd(0x05001), gPhx->p2Rd(0x05081), gPhx->p2Rd(0x0500C) & 0xffffff, gPhx->p2Rd(0x05672), gPhx->p2Rd(0x05872),
                            gP2.level, gP2.switching, gP2.halted, gP2.dmub.dead);
    }
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxhpd, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxHpdSysctl, "A",
            "hot-plug step 1 (root, reads only): HPD0-4 status/control/filter, DC_GPIO_HPD_*, HPD_INTERRUPT_DEST, board connector per HPD");

static int phxHpdSenseSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    phxhpd::Sample s{};
    int v = phxHpdSample(&s) ? static_cast<int>(phxhpd::packed(s)) : -1;
    return sysctl_handle_int(oidp, &v, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, phxhpd_sense, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxHpdSenseSysctl, "I",
            "hot-plug step 1 (reads only): bits 4:0 HPD0-4 SENSE_DELAYED, 12:8 raw SENSE, 20:16 GPIO pins, bit 31 bad read; -1 no kext");

// ---- A2: the poll and its glue ---------------------------------------------------------------------------------------
static phxhpd::Hot gHot;          // under gHotLock
static IOLock *gHotLock;
static thread_call_t gHotCall;
static IOService *gHotAccel;      // PhxAccel's accelerator (class NMXAccel): never unloads once matched (as selector 30's)
static int gHotInit;
static constexpr uint32_t kHotPollMs = 250;

static uint64_t hotNowMs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ull;
}
static IOService *hotAccel() {
    if (gHotAccel) return gHotAccel;
    OSDictionary *m = IOService::serviceMatching("NMXAccel");
    IOService *s = m ? IOService::copyMatchingService(m) : nullptr;
    OSSafeReleaseNULL(m);
    if (s && !OSCompareAndSwapPtr(nullptr, s, &gHotAccel)) s->release();
    return gHotAccel;
}
// One phxHead1Hotplug call. kH1Unknown = no PhxAccel, or one without the function (older than hpd1).
static int hotHead1Call(uint32_t op, uint8_t *edid, uint32_t *edidLen) {
    IOService *a = hotAccel();
    if (!a) return phxhpd::kH1Unknown;
    static phx_head1_hotplug h;   // every caller holds gHotLock (the poll, the level write)
    bzero(&h, sizeof h);
    h.version = PHX_HEAD1_HOTPLUG_VERSION; h.op = op; h.state = phxhpd::kH1Unknown;
    if (a->callPlatformFunction("phxHead1Hotplug", false, &h, nullptr, nullptr, nullptr) != kIOReturnSuccess) return phxhpd::kH1Unknown;
    if (edid && edidLen) {
        *edidLen = h.edid_len > sizeof h.edid ? static_cast<uint32_t>(sizeof h.edid) : h.edid_len;
        memcpy(edid, h.edid, *edidLen);
    }
    return h.state;
}
static int hotHead1(void *, uint8_t edid[256], uint32_t *edidLen) { return hotHead1Call(PHX_H1HP_QUERY, edid, edidLen); }
static int hotSetHead1(void *, bool on) { return hotHead1Call(on ? PHX_H1HP_ONLINE : PHX_H1HP_OFFLINE, nullptr, nullptr); }
static bool hotTryLock(void *) { return OSCompareAndSwap(0, 1, &gP2Busy); }
static void hotUnlock(void *) { OSCompareAndSwap(1, 0, &gP2Busy); }
static bool hotBusy(void *) { return gP2Busy != 0 || gP2.switching; }
// Under gP2Busy: pipe 1 lit by the ladder (the gamma gate, phx_gamma.cpp) and OTG1 running and counting frames.
static bool hotPipe1Lit(void *, const char **why) {
    if (!p2Up()) { *why = "PhxBringup not ready"; return false; }
    if (gP2.dmub.dead) { *why = "DMUB dead"; return false; }
    if (gP2.halted || gP2.failed) { *why = gP2.halted ? "pipe-1 engine halted" : "pipe-1 engine failed"; return false; }
    if (gP2.switching) { *why = "mode switch in progress"; return false; }
    if (gP2.level != 4 || !gP2.lit) { *why = "pipe 1 not at the plane level"; return false; }
    if (!(gPhx->p2Rd(phxp2::kOtg1Control) & 1)) { *why = "OTG1 off"; return false; }
    const uint32_t f0 = gPhx->p2Rd(phxp2::kOtg1Frame) & 0xffffff;
    IOSleep(40);   // >= 2 frames at 60 Hz
    if ((gPhx->p2Rd(phxp2::kOtg1Frame) & 0xffffff) == f0) { *why = "OTG1 frame counter stopped"; return false; }
    return true;
}
static int hotReadEdid(void *, uint32_t src, uint8_t out[256], uint32_t *len) {
    if (!gPhx) return -100;
    int32_t rc = phxddc::kErrArg;
    const IOReturn r = gPhx->edidFetch(out, len, &rc, src);
    return r != kIOReturnSuccess ? -100 : rc;
}
static const phxhpd::Glue kHotGlue = {nullptr, hotHead1, hotSetHead1, hotTryLock, hotUnlock, hotBusy, hotPipe1Lit, hotReadEdid};

static bool hotSetup() {
    if (gHotInit) return true;
    IOLock *l = IOLockAlloc();
    if (l && !OSCompareAndSwapPtr(nullptr, l, &gHotLock)) IOLockFree(l);
    if (!gHotLock) return false;
    IOLockLock(gHotLock);
    if (!gHotInit) {
        gHotCall = thread_call_allocate([](thread_call_param_t, thread_call_param_t) {
            IOLockLock(gHotLock);
            bool again = false;
            if (gHot.level != phxhpd::kLvlOff && gPhx) {
                phxhpd::Sample s{};
                phxHpdSample(&s);
                unsigned nt = 0;
                const phxddc::Connector *t = phxddc::boardConnectors(&nt);
                phxhpd::poll(gHot, s, hotNowMs(), t, nt, kHotGlue);
                again = gHot.level != phxhpd::kLvlOff;
            }
            IOLockUnlock(gHotLock);
            if (again) {
                uint64_t d; clock_interval_to_deadline(kHotPollMs, kMillisecondScale, &d);
                thread_call_enter_delayed(gHotCall, d);
            }
        }, nullptr);
        if (gHotCall) {
            unsigned nt = 0;
            const phxddc::Connector *t = phxddc::boardConnectors(&nt);
            phxhpd::init(gHot, t, nt);
            gHotInit = 1;
        }
    }
    IOLockUnlock(gHotLock);
    return gHotInit != 0;
}

static int phxHotplugSysctl SYSCTL_HANDLER_ARGS {
    int v = gHot.level;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;
    char a[8];
    if (PE_parse_boot_argn("-phxhotplugoff", a, sizeof a)) return EPERM;
    if (v < phxhpd::kLvlOff || v > phxhpd::kLvlMax) return EINVAL;   // 3 (head 0) is a later step
    if (!gPhx || !hotSetup()) return ENXIO;
    int r = 0;
    if (v == phxhpd::kLvlHead1) {
        IOLockLock(gHotLock);   // hotHead1Call's buffer: never at the same time as a poll
        const int h1 = hotHead1Call(PHX_H1HP_QUERY, nullptr, nullptr);
        IOLockUnlock(gHotLock);
        if (h1 == phxhpd::kH1Unknown) { IOLog("PhxBringup: debug.phxhotplug=2 refused: PhxAccel has no phxHead1Hotplug (needs PhxAccel hpd1+)\n"); r = ENODEV; }
    }
    if (!r) {
        IOLockLock(gHotLock);
        const int s = phxhpd::setLevel(gHot, v);
        IOLockUnlock(gHotLock);
        r = s == phxhpd::kSetOk ? 0 : s == phxhpd::kSetLatched ? EBUSY : s == phxhpd::kSetNoHead1 ? ENODEV : EINVAL;
    }
    if (!r) {
        if (v != phxhpd::kLvlOff) { uint64_t d; clock_interval_to_deadline(kHotPollMs, kMillisecondScale, &d); thread_call_enter_delayed(gHotCall, d); }
        else thread_call_cancel(gHotCall);
    }
    return r;
}
SYSCTL_PROC(_debug, OID_AUTO, phxhotplug, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxHotplugSysctl, "I",
            "b102 hot-plug A2 (root): 0 off (default), 1 observe (250 ms poll, debounce, count), 2 head 1 offline/online on the "
            "Acer's unplug/plug (debug.phxhead2 path; pipe 1 untouched); a failed action latches 1 (debug.phxhotplug_stat)");

static int phxHotplugStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 4096;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    unsigned nt = 0;
    const phxddc::Connector *t = phxddc::boardConnectors(&nt);
    int len;
    if (!gHotInit) len = snprintf(b, kN, "hotplug v1 level=0 never armed\n");
    else {
        IOLockLock(gHotLock);
        len = phxhpd::formatHot(gHot, hotNowMs(), t, nt, b, kN);
        IOLockUnlock(gHotLock);
    }
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxhotplug_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxHotplugStatSysctl, "A",
            "b102 hot-plug A2: level, latch, counters, last reason, per-connector debounce state, the last 16 settled edges / actions");

// PhxBringup::stop: no poll after the provider goes.
static void phxHotplugStop() {
    if (!gHotInit) return;
    IOLockLock(gHotLock);
    gHot.level = phxhpd::kLvlOff;
    IOLockUnlock(gHotLock);
    thread_call_cancel_wait(gHotCall);
}
