// SPDX-License-Identifier: MIT
// phx_client.cpp: sessions, newUserClient (registers the sysctls) and the user client: open/close, BOs, host imports,
// GPU VA, contexts, submit.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

bool PhxBringup::claimSession() { return OSCompareAndSwap(0, 1, &sessionOpen); }

void PhxBringup::releaseSession() { OSCompareAndSwap(1, 0, &sessionOpen); }

IOReturn PhxBringup::readRegs(uint32_t first, uint32_t count, uint32_t *out) {
    if (!mmio)
        return kIOReturnNotReady;
    uint64_t len = mmio->getLength();
    if ((static_cast<uint64_t>(first) + count) * 4 > len)
        return kIOReturnBadArgument;
    volatile uint32_t *r = reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress());
    IOSimpleLock *l = phxMmIdxDword(first, count) ? gMmIdxLock : nullptr;   // b82: never between an MM_INDEX write and its data
    const IOInterruptState s = l ? IOSimpleLockLockDisableInterrupt(l) : 0;
    for (uint32_t i = 0; i < count; i++)
        out[i] = r[first + i];
    if (l) IOSimpleLockUnlockEnableInterrupt(l, s);
    return kIOReturnSuccess;
}

// b79 (M6 step 2): PhxAMDFB (PhxAccel) asks for one live EDID read: param1 = uint8_t[256] out, param2 = uint32_t *len,
// param3 = int32_t *rc (PhxDdc.h result), param4 = uint32_t *src (out: the source read, debug.phxedid_src).
// b81 (M7 S3a): *param4 is also read first, as the requested source (phxddc::resolveSource): 0 / ~0u = the b80 rule,
// ~1u = the other monitor (a second head), a valid source = that one. Marked by the property phx-edid-req.
struct phx_pipe2_setmode;
static IOReturn phxPipe2SetModeFn(phx_pipe2_setmode *m);   // b93, phx_pipe2.cpp (PhxSwitchABI.h)
struct phx_pipe2_gamma;
static IOReturn phxPipe2SetGammaFn(phx_pipe2_gamma *g);    // b98, phx_gamma.cpp (PhxGamma.h)
struct phx_pipe0_setmode;
static IOReturn phxPipe0SetModeFn(phx_pipe0_setmode *m);   // b101, phx_pipe0.cpp (PhxSwitchABI.h)
IOReturn PhxBringup::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                          void *param3, void *param4) {
    if (functionName && functionName->isEqualTo("phxPipe2SetMode"))
        return phxPipe2SetModeFn(static_cast<phx_pipe2_setmode *>(param1));
    if (functionName && functionName->isEqualTo("phxPipe2SetGamma"))
        return phxPipe2SetGammaFn(static_cast<phx_pipe2_gamma *>(param1));
    if (functionName && functionName->isEqualTo("phxPipe0SetMode"))
        return phxPipe0SetModeFn(static_cast<phx_pipe0_setmode *>(param1));
    if (functionName && functionName->isEqualTo("phxEdidRead")) {
        const uint32_t req = param4 ? *static_cast<uint32_t *>(param4) : 0u;
        // b80: the source follows the connector the GOP lit on pipe 0 (phxddc::scanoutSource) unless debug.phxedid_auto=0
        int auto_src = -1;
        uint32_t hpd = 0;
        if ((gEdidAuto || req == phxddc::kSrcOther) && mmio) {
            phxddc::Io io{this, [](void *c, uint32_t r) { return static_cast<PhxBringup *>(c)->rreg(r); },
                          [](void *, uint32_t, uint32_t) {}, [](void *, uint32_t) {}};   // reads only
            auto_src = phxddc::scanoutSource(io);
            if (req == phxddc::kSrcOther)
                for (unsigned n = 0; n < 5; n++) if (phxddc::hpdSensed(io, n)) hpd |= 1u << n;
        }
        bool sensed = false;
        const uint32_t src = phxddc::resolveSource(req, auto_src, gEdidAuto != 0, static_cast<uint32_t>(gEdidSrc), hpd, &sensed);
        if (req == phxddc::kSrcOther) {   // b81: the second head's choice, readable (our IOLog never reaches the log)
            gEdidOtherSrc = static_cast<int>(src);
            gEdidOtherInfo = static_cast<int>((auto_src & 0xFF) | (hpd << 8) | (sensed ? 1u << 16 : 0u));
        } else {
            gEdidAutoSrc = gEdidAuto ? auto_src : -1;
        }
        if (param4) *static_cast<uint32_t *>(param4) = src;
        return edidFetch(static_cast<uint8_t *>(param1), static_cast<uint32_t *>(param2), static_cast<int32_t *>(param3), src);
    }
    return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

IOReturn PhxBringup::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
    // the sysctls go up with the first client, from user space, rather than in start(): the bootloader injects this
    // kext into the boot collection, and it starts long before the rest of BSD
    if (!gSysctlUp && OSCompareAndSwap(0, 1, &gSysctlUp)) {
        sysctl_register_oid(&sysctl__debug_phxbos_flush);
        sysctl_register_oid(&sysctl__debug_phxwait_spin_us);
        sysctl_register_oid(&sysctl__debug_phxbo_zero);
        sysctl_register_oid(&sysctl__debug_phxgva);
        sysctl_register_oid(&sysctl__debug_phxih);
        sysctl_register_oid(&sysctl__debug_phxih_stat);
        sysctl_register_oid(&sysctl__debug_phxeopirq);
        sysctl_register_oid(&sysctl__debug_phxih_vram);
        sysctl_register_oid(&sysctl__debug_phxirq);
        sysctl_register_oid(&sysctl__debug_phxirq_stat);
        sysctl_register_oid(&sysctl__debug_phxwait_irq);
        sysctl_register_oid(&sysctl__debug_phxwait_stat);
        sysctl_register_oid(&sysctl__debug_phxwait_lat);
        sysctl_register_oid(&sysctl__debug_phxwait_irq_dl_us);
        sysctl_register_oid(&sysctl__debug_phxvcn);
        sysctl_register_oid(&sysctl__debug_phxvcn_stat);
        sysctl_register_oid(&sysctl__debug_phxmmvm);
        sysctl_register_oid(&sysctl__debug_phxmmvm_stat);
        sysctl_register_oid(&sysctl__debug_phxbo_stats);
        sysctl_register_oid(&sysctl__debug_phxwait_step_us);
        sysctl_register_oid(&sysctl__debug_phxwait_scoped);
        sysctl_register_oid(&sysctl__debug_phxwait_scoped_stat);
        sysctl_register_oid(&sysctl__debug_phxscan_latchfix);
        sysctl_register_oid(&sysctl__debug_phxscan_race_stat);
        sysctl_register_oid(&sysctl__debug_phxdal_dead);
        sysctl_register_oid(&sysctl__debug_phxdal_stat);
        sysctl_register_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_register_oid(&sysctl__debug_phxbos_flush_host);
        sysctl_register_oid(&sysctl__debug_phxvm);
        sysctl_register_oid(&sysctl__debug_phxpt_reclaim);
        sysctl_register_oid(&sysctl__debug_phxpt_keep);
        sysctl_register_oid(&sysctl__debug_phxpt_stat);
        sysctl_register_oid(&sysctl__debug_phxmtype);
        sysctl_register_oid(&sysctl__debug_phxshot);
        sysctl_register_oid(&sysctl__debug_phxedid);
        sysctl_register_oid(&sysctl__debug_phxedid_src);
        sysctl_register_oid(&sysctl__debug_phxedid_auto);
        sysctl_register_oid(&sysctl__debug_phxedid_auto_src);
        sysctl_register_oid(&sysctl__debug_phxedid_other_src);   // b81
        sysctl_register_oid(&sysctl__debug_phxedid_other_info);
        sysctl_register_oid(&sysctl__debug_phxkey);
        sysctl_register_oid(&sysctl__debug_phxcalls);
        sysctl_register_oid(&sysctl__debug_phxscan);
        if (!gMmIdxLock) gMmIdxLock = IOSimpleLockAlloc();   // b82: before the sysctls that use it
        sysctl_register_oid(&sysctl__debug_phxpipe2);
        sysctl_register_oid(&sysctl__debug_phxpipe2_stat);
        sysctl_register_oid(&sysctl__debug_phxpipe2_plan);
        sysctl_register_oid(&sysctl__debug_phxpipe2_head1);   // b87
        sysctl_register_oid(&sysctl__debug_phxdmub_ring);
        sysctl_register_oid(&sysctl__debug_phxdmub_dead);
        sysctl_register_oid(&sysctl__debug_phxdmub_trace);   // b88
        sysctl_register_oid(&sysctl__debug_phxpipe2_mode);   // b90
        sysctl_register_oid(&sysctl__debug_phxpipe2_switch);   // b91
        sysctl_register_oid(&sysctl__debug_phxpipe2_switch_stat);
        sysctl_register_oid(&sysctl__debug_phxpipe2_phase);
        sysctl_register_oid(&sysctl__debug_phxpipe2_det);
        sysctl_register_oid(&sysctl__debug_phxscdc);   // b107 SCDC1
        sysctl_register_oid(&sysctl__debug_phxdmub_restart); sysctl_register_oid(&sysctl__debug_phxdmub_restart_stat);   // b109
        sysctl_register_oid(&sysctl__debug_phxdmub_trace_pre);
        sysctl_register_oid(&sysctl__debug_phxscdc_fix); sysctl_register_oid(&sysctl__debug_phxscdc_fix_stat);
        sysctl_register_oid(&sysctl__debug_phxdmub_regtest); sysctl_register_oid(&sysctl__debug_phxdmub_regtest_stat);   // b111
        sysctl_register_oid(&sysctl__debug_phxgamma);   // b98
        sysctl_register_oid(&sysctl__debug_phxgamma_test);
        sysctl_register_oid(&sysctl__debug_phxgamma_stat);
        sysctl_register_oid(&sysctl__debug_phxpipe0); sysctl_register_oid(&sysctl__debug_phxpipe0_enable);   // b99
        sysctl_register_oid(&sysctl__debug_phxpipe0_mode); sysctl_register_oid(&sysctl__debug_phxpipe0_stat);
        sysctl_register_oid(&sysctl__debug_phxpipe0_plan); sysctl_register_oid(&sysctl__debug_phxpipe0_dead);
        sysctl_register_oid(&sysctl__debug_phxpipe0_modes); sysctl_register_oid(&sysctl__debug_phxpipe0_uclk);   // b101
        sysctl_register_oid(&sysctl__debug_phxpipe0_solo);   // p0solo
        sysctl_register_oid(&sysctl__debug_phxpipe0_live);   // b123
        sysctl_register_oid(&sysctl__debug_phxhpd); sysctl_register_oid(&sysctl__debug_phxhpd_sense);   // b102
        sysctl_register_oid(&sysctl__debug_phxdpcd); sysctl_register_oid(&sysctl__debug_phxdpcd_stat);   // b112
        sysctl_register_oid(&sysctl__debug_phxc3); sysctl_register_oid(&sysctl__debug_phxc3_stat); sysctl_register_oid(&sysctl__debug_phxpoweron);   // b113
        sysctl_register_oid(&sysctl__debug_phxdplink); sysctl_register_oid(&sysctl__debug_phxdplink_stat);   // b116
        sysctl_register_oid(&sysctl__debug_phxdpstream); sysctl_register_oid(&sysctl__debug_phxdpstream_stat);   // b117
        sysctl_register_oid(&sysctl__debug_phxdpwatch); sysctl_register_oid(&sysctl__debug_phxdpwatch_stat); sysctl_register_oid(&sysctl__debug_phxdpwatch_snap);   // b120, b122
        sysctl_register_oid(&sysctl__debug_phxhotplug); sysctl_register_oid(&sysctl__debug_phxhotplug_stat);
    }
    if (type != PHXN_UC_TYPE)
        return kIOReturnBadArgument;
    IOAccelPhxNativeClient *client = OSTypeAlloc(IOAccelPhxNativeClient);
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type) || !client->attach(this)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnExclusiveAccess;
    }
    *handler = client;
    return kIOReturnSuccess;
}

// ---- user client ----------------------------------------------------------------------------------------

bool IOAccelPhxNativeClient::initWithTask(task_t owningTask, void *securityID, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, securityID, type))
        return false;
    privileged = clientHasPrivilege(owningTask, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    task = owningTask;
    lock = IOLockAlloc();
    bos = static_cast<Bo *>(IOMallocZero(sizeof(Bo) * PHXN_MAX_BOS));
    maps = static_cast<Map *>(IOMallocZero(sizeof(Map) * kMaxMaps));
    return lock && bos && maps;
}

bool IOAccelPhxNativeClient::start(IOService *provider) {
    owner = OSDynamicCast(PhxBringup, provider);
    if (!owner)
        return false;
    // E9: several processes at once, each in its own GPUVM context (vmidAlloc); the first one still marks the session
    if (owner->claimSession())
        sessionClaimed = true;
    return IOUserClient::start(provider);
}

void IOAccelPhxNativeClient::freeAll() {
    if (!bos)
        return;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++)
        if (bos[h].md) {
            const Bo &b = bos[h];
            OSAddAtomic64(-static_cast<SInt64>(b.size), &gBoBytes[b.host ? 3 : b.domain == 2 ? 2 : (b.bits & PHXN_PLACED_HI_POOL) ? 1 : 0]);
            boRelease(bos[h]);
        }
}

IOReturn IOAccelPhxNativeClient::clientClose() {
    if (owner && owner->scanHeldBy(this))   // the panel back on the console before this client's BOs go
        owner->scanRelease(this, nullptr);
    // the GPU must be past this client's work before its pages go back to the system: wait first, free after
    const bool idle = !vmid || waitGpuPastLast("close");
    if (lock)
        IOLockLock(lock);
    if (idle)
        freeAll();
    else
        leaked = true;   // BOs stay wired and mapped for the rest of the boot
    if (lock)
        IOLockUnlock(lock);
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    if (sessionClaimed && owner) {
        owner->releaseSession();
        sessionClaimed = false;
    }
    terminate();
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::free() {
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    if (!leaked)
        freeAll();
    if (sessionClaimed && owner)
        owner->releaseSession();
    if (bos)
        IOFree(bos, sizeof(Bo) * PHXN_MAX_BOS);
    if (maps)
        IOFree(maps, sizeof(Map) * kMaxMaps);
    if (lock)
        IOLockFree(lock);
    IOUserClient::free();
}

IOAccelPhxNativeClient::Bo *IOAccelPhxNativeClient::findBo(uint32_t h) {
    if (h == 0 || h >= PHXN_MAX_BOS || !bos[h].md || bos[h].freeSeq)
        return nullptr;
    return &bos[h];
}

IOReturn IOAccelPhxNativeClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    IOLockLock(lock);
    Bo *b = findBo(type);
    if (!b) {
        IOLockUnlock(lock);
        return kIOReturnNotFound;
    }
    b->md->retain();
    *memory = b->md;
    *options = 0;
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}

// Exact shape check, as in the contract: scalar counts and struct sizes must match.
static bool shape(const IOExternalMethodArguments *a, uint32_t sin, uint32_t sout, uint32_t stin, uint32_t stout) {
    return a->scalarInputCount == sin && a->scalarOutputCount == sout && a->structureInputSize == stin &&
           a->structureOutputSize == stout && !a->structureInputDescriptor && !a->structureOutputDescriptor;
}

// b95: WindowServer's composite marks for PhxAccel (PhxNativeABI.h PHXN_SEL_SURFGPUMARK). Outside the client lock: it
// touches no client state and only forwards to NVAccel's nvSurfVram, which takes its own short lock.
#include "../phxaccel/nvrm_surfshare_abi.h"
IOReturn IOAccelPhxNativeClient::doSurfGpuMark(IOExternalMethodArguments *a) {
    if (!shape(a, 2, 1, 0, 0)) return kIOReturnBadArgument;
    a->scalarOutput[0] = NVRM_SS_OFF;
    const uint32_t plane = static_cast<uint32_t>(a->scalarInput[1]);
    if (!(plane & NVRM_SS_PLANE_GPU_MARK) || (plane & ~(NVRM_SS_PLANE_GPU_MARK | 1u))) return kIOReturnSuccess;
    static IOService *accel;   // PhxAccel's accelerator never unloads once matched
    if (!accel) {
        // b96: its class is NMXAccel (nmx_accel_names.h renames NVAccel); b95 matched "NVAccel" and found nothing
        IOService *s = IOService::copyMatchingService(IOService::serviceMatching("NMXAccel"));
        if (!s) return kIOReturnSuccess;
        if (!OSCompareAndSwapPtr(nullptr, s, &accel)) s->release();
    }
    const OSSymbol *fn = OSSymbol::withCStringNoCopy("nvSurfVram");
    if (!fn) return kIOReturnSuccess;
    NVRMSurfShareEsc e = {};
    e.version = NVRM_SURFSHARE_VERSION; e.surfaceID = static_cast<uint32_t>(a->scalarInput[0]); e.plane = plane;
    e.status = NVRM_SS_OFF;
    const IOReturn r = accel->callPlatformFunction(fn, false, &e, current_task(), reinterpret_cast<void *>(static_cast<uintptr_t>(NVRM_ESC_SURF_DIRTY)), nullptr);
    fn->release();
    a->scalarOutput[0] = r == kIOReturnSuccess ? e.status : NVRM_SS_OFF;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                IOExternalMethodDispatch *, OSObject *, void *) {
    if (selector == PHXN_SEL_SURFGPUMARK) return hello ? doSurfGpuMark(a) : kIOReturnNotReady;
    if (selector == PHXN_SEL_WAITSEQ) {
        const uint64_t w0 = phxNowNs();
        const IOReturn wr = doWaitSeq(a);
        phxCallNote(66, 0, phxNowNs() - w0);
        return wr;
    }

    const uint64_t t0 = phxNowNs();
    IOLockLock(lock);
    const uint64_t t1 = phxNowNs();
    reap();
    IOReturn r;
    if (selector != PHXN_SEL_HELLO && !hello) {
        r = kIOReturnNotReady;
    } else {
        switch (selector) {
        case PHXN_SEL_HELLO:     r = doHello(a); break;
        case PHXN_SEL_QUERYINFO: r = doQueryInfo(a); break;
        case PHXN_SEL_READREGS:  r = doReadRegs(a); break;
        case PHXN_SEL_BOCREATE:  r = doBoCreate(a); break;
        case PHXN_SEL_BOFREE:    r = doBoFree(a); break;
        case PHXN_SEL_BOIMPORTHOST: r = doBoImportHost(a); break;
        case PHXN_SEL_GEMVA:     r = doGemVa(a); break;
        case PHXN_SEL_CTX:       r = doCtx(a); break;
        case PHXN_SEL_SUBMIT:    r = doSubmit(a); break;
        case PHXN_SEL_DBG_SMU:   r = doDbgSmu(a); break;
        case PHXN_SEL_DBG_PSP_RING: r = doDbgPspRing(a); break;
        case PHXN_SEL_DBG_VRAM_READ: r = doDbgVramRead(a); break;
        case PHXN_SEL_DBG_PSP_SUBMIT: r = doDbgPspSubmit(a); break;
        case PHXN_SEL_DBG_FW_STAGE: r = doDbgFwStage(a); break;
        case PHXN_SEL_DBG_PSP_LOADIPFW: r = doDbgLoadIpFw(a); break;
        case PHXN_SEL_DBG_PSP_CTL: r = doDbgPspCtl(a); break;
        case PHXN_SEL_DBG_PSP_RAW: r = doDbgPspRaw(a); break;
        case PHXN_SEL_DBG_WRITE_REG: r = doDbgWriteReg(a); break;
        case PHXN_SEL_DBG_VRAM_WRITE: r = doDbgVramWrite(a); break;
        case PHXN_SEL_DBG_DOORBELL: r = doDbgDoorbell(a); break;
        case PHXN_SEL_DBG_SYSMEM: r = doDbgSysMem(a); break;
        case PHXN_SEL_DBG_ADOPTQ: r = doDbgAdoptQueue(a); break;
        case PHXN_SEL_DBG_CLAIM: r = doDbgClaim(a); break;
        case PHXN_SEL_SCAN_QUERY: case PHXN_SEL_SCAN_ACQUIRE: case PHXN_SEL_SCAN_REGISTER:
        case PHXN_SEL_SCAN_PRESENT: case PHXN_SEL_SCAN_STATUS: case PHXN_SEL_SCAN_RELEASE:
            r = doScan(selector, a); break;
        default:                 r = kIOReturnBadArgument; break;
        }
    }
    const uint64_t t2 = phxNowNs();
    IOLockUnlock(lock);
    phxCallNote(selector < 64 ? selector : 63, t1 - t0, t2 - t1);
    return r;
}

IOReturn IOAccelPhxNativeClient::doHello(IOExternalMethodArguments *a) {
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    if (a->scalarInput[1] != 0 && a->scalarInput[1] != PHXN_HELLO_F_MINOR)
        return kIOReturnBadArgument;
    if (a->scalarInput[0] != PHXN_ABI_VERSION)
        return kIOReturnUnsupported;
    hello = true;
    a->scalarOutput[0] = PHXN_ABI_VERSION | (a->scalarInput[1] ? static_cast<uint64_t>(PHXN_ABI_MINOR) << 16 : 0);
    a->scalarOutput[1] = PHX_KEXT_BUILD;
    a->scalarOutput[2] = PHXN_MAX_IBS;
    a->scalarOutput[3] = PHXN_VMID;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doQueryInfo(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, 0, sizeof(phxn_info)))
        return kIOReturnBadArgument;
    phxn_info inf;
    bzero(&inf, sizeof(inf));
    inf.abi_version = PHXN_ABI_VERSION;
    inf.kext_build = PHX_KEXT_BUILD;
    inf.reserved[2] = PHXN_ABI_MINOR;
    inf.flags = PHXN_INFO_S1B_POSITIVE | PHXN_INFO_NATIVE_BOOT | (owner->vcnSubmitReady() ? PHXN_INFO_VCN : 0u);
    inf.vmid = PHXN_VMID;
    inf.gb_addr_config = kGbAddrConfig;
    inf.gc_version = kGcVersion;
    inf.ring_size_dw = 4096;
    inf.max_ibs = PHXN_MAX_IBS;
    inf.vram_vis_total = kVisTotal;
    inf.vram_vis_free = kVisTotal - visUsed;
    inf.vram_hi_total = kHiTotal;
    inf.vram_hi_free = kHiTotal - hiUsed;
    inf.gtt_cap = kGttCap;
    inf.gtt_used = gttUsed;
    inf.gtt_max_bo = kGttMaxBo;
    inf.seq_emitted = emitted;
    inf.seq_retired = owner->gpuReady() ? owner->gpuRetired() : emitted;
    inf.va_low_first = 0x10000;
    inf.va_low_last = 0x7FFFFFFFFFFFull;
    inf.va_high_first = 0xFFFF800000000000ull;
    inf.va_high_last = 0xFFFFFFFFFFFFFFFFull;
    inf.max_bos = PHXN_MAX_BOS;
    inf.fence_slots = PHXN_FENCE_SLOTS;
    bcopy(&inf, a->structureOutput, sizeof(inf));
    return kIOReturnSuccess;
}

static constexpr uint64_t kGoldenTscUpper = 0x16a2d;
IOReturn IOAccelPhxNativeClient::doReadRegs(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 3 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t count = a->scalarInput[1];
    if (count < 1 || count > 16 || a->scalarInput[2] != 0xFFFFFFFFull || a->structureOutputSize != count * 4 ||
        a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    // SMUIO's GOLDEN_TSC (upper 0x16a2d, lower 0x16a2e on Phoenix, SMUIO 13.0.7 base 0x16a00) is the 100 MHz counter the
    // CP stamps command buffers with: every client may read it (Mesa's AMDGPU_INFO_TIMESTAMP, which calibrates GPU
    // times against the host's; without it Metal's GPUStartTime/GPUEndTime were 0 in every app).
    const uint64_t first = a->scalarInput[0];
    const bool tsc = first >= kGoldenTscUpper && first + count <= kGoldenTscUpper + 2;
    if (!privileged && !tsc)
        return kIOReturnNotPrivileged;
    return owner->readRegs(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::boRefused(int cause, uint64_t size, uint64_t domains, uint64_t flags, IOReturn r) {
    OSIncrementAtomic(&gBoFails[cause]);
    uint32_t live = 0;
    for (uint32_t i = 1; i < PHXN_MAX_BOS; i++) live += bos[i].md != nullptr;
    char name[32]; proc_selfname(name, sizeof name);
    snprintf(gBoLastFail, sizeof gBoLastFail, "pid %d %s: %llu KiB domains 0x%llx flags 0x%llx cause %d (client vis %llu/%llu MiB, hi %llu MiB, gtt %llu MiB, BOs %u)",
             proc_selfpid(), name, size >> 10, domains, flags, cause, visUsed >> 20, kVisTotal >> 20, hiUsed >> 20, gttUsed >> 20, live);
    return r;
}

IOReturn IOAccelPhxNativeClient::doBoCreate(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 4, sizeof(phxn_gem_create_in), 0))
        return kIOReturnBadArgument;
    const phxn_gem_create_in *in = static_cast<const phxn_gem_create_in *>(a->structureInput);
    uint64_t size = in->bo_size;
    if (size == 0)
        return kIOReturnBadArgument;
    size = (size + 4095) & ~4095ull;
    uint64_t align = in->alignment;
    if (align && ((align & (align - 1)) || align > (2ull << 20)))
        return kIOReturnBadArgument;
    if (align < 4096)
        align = 4096;
    const uint64_t d = in->domains, f = in->domain_flags;
    if (d == 0 || (d & ~static_cast<uint64_t>(PHX_AMDGPU_GEM_DOMAIN_GTT | PHX_AMDGPU_GEM_DOMAIN_VRAM)))
        return kIOReturnUnsupported;
    if (f & (PHX_AMDGPU_GEM_CREATE_ENCRYPTED | PHX_AMDGPU_GEM_CREATE_CP_MQD_GFX9 | PHX_AMDGPU_GEM_CREATE_PREEMPTIBLE))
        return kIOReturnUnsupported;

    uint32_t domain, bits;
    uint64_t *pool;
    if (d & PHX_AMDGPU_GEM_DOMAIN_VRAM) {
        if (visUsed + size <= kVisTotal) {
            domain = 4;
            bits = PHXN_PLACED_CPU_MAPPABLE | PHXN_PLACED_ZEROED;
            pool = &visUsed;
        } else if ((f & PHX_AMDGPU_GEM_CREATE_NO_CPU_ACCESS) && !(f & PHX_AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) &&
                   hiUsed + size <= kHiTotal) {
            domain = 4;
            bits = PHXN_PLACED_HI_POOL | PHXN_PLACED_ZEROED;
            pool = &hiUsed;
        } else if (size <= kGttMaxBo && gttUsed + size <= kGttCap) {
            // b70: the visible budget is full and the hi pool refuses CPU access: place it as GTT (every BO is system
            // memory here, so the bytes are the same) instead of refusing. amdgpu would evict or fall back too; refusing
            // made RADV fail allocations in processes that already hold 256 MiB visible (Chrome's GPU process: AV1 decode
            // contexts, then a teardown trap). debug.phxvm counts these.
            OSIncrementAtomic(&gBoVisToGtt);
            domain = 2;
            bits = PHXN_PLACED_ZEROED;
            pool = &gttUsed;
        } else {
            return boRefused((f & PHX_AMDGPU_GEM_CREATE_NO_CPU_ACCESS) ? 1 : 0, size, d, f, kIOReturnNoMemory);
        }
    } else {
        if (size > kGttMaxBo || gttUsed + size > kGttCap)
            return boRefused(2, size, d, f, kIOReturnNoMemory);
        domain = 2;
        bits = PHXN_PLACED_ZEROED;
        pool = &gttUsed;
    }

    uint32_t h = 1;
    while (h < PHXN_MAX_BOS && bos[h].md)
        h++;
    if (h >= PHXN_MAX_BOS)
        return boRefused(3, size, d, f, kIOReturnNoResources);

    // Page alignment only: the GPU maps BOs page by page, and the client picks the VA (GEMVA) with its own alignment.
    // Handed to IOBufferMemoryDescriptor, RADV's 64 KiB..2 MiB alignment made every allocation PHYSICALLY CONTIGUOUS
    // (XNU allocates an alignment above the page size with kmem_alloc_contig): once memory was fragmented, every
    // buffer of 8 MiB or more failed after a 400 ms search, and each Metal app spent ~0.8 s at launch (2026-10-21).
    (void)align;
    const uint64_t t0 = phxNowNs();
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, size, PAGE_SIZE);
    if (!md)
        return boRefused(4, size, d, f, kIOReturnNoMemory);
    const uint64_t t1 = phxNowNs();
    int zmode = gBoZero;
    if (zmode == 3 && (gBoN & 63) == 0)
        zmode = 2;
    if (zmode == 2) {
        const uint64_t *w = static_cast<const uint64_t *>(md->getBytesNoCopy());
        for (uint64_t pg = 0; pg < size / 4096; pg++) {
            uint64_t acc = 0;
            for (uint32_t k = 0; k < 512; k++)
                acc |= w[pg * 512 + k];
            if (acc) {
                OSIncrementAtomic64(&gBoDirtyPages);
                if (gBoZero == 3)
                    gBoZero = 1;
            }
        }
        OSAddAtomic64(static_cast<SInt64>(size / 4096), &gBoCheckedPages);
    }
    if (zmode == 1 || zmode == 2)
        bzero(md->getBytesNoCopy(), size);
    OSIncrementAtomic64(&gBoN);
    OSAddAtomic64(static_cast<SInt64>(size), &gBoBytesAll);
    OSAddAtomic64(static_cast<SInt64>(t1 - t0), &gBoAllocNs);
    OSAddAtomic64(static_cast<SInt64>(phxNowNs() - t1), &gBoZeroNs);
    *pool += size;
    OSAddAtomic64(static_cast<SInt64>(size), &gBoBytes[pool == &visUsed ? 0 : pool == &hiUsed ? 1 : 2]);
    bos[h] = Bo{md, size, domain, bits, static_cast<uint8_t *>(md->getBytesNoCopy()), false};
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = domain;
    a->scalarOutput[3] = bits;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doBoFree(IOExternalMethodArguments *a) {
    if (!shape(a, 1, 0, 0, 0))
        return kIOReturnBadArgument;
    uint32_t h = static_cast<uint32_t>(a->scalarInput[0]);
    Bo *b = findBo(h);
    if (!b)
        return kIOReturnNotFound;
    // the GPU may still be running work that reads this BO, and a mapping left behind would keep its freed pages
    // reachable. b52: unless it is a scanout slot, the release waits for this client's last submission off the lock:
    // it is recorded here and finished by reap() once that seqno retired (handle, pages and maps stay until then)
    if (!b->scanSlot && vmid && owner->gpuRetired() < lastSeq) {
        b->freeSeq = lastSeq;
        deferred++;
        OSIncrementAtomic(&gDeferFrees);
        if ((SInt32)deferred > gDeferPendingMax) gDeferPendingMax = (SInt32)deferred;
        return kIOReturnSuccess;
    }
    if (!waitGpuPastLast("BO free"))
        return kIOReturnTimeout;   // kept whole (mapped, pages wired): a leak beats the GPU reading freed pages
    if (b->scanSlot && owner->scanHeldBy(this)) {   // its slot may be on screen: the console goes back up first
        owner->scanRelease(this, nullptr);
        scanUnbind();
    }
    finishFree(h);
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::finishFree(uint32_t h) {
    Bo *b = &bos[h];
    for (int i = 0; i < kMaxMaps; i++)
        if (maps[i].used && maps[i].handle == h) {
            if (maps[i].unmapSeq) { maps[i].unmapSeq = 0; deferred--; }
            maps[i].used = false;
            if (vmid)
                owner->vmUnmap(vmid, maps[i].va, maps[i].size);
        }
    const int pi = b->host ? 3 : b->domain == 2 ? 2 : (b->bits & PHXN_PLACED_HI_POOL) ? 1 : 0;
    OSAddAtomic64(-static_cast<SInt64>(b->size), &gBoBytes[pi]);
    if (b->host)
        hostUsed -= b->size;
    else if (b->domain == 2)
        gttUsed -= b->size;
    else if (b->bits & PHXN_PLACED_HI_POOL)
        hiUsed -= b->size;
    else
        visUsed -= b->size;
    boRelease(*b);
}

void IOAccelPhxNativeClient::reap() {
    if (!deferred || !owner)
        return;
    const uint64_t r = owner->gpuRetired();
    if (r == reapedAt)
        return;
    reapedAt = r;
    for (int i = 0; i < kMaxMaps && deferred; i++)
        if (maps[i].used && maps[i].unmapSeq && maps[i].unmapSeq <= r) {
            maps[i].unmapSeq = 0;
            maps[i].used = false;
            deferred--;
            if (vmid)
                owner->vmUnmap(vmid, maps[i].va, maps[i].size);
        }
    for (uint32_t h = 1; h < PHXN_MAX_BOS && deferred; h++)
        if (bos[h].md && bos[h].freeSeq && bos[h].freeSeq <= r) {
            deferred--;
            finishFree(h);
        }
}

bool IOAccelPhxNativeClient::waitRetired(uint64_t s, const char *why) {
    for (int i = 0; i < 20000 && owner->gpuRetired() < s; i++)
        IODelay(100);
    if (owner->gpuRetired() >= s)
        return true;
    OSIncrementAtomic(&gDeferWaitTimeouts);
    char n[20] = {0}; proc_selfname(n, sizeof n);
    IOLog("PhxBringup: %s: pid %d %s: GPU at %llu, deferred release waits for %llu after 2 s\n", why, proc_selfpid(), n, owner->gpuRetired(), s);
    return false;
}

// A new map over a VA the client already released (an unmap or a BO free still waiting for the GPU): the old PTEs may
// still be read by work in flight, so that one release is finished now, waiting for its seqno as the old code did.
bool IOAccelPhxNativeClient::settleOverlaps(uint64_t va, uint64_t size) {
    for (int i = 0; i < kMaxMaps; i++) {
        Map &m = maps[i];
        if (!m.used || !(va < m.va + m.size && m.va < va + size))
            continue;
        const uint64_t s = m.unmapSeq ? m.unmapSeq : bos[m.handle].freeSeq;
        if (!s)
            continue;   // a live map: the caller refuses the overlap
        OSIncrementAtomic(&gDeferOverlapWaits);
        if (!waitRetired(s, "remap"))
            return false;
        if (m.unmapSeq) {
            m.unmapSeq = 0;
            m.used = false;
            deferred--;
            if (vmid)
                owner->vmUnmap(vmid, m.va, m.size);
        } else {
            deferred--;
            finishFree(m.handle);
        }
    }
    return true;
}

void IOAccelPhxNativeClient::boRelease(Bo &b) {
    if (b.host)
        b.md->complete();
    b.md->release();
    b = Bo{};
}

// PHXN_SEL_BOIMPORTHOST (ABI 1.9, VK_EXT_external_memory_host): a BO over the caller's own pages, wired for as long as
// it lives, GPU-mapped later through GemVa like any other. The kext reported ABI 1.12 without it, so every import
// RADV made (no-copy buffers, IOSurface textures over their surfaces' pages) failed and fell back to copies
// (2026-10-20). in: [0] host VA [1] size (both page-aligned) [2] flags 0 [3] GPU VA 0; out: [0] handle [1] size
// [2] GPU VA 0 [3] placement bits.
IOReturn IOAccelPhxNativeClient::doBoImportHost(IOExternalMethodArguments *a) {
    if (!shape(a, 4, 4, 0, 0))
        return kIOReturnBadArgument;
    const uint64_t va = a->scalarInput[0], size = a->scalarInput[1];
    if (!va || !size || (va & 4095) || (size & 4095) || a->scalarInput[2] || a->scalarInput[3] || !task)
        return kIOReturnBadArgument;
    if (size > kHostMaxBo || hostUsed + size > kHostCap)
        return kIOReturnNoMemory;
    uint32_t h = 1;
    while (h < PHXN_MAX_BOS && bos[h].md)
        h++;
    if (h >= PHXN_MAX_BOS)
        return kIOReturnNoResources;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(va, size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnNoMemory;
    const IOReturn r = md->prepare();
    if (r != kIOReturnSuccess) {
        md->release();
        return r;
    }
    hostUsed += size;
    OSAddAtomic64(static_cast<SInt64>(size), &gBoBytes[3]);
    bos[h] = Bo{md, size, 2, 0, nullptr, true};
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = 0;
    a->scalarOutput[3] = 0;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doGemVa(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_gem_va), 0))
        return kIOReturnBadArgument;
    const phxn_gem_va *v = static_cast<const phxn_gem_va *>(a->structureInput);
    if (v->_pad || v->vm_timeline_point || v->vm_timeline_syncobj_out || v->num_syncobj_handles ||
        v->input_fence_syncobj_handles)
        return kIOReturnBadArgument;
    if (v->handle == 0 || (v->flags & PHX_AMDGPU_VM_PAGE_PRT)) {
        mapComplain(v->handle ? "refused (PRT)" : "refused (no BO)", v->va_address, v->map_size, v->operation, v->flags, kIOReturnUnsupported);
        return kIOReturnUnsupported; }
    if (v->operation == PHX_AMDGPU_VA_OP_CLEAR || v->operation == PHX_AMDGPU_VA_OP_REPLACE) {
        mapComplain(v->operation == PHX_AMDGPU_VA_OP_CLEAR ? "refused (CLEAR)" : "refused (REPLACE)", v->va_address, v->map_size, v->operation, v->flags, kIOReturnUnsupported);
        return kIOReturnUnsupported; }
    if (v->operation != PHX_AMDGPU_VA_OP_MAP && v->operation != PHX_AMDGPU_VA_OP_UNMAP)
        return kIOReturnBadArgument;
    Bo *b = findBo(v->handle);
    if (!b)
        return kIOReturnNotFound;
    if (!vaCanonical(v->va_address))
        return kIOReturnBadArgument;
    const uint64_t va = v->va_address & kVaMask;
    if (((v->va_address | v->offset_in_bo | v->map_size) & 4095) || v->map_size == 0)
        return kIOReturnBadArgument;
    if (va < 0x10000 || va + v->map_size > (1ull << 48))
        return kIOReturnBadArgument;
    if (v->offset_in_bo + v->map_size > b->size)
        return kIOReturnBadArgument;

    if (v->operation == PHX_AMDGPU_VA_OP_MAP) {
        if (deferred && !settleOverlaps(va, v->map_size))
            return kIOReturnTimeout;
        int freei = -1;
        for (int i = 0; i < kMaxMaps; i++) {
            if (!maps[i].used) {
                if (freei < 0)
                    freei = i;
                continue;
            }
            if (va < maps[i].va + maps[i].size && maps[i].va < va + v->map_size) {
                mapComplain("refused (overlaps a live map)", v->va_address, v->map_size, v->operation, v->flags, kIOReturnBadArgument);
                return kIOReturnBadArgument; }
        }
        if (freei < 0) {
            mapComplain("refused (map table full)", v->va_address, v->map_size, v->operation, v->flags, kIOReturnNoMemory);
            return kIOReturnNoMemory; }
        maps[freei] = Map{true, v->handle, va, v->map_size, v->offset_in_bo, v->flags};
        if (owner->gpuReady()) {
            const IOReturn r = !ensureVmid() ? kIOReturnNoResources
                             : b->scanSlot ? owner->vmMapVram(vmid, va, b->scanVram + v->offset_in_bo, v->map_size)
                             : owner->vmMap(vmid, va, b->md, v->offset_in_bo, v->map_size);
            if (r != kIOReturnSuccess) {
                maps[freei].used = false;
                mapComplain("FAILED", v->va_address, v->map_size, v->operation, v->flags, r);
                return r;
            }
        }
        return kIOReturnSuccess;
    }
    for (int i = 0; i < kMaxMaps; i++) {
        if (maps[i].used && !maps[i].unmapSeq && maps[i].handle == v->handle && maps[i].va == va && maps[i].size == v->map_size) {
            if (vmid && owner->gpuRetired() < lastSeq) {   // b52: cleared by reap() once the GPU is past it
                maps[i].unmapSeq = lastSeq;
                deferred++;
                OSIncrementAtomic(&gDeferUnmaps);
                if ((SInt32)deferred > gDeferPendingMax) gDeferPendingMax = (SInt32)deferred;
                return kIOReturnSuccess;
            }
            maps[i].used = false;
            if (vmid)
                owner->vmUnmap(vmid, va, v->map_size);
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn IOAccelPhxNativeClient::doCtx(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_ctx), sizeof(phxn_ctx)))
        return kIOReturnBadArgument;
    phxn_ctx in, out;
    bcopy(a->structureInput, &in, sizeof(in));
    bzero(&out, sizeof(out));
    if (in.op == PHX_AMDGPU_CTX_OP_ALLOC_CTX) {
        uint32_t id = 1;
        while (id <= PHXN_MAX_CTX && ctx[id])
            id++;
        if (id > PHXN_MAX_CTX)
            return kIOReturnNoResources;
        ctx[id] = true;
        out.op = id;
    } else if (in.op == PHX_AMDGPU_CTX_OP_FREE_CTX) {
        if (in.ctx_id == 0 || in.ctx_id > PHXN_MAX_CTX || !ctx[in.ctx_id])
            return kIOReturnNotFound;
        ctx[in.ctx_id] = false;
    } else if (in.op != PHX_AMDGPU_CTX_OP_QUERY_STATE2) {
        return kIOReturnBadArgument;
    }
    bcopy(&out, a->structureOutput, sizeof(out));
    return kIOReturnSuccess;
}

bool IOAccelPhxNativeClient::ibCovered(uint64_t vaCanon, uint64_t bytes) {
    uint64_t cur = vaCanon & kVaMask;
    const uint64_t end = cur + bytes;
    while (cur < end) {
        bool ok = false;
        for (int i = 0; i < kMaxMaps; i++) {
            const Map &mp = maps[i];
            if (mp.used && !mp.unmapSeq && !bos[mp.handle].freeSeq && (mp.flags & PHX_AMDGPU_VM_PAGE_EXECUTABLE) && cur >= mp.va && cur < mp.va + mp.size) {
                cur = mp.va + mp.size < end ? mp.va + mp.size : end;
                ok = true;
                break;
            }
        }
        if (!ok)
            return false;
    }
    return true;
}

IOReturn IOAccelPhxNativeClient::doSubmit(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 0 || a->scalarOutputCount != 1 || a->structureOutputSize != 0 ||
        a->structureInputDescriptor || a->structureInputSize < 64 ||
        a->structureInputSize > 32 + 32 * PHXN_MAX_IBS)
        return kIOReturnBadArgument;
    const uint8_t *in = static_cast<const uint8_t *>(a->structureInput);
    phxn_cs_in h;
    bcopy(in, &h, sizeof(h));
    if (h.abi != PHXN_ABI_VERSION || h.num_ibs < 1 || h.num_ibs > PHXN_MAX_IBS ||
        a->structureInputSize != sizeof(h) + h.num_ibs * sizeof(phxn_cs_ib) || h.reserved != 0 ||
        (h.flags & ~PHXN_CS_HAS_FENCE))
        return kIOReturnBadArgument;
    if (h.ctx_id == 0 || h.ctx_id > PHXN_MAX_CTX || !ctx[h.ctx_id])
        return kIOReturnNotFound;

    for (uint32_t i = 0; i < h.num_ibs; i++) {
        phxn_cs_ib ib;
        bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
        if (ib._pad || (ib.ip_type != PHX_AMDGPU_HW_IP_GFX && ib.ip_type != PHX_AMDGPU_HW_IP_COMPUTE &&
                        ib.ip_type != PHX_AMDGPU_HW_IP_VCN_ENC) || ib.ip_instance != 0 || ib.ring != 0)
            return kIOReturnBadArgument;
        if (ib.ip_type == PHX_AMDGPU_HW_IP_VCN_ENC && !owner->vcnSubmitReady())
            return kIOReturnNotReady;   // b67: VCN IBs need debug.phxvcn=1 and debug.phxmmvm=1
        if (ib.ib_bytes == 0 || (ib.ib_bytes & 3) || ib.ib_bytes > 0xFFFFFu * 4)
            return kIOReturnBadArgument;
        if (!vaCanonical(ib.va_start) || (ib.va_start & 3))
            return kIOReturnBadArgument;
        if (ib.flags & (PHX_AMDGPU_IB_FLAG_CE | PHX_AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID | PHX_AMDGPU_IB_FLAGS_SECURE |
                        PHX_AMDGPU_IB_FLAG_EMIT_MEM_SYNC))
            return kIOReturnUnsupported;
        if (ib.flags & ~static_cast<uint32_t>(PHX_AMDGPU_IB_FLAG_PREAMBLE | PHX_AMDGPU_IB_FLAG_PREEMPT |
                                              PHX_AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE))
            return kIOReturnBadArgument;
        if (!ibCovered(ib.va_start, ib.ib_bytes))
            return kIOReturnBadArgument;
    }

    Bo *fb = nullptr;
    if (h.flags & PHXN_CS_HAS_FENCE) {
        fb = findBo(h.fence_handle);
        if (!fb)
            return kIOReturnNotFound;
        if (!fb->kva || (h.fence_offset & 7) || static_cast<uint64_t>(h.fence_offset) + 8 > fb->size)
            return kIOReturnBadArgument;   // the kernel writes the fence: not into a host import
    } else if (h.fence_handle || h.fence_offset) {
        return kIOReturnBadArgument;
    }

    if (owner->gpuReady()) {
        // E8: real execution on the adopted compute queue, IBs run in VMID 8
        uint64_t va[PHXN_MAX_IBS];
        uint32_t dw[PHXN_MAX_IBS];
        for (uint32_t i = 0; i < h.num_ibs; i++) {
            phxn_cs_ib ib;
            bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
            va[i] = ib.va_start & kVaMask;
            dw[i] = ib.ib_bytes / 4;
        }
        // log the IBs RADV sends (first 96 dwords each) for bring-up: a client's first submissions only, as every
        // WindowServer frame dumped its IBs and the log held nothing else (2026-10-20)
        if (owner->debugEnabled() && emitted < 4) {
            for (uint32_t i = 0; i < h.num_ibs; i++) {
                for (int m = 0; m < kMaxMaps; m++) {
                    if (!maps[m].used || va[i] < maps[m].va || va[i] >= maps[m].va + maps[m].size)
                        continue;
                    Bo *bo = findBo(maps[m].handle);
                    if (!bo || !bo->kva)
                        break;
                    const uint32_t *d = reinterpret_cast<const uint32_t *>(bo->kva + maps[m].off + (va[i] - maps[m].va));
                    const uint32_t cnt = dw[i] < 96 ? dw[i] : 96;
                    for (uint32_t j = 0; j < cnt; j += 8)
                        IOLog("PhxIB %u.%u +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)(emitted + 1), i, j,
                              d[j], j + 1 < cnt ? d[j + 1] : 0, j + 2 < cnt ? d[j + 2] : 0, j + 3 < cnt ? d[j + 3] : 0,
                              j + 4 < cnt ? d[j + 4] : 0, j + 5 < cnt ? d[j + 5] : 0, j + 6 < cnt ? d[j + 6] : 0,
                              j + 7 < cnt ? d[j + 7] : 0);
                    break;
                }
            }
        }
        flushBos();
        uint64_t seq = 0;
        phxn_cs_ib ib0;
        bcopy(in + sizeof(h), &ib0, sizeof(ib0));
        if (!ensureVmid())
            return kIOReturnNoResources;
        for (uint32_t i = 1; i < h.num_ibs; i++) {   // one engine per submission
            phxn_cs_ib ibi;
            bcopy(in + sizeof(h) + i * sizeof(ibi), &ibi, sizeof(ibi));
            if ((ibi.ip_type == PHX_AMDGPU_HW_IP_VCN_ENC) != (ib0.ip_type == PHX_AMDGPU_HW_IP_VCN_ENC))
                return kIOReturnBadArgument;
        }
        const IOReturn r = ib0.ip_type == PHX_AMDGPU_HW_IP_VCN_ENC
                               ? owner->gpuSubmitVcn(vmid, va, dw, h.num_ibs, &seq)
                               : owner->gpuSubmit(vmid, ib0.ip_type == PHX_AMDGPU_HW_IP_GFX, va, dw, h.num_ibs, &seq);
        if (r != kIOReturnSuccess)
            return r;
        qmask |= owner->submitQueueBit(ib0.ip_type == PHX_AMDGPU_HW_IP_GFX, ib0.ip_type == PHX_AMDGPU_HW_IP_VCN_ENC);
        lastSeq = seq;
        emitted = seq;   // externalMethod already holds `lock`
        if (fb) {
            pendFenceHandle = h.fence_handle;
            pendFenceOffset = h.fence_offset;
            pendFenceSeq = seq;
        }
        a->scalarOutput[0] = seq;
        return kIOReturnSuccess;
    }
    // E1: no GPU behind the call; the submission retires at once, as in the mock.
    const uint64_t seq = ++emitted;
    if (fb)
        bcopy(&seq, fb->kva + h.fence_offset, sizeof(seq));
    a->scalarOutput[0] = seq;
    return kIOReturnSuccess;
}

