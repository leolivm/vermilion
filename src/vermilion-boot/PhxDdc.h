// SPDX-License-Identifier: MIT
// PhxDdc.h: the display side channels (DC_I2C DDC, DP AUX) of PhxBringup
//
// Reads a monitor's EDID on Navi31 (DCN 3.2.0) through either display side channel, with no kernel dependencies so the
// same header builds in PhxBringup and in a host test:
//   - DC_I2C, the hardware I2C engine for HDMI/DVI DDC lines. Sequence = what Linux amdgpu (dce_i2c_hw.c) wrote on this
//     card in a register trace (DDC1 block reads around t=2332.155-2332.168), value for value.
//   - DP AUX software engine n (I2C-over-AUX to 0x50, and native DPCD reads), after Linux dce_aux.c
//     (acquire_engine / submit_channel_request / get_channel_status / read_channel_reply), the RedNavi auxTransfer
//     (BSD-3, RedNavi/kern_probe.cpp) and RX4Darwin auxTransaction (BSD-3, RX4Darwin/src/device.cpp)
//     for the defer retries and the DMCU-owner check. Per EDID block: I2C write of the offset with MOT, 8 reads of 16
//     bytes with MOT, then a zero-length read without MOT (the I2C stop), as Linux does in the same trace (AUX2).
// SCDC1: the DC_I2C path is a generic transfer (i2cXfer, allow-list EDID 0x50 / SCDC 0x54 / DDC/CI 0x37) with the SCDC
// helpers of Linux link_ddc.c on top (write_scdc_data, the status reads); ddcRead is its EDID case.
//
// Register offsets: absolute BAR5 dwords = DCN BASE_IDX 2 segment 0x34C0 (the card's IP discovery table) + the offsets of
// Linux's dcn_3_2_0_offset.h. dcn_3_2_1_offset.h (Navi32, RedNavi's card) has the same values for every register
// used here. Field shifts from dcn_3_2_0_sh_mask.h.
//
// Side effects, all undone before returning: the SW arbitration request of the engine (released even on error), the
// DDCn_SETUP value (restored), AUX_EN (set only if it was clear, as Linux does; left set). Not undone (b115, as Linux):
// the DDC pad of a DP connector's AUX engine is put in AUX mode and left there (auxPadSetup). No display, link, clock or
// OTG register is written. An SCDC or DDC/CI write changes the monitor's own state, which is the caller's to undo.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace phxddc {

constexpr uint32_t kSeg2 = 0x34C0;
// DC_I2C (BASE_IDX 2)
constexpr uint32_t kI2cControl = kSeg2 + 0x1e98;   // GO 0, SOFT_RESET 1, SW_STATUS_RESET 3, DDC_SELECT 10:8, TRANSACTION_COUNT 21:20
constexpr uint32_t kI2cArb = kSeg2 + 0x1e99;       // REG_RW_CNTL_STATUS 3:2 (2 = used by HW), SW_USE_I2C_REG_REQ 20, SW_DONE_USING_I2C_REG 21
constexpr uint32_t kI2cSwStatus = kSeg2 + 0x1e9b;  // SW_STATUS 1:0 (0 idle), DONE 2, ABORTED 4, TIMEOUT 5, INTERRUPTED 6, OVERFLOW 7, STOPPED_ON_NACK 8
constexpr uint32_t kI2cSpeed0 = kSeg2 + 0x1ea2;    // DDC1_SPEED; DDCn at +2*(n-1), n = 1..5 on DCN 3.2
constexpr uint32_t kI2cSetup0 = kSeg2 + 0x1ea3;    // DDC1_SETUP; ENABLE 6, TIME_LIMIT 31:24
constexpr uint32_t kI2cTxn0 = kSeg2 + 0x1eae;      // RW 0, STOP_ON_NACK 8, START 12, STOP 13, COUNT 23:16
constexpr uint32_t kI2cTxn1 = kSeg2 + 0x1eaf;
constexpr uint32_t kI2cData = kSeg2 + 0x1eb2;      // DATA_RW 0, DATA 15:8, INDEX 25:16 (approx.), INDEX_WRITE 31
// DP AUX engine n: CONTROL +0, SW_CONTROL +1, ARB_CONTROL +2, INTERRUPT_CONTROL +3, SW_STATUS +4, SW_DATA +6
constexpr uint32_t kAux0 = kSeg2 + 0x1f50, kAuxStride = 0x1c;
// HPD: HPDn_DC_HPD_INT_STATUS (bit 1 SENSE), DC_GPIO_HPD_Y (raw pins)
constexpr uint32_t kHpd0IntStatus = kSeg2 + 0x1f14, kHpdStride = 8;
constexpr uint32_t kGpioHpdY = kSeg2 + 0x28f7;

// The values Linux used on this card (regtrace): DDC1_SETUP after setup_engine, DDC1_SPEED as found (not written by us).
constexpr uint32_t kLinuxDdcSetup = 0x03000040;

struct Io {
    void *ctx;
    uint32_t (*rd)(void *ctx, uint32_t dword);
    void (*wr)(void *ctx, uint32_t dword, uint32_t value);
    void (*delayUs)(void *ctx, uint32_t us);
};

enum : int {
    kOk = 0,
    kErrArg = -1,
    kErrBusy = -2,        // engine not idle before we asked for it
    kErrNotGranted = -3,  // arbitration: the hardware/firmware side owns it
    kErrTimeout = -4,     // no DONE, or AUX RX timeout
    kErrNack = -5,        // I2C NACK / aborted / AUX NACK
    kErrDefer = -6,       // the sink kept deferring
    kErrHpd = -7,         // AUX: HPD low during the transaction
    kErrHeader = -8,      // EDID header wrong
    kErrChecksum = -9,    // EDID block checksum wrong
};

struct Diag {
    uint32_t status;   // last SW_STATUS
    uint32_t arb;      // ARB value after the request
    uint32_t retries;  // AUX defers retried
    uint32_t step;     // block / chunk where it stopped
};

inline uint32_t rd(const Io &io, uint32_t r) { return io.rd(io.ctx, r); }
inline void wr(const Io &io, uint32_t r, uint32_t v) { io.wr(io.ctx, r, v); }
inline void delay(const Io &io, uint32_t us) { io.delayUs(io.ctx, us); }

inline bool hpdSensed(const Io &io, unsigned n) { return n < 5 && (rd(io, kHpd0IntStatus + kHpdStride * n) & 2u); }

// ---- DC_I2C ---------------------------------------------------------------------------------------------------------
// SCDC1: the generic transfer, with
// a hard allow-list of 7-bit addresses: 0x50 EDID, 0x54 SCDC (HDMI 2.0 status and control), 0x37 DDC/CI. Any other
// address is refused before the first register access.
constexpr uint8_t kAddrEdid = 0x50, kAddrScdc = 0x54, kAddrDdcCi = 0x37;
constexpr int kErrRefused = -10;   // the 7-bit address is not on the allow-list (nothing touched)
// Bytes of one call in the engine's data buffer (the two address bytes + wn + rn): at most what ddcRead puts there for an
// EDID block (3 + 128), which runs on every boot. The hardware buffer size itself was not checked [I].
constexpr unsigned kI2cMaxBytes = 131;
inline bool i2cAddrAllowed(uint8_t a7) { return a7 == kAddrEdid || a7 == kAddrScdc || a7 == kAddrDdcCi; }

// One call on DDC line `line` (0 = DDC1): wn bytes written to addr7 then, if rn, rn bytes read back, as one engine run.
// wn and rn both set = two transactions (the write without STOP, then a repeated START for the read), as ddcRead and
// Linux link_query_ddc_data; wn only = one write transaction with STOP (Linux's SCDC writes, its plug trace DC_I2C
// 216.711833: TRANSACTION0 0x00023100, CONTROL 0 then 1); rn only = one read transaction.
inline int i2cXfer(const Io &io, unsigned line, uint8_t addr7, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn, Diag *d) {
    if (line > 4 || (wn && !w) || (rn && !r) || (!wn && !rn) || rn > 128 || wn + rn + 2 > kI2cMaxBytes) return kErrArg;   // DDC1..DDC5
    if (!i2cAddrAllowed(addr7)) return kErrRefused;
    const uint32_t setupReg = kI2cSetup0 + 2 * line;
    const uint32_t a8 = static_cast<uint32_t>(addr7) << 1;
    const uint32_t sel = ((wn && rn) ? (1u << 20) : 0u) | (line << 8);   // TRANSACTION_COUNT = transactions - 1, DDC_SELECT
    if (rd(io, kI2cSwStatus) & 3u) return kErrBusy;  // dce_i2c_hw is_hw_busy
    wr(io, kI2cArb, rd(io, kI2cArb) | (1u << 20));
    const uint32_t arb = rd(io, kI2cArb);
    if (d) d->arb = arb;
    auto release = [&]() { wr(io, kI2cArb, (rd(io, kI2cArb) & ~(1u << 20)) | (1u << 21)); };
    if (((arb >> 2) & 3u) == 2u) { release(); return kErrNotGranted; }
    const uint32_t setupSaved = rd(io, setupReg);
    wr(io, kI2cControl, 0x8);                       // SW_STATUS_RESET
    wr(io, setupReg, kLinuxDdcSetup);
    if (wn) {
        wr(io, kI2cTxn0, (wn << 16) | (rn ? 0x1100u : 0x3100u));   // write, START, STOP_ON_NACK (+ STOP when last), wn bytes
        wr(io, kI2cData, 0x80000000u | (a8 << 8));                  // INDEX_WRITE, index 0: addr << 1 (write)
        for (unsigned i = 0; i < wn; i++) wr(io, kI2cData, static_cast<uint32_t>(w[i]) << 8);
    }
    if (rn) {
        wr(io, wn ? kI2cTxn1 : kI2cTxn0, (rn << 16) | 0x3101);    // read, START, STOP, STOP_ON_NACK, rn bytes
        wr(io, kI2cData, (wn ? 0u : 0x80000000u) | ((a8 | 1u) << 8));   // addr << 1 | 1 (read)
    }
    wr(io, kI2cControl, 0x8);
    wr(io, kI2cControl, sel);
    wr(io, kI2cControl, sel | 1u);                  // GO
    uint32_t st = 0;
    bool done = false;
    for (unsigned i = 0; i < 2500 && !done; i++) {  // ~50 ms; 128 bytes at 100 kHz take ~12 ms
        st = rd(io, kI2cSwStatus);
        done = (st & 4u) != 0;
        if (!done) delay(io, 20);
    }
    if (d) d->status = st;
    int rc = !done ? kErrTimeout : (st & ((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8))) ? kErrNack : kOk;
    if (rc == kOk && rn) {
        const uint32_t at = wn ? wn + 2 : 1;       // the read data follows every byte sent (the addresses + wn)
        wr(io, kI2cData, 0x80000000u | (at << 16) | 1u);   // INDEX_WRITE, index at, DATA_RW = read
        for (unsigned i = 0; i < rn; i++) r[i] = static_cast<uint8_t>(rd(io, kI2cData) >> 8);
    }
    wr(io, kI2cControl, sel | 0x8);                 // as Linux after the transfer: 0x00100008 (two transactions), 0x8 (one)
    wr(io, setupReg, setupSaved);
    release();
    return rc;
}

// One "write offset, then read n bytes" pair of transactions to I2C address 0x50 on DDC line `line` (0 = DDC1). Since
// SCDC1 a wrapper of i2cXfer: the same register accesses in the same order as the b79-b106 code (host-tested).
inline int ddcRead(const Io &io, unsigned line, uint8_t offset, uint8_t *buf, unsigned n, Diag *d) {
    if (line > 4 || !buf || !n || n > 128) return kErrArg;   // DDC1..DDC5
    return i2cXfer(io, line, kAddrEdid, &offset, 1, buf, n, d);
}

// ---- SCDC (HDMI 2.0 status and control data channel, I2C 0x54) -----------------------------------------------------
// Offsets (HDMI 2.0 SCDC map; Linux dc_link_ddc.h HDMI_SCDC_*).
constexpr uint8_t kScdcSinkVersion = 0x01, kScdcSourceVersion = 0x02, kScdcTmdsConfig = 0x20, kScdcScramblerStatus = 0x21,
                  kScdcStatusFlags0 = 0x40, kScdcErrDet = 0x50;
inline int scdcRead(const Io &io, unsigned line, uint8_t offset, uint8_t *v, Diag *d) {
    return i2cXfer(io, line, kAddrScdc, &offset, 1, v, 1, d);
}
inline int scdcWrite(const Io &io, unsigned line, uint8_t offset, uint8_t value, Diag *d) {
    const uint8_t b[2] = {offset, value};
    return i2cXfer(io, line, kAddrScdc, b, 2, nullptr, 0, d);
}
// TMDS_Config as Linux link_ddc.c write_scdc_data picks it: 3 (bit 0 scrambling enable, bit 1 TMDS bit clock ratio
// 1/40) above 340 MHz, 1 for the sink's LTE_340Mcsc_scramble (HF-VSDB; the Acer's is 0), else 0.
constexpr uint8_t scdcTmdsConfigFor(bool over340, bool lte340Scramble) { return over340 ? 3 : lte340Scramble ? 1 : 0; }
// write_scdc_data: read Sink_Version; when it is 1, write Source_Version = 1; then write TMDS_Config. As Linux, a failed
// Sink_Version read only skips the Source_Version write (sink version 0); the result is the TMDS_Config write's.
// The Acer at 144 (Linux plug trace 216.711348-216.712497): Sink_Version 1, Source_Version 1 written, TMDS_Config 0 written.
inline int scdcWriteTmdsConfig(const Io &io, unsigned line, uint8_t cfg, Diag *d, uint8_t *sinkVersion) {
    uint8_t sv = 0;
    if (scdcRead(io, line, kScdcSinkVersion, &sv, d) != kOk) sv = 0;
    if (sinkVersion) *sinkVersion = sv;
    if (sv == 1) scdcWrite(io, line, kScdcSourceVersion, 1, d);
    return scdcWrite(io, line, kScdcTmdsConfig, cfg, d);
}
inline int scdcSetTmdsConfig(const Io &io, unsigned line, bool over340, Diag *d, uint8_t *sinkVersion) {
    return scdcWriteTmdsConfig(io, line, scdcTmdsConfigFor(over340, false), d, sinkVersion);
}
// The sink's view of the link (reads only): Sink_Version, TMDS_Config, Scrambler_Status (bit 0 = scrambled data seen),
// Status_Flags_0 (bit 0 Clock_Detected, bits 1-3 Ch0-2_Locked) and the character error counters 0x50-0x55 (per channel
// lo, hi; hi bit 7 = valid). Stops at the first failed read (d->step = its offset).
struct ScdcStatus { uint8_t sinkVersion, tmdsConfig, scrambler, flags0, err[6]; };
inline int scdcReadStatus(const Io &io, unsigned line, ScdcStatus *s, Diag *d) {
    if (!s) return kErrArg;
    *s = ScdcStatus{};
    const struct { uint8_t off; uint8_t *dst; unsigned n; } rows[] = {
        {kScdcSinkVersion, &s->sinkVersion, 1}, {kScdcTmdsConfig, &s->tmdsConfig, 1}, {kScdcScramblerStatus, &s->scrambler, 1},
        {kScdcStatusFlags0, &s->flags0, 1}, {kScdcErrDet, s->err, 6}};
    for (const auto &x : rows) {
        if (d) d->step = x.off;
        const int rc = i2cXfer(io, line, kAddrScdc, &x.off, 1, x.dst, x.n, d);
        if (rc != kOk) return rc;
    }
    return kOk;
}
inline bool scdcLocked(const ScdcStatus &s) { return (s.flags0 & 0xFu) == 0xFu; }   // clock detected + 3 channels locked
// Channel ch's character error count, -1 = not valid.
inline int scdcErrCount(const ScdcStatus &s, unsigned ch) {
    if (ch > 2 || !(s.err[2 * ch + 1] & 0x80u)) return -1;
    return ((s.err[2 * ch + 1] & 0x7f) << 8) | s.err[2 * ch];
}

// ---- Board connectors -----------------------------------------------------------------------------------------------
// The board's display connectors, one row per VBIOS display path. Filled today from this card's display object table
// (parsed by RedNavi parse_vbios_objects.py; HPD = pin - 1,
// line id low bits = the AUX engine / DDC line): path 0 DP HPD3/AUX3, path 1 DP HPD4/AUX4, path 2 DP HPD2/AUX2,
// path 3 HDMI HPD0/DDC1. boardConnectors() is the one accessor: the kext will later fill the table from the VBIOS it
// parses (other boards); everything below only walks what it returns.
enum : uint8_t { kConnDp = 0, kConnHdmi = 1 };
struct Connector {
    uint8_t path, type, hpd, line;   // line = the AUX engine (DP) or the DC_I2C DDC line (HDMI, 0 = DDC1)
};
constexpr uint32_t connSource(const Connector &c) { return c.type == kConnDp ? 0x20u + c.line : 0x10u + c.line; }
inline const Connector *boardConnectors(unsigned *n) {
    static const Connector kNavi31Board[] = {
        {0, kConnDp, 3, 3}, {1, kConnDp, 4, 4}, {2, kConnDp, 2, 2}, {3, kConnHdmi, 0, 0},
    };
    if (n) *n = sizeof kNavi31Board / sizeof kNavi31Board[0];
    return kNavi31Board;
}

// ---- DP AUX pad (b115) ----------------------------------------------------------------------------------------------
// A DDC pad carries either the DC_I2C/GPIO lines (I2C mode) or the DP AUX pair (AUX mode). AUX engine n drives DDC pad
// n + 1 (Linux v6.8 hw_factory_dcn32.c define_ddc_registers: ddc_data_regs_dcn[en] = DDC<en+1>; this board's VBIOS:
// DP E line 0x94 -> DC_GPIO_DDC5_A). On a connector the GOP did not light the pad
// is in I2C mode and the sink never hears the request: SW_STATUS 0xa1 (RX timeout), seen on the box. Linux
// puts the pad in AUX mode in every AUX acquire (dce_aux.c acquire -> dal_ddc_open(HW, MODE_AUX) -> hw_ddc.c set_config
// MODE_AUX): DC_GPIO_DDC<n+1>_MASK.AUX_PAD<n+1>_MODE (bit 16) = 1 if it is 0 (whole-register RMW), then
// DC_GPIO_AUX_CTRL_5.DDC_PAD<n+1>_I2CMODE (bit 12 + n) = 0 (RMW, every other bit kept); Linux DP E trace 240372 / 240374.
// Box proof: those two RMWs alone make native AUX answer on DP E (AUX4: 0x5DA0 0xcf400000 -> 0xcf410000, 0x5DDD
// 0x1f000 -> 0xf000) and DP A (AUX3: 0x5D9C, 0x1f000 -> 0x17000), both passed.
//
// What auxPadSetup does (before the engine is requested, as Linux opens the DDC before acquire_engine):
//   - only on an engine that is the AUX channel of a DP connector of this board (boardConnectors), never engine 0
//     (DDC1 = the HDMI connector's I2C DDC pad), never a pad a non-DP connector uses for I2C (its DDC line = n);
//   - reads DDC<n+1>_MASK; AUX_PAD<n+1>_MODE already 1 = "already aux": nothing is written;
//   - else writes MASK | bit 16, reads AUX_CTRL_5 and, only if its own PAD<n+1>_I2CMODE bit is 1, writes it with that
//     one bit cleared. No other register, no other bit, no DDC1 / AUX_CTRL_5 bit of another pad.
// One deviation from Linux, on evidence: Linux clears I2CMODE even when AUX_PAD_MODE is already 1. Here a pad already
// in AUX mode is left as found, because the GOP-lit DP B runs AUX2 with AUX_PAD3_MODE 1 and PAD3_I2CMODE 1
// (0x5D98 0xcf410000, 0x5DDD 0x1f000, read on the box) and its AUX
// traffic works (the pipe-0 EDID every boot, debug.phxdpcd=18): the lit AUX2 path gets no new write.
// Whether PAD_I2CMODE matters once AUX_PAD_MODE is 1 was not tested [I].
// The pad is left in AUX mode afterwards, as Linux (dce_aux.c release_engine -> dal_ddc_close restores only the per-pin
// MASK/A/EN fields, never AUX_PAD_MODE or PAD_I2CMODE; the Linux DP E trace keeps 0xcf411010 / 0x3000 across every transaction):
// restoring would flip the pad mode around every transaction for no user, since nothing here uses I2C on a DP pad
// (HDMI = DDC1; detection = HPD sense). Consequence: a later DC_I2C read on DDC n+1 (debug.phxedid_src 0x11 + n, a
// manual test only) would need the pad back in I2C mode, which i2cXfer does not do (Linux's dce_i2c_hw acquire does).
constexpr uint32_t kDdc1Mask = kSeg2 + 0x28d0;    // DC_GPIO_DDC1_MASK (0x5D90); DDCk_MASK at +4 * (k - 1)
constexpr uint32_t kAuxCtrl5 = kSeg2 + 0x291d;    // DC_GPIO_AUX_CTRL_5 (0x5DDD); DDC_PADk_I2CMODE = bit 11 + k
constexpr uint32_t kAuxPadMode = 1u << 16;        // DC_GPIO_DDCk_MASK.AUX_PADk_MODE
constexpr uint32_t padMaskReg(unsigned eng) { return kDdc1Mask + 4u * eng; }    // AUX n -> DDC n+1 MASK
constexpr uint32_t padI2cBit(unsigned eng) { return 1u << (12u + eng); }        // AUX n -> DDC_PAD<n+1>_I2CMODE
enum : uint32_t { kPadNone = 0, kPadAux = 1, kPadSetMode = 2, kPadClearI2c = 4 };   // auxPadSetup result bits
// Engine eng may have its pad put in AUX mode on the board table t: a DP connector uses AUX eng, no other connector
// uses DDC line eng (= the same pad) for I2C, and eng is 1-4 (engine 0's pad is DDC1).
inline bool auxPadOwnedIn(unsigned eng, const Connector *t, unsigned n) {
    if (eng == 0 || eng > 4 || !t) return false;
    bool dp = false;
    for (unsigned i = 0; i < n; i++) {
        if (t[i].line != eng) continue;
        if (t[i].type == kConnDp) dp = true;
        else return false;
    }
    return dp;
}
inline bool auxPadOwned(unsigned eng) {
    unsigned n = 0;
    const Connector *t = boardConnectors(&n);
    return auxPadOwnedIn(eng, t, n);
}
// Returns kPadNone (not a DP pad of this board, or the mask register unreadable: nothing written), kPadAux (already in
// AUX mode: nothing written) or kPadAux | kPadSetMode [| kPadClearI2c] (the writes made).
inline uint32_t auxPadSetup(const Io &io, unsigned eng) {
    if (!auxPadOwned(eng)) return kPadNone;
    const uint32_t mr = padMaskReg(eng), m = rd(io, mr);
    if (m == 0xFFFFFFFFu) return kPadNone;
    if (m & kAuxPadMode) return kPadAux;
    wr(io, mr, m | kAuxPadMode);
    uint32_t r = kPadAux | kPadSetMode;
    const uint32_t c = rd(io, kAuxCtrl5);
    if (c != 0xFFFFFFFFu && (c & padI2cBit(eng))) { wr(io, kAuxCtrl5, c & ~padI2cBit(eng)); r |= kPadClearI2c; }
    return r;
}

// ---- DP AUX ---------------------------------------------------------------------------------------------------------
// action already in the command nibble: DP read 0x90, DP write 0x80, I2C write+MOT 0x40, I2C read+MOT 0x50, I2C read 0x10.
// Returns the reply nibble (0 ACK, 1 NACK, 2 AUX DEFER, 4 I2C NACK, 8 I2C DEFER) or a negative error. *got = data bytes.
inline int auxOnce(const Io &io, unsigned eng, uint8_t action, uint32_t addr, const uint8_t *wdata, uint8_t len,
                   uint8_t *rdata, uint8_t cap, uint8_t *got, Diag *d) {
    if (got) *got = 0;
    if (eng > 4 || len > 16) return kErrArg;                  // DP_AUX0..4
    const uint32_t b = kAux0 + kAuxStride * eng, CTL = b, SWC = b + 1, ARB = b + 2, INTC = b + 3, STAT = b + 4, DATA = b + 6;
    const bool isWrite = (action & 0x10) == 0;
    uint32_t arb = rd(io, ARB);
    if (arb == 0xFFFFFFFFu) return kErrArg;
    if (((arb >> 2) & 3u) == 2u) { if (d) d->arb = arb; return kErrNotGranted; }   // DMCU owns it
    auxPadSetup(io, eng);                                                            // b115: the pad in AUX mode
    if (!(rd(io, CTL) & 1u)) wr(io, CTL, rd(io, CTL) | 1u);                         // AUX_EN
    wr(io, ARB, rd(io, ARB) | (1u << 16));                                           // SW_USE_AUX_REG_REQ
    arb = rd(io, ARB);
    if (d) d->arb = arb;
    auto release = [&]() { wr(io, ARB, (rd(io, ARB) & ~(1u << 16)) | (1u << 17)); };
    if (((arb >> 2) & 3u) != 1u) { release(); return kErrNotGranted; }
    wr(io, INTC, rd(io, INTC) | (1u << 1));                                          // SW_DONE_ACK
    for (int i = 0; i < 100 && (rd(io, STAT) & 1u); i++) delay(io, 10);
    const uint32_t bytes = (len ? 4u : 3u) + (isWrite ? len : 0u);
    wr(io, SWC, (rd(io, SWC) & ~((0xFu << 4) | (0x1Fu << 16))) | (bytes << 16));
    wr(io, DATA, 0x80000000u | (static_cast<uint32_t>(action | ((addr >> 16) & 0xF)) << 8));   // index 0, no autoinc on byte 0
    wr(io, DATA, ((addr >> 8) & 0xFFu) << 8);
    wr(io, DATA, (addr & 0xFFu) << 8);
    if (len) wr(io, DATA, static_cast<uint32_t>(len - 1) << 8);
    if (isWrite) for (uint8_t i = 0; i < len; i++) wr(io, DATA, static_cast<uint32_t>(wdata ? wdata[i] : 0) << 8);
    wr(io, SWC, rd(io, SWC) | 1u);                                                    // AUX_SW_GO
    uint32_t st = 0;
    bool done = false;
    for (int i = 0; i < 2000 && !done; i++) { st = rd(io, STAT); done = (st & 1u) != 0; if (!done) delay(io, 10); }
    if (d) d->status = st;
    int rc;
    if (!done || (st & ((1u << 7) | (7u << 4)))) rc = kErrTimeout;
    else if (st & (1u << 9)) rc = kErrHpd;
    else if (st & ((1u << 14) | (1u << 20) | (1u << 22) | (1u << 23)) || !((st >> 24) & 0x1Fu)) rc = kErrNack;
    else {
        const uint32_t count = (st >> 24) & 0x1Fu;
        wr(io, DATA, 0);                                                              // as Linux (trace, AUX2): index 0,
        wr(io, DATA, 0x80000000u);                                                    // autoincrement disable,
        wr(io, DATA, 0x80000001u);                                                    // DATA_RW = read
        rc = static_cast<int>((rd(io, DATA) >> 12) & 0xFu);                           // reply header byte >> 4
        uint8_t n = 0;
        for (uint32_t i = 1; i < count; i++) {
            const uint8_t v = static_cast<uint8_t>(rd(io, DATA) >> 8);
            if (rdata && n < cap) rdata[n++] = v;
        }
        if (got) *got = n;
    }
    release();
    return rc;
}

// One AUX transaction with Linux's retry rules (dce_aux_transfer_with_retries: defers waited out, a few timeouts retried).
inline int aux(const Io &io, unsigned eng, uint8_t action, uint32_t addr, const uint8_t *wdata, uint8_t len,
               uint8_t *rdata, uint8_t cap, uint8_t *got, Diag *d) {
    unsigned timeouts = 0;
    for (unsigned t = 0; t < 54; t++) {
        int rc = auxOnce(io, eng, action, addr, wdata, len, rdata, cap, got, d);
        if (rc == 0) return kOk;
        if (rc == kErrTimeout && timeouts++ < 3) { delay(io, 500); continue; }
        if (rc == 2 || rc == 8) { if (d) d->retries++; delay(io, t < 7 ? 500 : 1000); continue; }
        if (rc == 1 || rc == 4) return kErrNack;
        return rc < 0 ? rc : kErrNack;
    }
    return kErrDefer;
}

inline int dpcdRead(const Io &io, unsigned eng, uint32_t addr, uint8_t *buf, uint8_t n, Diag *d) {
    uint8_t got = 0;
    int rc = aux(io, eng, 0x90, addr, nullptr, n, buf, n, &got, d);
    return rc == kOk && got == n ? kOk : (rc == kOk ? kErrNack : rc);
}

// One 128-byte EDID block over I2C-over-AUX.
inline int auxEdidBlock(const Io &io, unsigned eng, uint8_t offset, uint8_t *buf, Diag *d) {
    uint8_t got = 0;
    int rc = aux(io, eng, 0x40, 0x50, &offset, 1, nullptr, 0, &got, d);
    if (rc != kOk) return rc;
    for (unsigned pos = 0; pos < 128;) {
        if (d) d->step = pos;
        rc = aux(io, eng, 0x50, 0x50, nullptr, 16, buf + pos, static_cast<uint8_t>(128 - pos), &got, d);
        if (rc != kOk) break;
        if (!got) { rc = kErrNack; break; }
        pos += got;                                                                   // a sink may return fewer bytes
    }
    aux(io, eng, 0x10, 0x50, nullptr, 0, nullptr, 0, &got, nullptr);                 // address-only read: I2C stop
    return rc;
}

// ---- EDID -----------------------------------------------------------------------------------------------------------
inline bool blockOk(const uint8_t *b) { uint8_t s = 0; for (int i = 0; i < 128; i++) s = static_cast<uint8_t>(s + b[i]); return s == 0; }

// source: 0x10 + n = DC_I2C DDC line n (0x10 = DDC1); 0x20 + n = DP AUX engine n. Reads block 0 and, if the EDID has
// extensions, block 1 (256 bytes at most; more than one extension needs the E-DDC segment pointer, not done).
inline int readEdid(const Io &io, unsigned source, uint8_t out[256], unsigned *len, Diag *d) {
    *len = 0;
    const bool viaAux = (source & 0xF0) == 0x20;
    const unsigned n = source & 0xF;
    if (!viaAux && (source & 0xF0) != 0x10) return kErrArg;
    for (unsigned blk = 0; blk < 2; blk++) {
        if (d) d->step = blk << 8;
        int rc = viaAux ? auxEdidBlock(io, n, static_cast<uint8_t>(blk * 128), out + blk * 128, d)
                        : ddcRead(io, n, static_cast<uint8_t>(blk * 128), out + blk * 128, 128, d);
        if (rc != kOk) return blk ? kOk : rc;                                         // block 1 failing keeps block 0
        static const uint8_t kHdr[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
        if (!blk) for (int i = 0; i < 8; i++) if (out[i] != kHdr[i]) return kErrHeader;
        if (!blockOk(out + blk * 128)) return blk ? kOk : kErrChecksum;
        *len = (blk + 1) * 128;
        if (!blk && out[126] == 0) break;
    }
    return kOk;
}

// M7 / M6: the EDID source of the connector the GOP lit on pipe 0 (OTG0). The enabled DIG back end (DIGn_DIG_BE_EN_CNTL
// bit 0) whose front end takes OTG0 (DIGn_DIG_FE_CNTL DIG_SOURCE_SELECT 2:0 = 0) names the connector: DIG_BE_CNTL
// DIG_HPD_SELECT (30:28) = HPD n, DIG_MODE (18:16): 0 = DP -> AUX engine n (0x20 + n), 3 = HDMI -> DDC1 (0x10, the only
// HDMI connector of this board: vbios object table). Seen 10-29: Acer alone = DIG3 BE_CNTL 0x00030800 (HPD0, HDMI);
// with the AOC on DP the GOP drove OTG0 from DIG1 0x20000200 (HPD2, DP). Returns -1 when nothing matches (caller keeps
// its default source). DIG register stride 0x100 from DIG0 (0x554B FE_CNTL, 0x5571 BE_CNTL, 0x5572 BE_EN_CNTL).
constexpr uint32_t kDig0FeCntl = 0x554B, kDig0BeCntl = 0x5571, kDig0BeEnCntl = 0x5572, kDigStride = 0x100;
inline int scanoutSource(const Io &io) {
    for (unsigned d = 0; d < 5; d++) {
        if (!(rd(io, kDig0BeEnCntl + kDigStride * d) & 1u)) continue;
        if (rd(io, kDig0FeCntl + kDigStride * d) & 7u) continue;            // fed by another OTG
        const uint32_t be = rd(io, kDig0BeCntl + kDigStride * d);
        const unsigned hpd = (be >> 28) & 7u, mode = (be >> 16) & 7u;
        if (mode == 0 && hpd < 5) return static_cast<int>(0x20 + hpd);
        if (mode == 3) return 0x10;
    }
    return -1;
}

// M7 S3a (b81): which source a phxEdidRead caller gets. The caller's in-value of param4 asks for one:
//   0 or kSrcAuto  = the b80 rule (the connector lit on pipe 0, else debug.phxedid_src): every b79/b80 caller passed 0;
//   kSrcOther      = a second head: the board's other monitor, not the one on pipe 0 (otherSource);
//   a valid source = that source as is (0x10 + n DDC line n, 0x20 + n AUX engine n, n < 5);
//   anything else  = treated as auto, as b80 did (it ignored the in-value).
constexpr uint32_t kSrcAuto = 0xFFFFFFFFu, kSrcOther = 0xFFFFFFFEu;
inline bool validSource(uint32_t s) { return (s >= 0x10 && s <= 0x14) || (s >= 0x20 && s <= 0x24); }
// The connector a source belongs to (nullptr = none on this board).
inline const Connector *connectorOf(uint32_t src, const Connector *t, unsigned n) {
    for (unsigned i = 0; t && i < n; i++) if (connSource(t[i]) == src) return &t[i];
    return nullptr;
}
// The HPD pin of a source on this board, -1 = no connector uses that source.
inline int hpdOfSource(uint32_t s) {
    unsigned n = 0;
    const Connector *c = connectorOf(s, boardConnectors(&n), n);
    return c ? c->hpd : -1;
}
// The other monitor for head 1 (head 0 = the connector of the DIG lit on OTG0, phxddc::scanoutSource): the first OTHER
// connector of the table whose HPD senses a sink (*sensed = true). With none sensed, a phantom head still needs a
// source: the first other connector of a different type than pipe 0's (else the first other one), *sensed = false.
// Returns 0 only for an empty table. hpdMask bit n = HPDn senses a sink.
inline uint32_t otherSourceIn(int pipe0Src, uint32_t hpdMask, const Connector *t, unsigned n, bool *sensed) {
    if (sensed) *sensed = false;
    const Connector *p0 = pipe0Src >= 0 ? connectorOf(static_cast<uint32_t>(pipe0Src), t, n) : nullptr;
    for (unsigned i = 0; t && i < n; i++)
        if (&t[i] != p0 && t[i].hpd < 32 && ((hpdMask >> t[i].hpd) & 1u)) { if (sensed) *sensed = true; return connSource(t[i]); }
    for (unsigned i = 0; p0 && i < n; i++) if (&t[i] != p0 && t[i].type != p0->type) return connSource(t[i]);
    for (unsigned i = 0; t && i < n; i++) if (&t[i] != p0) return connSource(t[i]);
    return 0;
}
inline uint32_t otherSource(int pipe0Src, uint32_t hpdMask, bool *sensed) {
    unsigned n = 0;
    const Connector *t = boardConnectors(&n);
    return otherSourceIn(pipe0Src, hpdMask, t, n, sensed);
}
// The source a phxEdidRead with in-value `req` reads. autoSrc = scanoutSource (or -1), defSrc = debug.phxedid_src,
// autoOn = debug.phxedid_auto (only the auto rule obeys it: a second head always needs to know pipe 0's connector).
inline uint32_t resolveSource(uint32_t req, int autoSrc, bool autoOn, uint32_t defSrc, uint32_t hpdMask, bool *sensed) {
    if (sensed) *sensed = false;
    if (req == kSrcOther) return otherSource(autoSrc, hpdMask, sensed);
    if (req != 0 && req != kSrcAuto && validSource(req)) return req;
    return (autoOn && autoSrc >= 0) ? static_cast<uint32_t>(autoSrc) : defSrc;
}

}  // namespace phxddc
