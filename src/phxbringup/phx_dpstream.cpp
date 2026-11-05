// SPDX-License-Identifier: MIT
// phx_dpstream.cpp: kext b117, multiport S2c: debug.phxdpstream +
// debug.phxdpstream_stat. b119: + 0x40 + n desktop (S2d, DIG n fed from the
// GOP's OTG0 = head 0's desktop on DP A / DP E), 0x50 + n its survey, and a survey while lit = the lit status (reads only).
//   debug.phxdpstream (int): root writes a selector: 0x10 + n = survey (reads + the DPCD caps read), 0x20 + n = stream (a
//     DPG magenta 1080p60 picture on OTG2 -> DIG n -> PHY n at RBR x4), 0x30 + n = undo; n = 0 (DP A) or 4 (DP E). The
//     stream needs the last debug.phxdplink run of this boot to be a PASS on the same connector. Reads return the last
//     selector (-1 = never); a read touches no register. Nothing runs at boot or until root writes it (inert).
//   debug.phxdpstream_stat (string): the last run (PhxDpStream.h stat).
// Engine: PhxDpStream.h (pipe-2 steps, gates, undo) over PhxDpRun.h (S2b gates, the per-run DIG/DP Io and guards) and
// PhxDpLink.h (enableStream / disableStream); host-tested. Under gP2Busy and the EDID/AUX
// lock (p2I2cLock) for the whole run, as debug.phxdplink; the AUX path is phx_dplink.cpp's (dplAuxRd / dplAuxWr: PhxDdc.h
// aux, AUX register writes limited to the engine block and its pad). Part of the PhxBringup.cpp translation unit, included
// after phx_dplink.cpp; not compiled on its own.

#include "PhxDpStream.h"

static int gDpsLast = -1;
static phxdps::Stream gDps;

static int phxDpStreamSysctl SYSCTL_HANDLER_ARGS {
    int v = gDpsLast;
    const int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;
    if (!p2Up()) return ENXIO;
    if (!OSCompareAndSwap(0, 1, &gP2Busy)) return EBUSY;
    if (!p2I2cLock(gPhx, 1)) { OSCompareAndSwap(1, 0, &gP2Busy); return EBUSY; }
    gDpsLast = v;
    const phxdpr::Aux aux{nullptr, dplAuxRd, dplAuxWr};
    const int r = phxdps::run(p2Env(), gP2, gDps, gDpl, v, gDmubRs.done, aux);
    p2I2cLock(gPhx, 0);
    OSCompareAndSwap(1, 0, &gP2Busy);
    IOLog("PhxBringup: debug.phxdpstream=0x%x: %s phase %d: %s (steps %u/%u, cr %u eq %u, vid 0x%x, otg2 %u Hz, aux_stray %u)\n", v,
          phxdps::kResName[r >= 0 && r <= phxdps::kUndoFail ? r : 0], gDps.phase, gDps.why ? gDps.why : "-", gDps.touched, gDps.n,
          gDps.rep.crLoops, gDps.rep.eqLoops, gDps.pVid, gDps.hz2, gDplAuxStray);
    return (r == phxdps::kPass || r == phxdps::kSurvey || r == phxdps::kUndone) ? 0 : r == phxdps::kRefused ? EINVAL
           : r == phxdps::kTimeout ? ETIMEDOUT : EIO;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdpstream, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, phxDpStreamSysctl, "I",
            "b117/b119 root: 0x10+n survey, 0x20+n stream (S2c: OTG2 DPG magenta 1080p60), 0x30+n undo, 0x40+n desktop (S2d: DIG n fed from OTG0), 0x50+n its survey; a survey while lit = lit status; n = 0 DP A, 4 DP E");

static int phxDpStreamStatSysctl SYSCTL_HANDLER_ARGS {
    (void)oidp; (void)arg1; (void)arg2;
    if (req->newptr) return EPERM;
    static constexpr int kN = 3072;
    char *b = static_cast<char *>(IOMalloc(kN));
    if (!b) return ENOMEM;
    int len = 0;
    if (!gDps.runs) len = snprintf(b, kN, "never run (root: sysctl -w debug.phxdpstream=0x14 survey DP E, 0x24 stream, 0x34 undo; after a debug.phxdplink=4 PASS)\n");
    else {
        len = phxdps::stat(gDps, b, kN);
        if (len >= 0 && len < kN - 1) len += snprintf(b + len, static_cast<size_t>(kN - len), "aux_gate_stray=%u\n", gDplAuxStray);
    }
    if (len < 0) len = 0;
    if (len > kN - 1) len = kN - 1;
    b[len] = 0;
    const int e = SYSCTL_OUT(req, b, static_cast<size_t>(len) + 1);
    IOFree(b, kN);
    return e;
}
SYSCTL_PROC(_debug, OID_AUTO, phxdpstream_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, phxDpStreamStatSysctl, "A",
            "b117 the last debug.phxdpstream: result, action, phase, why, pipe-2 survey, caps, steps, training, stream, rates, sink, writes, undo, pipe 0/1");
