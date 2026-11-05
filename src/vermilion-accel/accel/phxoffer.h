// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxoffer.h: head 0's offer gate.
// Pure logic, no kernel headers: host-tested.
//
// The first WindowServer of a boot runs before the boot daemon and restores the stored one-display set ("AOC alone" at
// 144) once the arm publishes the display pipe. Head 0 cannot switch there (the pipe-0 kill switch), the refused switch
// rebuilds the pipe resource and that WindowServer dies. Its stored mode is matched against the
// mode list head 0 offers, comparing every key but the mode number, IODisplayModeID, IOFlags and bytes per row
// (CoreDisplay CGXCopyDisplayModeByMatchingDictionary, the refresh included), so a 144 mode that head 0 does not offer
// never matches: the set is "not valid", and CoreDisplay keeps the current mode.
//
// So while the gate is closed head 0 offers only the mode lit now. The gate closes when head 0 starts with a mode list (only
// a boot where the daemon armed phx-pipe0-modes has one), and opens for good on the daemon's debug.phxfb0_offer=1 just
// before its one WindowServer restart, or when a resync makes a hidden mode current. Opening also drops the IOFBConfig
// mode cache CoreDisplay stored on head 0, so the restarted WindowServer builds the list again with every mode.
#pragma once
#include <stdint.h>

namespace phxoffer {

// The modes head 0 offers: all of ids[0..n) when open; while closed only the one lit now (cur), or ids[0] when cur is not
// listed. n == 0 (the single-mode head) offers mode 1, as before. Returns the count written to out (out has room for n,
// at least 1).
inline unsigned offered(const uint32_t *ids, unsigned n, uint32_t cur, bool closed, uint32_t *out)
{
    if (!n) { out[0] = 1; return 1; }
    if (!closed) { for (unsigned i = 0; i < n; i++) out[i] = ids[i]; return n; }
    for (unsigned i = 0; i < n; i++) if (ids[i] == cur) { out[0] = cur; return 1; }
    out[0] = ids[0];
    return 1;
}

// Whether a listed mode id is offered now (connectFlags answers 0 for the others).
inline bool isOffered(const uint32_t *ids, unsigned n, uint32_t cur, bool closed, uint32_t id)
{
    uint32_t tmp[16];
    if (n > 16) return false;
    const unsigned k = offered(ids, n, cur, closed, tmp);
    for (unsigned i = 0; i < k; i++) if (tmp[i] == id) return true;
    return false;
}

// The gate at head 0's start: closed only with a mode list of two or more modes and no -phxfb0open boot-arg.
inline bool closedAtStart(unsigned nModes, bool bootArgOpen) { return nModes >= 2 && !bootArgOpen; }

// After a resync moved head 0 from `was` to `now`: the gate opens when the current mode changed while it was closed (an
// older daemon that never writes debug.phxfb0_offer re-times pipe 0 before its restart; the restarted WindowServer must find
// every mode listed, not only the new current one).
inline bool openAfterResync(uint32_t was, uint32_t now, bool closed) { return closed && now && now != was; }

}  // namespace phxoffer
