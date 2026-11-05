// SPDX-License-Identifier: MIT
// phx_wait.cpp: BO cache flushes around submissions and WAITSEQ (spin, timed steps, IRQ wait, scoped wait).
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

// Write back and invalidate the CPU caches over every BO: CPU-written data must be in memory before the GPU reads
// it, and the CPU must not read stale lines after the GPU wrote. Coarse: WindowServer's BOs (full-screen surfaces
// among them) made this its hottest loop, hence debug.phxbos_flush.
void IOAccelPhxNativeClient::flushBos() {
    const int mode = gBosFlush;
    if (!mode)
        return;
    uint64_t bytes = 0;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++) {
        // host imports: the CPU's own pages, which the GPU reads and writes snooped. Mode 3 flushes them too, through
        // a kernel mapping made for the purpose: whether a CPU read after the GPU wrote (the display pipe's copy of
        // WindowServer's frame) can still hit stale cache lines (2026-10-25, 64-byte runs of old pixels in motion)
        if (!bos[h].kva) {
            if (mode != 3 || !bos[h].host || !bos[h].md)
                continue;
            IOMemoryMap *m = bos[h].md->map(kIOMapReadOnly);
            if (!m)
                continue;
            const uint8_t *p = reinterpret_cast<const uint8_t *>(m->getVirtualAddress());
            const uint64_t len = m->getLength() < bos[h].size ? m->getLength() : bos[h].size;
            for (uint64_t o = 0; o < len; o += 64)
                __asm__ __volatile__("clflushopt (%0)" ::"r"(p + o) : "memory");
            __asm__ __volatile__("mfence" ::: "memory");
            m->release();
            bytes += len;
            OSAddAtomic64(1, &gBosFlushHost);
            continue;
        }
        const uint8_t *p = bos[h].kva;
        if (mode == 1 || mode == 3) {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflushopt (%0)" ::"r"(p + o) : "memory");
        } else {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflush (%0)" ::"r"(p + o) : "memory");
        }
        bytes += bos[h].size;
    }
    __asm__ __volatile__("mfence" ::: "memory");
    OSAddAtomic64((SInt64)bytes, &gBosFlushBytes);
}

IOReturn IOAccelPhxNativeClient::doWaitSeq(IOExternalMethodArguments *a) {
    if (!hello)
        return kIOReturnNotReady;
    if (!shape(a, 3, 3, 0, 0))
        return kIOReturnBadArgument;
    const uint64_t l0 = phxNowNs();
    IOLockLock(lock);
    const uint64_t l1 = phxNowNs();
    uint64_t target = a->scalarInput[0];
    const uint64_t em = emitted;
    IOLockUnlock(lock);
    phxCallNote(64, l1 - l0, phxNowNs() - l1);
    if (target == PHXN_SEQ_LAST)
        target = em;
    if (target > em)
        return kIOReturnBadArgument;
    if (owner->gpuReady()) {
        // E8: poll the RELEASE_MEM fence the adopted queue writes after each submission
        uint64_t timeout = a->scalarInput[1];
        if (timeout > PHXN_WAIT_CAP_NS)
            timeout = PHXN_WAIT_CAP_NS;
        // b54: spin for the first 20 us only, then sleep in 50 us timed steps (IOLockSleepDeadline on a private lock that
        // nothing signals: the deadline wakes it). The loop spun IODelay(100) for the whole wait, ~1.0-1.3 s of CPU per
        // System Settings Wallpaper-pane open across WindowServer and the apps. The timeout is now real time.
        const bool scoped = gWaitScoped && qmask;
        const uint32_t qm = scoped ? qmask : ~0u;
        uint64_t retired = owner->gpuRetiredMask(qm);
        const uint64_t w0 = phxNowNs();
        int slept = -1;   // b64: -1 = never slept, 0 = last sleep was a timed step, 1 = an IRQ wait
        while (retired < target) {
            const uint64_t waited = phxNowNs() - w0;
            if (waited >= timeout)
                break;
            if (waited < static_cast<uint64_t>(gWaitSpinUs) * 1000 || !gWaitSleepLock) {
                IODelay(5);
            } else if (gWaitIrq && gIrqOn && gEopIrq && !gIrqStorm) {
                // b63: sleep until the EOP interrupt; re-check after assert_wait closes the race with the filter
                event_t ev = reinterpret_cast<event_t>(const_cast<SInt32 *>(&gFenceEvt));
                uint64_t d;
                clock_interval_to_deadline(gWaitIrqDlUs > 0 ? static_cast<uint32_t>(gWaitIrqDlUs) : 2000u, kMicrosecondScale, &d);
                assert_wait_deadline(ev, THREAD_UNINT, d);
                const bool self = owner->gpuRetiredMask(qm) >= target;
                if (self) {
                    thread_wakeup(ev);
                    OSIncrementAtomic64(&gWiSelf);
                }
                OSIncrementAtomic64(&gWiSleeps);
                slept = 1;
                const wait_result_t wr = thread_block(THREAD_CONTINUE_NULL);
                if (self) {
                } else if (wr == THREAD_TIMED_OUT) {
                    OSIncrementAtomic64(&gWiDeadline);
                    if (owner->gpuRetiredMask(qm) >= target)
                        OSIncrementAtomic64(&gWiMissed);
                } else {
                    OSIncrementAtomic64(&gWiIrqWakes);
                    uint64_t ns;
                    absolutetime_to_nanoseconds(mach_absolute_time() - gIrqEopAbs, &ns);
                    if (ns < 100000000ull) {
                        OSAddAtomic64(static_cast<SInt64>(ns), &gWiLatNs);
                        if (static_cast<SInt64>(ns) > gWiLatMax)
                            gWiLatMax = static_cast<SInt64>(ns);
                    }
                }
            } else {
                uint64_t d;
                slept = 0;
                clock_interval_to_deadline(gWaitStepUs > 0 ? gWaitStepUs : 50, kMicrosecondScale, &d);
                IOLockLock(gWaitSleepLock);
                IOLockSleepDeadline(gWaitSleepLock, &gWaitSleepLock, d, THREAD_UNINT);
                IOLockUnlock(gWaitSleepLock);
            }
            retired = owner->gpuRetiredMask(qm);
        }
        const bool reached = retired >= target;
        if (scoped) {   // what goes back to the shim stays the global minimum: its syncobj bookkeeping marks up to it
            OSIncrementAtomic64(&gWsWaits);
            const uint64_t g = owner->gpuRetired();
            if (reached && g < target)
                OSIncrementAtomic64(&gWsEarly);
            retired = g;
        }
        if (reached) {
            if (slept < 0) {
                OSIncrementAtomic64(&gWaitSpinDone);
            } else if (gIhOn && gEopIrq) {
                OSIncrementAtomic64(&gWaitSlept[slept]);
                phxLatNote(static_cast<unsigned>(slept), static_cast<uint32_t>(target), mach_absolute_time());
            }
            // drop CPU cache lines so the CPU sees what the GPU wrote; under the lock, as another thread of the same
            // client (WindowServer) freed a BO mid-flush and the walk dereferenced its NULL descriptor (panic 2026-10-20)
            const uint64_t r0 = phxNowNs();
            IOLockLock(lock);
            const uint64_t r1 = phxNowNs();
            reap();
            flushBos();
            if (pendFenceSeq && pendFenceSeq <= retired) {
                Bo *fb = findBo(pendFenceHandle);
                if (fb && fb->kva && pendFenceOffset + 8 <= fb->size)
                    bcopy(&pendFenceSeq, fb->kva + pendFenceOffset, 8);
                pendFenceSeq = 0;
            }
            const uint64_t r2 = phxNowNs();
            IOLockUnlock(lock);
            phxCallNote(65, r1 - r0, r2 - r1);
            phxCallNote(67, 0, phxNowNs() - r0);
        }
        if (!reached) {
            // a stalled fence: log the GPU's VM fault state once per stall so a hang is told apart from a slow frame
            static uint64_t saidAt;
            if (saidAt != retired) {
                saidAt = retired;
                uint32_t f[3] = {0, 0, 0}, g = 0;
                owner->readRegs(0x2828, 3, f);   // GCVM_L2_PROTECTION_FAULT_STATUS, ADDR_LO32, ADDR_HI32
                owner->readRegs(0x2004, 1, &g);  // GRBM_STATUS
                int fpid = 0;
                const char *fname = owner->hwVmidOwner((f[0] >> 20) & 15, &fpid);
                IOLog("PhxBringup: fence stalled at %llu (want %llu, emitted %llu): GRBM_STATUS 0x%08x VM fault 0x%08x VMID %u (pid %d %s) CID %u VA 0x%llx\n",
                      retired, target, em, g, f[0], (f[0] >> 20) & 15, fpid, fname, (f[0] >> 9) & 0x1ff,
                      ((static_cast<uint64_t>(f[2] & 0xf) << 32) | f[1]) << 12);
            }
        }
        a->scalarOutput[0] = !reached;
        a->scalarOutput[1] = retired;
        a->scalarOutput[2] = em;
        return kIOReturnSuccess;
    }
    a->scalarOutput[0] = 0;   // not busy: E1 retires every submission at once
    a->scalarOutput[1] = em;
    a->scalarOutput[2] = em;
    return kIOReturnSuccess;
}

