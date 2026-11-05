// SPDX-License-Identifier: MIT
// phx_dpcd.cpp: kext b112, multiport S1a: debug.phxdpcd + debug.phxdpcd_stat.
//   debug.phxdpcd (int): root writes a selector = one read-only DPCD snapshot of the DP sink on AUX engine n
//     (n = 0-4; 0x10 + n = also when that connector is lit). Reads return the last selector written (-1 = never); a read
//     touches no register. Nothing runs at boot or until root writes it.
//   debug.phxdpcd_stat (string): the text of the last run (PhxDpcd.h format: rules, refusals, ranges, decode).
// b115: each AUX transaction first puts the engine's DDC pad in AUX mode when it is not (PhxDdc.h auxPadSetup, as Linux
// dce_aux acquire); the stat's "pad:" line shows it. The engine code is PhxDpcd.h (kernel-free,
// host-tested). The AUX engines share the EDID
// lock (edidBusy, through p2I2cLock of phx_pipe2.cpp: waits <= ~100 ms, like edidFetch and the SCDC sysctls).
// Part of the PhxBringup.cpp translation unit, included after phx_hpd.cpp; not compiled on its own.

#include "PhxDpcd.h"

static int gDpcdLast = -1;
static char gDpcdStat[1024] = "never run (root: sysctl -w debug.phxdpcd=n, n = AUX engine 0-4, 0x10 + n = also when lit)\n";
static constexpr size_t kDpcdStatN = sizeof gDpcdStat;
static IOLock *gDpcdLock;   // guards gDpcdStat (writer: one run; readers: the stat sysctl)

static IOLock *dpcdLock() {
    if (!gDpcdLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, &gDpcdLock)) IOLockFree(l);
    }
    return gDpcdLock;
}
static void dpcdStore(const char *s) {
    IOLock *l = dpcdLock();
    if (!l) return;
    IOLockLock(l);
    strlcpy(gDpcdStat, s, sizeof gDpcdStat);
    IOLockUnlock(l);
}

static int phxDpcdSysctl SYSCTL_HANDLER_ARGS {
    int v = gDpcdLast;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;   // it drives an AUX engine
    if (!gPhx || !gPhx->p2Ready()) return ENXIO;
    gDpcdLast = v;
    if (!p2I2cLock(gPhx, 1)) { dpcdStore("busy: the EDID/AUX lock is held (an EDID read or an I2C plan step)\n"); return EBUSY; }
    const phxddc::Io io{gPhx, p2IoRd, p2IoWr, p2IoDelay};
    phxdpcd::Result r{};
    const int rc = phxdpcd::run(io, static_cast<uint32_t>(v), &r);
    p2I2cLock(gPhx, 0);
    char *b = static_cast<char *>(IOMalloc(sizeof gDpcdStat));
    if (b) {
        phxdpcd::format(r, b, sizeof gDpcdStat);
        dpcdStore(b);
        IOFree(b, sizeof gDpcdStat);
    }
    return rc == phxdpcd::kRan ? 0 : rc == phxdpcd::kRefArg ? EINVAL : rc == phxdpcd::kRefLit ? EACCES : ENODEV;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdpcd, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxDpcdSysctl, "I",
            "b112 root, n = read DPCD 0x0-0xF, 0x2200-0x220F, 0x100-0x108, 0x200-0x207 over AUX n (reads only; 0x10+n = also when lit); reads the last n");

static int phxDpcdStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    char *b = static_cast<char *>(IOMalloc(sizeof gDpcdStat));
    if (!b) return ENOMEM;
    IOLock *l = dpcdLock();
    if (l) IOLockLock(l);
    strlcpy(b, gDpcdStat, kDpcdStatN);
    if (l) IOLockUnlock(l);
    const int e = SYSCTL_OUT(req, b, strlen(b) + 1);
    IOFree(b, sizeof gDpcdStat);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdpcd_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDpcdStatSysctl, "A",
            "b112 the last debug.phxdpcd: selector, connector, refusal, AUX control/ARB, DDC pad (b115), the four DPCD ranges, decode, fingerprint");
