// SPDX-License-Identifier: MIT
// PhxC3Load.h: kext b113, the cold-boot dark Acer (C3): the fixed DMUB REG_SEQ programs of
// debug.phxc3. Kernel-free: stdint only; the engine and its gates are PhxPipe2.h dmubC3, the kext glue phx_pipe2.cpp;
// host-tested.
//
// Mode 9 = the firmware's own PHY D (UNIPHY3) PLL-program prologue, HDMI branch (Linux DMCUB build 0x07002F00, function
// 0x60021BB0), then its 0xC3 SRAM load (loader 0x6001FBC0): RESERVED18 = 0x8000, the 20,480 image halfwords to RESERVED20,
// RESERVED13 bit 29. Replayed as 1475 DMUB commands through the GOP firmware's inbox:
// 9 type 1 (DMUB_CMD__REG_SEQ_READ_MODIFY_WRITE), 2 type 4 (DMUB_CMD__REG_REG_WAIT), 1464 type 3
// (DMUB_CMD__REG_SEQ_BURST_WRITE). Modes 1-5 = the 1a1-1a5 warm sub-tests of types 3, 4 and partial-mask 1 on the dark
// OTG1_OTG_H_TOTAL, 5 = two firmware-side reads (type 4) of PHY D's RESERVED13.
// Layouts (Linux v6.8 dmub_cmd.h): header {type:8, sub_type:8, ret_status:1, multi_cmd_pending:1, reserved0:6,
// payload_bytes:6, reserved1:2}; type 1 = {addr, modify_mask, modify_value} x n (payload 12n, n <= 5); type 3 = addr +
// write_values[n] (payload 4n, n <= 14, every value to the same address); type 4 = {addr, mask, condition_field_value,
// time_out_us} (payload 16). Firmware behaviour of the Linux build that shapes the rules below:
//   - type 1 skips the read only when the mask is 0x0000FFFF (then a plain store); with any other mask it reads and stores
//     (reg & ~mask) | value, value NOT masked: so every value must lie inside its mask, and 0x0000FFFF is never used;
//   - type 3 never reads: the only type allowed on RESERVED20 (a CR data port; a read may move its address [inferred]);
//   - type 2 is not dispatched (trace 0x16): never used;
//   - the handlers add addr * 4 to 0x5FFC0000 with no range check: every (type, address, mask) is pinned below.
// Nothing here takes an address or a value from outside: the modes select one of the fixed programs.
#pragma once
#include <stdint.h>
#include "PhxDmub.h"
#include "phx_c3_image.inc"   // kPhxC3Image[20480], kPhxC3ImageCrc32 (tools/firmware/c3_image.py)

namespace phxc3 {

enum : uint32_t { kRmw = 1, kBurst = 3, kWait = 4 };
static constexpr unsigned kMaxRmw = 5, kMaxBurst = 14;
static constexpr uint32_t kPlainStoreMask = 0x0000FFFFu;   // the Linux build's no-read mask (literal 0x6001B228): refused
static constexpr uint32_t kWaitMaxUs = 20000;
// PHY D = UNIPHY3 (dcn_3_2_0_offset.h DCIO_UNIPHY3_UNIPHY_MACRO_CNTL_RESERVEDn, BASE_IDX 2 + 0x34C0) and the shared bits
static constexpr uint32_t kRes1 = 0x06071, kRes13 = 0x0607D, kRes15 = 0x0607F, kRes16 = 0x06080, kRes18 = 0x06082, kRes20 = 0x06084,
                          kRes28 = 0x0608C, kRes29 = 0x0608D, kRes34 = 0x06092;
static constexpr uint32_t kGateDis2 = 0x0013C /*DCCG_GATE_DISABLE_CNTL2, BASE_IDX 1*/, kDcioSoftReset = 0x05D5E, kOtg1HTotal = 0x0506A;
static constexpr uint32_t kGateDis2PhyD = 0x08000000u, kSoftResetPhyD = 0x8u, kSramAddr = 0x8000u;
static constexpr uint32_t kRes13InitDone = 0x10000000u, kRes13ExtLdDone = 0x20000000u;
// The host never reads or writes the UNIPHY macro / RDPCS blocks (0xFFFFFFFF to the host, read on the box):
// DCIO_UNIPHY0..4 RESERVED0..57 and RDPCSTX/RDPCSPIPE0..4 span these absolute dwords (dcn_3_2_0_offset.h).
static constexpr uint32_t kPhyBlockLo = 0x05DE8, kPhyBlockHi = 0x06181;

// The per-address allow-list. Type 1 and 4: the exact masks; type 3: mask 0 (values checked in cmdOk).
struct Allow { uint32_t addr; uint32_t type, mask; };
static const Allow kAllow[] = {
    {kRes13, kRmw, 0x00000008u}, {kRes13, kRmw, 0x00000001u}, {kRes13, kRmw, 0x00000300u}, {kRes13, kRmw, 0x20000000u},
    {kRes13, kRmw, 0xC0000000u}, {kRes13, kRmw, 0x0C000000u}, {kRes13, kWait, kRes13InitDone}, {kRes13, kWait, kRes13ExtLdDone},
    {kRes1, kRmw, 0x00000030u}, {kRes1, kRmw, 0x00000200u}, {kRes1, kRmw, 0x00003000u}, {kRes1, kRmw, 0x00000002u},
    {kRes28, kRmw, 0x0000000Fu}, {kRes28, kRmw, 0x00003CF0u}, {kRes29, kRmw, 0x0000000Fu}, {kRes29, kRmw, 0x00003CF0u},
    {kRes34, kRmw, 0x00000FFFu}, {kRes34, kRmw, 0x0000F000u}, {kRes34, kWait, 0x000F0000u},
    {kRes15, kRmw, 0x00000008u}, {kRes16, kRmw, 0x00000808u},
    {kRes18, kBurst, 0}, {kRes20, kBurst, 0},
    {kGateDis2, kRmw, kGateDis2PhyD}, {kDcioSoftReset, kRmw, kSoftResetPhyD},
    {kOtg1HTotal, kBurst, 0}, {kOtg1HTotal, kWait, 0x1u}, {kOtg1HTotal, kRmw, 0x2u}, {kOtg1HTotal, kRmw, 0x4u},   // 1a1-1a4
};
inline bool allowed(uint32_t type, uint32_t addr, uint32_t mask) {
    for (const Allow &a : kAllow) if (a.addr == addr && a.type == type && a.mask == mask) return true;
    return false;
}

// The static (type, address, mask) table of a program: `count` consecutive commands of one shape. n = sequences (type 1),
// values (type 3) or 1 (type 4); gapUs = the host wait after the command was consumed (the firmware's own gap).
struct Shape { uint16_t count, gapUs; uint32_t type, n; uint32_t addr[kMaxRmw], mask[kMaxRmw]; };
static const Shape kLoadShape[] = {
    {1, 25, kRmw, 1, {kRes13}, {0x8}},                                                                                   // L1
    {1, 100, kRmw, 2, {kGateDis2, kDcioSoftReset}, {kGateDis2PhyD, kSoftResetPhyD}},                                    // L2
    {1, 100, kRmw, 2, {kDcioSoftReset, kGateDis2}, {kSoftResetPhyD, kGateDis2PhyD}},                                    // L3
    {1, 0, kRmw, 5, {kRes13, kRes13, kRes13, kRes13, kRes13}, {0x1, 0x300, 0x20000000, 0xC0000000, 0x0C000000}},          // L4
    {1, 0, kRmw, 5, {kRes28, kRes29, kRes1, kRes1, kRes34}, {0xF, 0xF, 0x30, 0x200, 0xFFF}},                              // L5
    {1, 50, kRmw, 5, {kRes28, kRes29, kRes15, kRes16, kRes1}, {0x3CF0, 0x3CF0, 0x8, 0x808, 0x3000}},                      // L6
    {1, 25, kRmw, 3, {kRes1, kRes13, kRes13}, {0x2, 0x1, 0x300}},                                                        // L7
    {1, 0, kRmw, 1, {kRes34}, {0xF000}},                                                                                 // L8
    {1, 0, kWait, 1, {kRes13}, {kRes13InitDone}},                                                                        // L9
    {1, 0, kWait, 1, {kRes34}, {0xF0000}},                                                                               // L10
    {1, 0, kBurst, 1, {kRes18}, {0}},                                                                                    // L11
    {1462, 0, kBurst, 14, {kRes20}, {0}},                                                                                // L12..L1473
    {1, 0, kBurst, 12, {kRes20}, {0}},                                                                                   // L1474
    {1, 0, kRmw, 1, {kRes13}, {kRes13ExtLdDone}},                                                                        // L1475
};
static constexpr unsigned kLoadN = 1475, kProN = 11, kL9 = 8, kL10 = 9, kL11 = 10, kLast = kLoadN - 1;
static_assert(kPhxC3ImageWords == 20480u && (kLoadN - kProN - 1) * kMaxBurst - 2 == kPhxC3ImageWords, "1463 bursts carry the image");
// The fixed commands L1..L11 and L1475 (the firmware's program, value by value).
static const uint32_t kPro[kProN][phxdmub::kCmdDw] = {
    {0x0C000001u, kRes13, 0x8, 0},
    {0x18000001u, kGateDis2, kGateDis2PhyD, kGateDis2PhyD, kDcioSoftReset, kSoftResetPhyD, kSoftResetPhyD},
    {0x18000001u, kDcioSoftReset, kSoftResetPhyD, 0, kGateDis2, kGateDis2PhyD, 0},
    {0x3C000001u, kRes13, 0x1, 0x1, kRes13, 0x300, 0, kRes13, 0x20000000, 0, kRes13, 0xC0000000, 0, kRes13, 0x0C000000, 0x0C000000},
    {0x3C000001u, kRes28, 0xF, 0x3, kRes29, 0xF, 0x3, kRes1, 0x30, 0x30, kRes1, 0x200, 0x200, kRes34, 0xFFF, 0xFF5},
    {0x3C000001u, kRes28, 0x3CF0, 0x2490, kRes29, 0x3CF0, 0x2490, kRes15, 0x8, 0x8, kRes16, 0x808, 0x808, kRes1, 0x3000, 0x3000},
    {0x24000001u, kRes1, 0x2, 0x2, kRes13, 0x1, 0, kRes13, 0x300, 0x300},
    {0x0C000001u, kRes34, 0xF000, 0xA000},
    {0x10000004u, kRes13, kRes13InitDone, kRes13InitDone, kWaitMaxUs},
    {0x10000004u, kRes34, 0xF0000, 0xA0000, kWaitMaxUs},
    {0x04000003u, kRes18, kSramAddr},
};
static const uint32_t kEnd[phxdmub::kCmdDw] = {0x0C000001u, kRes13, kRes13ExtLdDone, kRes13ExtLdDone};

// Shape of command i of a table (nullptr past its end).
inline const Shape *shapeAt(const Shape *t, unsigned nRows, unsigned i) {
    for (unsigned r = 0; r < nRows; r++) { if (i < t[r].count) return &t[r]; i -= t[r].count; }
    return nullptr;
}
// Command i (0-based, L(i+1)) of the mode-9 program; img = the image (the embedded one; a host test passes a copy).
inline bool loadCmd(unsigned i, uint32_t *c, const uint16_t *img = kPhxC3Image) {
    for (unsigned k = 0; k < phxdmub::kCmdDw; k++) c[k] = 0;
    if (i < kProN) { for (unsigned k = 0; k < phxdmub::kCmdDw; k++) c[k] = kPro[i][k]; return true; }
    if (i == kLast) { for (unsigned k = 0; k < phxdmub::kCmdDw; k++) c[k] = kEnd[k]; return true; }
    if (i > kLast) return false;
    const unsigned at = (i - kProN) * kMaxBurst, n = kPhxC3ImageWords - at < kMaxBurst ? kPhxC3ImageWords - at : kMaxBurst;
    c[0] = kBurst | (4u * n) << 24; c[1] = kRes20;
    for (unsigned k = 0; k < n; k++) c[2 + k] = img[at + k];
    return true;
}

// The 1a1-1a5 programs, all on OTG1_OTG_H_TOTAL except 1a5. b = (OTG1_OTG_H_TOTAL & 1) read by the host first:
// 1a2 waits for the value it has (matches at once), 1a3 for the other one (never matches: the firmware's timeout).
static const Shape kT1Shape[] = {{1, 0, kBurst, 14, {kOtg1HTotal}, {0}}};
static const Shape kT2Shape[] = {{1, 0, kWait, 1, {kOtg1HTotal}, {0x1}}};
static const Shape kT4Shape[] = {{1, 0, kRmw, 2, {kOtg1HTotal, kOtg1HTotal}, {0x2, 0x4}}};
static const Shape kT5Shape[] = {{1, 0, kWait, 1, {kRes13}, {kRes13InitDone}}, {1, 0, kWait, 1, {kRes13}, {kRes13ExtLdDone}}};
static constexpr uint32_t kT1First = 0x11, kT1Last = 0x1E, kT23TimeoutUs = 1000, kT5TimeoutUs = 2000;
inline void t1Cmd(uint32_t *c) {
    for (unsigned k = 0; k < phxdmub::kCmdDw; k++) c[k] = 0;
    c[0] = 0x38000003u; c[1] = kOtg1HTotal;
    for (unsigned k = 0; k < kMaxBurst; k++) c[2 + k] = kT1First + k;
}
inline void t23Cmd(uint32_t cond, uint32_t *c) {
    for (unsigned k = 0; k < phxdmub::kCmdDw; k++) c[k] = 0;
    c[0] = 0x10000004u; c[1] = kOtg1HTotal; c[2] = 0x1; c[3] = cond & 1u; c[4] = kT23TimeoutUs;
}
inline void t4Cmd(uint32_t *c) {
    for (unsigned k = 0; k < phxdmub::kCmdDw; k++) c[k] = 0;
    c[0] = 0x18000001u; c[1] = kOtg1HTotal; c[2] = 0x2; c[3] = 0x2; c[4] = kOtg1HTotal; c[5] = 0x4; c[6] = 0x4;
}
inline void t5Cmd(unsigned k, uint32_t *c) {
    for (unsigned j = 0; j < phxdmub::kCmdDw; j++) c[j] = 0;
    const uint32_t m = k ? kRes13ExtLdDone : kRes13InitDone;
    c[0] = 0x10000004u; c[1] = kRes13; c[2] = m; c[3] = m; c[4] = kT5TimeoutUs;
}

// The exact-shape check every command passes just before phxdmub::put: the header, the table row's (type, address, mask),
// the allow-list, the value rules. Anything else = refused (nothing sent).
inline bool cmdOk(const uint32_t *c, const Shape &s, const char **why) {
    const char *w = nullptr;
    const uint32_t h = c[0], type = h & 0xff, pb = (h >> 24) & 0x3f;
    unsigned used = phxdmub::kCmdDw;
    if (h & 0xC0FFFF00u) w = "header sub_type / status / reserved bits set";
    else if (type != s.type) w = "type not the table's";
    else if (type == kRmw) {
        if (s.n < 1 || s.n > kMaxRmw || pb != 12 * s.n) w = "type 1 payload not 12 x the table's sequences";
        for (unsigned k = 0; k < s.n && !w; k++) {
            const uint32_t a = c[1 + 3 * k], m = c[2 + 3 * k], v = c[3 + 3 * k];
            if (a != s.addr[k] || m != s.mask[k]) w = "type 1 address / mask not the table's";
            else if (!m || m == kPlainStoreMask) w = "type 1 mask 0 or 0x0000FFFF (the Linux build's plain store)";
            else if (v & ~m) w = "type 1 value outside its mask";
            else if (!allowed(kRmw, a, m)) w = "type 1 (address, mask) not on the allow-list";
        }
        used = 1 + 3 * s.n;
    } else if (type == kBurst) {
        const uint32_t a = c[1];
        if (s.n < 1 || s.n > kMaxBurst || pb != 4 * s.n) w = "type 3 payload not 4 x the table's values";
        else if (a != s.addr[0]) w = "type 3 address not the table's";
        else if (!allowed(kBurst, a, 0)) w = "type 3 address not on the allow-list";
        else if (a == kRes18 && (s.n != 1 || c[2] != kSramAddr)) w = "RESERVED18 other than the single 0x8000";
        for (unsigned k = 0; k < s.n && !w; k++) if (c[2 + k] > 0xFFFFu) w = "type 3 value above 0xFFFF";
        used = 2 + s.n;
    } else if (type == kWait) {
        const uint32_t a = c[1], m = c[2], cond = c[3], to = c[4];
        if (s.n != 1 || pb != 16) w = "type 4 payload not 16";
        else if (a != s.addr[0] || m != s.mask[0]) w = "type 4 address / mask not the table's";
        else if (!m || (cond & ~m)) w = "type 4 condition outside its mask";
        else if (!to || to > kWaitMaxUs) w = "type 4 timeout not 1..20000 us";
        else if (!allowed(kWait, a, m)) w = "type 4 (address, mask) not on the allow-list";
        used = 5;
    } else w = "type not 1, 3 or 4";
    for (unsigned k = used; k < phxdmub::kCmdDw && !w; k++) if (c[k]) w = "dwords past the payload not zero";
    if (why) *why = w;
    return !w;
}

// CRC-32 (zlib polynomial 0xEDB88320) of the image as its little-endian bytes: compared with kPhxC3ImageCrc32 before mode 9.
inline uint32_t imageCrc(const uint16_t *h, unsigned n) {
    uint32_t c = 0xffffffffu;
    for (unsigned i = 0; i < 2 * n; i++) {
        c ^= (h[i >> 1] >> ((i & 1) * 8)) & 0xffu;
        for (unsigned b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

}  // namespace phxc3
