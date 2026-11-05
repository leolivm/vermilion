// SPDX-License-Identifier: MIT
// PhxDpLink.h: multiport S2a: DisplayPort 8b/10b link training and the
// DIO stream-encoder sequence for a DP connector the GOP did not light (DP A = DIG0 / PHY A / AUX3 / HPD3, DP E = DIG4 /
// PHY E / AUX4 / HPD4 on this board). Kernel-free (stdint only). Kext b116 uses verifyLink through PhxDpRun.h
// (debug.phxdplink); b117/b119 use enableStream / disableStream through PhxDpStream.h (debug.phxdpstream). b119 adds
// Opts.onePpc (default off = every Linux golden unchanged): the DIG/DP side in the GOP's one-pixel-per-clock convention, for
// a DIG fed from the GOP's OTG0 (S2d).
// Host-tested (with mutations), also through PhxDpRun.h.
//
// Source of every step: Linux v6.8 amdgpu DC on this card, two plug traces
// (the AOC 24G2W1G4 on DP E / DP A), cut into
// golden files the host test replays event for event:
//   verifyLink  = Linux dp_verify_link_cap at detection (train only, no stream, link off at the end): S2b's box shape;
//   streamPre + train + streamPost = the modeset's link enable (setup_dio_stream_attribute, enable_link_dp,
//                 perform_link_training, setup_dio_stream_encoder, dp_unblank): S2c's DIG/DP part (the OTG, DTO and
//                 pixel-rate-div side is the plan's).
// Deliberate deviations from Linux: no TRANSMITTER_QUERY_DP_ALT, no source OUI
// (DPCD 0x300/0x303/0x340), no DPCD 0x107 = 0x90 (MSA_TIMING_PAR_IGNORE: no VRR), no audio / SDP / VSC, one attempt by
// default (box tests stop at the first failure), literal register values (Linux's RMW of the same fields gives
// these values from a clean state; the DP A trace shows Linux carrying stale status bits instead).
// Training logic follows Linux link_dp_training_8b_10b.c; RX4Darwin
// src/dptrain.cpp (BSD-3, Sunneva N. Mariu) was read as a design reference for the Io split; no code was copied.
//
// The PHY is never touched directly: TRANSMITTER_CONTROL (DMUB VBIOS 128/1) ENABLE / SETUP_VSEMPH / DISABLE only, with
// the payload bytes Linux sent (bytes 10/11 included). Every MMIO write, DMUB command and DPCD write goes through the Io
// the caller gives; Gate below drops (and counts) anything off the connector's allow-list.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace phxdpl {

constexpr uint32_t kSeg1 = 0xC0, kSeg2 = 0x34C0;   // DCN BASE_IDX 1 / 2 (the card's IP discovery table)

// ---- connectors -----------------------------------------------------------------------------------------------------
// dig = DIG FE = DIG BE = PHY index (multiport 3.4: FE = BE = PHY letter on DCN32); hpd = HPDn sense line; aux = DP_AUXn;
// hpdsel = the DMUB hpdsel byte = VBIOS HPD pin id = hpd + 1 (traces: DP E 5, DP A 4, DP B 3, HDMI D 1).
struct Conn { const char *name; uint8_t dig, phy, hpd, aux, hpdsel; };
static constexpr Conn kConns[] = {
    {"DP-A", 0, 0, 3, 3, 4},
    {"DP-E", 4, 4, 4, 4, 5},
};
static constexpr unsigned kNConns = sizeof kConns / sizeof kConns[0];
// DP B (DIG1 / PHY B) is the GOP's pipe 0 and HDMI D (DIG3 / PHY D) is pipe 1: never in kConns, so never driven here.
inline const Conn *connByDig(unsigned dig) {
    for (const Conn &c : kConns) if (c.dig == dig) return &c;
    return nullptr;
}

// ---- registers (absolute dwords; dcn_3_2_0_offset.h DIG0 / DP0 + 0x100 per instance) -------------------------------
struct Regs {
    uint32_t feCntl, fifo, beCntl, beEn;
    uint32_t linkCntl, pixFmt, msaColor, config, vidStream, steer, msaMisc, internal, vidTiming, vidN, vidM, framing,
             dphyCntl, tpsSel, prbs, scram, secCntl, secFraming4, msa1, msa2, msa3, msa4;
};
constexpr Regs regs(unsigned dig) {
    return Regs{kSeg2 + 0x208b + 0x100 * dig, kSeg2 + 0x2091 + 0x100 * dig, kSeg2 + 0x20b1 + 0x100 * dig,
                kSeg2 + 0x20b2 + 0x100 * dig,
                kSeg2 + 0x2108 + 0x100 * dig, kSeg2 + 0x2109 + 0x100 * dig, kSeg2 + 0x210a + 0x100 * dig,
                kSeg2 + 0x210b + 0x100 * dig, kSeg2 + 0x210c + 0x100 * dig, kSeg2 + 0x210d + 0x100 * dig,
                kSeg2 + 0x210e + 0x100 * dig, kSeg2 + 0x210f + 0x100 * dig, kSeg2 + 0x2110 + 0x100 * dig,
                kSeg2 + 0x2111 + 0x100 * dig, kSeg2 + 0x2112 + 0x100 * dig, kSeg2 + 0x2113 + 0x100 * dig,
                kSeg2 + 0x2117 + 0x100 * dig, kSeg2 + 0x2118 + 0x100 * dig, kSeg2 + 0x211d + 0x100 * dig,
                kSeg2 + 0x211e + 0x100 * dig, kSeg2 + 0x212b + 0x100 * dig, kSeg2 + 0x2130 + 0x100 * dig,
                kSeg2 + 0x214c + 0x100 * dig, kSeg2 + 0x214d + 0x100 * dig, kSeg2 + 0x214e + 0x100 * dig,
                kSeg2 + 0x214f + 0x100 * dig};
}
static_assert(regs(4).config == 0x059CB && regs(4).msa1 == 0x05A0C && regs(4).beCntl == 0x05971 && regs(4).fifo == 0x05951,
              "DP4/DIG4 = the trace's addresses");
static_assert(regs(0).beCntl == 0x05571 && regs(0).linkCntl == 0x055C8, "DIG0/DP0 (multiport 3.3)");

// The registers a connector's sequences may write: exactly these (BE_EN is the firmware's, never written).
inline bool writeAllowed(const Conn &c, uint32_t r) {
    const Regs g = regs(c.dig);
    const uint32_t ok[] = {g.feCntl, g.fifo, g.beCntl, g.linkCntl, g.pixFmt, g.msaColor, g.config, g.vidStream, g.steer,
                           g.msaMisc, g.internal, g.vidTiming, g.vidN, g.vidM, g.framing, g.dphyCntl, g.tpsSel, g.prbs,
                           g.scram, g.secCntl, g.secFraming4, g.msa1, g.msa2, g.msa3, g.msa4};
    if (r >= 0x1A751 && r <= 0x1A762) return false;   // MMVM_INVALIDATE_ENG*_SEM (forbidden: never read)
    for (uint32_t a : ok) if (a == r) return true;
    return false;
}

// Field values (dcn_3_2_0_sh_mask.h; values as the traces wrote them)
constexpr uint32_t kTrainPrbs = 0x7fffff00u;          // DP_DPHY_PRBS_CNTL during training and after (trace)
constexpr uint32_t kScram = 0x0101ff10u;              // DPHY_SCRAMBLER_BS_COUNT 0x1ff, ADVANCE, KCODE (trace)
constexpr uint32_t kFramingIdle = 0x00102000u;        // IDLE_BS_INTERVAL 0x2000 | BACK_TO_BACK_BS_AVOIDANCE
constexpr uint32_t kFramingEnhanced = 0x10000000u;    // DP_VID_ENHANCED_FRAME_MODE
constexpr uint32_t kLinkTrained = 0x10u;              // DP_LINK_CNTL.DP_LINK_TRAINING_COMPLETE
constexpr uint32_t kPixFmt8bpc2ppc = 0x41000000u;     // COMPONENT_DEPTH 1 (8 bpc), RGB, PER_CYCLE_PROCESSING_MODE 1
constexpr uint32_t kPixFmt8bpc1ppc = 0x01000000u;     // b119: the GOP's DP1 value on pipe 0 (one pixel per clock; read on the box)
constexpr uint32_t kMsaMisc0Rgb8 = 0x20000000u;       // DP_MSA_MISC0 0x20: RGB, 8 bpc
constexpr uint32_t kSecFraming4 = 0x20000000u;        // as Linux wrote it (bit 29 = AUDIO_MUTE_STATUS, read-only)
constexpr uint32_t kFifoBase = 0x11cu;                // READ_START_LEVEL 7, OUTPUT_PIXEL_MODE 1 (two pixels per clock)
constexpr uint32_t kFifoBase1ppc = 0x1cu;             // b119: READ_START_LEVEL 7, OUTPUT_PIXEL_MODE 0 (the GOP's DIG1 0x1d enabled)
constexpr uint32_t kFifoEnable = 0x1u, kFifoReset = 0x2u, kFifoResetDone = 0x100000u;
constexpr uint32_t kVidTimingNMul = 0x400u, kVidTimingGen = 0x100u;   // DP_VID_N_MUL 1, DP_VID_M_N_GEN_EN
constexpr uint32_t kVidN = 0x8000u;
constexpr uint32_t kStreamKeepout = 0x100000u, kStreamEnable = 0x1u, kStreamDisDefer2 = 0x200u, kStreamStatus = 0x10000u;
constexpr uint32_t kSteerTu = 0x3f000000u, kSteerReset = 0x1u;
inline constexpr uint32_t beCntl(const Conn &c, bool feOn) {   // DIG_MODE 0 (DP SST), DIG_HPD_SELECT, DIG_FE_SOURCE_SELECT
    return (static_cast<uint32_t>(c.hpd) << 28) | (feOn ? (0x100u << c.dig) : 0u);
}
inline constexpr uint32_t feCntl(unsigned otg, bool stereo) {   // DIG_STEREOSYNC_GATE_EN | STEREOSYNC_SELECT | SOURCE_SELECT
    return 0x100u | (stereo ? (otg << 4) : 0u) | otg;
}
static_assert(beCntl(kConns[1], false) == 0x40000000u && beCntl(kConns[1], true) == 0x40001000u &&
              beCntl(kConns[0], true) == 0x30000100u, "BE_CNTL = trace (DP E) / snap (DP A)");

// ---- link rates, caps, link choice ------------------------------------------------------------------------------------
enum : uint8_t { kRbr = 0x06, kHbr = 0x0a, kHbr2 = 0x14 };
struct Link { uint8_t rate, lanes; };
inline constexpr uint32_t symclk10k(uint8_t rate) { return rate * 2700u; }   // RBR 16200, HBR 27000, HBR2 54000
inline constexpr uint64_t linkKbps(Link l) { return static_cast<uint64_t>(l.rate) * 270000u * 8u / 10u * l.lanes; }
struct Sink { uint8_t rev, maxRate, maxLanes; bool enhanced, tps3, tps4, downspread; uint8_t trainIntervalRaw; };
inline bool parseCaps(const uint8_t caps[16], Sink *s) {
    if (caps[0] < 0x10 || caps[0] > 0x14 || !caps[1] || !(caps[2] & 0x1f)) return false;
    s->rev = caps[0];
    s->maxRate = caps[1];
    s->maxLanes = caps[2] & 0x1f;
    s->enhanced = (caps[2] & 0x80) != 0;
    s->tps3 = (caps[2] & 0x40) != 0;
    s->tps4 = (caps[3] & 0x80) != 0;
    s->downspread = (caps[3] & 0x01) != 0;
    s->trainIntervalRaw = caps[0xe];
    return s->maxLanes == 1 || s->maxLanes == 2 || s->maxLanes == 4;
}
// Linux decide_dp_link_settings order: more lanes before a higher rate; 24 bpp; 8b/10b.
inline bool pickLink(const Sink &s, uint32_t pclkKhz, Link *out) {
    const uint8_t rates[] = {kRbr, kHbr, kHbr2};
    for (uint8_t r : rates) {
        if (r > s.maxRate) break;
        for (uint8_t l = 1; l <= s.maxLanes; l *= 2) {
            const Link k{r, l};
            if (linkKbps(k) >= static_cast<uint64_t>(pclkKhz) * 24u) { *out = k; return true; }
        }
    }
    return false;
}

// ---- DMUB TRANSMITTER_CONTROL (dmub_dig_transmitter_control_data_v1_7, payloads = the traces) -------------------------
enum : uint8_t { kTxDisable = 0, kTxEnable = 1, kTxVsemph = 11 };
constexpr uint32_t kTxHdr = 0x3c000180u;   // type 128 VBIOS, sub 1, payload 60
inline void txCmd(uint32_t c[16], const Conn &k, uint8_t action, uint8_t lanes, uint32_t sym10k, uint8_t laneset) {
    for (int i = 0; i < 16; i++) c[i] = 0;
    c[0] = kTxHdr;
    const uint8_t b2 = action == kTxVsemph ? laneset : 0;   // digmode DP = 0, or dplaneset
    const uint8_t b10 = action == kTxEnable ? 0x00 : 0x13;  // connobj_id (0 on ENABLE; 0x13 as Linux left it otherwise)
    const uint8_t b11 = action == kTxVsemph ? 0xf6 : 0xf4;  // HPO_instance, unused for DIO; the bytes Linux sent
    c[1] = k.phy | (static_cast<uint32_t>(action) << 8) | (static_cast<uint32_t>(b2) << 16) |
           (static_cast<uint32_t>(action == kTxDisable ? 0 : lanes) << 24);
    c[2] = action == kTxDisable ? 0 : sym10k;
    c[3] = k.hpdsel | (static_cast<uint32_t>(b10) << 16) | (static_cast<uint32_t>(b11) << 24);
}
// The exact command shapes this file may send for connector k (anything else: refused, nothing submitted).
inline bool txAllowed(const uint32_t c[16], const Conn &k, const char **why) {
    const char *w = nullptr;
    const uint32_t phy = c[1] & 0xff, act = (c[1] >> 8) & 0xff, b2 = (c[1] >> 16) & 0xff, lanes = c[1] >> 24;
    const uint32_t hs = c[3] & 0xff, fe = (c[3] >> 8) & 0xff, b10 = (c[3] >> 16) & 0xff, b11 = c[3] >> 24;
    const bool rateOk = c[2] == symclk10k(kRbr) || c[2] == symclk10k(kHbr) || c[2] == symclk10k(kHbr2);
    if (c[0] != kTxHdr) w = "not TRANSMITTER_CONTROL (128/1, 60 bytes)";
    else if (phy != k.phy || (phy != 0 && phy != 4)) w = "PHY not this connector's (never PHY B / PHY D)";
    else if (hs != k.hpdsel) w = "hpdsel not this connector's";
    else if (fe != 0) w = "digfe_sel not 0";
    else if (act == kTxEnable) {
        if (b2 != 0) w = "ENABLE digmode not DP";
        else if (lanes != 1 && lanes != 2 && lanes != 4) w = "ENABLE lanes";
        else if (!rateOk) w = "ENABLE symclk not RBR/HBR/HBR2";
        else if (b10 != 0x00 || b11 != 0xf4) w = "ENABLE bytes 10/11";
    } else if (act == kTxVsemph) {
        if ((b2 & ~0x1bu) || (b2 & 3) + ((b2 >> 3) & 3) > 3) w = "SETUP_VSEMPH dplaneset (vs + pe <= 3)";
        else if (lanes != 1 && lanes != 2 && lanes != 4) w = "SETUP_VSEMPH lanes";
        else if (!rateOk) w = "SETUP_VSEMPH symclk";
        else if (b10 != 0x13 || b11 != 0xf6) w = "SETUP_VSEMPH bytes 10/11";
    } else if (act == kTxDisable) {
        if (b2 || lanes || c[2]) w = "DISABLE with lanes/symclk";
        else if (b10 != 0x13 || b11 != 0xf4) w = "DISABLE bytes 10/11";
    } else w = "action not ENABLE / SETUP_VSEMPH / DISABLE (no INIT)";
    for (int i = 4; i < 16 && !w; i++) if (c[i]) w = "payload tail not zero";
    if (why) *why = w;
    return !w;
}

// DPCD writes this file may make: link config 0x100-0x108 and SET_POWER 0x600 (nothing else: no OUI, no MST, no test).
inline bool dpcdWriteAllowed(uint32_t a, unsigned n) {
    if (!n || n > 5) return false;
    if (a == 0x600) return n == 1;
    return a >= 0x100 && a + n <= 0x109;
}

// ---- timing, MSA, M/N, clocks -----------------------------------------------------------------------------------------
struct Timing { uint16_t hTotal, hActive, hFront, hSync, vTotal, vActive, vFront, vSync; uint32_t pclkKhz; bool hNeg, vNeg; };
static const Timing kCea1080p60 = {2200, 1920, 88, 44, 1125, 1080, 4, 5, 148500, false, false};
inline void msa(const Timing &t, uint32_t m[4]) {
    const uint32_t hStart = t.hTotal - t.hActive - t.hFront, vStart = t.vTotal - t.vActive - t.vFront;
    m[0] = (static_cast<uint32_t>(t.hTotal) << 16) | t.vTotal;
    m[1] = (hStart << 16) | vStart;
    m[2] = (t.hNeg ? 0x80000000u : 0) | (static_cast<uint32_t>(t.hSync) << 16) | (t.vNeg ? 0x8000u : 0) | t.vSync;
    m[3] = (static_cast<uint32_t>(t.hActive) << 16) | t.vActive;
}
// DP_VID_M with DP_VID_N 0x8000: DCN32 counts two pixels per clock, so M = N x (pclk / 2) / link symbol clock, truncated
// (trace: 148.5 MHz at RBR -> 0x3aaa; the hardware then measures M itself, DP_VID_M_N_GEN_EN).
inline uint32_t vidM(uint32_t pclkKhz, uint8_t rate) {
    return static_cast<uint32_t>(static_cast<uint64_t>(kVidN) * (pclkKhz / 2u) / (symclk10k(rate) * 10u));
}
// b119: one pixel per clock (the GOP's convention on pipe 0: DP_VID_N_MUL 0, DTO0 = the full pixel clock): M = N x pclk /
// symclk (box: GOP DP1 at 144 HBR measured 0x9ab6 vs 0x9a64 computed; the hardware measures M itself).
inline uint32_t vidM1(uint32_t pclkKhz, uint8_t rate) {
    return static_cast<uint32_t>(static_cast<uint64_t>(kVidN) * pclkKhz / (symclk10k(rate) * 10u));
}
// The pixel clock of a DP pipe (DCCG, not this file's writes: the plan's): DP_DTOn at pclk / 2 over DPREFCLK.
constexpr uint32_t kDpRefClkHz = 716666000u;          // DP_DTO0_MODULO in the traces (0x2ab77490)
inline uint32_t dtoPhase(uint32_t pclkKhz) { return pclkKhz * 500u; }   // Hz / 2
constexpr uint32_t kOtgPixelRateCntlDto = 0x0000d010u;   // OTGn_PIXEL_RATE_CNTL: DP_DTO_ENABLE, DTO_SRC_SEL 1, errors cleared
inline constexpr uint32_t pixelRateDivMask(unsigned otg) { return 0x7u << (3 * otg); }
inline constexpr uint32_t pixelRateDivDp(unsigned otg) { return 0x2u << (3 * otg); }   // K1 /1, K2 /2 (trace OTG0: 0xdb2)
static_assert((0xdb6u & ~pixelRateDivMask(0)) == (0xdb2u & ~pixelRateDivMask(0)) && (0xdb2u & 7u) == pixelRateDivDp(0),
              "OTG_PIXEL_RATE_DIV: only OTG0's field changed in the trace");

// ---- the engine -------------------------------------------------------------------------------------------------------
struct Io {
    void *c;
    uint32_t (*rd)(void *c, uint32_t reg);
    void (*wr)(void *c, uint32_t reg, uint32_t v);
    int (*dmub)(void *c, const uint32_t cmd[16]);                            // 0 = consumed
    int (*auxRd)(void *c, uint32_t addr, uint8_t *buf, unsigned n);          // 0 = ACK with n bytes (defers retried below)
    int (*auxWr)(void *c, uint32_t addr, const uint8_t *buf, unsigned n);    // 0 = ACK
    void (*delayUs)(void *c, uint32_t us);
};
enum Rc : int { kOk = 0, kErrAux = 1, kErrDmub = 2, kErrCr = 3, kErrEq = 4, kErrCrLost = 5, kErrLinkLost = 6,
                kErrArg = 7, kErrFifo = 8, kErrStream = 9 };
inline const char *rcName(int rc) {
    static const char *const n[] = {"ok", "aux", "dmub", "clock-recovery", "equalisation", "clock-lost-in-eq",
                                    "link-lost-after", "arg", "dig-fifo-reset", "stream-status"};
    return rc >= 0 && rc <= kErrStream ? n[rc] : "?";
}
struct Opts {
    bool wakeRead = true;      // a 1-byte read of DPCD 0x000 before every other read (Linux drm_dp_dpcd_read probe; traces)
    bool panelModeRead = false;   // read DPCD 0x10A after SET_POWER (the modeset path does, detection does not)
    bool esiRead = false;      // read 0x2002 (14) after training (detection, DPCD 1.4 sinks)
    uint8_t attempts = 1;      // Linux 4; the box ladder 1 (stop at the first failure)
    uint16_t crMaxLoops = 100, crSameVsMax = 5;   // Linux limits (multiport 3.5)
    uint8_t eqMaxLoops = 6;
    bool onePpc = false;       // b119: the DIG/DP side in one pixel per clock (an OTG clocked the GOP's way, S2d); default =
                               // Linux's two pixels per clock (every golden of the host test)
};
inline uint32_t pixFmt(const Opts &o) { return o.onePpc ? kPixFmt8bpc1ppc : kPixFmt8bpc2ppc; }
inline uint32_t fifoBase(const Opts &o) { return o.onePpc ? kFifoBase1ppc : kFifoBase; }
inline uint32_t vidTimingNMul(const Opts &o) { return o.onePpc ? 0u : kVidTimingNMul; }
struct Report {
    int rc;
    uint8_t attempts, crLoops, eqLoops, vs, pe, pattern;
    uint8_t status[6];         // last 0x202..0x207 (training) or 0x200..0x205 (after)
    uint8_t after[6];          // 0x200..0x205 after training
    uint32_t delayUs, dmubSent, mmioWr, dpcdWr, dpcdRd;
};

struct Ctx { const Io &io; const Conn &k; const Opts &o; Report *r; };
inline void W(Ctx &x, uint32_t reg, uint32_t v) { x.io.wr(x.io.c, reg, v); if (x.r) x.r->mmioWr++; }
inline uint32_t R(Ctx &x, uint32_t reg) { return x.io.rd(x.io.c, reg); }
inline void D(Ctx &x, uint32_t us) { x.io.delayUs(x.io.c, us); if (x.r) x.r->delayUs += us; }
inline int dmub(Ctx &x, const uint32_t c[16]) {
    if (!txAllowed(c, x.k, nullptr)) return kErrArg;
    if (x.r) x.r->dmubSent++;
    return x.io.dmub(x.io.c, c) == 0 ? kOk : kErrDmub;
}
inline int dpRd(Ctx &x, uint32_t a, uint8_t *b, unsigned n) {
    uint8_t probe;
    if (x.o.wakeRead && a != 0) {
        if (x.r) x.r->dpcdRd++;
        if (x.io.auxRd(x.io.c, 0, &probe, 1)) return kErrAux;
    }
    if (x.r) x.r->dpcdRd++;
    return x.io.auxRd(x.io.c, a, b, n) ? kErrAux : kOk;
}
inline int dpWr(Ctx &x, uint32_t a, const uint8_t *b, unsigned n) {
    if (!dpcdWriteAllowed(a, n)) return kErrArg;
    if (x.r) x.r->dpcdWr++;
    return x.io.auxWr(x.io.c, a, b, n) ? kErrAux : kOk;
}
inline int dpWr1(Ctx &x, uint32_t a, uint8_t v) { return dpWr(x, a, &v, 1); }

// The DP block side of a training pattern (Linux dcn10_link_encoder_dp_set_phy_pattern, trace order).
inline void phyPattern(Ctx &x, uint32_t sel) {
    const Regs g = regs(x.k.dig);
    W(x, g.tpsSel, sel);
    W(x, g.linkCntl, 0);
    W(x, g.dphyCntl, 0);
    W(x, g.prbs, kTrainPrbs);
}
inline int drive(Ctx &x, Link l, uint8_t vs, uint8_t pe) {
    uint32_t c[16];
    txCmd(c, x.k, kTxVsemph, l.lanes, symclk10k(l.rate), static_cast<uint8_t>(vs | (pe << 3)));
    for (uint8_t i = 0; i < l.lanes; i++) {   // one per lane, as Linux; the struct has no lane field
        int rc = dmub(x, c);
        if (rc) return rc;
    }
    return kOk;
}
inline uint8_t laneSet(uint8_t vs, uint8_t pe) {   // DPCD TRAINING_LANEx_SET: VS, MAX_SWING_REACHED, PE, MAX_PE_REACHED
    return static_cast<uint8_t>(vs | (vs == 3 ? 4 : 0) | (pe << 3) | (pe == 3 ? 0x20 : 0));
}
inline bool laneBits(const uint8_t st[6], uint8_t lanes, uint8_t bits) {
    for (uint8_t i = 0; i < lanes; i++) if (((st[i / 2] >> (4 * (i & 1))) & bits) != bits) return false;
    return true;
}
inline void adjust(const uint8_t st[6], uint8_t lanes, uint8_t *vs, uint8_t *pe) {   // the highest any lane asks for
    uint8_t v = 0, p = 0;
    for (uint8_t i = 0; i < lanes; i++) {
        const uint8_t a = static_cast<uint8_t>(st[4 + i / 2] >> (4 * (i & 1)));
        if ((a & 3) > v) v = a & 3;
        if (((a >> 2) & 3) > p) p = (a >> 2) & 3;
    }
    if (v + p > 3) p = static_cast<uint8_t>(3 - v);
    *vs = v; *pe = p;
}

// Link up after the transmitter is enabled: SET_POWER D0 .. training .. pattern off .. PHY to video .. status check.
inline int trainOnce(Ctx &x, const Sink &s, Link l) {
    const Regs g = regs(x.k.dig);
    Report *r = x.r;
    int rc;
    uint8_t v, buf[16];
    if ((rc = dpWr1(x, 0x600, 1))) return rc;
    if (x.o.panelModeRead && (rc = dpRd(x, 0x10a, &v, 1))) return rc;
    uint8_t ival[2];
    for (int i = 0; i < 2; i++) if ((rc = dpRd(x, 0x00e, &ival[i], 1))) return rc;   // CR, then EQ interval (Linux reads twice)
    const uint32_t crUs = (ival[0] & 0x7f) ? (ival[0] & 0x7f) * 4000u : 100u;
    const uint32_t eqUs = (ival[1] & 0x7f) ? (ival[1] & 0x7f) * 4000u : 400u;
    if ((rc = dpWr1(x, 0x102, 0))) return rc;
    if ((rc = dpWr1(x, 0x108, 1))) return rc;                                     // 8b/10b
    if ((rc = dpWr1(x, 0x107, s.downspread ? 0x10 : 0x00))) return rc;
    if ((rc = dpWr1(x, 0x101, static_cast<uint8_t>(l.lanes | (s.enhanced ? 0x80 : 0))))) return rc;
    if ((rc = dpWr1(x, 0x100, l.rate))) return rc;
    // clock recovery on TPS1, scrambling off
    uint8_t vs = 0, pe = 0, lastVs = 0xff;
    unsigned same = 0;
    phyPattern(x, 0);
    for (unsigned loop = 0;; loop++) {
        if (r) r->crLoops = static_cast<uint8_t>(loop + 1);
        if ((rc = drive(x, l, vs, pe))) return rc;
        for (uint8_t i = 0; i < l.lanes; i++) buf[1 + i] = laneSet(vs, pe);
        if (loop == 0) { buf[0] = 0x21; rc = dpWr(x, 0x102, buf, 1u + l.lanes); }
        else rc = dpWr(x, 0x103, buf + 1, l.lanes);
        if (rc) return rc;
        D(x, crUs);
        uint8_t st[6];
        if ((rc = dpRd(x, 0x202, st, 6))) return rc;
        if (r) for (int i = 0; i < 6; i++) r->status[i] = st[i];
        if (laneBits(st, l.lanes, 0x1)) break;
        if (loop + 1 >= x.o.crMaxLoops) return kErrCr;
        uint8_t nv, np;
        adjust(st, l.lanes, &nv, &np);
        same = nv == lastVs ? same + 1 : 0;
        lastVs = nv;
        if (same >= x.o.crSameVsMax) return kErrCr;
        vs = nv; pe = np;
    }
    // channel equalisation: TPS4 / TPS3 / TPS2 by the sink's caps
    const uint32_t sel = s.tps4 ? 3 : s.tps3 ? 2 : 1;
    const uint8_t pat = s.tps4 ? 0x07 : s.tps3 ? 0x23 : 0x22;
    if (r) r->pattern = pat;
    phyPattern(x, sel);
    for (unsigned loop = 0;; loop++) {
        if (r) r->eqLoops = static_cast<uint8_t>(loop + 1);
        if ((rc = drive(x, l, vs, pe))) return rc;
        for (uint8_t i = 0; i < l.lanes; i++) buf[1 + i] = laneSet(vs, pe);
        if (loop == 0) { buf[0] = pat; rc = dpWr(x, 0x102, buf, 1u + l.lanes); }
        else rc = dpWr(x, 0x103, buf + 1, l.lanes);
        if (rc) return rc;
        D(x, eqUs);
        uint8_t st[6];
        if ((rc = dpRd(x, 0x202, st, 6))) return rc;
        if (r) for (int i = 0; i < 6; i++) r->status[i] = st[i];
        if (!laneBits(st, l.lanes, 0x1)) return kErrCrLost;
        if (laneBits(st, l.lanes, 0x7) && (st[2] & 1)) break;
        if (loop + 1 >= x.o.eqMaxLoops) return kErrEq;
        adjust(st, l.lanes, &vs, &pe);
    }
    if (r) { r->vs = vs; r->pe = pe; }
    // pattern off, then the PHY to video (Linux dp_set_hw_training_pattern(VIDEO_IDLE) + dcn10 passthrough)
    if ((rc = dpWr1(x, 0x102, 0))) return rc;
    W(x, g.internal, 0);
    W(x, g.framing, kFramingIdle | (s.enhanced ? kFramingEnhanced : 0));
    W(x, g.scram, kScram);
    W(x, g.linkCntl, kLinkTrained);
    W(x, g.dphyCntl, 0);
    W(x, g.prbs, kTrainPrbs);
    D(x, 5000);
    uint8_t a[6];
    if ((rc = dpRd(x, 0x200, a, 6))) return rc;
    if (r) for (int i = 0; i < 6; i++) r->after[i] = a[i];
    if (!laneBits(a + 2, l.lanes, 0x7) || !(a[4] & 1)) return kErrLinkLost;
    if (x.o.esiRead) {
        uint8_t esi[14];
        if ((rc = dpRd(x, 0x2002, esi, 14))) return rc;
    }
    return kOk;
}

inline void linkSetup(Ctx &x, Link l) {   // DP_UDI_LANES, scrambler (before the transmitter, trace)
    const Regs g = regs(x.k.dig);
    W(x, g.config, l.lanes == 4 ? 3u : l.lanes - 1u);
    W(x, g.scram, kScram);
}
inline int phyOn(Ctx &x, Link l) {
    uint32_t c[16];
    txCmd(c, x.k, kTxEnable, l.lanes, symclk10k(l.rate), 0);
    return dmub(x, c);
}
// Link off (Linux dp_disable_link_phy, trace order): D3, transmitter DISABLE, pattern/link/internal to 0. The DMUB step is
// skipped when `dmubOk` is false (a dead ring: nothing more may be sent).
inline int linkOff(Ctx &x, bool dmubOk = true) {
    const Regs g = regs(x.k.dig);
    int rc = dpWr1(x, 0x600, 2), rc2 = kOk;
    if (dmubOk) {
        uint32_t c[16];
        txCmd(c, x.k, kTxDisable, 0, 0, 0);
        rc2 = dmub(x, c);
    }
    W(x, g.tpsSel, 0);
    W(x, g.linkCntl, 0);
    W(x, g.internal, 0);
    return rc2 ? rc2 : rc;
}

// S2b: train only (Linux dp_verify_link_cap): D3, link setup, ENABLE, training, link off again whatever the result.
inline int verifyLink(const Io &io, const Conn &k, const Sink &s, Link l, const Opts &o, Report *r) {
    Report dummy{};
    if (!r) r = &dummy;
    *r = Report{};
    Ctx x{io, k, o, r};
    if (l.rate > s.maxRate || l.lanes > s.maxLanes || !(l.lanes == 1 || l.lanes == 2 || l.lanes == 4)) return r->rc = kErrArg;
    int rc = dpWr1(x, 0x600, 2);
    if (rc) return r->rc = rc;
    int last = kOk;
    bool dmubOk = true;
    for (uint8_t a = 0; a < (o.attempts ? o.attempts : 1); a++) {
        r->attempts = static_cast<uint8_t>(a + 1);
        if (a) { D(x, 10000u * a); }
        linkSetup(x, l);
        if ((last = phyOn(x, l)) == kOk) last = trainOnce(x, s, l);
        if (last == kErrDmub) dmubOk = false;
        if (last == kOk || last == kErrDmub || last == kErrArg) break;
        if (a + 1 < o.attempts) { linkOff(x, dmubOk); }
    }
    const int off = linkOff(x, dmubOk);
    return r->rc = last ? last : off;
}

// S2c, DIG/DP part before training (Linux setup_dio_stream_attribute + link encoder setup + FIFO, trace order). otg =
// the OTG that feeds this DIG FE; t = its timing.
inline void streamPre(Ctx &x, unsigned otg, const Timing &t) {
    const Regs g = regs(x.k.dig);
    uint32_t m[4];
    msa(t, m);
    W(x, g.feCntl, feCntl(otg, false));
    W(x, g.beCntl, beCntl(x.k, false));
    W(x, g.feCntl, feCntl(otg, true));
    W(x, g.feCntl, feCntl(otg, true));
    W(x, g.pixFmt, pixFmt(x.o));
    W(x, g.msaColor, kMsaMisc0Rgb8);
    W(x, g.msaMisc, 0);
    W(x, g.msa1, m[0]); W(x, g.msa2, m[1]); W(x, g.msa3, m[2]); W(x, g.msa4, m[3]);
    W(x, g.secFraming4, kSecFraming4);
    for (int i = 0; i < 4; i++) W(x, g.secCntl, 0);   // SDP / audio off (Linux: four field updates)
    W(x, g.beCntl, beCntl(x.k, true));
    const uint32_t fb = fifoBase(x.o);
    W(x, g.fifo, fb);
    W(x, g.fifo, fb | kFifoReset);
    W(x, g.fifo, fb);
    W(x, g.fifo, fb | kFifoEnable);
}
inline int fifoResetPulse(Ctx &x) {   // Linux enc32_reset_fifo: RESET 1, wait DONE 1, RESET 0, wait DONE 0
    const Regs g = regs(x.k.dig);
    const uint32_t fb = fifoBase(x.o);
    W(x, g.fifo, fb | kFifoEnable | kFifoReset);
    unsigned i = 0;
    for (; i < 100 && !(R(x, g.fifo) & kFifoResetDone); i++) D(x, 10);
    if (i == 100) return kErrFifo;
    W(x, g.fifo, fb | kFifoEnable | kFifoResetDone);
    for (i = 0; i < 100 && (R(x, g.fifo) & kFifoResetDone); i++) D(x, 10);
    if (i == 100) return kErrFifo;
    W(x, g.fifo, fb | kFifoEnable);
    return kOk;
}
// S2c, after training: re-setup (Linux's "workaround" in link_set_dpms_on), FIFO pulse, M/N, steer FIFO, stream on.
inline int streamPost(Ctx &x, const Timing &t, Link l) {
    const Regs g = regs(x.k.dig);
    W(x, g.beCntl, beCntl(x.k, false));
    W(x, g.beCntl, beCntl(x.k, true));
    const uint32_t fb = fifoBase(x.o);
    W(x, g.fifo, fb | kFifoEnable);
    int rc = fifoResetPulse(x);
    if (rc) return rc;
    for (int i = 0; i < 4; i++) W(x, g.secCntl, 0);
    W(x, g.fifo, fb | kFifoEnable);
    W(x, g.pixFmt, pixFmt(x.o));
    W(x, g.vidTiming, vidTimingNMul(x.o));
    W(x, g.vidN, kVidN);
    W(x, g.vidM, x.o.onePpc ? vidM1(t.pclkKhz, l.rate) : vidM(t.pclkKhz, l.rate));
    W(x, g.vidTiming, vidTimingNMul(x.o) | kVidTimingGen);
    W(x, g.pixFmt, pixFmt(x.o));
    W(x, g.vidStream, kStreamKeepout);
    W(x, g.steer, kSteerTu | kSteerReset);
    W(x, g.steer, kSteerTu);
    W(x, g.fifo, fb | kFifoEnable);
    if ((rc = fifoResetPulse(x))) return rc;
    W(x, g.vidStream, kStreamKeepout | kStreamEnable);
    return kOk;
}
// The DIG/DP part of a DP stream on `otg`: pre, link setup, ENABLE, training, post. On a training failure the link is
// turned off again (the caller's undo restores the registers).
inline int enableStream(const Io &io, const Conn &k, const Sink &s, Link l, unsigned otg, const Timing &t, const Opts &o,
                        Report *r) {
    Report dummy{};
    if (!r) r = &dummy;
    *r = Report{};
    Ctx x{io, k, o, r};
    if (otg > 3 || l.rate > s.maxRate || l.lanes > s.maxLanes) return r->rc = kErrArg;
    Link need;
    if (!pickLink(s, t.pclkKhz, &need) || linkKbps(l) < static_cast<uint64_t>(t.pclkKhz) * 24u) return r->rc = kErrArg;
    streamPre(x, otg, t);
    linkSetup(x, l);
    int rc = phyOn(x, l);
    r->attempts = 1;
    if (rc == kOk) rc = trainOnce(x, s, l);
    if (rc) { linkOff(x, rc != kErrDmub); return r->rc = rc; }
    return r->rc = streamPost(x, t, l);
}
// Stream off + link off (Linux enc1_stream_encoder_dp_blank, then dp_disable_link_phy): DIS_DEFER 2, stream disable, wait
// for the status, steer FIFO reset, link off, FE source off the BE, FIFO off.
inline int disableStream(const Io &io, const Conn &k, const Opts &o, Report *r, bool dmubOk = true) {
    Report dummy{};
    if (!r) r = &dummy;
    Ctx x{io, k, o, r};
    const Regs g = regs(k.dig);
    W(x, g.vidStream, kStreamKeepout | kStreamDisDefer2 | kStreamEnable);
    W(x, g.vidStream, kStreamKeepout | kStreamDisDefer2);
    unsigned i = 0;
    for (; i < 500 && (R(x, g.vidStream) & kStreamStatus); i++) D(x, 10);
    W(x, g.steer, kSteerTu | kSteerReset);
    const int off = linkOff(x, dmubOk);
    W(x, g.beCntl, beCntl(k, false));
    W(x, g.fifo, fifoBase(o));
    return i == 500 ? kErrStream : off;
}

// ---- survey: refusals decided before any write ------------------------------------------------------------------------
enum Ref : int { kRefNone = 0, kRefConn = 1, kRefNoSink = 2, kRefDigBusy = 3, kRefCaps = 4, kRefLink = 5, kRefC3 = 6 };
inline const char *refName(int f) {
    static const char *const n[] = {"none", "not a DP connector this file drives (DP A / DP E only)", "no sink (HPD low)",
                                    "DIG or DP block already in use", "DPCD caps not a sink's", "no link carries the mode",
                                    "PHY SRAM (C3) not loaded in this boot"};
    return f >= 0 && f <= kRefC3 ? n[f] : "?";
}
struct SurveyIn { bool hpdSensed; bool c3Done; const uint8_t *caps; uint32_t pclkKhz; };
// Reads only: the DIG BE enable, the DP link and stream state of the connector's blocks.
inline int survey(const Io &io, unsigned dig, const SurveyIn &in, Sink *s, Link *l) {
    const Conn *k = connByDig(dig);
    if (!k) return kRefConn;
    if (!in.hpdSensed) return kRefNoSink;
    const Regs g = regs(dig);
    if ((io.rd(io.c, g.beEn) & 1u) || (io.rd(io.c, g.linkCntl) & kLinkTrained) || (io.rd(io.c, g.vidStream) & kStreamEnable))
        return kRefDigBusy;
    if (!in.caps || !parseCaps(in.caps, s)) return kRefCaps;
    if (!pickLink(*s, in.pclkKhz, l)) return kRefLink;
    if (!in.c3Done) return kRefC3;
    return kRefNone;
}

// ---- Gate: an Io in front of the caller's that drops (and counts) every write off this connector's lists ---------------
struct Gate {
    const Io *inner; const Conn *k;
    uint32_t strayWr, strayDmub, strayDpcd;
};
inline uint32_t gRd(void *c, uint32_t r) { Gate *g = static_cast<Gate *>(c); return g->inner->rd(g->inner->c, r); }
inline void gWr(void *c, uint32_t r, uint32_t v) {
    Gate *g = static_cast<Gate *>(c);
    if (!writeAllowed(*g->k, r)) { g->strayWr++; return; }
    g->inner->wr(g->inner->c, r, v);
}
inline int gDmub(void *c, const uint32_t cmd[16]) {
    Gate *g = static_cast<Gate *>(c);
    if (!txAllowed(cmd, *g->k, nullptr)) { g->strayDmub++; return -1; }
    return g->inner->dmub(g->inner->c, cmd);
}
inline int gAuxRd(void *c, uint32_t a, uint8_t *b, unsigned n) { Gate *g = static_cast<Gate *>(c); return g->inner->auxRd(g->inner->c, a, b, n); }
inline int gAuxWr(void *c, uint32_t a, const uint8_t *b, unsigned n) {
    Gate *g = static_cast<Gate *>(c);
    if (!dpcdWriteAllowed(a, n)) { g->strayDpcd++; return -1; }
    return g->inner->auxWr(g->inner->c, a, b, n);
}
inline void gDelay(void *c, uint32_t us) { Gate *g = static_cast<Gate *>(c); g->inner->delayUs(g->inner->c, us); }
inline Io gated(Gate *g) { return Io{g, gRd, gWr, gDmub, gAuxRd, gAuxWr, gDelay}; }

}  // namespace phxdpl
