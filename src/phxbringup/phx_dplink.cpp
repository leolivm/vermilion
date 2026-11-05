// SPDX-License-Identifier: MIT
// phx_dplink.cpp: kext b116, multiport S2b: debug.phxdplink +
// debug.phxdplink_stat.
//   debug.phxdplink (int): root writes a selector = one train-only DP link check (Linux dp_verify_link_cap) on DP A (0) or
//     DP E (4) at RBR x4 (+ 0x10 = HBR x4): the PHY's C3 SRAM load when not loaded by this sysctl in this boot, then
//     TRANSMITTER_CONTROL ENABLE, link training over AUX, TRANSMITTER_CONTROL DISABLE. Reads return the last selector
//     (-1 = never); a read touches no register. Nothing runs at boot or until root writes it (inert).
//   debug.phxdplink_stat (string): the last run (PhxDpRun.h stat).
// Engine: PhxDpRun.h (gates, C3 for PHY A/E = PhxC3Phy.h, the per-run Io and guards) over PhxDpLink.h (training) and
// PhxDmub.h submitWith; host-tested. Under gP2Busy (the pipe-1 ladder / debug.phxc3 lock) and
// the EDID/AUX lock (p2I2cLock) for the whole run. The AUX path is PhxDdc.h aux (b115 pad setup unchanged) through an Io
// whose register writes are limited to the AUX engine's block and its DDC pad (phxdpr::auxWriteAllowed; others dropped).
// Part of the PhxBringup.cpp translation unit, included after phx_dpcd.cpp; not compiled on its own.

#include "PhxDpRun.h"

static int gDplLast = -1;
static phxdpr::Run gDpl;
static uint32_t gDplAuxStray;   // AUX-side register writes dropped by the AUX gate (0 unless PhxDdc.h changes)
static unsigned gDplAuxEng;

static void dplAuxIoWr(void *c, uint32_t r, uint32_t v) {
    if (!phxdpr::auxWriteAllowed(gDplAuxEng, r)) { gDplAuxStray++; return; }
    p2IoWr(c, r, v);
}
static int dplAuxRd(void *, unsigned eng, uint32_t a, uint8_t *b, unsigned n) {
    if (n > 16) return -1;
    gDplAuxEng = eng;
    const phxddc::Io io{gPhx, p2IoRd, dplAuxIoWr, p2IoDelay};
    phxddc::Diag d{};
    return phxddc::dpcdRead(io, eng, a, b, static_cast<uint8_t>(n), &d) == phxddc::kOk ? 0 : -1;
}
static int dplAuxWr(void *, unsigned eng, uint32_t a, const uint8_t *b, unsigned n) {
    if (n > 16) return -1;
    gDplAuxEng = eng;
    const phxddc::Io io{gPhx, p2IoRd, dplAuxIoWr, p2IoDelay};
    phxddc::Diag d{};
    uint8_t got = 0;
    return phxddc::aux(io, eng, 0x80, a, b, static_cast<uint8_t>(n), nullptr, 0, &got, &d) == phxddc::kOk ? 0 : -1;
}

static int phxDpLinkSysctl SYSCTL_HANDLER_ARGS {
    int v = gDplLast;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    if (!p2I2cLock(gPhx, 1)) { OSCompareAndSwap(1, 0, &gP2Busy); return EBUSY; }
    gDplLast = v;
    const phxdpr::Aux aux{nullptr, dplAuxRd, dplAuxWr};
    const int r = phxdpr::run(p2Env(), gP2, gDpl, v, gDmubRs.done, aux);
    p2I2cLock(gPhx, 0);
    OSCompareAndSwap(1, 0, &gP2Busy);
    IOLog("PhxBringup: debug.phxdplink=0x%x: %s phase %d: %s (c3 %u/%u, cr %u eq %u, after %02x %02x %02x, aux_stray %u)\n", v,
          phxdpr::kResName[r >= 0 && r <= phxdpr::kPostFail ? r : 0], gDpl.phase, gDpl.why ? gDpl.why : "-", gDpl.c3Ok,
          gDpl.c3Ran ? phxc3::kLoadN : 0, gDpl.rep.crLoops, gDpl.rep.eqLoops, gDpl.rep.after[2], gDpl.rep.after[3], gDpl.rep.after[4], gDplAuxStray);
    return r == phxdpr::kPass ? 0 : r == phxdpr::kRefused ? EINVAL : r == phxdpr::kTimeout ? ETIMEDOUT : EIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdplink, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxDpLinkSysctl, "I",
            "b116 root: train-only DP link check, 0 = DP A, 4 = DP E at RBR x4 (+0x10 = HBR x4): C3 for the PHY, ENABLE, training, DISABLE; reads the last selector");

static int phxDpLinkStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 2048;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len = 0;
    if (!gDpl.attempts) len = snprintf(b, kN, "never run (root: sysctl -w debug.phxdplink=4 = DP E, 0 = DP A; +0x10 = HBR)\n");
    else {
        len = phxdpr::stat(gDpl, b, kN);
        if (len >= 0 && len < kN - 1) len += snprintf(b + len, static_cast<size_t>(kN - len), "aux_gate_stray=%u\n", gDplAuxStray);
    }
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdplink_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDpLinkStatSysctl, "A",
            "b116 the last debug.phxdplink: result, phase, why, gates, DPCD caps, C3 load, training loops and lane status, writes, post checks, pipe 0/1");
