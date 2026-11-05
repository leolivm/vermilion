// SPDX-License-Identifier: MIT
// phx_sysctl.cpp: service-wide state and the debug.* sysctls (phxcalls, phxbo_*, phxgva, phxih*, phxirq*, phxvcn*,
// phxmmvm*, phxwait_*, phxdal_*, phxscan_latchfix/race_stat, phxvm, phxpt_stat, phxshot, phxedid*).
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.


// ---- service ---------------------------------------------------------------------------------------------

// Build 50 (the window-drag lag): what the client lock costs. Every user-client call but WaitSeq runs under the client's one lock; a
// WaitSeq takes it to read the emitted count and again once its seqno retired. Per selector: calls, time spent waiting
// to get the lock, time holding it, and the longest of each (all processes). Slots 64..67: WaitSeq's first lock, its
// lock after retirement, the whole WaitSeq call, and retirement seen -> return. Log only.
struct PhxCallStat { volatile SInt64 n, waitNs, holdNs, maxWaitNs, maxHoldNs; };
static PhxCallStat gCall[68];
static IOLock *gWaitSleepLock;   // b54: doWaitSeq sleeps on it with a deadline (never signalled)
// b56 (WP4): debug.phxbo_zero = how BOCREATE clears a new BO: 1 bzero (default), 2 count the pages that come back from
// the allocator non-zero, then bzero (measurement), 0 none. debug.phxbo_stats: BOs, bytes, allocation and clear time,
// pages found non-zero in mode 2.
// b57: default 3 = no second clear (b56: 603741 fresh pages checked, none non-zero: the allocator zero-fills), with every
// 64th BO checked whole; a non-zero page found that way turns mode 1 back on for good.
static int gBoZero = 3;
static volatile SInt64 gBoN, gBoBytesAll, gBoAllocNs, gBoZeroNs, gBoDirtyPages, gBoCheckedPages;
SYSCTL_INT(_debug, OID_AUTO, phxbo_zero, CTLFLAG_RW | CTLFLAG_LOCKED, &gBoZero, 0, "BOCREATE clear: 1 bzero, 2 count non-zero pages then bzero, 3 none with every 64th BO checked (default), 0 none");
static int phxBoStatsSysctl SYSCTL_HANDLER_ARGS {
    char b[256];
    snprintf(b, sizeof b, "bos %lld bytes %lld MB alloc %lld us clear %lld us pages checked %lld non-zero %lld",
             (long long)gBoN, (long long)(gBoBytesAll >> 20), (long long)(gBoAllocNs / 1000), (long long)(gBoZeroNs / 1000),
             (long long)gBoCheckedPages, (long long)gBoDirtyPages);
    return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxbo_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxBoStatsSysctl, "A", "BOCREATE allocation/clear statistics");
// b58 (WP6 M0): debug.phxgva=1 publishes on NMXAccel the properties VideoToolbox's GVA decoders look for (copied from
// Apple's AMD personality, with IODVDBundleName PhxVA: AppleGVA then loads /Library/GPUBundles/PhxVA.bundle); 0 removes
// them. Read by each new decoder session, so no reboot is needed either way.
static int gPhxGva = 0;
static void phxGvaApply(int on) {
    OSDictionary *m = IOService::serviceMatching("NMXAccel");
    IOService *svc = m ? IOService::copyMatchingService(m) : nullptr;
    OSSafeReleaseNULL(m);
    if (!svc)
        return;
    static const char *const keys[] = {"IOVARendererID", "IODVDBundleName", "IOGVACodec", "IOGVAHEVCDecode", "IOGVAHEVCDecodeCapabilities"};
    if (!on) {
        for (const char *k : keys)
            svc->removeProperty(k);
    } else {
        svc->setProperty("IOVARendererID", 0x1020000ull, 32);
        svc->setProperty("IODVDBundleName", "PhxVA");
        svc->setProperty("IOGVACodec", "AMDVCN2");
        svc->setProperty("IOGVAHEVCDecode", "1");
        OSDictionary *caps = OSDictionary::withCapacity(2), *per = OSDictionary::withCapacity(3);
        OSArray *profiles = OSArray::withCapacity(3);
        for (unsigned prof = 1; prof <= 3; prof++) {
            OSNumber *n = OSNumber::withNumber(prof, 32);
            profiles->setObject(n);
            OSDictionary *d = OSDictionary::withCapacity(1);
            OSNumber *lvl = OSNumber::withNumber(153ull, 32);
            d->setObject("VTMaxDecodeLevel", lvl);
            char k[4]; snprintf(k, sizeof k, "%u", prof);
            per->setObject(k, d);
            lvl->release(); d->release(); n->release();
        }
        caps->setObject("VTSupportedProfileArray", profiles);
        caps->setObject("VTPerProfileDetails", per);
        svc->setProperty("IOGVAHEVCDecodeCapabilities", caps);
        caps->release(); per->release(); profiles->release();
    }
    svc->release();
}
static int phxGvaSysctl SYSCTL_HANDLER_ARGS {
    int v = gPhxGva;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    gPhxGva = v ? 1 : 0;
    phxGvaApply(gPhxGva);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxgva, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxGvaSysctl, "I", "1 publishes the GVA video-decoder properties on NMXAccel (IODVDBundleName PhxVA), 0 removes them");
// b60 (WP1 step 2a): debug.phxih=1 programs the IH ring (256 KiB, physically contiguous system memory, MC_SPACE 2 because
// IH_CHICKEN.GPA_EN is set on this card) with RB_ENABLE but no interrupt,
// and turns on CP_INT_CNTL TIME_STAMP for the gfx and compute rings; reading debug.phxih_stat drains it and counts IVs
// per (client, src). debug.phxeopirq=1 makes each submission's RELEASE_MEM ask for the EOP interrupt (INT_SEL 2) with the
// fence's low dword in the packet. The ring is disabled at stop() and before a restart or power-off.
static int gIhOn = 0, gEopIrq = 0, gIhVram = 0;
static uint8_t *gIhRing;   // CPU view of the ring (system memory, or VRAM through the BAR with debug.phxih_vram)
// b61: debug.phxih_vram=1 (read when debug.phxih goes 0 -> 1) puts the ring in VRAM at fw_buf + 12 MiB, MC_SPACE 4
// (VMID 0 through MMHUB's system aperture, as the display reads VRAM): b60's system-memory ring (MC_SPACE 2) got no IV
static constexpr uint64_t kIhVramOff = 0x8C00000ull;
static IOBufferMemoryDescriptor *gIhBuf, *gIhDummy;
static uint64_t gIhPa;
static uint32_t gIhRptr;
static volatile SInt64 gIhIvs, gIhEop, gIhOther, gIhOverflow;
static uint32_t gIhLast[8], gIhKey[16], gIhCnt[16], gIhLastSeq;
static IOSimpleLock *gIhSpin;   // rptr and counters: the MSI filter and debug.phxih_stat both drain
static IONotifier *gIhPowerNotifier;
static constexpr uint32_t kIhRingBytes = 256u << 10;
static int phxIhSysctl SYSCTL_HANDLER_ARGS {
    int v = gIhOn;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (!gPhx)
        return ENXIO;
    if (v && !gIhOn)
        return gPhx->ihEnable() == kIOReturnSuccess ? 0 : EIO;
    if (!v && gIhOn)
        gPhx->ihDisable();
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxih, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxIhSysctl, "I", "1 enables the polled IH ring (WP1 step 2a), 0 disables it");
static int phxIhStatSysctl SYSCTL_HANDLER_ARGS {
    char b[1024];
    const int len = gPhx ? gPhx->ihStat(b, sizeof b) : snprintf(b, sizeof b, "no PhxBringup");
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxih_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxIhStatSysctl, "A", "IH ring state; reading drains it and counts IVs per client/src");
SYSCTL_INT(_debug, OID_AUTO, phxih_vram, CTLFLAG_RW | CTLFLAG_LOCKED, &gIhVram, 0, "1 = the next debug.phxih=1 puts the IH ring in VRAM (MC_SPACE 4)");
SYSCTL_INT(_debug, OID_AUTO, phxeopirq, CTLFLAG_RW | CTLFLAG_LOCKED, &gEopIrq, 0, "1 = RELEASE_MEM requests the EOP interrupt (INT_SEL 2) and carries the fence low dword");
// b62 (WP1 step 2b): debug.phxirq=1 (needs debug.phxih=1) registers an IOFilterInterruptEventSource on the PCI MSI index
// (NullMoth's getInterruptType loop), enables it, then sets IH_RB_CNTL.ENABLE_INTR. The filter drains the ring in
// primary interrupt context (no hop to the work loop) and counts; an MSI storm (> 50k/s) clears ENABLE_INTR again.
// Waits do not use it yet (step 3).
static int gIrqOn = 0, gIrqIndex = -1, gIrqStorm = 0;
static IOWorkLoop *gIrqWl;
static IOFilterInterruptEventSource *gIrqSrc;
static volatile SInt64 gIrqMsi, gIrqSpurious, gIrqLoops;
static uint64_t gIrqWinAbs, gIrqWinLen;
static uint32_t gIrqWinCnt;
static int phxIrqSysctl SYSCTL_HANDLER_ARGS {
    int v = gIrqOn;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (!gPhx)
        return ENXIO;
    if (v && !gIrqOn) {
        const IOReturn r = gPhx->irqEnable();
        return r == kIOReturnSuccess ? 0 : r == kIOReturnNotReady ? EINVAL : EIO;
    }
    if (!v && gIrqOn)
        gPhx->irqDisable();
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxirq, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxIrqSysctl, "I", "1 lets the IH ring raise the MSI (needs debug.phxih=1; WP1 step 2b), 0 masks it");
static int phxIrqStatSysctl SYSCTL_HANDLER_ARGS {
    char b[512];
    const int len = gPhx ? gPhx->irqStat(b, sizeof b) : snprintf(b, sizeof b, "no PhxBringup");
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxirq_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxIrqStatSysctl, "A", "MSI state and counters (does not drain the ring)");
static bool phxIrqFilter(OSObject *owner, IOFilterInterruptEventSource *) {
    static_cast<PhxBringup *>(owner)->irqFilter();
    return false;
}
static void phxIrqAction(OSObject *, IOInterruptEventSource *, int) {}
// b66 (WP6 B1): debug.phxvcn=1 adopts the VCN0 unified ring left by vcn_up.sh --run (the vcn_m1_gen.py layout: ring at
// VRAM 0x8800000, 0x400 dw, fence at VRAM 0x8801000) after checking the VCN registers agree; 2 runs a ring test from
// the kext (VCN_ENC_CMD_END + 63 NOPs at the adopted wptr, UVD_RB_WPTR by MMIO, wait for UVD_RB_RPTR); 0 forgets it.
// Nothing else uses the ring yet (B2 adds submissions).
static constexpr uint32_t kVcnPowerStatus = 0x7e02, kVcnStatus = 0x7f43, kVcnRbBaseLo = 0x7eea, kVcnRbBaseHi = 0x7eeb,
                          kVcnRbSize = 0x7eec, kVcnRbEnable = 0x7e85, kVcnRbRptr = 0x7eac, kVcnRbWptr = 0x7ead;
static constexpr uint64_t kVcnRingOff = 0x8800000ull, kVcnFenceOff = 0x8801000ull;
static constexpr uint32_t kVcnRingDw = 0x400;
static int gVcnOn = 0;
static uint32_t gVcnWptr, gVcnTestsOk, gVcnTestsFail, gVcnLastUs, gVcnLastErr;
static IOLock *gVcnLock;
static int phxVcnSysctl SYSCTL_HANDLER_ARGS {
    int v = gVcnOn;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    return gPhx ? gPhx->vcnCtl(v) : ENXIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxvcn, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxVcnSysctl, "I", "1 adopts the VCN0 ring vcn_up.sh brought up, 2 runs a kext ring test, 0 forgets it (WP6 B1)");
static int phxVcnStatSysctl SYSCTL_HANDLER_ARGS {
    char b[512];
    const int len = gPhx ? gPhx->vcnStat(b, sizeof b) : snprintf(b, sizeof b, "no PhxBringup");
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxvcn_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxVcnStatSysctl, "A", "VCN0 ring state (WP6 B1)");
// b67 (WP6 B2): debug.phxmmvm=1 turns MMHUB VM translation on with the values Linux programs on this card
// (read on the box): the VBIOS left MMHUB physical-only (MX_L1_TLB_CNTL 0x501), so VCN could
// not reach a client VA. Context 0 stays off (VMID 0 = display, VCN ring/fence, IH: all inside the system aperture);
// contexts 8..15 get the same root as GC when vmBind lends the VMID. 0 restores the VBIOS values.
static constexpr uint32_t kMmL2Cntl = 0x1A700, kMmL2Cntl2 = 0x1A701, kMmL2Cntl3 = 0x1A702, kMmL2Status = 0x1A703,
                          kMmFaultCntl2 = 0x1A709, kMmFaultStatus = 0x1A70C, kMmFaultAddrLo = 0x1A70D, kMmFaultAddrHi = 0x1A70E,
                          kMmFaultDefLo = 0x1A70F, kMmFaultDefHi = 0x1A710, kMmIdApLowLo = 0x1A712, kMmL2Cntl4 = 0x1A718,
                          kMmBankSel = 0x1A71B, kMmL2Cntl5 = 0x1A71E, kMmCtx0Cntl = 0x1A740, kMmEng0Sem = 0x1A751,
                          kMmEng17Sem = 0x1A762, kMmEng17Req = 0x1A774, kMmEng17Ack = 0x1A786, kMmEng0RangeLo = 0x1A787,
                          kMmCtx0BaseLo = 0x1A7AB, kMmCtx0StartLo = 0x1A7CB, kMmCtx0EndLo = 0x1A7EB, kMmSysapDefLsb = 0x1A8D8,
                          kMmSysapDefMsb = 0x1A8D9, kMmMxL1TlbCntl = 0x1A8F3;
static int gMmvmOn = 0;
static uint32_t gMmvmSaved[8], gMmvmBad, gMmvmFlushTimeouts, gMmvmFlushMaxUs, gMmvmFlushes;
static IOBufferMemoryDescriptor *gMmDummy;
static volatile SInt64 gVcnSubmits;
// b68: when each VCN ring entry was queued (mach absolute time), and the watchdog state
static uint64_t gVcnSubAbs[64], gVcnStuckAbs;
static volatile SInt32 gVcnStuck, gVcnStuckCount;
static int phxMmvmSysctl SYSCTL_HANDLER_ARGS {
    int v = gMmvmOn;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    return gPhx ? gPhx->mmvmCtl(v) : ENXIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxmmvm, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxMmvmSysctl, "I", "1 turns MMHUB VM translation on for VCN (WP6 B2), 0 restores the VBIOS physical-only mode");
static int phxMmvmStatSysctl SYSCTL_HANDLER_ARGS {
    char b[640];
    const int len = gPhx ? gPhx->mmvmStat(b, sizeof b) : snprintf(b, sizeof b, "no PhxBringup");
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxmmvm_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxMmvmStatSysctl, "A", "MMHUB VM state (never reads the semaphore registers)");
// b63 (WP1 step 3): debug.phxwait_irq=1 makes WAITSEQ, after its spin, sleep on gFenceEvt with a 2 ms deadline instead
// of b54's 50 us timed steps; the MSI filter calls thread_wakeup(&gFenceEvt) whenever it drained an EOP IV. Only used
// while debug.phxirq and debug.phxeopirq are on and no storm was seen; otherwise the timed steps stay.
static int gWaitIrq = 0, gWaitIrqDlUs = 2000;
SYSCTL_INT(_debug, OID_AUTO, phxwait_irq_dl_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gWaitIrqDlUs, 0, "IRQ wait: microseconds per sleep before re-checking the fence (b65)");
static volatile SInt32 gFenceEvt;
static volatile uint64_t gIrqEopAbs;   // when the filter last drained an EOP IV
static volatile SInt64 gWiSleeps, gWiIrqWakes, gWiDeadline, gWiMissed, gWiSelf, gWiLatNs, gWiLatMax;
// b64: the filter stamps each EOP IV's seq (src_data[0] = the fence low dword) into a small table; a WAITSEQ that slept
// looks up its target and adds (return time - stamp) to a histogram for its mode (0 = timed steps, 1 = IRQ wait)
static constexpr unsigned kEopArrN = 1024;
static volatile uint32_t gEopArrSeq[kEopArrN];
static volatile uint64_t gEopArrAbs[kEopArrN];
static const uint32_t kLatEdgeUs[] = {5, 10, 25, 50, 100, 200, 500, 1000, 2000};
static constexpr unsigned kLatBins = sizeof kLatEdgeUs / sizeof kLatEdgeUs[0] + 1;
static volatile SInt64 gLatHist[2][kLatBins], gLatSum[2], gLatN[2], gLatNoStamp[2], gLatNeg[2], gWaitSpinDone, gWaitSlept[2];
static int phxWaitLatSysctl SYSCTL_HANDLER_ARGS {
    char b[1024];
    int o = snprintf(b, sizeof b, "waits done in spin %lld", (long long)gWaitSpinDone);
    for (unsigned m = 0; m < 2; m++) {
        o += snprintf(b + o, sizeof b - static_cast<size_t>(o), "\n%s: slept %lld, measured %lld avg %lld us, no stamp %lld, before stamp %lld; us<=",
                      m ? "irq" : "timed", (long long)gWaitSlept[m], (long long)gLatN[m], gLatN[m] ? (long long)(gLatSum[m] / gLatN[m] / 1000) : 0ll,
                      (long long)gLatNoStamp[m], (long long)gLatNeg[m]);
        for (unsigned k = 0; k < kLatBins - 1; k++)
            o += snprintf(b + o, sizeof b - static_cast<size_t>(o), " %u:%lld", kLatEdgeUs[k], (long long)gLatHist[m][k]);
        o += snprintf(b + o, sizeof b - static_cast<size_t>(o), " more:%lld", (long long)gLatHist[m][kLatBins - 1]);
    }
    if (req->newptr) {   // any write resets
        bzero(const_cast<SInt64 *>(&gLatHist[0][0]), sizeof gLatHist);
        gLatSum[0] = gLatSum[1] = gLatN[0] = gLatN[1] = gLatNoStamp[0] = gLatNoStamp[1] = gLatNeg[0] = gLatNeg[1] = 0;
        gWaitSpinDone = gWaitSlept[0] = gWaitSlept[1] = 0;
        return 0;
    }
    return SYSCTL_OUT(req, b, static_cast<size_t>(o) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxwait_lat, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxWaitLatSysctl, "A", "EOP interrupt -> waiter running histogram per wait mode; write anything to reset (WP1 step 3)");
static void phxLatNote(unsigned m, uint32_t target, uint64_t nowAbs) {
    const unsigned i = target & (kEopArrN - 1);
    if (gEopArrSeq[i] != target) { OSIncrementAtomic64(&gLatNoStamp[m]); return; }
    const uint64_t st = gEopArrAbs[i];
    if (nowAbs < st) { OSIncrementAtomic64(&gLatNeg[m]); return; }
    uint64_t ns;
    absolutetime_to_nanoseconds(nowAbs - st, &ns);
    unsigned k = 0;
    while (k < kLatBins - 1 && ns > static_cast<uint64_t>(kLatEdgeUs[k]) * 1000) k++;
    OSIncrementAtomic64(&gLatHist[m][k]);
    OSAddAtomic64(static_cast<SInt64>(ns), &gLatSum[m]);
    OSIncrementAtomic64(&gLatN[m]);
}
SYSCTL_INT(_debug, OID_AUTO, phxwait_irq, CTLFLAG_RW | CTLFLAG_LOCKED, &gWaitIrq, 0, "1 = WAITSEQ sleeps until the EOP interrupt (needs debug.phxirq and debug.phxeopirq), 2 ms deadline");
static int phxWaitStatSysctl SYSCTL_HANDLER_ARGS {
    char b[384];
    const int len = snprintf(b, sizeof b, "wait_irq %d (active %d); sleeps %lld irq wakes %lld deadline wakes %lld (fence already done %lld) self wakes %lld; filter->waiter latency avg %lld ns max %lld ns",
                             gWaitIrq, gWaitIrq && gIrqOn && gEopIrq && !gIrqStorm, (long long)gWiSleeps, (long long)gWiIrqWakes,
                             (long long)gWiDeadline, (long long)gWiMissed, (long long)gWiSelf,
                             gWiIrqWakes ? (long long)(gWiLatNs / gWiIrqWakes) : 0ll, (long long)gWiLatMax);
    if (req->newptr)
        return EPERM;
    return SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxwait_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxWaitStatSysctl, "A", "WAITSEQ interrupt-wait counters (WP1 step 3)");
static IOReturn phxIhPowerHandler(void *, void *, UInt32 type, IOService *, void *, vm_size_t) {
    if ((type == kIOMessageSystemWillRestart || type == kIOMessageSystemWillPowerOff) && gIhOn && gPhx)
        gPhx->ihDisable();
    return kIOReturnSuccess;
}
// b55: debug.phxwait_spin_us / debug.phxwait_step_us: how long WAITSEQ spins before it sleeps, and each sleep step
// (a same-boot A/B of b54's sleeping wait; spin >= the timeout = b53's behaviour, spinning for the whole wait)
static int gWaitSpinUs = 20, gWaitStepUs = 50;
// b72: debug.phxwait_scoped (default 0 = b71 behaviour): WAITSEQ waits only on the client's own queues
static int gWaitScoped = 0;
static SInt64 gWsWaits, gWsEarly;   // scoped waits; of those, ended while the global minimum was still behind
SYSCTL_INT(_debug, OID_AUTO, phxwait_scoped, CTLFLAG_RW | CTLFLAG_LOCKED, &gWaitScoped, 0, "1 = WAITSEQ ends when the client's own queues retired its target (b72)");
static int phxWaitScopedStat SYSCTL_HANDLER_ARGS {
    char b[160];
    snprintf(b, sizeof b, "scoped waits %lld, ended before the global minimum %lld", gWsWaits, gWsEarly);
    return sysctl_handle_string(oidp, b, 0, req);
}
// b76: DAL mailbox state (idle-power work)
static int gDalDead = 0;   // 1 after the first DAL timeout: every later DAL and driver SMU message is refused until reboot
static SInt64 gDalSent, gDalOk, gDalNotOk, gDalBusy, gDalTimeouts;
static uint32_t gDalLastMsg, gDalLastArg, gDalLastResp;
SYSCTL_INT(_debug, OID_AUTO, phxdal_dead, CTLFLAG_RD | CTLFLAG_LOCKED, &gDalDead, 0, "1 = a DAL message timed out; SMU/DAL messages refused until reboot (b76)");
static int phxDalStat SYSCTL_HANDLER_ARGS {
    char b[200];
    snprintf(b, sizeof b, "sent %lld ok %lld not-ok %lld busy %lld timeouts %lld dead %d last msg 0x%x arg 0x%x resp 0x%x",
             gDalSent, gDalOk, gDalNotOk, gDalBusy, gDalTimeouts, gDalDead, gDalLastMsg, gDalLastArg, gDalLastResp);
    return sysctl_handle_string(oidp, b, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, phxdal_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDalStat, "A", "b76 DAL mailbox counters");
// b74: the scanout latch race (UI shake)
static int gScanLatchFix = 0;
static SInt64 gScanEarliestLag, gScanAcceptedForgotten, gScanReusableGhost;
static uint32_t gScanGhost = 0xffffffffu;   // slot of the last accepted-but-forgotten flip (on screen until a later latch)
SYSCTL_INT(_debug, OID_AUTO, phxscan_latchfix, CTLFLAG_RW | CTLFLAG_LOCKED, &gScanLatchFix, 0, "1 = latch a flip on FLIP_PENDING clear alone; the front slot is never reusable (b74)");
static int phxScanRaceStat SYSCTL_HANDLER_ARGS {
    char b[200];
    snprintf(b, sizeof b, "earliest lag %lld, accepted flips forgotten %lld, on-screen slot reported reusable %lld, latchfix %d",
             gScanEarliestLag, gScanAcceptedForgotten, gScanReusableGhost, gScanLatchFix);
    return sysctl_handle_string(oidp, b, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, phxscan_race_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxScanRaceStat, "A", "b74 scanout latch race counters");
SYSCTL_PROC(_debug, OID_AUTO, phxwait_scoped_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxWaitScopedStat, "A", "b72 scoped WAITSEQ counters");
SYSCTL_INT(_debug, OID_AUTO, phxwait_spin_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gWaitSpinUs, 0, "WAITSEQ: microseconds spun before sleeping");
SYSCTL_INT(_debug, OID_AUTO, phxwait_step_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gWaitStepUs, 0, "WAITSEQ: microseconds per sleep step");
static volatile SInt32 gDeferFrees, gDeferUnmaps, gDeferOverlapWaits, gDeferWaitTimeouts, gDeferPendingMax;
static inline uint64_t phxNowNs() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns; }
static void phxCallNote(unsigned slot, uint64_t waitNs, uint64_t holdNs) {
    if (slot >= 68) return;
    PhxCallStat &c = gCall[slot];
    OSAddAtomic64(1, &c.n); OSAddAtomic64((SInt64)waitNs, &c.waitNs); OSAddAtomic64((SInt64)holdNs, &c.holdNs);
    if ((SInt64)waitNs > c.maxWaitNs) c.maxWaitNs = (SInt64)waitNs;
    if ((SInt64)holdNs > c.maxHoldNs) c.maxHoldNs = (SInt64)holdNs;
}
static int phxcalls_sysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    char *buf = static_cast<char *>(IOMalloc(8192));
    if (!buf) return ENOMEM;
    int len = 0;
    for (unsigned i = 0; i < 68 && len < 8000; i++) {
        const PhxCallStat &c = gCall[i];
        if (!c.n) continue;
        len += snprintf(buf + len, 8192 - len, "sel %u n %lld wait_us %lld maxwait_us %lld hold_us %lld maxhold_us %lld\n", i, c.n,
                        c.waitNs / 1000, c.maxWaitNs / 1000, c.holdNs / 1000, c.maxHoldNs / 1000);
    }
    len += snprintf(buf + len, 8192 - len, "deferred: frees %d unmaps %d remap-waits %d wait-timeouts %d max-pending %d\n",
                    gDeferFrees, gDeferUnmaps, gDeferOverlapWaits, gDeferWaitTimeouts, gDeferPendingMax);
    if (len > 8191) len = 8191;
    const int err = SYSCTL_OUT(req, buf, len + 1);
    IOFree(buf, 8192);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, phxcalls, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxcalls_sysctl, "A",
            "user-client calls: count, client-lock wait and hold time per selector");
static int phxvm_sysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    char *buf = static_cast<char *>(IOMalloc(4096));
    if (!buf) return ENOMEM;
    int len = gPhx ? gPhx->vmDump(buf, 4096) : snprintf(buf, 4096, "no PhxBringup\n");
    if (len < 0) len = 0;
    if (len > 4095) len = 4095;
    const int err = SYSCTL_OUT(req, buf, len + 1);
    IOFree(buf, 4096);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, phxvm, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxvm_sysctl, "A",
            "GPU VM state: page-table pool, hardware VMID owners, map failures");
static int phxpt_stat_sysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    char *buf = static_cast<char *>(IOMalloc(8192));
    if (!buf) return ENOMEM;
    int len = gPhx ? gPhx->ptStat(buf, 8192) : snprintf(buf, 8192, "no PhxBringup\n");
    if (len < 0) len = 0;
    if (len > 8191) len = 8191;
    const int err = SYSCTL_OUT(req, buf, len + 1);
    IOFree(buf, 8192);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, phxpt_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxpt_stat_sysctl, "A",
            "B-14 page-table census per context (leaves by valid PTEs, parked) and reclaim counters");
static int phxshot_sysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    return gPhx ? gPhx->shot(req) : ENXIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxshot, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxshot_sysctl, "S",
            "the scanned-out frame: 'PSHT', width, height, VRAM offset, then BGRA rows");
static int gEdidSrc = 0x10;
// b80: phxEdidRead (PhxAMDFB) reads the EDID of the connector lit on pipe 0 (phxddc::scanoutSource); 0 = always gEdidSrc
static int gEdidAuto = 1, gEdidAutoSrc = -1;
SYSCTL_INT(_debug, OID_AUTO, phxedid_auto, CTLFLAG_RW | CTLFLAG_LOCKED, &gEdidAuto, 0,
           "1 = PhxAMDFB's EDID comes from the connector the GOP lit on pipe 0 (DIG HPD select + mode), 0 = debug.phxedid_src");
SYSCTL_INT(_debug, OID_AUTO, phxedid_auto_src, CTLFLAG_RD | CTLFLAG_LOCKED, &gEdidAutoSrc, 0,
           "the source the last phxEdidRead chose (0x10 DDC1, 0x20+n AUX n; -1 = no lit DIG found, debug.phxedid_src used)");
// b81 (M7 S3a): the last phxEdidRead for a second head (in-value phxddc::kSrcOther): the source chosen, and how
static int gEdidOtherSrc = -1, gEdidOtherInfo = 0;
SYSCTL_INT(_debug, OID_AUTO, phxedid_other_src, CTLFLAG_RD | CTLFLAG_LOCKED, &gEdidOtherSrc, 0,
           "the source the last second-head phxEdidRead chose (the monitor not on pipe 0; -1 = none asked yet)");
SYSCTL_INT(_debug, OID_AUTO, phxedid_other_info, CTLFLAG_RD | CTLFLAG_LOCKED, &gEdidOtherInfo, 0,
           "that choice: bits 7:0 pipe 0's source (0xff = none lit), 12:8 HPD0-4 sensed, bit 16 = the chosen one's HPD sensed");
SYSCTL_INT(_debug, OID_AUTO, phxedid_src, CTLFLAG_RW | CTLFLAG_LOCKED, &gEdidSrc, 0,
           "debug.phxedid source: 0x10+n = DC_I2C DDC line n (0x10 = DDC1, HDMI), 0x20+n = DP AUX engine n (0x22 = AUX2)");
static int phxedid_sysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (proc_suser(req->p)) return EPERM;                                  // it drives the I2C/AUX engine: root only
    return gPhx ? gPhx->edidRead(req, static_cast<unsigned>(gEdidSrc)) : ENXIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxedid, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxedid_sysctl, "S",
            "live EDID read: 'PEDI', source, rc, length, SW_STATUS, ARB, HPD_Y, HPD sense, then 256 bytes");

