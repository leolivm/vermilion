// SPDX-License-Identifier: MIT
// PhxC3Phy.h: kext b116 (multiport S2b): the C3 PHY SRAM
// load of PhxC3Load.h (the cold-boot fix, PHY D) for another PHY of the same family. Kernel-free; PhxC3Load.h is used as is (nothing
// in it changes, so debug.phxc3 = 9 on PHY D stays byte-identical; host-tested).
//
// Command i for PHY n = PHY D's command i (phxc3::loadCmd) with every PHY-indexed field moved to PHY n:
//   - the UNIPHY macro registers RESERVED1/13/15/16/18/20/28/29/34: UNIPHY3 address + 0xD8 x (n - 3)
//     (dcn_3_2_0_offset.h: DCIO_UNIPHY0..4 blocks 0xD8 apart; UNIPHY4 RESERVED13 0x6155, UNIPHY0 RESERVED13 0x5DF5);
//   - DCCG_GATE_DISABLE_CNTL2 PHYxSYMCLK_GATE_DISABLE: bit 27 (D) -> bit 24 + n (sh_mask: A 0x01000000 .. E 0x10000000);
//   - DCIO_SOFT_RESET UNIPHYx_SOFT_RESET: bit 3 (D) -> bit n (sh_mask: A 0x1 .. E 0x10).
// Values written to the PHY registers, the image and every timing are PHY D's (the firmware's own program uses one image
// for every PHY and mode). The prologue is the HDMI branch C3 proved on
// PHY D; the firmware's DP ENABLE re-runs its own (DP) prologue before using the SRAM [inferred].
// The check (cmdOkPhy): map the command back to PHY D (fromPhy), require phxc3::cmdOk on PHY D's table row, and require
// that mapping it forward again gives the same 16 dwords (so nothing outside the PHY-indexed fields can differ), and that
// it equals loadCmdPhy(i, n) dword for dword.
#pragma once
#include <stdint.h>
#include "PhxC3Load.h"

namespace phxc3p {

constexpr uint32_t kUniphyStride = 0xD8;
inline bool phyOk(unsigned n) { return n == 0 || n == 4; }   // PHY A, PHY E (D = debug.phxc3; B = the GOP's; C absent)
inline bool isRes(uint32_t a) {   // a UNIPHY3 (PHY D) macro register of the program
    using namespace phxc3;
    return a == kRes1 || a == kRes13 || a == kRes15 || a == kRes16 || a == kRes18 || a == kRes20 || a == kRes28 ||
           a == kRes29 || a == kRes34;
}
inline uint32_t gateBit(unsigned n) { return 1u << (24 + n); }
inline uint32_t resetBit(unsigned n) { return 1u << n; }
inline uint32_t res(uint32_t resD, unsigned n) { return resD + kUniphyStride * n - kUniphyStride * 3u; }
static_assert(phxc3::kRes13 + kUniphyStride == 0x06155u && phxc3::kRes13 - 3u * kUniphyStride == 0x05DF5u, "UNIPHY4 / UNIPHY0 RESERVED13");
static_assert(phxc3::kGateDis2PhyD == (1u << 27) && phxc3::kSoftResetPhyD == (1u << 3), "PHY D bits");

// Map one address/mask/value triple of a PHY D command to PHY n (dir = +1) or back to PHY D (dir = -1, src = n).
inline void mapAddr(uint32_t &a, unsigned n, bool toN) {
    if (toN) { if (isRes(a)) a = res(a, n); return; }
    static const uint32_t kResD[] = {phxc3::kRes1, phxc3::kRes13, phxc3::kRes15, phxc3::kRes16, phxc3::kRes18, phxc3::kRes20,
                                     phxc3::kRes28, phxc3::kRes29, phxc3::kRes34};
    for (uint32_t d : kResD)
        if (a == res(d, n)) { a = d; return; }
}
inline uint32_t mapBits(uint32_t a, uint32_t v, unsigned n, bool toN) {   // a = the (PHY D side) register
    const uint32_t from = toN ? (a == phxc3::kGateDis2 ? phxc3::kGateDis2PhyD : phxc3::kSoftResetPhyD)
                              : (a == phxc3::kGateDis2 ? gateBit(n) : resetBit(n));
    const uint32_t to = toN ? (a == phxc3::kGateDis2 ? gateBit(n) : resetBit(n))
                            : (a == phxc3::kGateDis2 ? phxc3::kGateDis2PhyD : phxc3::kSoftResetPhyD);
    return (v & from) ? ((v & ~from) | to) : v;
}
// in -> out, one 16-dword command, PHY D -> PHY n (toN) or PHY n -> PHY D. Unknown types are copied (cmdOk refuses them).
inline void mapCmd(const uint32_t *in, uint32_t *out, unsigned n, bool toN) {
    using namespace phxc3;
    for (unsigned k = 0; k < phxdmub::kCmdDw; k++) out[k] = in[k];
    const uint32_t type = in[0] & 0xff, pb = (in[0] >> 24) & 0x3f;
    if (type == kRmw) {
        for (unsigned k = 0; k < pb / 12 && k < kMaxRmw; k++) {
            uint32_t &a = out[1 + 3 * k];
            if (a == kGateDis2 || a == kDcioSoftReset) {
                out[2 + 3 * k] = mapBits(a, in[2 + 3 * k], n, toN);
                out[3 + 3 * k] = mapBits(a, in[3 + 3 * k], n, toN);
            } else mapAddr(a, n, toN);
        }
    } else if (type == kBurst || type == kWait) mapAddr(out[1], n, toN);
}
// Command i of PHY n's load (0..kLoadN-1); false past the end or for a PHY this file does not drive.
inline bool loadCmdPhy(unsigned i, unsigned n, uint32_t *c, const uint16_t *img = kPhxC3Image) {
    uint32_t d[phxdmub::kCmdDw];
    if (!phyOk(n) || !phxc3::loadCmd(i, d, img)) return false;
    mapCmd(d, c, n, true);
    return true;
}
// The exact-shape check of command i for PHY n: back to PHY D, PHY D's own cmdOk on its table row, forward again identical.
inline bool cmdOkPhy(const uint32_t *c, unsigned i, unsigned n, const char **why) {
    using namespace phxc3;
    if (!phyOk(n)) { if (why) *why = "PHY not A or E"; return false; }
    const Shape *s = shapeAt(kLoadShape, sizeof kLoadShape / sizeof kLoadShape[0], i);
    if (!s) { if (why) *why = "no table row"; return false; }
    uint32_t d[phxdmub::kCmdDw], f[phxdmub::kCmdDw];
    mapCmd(c, d, n, false);
    if (!cmdOk(d, *s, why)) return false;
    mapCmd(d, f, n, true);
    for (unsigned k = 0; k < phxdmub::kCmdDw; k++)
        if (f[k] != c[k]) { if (why) *why = "a field outside the PHY-indexed ones differs from PHY D's command"; return false; }
    // Nothing of PHY D (or another PHY) may remain: every UNIPHY address must be PHY n's
    const uint32_t type = c[0] & 0xff, pb = (c[0] >> 24) & 0x3f;
    const unsigned na = type == kRmw ? pb / 12 : 1;
    for (unsigned k = 0; k < na; k++) {
        const uint32_t a = type == kRmw ? c[1 + 3 * k] : c[1];
        if (a >= kPhyBlockLo && a <= kPhyBlockHi && (a < res(kRes1, n) || a > res(kRes34, n))) {
            if (why) *why = "a UNIPHY address outside PHY n's block";
            return false;
        }
        if ((a == kGateDis2 || a == kDcioSoftReset) && type == kRmw) {
            const uint32_t m = c[2 + 3 * k], want = a == kGateDis2 ? gateBit(n) : resetBit(n);
            if (m != want) { if (why) *why = "DCCG / DCIO bit not PHY n's"; return false; }
        }
    }
    // and it is exactly the program's command i for PHY n (values included; the kext never builds them from outside)
    uint32_t want[phxdmub::kCmdDw];
    if (!loadCmdPhy(i, n, want)) { if (why) *why = "no command i"; return false; }
    for (unsigned k = 0; k < phxdmub::kCmdDw; k++)
        if (want[k] != c[k]) { if (why) *why = "not the program's command for this step"; return false; }
    if (why) *why = nullptr;
    return true;
}

}  // namespace phxc3p
