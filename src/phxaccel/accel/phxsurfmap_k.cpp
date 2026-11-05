// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxsurfmap_k.cpp: M7 S4b T1, the kernel side of the IOSurface ID -> head table (phxsurfmap.h, host-tested).
// Learn only: nothing
// reads the table in the kernel and no present path changes. NVDisplayPipe::performTransaction (nvrm-accel.cpp) calls
// phxSurfMapLearn for every transaction's plane-0 surface with the head its pipe drives; the table is published as
//   debug.phxsurfmap          opaque, read-only, phxsurfmap::exportBlob's bytes (magic 'PSM1')
//   debug.phxsurfmap_learns   transactions learned (a live source: it grows while either monitor updates)
//   debug.phxsurfmap_moved    learns where the ID was last named by the other head
//   debug.phxsurfmap_evicted  learns that pushed out the entry named longest ago
//   debug.phxsurfmap_reset    write 1 (root): forget everything (a WindowServer restart reuses IDs)
// Installed with head 0's framebuffer (phxfb.cpp start / stop), like phxagdc.cpp.
#include <IOKit/IOLib.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include "phxsurfmap.h"

static phxsurfmap::Table gSm;
static IOSimpleLock *gSmLock;
static SInt64 gSmLearns, gSmMoved, gSmEvicted;
static bool gSmInstalled;

static uint64_t smNowNs() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns; }

void phxSurfMapLearn(uint32_t id, unsigned head)
{
    IOSimpleLock *l = __atomic_load_n(&gSmLock, __ATOMIC_ACQUIRE);
    if (!l) return;
    const uint64_t now = smNowNs();
    IOSimpleLockLock(l);
    const int r = phxsurfmap::learn(gSm, id, head, now);
    IOSimpleLockUnlock(l);
    if (r >= 0) OSAddAtomic64(1, &gSmLearns);
    if (r == phxsurfmap::kMoved) OSAddAtomic64(1, &gSmMoved);
    if (r == phxsurfmap::kEvicted) OSAddAtomic64(1, &gSmEvicted);
}

static int phxSurfMapSysctl SYSCTL_HANDLER_ARGS
{
    uint8_t buf[phxsurfmap::kBlobMax];
    size_t n = 0;
    if (IOSimpleLock *l = __atomic_load_n(&gSmLock, __ATOMIC_ACQUIRE)) {
        const uint64_t now = smNowNs();
        IOSimpleLockLock(l);
        n = phxsurfmap::exportBlob(gSm, now, buf, sizeof buf);
        IOSimpleLockUnlock(l);
    }
    return SYSCTL_OUT(req, buf, n);
}
static int phxSurfMapResetSysctl SYSCTL_HANDLER_ARGS
{
    int v = 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr || v != 1) return err;
    if (IOSimpleLock *l = __atomic_load_n(&gSmLock, __ATOMIC_ACQUIRE)) {
        IOSimpleLockLock(l); phxsurfmap::reset(gSm); IOSimpleLockUnlock(l);
    }
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxsurfmap, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, phxSurfMapSysctl, "S",
            "M7 S4b: IOSurface ID -> head table ('PSM1', generation, n, nowMs, then n x {id, head, flags, ageMs})");
SYSCTL_PROC(_debug, OID_AUTO, phxsurfmap_reset, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxSurfMapResetSysctl, "I",
            "write 1: forget the surface -> head table");
SYSCTL_QUAD(_debug, OID_AUTO, phxsurfmap_learns, CTLFLAG_RD | CTLFLAG_LOCKED, &gSmLearns, "transactions learned into the surface -> head table");
SYSCTL_QUAD(_debug, OID_AUTO, phxsurfmap_moved, CTLFLAG_RD | CTLFLAG_LOCKED, &gSmMoved, "learns whose ID was last named by the other head");
SYSCTL_QUAD(_debug, OID_AUTO, phxsurfmap_evicted, CTLFLAG_RD | CTLFLAG_LOCKED, &gSmEvicted, "learns that evicted the entry named longest ago");

void phxSurfMapInstall()
{
    if (gSmInstalled) return;
    if (!gSmLock) gSmLock = IOSimpleLockAlloc();
    if (!gSmLock) return;
    sysctl_register_oid(&sysctl__debug_phxsurfmap); sysctl_register_oid(&sysctl__debug_phxsurfmap_reset);
    sysctl_register_oid(&sysctl__debug_phxsurfmap_learns); sysctl_register_oid(&sysctl__debug_phxsurfmap_moved);
    sysctl_register_oid(&sysctl__debug_phxsurfmap_evicted);
    gSmInstalled = true;
}

void phxSurfMapRemove()
{
    if (!gSmInstalled) return;
    sysctl_unregister_oid(&sysctl__debug_phxsurfmap); sysctl_unregister_oid(&sysctl__debug_phxsurfmap_reset);
    sysctl_unregister_oid(&sysctl__debug_phxsurfmap_learns); sysctl_unregister_oid(&sysctl__debug_phxsurfmap_moved);
    sysctl_unregister_oid(&sysctl__debug_phxsurfmap_evicted);
    gSmInstalled = false;
    // the lock stays: a transaction may still be learning on another thread; learn() into a table nobody reads is harmless
}
