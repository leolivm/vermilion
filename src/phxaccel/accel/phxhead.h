// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxhead.h: the pure pieces of PhxAMDFB's second head (M7 S3a). Kernel-free, so
// the same header builds in PhxAccel (phxfb.cpp, phxagdc.cpp) and in a host test.
//
// S3a = a phantom head: macOS gets a second framebuffer with its own surface in VRAM and its own 60 Hz VBL timer; the
// display engine is never touched for it (no DCN register read or write, no HUBP binding). Frames WindowServer
// composites for it are copied by the CPU into its surface (the accelerator's display pipe, as head 0's copy path).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace phxhead {

constexpr unsigned kMaxHeads = 2;

// ---- VRAM layout (byte offsets from the start of VRAM = the start of the BAR the GOP console sits in) ---------------
// Evidence (M7 S3a, VRAM placement): 0.. = the GOP console A (8.3 MB at 1080p, up to ~33 MB
// at 4K); 16 MiB = phxfb's flip buffer B (debug.phxfb_flip, off); 24-48 MiB = the three GPU-present slots
// (PhxNativeABI.h PHXN_SCAN_SLOT_VRAM); 48 MiB = the PSP ring + cmd + fence (bringup_navi31.sh RING_OFF, 36 KiB used,
// 1 MiB kept); 96 MiB = the TMR (to 115 MiB); 128 MiB fw_buf; 136 MiB VCN ring; 140 MiB IH ring; 144 MiB SMU tables;
// 160-164 MiB mesboot; 164-240 MiB page tables. 64-96 MiB is used by nothing in the kext, mesboot (only its manual
// fbflip/fbcomp test verbs paint 64 MiB) or the shim. Head 1 takes 64-72 MiB for its surface and keeps 72-96 MiB for
// its three GPU-present slots of S4b (not used in S3a).
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kFlipBLo = 16 * kMiB, kSlotsLo = 24 * kMiB, kSlotsHi = 48 * kMiB;
constexpr uint64_t kPspLo = 48 * kMiB, kPspHi = 49 * kMiB, kTmrOff = 96 * kMiB;
constexpr uint64_t kHead1SurfOff = 64 * kMiB, kHead1SurfCap = 8 * kMiB;
constexpr uint64_t kHead1SlotsOff = 72 * kMiB, kHead1SlotBytes = 8 * kMiB;   // S4b, reserved
constexpr uint32_t kPitchAlign = 256;   // bytes; DCN HUBP pitch is in pixels, 256 B keeps any later binding legal
constexpr uint64_t kRegionAlign = 64 * 1024;

enum : int {
    kOk = 0,
    kErrSize = -1,        // width/height out of range
    kErrTooBig = -2,      // the surface does not fit its 8 MiB region
    kErrBar = -3,         // the region is past the end of the BAR
    kErrOverlapA = -4,    // the region overlaps head 0's console surface
    kErrOverlapFixed = -5 // the region overlaps a fixed user (slots, PSP ring, TMR)
};

struct Layout { uint64_t off, bytes, region; uint32_t w, h, pitch; };

constexpr bool overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 < b1 && b0 < a1; }

// Head 1's surface for a w x h 32 bpp mode. barLen = the console BAR's length; [aOff, aOff + aBytes) = head 0's A.
inline int planSurface(uint32_t w, uint32_t h, uint64_t barLen, uint64_t aOff, uint64_t aBytes, Layout *out) {
    if (!out) return kErrSize;
    if (w < 640 || w > 4096 || h < 480 || h > 2304) return kErrSize;
    const uint32_t pitch = (w * 4 + kPitchAlign - 1) / kPitchAlign * kPitchAlign;
    const uint64_t bytes = static_cast<uint64_t>(pitch) * h;
    const uint64_t region = (bytes + kRegionAlign - 1) / kRegionAlign * kRegionAlign;
    if (region > kHead1SurfCap) return kErrTooBig;
    const uint64_t off = kHead1SurfOff, end = off + region;
    if (end > barLen) return kErrBar;
    if (aBytes && overlaps(off, end, aOff, aOff + aBytes)) return kErrOverlapA;
    if (overlaps(off, end, kSlotsLo, kSlotsHi) || overlaps(off, end, kPspLo, kPspHi) || end > kTmrOff) return kErrOverlapFixed;
    *out = Layout{off, bytes, region, w, h, pitch};
    return kOk;
}

// The Metal plugin's WindowServer GPU present (NVMTLWSPresent.m nvmtl_ws_note_shown / _note_display) takes any BGRA or
// l10r surface of at least 1920 x 1080 a swap commit writes and flips HUBP0 to it. A head-1 mode of that size would put
// head 1's frames on head 0's monitor, so by default head 1 runs a smaller mode. Since S4b (10-30)
// the plugin's head gate (nvmtl_headmap_gate.c, ws-headmap = enforce, fed by debug.phxsurfmap) refuses head 1's surfaces,
// and tools/phxctl/m7_boot.sh lifts this rule per boot (debug.phxfb1_force=1 + 1920x1080) ONLY when that gate
// is on and the kernel table exists. Keep the rule: an old plugin, or the gate off, must never see a 1920x1080 head 1.
constexpr bool pluginSafe(uint32_t w, uint32_t h) { return !(w >= 1920 && h >= 1080); }
constexpr uint32_t kDefW = 1680, kDefH = 1050;   // a standard timing both monitors' EDIDs list, under 1920 x 1080
constexpr uint32_t kRefreshMilliHz = 60000;      // head 1's synthetic VBL
constexpr uint64_t framePeriodNs(uint32_t milliHz) { return milliHz ? 1000000000000ull / milliHz : 16666667ull; }

// ---- S3b: head n's display pipe, read only (OTG n, HUBP n) --------------------------------------------------------------
// Head n is scanned by pipe n (S2: the ladder lights OTG1/HUBP1 for head 1). Absolute BAR5 dwords: DCN 3.2 segment 2 base
// 0x34c0 + dcn_3_2_0_offset.h; OTG/ODM stride 0x80, HUBP/HUBPREQ stride 0xdc. Head 0's values are the ones phxfb.cpp
// always used (measureRefresh, usToVblank, the flip registers).
struct PipeRegs { uint32_t otgControl, hTotal, vTotal, vBlank, pos, frame, hubpCntl, addrLo, addrHi; };
constexpr PipeRegs pipeRegs(unsigned n) {
    return { 0x5001 + 0x80 * n, 0x4fea + 0x80 * n, 0x4fef + 0x80 * n, 0x4ff6 + 0x80 * n, 0x500a + 0x80 * n,
             0x500c + 0x80 * n, 0x3ab3 + 0xdc * n, 0x3aca + 0xdc * n, 0x3acb + 0xdc * n };
}
constexpr uint32_t kRegFbLocationBase = 0x3935;
// S4a: the double buffer's HUBPREQ n registers (phxfb.cpp flip*: head 0's were its fixed kReg* constants) and the VRAM
// window its back buffer B may take: head 0 16-48 MiB (below PhxBringup's PHX_VRAM_RING_MIN, as before), head 1 the
// 72-80 MiB slot reserved next to its surface (the S4b GPU-present slots keep 80-96 MiB).
struct FlipRegs { uint32_t addrLo, addrHi, pitch, flipCtl, inuseLo, inuseHi; };
constexpr FlipRegs flipRegs(unsigned n) {
    return { 0x3aca + 0xdc * n, 0x3acb + 0xdc * n, 0x3ac7 + 0xdc * n, 0x3adb + 0xdc * n, 0x3ae4 + 0xdc * n, 0x3ae5 + 0xdc * n };
}
struct Range { uint64_t lo, hi; };
constexpr Range flipBRange(unsigned n) { return n == 0 ? Range{ 16 * kMiB, 48 * kMiB } : Range{ kHead1SlotsOff, kHead1SlotsOff + kHead1SlotBytes }; }
   // DCN_VM_FB_LOCATION_BASE, 16 MiB units
// OTG_CONTROL: MASTER_EN (0) and CURRENT_MASTER_EN_STATE (16) = the timing generator runs
constexpr bool otgRunning(uint32_t ctl) { return (ctl & 1u) && (ctl & (1u << 16)); }
// microseconds from scan line v to the vblank start vbs of a vt-line frame of frameUs (200 when the registers make no sense)
constexpr uint64_t usToVblank(uint32_t v, uint32_t vbs, uint32_t vt, uint64_t frameUs) {
    return (!vt || v >= vt || vbs >= vt) ? 200 : (uint64_t)((vbs + vt - v) % vt) * frameUs / vt;
}
// The refresh from a frame count over ns, snapped to a standard rate within 2 %; 0 = out of range (phxfb.cpp
// measureRefresh's arithmetic, now shared by both heads).
inline uint32_t refreshMilliHz(uint32_t frames, uint64_t ns) {
    if (!ns || frames < 4) return 0;
    uint32_t mhz = (uint32_t)(((uint64_t)frames * 1000000000000ull) / ns);
    static const uint32_t kStd[] = { 48000, 50000, 60000, 75000, 90000, 100000, 120000, 144000, 165000, 240000 };
    for (uint32_t k : kStd) if (mhz > k * 49 / 50 && mhz < k * 51 / 50) { mhz = k; break; }
    return (mhz < 24000 || mhz > 360000) ? 0 : mhz;
}
// The phase lock of a head's VBL tick: the free-running deadline is candUs from now (one period after the last), the
// pipe's next vblank start is posUs from now. Take the pipe's when it is within a quarter frame of the candidate, so a
// tick never doubles (a tick fired just before vblank start sees posUs ~ 0 while candUs ~ one frame) nor skips.
constexpr uint64_t lockedNextUs(uint64_t candUs, uint64_t posUs, uint64_t frameUs) {
    return (posUs + frameUs / 4 >= candUs && posUs <= candUs + frameUs / 4) ? posUs : candUs;
}
// HUBP n scans this head's surface: unblanked (DCHUBP_CNTL HUBP_BLANK_EN bit 0 = 0) and its primary address is the
// surface's MC address (FB location base + VRAM offset).
constexpr bool scansSurface(uint32_t hubpCntl, uint32_t addrLo, uint32_t addrHi, uint32_t fbBase, uint64_t vramOff) {
    return !(hubpCntl & 1u) && ((((uint64_t)(addrHi & 0xffff)) << 32) | addrLo) == (((uint64_t)(fbBase & 0xffffff)) << 24) + vramOff;
}

// ---- AGDC kAGDCGPUCapability (0x980) ----------------------------------------------------------------------------------
// n framebuffers: portMask = OR of (2 << i), countA = maxFramebuffers = n (Cobalt64 native_agdc: mask, +0x20, +0x30 =
// nfb, measured with 2 and 3 displays on Tahoe). countB..D stay 1 as PhxAGDC answered for one framebuffer, so n = 1 is
// exactly the pre-M7 reply (mask 2, counts 1, max 1).
struct GpuCaps { uint64_t portMask; uint32_t countA, countB, countC, countD, maxFramebuffers; };
inline GpuCaps agdcCaps(unsigned n) {
    if (n < 1) n = 1;
    if (n > kMaxHeads) n = kMaxHeads;
    GpuCaps c{0, n, 1, 1, 1, n};
    for (unsigned i = 0; i < n; i++) c.portMask |= 2ull << i;
    return c;
}
// 0x921 / 0x711 carry an endpoint dword = the index into the 0x980 list (Cobalt64, measured); out of range = head 0.
constexpr unsigned agdcPosition(uint32_t ep, unsigned n) { return ep < n ? ep : 0u; }

// ---- head 1's mode list (mode switch; the R0 rules of IOFramebuffer's setDisplayMode) ----------------------------
// -------------------------------------------------------------------------------------------------------------------
// Behind debug.phxfb1_modes=1 (read at head-1 start; macOS caches the list, it must be complete from the first query):
// the pipe-1 plans PhxBringup has compiled (b92 debug.phxpipe2_switch 60 / 144, head 1 1920x1080), each offered only
// when the monitor's EDID has a DTD with its exact timing. ids are fixed per plan.
struct HeadMode { uint32_t id, w, h, milliHz, pclk10kHz, hTotal, vTotal; };
static const HeadMode kHead1Modes[] = {
    {1, 1920, 1080, 60000, 14850, 2200, 1125},    // CEA 1080p60 (the 60 plan)
    {2, 1920, 1080, 144000, 32508, 2056, 1098},   // the Acer KG241QS DTD 1 (the 144 plan)
    {3, 1920, 1080, 165000, 38096, 2080, 1110},   // acc165: the Acer CTA DTD 1, HDMI 2.0 (the kext b108 165 plan; Linux runs it at 164.80 Hz)
};
constexpr unsigned kNHead1Modes = sizeof kHead1Modes / sizeof kHead1Modes[0];
// Head 0's (the AOC on DP, pipe 0; AOC at 144): the pipe-0 plans PhxBringup
// b101 has compiled (PhxPipe0.h phx_p0_plan_1080p60.inc / _1080p144.inc, switched by "phxPipe0SetMode"), offered behind the
// kext's debug.phxpipe0_modes=1 (property phx-pipe0-modes, read when head 0 starts) and only for the DTDs the EDID has.
static const HeadMode kHead0Modes[] = {
    {1, 1920, 1080, 60000, 14850, 2200, 1125},    // CEA 1080p60 (the mode the GOP lights; the 60 plan, OTG0 0x897/0x464)
    {2, 1920, 1080, 144000, 32567, 2056, 1100},   // the AOC 24G2W1G4 CTA DTD at 0xa7 (the 144 plan, OTG0 0x807/0x44b)
};
constexpr unsigned kNHead0Modes = sizeof kHead0Modes / sizeof kHead0Modes[0];
constexpr unsigned kNMaxModes = kNHead0Modes > kNHead1Modes ? kNHead0Modes : kNHead1Modes;
struct ModeTable { const HeadMode *m; unsigned n; };
inline ModeTable headModes(unsigned head) { return head == 0 ? ModeTable{kHead0Modes, kNHead0Modes} : ModeTable{kHead1Modes, kNHead1Modes}; }
// One 18-byte detailed timing descriptor = m (pixel clock within 0.2 %, active and total sizes exact).
inline bool dtdIs(const uint8_t *d, const HeadMode &m) {
    const uint32_t pc = d[0] | (uint32_t)d[1] << 8;
    if (!pc) return false;
    const uint32_t ha = d[2] | (uint32_t)(d[4] >> 4) << 8, hb = d[3] | (uint32_t)(d[4] & 15) << 8;
    const uint32_t va = d[5] | (uint32_t)(d[7] >> 4) << 8, vb = d[6] | (uint32_t)(d[7] & 15) << 8;
    const uint32_t lo = m.pclk10kHz - m.pclk10kHz / 500, hi = m.pclk10kHz + m.pclk10kHz / 500;
    return pc >= lo && pc <= hi && ha == m.w && va == m.h && ha + hb == m.hTotal && va + vb == m.vTotal;
}
// The EDID (base block + CTA-861 extensions) has a DTD for m.
inline bool edidHasMode(const uint8_t *e, uint32_t len, const HeadMode &m) {
    static const uint8_t kHdr[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (!e || len < 128) return false;
    for (unsigned i = 0; i < 8; i++) if (e[i] != kHdr[i]) return false;
    for (unsigned o = 54; o <= 108; o += 18) if (dtdIs(e + o, m)) return true;
    for (uint32_t b = 128; b + 128 <= len; b += 128) {
        const uint8_t *x = e + b;
        if (x[0] != 0x02 || x[2] < 4 || x[2] > 127) continue;   // CTA-861 block, DTDs from offset x[2]
        for (unsigned o = x[2]; o + 18 <= 127; o += 18) { if (!x[o] && !x[o + 1]) break; if (dtdIs(x + o, m)) return true; }
    }
    return false;
}
// The ids head `head` offers for a w x h surface, in table order (out holds kNMaxModes).
inline unsigned modeList(unsigned head, const uint8_t *edid, uint32_t len, uint32_t w, uint32_t h, uint32_t *out) {
    const ModeTable t = headModes(head);
    unsigned n = 0;
    for (unsigned i = 0; i < t.n; i++) if (t.m[i].w == w && t.m[i].h == h && edidHasMode(edid, len, t.m[i])) out[n++] = t.m[i].id;
    return n;
}
inline const HeadMode *headMode(unsigned head, uint32_t id) {
    const ModeTable t = headModes(head);
    for (unsigned i = 0; i < t.n; i++) if (t.m[i].id == id) return &t.m[i];
    return nullptr;
}
// The listed mode whose timing has these totals (OTG H/V_TOTAL + 1), 0 = none. Exact, unlike a 200 ms frame count, which
// is +-1 frame = +-3.5 % at 144 Hz (boot 10-30 21:19: 28 frames -> 139.218 Hz, no mode, the list was dropped).
inline uint32_t modeForTotals(unsigned head, uint32_t hTotal, uint32_t vTotal, const uint32_t *ids, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        const HeadMode *m = headMode(head, ids[i]);
        if (m && m->hTotal == hTotal && m->vTotal == vTotal) return m->id;
    }
    return 0;
}
// The listed mode a measured refresh is (within 3 %), 0 = none.
inline uint32_t modeFor(unsigned head, uint32_t milliHz, const uint32_t *ids, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        const HeadMode *m = headMode(head, ids[i]);
        if (m && milliHz >= m->milliHz - m->milliHz * 3 / 100 && milliHz <= m->milliHz + m->milliHz * 3 / 100) return m->id;
    }
    return 0;
}
// A mode's frame period from its pixel clock and totals (the VBL tick of a head whose totals identified the mode: a 200 ms
// frame count at 144 Hz is +-3.5 %; 2056 x 1100 / 325.67 MHz = 6944452 ns, 2200 x 1125 / 148.5 MHz = 16666666 ns).
constexpr uint64_t modeFrameNs(const HeadMode &m) {
    return m.pclk10kHz ? (uint64_t)m.hTotal * m.vTotal * 100000ull / m.pclk10kHz : 16666667ull;
}
// ---- head 0's detailed timings (F1) ----------------------------------------
// What getTimingInfoForDisplayMode reports for head 0's listed modes, so IOFramebuffer saves a startup-timing
// (extSetStartupDisplayMode keeps one only for a kIODetailedTimingValid mode) and CoreDisplay's connect-change handler
// re-applies that mode instead of the default. Each one is the AOC EDID DTD of its plan exactly as IOGraphicsLib's
// EDIDDescToDetailedTiming decodes it (clock in Hz, sync offset = front porch, no borders, polarity from the DTD flags):
// IOFBInstallMode hides a driver mode (kDisplayModeNeverShowFlag) that has an EDID DTD's clock and blanking but another sync.
// The sync of each mode, by id (clock, active and totals are kHead0Modes'):
struct HeadSync { uint32_t id, hSyncOffset, hSyncWidth, vSyncOffset, vSyncWidth; bool hSyncPos, vSyncPos; };
static const HeadSync kHead0Sync[] = {
    {1, 88, 44, 4, 5, true, true},   // base DTD @0x36, flags 0x1e; the 60 plan's OTG0 H_SYNC_A 0x2c, V_SYNC_A 5
    {2, 24, 32, 3, 5, true, true},   // CTA DTD @0xa7, flags 0x1e; the 144 plans' OTG0 H_SYNC_A 0x20, V_SYNC_A 5
};
struct DetailedTiming {
    uint64_t pixelHz;
    uint32_t hActive, hBlank, hSyncOffset, hSyncWidth, vActive, vBlank, vSyncOffset, vSyncWidth;
    bool hSyncPos, vSyncPos;
};
// Head 0's mode id -> its detailed timing; false for head 1 (no detailed timing, as before) and for unknown ids.
inline bool detailedTiming(unsigned head, uint32_t id, DetailedTiming *out) {
    const HeadMode *m = head == 0 ? headMode(0, id) : nullptr;
    if (!m || !out) return false;
    for (const HeadSync &s : kHead0Sync) {
        if (s.id != id) continue;
        *out = {(uint64_t)m->pclk10kHz * 10000ull, m->w, m->hTotal - m->w, s.hSyncOffset, s.hSyncWidth,
                m->h, m->vTotal - m->h, s.vSyncOffset, s.vSyncWidth, s.hSyncPos, s.vSyncPos};
        return true;
    }
    return false;
}
// head 1's names (m7modes1 .. m7gamma1, host-tested)
inline unsigned head1ModeList(const uint8_t *edid, uint32_t len, uint32_t w, uint32_t h, uint32_t out[kNHead1Modes]) { return modeList(1, edid, len, w, h, out); }
inline const HeadMode *head1Mode(uint32_t id) { return headMode(1, id); }
inline uint32_t head1ModeForTotals(uint32_t hTotal, uint32_t vTotal, const uint32_t *ids, unsigned n) { return modeForTotals(1, hTotal, vTotal, ids, n); }
inline uint32_t head1ModeFor(uint32_t milliHz, const uint32_t *ids, unsigned n) { return modeFor(1, milliHz, ids, n); }
// PhxBringup's switch result (PhxNativeABI.h phx_pipe2_setmode.result = phxp2::SwRes) -> what setDisplayMode does (R0 D9-D12):
// Success only when the target is lit; refused = NotReady (nothing touched); fell back = IOError with the old id;
// dark = IOError + head 1 offline (connect-change interrupt), else WindowServer keeps drawing to a dead head.
enum SwAction : int { kActDone = 0, kActRefused = 1, kActFellBack = 2, kActDark = 3 };
constexpr int swAction(int swRes) {
    return swRes == 0 || swRes == 1 ? kActDone : swRes == 2 ? kActRefused : swRes == 3 ? kActFellBack : kActDark;
}
// Head 0 (b101 phx_pipe0_setmode.result = phxp0::Res: 0 done, 1 same, 2 refused, 3 fell back, 4 dark; -1 not run) uses the
// same numbers and the same mapping; head 0 stays online on dark (it is the console and the head the accelerator paces on;
// the GOP relights 1080p60 at the next boot), so setDisplayMode only returns the error.
constexpr int p0Action(int res) { return res < 0 ? kActRefused : swAction(res); }

// ---- the heads' EDID fallback -----------------------------------------------------------------------------------------
// When the live read fails, a head serves the EDID of the monitor its source names: DDC1 = the Acer (HDMI, kPhxEDID in
// phxfb.cpp), any DP AUX = the AOC 24G2W1G4 (the Linux 10-23 AUX2 capture below,
// with its serial number fields cleared in this release: ID serial bytes 12-15 = 0, the 0xFF serial descriptor empty, checksum
// byte 127 recomputed; md5 8dbf52e4d6780688085d19e70e42cefd). It is a generic example of that model, not anyone's monitor.
enum : int { kFallbackAcer = 0, kFallbackAoc = 1 };
constexpr int fallbackFor(uint32_t src) { return (src & 0xF0) == 0x20 ? kFallbackAoc : kFallbackAcer; }
// phxfb.cpp loadEdid's choice for head `head` (srcValid = phxddc::validSource(src), src = the source the read reported,
// which PhxBringup writes back even when the read fails). Both heads follow their source: head 0 used to keep kPhxEDID
// whatever its source, so the AOC on pipe 0 (DP AUX) got the Acer's EDID when its live read failed. With no
// valid source (no provider, -phxedidstatic, head 1 on a kext older than b81) each head keeps its former default.
constexpr int fallbackForHead(unsigned head, bool srcValid, uint32_t src) {
    return srcValid ? fallbackFor(src) : (head != 0 ? kFallbackAoc : kFallbackAcer);
}
static const uint8_t kAocEdid[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x05, 0xe3, 0x02, 0x24,
    0x00, 0x00, 0x00, 0x00, 0x1b, 0x21, 0x01, 0x04, 0xa5, 0x35, 0x1e, 0x78,
    0x3b, 0x0a, 0x85, 0xaa, 0x4f, 0x46, 0xab, 0x27, 0x0d, 0x50, 0x54, 0xbf,
    0xef, 0x00, 0xd1, 0xc0, 0x81, 0x80, 0x31, 0x68, 0x31, 0x7c, 0x45, 0x68,
    0x45, 0x7c, 0x61, 0x68, 0x61, 0x7c, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38,
    0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x0f, 0x28, 0x21, 0x00, 0x00, 0x1e,
    0x00, 0x00, 0x00, 0xff, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x32,
    0x34, 0x47, 0x32, 0x57, 0x31, 0x47, 0x34, 0x0a, 0x20, 0x20, 0x20, 0x20,
    0x00, 0x00, 0x00, 0xfd, 0x00, 0x30, 0x90, 0xa0, 0xa0, 0x21, 0x01, 0x0a,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x1d, 0x02, 0x03, 0x27, 0xf1,
    0x4c, 0x01, 0x03, 0x05, 0x14, 0x04, 0x13, 0x1f, 0x12, 0x02, 0x11, 0x90,
    0x3f, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0x6d, 0x1a, 0x00,
    0x00, 0x02, 0x01, 0x30, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37,
    0x7f, 0x80, 0x88, 0x70, 0x38, 0x14, 0x40, 0x18, 0x20, 0x35, 0x00, 0x0f,
    0x28, 0x21, 0x00, 0x00, 0x1e, 0x86, 0x6f, 0x80, 0xa0, 0x70, 0x38, 0x40,
    0x40, 0x30, 0x20, 0x35, 0x00, 0x0f, 0x28, 0x21, 0x00, 0x00, 0x1e, 0xfe,
    0x5b, 0x80, 0xa0, 0x70, 0x38, 0x35, 0x40, 0x30, 0x20, 0x35, 0x00, 0x0f,
    0x28, 0x21, 0x00, 0x00, 0x1e, 0x2a, 0x44, 0x80, 0xa0, 0x70, 0x38, 0x27,
    0x40, 0x30, 0x20, 0x35, 0x00, 0x0f, 0x28, 0x21, 0x00, 0x00, 0x1a, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x4e,
};

}  // namespace phxhead
