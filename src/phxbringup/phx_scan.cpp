// SPDX-License-Identifier: MIT
// phx_scan.cpp: scanout (selectors 9..14, service side): VRAM slot mapping, HUBP0 flips, poll/watchdog, acquire/
// present/status/release.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

// A client's mapping of VRAM (a scanout slot) instead of system pages: local PTEs, as Linux writes them for VRAM
// (amdgpu_vm: address = VRAM offset + vram_base_offset, here fbPhys) with VALID|EXEC|READ|WRITE and no SYSTEM/SNOOPED.
IOReturn PhxBringup::vmMapVram(uint32_t handle, uint64_t va, uint64_t vramOff, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOReturn r = kIOReturnSuccess;
    IOLockLock(gpuLock);
    PhxVmEnv e = vmEnv();   // b53 walk + b48 undo, phxvm::map (b77)
    bool noPage = false;
    const uint64_t base = fbPhys + vramOff;
    if (phxvm::map(e, c, va, size, [&](uint64_t p) -> uint64_t { return ((base + p) & kPtAddrMask) | 0x71ull; }, &noPage) < size) {
        r = kIOReturnNoMemory;
        OSIncrementAtomic(&gVmMapFails);
    }
    if (c.hw)
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return r;
}

// ---- scanout (selectors 9..14) ----------------------------------------------------------------------------------
// The display pipe copies each WindowServer frame on the CPU, and that copy could read a frame the GPU was still
// drawing (the family's wait for the plane surface's last GPU write never fires on our stack, RE'd 2026-10-25).
// Cobalt64 instead has the frame copied on the GPU, at the end of the command buffer that drew it, into a VRAM slot,
// and flips HUBP0 to that slot once the GPU is done. These are its selectors, with its structs, so RADV's
// radv_darwin_scanout_* work unchanged. Slots live at fixed VRAM offsets (PHXN_SCAN_SLOT_VRAM); a client's BO is
// rebound onto one (its PTEs point at the slot's VRAM). The console surface A (VRAM 0) keeps receiving the display
// pipe's copies (debug.phxfb_flip must be 0 for Acquire), so a restore is a flip back to A. HUBP0 registers are
// absolute dwords (DCN 3.2 segment 0x34c0 + dcn_3_2_0_offset.h), the flip sequence is Linux's hubp3 one.
uint64_t PhxBringup::scanFrameLocked() {
    const uint32_t f = rreg(kOtg0StatusFrameCount) & 0xffffff;
    scan.frameExt += (f - scan.frameLast) & 0xffffff;
    scan.frameLast = f;
    return scan.frameExt;
}

void PhxBringup::scanFlipLocked(uint64_t mc, bool immediate) {
    const uint32_t fc = rreg(kHubpreq0FlipControl);
    wreg(kHubpreq0FlipControl, immediate ? (fc | 2u) : (fc & ~2u));
    wreg(kHubpreq0PrimaryAddrHigh, static_cast<uint32_t>(mc >> 32) & 0xffff);
    wreg(kHubpreq0PrimaryAddr, static_cast<uint32_t>(mc));   // the low half arms the latch
}

void PhxBringup::scanPollLocked() {
    if (scan.pending == PHXN_SCAN_NO_SLOT)
        return;
    if (rreg(kHubpreq0FlipControl) & 0x100)
        return;
    if (scanEarliest() != scan.slotMc[scan.pending]) {   // b74: FLIP_PENDING cleared, EARLIEST_INUSE not there yet
        OSIncrementAtomic64(&gScanEarliestLag);
        if (!gScanLatchFix)
            return;
    }
    const uint32_t k = scan.pending;
    gScanGhost = 0xffffffffu;
    scan.front = k;
    scan.pending = PHXN_SCAN_NO_SLOT;
    scan.latched++;
    scan.slotLatches[k]++;
    scan.slotLatchedFrame[k] = scanFrameLocked();
    uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    if (!scan.firstLatchNs) scan.firstLatchNs = ns;
    scan.lastLatchNs = ns;
}

// Back to the console surface A. true once HUBP0 is scanning it.
bool PhxBringup::scanRestoreLocked() {
    if (!scan.acquired)
        return true;
    scanFlipLocked(scan.consoleMc, false);
    bool ok = false;
    for (int i = 0; i < 500 && !ok; i++) {
        ok = !(rreg(kHubpreq0FlipControl) & 0x100) && scanEarliest() == scan.consoleMc;
        if (!ok) IODelay(100);
    }
    if (!ok) {   // a vsync flip that never latched: put A up at once
        scanFlipLocked(scan.consoleMc, true);
        IODelay(1000);
        ok = scanEarliest() == scan.consoleMc;
    }
    scan.acquired = false;
    scan.front = scan.pending = PHXN_SCAN_NO_SLOT;
    scan.restores++;
    IOLog("PhxBringup: scanout restored to the console (%s)\n", ok ? "verified" : "NOT verified");
    return ok;
}

void PhxBringup::scanWatchFire(thread_call_param_t p0, thread_call_param_t) {
    PhxBringup *me = static_cast<PhxBringup *>(p0);
    IOLockLock(me->scanLock);
    if (me->scan.acquired) {
        me->scanPollLocked();
        const uint64_t plane = me->scanPlane();
        bool known = plane == me->scan.consoleMc;
        for (uint32_t k = 0; k < PHXN_SCAN_MAX_SLOTS; k++) known |= me->scan.slotUsed[k] && plane == me->scan.slotMc[k];
        uint64_t idleNs; absolutetime_to_nanoseconds(mach_absolute_time() - me->scan.lastCallAbs, &idleNs);
        if (!gScanOn || !known || idleNs > 5000000000ull) {
            IOLog("PhxBringup: scanout watchdog restores the console (%s)\n", !gScanOn ? "debug.phxscan=0" : !known ? "HUBP0 points elsewhere" : "5 s without a call");
            me->scanRestoreLocked();
            me->scan.watchdogRestores++;
        }
    }
    const bool again = me->scan.acquired;
    IOLockUnlock(me->scanLock);
    if (again) {
        uint64_t d; clock_interval_to_deadline(50, kMillisecondScale, &d);
        thread_call_enter_delayed(me->scanWatch, d);
    }
}

void PhxBringup::scanQuery(phxn_scan_query *q) {
    bzero(q, sizeof(*q));
    if (!mmio) return;
    const uint32_t vb = rreg(kOtg0VBlankStartEnd), vbs = vb & 0x7fff, vbe = (vb >> 16) & 0x7fff;
    q->h_total = rreg(kOtg0HTotal) + 1;
    q->v_total = rreg(kOtg0VTotal) + 1;
    q->pitch_px = (rreg(kHubpreq0SurfacePitch) & 0x3fff) + 1;
    q->hubp_format = rreg(kHubp0SurfaceConfig) & 0x7f;
    q->sw_mode = rreg(kHubp0TilingConfig) & 0x1f;
    q->dcc_en = rreg(kHubpreq0SurfaceControl) & 1;
    q->plane_w = q->h_active = q->pitch_px;
    q->plane_h = q->v_active = vbs > vbe ? vbs - vbe : 0;
    q->flags = ((rreg(kOtg0Control) & 1) ? PHXN_SCANQ_LIT : 0) | PHXN_SCANQ_NATIVE;
    const uint64_t bytes = static_cast<uint64_t>(q->pitch_px) * 4 * q->plane_h;
    if (q->hubp_format == PHXN_SCAN_FMT_ARGB8888 && !q->sw_mode && !q->dcc_en && q->plane_h && bytes <= PHXN_SCAN_SLOT_BYTES)
        q->flags |= PHXN_SCANQ_GEOM_OK;
    if (scanLock) IOLockLock(scanLock);
    q->frame_count = scanFrameLocked();
    q->acquired = scan.acquired;
    if (scan.acquired) q->flags |= PHXN_SCANQ_ACQUIRED;
    q->console_mc = scan.acquired ? scan.consoleMc : scanPlane();
    if (scanLock) IOLockUnlock(scanLock);
    q->plane_mc = scanPlane();
    q->earliest_mc = scanEarliest();
    q->otg = 0;
}

IOReturn PhxBringup::scanAcquire(const void *who, uint64_t out[2]) {
    if (!mmio || !fbAper || !gScanOn)
        return kIOReturnUnsupported;
    if (!scanLock) scanLock = IOLockAlloc();
    if (!scanWatch) scanWatch = thread_call_allocate(scanWatchFire, this);
    if (!scanLock || !scanWatch)
        return kIOReturnNoMemory;
    phxn_scan_query q;
    scanQuery(&q);
    IOLockLock(scanLock);
    IOReturn r = kIOReturnSuccess;
    const uint64_t fbBase = static_cast<uint64_t>(rreg(kDcnVmFbLocationBase) & 0xffffff) << 24;
    if (scan.acquired) {
        r = kIOReturnBusy;
    } else if (!(q.flags & PHXN_SCANQ_GEOM_OK) || !(q.flags & PHXN_SCANQ_LIT)) {
        r = kIOReturnUnsupported;
    } else if (q.plane_mc != fbBase || q.earliest_mc != fbBase || (rreg(kHubpreq0FlipControl) & 0x100)) {
        r = kIOReturnNotReady;   // the console surface A must be on screen and idle (debug.phxfb_flip=0)
    } else {
        const void *prev = scan.owner;
        bzero(&scan, sizeof(scan));
        scan.frameLast = rreg(kOtg0StatusFrameCount) & 0xffffff;
        scan.owner = who;
        scan.acquired = true;
        scan.consoleMc = q.plane_mc;
        scan.fbBase = fbBase;
        scan.pitchPx = q.pitch_px;
        scan.planeH = q.plane_h;
        scan.front = scan.pending = PHXN_SCAN_NO_SLOT;
        scan.lastCallAbs = mach_absolute_time();
        out[0] = scan.consoleMc;
        out[1] = scanFrameLocked();
        IOLog("PhxBringup: scanout acquired (console MC 0x%llx, %ux%u)%s\n", scan.consoleMc, q.pitch_px, q.plane_h, prev ? " (previous owner dropped)" : "");
    }
    IOLockUnlock(scanLock);
    if (r == kIOReturnSuccess) {
        uint64_t d; clock_interval_to_deadline(50, kMillisecondScale, &d);
        thread_call_enter_delayed(scanWatch, d);
    }
    return r;
}

IOReturn PhxBringup::scanAllocSlot(const void *who, const phxn_scan_reg *r, uint32_t *slot, uint64_t *mc, uint64_t *vramOff) {
    if (!scanLock || !gScanOn)
        return kIOReturnUnsupported;
    IOLockLock(scanLock);
    IOReturn ret = kIOReturnSuccess;
    if (!scan.acquired || scan.owner != who) {
        ret = kIOReturnNotPermitted;
    } else if (r->reserved0 || r->offset || r->format != PHXN_SCAN_FMT_ARGB8888 || r->pitch_bytes != scan.pitchPx * 4 ||
               r->width != scan.pitchPx || r->height != scan.planeH ||
               static_cast<uint64_t>(r->pitch_bytes) * r->height > PHXN_SCAN_SLOT_BYTES) {
        ret = kIOReturnBadArgument;
    } else {
        uint32_t k = 0;
        while (k < PHXN_SCAN_MAX_SLOTS && scan.slotUsed[k]) k++;
        if (k == PHXN_SCAN_MAX_SLOTS) {
            ret = kIOReturnNoResources;
        } else {
            scan.slotUsed[k] = true;
            *vramOff = PHXN_SCAN_SLOT_VRAM(k);
            scan.slotMc[k] = scan.fbBase + *vramOff;
            *slot = k;
            *mc = scan.slotMc[k];
            scan.lastCallAbs = mach_absolute_time();
        }
    }
    IOLockUnlock(scanLock);
    return ret;
}

IOReturn PhxBringup::scanPresent(const void *who, uint32_t slot, uint64_t out[3]) {
    if (!scanLock || !gScanOn)
        return kIOReturnUnsupported;
    IOLockLock(scanLock);
    IOReturn r = kIOReturnSuccess;
    if (!scan.acquired || scan.owner != who) {
        r = kIOReturnNotPermitted;
    } else if (slot >= PHXN_SCAN_MAX_SLOTS || !scan.slotUsed[slot]) {
        scan.refused++;
        r = kIOReturnBadArgument;
    } else {
        scanPollLocked();
        if (scan.pending != PHXN_SCAN_NO_SLOT) {
            scan.replaced++;   // a flip not latched yet: the newest one wins
            if (!(rreg(kHubpreq0FlipControl) & 0x100)) {   // b74: ...but the hardware already took it: that slot is on screen
                OSIncrementAtomic64(&gScanAcceptedForgotten);
                gScanGhost = scan.pending;
            }
        }
        scanFlipLocked(scan.slotMc[slot], false);
        scan.pending = slot;
        scan.presents++;
        scan.slotPresents[slot]++;
        scan.lastCallAbs = mach_absolute_time();
        out[0] = scan.presents;
        out[1] = scanFrameLocked() + 1;
        out[2] = 0;
    }
    IOLockUnlock(scanLock);
    return r;
}

IOReturn PhxBringup::scanStatus(const void *who, phxn_scan_status *st) {
    bzero(st, sizeof(*st));
    if (!scanLock)
        return kIOReturnUnsupported;
    IOLockLock(scanLock);
    if (scan.owner == who) scan.lastCallAbs = mach_absolute_time();
    if (scan.acquired) scanPollLocked();
    const uint64_t earliest = scanEarliest();
    st->acquired = scan.acquired && scan.owner == who;
    st->front_slot = scan.front;
    st->pending_slot = scan.pending;
    st->frame_count = scanFrameLocked();
    st->console_mc = scan.consoleMc;
    st->plane_mc = scanPlane();
    st->earliest_mc = earliest;
    st->presents = scan.presents; st->latched = scan.latched; st->replaced = scan.replaced; st->refused = scan.refused;
    st->latch_poll = scan.latched;
    st->first_latch_ns = scan.firstLatchNs; st->last_latch_ns = scan.lastLatchNs;
    st->watchdog_restores = scan.watchdogRestores;
    for (uint32_t k = 0; k < PHXN_SCAN_MAX_SLOTS; k++) {
        phxn_scan_slot &o = st->slot[k];
        o.mc = scan.slotMc[k];
        o.used = scan.slotUsed[k];
        o.presents = scan.slotPresents[k];
        o.latches = scan.slotLatches[k];
        o.latched_frame = scan.slotLatchedFrame[k];
        if (!scan.slotUsed[k]) continue;
        const bool pend = scan.acquired && scan.pending == k, inuse = earliest == scan.slotMc[k];
        const bool front = gScanLatchFix && scan.front == k;   // b74: the latched front is on screen until the next latch
        const bool reuse = !pend && !inuse && !front;
        if (reuse && k == gScanGhost)
            OSIncrementAtomic64(&gScanReusableGhost);
        o.flags = (pend ? PHXN_SCANSLOT_PENDING : 0) | (inuse ? PHXN_SCANSLOT_INUSE : 0) | (reuse ? PHXN_SCANSLOT_REUSABLE : 0);
    }
    IOLockUnlock(scanLock);
    return kIOReturnSuccess;
}

IOReturn PhxBringup::scanRelease(const void *who, uint64_t out[2]) {
    if (out) { out[0] = 1; out[1] = mmio ? scanPlane() : 0; }
    if (!scanLock)
        return kIOReturnSuccess;
    IOLockLock(scanLock);
    if (scan.owner == who) {
        const bool ok = scanRestoreLocked();
        if (out) { out[0] = ok; out[1] = scanPlane(); }
        scan.owner = nullptr;
        for (uint32_t k = 0; k < PHXN_SCAN_MAX_SLOTS; k++) scan.slotUsed[k] = false;
    }
    IOLockUnlock(scanLock);
    return kIOReturnSuccess;
}

