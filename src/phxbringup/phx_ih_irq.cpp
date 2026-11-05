// SPDX-License-Identifier: MIT
// phx_ih_irq.cpp: IH ring (polled or in VRAM), MSI interrupt filter and their stat strings.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

IOReturn PhxBringup::ihEnable() {
    if (!mmio || gIhOn)
        return mmio ? kIOReturnSuccess : kIOReturnNotReady;
    if (!gIhSpin && !(gIhSpin = IOSimpleLockAlloc()))
        return kIOReturnNoMemory;
    wreg(0x1120, rreg(0x1120) & ~0x00020001u);   // IH_RB_CNTL: RB_ENABLE and ENABLE_INTR off first
    wreg(0x1123, 0);
    wreg(0x1124, 0);
    if (rreg(0x1120) & 1)
        return kIOReturnIOError;
    const bool vram = gIhVram && fbAper && kIhVramOff + kIhRingBytes + 4096 <= fbAperLen;
    gIhBuf = vram ? nullptr : IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                             kIhRingBytes + 4096, 0x00007FFFFFFFF000ull);
    gIhDummy = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                               4096, 0x00007FFFFFFFF000ull);
    if ((!vram && (!gIhBuf || gIhBuf->prepare() != kIOReturnSuccess)) || !gIhDummy || gIhDummy->prepare() != kIOReturnSuccess) {
        OSSafeReleaseNULL(gIhBuf); OSSafeReleaseNULL(gIhDummy);
        return kIOReturnNoMemory;
    }
    if (vram) {
        gIhRing = reinterpret_cast<uint8_t *>(fbAper->getVirtualAddress() + kIhVramOff);
        for (uint32_t o = 0; o < kIhRingBytes + 4096; o += 4)
            *reinterpret_cast<volatile uint32_t *>(gIhRing + o) = 0;
        gIhPa = fbMcBase + kIhVramOff;
    } else {
        gIhRing = static_cast<uint8_t *>(gIhBuf->getBytesNoCopy());
        bzero(gIhRing, kIhRingBytes + 4096);
        gIhPa = gIhBuf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    }
    bzero(gIhDummy->getBytesNoCopy(), 4096);
    const uint64_t dpa = gIhDummy->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone), wb = gIhPa + kIhRingBytes;
    wreg(0xE12, static_cast<uint32_t>(dpa >> 8));   // NBIF INTERRUPT_CNTL2: the dummy-read page
    wreg(0xE11, 0);                                 // INTERRUPT_CNTL
    wreg(0x1121, static_cast<uint32_t>(gIhPa >> 8));
    wreg(0x1122, static_cast<uint32_t>(gIhPa >> 40) & 0xFF);
    wreg(0x1120, vram ? 0xC0310120 : 0xA0310120);   // MC_SPACE 4 (VRAM) or 2 (bus address) | WPTR_OVERFLOW_CLEAR | RPTR_REARM | MC_SNOOP | WPTR_OVERFLOW_ENABLE | WPTR_WB | RB_SIZE 16
    wreg(0x1126, static_cast<uint32_t>(wb));
    wreg(0x1125, static_cast<uint32_t>(wb >> 32) & 0xFFFF);
    wreg(0x1124, 0);
    wreg(0x1123, 0);
    wreg(0x1127, 0);            // IH_DOORBELL_RPTR: no doorbell, the rptr is written by MMIO
    wreg(0x117A, rreg(0x117A) | 1u << 18);           // IH_STORM_CLIENT_LIST_CNTL: VMC
    wreg(0x1175, rreg(0x1175) | 1u << 3);            // IH_INT_FLOOD_CNTL
    wreg(0x1191, (rreg(0x1191) & ~7u) | 3);          // IH_MSI_STORM_CTRL delay
    if (rreg(0x1121) != static_cast<uint32_t>(gIhPa >> 8)) {
        IOLog("PhxBringup: IH base read-back mismatch\n");
        ihDisable();
        return kIOReturnIOError;
    }
    gIhRptr = 0;
    bzero(gIhKey, sizeof gIhKey); bzero(gIhCnt, sizeof gIhCnt); bzero(gIhLast, sizeof gIhLast);
    gIhIvs = gIhEop = gIhOther = gIhOverflow = 0;
    wreg(0x1120, rreg(0x1120) | 1);                  // RB_ENABLE, still no ENABLE_INTR (polled)
    wreg(0x306A, rreg(0x306A) | 1u << 26);           // CP_INT_CNTL_RING0 TIME_STAMP (gfx EOP)
    wreg(0x3085, rreg(0x3085) | 1u << 26);           // CP_ME1_PIPE0_INT_CNTL TIME_STAMP (compute EOP)
    gIhOn = 1;
    if (!gIhPowerNotifier)
        gIhPowerNotifier = registerPrioritySleepWakeInterest(phxIhPowerHandler, this);
    IOLog("PhxBringup: IH ring on at 0x%llx (%s), IH_RB_CNTL 0x%08x (polled)\n", gIhPa, vram ? "VRAM" : "system memory", rreg(0x1120));
    return kIOReturnSuccess;
}

void PhxBringup::ihDisable() {
    if (gIrqOn)
        irqDisable();
    if (gIhSpin) {
        const IOInterruptState is = IOSimpleLockLockDisableInterrupt(gIhSpin);
        gIhOn = 0;
        IOSimpleLockUnlockEnableInterrupt(gIhSpin, is);
    }
    if (mmio) {
        wreg(0x306A, rreg(0x306A) & ~(1u << 26));
        wreg(0x3085, rreg(0x3085) & ~(1u << 26));
        wreg(0x1120, rreg(0x1120) & ~0x00020001u);
        wreg(0x1123, 0);
        wreg(0x1124, 0);
        IODelay(1000);
    }
    gIhOn = 0;
    gEopIrq = 0;
    gIhRing = nullptr;
    if (gIhBuf) { gIhBuf->complete(); OSSafeReleaseNULL(gIhBuf); }
    if (gIhDummy) { gIhDummy->complete(); OSSafeReleaseNULL(gIhDummy); }
}

int PhxBringup::ihStat(char *b, size_t n) {
    if (!gIhOn || !gIhRing)
        return snprintf(b, n, "IH off (IH_RB_CNTL 0x%08x)", mmio ? rreg(0x1120) : 0);
    const IOInterruptState is = IOSimpleLockLockDisableInterrupt(gIhSpin);
    ihDrain();
    IOSimpleLockUnlockEnableInterrupt(gIhSpin, is);
    const uint8_t *ring = gIhRing;
    const uint32_t wbw = *reinterpret_cast<const volatile uint32_t *>(ring + kIhRingBytes), mw = rreg(0x1124);
    int o = snprintf(b, n, "IH on %s 0x%llx IH_RB_CNTL 0x%08x wptr wb 0x%x mmio 0x%x rptr 0x%x status 0x%08x rb_status 0x%08x; IVs %lld eop %lld other %lld overflow %lld; CP_INT_CNTL_RING0 0x%08x ME1_PIPE0 0x%08x eopirq %d; last IV %08x %08x %08x %08x %08x %08x %08x %08x; by client:src",
                     gIhBuf ? "sysmem" : "vram", gIhPa, rreg(0x1120), wbw, mw, gIhRptr, rreg(0x1162), rreg(0x1174), (long long)gIhIvs, (long long)gIhEop,
                     (long long)gIhOther, (long long)gIhOverflow, rreg(0x306A), rreg(0x3085), gEopIrq,
                     gIhLast[0], gIhLast[1], gIhLast[2], gIhLast[3], gIhLast[4], gIhLast[5], gIhLast[6], gIhLast[7]);
    for (unsigned k = 0; k < 16 && gIhCnt[k] && o > 0 && static_cast<size_t>(o) < n; k++)
        o += snprintf(b + o, n - static_cast<size_t>(o), " %02x:%02x=%u", gIhKey[k] >> 8, gIhKey[k] & 0xFF, gIhCnt[k]);
    return o;
}

uint32_t PhxBringup::ihDrain() {
    const uint8_t *ring = gIhRing;
    if (!ring)
        return 0;
    const volatile uint32_t *wbp = reinterpret_cast<const volatile uint32_t *>(ring + kIhRingBytes);
    uint32_t n = 0;
    for (unsigned pass = 0; pass < 4; pass++) {   // Linux restart_ih: drain again if the wptr moved meanwhile
        const uint32_t wbw = *wbp;
        uint32_t w = wbw & (kIhRingBytes - 1) & ~31u;
        if (wbw & 1) {   // RB_OVERFLOW: skip to the oldest valid IV, then clear the flag (ih_v6_0_get_wptr)
            OSIncrementAtomic64(&gIhOverflow);
            gIhRptr = (w + 32) & (kIhRingBytes - 1);
            wreg(0x1120, rreg(0x1120) | 1u << 31);
            wreg(0x1120, rreg(0x1120) & ~(1u << 31));
        }
        if (gIhRptr == w)
            break;
        while (gIhRptr != w) {
            const uint32_t *iv = reinterpret_cast<const uint32_t *>(ring + gIhRptr);
            const uint32_t client = iv[0] & 0xFF, src = (iv[0] >> 8) & 0xFF, key = client << 8 | src;
            gIhIvs++;
            if (client == 0x14 && src == 0xB5) {
                gIhEop++;
                gIhLastSeq = iv[4];
                gEopArrAbs[iv[4] & (kEopArrN - 1)] = mach_absolute_time();
                gEopArrSeq[iv[4] & (kEopArrN - 1)] = iv[4];
            } else
                gIhOther++;
            for (unsigned k = 0; k < 16; k++)
                if (gIhKey[k] == key || !gIhCnt[k]) { gIhKey[k] = key; gIhCnt[k]++; break; }
            for (unsigned k = 0; k < 8; k++) gIhLast[k] = iv[k];
            gIhRptr = (gIhRptr + 32) & (kIhRingBytes - 1);
            n++;
        }
        wreg(0x1123, gIhRptr);   // with RPTR_REARM this re-arms the MSI if more IVs are pending
        gIrqLoops++;
    }
    return n;
}

IOReturn PhxBringup::irqEnable() {
    if (!gIhOn || !pci || !mmio)
        return kIOReturnNotReady;
    if (!gIrqSrc) {
        int msi = -1;
        for (int i = 0; i < 8; i++) {
            int type = 0;
            if (pci->getInterruptType(i, &type) != kIOReturnSuccess)
                break;
            if (msi < 0 && (type & kIOInterruptTypePCIMessaged))
                msi = i;
        }
        if (msi < 0) {
            IOLog("PhxBringup: no MSI interrupt source\n");
            return kIOReturnUnsupported;
        }
        gIrqIndex = msi;
        gIrqWl = IOWorkLoop::workLoop();
        gIrqSrc = IOFilterInterruptEventSource::filterInterruptEventSource(this, phxIrqAction, phxIrqFilter, pci, msi);
        if (!gIrqWl || !gIrqSrc || gIrqWl->addEventSource(gIrqSrc) != kIOReturnSuccess) {
            OSSafeReleaseNULL(gIrqSrc);
            OSSafeReleaseNULL(gIrqWl);
            return kIOReturnError;
        }
    }
    nanoseconds_to_absolutetime(1000000000ull, &gIrqWinLen);
    gIrqWinAbs = mach_absolute_time();
    gIrqWinCnt = 0;
    gIrqMsi = gIrqSpurious = gIrqLoops = 0;
    gIrqStorm = 0;
    gIrqSrc->enable();
    gIrqOn = 1;
    wreg(0x1120, rreg(0x1120) | 1u << 17);   // ENABLE_INTR, only now that the vector has a handler
    return kIOReturnSuccess;
}

void PhxBringup::irqDisable() {
    if (mmio)
        wreg(0x1120, rreg(0x1120) & ~(1u << 17));
    if (gIrqSrc)
        gIrqSrc->disable();
    gIrqOn = 0;
}

// primary interrupt context: no IOLock, no allocation, no blocking
void PhxBringup::irqFilter() {
    OSIncrementAtomic64(&gIrqMsi);
    IOSimpleLockLock(gIhSpin);
    const SInt64 eop0 = gIhEop;
    const uint32_t n = gIhOn ? ihDrain() : 0;
    const bool eop = gIhEop != eop0;
    IOSimpleLockUnlock(gIhSpin);
    if (eop) {
        gIrqEopAbs = mach_absolute_time();
        thread_wakeup(reinterpret_cast<event_t>(const_cast<SInt32 *>(&gFenceEvt)));
    }
    if (!n)
        OSIncrementAtomic64(&gIrqSpurious);
    const uint64_t now = mach_absolute_time();
    if (now - gIrqWinAbs > gIrqWinLen) {
        gIrqWinAbs = now;
        gIrqWinCnt = 0;
    } else if (++gIrqWinCnt > 50000 && !gIrqStorm) {
        gIrqStorm = 1;
        wreg(0x1120, rreg(0x1120) & ~(1u << 17));
    }
}

int PhxBringup::irqStat(char *b, size_t n) {
    uint8_t cap = 0;
    const uint16_t msiCtl = pci && pci->findPCICapability(kIOPCIMSICapability, &cap) && cap ? pci->configRead16(cap + 2) : 0xFFFF;
    return snprintf(b, n, "irq %d index %d storm %d; msi %lld spurious %lld drain passes %lld; IVs %lld eop %lld other %lld overflow %lld last seq 0x%x; MSI cap 0x%x ctl 0x%04x; IH_RB_CNTL 0x%08x rptr 0x%x wptr 0x%x",
                    gIrqOn, gIrqIndex, gIrqStorm, (long long)gIrqMsi, (long long)gIrqSpurious, (long long)gIrqLoops,
                    (long long)gIhIvs, (long long)gIhEop, (long long)gIhOther, (long long)gIhOverflow, gIhLastSeq,
                    cap, msiCtl, mmio ? rreg(0x1120) : 0, mmio ? rreg(0x1123) : 0, mmio ? rreg(0x1124) : 0);
}

