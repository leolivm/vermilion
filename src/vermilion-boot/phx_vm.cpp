// SPDX-License-Identifier: MIT
// phx_vm.cpp: GPU VM (E8): contexts, VMIDs, TLB flush (GC + MMHUB), map/unmap through PhxVmTables.h, debug.phxpt_stat,
// plus adoptQueue (the compute/gfx ring adoption) and claimAccel.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

// ---- E8: GPU execution ------------------------------------------------------------------------------------
// Everything here reproduces what tools/phxctl/mesboot.c proved on the 780M: VMID 8 with a 4-level table in
// system memory (PDE = next|VALID|SYSTEM|SNOOPED, no BFS/TF since gmc_v11 has translate_further off), system-page
// PTEs with MTYPE 4, invalidation on engine 17, and an ACQUIRE/RELEASE_MEM GL2 writeback after each IB.
static constexpr uint32_t kFirstUserVmid = 8, kNumUserVmids = 8;   // VMIDs 8..15, lent to client contexts (vmBind)
static constexpr uint64_t kPteSysFlags = 0x77ull;   // VALID SYSTEM SNOOPED EXEC READ WRITE; MTYPE from debug.phxmtype
static constexpr uint64_t kPtAddrMask = 0x0000FFFFFFFFF000ull;
// GC registers (absolute dword index, GC base 0x1260): context n = context 0 + n (CNTL) / + 2n (address pairs),
// invalidation engine 17
static constexpr uint32_t kRegCtx0Cntl = 0x28E8, kRegCtx0BaseLo = 0x2953, kRegCtx0StartLo = 0x2973, kRegCtx0EndLo = 0x2993;
static constexpr uint32_t kRegInvEng17Req = 0x291C, kRegInvEng17Ack = 0x292E;

static inline void flushLine(const volatile void *p) {
    __asm__ __volatile__("clflush (%0)" ::"r"(p) : "memory");
}

// b77: what PhxVmTables.h needs from the kext (see its header comment); built per call, under gpuLock
struct PhxVmEnv {
    PhxBringup *p;
    phxvm::Shadow *sh;
    uint64_t poolPhys;
    uint8_t *poolCpu;
    uint64_t tablePhys(phxvm::Ctx &c, uint32_t i) { return p->ptTablePhys(c, i); }
    void flush(const volatile void *a) const { flushLine(a); }
    void event(phxvm::Ev ev) const {
        OSIncrementAtomic(&gPtEv[ev]);
        if (ev == phxvm::EvCap) OSIncrementAtomic(&gVmCtxCap);          // debug.phxvm "ctx cap", as before b77
        else if (ev == phxvm::EvPoolFull) OSIncrementAtomic(&gVmPoolFull);
    }
    int mode() const { return gPtReclaim; }
    uint32_t keep() const { return gPtKeep < 0 ? 0u : static_cast<uint32_t>(gPtKeep); }
    uint64_t emitted() const { return p->gSeq; }
    uint64_t retired() const { return p->gpuRetired(); }
};

PhxVmEnv PhxBringup::vmEnv() {
    PhxVmEnv e{this, ptShadowMem ? &ptShadow : nullptr, fbPhys + PHX_VRAM_PT_OFF, nullptr};
    if (fbAper) e.poolCpu = reinterpret_cast<uint8_t *>(fbAper->getVirtualAddress() + PHX_VRAM_PT_OFF);
    if (!e.poolCpu) e.sh = nullptr;
    return e;
}

// The physical address of table i, allocating (zeroed, flushed) the chunk it lives in; 0 when that fails.
uint64_t PhxBringup::ptTablePhys(VmCtx &c, uint32_t i) {
    const uint32_t k = i / kPtChunkPages;
    if (k >= kPtChunks)
        return 0;
    if (!c.chunk[k]) {
        // navi31: chunks come from the VRAM pool, written through BAR0 (the HDP is flushed before TLB invalidation)
        const uint64_t off = PHX_VRAM_PT_OFF + static_cast<uint64_t>(ptPoolNext) * kPtChunkBytes;
        if (!fbAper || off + kPtChunkBytes > PHX_VRAM_PT_OFF + PHX_VRAM_PT_SIZE || off + kPtChunkBytes > fbAperLen)
            return 0;
        ptPoolNext++;
        uint8_t *p = reinterpret_cast<uint8_t *>(fbAper->getVirtualAddress() + off);
        for (uint32_t o = 0; o < kPtChunkBytes; o += 8)
            *reinterpret_cast<volatile uint64_t *>(p + o) = 0;
        c.chunk[k] = p;
        c.chunkPhys[k] = fbPhys + off;   // VRAM physical address (MMMC_VM_FB_OFFSET is 0 on this dGPU)
    }
    return c.chunkPhys[k] + static_cast<uint64_t>(i % kPtChunkPages) * 4096;
}

bool PhxBringup::vmFlushTlb(uint32_t vmid) const {
    const uint32_t mmTimeouts = gMmvmFlushTimeouts;
    bool acked = true;
    __asm__ __volatile__("mfence" ::: "memory");
    hdpFlush();   // navi31: page-table writes went through BAR0 and sit in the HDP until flushed
    wreg(kRegInvEng17Req, 0x00F80000u | (1u << vmid) | 1u);
    // b48: up to 100 ms (was 1 ms, and a missing ack went unnoticed): a submission must not run on stale translations
    // after 16 timeouts the ack is taken as unreliable and the old 1 ms bound applies again (still counted)
    const uint32_t cap = gVmInvTimeouts > 16 ? 1000u : 100000u;
    uint32_t us = 0;
    while (!(rreg(kRegInvEng17Ack) & (1u << vmid))) {
        if (us >= cap) { OSIncrementAtomic(&gVmInvTimeouts); acked = false; break; }
        IODelay(us < 1000 ? 1 : 10);
        us += us < 1000 ? 1 : 10;
    }
    if (us > gVmInvMaxUs)
        gVmInvMaxUs = us;
    if (gMmvmOn)
        mmhubFlush(vmid);
    return acked && gMmvmFlushTimeouts == mmTimeouts;
}

// gmc_v11_0_flush_gpu_tlb on MMHUB (engine 17, with its semaphore: a read that returns 1 takes it), as Linux before KIQ
void PhxBringup::mmhubFlush(uint32_t vmid) const {
    uint32_t us = 0;
    while (!(rreg(kMmEng17Sem) & 1)) {
        if (++us > 100000) { gMmvmFlushTimeouts++; return; }
        IODelay(1);
    }
    wreg(kMmEng17Req, 0x00F80000u | (1u << vmid));
    uint32_t t = 0;
    while (!(rreg(kMmEng17Ack) & (1u << vmid))) {
        if (++t > 100000) { gMmvmFlushTimeouts++; break; }
        IODelay(1);
    }
    wreg(kMmEng17Sem, 0);
    wreg(kMmBankSel, rreg(kMmBankSel) | 1u << 25);
    (void)rreg(kMmBankSel);
    gMmvmFlushes++;
    if (t > gMmvmFlushMaxUs)
        gMmvmFlushMaxUs = t;
}

// Takes a free context: empty tables (root only), no hardware VMID until its first submission. Returns its handle,
// 0 when all kMaxCtx are in use.
uint32_t PhxBringup::vmidAlloc() {
    if (!gpuReady())
        return 0;
    IOLockLock(gpuLock);
    if (!ptShadowTried) {   // b77: before the first table of the first context, so the shadow sees every table
        ptShadowTried = true;
        const uint32_t n = static_cast<uint32_t>(PHX_VRAM_PT_SIZE / 4096);
        static_assert(PHX_VRAM_PT_SIZE / 4096 < phxvm::kNil, "pool table numbers must fit the 16-bit FIFO links");
        if ((ptShadowMem = IOMalloc(phxvm::Shadow::bytes(n)))) {
            bzero(ptShadowMem, phxvm::Shadow::bytes(n));
            ptShadow.carve(ptShadowMem, n);
        }
    }
    for (uint32_t i = 0; i < kMaxCtx; i++) {
        VmCtx &c = vm[i];
        if (c.busy)
            continue;
        if (!c.chunk[0]) {
            c.phys = ptTablePhys(c, 0);
            if (!c.phys)
                break;
            c.pages = kPtChunks * kPtChunkPages;
        }
        c.used = 1;
        c.busy = true;
        c.hw = 0;
        PhxVmEnv e = vmEnv();
        phxvm::reset(e, c);   // b77: root only, nothing parked
        IOLockUnlock(gpuLock);
        return i + 1;
    }
    IOLockUnlock(gpuLock);
    return 0;
}

// Points a hardware VMID at the context's tables for a submission (gpuLock held): the one it already has, a free one,
// or the least recently used one whose submissions the GPU has all finished (waits for one, bounded ~2 s). 0 on timeout.
uint32_t PhxBringup::vmBind(uint32_t handle) {
    VmCtx &c = vm[handle - 1];
    const uint64_t now = ++hwTick;
    if (c.hw) {
        hwv[c.hw - kFirstUserVmid].lastUse = now;
        return c.hw;
    }
    int pick = -1;
    for (int spin = 0; pick < 0; spin++) {
        const uint64_t done = gpuRetired();
        uint64_t oldest = ~0ull;
        for (uint32_t i = 0; i < kNumUserVmids; i++) {
            if (!hwv[i].ctx) {
                pick = static_cast<int>(i);
                break;
            }
            if (hwv[i].lastSeq <= done && hwv[i].lastUse < oldest) {
                oldest = hwv[i].lastUse;
                pick = static_cast<int>(i);
            }
        }
        if (pick >= 0)
            break;
        if (spin > 40000)
            return 0;
        IODelay(50);
    }
    HwVmid &h = hwv[pick];
    if (h.ctx)
        vm[h.ctx - 1].hw = 0;
    h.ctx = handle;
    h.lastSeq = 0;
    h.lastUse = now;
    const uint32_t vmid = kFirstUserVmid + static_cast<uint32_t>(pick);
    c.hw = vmid;
    wreg(kRegCtx0BaseLo + 2 * vmid, static_cast<uint32_t>(c.phys | 1));   // navi31: root in VRAM (local)
    wreg(kRegCtx0BaseLo + 2 * vmid + 1, static_cast<uint32_t>(c.phys >> 32));
    wreg(kRegCtx0Cntl + vmid, 0x01fffe07);
    wreg(kRegCtx0StartLo + 2 * vmid, 0);
    wreg(kRegCtx0StartLo + 2 * vmid + 1, 0);
    wreg(kRegCtx0EndLo + 2 * vmid, 0xffffffff);
    wreg(kRegCtx0EndLo + 2 * vmid + 1, 0xf);
    if (gMmvmOn) {   // b67: the same tables for VCN on MMHUB
        wreg(kMmCtx0BaseLo + 2 * vmid, static_cast<uint32_t>(c.phys | 1));
        wreg(kMmCtx0BaseLo + 2 * vmid + 1, static_cast<uint32_t>(c.phys >> 32));
        wreg(kMmCtx0StartLo + 2 * vmid, 0);
        wreg(kMmCtx0StartLo + 2 * vmid + 1, 0);
        wreg(kMmCtx0EndLo + 2 * vmid, 0xffffffff);
        wreg(kMmCtx0EndLo + 2 * vmid + 1, 0xf);
        wreg(kMmCtx0Cntl + vmid, 0x01fffe07);
    }
    vmFlushTlb(vmid);
    return vmid;
}

// Returns a context once the GPU is past the client's last submission (bounded wait): the tables used so far are
// cleared, so nothing the client mapped stays reachable, and its hardware VMID (if any) is freed.
void PhxBringup::vmidFree(uint32_t handle, uint64_t lastSeq) {
    if (!vmCtx(handle))
        return;
    int waited = 0;
    for (; waited < 2000 && gpuRetired() < lastSeq; waited++)
        IOSleep(1);
    const uint64_t done = gpuRetired();
    {
        const VmCtx &cc = vm[handle - 1];
        IOLog("PhxBringup: context %u (pid %d %s) closed: hw VMID %u, last submission %llu, GPU at %llu after %d ms%s\n", handle,
              ctxPid[handle - 1], ctxName[handle - 1], cc.hw, lastSeq, done, waited,
              done < lastSeq ? " - NOT IDLE: tables kept, context retired for this boot" : "");
    }
    // A context whose work may still be running keeps its tables: clearing them turned a slow or stuck submission into a
    // VM fault on a root with nothing mapped (GCVM fault, VMID 11/12, 2026-10-24). It stays busy, so it is never reused;
    // its hardware VMID is lent again only once the GPU passes that VMID's last submission (vmBind).
    OSIncrementAtomic(&gVmCtxCloses);
    if (done < lastSeq) {
        OSIncrementAtomic(&gVmCtxRetired);
        return;
    }
    IOLockLock(gpuLock);
    VmCtx &c = vm[handle - 1];
    for (uint32_t k = 0; k < kPtChunks && c.chunk[k] && k * kPtChunkPages < c.used; k++) {
        const uint32_t n = c.used - k * kPtChunkPages < kPtChunkPages ? c.used - k * kPtChunkPages : kPtChunkPages;
        uint8_t *p = c.chunk[k];
        for (uint32_t o = 0; o < n * 4096u; o += 8)
            *reinterpret_cast<volatile uint64_t *>(p + o) = 0;
    }
    if (c.hw) {
        vmFlushTlb(c.hw);
        hwv[c.hw - kFirstUserVmid].ctx = 0;
        c.hw = 0;
    }
    {
        PhxVmEnv e = vmEnv();
        phxvm::reset(e, c);   // b77: the shadow of the tables just zeroed (parked ones included)
    }
    c.used = 1;
    c.busy = false;
    IOLockUnlock(gpuLock);
}

// debug.phxpt_stat (b77): reclaim counters, then per busy context its tables by kind and its leaves by valid PTEs
// (from the CPU shadow, no BAR0 reads). "empty" leaves are attached with no valid PTE: what reclaim can give back.
int PhxBringup::ptStat(char *b, size_t n) {
    int o = snprintf(b, n, "reclaim mode %d keep %d shadow %s; leaves fresh %d reclaimed %d reused %d cancelled %d; dirty %d drift %d no-pde %d; cap %d pool-full %d\n",
                     gPtReclaim, gPtKeep, ptShadowMem ? "on" : "off", gPtEv[phxvm::EvFresh], gPtEv[phxvm::EvReclaim], gPtEv[phxvm::EvReuse],
                     gPtEv[phxvm::EvCancel], gPtEv[phxvm::EvDirty], gPtEv[phxvm::EvDrift], gPtEv[phxvm::EvNoPde],
                     gPtEv[phxvm::EvCap], gPtEv[phxvm::EvPoolFull]);
    if (!gpuLock || o < 0)
        return o;
    IOLockLock(gpuLock);
    PhxVmEnv e = vmEnv();
    const uint64_t r = gpuRetired();
    for (uint32_t i = 0; i < kMaxCtx && static_cast<size_t>(o) + 320 < n; i++) {
        const VmCtx &c = vm[i];
        if (!c.busy)
            continue;
        const phxvm::Census z = phxvm::census(e, c, r);
        o += snprintf(b + o, n - static_cast<size_t>(o), "ctx %u pid %d %s vmid %u: tables %u/%u dirs %u leaves %u (empty %u, 1-16 %u, 17-255 %u, 256-512 %u) parked %u (ready %u) lost %u; mapped %llu MiB\n",
                      i + 1, ctxPid[i], ctxName[i], c.hw, z.tables, c.pages, z.dirs, z.leaves, z.empty, z.sparse, z.mid, z.dense,
                      z.parked, z.ready, z.lost, (z.livePtes * 4096) >> 20);
    }
    IOLockUnlock(gpuLock);
    return o;
}

IOReturn PhxBringup::adoptQueue(const uint64_t in[6]) {
    if (!fbAper || !doorbellMap || !mmio || !fbMcBase || !gpuLock)
        return kIOReturnNotReady;
    // in[5] = fence offset | queue index (0 compute, 1 gfx): the fence is 8-byte aligned, so bit 0 is free
    const uint64_t ringOff = in[0], ringDw = in[1], wbOff = in[2], dbByte = in[3], wptr = in[4], fenceOff = in[5] & ~1ull;
    const uint32_t qi = static_cast<uint32_t>(in[5] & 1);
    if (ringDw < 256 || ringDw > 65536 || (ringDw & (ringDw - 1)) || (ringOff & 0xFF) ||
        ringOff + ringDw * 4 > fbAperLen || (wbOff & 7) || wbOff + 32 > fbAperLen || (fenceOff & 7) ||
        fenceOff + 8 > fbAperLen || (dbByte & 7) || dbByte + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    GpuQueue &q = gq[qi];
    bzero(&q, sizeof(q));
    q.gfx = qi == 1;
    q.ringOff = static_cast<uint32_t>(ringOff);
    q.ringDw = static_cast<uint32_t>(ringDw);
    q.wbOff = static_cast<uint32_t>(wbOff);
    q.fenceOff = static_cast<uint32_t>(fenceOff);
    q.dbByte = dbByte;
    q.wptr = q.doneEnd = wptr;
    volatile uint32_t *fence = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
    fence[0] = static_cast<uint32_t>(gSeq);
    fence[1] = static_cast<uint32_t>(gSeq >> 32);
    hdpFlush();
    q.ready = true;
    IOLockUnlock(gpuLock);
    IOLog("PhxBringup: adopted %s queue ring=+0x%llx (%llu dw) wptr=%llu doorbell=+0x%llx\n",
          qi ? "gfx" : "compute", ringOff, ringDw, wptr, dbByte);
    return kIOReturnSuccess;
}

// PhxAccel (the NullMoth accelerator in its AMD configuration) starts only on a GPU marked as driven; matching is
// re-run so the accelerator personality, refused at boot, is offered the device again.
IOReturn PhxBringup::claimAccel() {
    if (!pci || !gpuReady())
        return kIOReturnNotReady;
    pci->setProperty("nmxgpu-claimed", kOSBooleanTrue);
    pci->registerService();
    IOLog("PhxBringup: GPU claimed for PhxAccel, matching re-run\n");
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vmMap(uint32_t handle, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOReturn r = kIOReturnSuccess;
    IOLockLock(gpuLock);
    // b53: the leaf table is walked once per 2 MiB and its entries written in a row; the walk reads each level's entry
    // from the tables in VRAM over BAR0, uncached, and doing it for every 4 KiB page cost ~1-2.5 ms per GEM_VA (the Wallpaper-pane lag).
    // b77: the walk and the b48 undo are phxvm::map (PhxVmTables.h)
    PhxVmEnv e = vmEnv();
    const uint64_t flags = kPteSysFlags | (static_cast<uint64_t>(gPteMtype & 7) << 48);
    bool noPage = false;
    const uint64_t done = phxvm::map(e, c, va, size, [&](uint64_t p) -> uint64_t {
        IOByteCount segLen = 0;
        const addr64_t pa = md->getPhysicalSegment(off + p, &segLen, kIOMemoryMapperNone);
        return pa ? (pa & kPtAddrMask) | flags : 0;
    }, &noPage);
    if (done < size) {
        r = noPage ? kIOReturnVMError : kIOReturnNoMemory;
        OSIncrementAtomic(&gVmMapFails);
        snprintf(gVmLastFail, sizeof gVmLastFail, "ctx %u pid %d %s: map va 0x%llx+0x%llx failed at +0x%llx (%s; ctx tables %u/%u, pool chunks %u/%u)",
                 handle, ctxPid[handle - 1], ctxName[handle - 1], va, size, done,
                 noPage ? "no physical page" : c.used >= c.pages ? "context table cap" : "page-table pool exhausted",
                 c.used, c.pages, ptPoolNext, static_cast<uint32_t>(PHX_VRAM_PT_SIZE / kPtChunkBytes));
    }
    if (c.hw)   // an unbound context is flushed when it is next bound
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return r;
}

IOReturn PhxBringup::vmUnmap(uint32_t handle, uint64_t va, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOLockLock(gpuLock);
    PhxVmEnv e = vmEnv();
    // b53: one walk per 2 MiB, as in vmMap. b77: with debug.phxpt_reclaim, emptied leaves are detached and parked; they
    // are reused only if this invalidation is acknowledged. Without a hardware VMID nothing is cached for the context:
    // the VMID was invalidated when it was taken away (vmBind), and again when one is next bound.
    const phxvm::Park mk = phxvm::unmap(e, c, va, size);
    if (c.hw && !vmFlushTlb(c.hw))
        phxvm::cancel(e, c, mk);
    IOLockUnlock(gpuLock);
    return kIOReturnSuccess;
}

