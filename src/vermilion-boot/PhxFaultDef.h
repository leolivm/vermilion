// SPDX-License-Identifier: MIT
// PhxFaultDef.h: the GFX hub's protection-fault default page (debug.phxfaultdef, H-FAULTDEF).
//
// mesboot gart() leaves GCVM_L2_PROTECTION_FAULT_CNTL at 0x2: none of the *_PROTECTION_FAULT_ENABLE_DEFAULT fields is
// set, so an access that faults in a user VMID is redirected nowhere. On this card both GFX faults seen so far (a TCP
// read on 10-24, a TCP write on 11-12) stalled the whole GFX engine. Linux sets the eleven ENABLE_DEFAULT fields
// (gfx_v11_0_gfxhub_enable -> gfxhub_v3_0_set_fault_enable_default(true)) and points DEFAULT_ADDR at a dummy page in
// system memory (gfxhub_v3_0_init_system_aperture_regs), so the access completes against that page instead.
//
// Kernel-free on purpose: tests/mock/faultdef_test.sh runs this exact code against a register recorder. The kext
// supplies an environment E with:
//   uint32_t rd(uint32_t reg);            a GC register (absolute dword index)
//   void wr(uint32_t reg, uint32_t v);
//   void delayUs(uint32_t us);
// The caller holds gpuLock with every queue idle, owns the page and frees it only after disable() returned kOk.
// Registers written: CNTL, DEFAULT_ADDR LO/HI, the GC invalidation engine 17 request. Read: those, the engine 17 ack,
// and (stat only) STATUS, ADDR LO/HI, GRBM_STATUS. Nothing on MMHUB.
#pragma once
#include <stdint.h>

namespace phxfd {

// gc_11_0_0 offsets + the GC base 0x1260
constexpr uint32_t kRegCntl = 0x2824;                          // GCVM_L2_PROTECTION_FAULT_CNTL
constexpr uint32_t kRegStatus = 0x2828;                        // GCVM_L2_PROTECTION_FAULT_STATUS
constexpr uint32_t kRegAddrLo = 0x2829, kRegAddrHi = 0x282A;   // GCVM_L2_PROTECTION_FAULT_ADDR_LO32 / HI32 (page >> 12)
constexpr uint32_t kRegDefLo = 0x282B, kRegDefHi = 0x282C;     // GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32 / HI32
constexpr uint32_t kRegInvReq = 0x291C, kRegInvAck = 0x292E;   // GCVM_INVALIDATE_ENG17_REQ / ACK
constexpr uint32_t kRegGrbmStatus = 0x2004;

constexpr uint32_t kCntlBoot = 0x00000002u;         // mesboot gart(): ALLOW_SUBSEQUENT_PROTECTION_FAULT_STATUS_ADDR_UPDATES only
// bits 2-12: RANGE, PDE0, PDE1, PDE2, TRANSLATE_FURTHER, NACK, DUMMY_PAGE, VALID, READ, WRITE, EXECUTE _ENABLE_DEFAULT
constexpr uint32_t kEnableDefault = 0x00001FFCu;
// the no-retry fault interrupts (bits 13-29) and CRASH_ON_NO_RETRY / CRASH_ON_RETRY (30, 31) stay 0
constexpr uint32_t kCntlOn = kCntlBoot | kEnableDefault;   // 0x1FFE
constexpr uint32_t kDefLoBoot = 0xA3FFu, kDefHiBoot = 0;   // what mesboot gart() writes: its VRAM SAFE_PAGE 0xA3FF000 >> 12
constexpr uint64_t kPageMask = 0x0000FFFFFFFFF000ull;      // DEFAULT_ADDR_HI32 holds address bits 44-47 only
constexpr uint32_t kInvVmid0 = 0x00F80001u;                 // VMID 0, every flush type, as PhxBringup::vmFlushTlb
constexpr uint32_t kAckCapUs = 100000;                      // the bound vmFlushTlb uses while the ack is reliable

enum Result : int {
    kOk = 0,
    kNoPage,         // no page (the allocation failed): nothing written
    kUnaligned,      // the page address is not 4 KiB aligned: nothing written
    kTooHigh,        // the page lies above 48 bits: nothing written
    kNotBoot,        // CNTL is not mesboot's 0x2: an unknown state, nothing written
    kAddrMismatch,   // DEFAULT_ADDR did not read back: restored, CNTL untouched
    kCntlMismatch,   // CNTL did not read back: CNTL and DEFAULT_ADDR restored
    kBusy,           // the GPU did not go idle (the kext's own check): nothing written
};

inline const char *resultName(int r) {
    switch (r) {
    case kOk: return "ok";
    case kNoPage: return "no page";
    case kUnaligned: return "page not 4 KiB aligned";
    case kTooHigh: return "page above 48 bits";
    case kNotBoot: return "CNTL not 0x2 before enable";
    case kAddrMismatch: return "DEFAULT_ADDR read-back mismatch";
    case kCntlMismatch: return "CNTL read-back mismatch";
    case kBusy: return "GPU not idle";
    default: return "?";
    }
}

// GC engine 17, VMID 0, as Linux flushes the GFXHUB after set_fault_enable_default. False when the ack did not come.
template <class E> bool flushVmid0(E &e, uint32_t *usOut) {
    e.wr(kRegInvReq, kInvVmid0);
    uint32_t us = 0;
    bool acked = true;
    while (!(e.rd(kRegInvAck) & 1u)) {
        if (us >= kAckCapUs) { acked = false; break; }
        e.delayUs(us < 1000 ? 1 : 10);
        us += us < 1000 ? 1 : 10;
    }
    if (usOut) *usOut = us;
    return acked;
}

// The address first, read back; only then the ENABLE_DEFAULT bits, read back; then the flush. Every refusal leaves the
// hub as it was (mesboot's CNTL 0x2 and DEFAULT_ADDR 0xA3FF).
template <class E> int enable(E &e, uint64_t pa, uint32_t *ackUs, bool *acked) {
    if (!pa) return kNoPage;
    if (pa & 0xFFFull) return kUnaligned;
    if (pa & ~kPageMask) return kTooHigh;
    const uint32_t cntl = e.rd(kRegCntl);
    if (cntl != kCntlBoot) return kNotBoot;
    const uint32_t lo = static_cast<uint32_t>(pa >> 12), hi = static_cast<uint32_t>(pa >> 44);
    e.wr(kRegDefLo, lo);
    e.wr(kRegDefHi, hi);
    if (e.rd(kRegDefLo) != lo || (e.rd(kRegDefHi) & 0xFu) != hi) {
        e.wr(kRegDefLo, kDefLoBoot);
        e.wr(kRegDefHi, kDefHiBoot);
        return kAddrMismatch;
    }
    e.wr(kRegCntl, cntl | kEnableDefault);
    if (e.rd(kRegCntl) != kCntlOn) {
        e.wr(kRegCntl, kCntlBoot);
        e.wr(kRegDefLo, kDefLoBoot);
        e.wr(kRegDefHi, kDefHiBoot);
        flushVmid0(e, nullptr);
        return kCntlMismatch;
    }
    const bool a = flushVmid0(e, ackUs);
    if (acked) *acked = a;
    return kOk;
}

// The reverse order: the bits off first, then mesboot's address, then the flush. kOk only when CNTL reads back 0x2:
// the caller must not free the page otherwise.
template <class E> int disable(E &e, uint32_t *ackUs, bool *acked) {
    e.wr(kRegCntl, kCntlBoot);
    e.wr(kRegDefLo, kDefLoBoot);
    e.wr(kRegDefHi, kDefHiBoot);
    const bool a = flushVmid0(e, ackUs);
    if (acked) *acked = a;
    return (e.rd(kRegCntl) & kEnableDefault) ? kCntlMismatch : kOk;
}

// Dwords of the page that differ from the fill pattern; the first one's byte offset and value (0xFFFFFFFF / 0: none).
inline uint32_t scanPage(const volatile uint32_t *p, uint32_t fill, uint32_t *firstOff, uint32_t *firstVal) {
    uint32_t n = 0;
    *firstOff = 0xFFFFFFFFu;
    *firstVal = 0;
    for (uint32_t i = 0; i < 1024; i++) {
        const uint32_t v = p[i];
        if (v == fill) continue;
        if (!n) { *firstOff = i * 4; *firstVal = v; }
        n++;
    }
    return n;
}

}  // namespace phxfd
