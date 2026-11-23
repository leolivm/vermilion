// SPDX-License-Identifier: MIT
// phx_recover.cpp: b127 automatic recovery of a queue stalled by a shader fault (debug.phxrecover, PhxRecover.h).
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.
//
// Lab KILL-DRAINED-PASS-1123: with debug.phxfaultdef=1 a shader store to an unmapped VA leaves its wave resident and its
// queue stalled with the memory pipe drained; one SQ_CMD kill of the VMID latched in GCVM_L2_PROTECTION_FAULT_STATUS
// recovers the GPU in under a second, WindowServer included. This file does that from the fence waits:
// - recoverPoll is called by every wait loop that runs without gpuLock: WAITSEQ (phx_wait.cpp), the client's BO free /
//   close / remap waits (waitGpuPastLast, waitRetired) and vmidFree. Never from a loop that holds gpuLock (gpuSubmit,
//   vmBind, faultDefCtl): it takes gpuLock itself for the status clear.
// - gRecLock is taken with a try: one thread polls and recovers at a time, every other waiter just keeps waiting.
// - The context is marked lost before the kill, as amdgpu_ring_soft_recovery sets the fence error before its loop: the
//   context cannot go away meanwhile, since its submission at a stalled queue head keeps its VMID bound (vmBind lends a
//   VMID again only once retired passed its last submission) and vmidFree keeps a context with unfinished work.
// - Nothing here resets anything: a kill that does not unstick within 10 ms is counted and logged, the stall is left as
//   it was (the HQD / CP reset paths are a later, separate step).

namespace {
struct PhxRecEnv {
    PhxBringup *p;
    uint32_t (PhxBringup::*r)(uint32_t) const;
    void (PhxBringup::*w)(uint32_t, uint32_t) const;
    uint64_t stalledAt;   // the global retired value of the stall
    uint32_t rd(uint32_t reg) { return (p->*r)(reg); }
    void wr(uint32_t reg, uint32_t v) { (p->*w)(reg, v); }
    void delayUs(uint32_t us) { IODelay(us); }
    bool moved() { return p->gpuRetired() > stalledAt; }
};

void phxRecNote(PhxRecLast &l, int verdict, uint64_t stalledNs, uint64_t retired, uint64_t emitted, uint32_t st,
                uint32_t alo, uint32_t ahi) {
    l = PhxRecLast{};
    absolutetime_to_nanoseconds(mach_absolute_time(), &l.upNs);
    clock_usec_t us;
    clock_get_calendar_microtime(&l.epoch, &us);
    l.verdict = verdict;
    l.kill = -1;
    l.stalledMs = stalledNs / 1000000ull;
    l.retiredBefore = l.retiredAfter = retired;
    l.emitted = emitted;
    l.status = l.statusAfter = st;
    l.addrLo = alo;
    l.addrHi = ahi;
}
}  // namespace

// The VMIDs (bit v) that ran the first unfinished entry of a GC queue and are lent to a live context. Lock-free like
// gpuRetiredMask: an entry is written before `tail` moves past it.
uint32_t PhxBringup::recoverOwned() const {
    phxrec::QView qv[3];
    for (uint32_t qi = 0; qi < 3; qi++) {
        const GpuQueue &q = gq[qi];
        const uint32_t head = q.head, tail = q.tail;
        qv[qi] = phxrec::QView{q.ready, q.vcn, q.ready && !q.vcn ? queueFence(q) : 0, head, tail, q.fseq, q.fvmid};
    }
    uint32_t live = 0;
    for (uint32_t i = 0; i < kNumUserVmids; i++) {
        const uint32_t h = hwv[i].ctx;
        if (h && h <= kMaxCtx && vm[h - 1].busy)
            live |= 1u << (kFirstUserVmid + i);
    }
    return phxrec::stalledHeads(qv, 3) & live;
}

void PhxBringup::recoverPoll() {
    if (!gRecOn || !gRecLock || !mmio || !gpuReady())
        return;
    if (!IOLockTryLock(gRecLock))
        return;   // another waiter is polling or recovering
    const uint64_t retired = gpuRetired(), emitted = gSeq;
    const uint64_t stalled = phxrec::track(gRecStall, retired, emitted, phxNowNs());
    const uint64_t thr = static_cast<uint64_t>(gRecMs) * 1000000ull;
    if (!stalled || stalled < thr || gRecStall.acted) {
        IOLockUnlock(gRecLock);
        return;
    }
    // past the threshold, once per stall: read the latched fault (a dummy read first, as Linux after a fault IV)
    (void)rreg(phxrec::kRegStatus);
    const uint32_t st = rreg(phxrec::kRegStatus), alo = rreg(phxrec::kRegAddrLo), ahi = rreg(phxrec::kRegAddrHi);
    const uint32_t owned = recoverOwned();
    const phxrec::Input in{gRecOn != 0, gFdOn != 0, stalled, thr, false, st, owned};
    uint32_t vmid = 0;
    const int v = phxrec::decide(in, &vmid);
    gRecStall.acted = true;   // a refusal is not retried until retired moves or the GPU goes idle
    const uint64_t va = ((static_cast<uint64_t>(ahi & 0xF) << 32) | alo) << 12;
    if (v != phxrec::kRecover) {
        if (v >= 0 && v <= phxrec::kRecover)
            gRecRefused[v]++;
        phxRecNote(gRecLastRefusal, v, stalled, retired, emitted, st, alo, ahi);
        IOLog("PhxBringup: recover: queue stalled %llu ms at %llu (emitted %llu): not acting (%s); STATUS 0x%08x ADDR VA 0x%llx owned 0x%04x faultdef %d\n",
              stalled / 1000000ull, retired, emitted, phxrec::verdictName(v), st, va, owned, gFdOn);
        IOLockUnlock(gRecLock);
        return;
    }
    // the guilty context: the one the latched VMID is lent to (decide checked it is live and at a stalled head)
    const uint32_t h = hwv[vmid - kFirstUserVmid].ctx;
    PhxRecLast &l = gRecLast;
    phxRecNote(l, v, stalled, retired, emitted, st, alo, ahi);
    l.vmid = vmid;
    l.ctx = h;
    if (h && h <= kMaxCtx) {
        l.pid = ctxPid[h - 1];
        strlcpy(l.name, ctxName[h - 1], sizeof l.name);
        lost[h - 1] = 1;
    }
    l.ws = !strncmp(l.name, "WindowServer", sizeof l.name);
    if (l.ws)
        IOLog("PhxBringup: recover: *** THE LATCHED FAULT NAMES WINDOWSERVER (VMID %u, pid %d): killing its waves and marking its context lost ***\n",
              vmid, l.pid);
    PhxRecEnv e{this, &PhxBringup::rreg, &PhxBringup::wreg, retired};
    const uint64_t k0 = phxNowNs();
    uint32_t writes = 0;
    l.kill = phxrec::kill(e, vmid, &writes);
    l.killUs = static_cast<uint32_t>((phxNowNs() - k0) / 1000ull);
    l.kills = writes;
    gRecKills += writes;
    l.retiredAfter = gpuRetired();
    if (l.kill == phxrec::kKillOk) {
        gRecRecoveries++;
        IOLockLock(gpuLock);   // the CNTL read-modify-write must not interleave with faultDefCtl's CNTL writes
        l.statusAfter = phxrec::clearStatus(e);
        IOLockUnlock(gpuLock);
    } else {
        gRecFailures++;   // nothing else: the latched status is kept as evidence, no reset
    }
    IOLog("PhxBringup: recover: queue stalled %llu ms at %llu (emitted %llu), STATUS 0x%08x VMID %u CID %u ADDR VA 0x%llx = ctx %u pid %d %s: %u kill(s) 0x%08x in %u us -> %s, retired %llu, STATUS after 0x%08x\n",
          stalled / 1000000ull, retired, emitted, st, vmid, phxrec::decode(st).cid, va, h, l.pid, l.name, writes,
          phxrec::killValue(vmid), l.killUs, l.kill == phxrec::kKillOk ? "RECOVERED" : "FAILED (retired did not move in 10 ms; no reset)",
          l.retiredAfter, l.statusAfter);
    IOLockUnlock(gRecLock);
}

int PhxBringup::recoverCtl(int v) {
    if (v != 0 && v != 1)
        return EINVAL;
    if (!gRecLock)
        return ENXIO;
    IOLockLock(gRecLock);
    if (v && !gRecOn)
        gRecStall = phxrec::Stall{};   // a stall that began while off is timed from now
    gRecOn = v;
    IOLockUnlock(gRecLock);
    IOLog("PhxBringup: recover %d (faultdef %d, %d ms)\n", v, gFdOn, gRecMs);
    return 0;
}

int PhxBringup::recoverStat(char *b, size_t n) {
    if (gRecLock) IOLockLock(gRecLock);   // a consistent copy: a recovery holds the lock for 10 ms at most
    const PhxRecLast l = gRecLast, f = gRecLastRefusal;
    if (gRecLock) IOLockUnlock(gRecLock);
    const phxrec::Fault d = phxrec::decode(l.status);
    int o = snprintf(b, n, "recover %d ms %d faultdef %d (boot-arg phxrecover=1 %s, phxfaultdef=1 %s); recoveries %u kills %u failures %u; "
                     "refused: faultdef-off %u no-fault %u not-user %u not-owned %u; lost-context submits refused %u\n",
                     gRecOn, gRecMs, gFdOn, gRecBoot ? "yes" : "no", gFdBoot ? "yes" : "no",
                     gRecRecoveries, gRecKills, gRecFailures, gRecRefused[phxrec::kFaultDefOff], gRecRefused[phxrec::kNoFault],
                     gRecRefused[phxrec::kNotUser], gRecRefused[phxrec::kNotOwned], gRecLostSubmits);
    if (o < 0 || static_cast<size_t>(o) >= n)
        return o;
    if (l.verdict != phxrec::kRecover) {
        o += snprintf(b + o, n - static_cast<size_t>(o), "last recovery: none\n");
    } else {
        o += snprintf(b + o, n - static_cast<size_t>(o),
                      "last recovery: uptime %llu.%03llu s epoch %lu: %s VMID %u ctx %u pid %d %s%s; STATUS 0x%08x (CID %u RW %u mapping %u perm 0x%x) "
                      "ADDR VA 0x%llx; stalled %llu ms at retired %llu (emitted %llu); kills %u (0x%08x) in %u us; retired after %llu; STATUS after 0x%08x\n",
                      l.upNs / 1000000000ull, (l.upNs / 1000000ull) % 1000ull, static_cast<unsigned long>(l.epoch),
                      l.kill == phxrec::kKillOk ? "RECOVERED" : "FAILED", l.vmid, l.ctx, l.pid, l.name, l.ws ? " (WINDOWSERVER)" : "",
                      l.status, d.cid, d.rw, d.mapping, d.perm, ((static_cast<uint64_t>(l.addrHi & 0xF) << 32) | l.addrLo) << 12,
                      l.stalledMs, l.retiredBefore, l.emitted, l.kills, phxrec::killValue(l.vmid), l.killUs, l.retiredAfter, l.statusAfter);
    }
    if (o < 0 || static_cast<size_t>(o) >= n)
        return o;
    if (!f.upNs)
        o += snprintf(b + o, n - static_cast<size_t>(o), "last refusal: none\n");
    else
        o += snprintf(b + o, n - static_cast<size_t>(o), "last refusal: uptime %llu.%03llu s epoch %lu: %s; stalled %llu ms at retired %llu (emitted %llu); STATUS 0x%08x ADDR VA 0x%llx\n",
                      f.upNs / 1000000000ull, (f.upNs / 1000000ull) % 1000ull, static_cast<unsigned long>(f.epoch), phxrec::verdictName(f.verdict),
                      f.stalledMs, f.retiredBefore, f.emitted, f.status, ((static_cast<uint64_t>(f.addrHi & 0xF) << 32) | f.addrLo) << 12);
    if (o < 0 || static_cast<size_t>(o) >= n)
        return o;
    o += snprintf(b + o, n - static_cast<size_t>(o), "lost contexts:");
    bool any = false;
    for (uint32_t i = 0; i < kMaxCtx && o > 0 && static_cast<size_t>(o) + 48 < n; i++) {
        if (!lost[i] || !vm[i].busy)
            continue;
        any = true;
        o += snprintf(b + o, n - static_cast<size_t>(o), " ctx %u pid %d %s;", i + 1, ctxPid[i], ctxName[i]);
    }
    if (o > 0 && static_cast<size_t>(o) < n)
        o += snprintf(b + o, n - static_cast<size_t>(o), "%s", any ? "" : " none");
    return o;
}
