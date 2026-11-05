// SPDX-License-Identifier: MIT
// phx_queue.cpp: GPU queues: ring writes, the global fence sequence, gpuRetired, gpuSubmit, doorbell and VRAM writes.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

void PhxBringup::ringPut(GpuQueue &q, uint32_t idx, uint32_t value) {
    const uint64_t slot = (q.wptr + idx) & (q.ringDw - 1);
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff + slot * 4) = value;
}

uint64_t PhxBringup::queueFence(const GpuQueue &q) const {
    const volatile uint32_t *f = reinterpret_cast<const volatile uint32_t *>(fbAper->getVirtualAddress() + q.fenceOff);
    if (q.vcn) {   // VCN_ENC_CMD_FENCE writes the low 32 bits of the sequence only
        const uint64_t cur = gSeq;
        uint64_t v = (cur & ~0xFFFFFFFFull) | f[0];
        if (v > cur)
            v -= 1ull << 32;
        return v;
    }
    return static_cast<uint64_t>(f[0]) | (static_cast<uint64_t>(f[1]) << 32);
}

// Everything up to the returned sequence has finished: the oldest submission still running on any queue, minus one.
// Lock-free reader: an entry is written before `tail` moves past it.
uint64_t PhxBringup::gpuRetiredMask(uint32_t qmask) const {
    uint64_t r = gSeq;
    for (uint32_t qi = 0; qi < 3; qi++) {
        const GpuQueue &q = gq[qi];
        if (!q.ready || !(qmask & (1u << qi)))
            continue;
        const uint64_t done = queueFence(q);
        const uint32_t tail = q.tail;
        for (uint32_t i = q.head; i != tail; i++) {
            if (q.fseq[i & 63] > done && q.vcn) {
                // b68: a hung VCN must not stall the GFX waits: past 200 ms the entry stops counting
                if (gVcnStuck)
                    break;
                uint64_t ns;
                absolutetime_to_nanoseconds(mach_absolute_time() - gVcnSubAbs[i & 63], &ns);
                if (ns > 200000000ull) {
                    if (OSCompareAndSwap(0, 1, reinterpret_cast<volatile UInt32 *>(&gVcnStuck))) {
                        gVcnStuckAbs = mach_absolute_time();
                        OSIncrementAtomic(&gVcnStuckCount);
                    }
                    break;
                }
            }
            if (q.fseq[i & 63] > done) {
                if (q.fseq[i & 63] - 1 < r)
                    r = q.fseq[i & 63] - 1;
                break;
            }
        }
    }
    return r;
}

// b72: the queue gpuSubmit / gpuSubmitVcn will use for this submission, as a gpuRetiredMask bit
uint32_t PhxBringup::submitQueueBit(bool gfx, bool vcn) const {
    if (vcn) {
        for (uint32_t i = 0; i < 3; i++)
            if (gq[i].vcn)
                return 1u << i;
        return 0;
    }
    return gfx ? (gq[1].ready ? 2u : 1u) : (gq[0].ready ? 1u : 2u);
}

IOReturn PhxBringup::gpuSubmit(uint32_t handle, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seqOut) {
    // graphics IBs go to the gfx queue and compute IBs to the compute queue; with one of them missing, the other runs both
    GpuQueue *qp = gfx ? (gq[1].ready ? &gq[1] : &gq[0]) : (gq[0].ready ? &gq[0] : &gq[1]);
    if (!qp->ready || !vmCtx(handle))
        return kIOReturnNotReady;
    GpuQueue &q = *qp;
    const uint32_t need = n * 4 + 8;
    if (need + 16 > q.ringDw)
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    // Ring space and the 64-entry history: retire finished entries, wait for the oldest one if needed (bounded, ~2 s).
    for (int spin = 0;; spin++) {
        const uint64_t done = queueFence(q);
        while (q.head != q.tail && q.fseq[q.head & 63] <= done) {
            q.doneEnd = q.fend[q.head & 63];
            q.head++;
        }
        if (q.tail - q.head < 63 && q.wptr + need - q.doneEnd <= q.ringDw - 16)
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
        ringPut(q, k++, 0xC0023F00);                               // INDIRECT_BUFFER
        // the CP wants the canonical (sign-extended) address: an upper-half IB (bit 47, where RADV puts them) with a
        // bare 16-bit high word silently wedges the queue
        const uint64_t cva = (ibVa[i] & (1ull << 47)) ? (ibVa[i] | 0xFFFF000000000000ull) : ibVa[i];
        ringPut(q, k++, static_cast<uint32_t>(cva) & ~3u);
        ringPut(q, k++, static_cast<uint32_t>(cva >> 32));
        // size | VMID, plus VALID on compute (gfx_v11_0_ring_emit_ib_compute; the gfx variant has no VALID bit)
        ringPut(q, k++, ibDw[i] | (q.gfx ? 0u : (1u << 23)) | (vmid << 24));
    }
    const uint64_t seq = ++gSeq;
    hwv[vmid - kFirstUserVmid].lastSeq = seq;
    const uint64_t fenceMc = fbMcBase + q.fenceOff;
    ringPut(q, k++, 0xC0064900);                                   // RELEASE_MEM, as gfx_v11_0_ring_emit_fence
    ringPut(q, k++, 0x06603514);                                   // GCR SEQ|GL2_WB|GLM_INV|GLM_WB, CP 3, CACHE_FLUSH_AND_INV_TS, idx 5
    ringPut(q, k++, gEopIrq ? 0x42000000u : 0x40000000u);         // DATA_SEL 2 (64-bit), INT_SEL 2 with debug.phxeopirq (b60)
    ringPut(q, k++, static_cast<uint32_t>(fenceMc));
    ringPut(q, k++, static_cast<uint32_t>(fenceMc >> 32));
    ringPut(q, k++, static_cast<uint32_t>(seq));
    ringPut(q, k++, static_cast<uint32_t>(seq >> 32));
    ringPut(q, k++, gEopIrq ? static_cast<uint32_t>(seq) : 0u);     // reaches the IV's src_data[0] (b60)
    q.wptr += k;
    q.fseq[q.tail & 63] = seq;
    q.fend[q.tail & 63] = q.wptr;
    __asm__ __volatile__("" ::: "memory");
    q.tail = q.tail + 1;
    // BAR0 is write-combined: drain the WC buffers so the packets are in VRAM before the CP is told about them
    __asm__ __volatile__("sfence" ::: "memory");
    (void)*reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff);
    hdpFlush();
    volatile uint32_t *wb = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.wbOff + 16);
    wb[0] = static_cast<uint32_t>(q.wptr);
    wb[1] = static_cast<uint32_t>(q.wptr >> 32);
    __asm__ __volatile__("sfence" ::: "memory");
    (void)wb[0];
    hdpFlush();
    __asm__ __volatile__("mfence" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + q.dbByte) = q.wptr;
    IOLockUnlock(gpuLock);
    *seqOut = seq;
    return kIOReturnSuccess;
}

IOReturn PhxBringup::doorbellWrite(uint64_t byteOff, uint64_t value) {
    if (!doorbellMap)
        return kIOReturnNotReady;
    if ((byteOff & 7) || byteOff + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + byteOff) = value;
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vramWrite(uint32_t byteOff, uint32_t value) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || static_cast<uint64_t>(byteOff) + 4 > fbAperLen)
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff) = value;
    __asm__ __volatile__("sfence" ::: "memory");
    return kIOReturnSuccess;
}

