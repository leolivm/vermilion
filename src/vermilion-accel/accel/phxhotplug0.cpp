// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxhotplug0.cpp (PhxAccel hp0): hot-plug for head 0, the AOC on DP.
// Part of phxfb.cpp's translation unit (included after phxFbHeadInfo); not compiled on its own.
//   debug.phxhotplug0 (root): 0 off (default), 1 observe (250 ms read-only poll of HPD2/3/4 + DIG1/0/4 / DP1/0/4 stream state
//     through head 0's BAR5 mapping, debounce, counters, event ring), 2 act: head 0 offline on a 6 s unplug of every head-0 DP
//     connector, online again once a connector has had HPD 1 s + a stream from OTG0 for 500 ms and the EDID matches
//     (phxhp0.h). A failed action latches the level to 1 for the boot. Boot-arg -phxhotplugoff refuses every write (the
//     same kill switch as PhxBringup's debug.phxhotplug).
//   debug.phxhotplug0_stat: the policy state, the live sample, the last 16 events (our IOLog never reaches the unified log).
//   debug.phxhotplug0_opts (root): bit 0 = compare the EDID before online (default 1).
//   debug.phxhead0_online (root): reads head 0's state; 0 = offline now / 1 = online now (box-tested, no cable), the
//     same guards (offer gate open, head 1 online, no head-0 switch in flight).
//   debug.phxfb0_online: head 0 reports a monitor (1) or not (0).
// No register is written anywhere here; no IOFB gate is taken (the connect handler only bumps IOFramebuffer's counter).

static phxhp0::Hp0 gHp0;           // under gHp0Lock
static IOLock *gHp0Lock;
static thread_call_t gHp0Call;
static volatile int gHp0Stopping = 0;
static volatile SInt32 gHp0InFlight = 0;   // hp0Fire bodies running now (stop drains it: thread_call_cancel_wait is
                                           // com.apple.kpi.private only)
static constexpr uint32_t kHp0PollMs = 250;
static int gHp0Level = 0;          // the sysctl's copy of gHp0.level (read without the lock)
static int gHp0Opts = (int)phxhp0::kOptsDefault;

static uint64_t hp0NowMs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ull;
}
static uint32_t hp0Rd(void *c, uint32_t r) { return static_cast<PhxAMDFB *>(c)->hp0Read(r); }

// One live EDID read (PhxBringup b79+ "phxEdidRead", the provider loadEdid uses), validated like loadEdid's (PhxEdid.h).
int PhxAMDFB::hp0EdidLive(uint32_t src, uint8_t out[256], uint32_t *len)
{
    *len = 0;
    if (!fPCI) return -1;
    int res = -2;
    if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject()) {
            IOService *s = OSDynamicCast(IOService, o);
            if (!s || s->getProperty("phx-edid-fn") != kOSBooleanTrue) continue;
            int32_t rc = 0;
            uint32_t n = 0, sr = src;
            const IOReturn r = s->callPlatformFunction("phxEdidRead", false, out, &n, &rc, &sr);
            res = r != kIOReturnSuccess ? -3 : rc ? (int)rc : 0;
            if (!res) { if (phxedid::check(out, n) != phxedid::kLive) res = -4; else *len = n; }
            break;
        }
        it->release();
    }
    return res;
}

static int hp0Head0Online(void *) { return gPhxFB ? gHead0Online : -1; }
static int hp0Head1Online(void *)
{
    IOLock *l = headsLock();
    if (l) IOLockLock(l);
    const bool up = gHeads[1] != nullptr;
    if (l) IOLockUnlock(l);
    return up && gHead1Online ? 1 : 0;
}
static bool hp0OfferOpen(void *) { return gHead0OfferOpen != 0; }
static bool hp0Switching(void *) { return gHead0SwBusy != 0; }
static int hp0SetHead0(void *, bool on) { return phxFbSetHeadOnline(0, on); }
static int hp0ReadEdid(void *c, uint32_t src, uint8_t out[256], uint32_t *len) { return static_cast<PhxAMDFB *>(c)->hp0EdidLive(src, out, len); }
static uint32_t hp0ServedEdid(void *c, uint8_t out[256]) { return static_cast<PhxAMDFB *>(c)->hp0Served(out); }
static phxhp0::Glue hp0Glue(PhxAMDFB *fb)
{
    return phxhp0::Glue{fb, hp0Head0Online, hp0Head1Online, hp0OfferOpen, hp0Switching, hp0SetHead0, hp0ReadEdid, hp0ServedEdid};
}

static void hp0Fire(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gHp0InFlight);
    if (!gHp0Lock || gHp0Stopping) { OSDecrementAtomic(&gHp0InFlight); return; }
    IOLockLock(gHp0Lock);
    bool again = false;
    PhxAMDFB *fb = gPhxFB;
    if (gHp0.level != phxhp0::kLvlOff && fb && !gHp0Stopping) {
        phxhp0::Sample s{};
        const phxhp0::RoIo io{fb, hp0Rd};
        phxhp0::sample(io, &s);
        gHp0.opts = (uint32_t)gHp0Opts;
        const int ev = phxhp0::poll(gHp0, s, hp0NowMs(), hp0Glue(fb));
        if (ev == phxhp0::kEvOffline || ev == phxhp0::kEvOnline || ev == phxhp0::kEvFail) PFBLOG("hp0: %s", gHp0.why);
        gHp0Level = gHp0.level;
        again = gHp0.level != phxhp0::kLvlOff;
    }
    IOLockUnlock(gHp0Lock);
    if (again && !gHp0Stopping) {   // a re-arm that races the stop flag is cancelled by phxHp0Remove after the drain
        uint64_t d; clock_interval_to_deadline(kHp0PollMs, kMillisecondScale, &d);
        thread_call_enter_delayed(gHp0Call, d);
    }
    OSDecrementAtomic(&gHp0InFlight);
}

static int phxHotplug0Sysctl SYSCTL_HANDLER_ARGS
{
    int v = gHp0Level;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;
    char a[8];
    if (PE_parse_boot_argn("-phxhotplugoff", a, sizeof a)) return EPERM;
    if (!gPhxFB || !gHp0Lock || !gHp0Call) return ENXIO;
    IOLockLock(gHp0Lock);
    const int s = phxhp0::setLevel(gHp0, v);
    gHp0Level = gHp0.level;
    IOLockUnlock(gHp0Lock);
    if (s == phxhp0::kSetBad) return EINVAL;
    if (s == phxhp0::kSetLatched) return EBUSY;
    PFBLOG("hp0: debug.phxhotplug0=%d", v);
    if (v != phxhp0::kLvlOff && !gHp0Stopping) { uint64_t d; clock_interval_to_deadline(kHp0PollMs, kMillisecondScale, &d); thread_call_enter_delayed(gHp0Call, d); }
    else thread_call_cancel(gHp0Call);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxhotplug0, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxHotplug0Sysctl, "I",
            "hp0 head-0 hot-plug (root): 0 off (default), 1 observe (250 ms read-only poll of HPD2/3/4 + the DP stream from OTG0), "
            "2 head 0 offline on a 6 s AOC unplug / online after replug + stream up + EDID match; a failure latches 1");
SYSCTL_INT(_debug, OID_AUTO, phxhotplug0_opts, CTLFLAG_RW | CTLFLAG_LOCKED, &gHp0Opts, 0,
           "hp0: bit 0 = compare the sink's EDID block 0 with head 0's before online (default 1)");

static int phxHotplug0StatSysctl SYSCTL_HANDLER_ARGS
{
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 4096;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len;
    if (!gHp0Lock) len = snprintf(b, kN, "hp0 v1 not installed\n");
    else {
        IOLockLock(gHp0Lock);
        phxhp0::Sample s{};
        PhxAMDFB *fb = gPhxFB;
        if (fb) { const phxhp0::RoIo io{fb, hp0Rd}; phxhp0::sample(io, &s); }
        len = phxhp0::format(gHp0, fb ? &s : nullptr, hp0NowMs(), fb ? gHead0Online : -1, b, kN);
        IOLockUnlock(gHp0Lock);
    }
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxhotplug0_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, phxHotplug0StatSysctl, "A",
            "hp0: level, latch, counters, why, the live DP B/A/E sample (reads only), the last 16 events");

static int phxHead0OnlineSysctl SYSCTL_HANDLER_ARGS
{
    int v = gHead0Online;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;
    char a[8];
    if (PE_parse_boot_argn("-phxhotplugoff", a, sizeof a)) return EPERM;
    if (v != 0 && v != 1) return EINVAL;
    if (!gPhxFB || !gHp0Lock) return ENXIO;
    IOLockLock(gHp0Lock);
    const int r = phxhp0::manual(gHp0, hp0NowMs(), v == 1, hp0Glue(gPhxFB));
    IOLockUnlock(gHp0Lock);
    PFBLOG("hp0: debug.phxhead0_online=%d -> errno %d (head 0 now %s)", v, r, gHead0Online ? "online" : "offline");
    return r;
}
SYSCTL_PROC(_debug, OID_AUTO, phxhead0_online, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxHead0OnlineSysctl, "I",
            "hp0 (root): head 0 reports a monitor; write 0 = offline now (refused while the offer gate is closed, head 1 is not "
            "online or a head-0 switch runs), 1 = online now");
SYSCTL_INT(_debug, OID_AUTO, phxfb0_online, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0Online, 0, "head 0 reports a connected monitor (hp0)");

static struct sysctl_oid *const kHp0Oids[] = {&sysctl__debug_phxhotplug0, &sysctl__debug_phxhotplug0_opts,
                                              &sysctl__debug_phxhotplug0_stat, &sysctl__debug_phxhead0_online,
                                              &sysctl__debug_phxfb0_online};
static bool gHp0Sysctl;

// head 0 start: the lock, the thread call (not armed) and the sysctls. The policy state restarts with head 0.
static void phxHp0Install()
{
    if (!gHp0Lock) gHp0Lock = IOLockAlloc();
    if (!gHp0Call) gHp0Call = thread_call_allocate(hp0Fire, nullptr);
    gHp0Stopping = 0;
    if (gHp0Lock) { IOLockLock(gHp0Lock); gHp0 = phxhp0::Hp0{}; gHp0Level = 0; IOLockUnlock(gHp0Lock); }
    gHead0Online = 1;
    if (!gHp0Sysctl) { for (auto *o : kHp0Oids) sysctl_register_oid(o); gHp0Sysctl = true; }
}
// head 0 stop: no poll after this (it reads head 0's BAR mapping), and none of our code runs on the call afterwards (unload).
// KPI only (com.apple.kpi.mach thread_call_cancel / thread_call_free): stop flag first (no new body acts or re-arms), the
// sysctls go (no new level write arms it), cancel, drain the bodies already running, cancel again (a re-arm made before the
// flag was seen), then free as head 0's VBL timer does (thread_call_free refuses a queued call), then drain once more (a body
// that started between the drain and the free holds the call's own reference until it returns).
static void hp0Drain() { for (unsigned i = 0; gHp0InFlight && i < 2000; i++) IOSleep(1); }
static void phxHp0Remove()
{
    gHp0Stopping = 1;
    if (gHp0Sysctl) { for (auto *o : kHp0Oids) sysctl_unregister_oid(o); gHp0Sysctl = false; }
    if (gHp0Lock) { IOLockLock(gHp0Lock); gHp0.level = phxhp0::kLvlOff; gHp0Level = 0; IOLockUnlock(gHp0Lock); }
    if (gHp0Call) {
        thread_call_cancel(gHp0Call);
        hp0Drain();
        thread_call_cancel(gHp0Call);
        while (!thread_call_free(gHp0Call)) { thread_call_cancel(gHp0Call); IOSleep(5); }
        gHp0Call = nullptr;
        hp0Drain();
    }
}
