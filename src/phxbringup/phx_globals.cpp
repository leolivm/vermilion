// SPDX-License-Identifier: MIT
// phx_globals.cpp: debug tunables read by the hot paths (phxbos_flush, phxmtype, phxscan, phxpt_reclaim/keep), VM and BO
// counters, the PSP ring frame, GC identity and memory-pool budgets.
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

// debug.phxbos_flush: how a client's BOs are made coherent around each submission and each completed wait.
// 2 = clflush every line (the bring-up way), 1 = clflushopt (the same lines, pipelined), 0 = none (the GPU maps system
// memory snooped, kPteSysFlags). With clflush, WindowServer spent ~73% of its CPU here and composited ~13 fps; with
// none, 41 fps and no artifacts (2026-10-20), hence the default.
static int gBosFlush = 0;
static volatile SInt64 gBosFlushBytes, gBosFlushHost;
SYSCTL_INT(_debug, OID_AUTO, phxbos_flush, CTLFLAG_RW | CTLFLAG_LOCKED, &gBosFlush, 0,
           "BO cache maintenance per submit/wait: 0 none, 1 clflushopt, 2 clflush, 3 clflushopt incl. host imports");
SYSCTL_QUAD(_debug, OID_AUTO, phxbos_flush_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gBosFlushBytes,
            "bytes of BOs flushed from the CPU caches");
SYSCTL_QUAD(_debug, OID_AUTO, phxbos_flush_host, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gBosFlushHost,
            "host-imported BOs flushed (debug.phxbos_flush=3)");
// debug.phxmtype: the MTYPE of system-page PTEs (bits 48-50). 4 (C_RW_S) came from the 780M bring-up; Linux's
// gmc_v11_0 and Cobalt64 map snooped system memory NC (0), and on the RX 7900 XT window motion showed 64-byte runs of
// old pixels (2026-10-24). Only mappings made after a change take it.
static int gPteMtype = 0;
SYSCTL_INT(_debug, OID_AUTO, phxmtype, CTLFLAG_RW | CTLFLAG_LOCKED, &gPteMtype, 0,
           "MTYPE of system-page PTEs: 0 NC (Linux gfx11 default), 3 UC, 4 C_RW_S (780M bring-up); new mappings only");
// debug.phxscan: the scanout selectors (9..14) answer only while 1. Writing 0 while a client holds the plane hands
// the panel back to the console within 50 ms (the watchdog).
static int gScanOn = 0;
SYSCTL_INT(_debug, OID_AUTO, phxscan, CTLFLAG_RW | CTLFLAG_LOCKED, &gScanOn, 0,
           "1 = scanout selectors 9..14 (GPU-written VRAM slots flipped on HUBP0) allowed, 0 = refused and any held plane restored");
static volatile UInt32 gSysctlUp;
// b82: the BIF indirect window (BAR5 dwords 0 MM_INDEX, 1 MM_DATA, 6 MM_INDEX_HI) is one shared index pair: every
// access to those dwords (phx_pipe2.cpp mmIdxRead/Write, ReadRegs, WriteReg) holds this lock with interrupts off.
// Allocated with the sysctls (newUserClient); until then the indirect path refuses.
static IOSimpleLock *gMmIdxLock;
static inline bool phxMmIdxDword(uint64_t first, uint64_t count) { return first <= 6 && count; }
// GPU VM telemetry, read with `sysctl debug.phxvm` (this kext's IOLog output never shows in the unified log)
static volatile SInt32 gVmMapFails, gVmUnmapTimeouts, gVmCtxCloses, gVmCtxRetired, gVmPoolFull, gVmCtxCap, gVmInvTimeouts;
static volatile UInt32 gVmInvMaxUs;
// bytes of BOs alive in each pool, all clients: 0 visible VRAM, 1 hi VRAM, 2 GTT, 3 host imports (debug.phxvm)
static volatile SInt64 gBoBytes[4];
static char gVmLastFail[192];
// BO creations refused, by cause: 0 visible VRAM budget, 1 hi VRAM budget, 2 GTT, 3 handle table, 4 page allocation
// (debug.phxvm), and the last refusal with that client's pools as they were
static volatile SInt32 gBoFails[5];
static volatile SInt32 gBoVisToGtt;   // b70: VRAM BOs placed as GTT because the visible budget was full
static char gBoLastFail[224];
// b77: debug.phxpt_reclaim: 0 (default) = page tables behave as b76 (a leaf table, once attached, stays until
// its context is freed; only the CPU-side accounting runs); 1 = a context keeps at most debug.phxpt_keep leaf tables
// with no valid PTE attached (an emptied 2 MiB is often mapped again: the VA allocator is first-fit); after an unmap the
// oldest emptied beyond that (up to 64 per unmap) are detached and handed out again to a later map of the same context
// once the GPU retired every submission emitted before the detach and the TLB invalidation after it was acknowledged;
// 2 = 1 + each reused table is checked to be all zero over BAR0. Live-switchable; back to 0 stops new detaches and
// reuses (parked tables then wait for the context's end). Host-tested.
static int gPtReclaim = 0;
SYSCTL_INT(_debug, OID_AUTO, phxpt_reclaim, CTLFLAG_RW | CTLFLAG_LOCKED, &gPtReclaim, 0,
           "B-14 empty leaf page-table reclaim: 0 off (b76), 1 detach + reuse after retire and TLB ack, 2 = 1 + zero check on reuse");
static int gPtKeep = 64;
SYSCTL_INT(_debug, OID_AUTO, phxpt_keep, CTLFLAG_RW | CTLFLAG_LOCKED, &gPtKeep, 0,
           "B-14: empty leaf page tables a context keeps attached before debug.phxpt_reclaim detaches the oldest");
// phxvm::Ev counters: cap, pool full, fresh leaves, reclaimed, reused, cancelled (no TLB ack), dirty, drift, no PDE
static volatile SInt32 gPtEv[phxvm::EvCount];
class PhxBringup;
struct PhxVmEnv;
static PhxBringup *gPhx;

// psp_gfx_rb_frame (64 B) — the ring entry the PSP reads. Only 5 fields are set; the rest stay zero
// (Linux amdgpu_psp.c:3485: a non-zero cmd_buf_size makes the PSP silently drop the frame).
struct PhxGfxRbFrame {
    uint32_t cmd_buf_addr_lo, cmd_buf_addr_hi, cmd_buf_size;
    uint32_t fence_addr_lo, fence_addr_hi, fence_value;
    uint32_t sid_lo, sid_hi;
    uint8_t vmid, frame_type, reserved1[2];
    uint32_t reserved2[7];
} __attribute__((packed));
static_assert(sizeof(PhxGfxRbFrame) == 64, "PhxGfxRbFrame must be 64 B");

// navi31: GB_ADDR_CONFIG as the Linux driver reads it on the RX 7900 XT (0x545), GC 11.0.0 (IP discovery).
static constexpr uint32_t kGbAddrConfig = 0x00000545;
static constexpr uint32_t kGcVersion = (11u << 16) | (0u << 8) | 0u;

// Pools reported to RADV. E1 backs every BO with system memory, so these are budgets, not hardware:
// BAR0 is 256 MiB on this machine, the UMA carve-out 6 GiB.
static constexpr uint64_t kVisTotal = 256ull << 20;
// b47: 6 GiB / 2 GiB (were 1536 / 512 MiB): WindowServer's images reached 1690 MB after ~50 min and every later colour
// image failed (cursor trails on an empty desktop, 2026-10-25 22:47); the machine has 32 GB
static constexpr uint64_t kHiTotal = 6144ull << 20;
static constexpr uint64_t kGttCap = 2048ull << 20;
static constexpr uint64_t kGttMaxBo = 256ull << 20;
static constexpr uint64_t kHostCap = 4096ull << 20, kHostMaxBo = 1024ull << 20;   // host imports, per client
static constexpr uint64_t kVaMask = (1ull << 48) - 1;
static constexpr IOByteCount kFbAperMapMax = 512ull << 20;   // navi31: BAR0 window mapped into the kernel
static constexpr int kMaxMaps = 4096;

static bool vaCanonical(uint64_t va) {
    uint64_t top = va >> 47;
    return top == 0 || top == 0x1FFFF;
}

