// SPDX-License-Identifier: MIT
// phx_vcn_mmhub.cpp: VCN0 ring adoption + ring test, MMHUB VM translation, VCN submissions (gpuSubmitVcn).
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

int PhxBringup::vcnCtl(int v) {
    if (!mmio || !fbAper || !fbMcBase || kVcnFenceOff + 8 > fbAperLen)
        return ENXIO;
    if (!gVcnLock && !(gVcnLock = IOLockAlloc()))
        return ENOMEM;
    IOLockLock(gVcnLock);
    int r = 0;
    if (v == 0) {
        if (gpuLock && gq[2].ready) {
            IOLockLock(gpuLock);
            const bool idle = gVcnStuck || queueFence(gq[2]) >= (gq[2].head != gq[2].tail ? gq[2].fseq[(gq[2].tail - 1) & 63] : 0);
            if (idle && !gMmvmOn)
                gq[2].ready = false;
            else
                r = EBUSY;   // pending VCN work or MMHUB VM still on
            IOLockUnlock(gpuLock);
        }
        if (!r)
            gVcnOn = 0;
    } else if (v == 1) {
        const uint64_t mc = fbMcBase + kVcnRingOff;
        const uint32_t ps = rreg(kVcnPowerStatus), rp = rreg(kVcnRbRptr), wp = rreg(kVcnRbWptr);
        if (ps == 0xFFFFFFFFu || (ps & 3) != 1) {
            gVcnLastErr = 1; r = ENODEV;   // VCN0 not powered up (vcn_up.sh --run not done this boot)
        } else if (rreg(kVcnRbBaseLo) != static_cast<uint32_t>(mc) || rreg(kVcnRbBaseHi) != static_cast<uint32_t>(mc >> 32) ||
                   rreg(kVcnRbSize) != kVcnRingDw || (rreg(kVcnRbEnable) & 1) == 0) {
            gVcnLastErr = 2; r = EINVAL;   // the ring is not the M1 layout
        } else if (rp != wp || (wp & 63) || wp >= kVcnRingDw) {
            gVcnLastErr = 3; r = EBUSY;    // not idle
        } else if (!gpuLock) {
            r = ENXIO;
        } else {
            IOLockLock(gpuLock);   // b67: the ring is the third queue on the global sequence
            GpuQueue &q = gq[2];
            bzero(&q, sizeof(q));
            q.vcn = true;
            q.ringOff = static_cast<uint32_t>(kVcnRingOff);
            q.ringDw = kVcnRingDw;
            q.fenceOff = static_cast<uint32_t>(kVcnFenceOff);
            q.wptr = q.doneEnd = wp;
            gVcnStuck = 0;
            *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + kVcnFenceOff) = static_cast<uint32_t>(gSeq);
            hdpFlush();
            q.ready = true;
            IOLockUnlock(gpuLock);
            gVcnWptr = wp;
            gVcnOn = 1;
            gVcnLastErr = 0;
        }
    } else if (v == 2) {
        if (!gVcnOn || !gq[2].ready || queueFence(gq[2]) < (gq[2].head != gq[2].tail ? gq[2].fseq[(gq[2].tail - 1) & 63] : 0)) {
            r = gVcnOn ? EBUSY : EINVAL;
        } else {
            IOLockLock(gpuLock);
            GpuQueue &q = gq[2];
            for (uint32_t i = 0; i < 64; i++)
                ringPut(q, i, i ? 0u : 0x00000001u);   // VCN_ENC_CMD_END + 63 VCN_ENC_CMD_NO_OP
            q.wptr += 64;
            q.doneEnd = q.wptr;   // nothing to retire: a ring test carries no fence
            const uint32_t nw = static_cast<uint32_t>(q.wptr & (kVcnRingDw - 1));
            __asm__ __volatile__("sfence" ::: "memory");
            hdpFlush();
            const uint64_t t0 = phxNowNs();
            wreg(kVcnRbWptr, nw);
            uint32_t rp = 0;
            for (int i = 0; i < 2000 && (rp = rreg(kVcnRbRptr)) != nw; i++)
                IODelay(10);
            IOLockUnlock(gpuLock);
            gVcnLastUs = static_cast<uint32_t>((phxNowNs() - t0) / 1000);
            gVcnWptr = nw;
            if (rp == nw) {
                gVcnTestsOk++;
            } else {
                gVcnTestsFail++;
                gVcnLastErr = 4;
                gVcnOn = 0;   // stuck: stop using it
                r = ETIMEDOUT;
            }
        }
    } else {
        r = EINVAL;
    }
    IOLockUnlock(gVcnLock);
    return r;
}

int PhxBringup::vcnStat(char *b, size_t n) {
    const volatile uint32_t *fence = fbAper ? reinterpret_cast<const volatile uint32_t *>(fbAper->getVirtualAddress() + kVcnFenceOff) : nullptr;
    return snprintf(b, n, "vcn %d stuck %d (times %d) queue %d; wptr 0x%x; tests ok %u fail %u last %u us; last err %u; UVD_POWER_STATUS 0x%08x UVD_STATUS 0x%08x RB_ENABLE 0x%08x RB_BASE 0x%x_%08x RB_SIZE 0x%x RPTR 0x%x WPTR 0x%x; fence 0x%x",
                    gVcnOn, (int)gVcnStuck, (int)gVcnStuckCount, gq[2].ready ? 1 : 0, gVcnWptr, gVcnTestsOk, gVcnTestsFail, gVcnLastUs, gVcnLastErr,
                    mmio ? rreg(kVcnPowerStatus) : 0, mmio ? rreg(kVcnStatus) : 0, mmio ? rreg(kVcnRbEnable) : 0,
                    mmio ? rreg(kVcnRbBaseHi) : 0, mmio ? rreg(kVcnRbBaseLo) : 0, mmio ? rreg(kVcnRbSize) : 0,
                    mmio ? rreg(kVcnRbRptr) : 0, mmio ? rreg(kVcnRbWptr) : 0, fence ? fence[0] : 0);
}

int PhxBringup::mmvmCtl(int v) {
    if (!mmio || !gpuLock)
        return ENXIO;
    if (v != 0 && v != 1)
        return EINVAL;
    IOLockLock(gpuLock);
    int r = 0;
    if (v == 1 && !gMmvmOn) {
        if (gq[2].ready && rreg(kVcnRbRptr) != rreg(kVcnRbWptr)) {
            IOLockUnlock(gpuLock);
            return EBUSY;   // VCN must be idle while its hub changes mode
        }
        if (!gMmDummy) {
            gMmDummy = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                        4096, 0x00007FFFFFFFF000ull);
            if (!gMmDummy || gMmDummy->prepare() != kIOReturnSuccess) {
                OSSafeReleaseNULL(gMmDummy);
                IOLockUnlock(gpuLock);
                return ENOMEM;
            }
            bzero(gMmDummy->getBytesNoCopy(), 4096);
        }
        const uint64_t dpa = gMmDummy->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
        gMmvmSaved[0] = rreg(kMmL2Cntl); gMmvmSaved[1] = rreg(kMmL2Cntl3); gMmvmSaved[2] = rreg(kMmL2Cntl4);
        gMmvmSaved[3] = rreg(kMmL2Cntl5); gMmvmSaved[4] = rreg(kMmMxL1TlbCntl); gMmvmSaved[5] = rreg(kMmFaultCntl2);
        gMmvmSaved[6] = rreg(kMmSysapDefLsb); gMmvmSaved[7] = rreg(kMmFaultDefLo);
        gMmvmBad = 0;
        for (uint32_t n = 0; n < 18; n++)
            wreg(kMmEng0Sem + n, 0);
        for (uint32_t n = 0; n < 18; n++) {
            wreg(kMmEng0RangeLo + 2 * n, 0xFFFFFFFFu);
            wreg(kMmEng0RangeLo + 2 * n + 1, 0x1F);
        }
        wreg(kMmFaultDefLo, static_cast<uint32_t>(dpa >> 12));
        wreg(kMmFaultDefHi, static_cast<uint32_t>(dpa >> 44));
        wreg(kMmSysapDefLsb, 0xA3FF);
        wreg(kMmSysapDefMsb, 0);
        wreg(kMmIdApLowLo, 0xFFFFFFFFu);
        wreg(kMmIdApLowLo + 1, 0xF);
        for (uint32_t i = 2; i < 6; i++)
            wreg(kMmIdApLowLo + i, 0);
        wreg(kMmFaultCntl2, 0x000E0000);
        for (uint32_t vmid = kFirstUserVmid; vmid < kFirstUserVmid + kNumUserVmids; vmid++) {
            wreg(kMmCtx0StartLo + 2 * vmid, 0);
            wreg(kMmCtx0StartLo + 2 * vmid + 1, 0);
            wreg(kMmCtx0EndLo + 2 * vmid, 0xffffffff);
            wreg(kMmCtx0EndLo + 2 * vmid + 1, 0xf);
        }
        wreg(kMmL2Cntl3, 0x80130009);
        wreg(kMmL2Cntl4, 0x00000001);
        wreg(kMmL2Cntl5, 0x00003FE0);
        wreg(kMmL2Cntl, 0x00080E01);
        wreg(kMmL2Cntl2, 0x00000003);
        wreg(kMmMxL1TlbCntl, 0x00001859);
        if (rreg(kMmMxL1TlbCntl) != 0x1859) gMmvmBad |= 1;
        if (rreg(kMmL2Cntl) != 0x00080E01) gMmvmBad |= 2;
        if (rreg(kMmL2Cntl3) != 0x80130009) gMmvmBad |= 4;
        gMmvmOn = 1;
        for (uint32_t i = 0; i < kNumUserVmids; i++) {   // contexts already holding a VMID: mirror their roots now
            if (!hwv[i].ctx)
                continue;
            const uint32_t vmid = kFirstUserVmid + i;
            const VmCtx &c = vm[hwv[i].ctx - 1];
            wreg(kMmCtx0BaseLo + 2 * vmid, static_cast<uint32_t>(c.phys | 1));
            wreg(kMmCtx0BaseLo + 2 * vmid + 1, static_cast<uint32_t>(c.phys >> 32));
            wreg(kMmCtx0Cntl + vmid, 0x01fffe07);
        }
        mmhubFlush(0);
        for (uint32_t i = 0; i < kNumUserVmids; i++)
            if (hwv[i].ctx)
                mmhubFlush(kFirstUserVmid + i);
    } else if (v == 0 && gMmvmOn) {
        if (gq[2].ready && !gVcnStuck && rreg(kVcnRbRptr) != rreg(kVcnRbWptr)) {
            IOLockUnlock(gpuLock);
            return EBUSY;
        }
        for (uint32_t vmid = kFirstUserVmid; vmid < kFirstUserVmid + kNumUserVmids; vmid++)
            wreg(kMmCtx0Cntl + vmid, 0x01fffe00);
        wreg(kMmMxL1TlbCntl, gMmvmSaved[4]);
        wreg(kMmL2Cntl, gMmvmSaved[0]);
        wreg(kMmL2Cntl3, gMmvmSaved[1]);
        wreg(kMmL2Cntl4, gMmvmSaved[2]);
        wreg(kMmL2Cntl5, gMmvmSaved[3]);
        wreg(kMmFaultCntl2, gMmvmSaved[5]);
        wreg(kMmSysapDefLsb, gMmvmSaved[6]);
        wreg(kMmFaultDefLo, gMmvmSaved[7]);
        wreg(kMmL2Cntl2, 0x00000003);
        gMmvmOn = 0;
    }
    IOLockUnlock(gpuLock);
    return r;
}

int PhxBringup::mmvmStat(char *b, size_t n) {
    if (!mmio)
        return snprintf(b, n, "no mmio");
    int o = snprintf(b, n, "mmvm %d bad 0x%x; MX_L1_TLB_CNTL 0x%08x L2_CNTL 0x%08x L2_CNTL3 0x%08x L2_STATUS 0x%08x; fault status 0x%08x addr 0x%x_%08x; flushes %u timeouts %u max %u us; vcn submits %lld; ctx:",
                     gMmvmOn, gMmvmBad, rreg(kMmMxL1TlbCntl), rreg(kMmL2Cntl), rreg(kMmL2Cntl3), rreg(kMmL2Status),
                     rreg(kMmFaultStatus), rreg(kMmFaultAddrHi), rreg(kMmFaultAddrLo), gMmvmFlushes, gMmvmFlushTimeouts,
                     gMmvmFlushMaxUs, (long long)gVcnSubmits);
    for (uint32_t vmid = kFirstUserVmid; vmid < kFirstUserVmid + kNumUserVmids && o > 0 && static_cast<size_t>(o) < n; vmid++)
        o += snprintf(b + o, n - static_cast<size_t>(o), " %u:%08x/%08x(gc %08x)", vmid, rreg(kMmCtx0Cntl + vmid),
                      rreg(kMmCtx0BaseLo + 2 * vmid), rreg(kRegCtx0BaseLo + 2 * vmid));
    return o;
}

bool PhxBringup::vcnSubmitReady() const {
    return gq[2].ready && !gVcnStuck && gMmvmOn && !gMmvmBad && rreg(kMmMxL1TlbCntl) == 0x1859;
}

// b67: a VCN unified-ring frame per Linux (vcn_v2_0_enc_ring_emit_ib, emit_fence, insert_end, NOP pad to 64 dw):
// IB {2, vmid, va_lo, va_hi, len} per IB, FENCE {3, addr_lo, addr_hi, seq, TRAP} into VRAM (VMID 0), END {1}
IOReturn PhxBringup::gpuSubmitVcn(uint32_t handle, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seqOut) {
    if (!vcnSubmitReady() || !vmCtx(handle))
        return kIOReturnNotReady;
    GpuQueue &q = gq[2];
    const uint32_t need = (n * 5 + 6 + 63) & ~63u;
    if (need + 64 > q.ringDw)
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    for (int spin = 0;; spin++) {
        const uint64_t done = queueFence(q);
        while (q.head != q.tail && q.fseq[q.head & 63] <= done) {
            q.doneEnd = q.fend[q.head & 63];
            q.head++;
        }
        if (q.tail - q.head < 63 && q.wptr + need - q.doneEnd <= q.ringDw - 64)
            break;
        if (spin > 40000) {
            IOLockUnlock(gpuLock);
            return kIOReturnBusy;
        }
        IODelay(50);
    }
    const uint32_t vmid = vmBind(handle);
    if (!vmid) {
        IOLockUnlock(gpuLock);
        return kIOReturnBusy;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        ringPut(q, k++, 0x00000002);   // VCN_ENC_CMD_IB
        ringPut(q, k++, vmid);
        ringPut(q, k++, static_cast<uint32_t>(ibVa[i]) & ~3u);
        ringPut(q, k++, static_cast<uint32_t>(ibVa[i] >> 32) & 0xFFFF);
        ringPut(q, k++, ibDw[i]);
    }
    const uint64_t seq = ++gSeq;
    hwv[vmid - kFirstUserVmid].lastSeq = seq;
    const uint64_t fenceMc = fbMcBase + q.fenceOff;
    ringPut(q, k++, 0x00000003);       // VCN_ENC_CMD_FENCE
    ringPut(q, k++, static_cast<uint32_t>(fenceMc));
    ringPut(q, k++, static_cast<uint32_t>(fenceMc >> 32));
    ringPut(q, k++, static_cast<uint32_t>(seq));
    ringPut(q, k++, 0x00000004);       // VCN_ENC_CMD_TRAP
    ringPut(q, k++, 0x00000001);       // VCN_ENC_CMD_END
    while (k < need)
        ringPut(q, k++, 0);            // VCN_ENC_CMD_NO_OP
    q.wptr += k;
    q.fseq[q.tail & 63] = seq;
    q.fend[q.tail & 63] = q.wptr;
    gVcnSubAbs[q.tail & 63] = mach_absolute_time();
    __asm__ __volatile__("" ::: "memory");
    q.tail = q.tail + 1;
    __asm__ __volatile__("sfence" ::: "memory");
    (void)*reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff);
    hdpFlush();
    wreg(kVcnRbWptr, static_cast<uint32_t>(q.wptr & (q.ringDw - 1)));
    OSIncrementAtomic64(&gVcnSubmits);
    IOLockUnlock(gpuLock);
    *seqOut = seq;
    return kIOReturnSuccess;
}

