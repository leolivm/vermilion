// SPDX-License-Identifier: MIT
// PhxRecover.h: automatic recovery of a GFX ring stalled by a shader fault (debug.phxrecover, kext b127).
//
// Proven by hand on the box (lab KILL-DRAINED-PASS-1123): with the GFX hub's fault default page on (debug.phxfaultdef,
// PhxFaultDef.h) a shader store to an unmapped VA drains out of the TCP, but the faulting wave stays resident and its
// queue never retires. One SQ_CMD write that kills every wave of the VMID latched in GCVM_L2_PROTECTION_FAULT_STATUS
// (CMD 3 KILL, MODE 1 BROADCAST, CHECK_VMID) lets the queue go on: its RELEASE_MEM fence retires and WindowServer keeps
// drawing. With the default page off the same kill does nothing (TCP-KILL-1123), hence the faultdef gate below.
// Linux does the same on a gfx ring timeout: amdgpu_ring_soft_recovery (v6.8 amdgpu_ring.c) repeats
// gfx_v11_0_ring_soft_recovery (SQ_CMD = KILL | BROADCAST | CHECK_VMID | VM_ID) for up to 10 ms until the fence signals.
// The latched status is then cleared as gmc_v11_0_process_interrupt does (WREG32_P(vm_l2_pro_fault_cntl, 1, ~1)), here
// as a pulse that puts CLEAR_PROTECTION_FAULT_STATUS_ADDR back to 0 so CNTL reads what debug.phxfaultdef wrote.
//
// Kernel-free on purpose: tests/mock/recover_test.sh runs this exact code. The kext supplies an environment E with:
//   uint32_t rd(uint32_t reg);            a GC register (absolute dword index)
//   void wr(uint32_t reg, uint32_t v);
//   void delayUs(uint32_t us);
//   bool moved();                         the stalled sequence retired (the global retired went past it)
// Registers written: SQ_CMD (kill) and CNTL (the clear pulse) only. Read: STATUS, CNTL. Nothing on MMHUB, no reset.
#pragma once
#include <stdint.h>

namespace phxrec {

// gc_11_0_0 offsets + the GC base 0x1260
constexpr uint32_t kRegSqCmd = 0x237B;                         // SQ_CMD (0x111b), write-only (reads 0xbebebeef)
constexpr uint32_t kRegCntl = 0x2824;                          // GCVM_L2_PROTECTION_FAULT_CNTL
constexpr uint32_t kRegStatus = 0x2828;                        // GCVM_L2_PROTECTION_FAULT_STATUS
constexpr uint32_t kRegAddrLo = 0x2829, kRegAddrHi = 0x282A;   // GCVM_L2_PROTECTION_FAULT_ADDR_LO32 / HI32 (page >> 12)
constexpr uint32_t kCntlClear = 0x1u;                          // CNTL CLEAR_PROTECTION_FAULT_STATUS_ADDR

constexpr uint32_t kFirstUserVmid = 8, kLastUserVmid = 15;     // the VMIDs the kext lends to client contexts
// SQ_CMD: CMD 3 (SQ_IND_CMD_CMD_KILL) | MODE 1 (SQ_IND_CMD_MODE_BROADCAST) << 4 | CHECK_VMID << 7; VM_ID << 28
constexpr uint32_t kKillBase = 0x00000093u;
constexpr uint32_t kKillTries = 10, kKillStepUs = 1000;        // one write per ms, 10 ms at most (amdgpu_ring_soft_recovery)

// GCVM_L2_PROTECTION_FAULT_STATUS (gc_11_0_0_sh_mask.h)
struct Fault {
    uint32_t raw;
    bool more;          // bit 0 MORE_FAULTS
    uint32_t walker;    // bits 1-3 WALKER_ERROR
    uint32_t perm;      // bits 4-7 PERMISSION_FAULTS
    bool mapping;       // bit 8 MAPPING_ERROR
    uint32_t cid;       // bits 9-17 CID (8 = TCP)
    bool rw;            // bit 18 RW (1 = write)
    uint32_t vmid;      // bits 20-23 VMID
};

inline Fault decode(uint32_t st) {
    return Fault{st, (st & 1u) != 0, (st >> 1) & 7u, (st >> 4) & 0xFu, ((st >> 8) & 1u) != 0, (st >> 9) & 0x1FFu,
                 ((st >> 18) & 1u) != 0, (st >> 20) & 0xFu};
}
inline bool latched(const Fault &f) { return f.mapping || f.perm; }
inline bool userVmid(uint32_t v) { return v >= kFirstUserVmid && v <= kLastUserVmid; }
// the kill for one user VMID; 0 for any other VMID (never a broadcast without CHECK_VMID, never VMID 0-7)
inline uint32_t killValue(uint32_t vmid) { return userVmid(vmid) ? (kKillBase | (vmid << 28)) : 0u; }

// The stall clock. A queue entry the global retired has not passed is pending; the clock starts at the first poll
// that sees the GPU pending at this retired value and restarts whenever retired moves or the GPU is seen idle. retired
// unchanged since that poll means the oldest unfinished submission (retired + 1) was unfinished then too, so the time
// since that poll is a lower bound of the stall. Returns the stalled time (0 while not stalled).
struct Stall {
    bool armed;
    uint64_t retired, sinceNs;
    bool acted;            // recovery already ran (or was refused for good) at this retired value
};
inline uint64_t track(Stall &s, uint64_t retired, uint64_t emitted, uint64_t nowNs) {
    if (retired >= emitted) {
        s.armed = false;
        s.acted = false;
        return 0;
    }
    if (!s.armed || s.retired != retired) {
        s.armed = true;
        s.retired = retired;
        s.sinceNs = nowNs;
        s.acted = false;
        return 0;
    }
    return nowNs - s.sinceNs;
}

// The queues as the kext keeps them: each one's outstanding submissions in order (fseq) with the VMID that ran them.
struct QView {
    bool ready, vcn;
    uint64_t done;                 // the queue's fence: the newest sequence it finished
    uint32_t head, tail;           // fseq[head & 63] .. fseq[(tail - 1) & 63]
    const uint64_t *fseq;
    const uint8_t *fvmid;
};
// Bit v set = VMID v ran the first unfinished entry of a GC queue (VCN faults are on MMHUB, not in this STATUS).
inline uint32_t stalledHeads(const QView *q, uint32_t n) {
    uint32_t m = 0;
    for (uint32_t qi = 0; qi < n; qi++) {
        if (!q[qi].ready || q[qi].vcn)
            continue;
        for (uint32_t i = q[qi].head; i != q[qi].tail; i++) {
            if (q[qi].fseq[i & 63] > q[qi].done) {
                if (q[qi].fvmid[i & 63] < 32)
                    m |= 1u << q[qi].fvmid[i & 63];
                break;
            }
        }
    }
    return m;
}

enum Verdict : int {
    kWait = 0,       // not stalled, or not for long enough
    kOff,            // debug.phxrecover 0
    kActed,          // this stall already had its recovery (or a refusal that cannot change)
    kFaultDefOff,    // debug.phxfaultdef 0: the kill alone does not unstick the TCP (TCP-KILL-1123)
    kNoFault,        // STATUS has neither MAPPING_ERROR nor PERMISSION_FAULTS
    kNotUser,        // STATUS names VMID 0-7
    kNotOwned,       // the VMID STATUS names is not at the head of a stalled queue with a live context
    kRecover,        // kill the VMID STATUS names
};

inline const char *verdictName(int v) {
    switch (v) {
    case kWait: return "wait";
    case kOff: return "off";
    case kActed: return "acted";
    case kFaultDefOff: return "phxfaultdef off";
    case kNoFault: return "no fault latched";
    case kNotUser: return "not a user VMID";
    case kNotOwned: return "VMID not at a stalled head / no live context";
    case kRecover: return "recover";
    default: return "?";
    }
}

struct Input {
    bool recoverOn, faultDefOn;
    uint64_t stalledNs, thresholdNs;
    bool acted;
    uint32_t status;
    uint32_t ownedMask;   // stalledHeads() & the VMIDs lent to a live context
};

// The only VMID this may ever return is the one STATUS names.
inline int decide(const Input &in, uint32_t *vmidOut) {
    *vmidOut = 0;
    if (!in.recoverOn)
        return kOff;
    if (!in.stalledNs || in.stalledNs < in.thresholdNs)
        return kWait;
    if (in.acted)
        return kActed;
    if (!in.faultDefOn)
        return kFaultDefOff;
    const Fault f = decode(in.status);
    if (!latched(f))
        return kNoFault;
    if (!userVmid(f.vmid))
        return kNotUser;
    if (!(in.ownedMask & (1u << f.vmid)))
        return kNotOwned;
    *vmidOut = f.vmid;
    return kRecover;
}

enum KillResult : int { kKillOk = 0, kKillBadVmid, kKillStuck };

// One kill per ms until the stalled sequence retires, kKillTries at most. *writes counts the SQ_CMD writes.
template <class E> int kill(E &e, uint32_t vmid, uint32_t *writes) {
    const uint32_t v = killValue(vmid);
    *writes = 0;
    if (!v)
        return kKillBadVmid;
    for (uint32_t i = 0; i < kKillTries; i++) {
        e.wr(kRegSqCmd, v);
        (*writes)++;
        e.delayUs(kKillStepUs);
        if (e.moved())
            return kKillOk;
    }
    return kKillStuck;
}

// The latched STATUS / ADDR cleared so the next fault latches fresh: a dummy STATUS read first (Linux does it for the
// fast GRBM interface), then CNTL with CLEAR_PROTECTION_FAULT_STATUS_ADDR set, then CNTL as it was without it. Every
// other CNTL bit (debug.phxfaultdef's ENABLE_DEFAULT bits) is written back unchanged. Returns STATUS read after.
template <class E> uint32_t clearStatus(E &e) {
    (void)e.rd(kRegStatus);
    const uint32_t c = e.rd(kRegCntl) & ~kCntlClear;
    e.wr(kRegCntl, c | kCntlClear);
    e.wr(kRegCntl, c);
    return e.rd(kRegStatus);
}

}  // namespace phxrec
