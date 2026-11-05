// SPDX-License-Identifier: MIT
// phx_core.cpp: PhxBringup start/stop (bring-up), MMIO/VRAM register access, the FB layout, HDP flush, sysMem.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

bool PhxBringup::start(IOService *provider) {
    int dummy = 0;
    if (PE_parse_boot_argn("-phxbringupoff", &dummy, sizeof(dummy)))
        return false;
    if (!IOService::start(provider))
        return false;
    gPhx = this;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;
    pci->retain();
    {   // the GPU must be a bus master before it can DMA to/from system memory (page tables, user BOs)
        uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
        setProperty("phx-pci-cmd-before", cmd, 16);
        pci->setBusMasterEnable(true);
        pci->setMemoryEnable(true);
        setProperty("phx-pci-cmd-after", pci->configRead16(kIOPCIConfigCommand), 16);
    }
    mmio = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    // navi31: with Resizable BAR, BAR0 is the whole 20 GiB of VRAM (a 32 GiB window). Everything this kext places
    // in VRAM lives below 160 MiB, so map only the first kFbAperMapMax bytes instead of 32 GiB of kernel VA.
    if (IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)) {
        IOByteCount len = bar0->getLength() < kFbAperMapMax ? bar0->getLength() : kFbAperMapMax;
        if (IODeviceMemory *sub = IODeviceMemory::withSubRange(bar0, 0, len)) {
            fbAper = sub->map();
            sub->release();
        }
        setProperty("phx-bar0-len", static_cast<uint64_t>(bar0->getLength()), 64);
    }
    if (fbAper) {
        fbAperLen = fbAper->getLength();
        fbAperPhys = fbAper->getPhysicalAddress();
    }
    doorbellMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    smuLock = IOLockAlloc();
    gpuLock = IOLockAlloc();
    if (!gWaitSleepLock) gWaitSleepLock = IOLockAlloc();
    uint32_t dbg = 0;
    debug = PE_parse_boot_argn("phxdebug", &dbg, sizeof(dbg)) && dbg == 1;
    if (mmio) {
        phxC3LatchPowerOn(rreg(0x05D89));   // b113: DC_GPIO_GENERIC_A before the boot clears bit 8 (debug.phxpoweron, debug.phxc3 G1)
        readFbLayout();
        // HDP flush window: the NBIO remap register holds the BAR5 byte offset where writing 0 flushes the
        // host data path. Accept it only if it is a plausible dword offset inside our MMIO aperture.
        uint32_t remap = rreg(PHX_NBIO_REMAP_HDP_MEM_FLUSH_CNTL);
        if (remap && remap != 0xFFFFFFFFu && (remap & 3) == 0 && remap + 4 <= mmio->getLength())
            hdpFlushDword = remap >> 2;
        setProperty("phx-hdp-flush-dword", hdpFlushDword, 32);
    }
    clearRing();   // navi31: also sets up fw_buf (needs fbMcBase and fbAperLen from above)
    setProperty("phx-fwbuf-mc", fwBufMc, 64);
    setProperty("phx-build", PHX_KEXT_BUILD, 32);
    setProperty("phx-edid-fn", kOSBooleanTrue);   // b79: PhxAMDFB may call the "phxEdidRead" platform function
    setProperty("phx-edid-req", kOSBooleanTrue);  // b81: ...and ask it for a source (param4 in, phxddc::resolveSource)
    setProperty("phx-pipe2-mode-fn", kOSBooleanTrue);   // b93: PhxAMDFB head 1 may call "phxPipe2SetMode" (PhxSwitchABI.h)
    setProperty("phx-pipe2-gamma-fn", kOSBooleanTrue);  // b98: ...and "phxPipe2SetGamma" (PhxGamma.h; writes only with debug.phxgamma=1)
    setProperty("phx-pipe0-mode-fn", kOSBooleanTrue);   // b101: PhxAMDFB head 0 may call "phxPipe0SetMode" (PhxSwitchABI.h; needs debug.phxpipe0_enable=1)
    setProperty("phx-stage", "E3");
    setProperty("phx-debug", debug);
    setProperty("phx-fb-aper-phys", fbAperPhys, 64);
    setProperty("phx-fb-aper-len", fbAperLen, 64);
    IOLog("PhxBringup: service up (mmio %s, bar0 %s %llu MiB, debug %d)\n", mmio ? "mapped" : "unmapped",
          fbAper ? "mapped" : "unmapped", (unsigned long long)(fbAperLen >> 20), debug);
    registerService();
    return true;
}

void PhxBringup::stop(IOService *provider) {
    phxHotplugStop();   // b102: the hot-plug poll first (it reads MMIO and calls PhxAccel)
    phxDpWatchStop();   // b120: the DP stream watcher (it reads MMIO)
    if (gIhOn)
        ihDisable();
    if (gIrqSrc) {
        gIrqSrc->disable();
        if (gIrqWl)
            gIrqWl->removeEventSource(gIrqSrc);
        OSSafeReleaseNULL(gIrqSrc);
    }
    OSSafeReleaseNULL(gIrqWl);
    if (gIhSpin) { IOSimpleLockFree(gIhSpin); gIhSpin = nullptr; }
    if (gVcnLock) { IOLockFree(gVcnLock); gVcnLock = nullptr; }
    gVcnOn = 0;
    if (gIhPowerNotifier) { gIhPowerNotifier->remove(); gIhPowerNotifier = nullptr; }
    if (gSysctlUp) {
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush); sysctl_unregister_oid(&sysctl__debug_phxwait_spin_us); sysctl_unregister_oid(&sysctl__debug_phxbo_zero); sysctl_unregister_oid(&sysctl__debug_phxgva); sysctl_unregister_oid(&sysctl__debug_phxih); sysctl_unregister_oid(&sysctl__debug_phxih_stat); sysctl_unregister_oid(&sysctl__debug_phxeopirq); sysctl_unregister_oid(&sysctl__debug_phxih_vram); sysctl_unregister_oid(&sysctl__debug_phxirq); sysctl_unregister_oid(&sysctl__debug_phxirq_stat); sysctl_unregister_oid(&sysctl__debug_phxwait_irq); sysctl_unregister_oid(&sysctl__debug_phxwait_stat); sysctl_unregister_oid(&sysctl__debug_phxwait_lat); sysctl_unregister_oid(&sysctl__debug_phxwait_irq_dl_us); sysctl_unregister_oid(&sysctl__debug_phxvcn); sysctl_unregister_oid(&sysctl__debug_phxvcn_stat); sysctl_unregister_oid(&sysctl__debug_phxmmvm); sysctl_unregister_oid(&sysctl__debug_phxmmvm_stat); sysctl_unregister_oid(&sysctl__debug_phxbo_stats); sysctl_unregister_oid(&sysctl__debug_phxwait_step_us); sysctl_unregister_oid(&sysctl__debug_phxwait_scoped); sysctl_unregister_oid(&sysctl__debug_phxwait_scoped_stat); sysctl_unregister_oid(&sysctl__debug_phxscan_latchfix); sysctl_unregister_oid(&sysctl__debug_phxscan_race_stat); sysctl_unregister_oid(&sysctl__debug_phxdal_dead); sysctl_unregister_oid(&sysctl__debug_phxdal_stat);
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_unregister_oid(&sysctl__debug_phxvm);
        sysctl_unregister_oid(&sysctl__debug_phxpt_reclaim);
        sysctl_unregister_oid(&sysctl__debug_phxpt_keep);
        sysctl_unregister_oid(&sysctl__debug_phxpt_stat);
        sysctl_unregister_oid(&sysctl__debug_phxcalls);
        sysctl_unregister_oid(&sysctl__debug_phxpipe2); sysctl_unregister_oid(&sysctl__debug_phxpipe2_stat);   // b82
        sysctl_unregister_oid(&sysctl__debug_phxpipe2_plan); sysctl_unregister_oid(&sysctl__debug_phxdmub_ring);
        sysctl_unregister_oid(&sysctl__debug_phxdmub_dead);
        sysctl_unregister_oid(&sysctl__debug_phxpipe2_head1);   // b87
        sysctl_unregister_oid(&sysctl__debug_phxdmub_trace);   // b88
        sysctl_unregister_oid(&sysctl__debug_phxpipe2_mode);   // b90
        sysctl_unregister_oid(&sysctl__debug_phxpipe2_switch);   // b91
        sysctl_unregister_oid(&sysctl__debug_phxpipe2_switch_stat);
        sysctl_unregister_oid(&sysctl__debug_phxpipe2_phase);
        sysctl_unregister_oid(&sysctl__debug_phxpipe2_det);
        sysctl_unregister_oid(&sysctl__debug_phxscdc);   // b107
        sysctl_unregister_oid(&sysctl__debug_phxdmub_restart); sysctl_unregister_oid(&sysctl__debug_phxdmub_restart_stat);   // b109
        sysctl_unregister_oid(&sysctl__debug_phxdmub_trace_pre);
        sysctl_unregister_oid(&sysctl__debug_phxscdc_fix); sysctl_unregister_oid(&sysctl__debug_phxscdc_fix_stat);
        sysctl_unregister_oid(&sysctl__debug_phxdmub_regtest); sysctl_unregister_oid(&sysctl__debug_phxdmub_regtest_stat);   // b111
        sysctl_unregister_oid(&sysctl__debug_phxgamma); sysctl_unregister_oid(&sysctl__debug_phxgamma_test);   // b98
        sysctl_unregister_oid(&sysctl__debug_phxgamma_stat);
        sysctl_unregister_oid(&sysctl__debug_phxpipe0); sysctl_unregister_oid(&sysctl__debug_phxpipe0_enable);   // b99
        sysctl_unregister_oid(&sysctl__debug_phxpipe0_mode); sysctl_unregister_oid(&sysctl__debug_phxpipe0_stat);
        sysctl_unregister_oid(&sysctl__debug_phxpipe0_plan); sysctl_unregister_oid(&sysctl__debug_phxpipe0_dead);
        sysctl_unregister_oid(&sysctl__debug_phxpipe0_modes); sysctl_unregister_oid(&sysctl__debug_phxpipe0_uclk);   // b101
        sysctl_unregister_oid(&sysctl__debug_phxpipe0_solo);   // p0solo
        sysctl_unregister_oid(&sysctl__debug_phxpipe0_live);   // b123
        sysctl_unregister_oid(&sysctl__debug_phxhpd); sysctl_unregister_oid(&sysctl__debug_phxhpd_sense);   // b102
        sysctl_unregister_oid(&sysctl__debug_phxdpcd); sysctl_unregister_oid(&sysctl__debug_phxdpcd_stat);   // b112
        sysctl_unregister_oid(&sysctl__debug_phxc3); sysctl_unregister_oid(&sysctl__debug_phxc3_stat); sysctl_unregister_oid(&sysctl__debug_phxpoweron);   // b113
        sysctl_unregister_oid(&sysctl__debug_phxdplink); sysctl_unregister_oid(&sysctl__debug_phxdplink_stat);   // b116
        sysctl_unregister_oid(&sysctl__debug_phxdpstream); sysctl_unregister_oid(&sysctl__debug_phxdpstream_stat);   // b117
        sysctl_unregister_oid(&sysctl__debug_phxdpwatch); sysctl_unregister_oid(&sysctl__debug_phxdpwatch_stat); sysctl_unregister_oid(&sysctl__debug_phxdpwatch_snap);   // b120, b122
        sysctl_unregister_oid(&sysctl__debug_phxhotplug); sysctl_unregister_oid(&sysctl__debug_phxhotplug_stat);
        gPhx = nullptr;
        gSysctlUp = 0;
    }
    clearRing();
    if (fbAper) {
        fbAper->release();
        fbAper = nullptr;
    }
    if (sysBuf) {
        sysBuf->release();
        sysBuf = nullptr;
    }
    gq[0].ready = gq[1].ready = gq[2].ready = false;
    for (VmCtx &c : vm)
        c = VmCtx{};   // navi31: chunks are VRAM, nothing to release
    ptPoolNext = 0;
    if (ptShadowMem) {   // b77
        IOFree(ptShadowMem, phxvm::Shadow::bytes(ptShadow.n));
        ptShadowMem = nullptr;
    }
    ptShadow = phxvm::Shadow{};
    ptShadowTried = false;
    for (HwVmid &h : hwv)
        h = HwVmid{};
    if (gpuLock) {
        IOLockFree(gpuLock);
        gpuLock = nullptr;
    }
    if (doorbellMap) {
        doorbellMap->release();
        doorbellMap = nullptr;
    }
    if (mmio) {
        mmio->release();
        mmio = nullptr;
    }
    if (pci) {
        pci->release();
        pci = nullptr;
    }
    if (smuLock) {
        IOLockFree(smuLock);
        smuLock = nullptr;
    }
    IOService::stop(provider);
}

uint32_t PhxBringup::rreg(uint32_t reg) const {
    return reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg];
}

void PhxBringup::wreg(uint32_t reg, uint32_t value) const {
    reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg] = value;
}

// FB location (MC, 16 MiB units, FB_BASE bits 23:0) and the system physical address of the carve-out (FB_OFFSET << 24).
void PhxBringup::readFbLayout() {
    uint32_t base = rreg(PHX_MMMC_VM_FB_LOCATION_BASE) & 0x00FFFFFF;
    uint32_t top = rreg(PHX_MMMC_VM_FB_LOCATION_TOP) & 0x00FFFFFF;
    fbMcBase = static_cast<uint64_t>(base) << 24;
    fbSize = top >= base ? (static_cast<uint64_t>(top - base) + 1) << 24 : 0;
    fbPhys = static_cast<uint64_t>(rreg(PHX_MMMC_VM_FB_OFFSET)) << 24;
    setProperty("phx-fb-mc-base", fbMcBase, 64);
    setProperty("phx-fb-size", fbSize, 64);
    setProperty("phx-fb-phys", fbPhys, 64);
    // About This Mac / System Information name the GPU from the PCI device's "model" and size its memory from
    // "VRAM,totalMB"; without them the 780M read "Display, 15 MB" (the boot framebuffer)
    static const char kModel[] = "AMD Radeon RX 7900 XT";
    if (OSData *m = OSData::withBytes(kModel, sizeof(kModel))) {
        pci->setProperty("model", m);
        m->release();
    }
    if (fbSize)
        pci->setProperty("VRAM,totalMB", fbSize >> 20, 32);
}

void PhxBringup::clearRing() {
    ringMc = ringOff = 0;
    cmdOff = cmdMc = fenceOff = fenceMc = 0;
    fenceCounter = 0;
    tmrOff = tmrMc = tmrSize = 0;
    tmrSetUp = false;
    // navi31: fw_buf does not depend on the ring. On a dGPU the PSP bootloader stage (KDB/SPL/SYS_DRV/SOS through
    // C2PMSG_35/36) needs staged firmware before the sOS — and so the ring — exists, so keep fw_buf usable always.
    fwBufOff = fwBufMc = fwBufSize = 0;
    if (fbMcBase && PHX_VRAM_FWBUF_OFF + PHX_VRAM_FWBUF_SIZE <= fbAperLen) {
        fwBufOff = PHX_VRAM_FWBUF_OFF;
        fwBufMc = fbMcBase + fwBufOff;
        fwBufSize = PHX_VRAM_FWBUF_SIZE;
    }
}

// Drain the host data path so the PSP sees our VRAM writes before we ring the doorbell (C2PMSG_67).
void PhxBringup::hdpFlush() const {
    if (hdpFlushDword)
        wreg(hdpFlushDword, 0);
    (void)rreg(0x0DE3);   // read-back barrier (GRBM_STATUS), as in the reference
}

// Read-only: count dwords from VRAM at byteOff, through the BAR0 aperture. Used by debug selector 102 to
// find the live console framebuffer and a free (all-zero) VRAM region for the PSP structures.
IOReturn PhxBringup::vramRead(uint32_t byteOff, uint32_t count, uint32_t *out) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || (static_cast<uint64_t>(byteOff) + static_cast<uint64_t>(count) * 4) > fbAperLen)
        return kIOReturnBadArgument;
    volatile uint32_t *v = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff);
    for (uint32_t i = 0; i < count; i++)
        out[i] = v[i];
    return kIOReturnSuccess;
}

// Write one MMIO dword (bring-up experimentation; root + phxdebug only). Returns the readback.
IOReturn PhxBringup::writeReg(uint32_t reg, uint32_t value, uint32_t *readback) {
    if (!mmio)
        return kIOReturnNotReady;
    if ((static_cast<uint64_t>(reg) + 1) * 4 > mmio->getLength())
        return kIOReturnBadArgument;
    IOSimpleLock *l = phxMmIdxDword(reg, 1) ? gMmIdxLock : nullptr;   // b82: see gMmIdxLock
    const IOInterruptState s = l ? IOSimpleLockLockDisableInterrupt(l) : 0;
    wreg(reg, value);
    __asm__ __volatile__("mfence" ::: "memory");
    *readback = rreg(reg);
    if (l) IOSimpleLockUnlockEnableInterrupt(l, s);
    return kIOReturnSuccess;
}

// Write one dword into VRAM at byteOff through the BAR0 aperture.
// op 1: allocate a physically contiguous buffer of a bytes (replaces any previous), out0 = physical address
// op 2: read dword at byte offset a -> out1;  op 3: write dword b at byte offset a;  op 4: free
IOReturn PhxBringup::sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1) {
    *out0 = *out1 = 0;
    if (op == 1) {
        if (sysBuf) {
            sysBuf->release();
            sysBuf = nullptr;
        }
        if (a == 0 || a > (64ull << 20))
            return kIOReturnBadArgument;
        sysBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, a, 0x00007FFFFFFFF000ull);
        if (!sysBuf)
            return kIOReturnNoMemory;
        bzero(sysBuf->getBytesNoCopy(), a);
        *out0 = sysBuf->getPhysicalAddress();
        *out1 = sysBuf->getLength();
        return kIOReturnSuccess;
    }
    if (!sysBuf)
        return kIOReturnNotReady;
    if (op == 4) {
        sysBuf->release();
        sysBuf = nullptr;
        return kIOReturnSuccess;
    }
    if ((a & 3) || a + 4 > sysBuf->getLength())
        return kIOReturnBadArgument;
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(sysBuf->getBytesNoCopy()) + a);
    if (op == 2) {
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory");
        *out1 = *p;
        *out0 = sysBuf->getPhysicalAddress();
        return kIOReturnSuccess;
    }
    if (op == 3) {
        *p = static_cast<uint32_t>(b);
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory"); // GPU walker may not snoop CPU caches
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

