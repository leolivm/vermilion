// SPDX-License-Identifier: MIT
// PhxDmub.h: the DMCUB inbox1 ring of the boot (GOP) display firmware, for kext b82 (M7 S2).
// Kernel-free: stdint only; the kext (phx_pipe2.cpp) and the host test
// supply the register and VRAM accessors through Io.
//
// The ring lives in REGION4 near the top of the 20 GB VRAM (VRAM 0x4_FEC00000 on 10-29), out of reach of the 256 MiB
// BAR0, so its dwords go through the BIF indirect window: BAR5 dword 6 = MM_INDEX_HI (offset >> 31), dword 0 =
// MM_INDEX ((offset & 0x7ffffffc) | 0x80000000, bit 31 = the VRAM aperture), dword 1 = MM_DATA (amdgpu_device_mm_access;
// RX4Darwin RDNA4Device::vramRead32/vramWrite32). The caller makes each index+data triple atomic (phx_pipe2.cpp: one
// IOSimpleLock with interrupts off; nothing else in the kext uses the pair, and ReadRegs/WriteReg of dwords 0-6 take the
// same lock).
//
// Submit protocol (RX4Darwin dmubSubmit on our registers): the ring must be idle (WPTR == RPTR, WPTR
// 64-aligned and inside the ring); the 16 dwords are written at WPTR and all read back (a mismatch = nothing submitted);
// INBOX1_WPTR <- (WPTR + 64) % size; acceptance = RPTR reaches the new WPTR within 200 ms. A timeout latches `dead`:
// every later submit is refused without touching the hardware, and nothing is ever retried (a firmware that stopped answering is never poked again).
//
// Allow-list: only (type, subtype) 6/0 QUERY_FEATURE_CAPS, 128/0 DIGX_ENCODER_CONTROL, 128/1
// DIG1_TRANSMITTER_CONTROL, 128/2 SET_PIXEL_CLOCK, and within those only what pipe 1 / DIG3 / PHY D needs: DIG 3 stream
// setup, PHY D enable/disable, PLL3 for crtc 1 or the PLL3 power-down (crtc 255). Never FAMS (76), CAB (75), EDID_CEA
// (79), HW_LOCK (69), inbox0 or GPINT, and never a command that names crtc 0, PHY B or DIG 1 (the AOC's pipe).
// b109: the one exception is restart() (end of this file): GPINT STOP_FW and, with debug.phxdmub_restart 3/4, the GOP's
// own 0x0C post-start command (submitInit), both only through that one-shot path. b111: submitRegTest, one type-1
// REG_SEQ write pinned to OTG1_OTG_H_TOTAL (debug.phxdmub_regtest), its own exact-shape check, never cmdAllowed.
#pragma once
#include <stdint.h>

namespace phxdmub {

// DCN 3.2 absolute dwords (DCN BASE_IDX 2 base 0x34C0 + dcn_3_2_0_offset.h; read on the box)
static constexpr uint32_t kRegion4Off = 0x3656, kRegion4OffHi = 0x3657, kRegion4Top = 0x3661, kIntStatus = 0x3687,
                          kInbox1Base = 0x3694, kInbox1Size = 0x3695, kInbox1Wptr = 0x3696, kInbox1Rptr = 0x3697,
                          kScratch0 = 0x36A3, kScratch1 = 0x36A4, kScratch7 = 0x36AA, kCntl = 0x36B6,
                          kDcnVmFbBase = 0x3935;
// BIF indirect window (nbio_4_3_0_offset.h regBIF_BX_PF0_MM_INDEX / _DATA / _INDEX_HI), BAR5 dwords
static constexpr uint32_t kMmIndex = 0, kMmData = 1, kMmIndexHi = 6;
static constexpr uint32_t kCmdBytes = 64, kCmdDw = 16, kWinBytes = 0x10000;   // REGION4 window = 64 KiB
static constexpr uint32_t kAcceptUs = 200000, kPollStepUs = 10;

inline uint32_t mmIndexLo(uint64_t off) { return (static_cast<uint32_t>(off) & 0x7ffffffcu) | 0x80000000u; }
inline uint32_t mmIndexHi(uint64_t off) { return static_cast<uint32_t>(off >> 31); }

struct Io {
    void *c;
    uint32_t (*rd)(void *c, uint32_t reg);                 // MMIO dword read
    void (*wr)(void *c, uint32_t reg, uint32_t v);         // MMIO dword write
    uint32_t (*mmRd)(void *c, uint64_t vramOff);           // indirect VRAM dword read (atomic triple)
    void (*mmWr)(void *c, uint64_t vramOff, uint32_t v);   // indirect VRAM dword write (atomic triple)
    void (*delayUs)(void *c, uint32_t us);
};

struct Ring { uint64_t vram; uint32_t size, wptr, rptr; };

enum Rc { kOk = 0, kDead, kNotAllowed, kNoRing, kNotIdle, kReadback, kTimeout };
inline const char *rcName(int rc) {
    static const char *const n[] = {"ok", "dead (an earlier command timed out)", "command not on the allow-list", "ring geometry not as planned",
                                    "ring not idle", "ring write read-back mismatch (nothing submitted)", "timeout: RPTR did not catch up"};
    return rc >= 0 && rc <= kTimeout ? n[rc] : "?";
}

// Firmware/ring counters; `dead` is the latch (also exported as debug.phxdmub_dead).
struct St {
    int dead;
    uint32_t sent, consumed, timeouts, refused, readbackFails;
    uint32_t lastRc, lastHdr, lastSlot, lastUs, maxUs;
    uint32_t reply[kCmdDw];                // the slot read back after the firmware consumed it
    uint32_t deadInfo[6];                  // at the timeout: SCRATCH0, SCRATCH7, INTERRUPT_STATUS, CNTL, WPTR, RPTR
};

// The allow-list. `why` names the refusal.
inline bool cmdAllowed(const uint32_t *c, const char **why) {
    const char *w = nullptr;
    const uint32_t h = c[0], type = h & 0xff, sub = (h >> 8) & 0xff, pb = (h >> 24) & 0x3f;
    if (h & 0xc0ff0000u) w = "header status/multi/reg-based bits set";
    else if (type == 6 && sub == 0) {
        if (pb != 12) w = "QUERY_FEATURE_CAPS payload size";
        for (unsigned i = 1; i < kCmdDw && !w; i++) if (c[i]) w = "QUERY_FEATURE_CAPS payload not zero";
    } else if (type == 128 && sub == 0) {            // dig_encoder_stream_setup_parameters_v1_5
        if (pb != 12) w = "DIGX payload size";
        else if ((c[1] & 0xff) != 3) w = "DIGX not DIG 3";
        else if (((c[1] >> 8) & 0xff) != 0x0f) w = "DIGX action not STREAM_SETUP";
    } else if (type == 128 && sub == 1) {            // dmub_dig_transmitter_control_data_v1_7
        const uint32_t act = (c[1] >> 8) & 0xff;
        if (pb != 60) w = "TRANSMITTER payload size";
        else if ((c[1] & 0xff) != 3) w = "TRANSMITTER not PHY D";
        else if (act != 0 && act != 1 && act != 7) w = "TRANSMITTER action not ENABLE/DISABLE/INIT";
        else if (act == 7 && (((c[1] >> 24) & 0xff) != 4 || c[2] != 0)) w = "TRANSMITTER INIT not 4 lanes / symclk 0";
        else if ((c[3] & 0xff) != 1) w = "TRANSMITTER hpdsel not 1 (the Acer's HPD)";
    } else if (type == 128 && sub == 2) {            // set_pixel_clock_parameter_v1_7
        const uint32_t pll = c[2] & 0xff, crtc = c[3] & 0xff;
        if (pb != 16) w = "SET_PIXEL_CLOCK payload size";
        else if (pll != 23) w = "SET_PIXEL_CLOCK not COMBOPHY_PLL3";
        else if (crtc != 1 && crtc != 255) w = "SET_PIXEL_CLOCK crtc not 1 (or 255 = power-down)";
        else if (crtc == 255 && c[1] != 0) w = "PLL power-down with a pixel clock";
        else if (crtc == 1 && c[1] == 0) w = "SET_PIXEL_CLOCK crtc 1 without a pixel clock";
    } else w = "(type, subtype) not 6/0, 128/0, 128/1 or 128/2";
    if (why) *why = w;
    return !w;
}

// The ring as the registers describe it, checked against the plan. vramBytes bounds it.
inline bool ringGeometry(const Io &io, uint64_t vramBytes, Ring *r, const char **why) {
    const char *w = nullptr;
    const uint64_t r4 = io.rd(io.c, kRegion4Off) | static_cast<uint64_t>(io.rd(io.c, kRegion4OffHi)) << 32;
    const uint32_t top = io.rd(io.c, kRegion4Top), base = io.rd(io.c, kInbox1Base), size = io.rd(io.c, kInbox1Size);
    const uint64_t fb = static_cast<uint64_t>(io.rd(io.c, kDcnVmFbBase) & 0xffffff) << 24;
    if (!(top & 0x80000000u)) w = "REGION4 not enabled";
    else if (base != 0x80000000u) w = "INBOX1 base not REGION4 offset 0";
    else if (!size || size % kCmdBytes || size > kWinBytes || size > (top & 0xffffffu) + 1) w = "INBOX1 size not a 64-byte multiple inside REGION4";
    else if (r4 <= fb || (r4 & 3)) w = "REGION4 not above the FB base";
    else if (r4 - fb + size > vramBytes) w = "ring beyond VRAM";
    r->vram = w ? 0 : r4 - fb;
    r->size = w ? 0 : size;
    r->wptr = io.rd(io.c, kInbox1Wptr);
    r->rptr = io.rd(io.c, kInbox1Rptr);
    if (why) *why = w;
    return !w;
}

// One command already past the allow-list (submit, submitInit): the ring part.
inline int put(const Io &io, St &st, uint64_t vramBytes, const uint32_t *cmd, const char **why) {
    const char *w = nullptr;
    Ring r;
    if (!ringGeometry(io, vramBytes, &r, &w)) { st.refused++; st.lastRc = kNoRing; if (why) *why = w; return kNoRing; }
    if (r.wptr != r.rptr || r.wptr >= r.size || r.wptr % kCmdBytes) { st.refused++; st.lastRc = kNotIdle; return kNotIdle; }
    const uint64_t slot = r.vram + r.wptr;
    for (unsigned i = 0; i < kCmdDw; i++) io.mmWr(io.c, slot + 4u * i, cmd[i]);
    for (unsigned i = 0; i < kCmdDw; i++)
        if (io.mmRd(io.c, slot + 4u * i) != cmd[i]) { st.readbackFails++; st.lastRc = kReadback; return kReadback; }
    const uint32_t nw = (r.wptr + kCmdBytes) % r.size;
    st.lastHdr = cmd[0];
    st.lastSlot = r.wptr;
    io.wr(io.c, kInbox1Wptr, nw);
    st.sent++;
    uint32_t us = 0, rp = r.rptr;
    for (;;) {
        rp = io.rd(io.c, kInbox1Rptr);
        if (rp == nw) break;
        if (us >= kAcceptUs) {
            st.dead = 1;
            st.timeouts++;
            st.deadInfo[0] = io.rd(io.c, kScratch0); st.deadInfo[1] = io.rd(io.c, kScratch7);
            st.deadInfo[2] = io.rd(io.c, kIntStatus); st.deadInfo[3] = io.rd(io.c, kCntl);
            st.deadInfo[4] = io.rd(io.c, kInbox1Wptr); st.deadInfo[5] = rp;
            st.lastRc = kTimeout;
            return kTimeout;
        }
        io.delayUs(io.c, kPollStepUs);
        us += kPollStepUs;
    }
    st.consumed++;
    st.lastUs = us;
    if (us > st.maxUs) st.maxUs = us;
    for (unsigned i = 0; i < kCmdDw; i++) st.reply[i] = io.mmRd(io.c, slot + 4u * i);
    st.lastRc = kOk;
    return kOk;
}
// One command. reply (optional) gets the slot read back after it was consumed.
inline int submit(const Io &io, St &st, uint64_t vramBytes, const uint32_t *cmd, const char **why) {
    const char *w = nullptr;
    if (why) *why = nullptr;
    if (st.dead) { st.refused++; st.lastRc = kDead; return kDead; }
    if (!cmdAllowed(cmd, &w)) { st.refused++; st.lastRc = kNotAllowed; if (why) *why = w; return kNotAllowed; }
    return put(io, st, vramBytes, cmd, why);
}
// b116 (multiport S2b, debug.phxdplink): one command through a caller-supplied exact-shape check instead of cmdAllowed,
// so the pipe-1 plan's list (cmdAllowed) stays exactly as strict as before; same dead latch, same ring path (put).
typedef bool (*CmdCheck)(const uint32_t *c, const void *ctx, const char **why);
inline int submitWith(const Io &io, St &st, uint64_t vramBytes, const uint32_t *cmd, CmdCheck chk, const void *ctx, const char **why) {
    const char *w = nullptr;
    if (why) *why = nullptr;
    if (st.dead) { st.refused++; st.lastRc = kDead; return kDead; }
    if (!chk || !chk(cmd, ctx, &w)) { st.refused++; st.lastRc = kNotAllowed; if (why) *why = w ? w : "no check"; return kNotAllowed; }
    return put(io, st, vramBytes, cmd, why);
}
// b109: the GOP's post-start command, subtype 0x0C with no payload: slot 0x0000 of every stored GOP ring holds exactly
// these 16 dwords (three ring dumps taken on the box after cold boots). Only after
// restart() (debug.phxdmub_restart 3 / 4); outside cmdAllowed, which stays the plan's allow-list.
static const uint32_t kInitCmd[kCmdDw] = {0x00000c80u};
inline int submitInit(const Io &io, St &st, uint64_t vramBytes, const char **why) {
    if (why) *why = nullptr;
    if (st.dead) { st.refused++; st.lastRc = kDead; return kDead; }
    return put(io, st, vramBytes, kInitCmd, why);
}

// b111: ONE generic DMUB register write, to learn whether
// the GOP build 0x07008742 executes DMUB_CMD__REG_SEQ_READ_MODIFY_WRITE. Layout (dmub_cmd.h, Linux v6.8):
// enum dmub_cmd_type DMUB_CMD__REG_SEQ_READ_MODIFY_WRITE = 1; struct dmub_cmd_header {type:8, sub_type:8, ret_status:1,
// multi_cmd_pending:1, reserved0:6, payload_bytes:6, reserved1:2}; struct dmub_rb_cmd_read_modify_write {header;
// dmub_cmd_read_modify_write_sequence seq[5] = {addr, modify_mask, modify_value}}; modify_mask 0xffffffff = the parser
// skips the read (a plain write). dc_helper.c dmub_reg_value_pack / submit_dmub_read_modify_write: sub_type 0,
// addr = the absolute register dword, payload_bytes = 12 per sequence. So one sequence = header 0x0C000001.
// The address is pinned here (the firmware handler adds addr * 4 to 0x5FFC0000 with no range check): OTG1_OTG_H_TOTAL
// (dcn_3_2_0_offset.h 0x1baa + 0x34C0), and only the value comes from the caller. Outside cmdAllowed, which stays the
// plan's allow-list; regTestCmdOk is this command's own exact-shape check, re-run by submitRegTest before the ring.
static constexpr uint32_t kRegSeqRmwHdr = 0x0C000001u, kRegTestAddr = 0x0506Au, kRegTestMask = 0xFFFFFFFFu;
inline void regTestCmd(uint32_t value, uint32_t *c) {
    for (unsigned i = 0; i < kCmdDw; i++) c[i] = 0;
    c[0] = kRegSeqRmwHdr; c[1] = kRegTestAddr; c[2] = kRegTestMask; c[3] = value;
}
inline bool regTestCmdOk(const uint32_t *c) {
    if (c[0] != kRegSeqRmwHdr || c[1] != kRegTestAddr || c[2] != kRegTestMask) return false;
    for (unsigned i = 4; i < kCmdDw; i++) if (c[i]) return false;
    return true;
}
inline int submitRegTest(const Io &io, St &st, uint64_t vramBytes, uint32_t value, const char **why) {
    uint32_t c[kCmdDw];
    regTestCmd(value, c);
    if (why) *why = nullptr;
    if (st.dead) { st.refused++; st.lastRc = kDead; return kDead; }
    if (!regTestCmdOk(c)) { st.refused++; st.lastRc = kNotAllowed; if (why) *why = "not the pinned OTG1_OTG_H_TOTAL write"; return kNotAllowed; }
    return put(io, st, vramBytes, c, why);
}

// b88 (a cold-boot stream failure): the firmware's trace buffer, window CW5 (Linux DMUB_WINDOW_5_TRACEBUFF; the GOP set it to
// 64 KiB at MC 0x84_FEC50000 = VRAM 0x4_FEC50000), entries {code, tick, param0, param1} after a 16-byte header; OUTBOX0_WPTR
// is the firmware's write offset. Reads only, like the ring.
static constexpr uint32_t kCw5Base = 0x366A, kCw5Top = 0x3672, kCw5Off = 0x367F, kCw5OffHi = 0x3680, kOutbox0Wptr = 0x369A;
struct Trace { uint64_t vram; uint32_t size, wptr; };
inline bool traceGeometry(const Io &io, uint64_t vramBytes, Trace *t, const char **why) {
    const char *w = nullptr;
    const uint64_t off = io.rd(io.c, kCw5Off) | static_cast<uint64_t>(io.rd(io.c, kCw5OffHi)) << 32;
    const uint32_t base = io.rd(io.c, kCw5Base), top = io.rd(io.c, kCw5Top);
    const uint64_t fb = static_cast<uint64_t>(io.rd(io.c, kDcnVmFbBase) & 0xffffff) << 24;
    const uint32_t lo = base & 0x7fffffffu, hi = top & 0x7fffffffu;
    if (!(top & 0x80000000u)) w = "CW5 not enabled";
    else if (hi < lo || hi - lo + 1 > kWinBytes || ((hi - lo + 1) & 3)) w = "CW5 size not 4..64 KiB";
    else if (off <= fb || (off & 3)) w = "CW5 not above the FB base";
    else if (off - fb + (hi - lo + 1) > vramBytes) w = "CW5 beyond VRAM";
    t->vram = w ? 0 : off - fb;
    t->size = w ? 0 : hi - lo + 1;
    t->wptr = io.rd(io.c, kOutbox0Wptr);
    if (why) *why = w;
    return !w;
}
inline bool traceRead(const Io &io, uint64_t vramBytes, Trace *t, uint32_t *out, uint32_t outBytes, const char **why) {
    if (!traceGeometry(io, vramBytes, t, why)) return false;
    if (outBytes < t->size) { if (why) *why = "buffer too small"; return false; }
    for (uint32_t i = 0; i < t->size / 4; i++) out[i] = io.mmRd(io.c, t->vram + 4u * i);
    return true;
}

// Read the whole ring (reads only; MM_INDEX/MM_INDEX_HI are written as the window's address). out holds r.size bytes.
inline bool ringRead(const Io &io, uint64_t vramBytes, Ring *r, uint32_t *out, uint32_t outBytes, const char **why) {
    if (!ringGeometry(io, vramBytes, r, why)) return false;
    if (outBytes < r->size) { if (why) *why = "buffer too small"; return false; }
    for (uint32_t i = 0; i < r->size / 4; i++) out[i] = io.mmRd(io.c, r->vram + 4u * i);
    return true;
}

// ---- b109 debug.phxdmub_restart (cold-boot dark Acer, candidate C1): restart the
// running GOP firmware without an image reload, as ATOM table 66's "running" branch (ROM 0x452D-0x4867) and Linux
// dmub_dcn32_reset / _reset_release do: STOP_FW over GPINT, soft reset, pointers + SCRATCH0 cleared, CW4-CW6 zeroed,
// release, wait for SCRATCH0 mailbox_rdy. Never: a PSP message (C2PMSG_33), DMCUB_SEC_CNTL, CW0/CW1/CW2, an ATOM table,
// subtype 0x0D. Every register write goes through rsWr (the list below; anything else is dropped and counted).
// Offsets: dcn_3_2_0_offset.h + 0x34C0 (BASE_IDX 2), fields dcn_3_2_0_sh_mask.h.
static constexpr uint32_t kGpintIn1 = 0x36B8, kCntl2 = 0x36C0, kScratch14 = 0x36B1, kTimer = 0x36BD, kSecCntl = 0x368E,
                          kMmhubbubSoftReset = 0x3802, kOutbox0Rptr = 0x369B, kOutbox1Wptr = 0x369E, kOutbox1Rptr = 0x369F;
static constexpr uint32_t kFault[3] = {0x368C /*INST_FETCH*/, 0x368D /*DATA_WRITE*/, 0x36BA /*UNDEFINED_ADDRESS*/};
static constexpr uint32_t kFwVersion = 0x07008742u, kGpintStopFw = 0x10020000u, kGpintStopAck = 0x00020000u, kHaltMagic = 0xDEADDEADu;
static constexpr uint32_t kCntlEnable = 1u << 16, kCntlPwait = 1u << 20, kCntlRelease = 0x900C6u, kSoftReset = 1u, kDmuifReset = 0x100u;
// Saved before the first write, re-checked after the reset (step 10; a changed one gets its saved value back, as table 66
// re-programs the same values): CW3-CW6 BASE/TOP/OFFSET/OFFSET_HIGH, REGION4 and REGION5 OFFSET/OFFSET_HIGH/TOP, INBOX1 /
// OUTBOX0 / OUTBOX1 BASE + SIZE.
static constexpr uint32_t kRsSave[] = {0x3668, 0x3670, 0x367B, 0x367C, 0x3669, 0x3671, 0x367D, 0x367E,
                                       0x366A, 0x3672, 0x367F, 0x3680, 0x366B, 0x3673, 0x3681, 0x3682,
                                       0x3656, 0x3657, 0x3661, 0x3658, 0x3659, 0x3662,
                                       0x3694, 0x3695, 0x3698, 0x3699, 0x369C, 0x369D};
static constexpr unsigned kRsNSave = sizeof kRsSave / sizeof kRsSave[0];
inline bool rsWriteAllowed(uint32_t reg) {
    if (reg == kSecCntl || (reg >= 0x3665 && reg <= 0x3667) || (reg >= 0x366D && reg <= 0x366F) || (reg >= 0x3675 && reg <= 0x367A)) return false;
    switch (reg) {
    case kGpintIn1: case kCntl2: case kCntl: case kMmhubbubSoftReset: case kInbox1Wptr: case kInbox1Rptr:
    case kOutbox0Wptr: case kOutbox0Rptr: case kOutbox1Wptr: case kOutbox1Rptr: case kScratch0: case kScratch14: return true;
    default: break;
    }
    for (uint32_t r : kRsSave) if (r == reg) return true;
    return false;
}
// Trace codes: entries {code, tick, p0, p1} from byte 16 of CW5 to 16 + OUTBOX0_WPTR.
static constexpr uint32_t kTrMainBegin = 0x01, kTrPllBegin = 0xc0, kTrPllOff = 0xe8, kTrPll10a = 0x10a, kTrPhyD = 3;
static constexpr uint32_t kTrBoot[] = {0x02, 0xe9, 0xea, 0xeb, 0xe9, 0xea, 0xeb, 0xe9, 0xea, 0xeb, 0xe9, 0xea, 0xeb, 0x03, 0x11};
inline unsigned traceEntries(uint32_t size, uint32_t wptr) { return (wptr & 15) || static_cast<uint64_t>(wptr) + 16 > size ? ~0u : wptr / 16; }

enum RsRc { kRsOk = 0, kRsRefused, kRsNoAck, kRsDead, kRsHalted };
static const char *const kRsName[] = {"ok", "refused", "no-ack", "dead", "halted"};
static const char *const kRsStage[] = {"preconditions", "stop_fw", "ack", "halt", "gpint_clear", "soft_reset", "disable", "dmuif_reset",
                                       "pointers", "dmuif_release", "windows", "zero_cw4_cw6", "boot_options", "enable", "release",
                                       "boot_wait", "verify", "init_0x0c", "guard"};
// The one shot of this boot and its record (debug.phxdmub_restart_stat). done = the first register was written.
struct RestartSt {
    int done, rc, stage, mode;
    uint32_t attempts, refusals, gpioA;
    uint32_t pre[4], post[4];                   // SCRATCH0, SCRATCH1, SCRATCH7, CNTL before the first write / after the boot
    uint32_t otg0CtlPre, otg0CtlPost;          // b110: OTG0_OTG_CONTROL around the restart (the guard ignores DISABLE_POINT_CNTL)
    uint32_t ackUs, haltUs, pwaitUs, bootUs, initUs;
    int acked, stopped, guard, initRc;         // STOP_FW acked; SCRATCH7 read 0xDEADDEAD; pipe-0 guard passed; 0x0C rc (-1 none)
    uint32_t saved[kRsNSave], fault[3], diffMask, restored, zeroed, stray;
    uint64_t traceVram; uint32_t traceBytes, traceWptr, mainBeginPre, mainBeginPost, bootBlock;
    const char *why, *lastRefusal, *guardWhy;
};
inline int rsRefuse(RestartSt &rs, const char *why) { rs.rc = kRsRefused; rs.stage = 0; rs.why = rs.lastRefusal = why; rs.refusals++; return kRsRefused; }
inline int rsEnd(RestartSt &rs, int rc, int stage, const char *why) { rs.rc = rc; rs.stage = stage; rs.why = why; return rc; }
inline void rsWr(const Io &io, RestartSt &rs, uint32_t reg, uint32_t v) {
    if (!rsWriteAllowed(reg)) { rs.stray++; return; }
    io.wr(io.c, reg, v);
}
inline void rsPost(const Io &io, RestartSt &rs) {
    rs.post[0] = io.rd(io.c, kScratch0); rs.post[1] = io.rd(io.c, kScratch1); rs.post[2] = io.rd(io.c, kScratch7); rs.post[3] = io.rd(io.c, kCntl);
}
// After the reset point: the dead latch (nothing more is sent this boot), with the timeout's diagnosis registers.
inline int rsDead(const Io &io, St &st, RestartSt &rs, int stage, const char *why) {
    st.dead = 1;
    st.deadInfo[0] = io.rd(io.c, kScratch0); st.deadInfo[1] = io.rd(io.c, kScratch7); st.deadInfo[2] = io.rd(io.c, kIntStatus);
    st.deadInfo[3] = io.rd(io.c, kCntl); st.deadInfo[4] = io.rd(io.c, kInbox1Wptr); st.deadInfo[5] = io.rd(io.c, kInbox1Rptr);
    rsPost(io, rs);
    return rsEnd(rs, kRsDead, stage, why);
}
// rd(reg) & mask == val, polled every stepUs (1 ms after the first 20 ms) up to maxUs; *us = the time it took.
inline bool rsPoll(const Io &io, uint32_t reg, uint32_t mask, uint32_t val, uint32_t stepUs, uint32_t maxUs, uint32_t *us) {
    for (uint32_t t = 0;;) {
        if ((io.rd(io.c, reg) & mask) == val) { *us = t; return true; }
        if (t >= maxUs) { *us = t; return false; }
        const uint32_t s = t >= 20000 ? 1000 : stepUs;
        io.delayUs(io.c, s);
        t += s;
    }
}
// A CW window from the saved registers (k 0..3 = CW3..CW6): enabled, size <= cap, above the FB base, inside VRAM.
struct Win { uint64_t vram; uint32_t size; };
inline bool rsWin(const RestartSt &rs, unsigned k, uint64_t fb, uint64_t vramBytes, uint32_t cap, Win *w) {
    const uint32_t base = rs.saved[4 * k] & 0x7fffffffu, top = rs.saved[4 * k + 1];
    const uint64_t off = rs.saved[4 * k + 2] | static_cast<uint64_t>(rs.saved[4 * k + 3]) << 32;
    const uint32_t hi = top & 0x7fffffffu;
    if (!(top & 0x80000000u) || hi < base || hi - base + 1 > cap || ((hi - base + 1) & 3) || off <= fb || (off & 3)) return false;
    w->vram = off - fb; w->size = hi - base + 1;
    return w->vram + w->size <= vramBytes;
}
inline bool rsOverlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a < b + bn && b < a + an; }

// Steps 1-17 of the restart with its preconditions P5/P6/P8 (P1-P4, P7 and the step-18 guard: PhxPipe2.h dmubRestart).
// trace (traceBytes >= the CW5 size) gets the firmware trace as it was before the first write. init = step 17, the 0x0C
// replay. Returns RsRc; before step 1 only reads happen (refused leaves done = 0).
inline int restart(const Io &io, St &st, RestartSt &rs, uint64_t vramBytes, bool init, uint32_t *trace, uint32_t traceBytes) {
    const char *w = nullptr;
    rs.initRc = -1;
    const uint32_t cntl = io.rd(io.c, kCntl);
    if ((cntl & (kCntlEnable | kCntlPwait)) != (kCntlEnable | kCntlPwait)) return rsRefuse(rs, "DMCUB not enabled and idle (CNTL bit 16 / bit 20)");
    if (io.rd(io.c, kCntl2) & kSoftReset) return rsRefuse(rs, "DMCUB held in soft reset (CNTL2 bit 0)");
    if (io.rd(io.c, kScratch0) != 2) return rsRefuse(rs, "SCRATCH0 is not 0x2 (mailbox_rdy only)");
    if (io.rd(io.c, kScratch1) != kFwVersion) return rsRefuse(rs, "SCRATCH1 is not the GOP firmware 0x07008742");
    if (io.rd(io.c, kGpintIn1)) return rsRefuse(rs, "GPINT_DATAIN1 not 0 (a GPINT in flight)");
    if (io.rd(io.c, kScratch14)) return rsRefuse(rs, "SCRATCH14 (boot options) not 0");
    Ring r;
    if (!ringGeometry(io, vramBytes, &r, &w)) return rsRefuse(rs, w);
    if (r.wptr != r.rptr) return rsRefuse(rs, "ring not idle (INBOX1 WPTR != RPTR)");
    Trace t;
    if (!traceGeometry(io, vramBytes, &t, &w)) return rsRefuse(rs, w);
    // P8: the window registers, the fault registers, the CW4-CW6 extents (never over CW3 or the ring)
    for (unsigned i = 0; i < kRsNSave; i++) rs.saved[i] = io.rd(io.c, kRsSave[i]);
    for (unsigned i = 0; i < 3; i++) rs.fault[i] = io.rd(io.c, kFault[i]);
    const uint64_t fb = static_cast<uint64_t>(io.rd(io.c, kDcnVmFbBase) & 0xffffff) << 24;
    Win cw[4];
    for (unsigned k = 0; k < 4; k++)
        if (!rsWin(rs, k, fb, vramBytes, k ? kWinBytes : 0x1000000u, &cw[k])) return rsRefuse(rs, "a CW3-CW6 window is not enabled, sized or inside VRAM");
    const uint32_t ringWin = (io.rd(io.c, kRegion4Top) & 0xffffffu) + 1;
    for (unsigned k = 1; k < 4; k++)
        if (rsOverlap(cw[k].vram, cw[k].size, cw[0].vram, cw[0].size) || rsOverlap(cw[k].vram, cw[k].size, r.vram, ringWin))
            return rsRefuse(rs, "a CW4-CW6 window overlaps CW3 or the ring");
    if (cw[2].vram != t.vram || cw[2].size != t.size) return rsRefuse(rs, "CW5 registers and the trace geometry disagree");
    // P8 trace copy, then P6 on it
    if (!trace || traceBytes < t.size) return rsRefuse(rs, "trace buffer too small");
    for (uint32_t i = 0; i < t.size / 4; i++) trace[i] = io.mmRd(io.c, t.vram + 4u * i);
    rs.traceVram = t.vram; rs.traceBytes = t.size; rs.traceWptr = t.wptr;
    const unsigned n = traceEntries(t.size, t.wptr);
    if (n == ~0u) return rsRefuse(rs, "trace: OUTBOX0_WPTR outside the window");
    bool phyD = false;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t c = trace[4 + 4 * i], p0 = trace[6 + 4 * i];
        rs.mainBeginPre += c == kTrMainBegin;
        phyD |= (c == kTrPllBegin || c == kTrPllOff || c == kTrPll10a) && p0 == kTrPhyD;
    }
    if (rs.mainBeginPre != 1) return rsRefuse(rs, "trace: not exactly one MAIN_BEGIN (no trace, or a restart already happened)");
    if (phyD) return rsRefuse(rs, "trace: PHY D / PLL3 already programmed in this firmware boot (poisoned chain)");
    rs.pre[0] = io.rd(io.c, kScratch0); rs.pre[1] = io.rd(io.c, kScratch1); rs.pre[2] = io.rd(io.c, kScratch7); rs.pre[3] = cntl;

    rs.done = 1;   // the one shot is spent from the first write on
    rs.stage = 1; rsWr(io, rs, kGpintIn1, kGpintStopFw);
    rs.stage = 2; rs.acked = rsPoll(io, kGpintIn1, ~0u, kGpintStopAck, kPollStepUs, 10000, &rs.ackUs);
    if (!rs.acked) {
        rsWr(io, rs, kGpintIn1, 0);
        rs.stopped = io.rd(io.c, kScratch7) == kHaltMagic;
        if (!rs.stopped) { rsPost(io, rs); return rsEnd(rs, kRsNoAck, 2, "STOP_FW not acked within 10 ms: GPINT cleared, nothing else written, the firmware keeps running"); }
        // halted without the ack: both AMD procedures force the reset; go on
    } else {
        rs.stage = 3;
        rs.stopped = rsPoll(io, kScratch7, ~0u, kHaltMagic, kPollStepUs, 10000, &rs.haltUs);
        rsPoll(io, kCntl, kCntlPwait, kCntlPwait, kPollStepUs, 10000, &rs.pwaitUs);   // a timeout is not fatal (both AMD procedures)
        rs.stage = 4; rsWr(io, rs, kGpintIn1, 0);
    }
    // the reset point: any failure from here latches dead
    rs.stage = 5; rsWr(io, rs, kCntl2, io.rd(io.c, kCntl2) | kSoftReset);
    rs.stage = 6; rsWr(io, rs, kCntl, io.rd(io.c, kCntl) & ~kCntlEnable);
    rs.stage = 7; rsWr(io, rs, kMmhubbubSoftReset, io.rd(io.c, kMmhubbubSoftReset) | kDmuifReset); io.delayUs(io.c, 10);
    rs.stage = 8;
    rsWr(io, rs, kInbox1Rptr, 0); rsWr(io, rs, kInbox1Wptr, 0); rsWr(io, rs, kOutbox0Rptr, 0); rsWr(io, rs, kOutbox0Wptr, 0);
    rsWr(io, rs, kOutbox1Rptr, 0); rsWr(io, rs, kOutbox1Wptr, 0); rsWr(io, rs, kScratch0, 0);
    rs.stage = 9; rsWr(io, rs, kMmhubbubSoftReset, io.rd(io.c, kMmhubbubSoftReset) & ~kDmuifReset);
    rs.stage = 10;
    for (unsigned i = 0; i < kRsNSave; i++) {
        if (io.rd(io.c, kRsSave[i]) == rs.saved[i]) continue;
        rs.diffMask |= 1u << i; rs.restored++;
        rsWr(io, rs, kRsSave[i], rs.saved[i]);
        if (io.rd(io.c, kRsSave[i]) != rs.saved[i]) return rsDead(io, st, rs, 10, "a window register did not take its saved value back");
    }
    rs.stage = 11;
    for (unsigned k = 1; k < 4; k++) {
        for (uint32_t o = 0; o < cw[k].size; o += 4) io.mmWr(io.c, cw[k].vram + o, 0);
        rs.zeroed += cw[k].size;
        if (io.mmRd(io.c, cw[k].vram) || io.mmRd(io.c, cw[k].vram + cw[k].size - 4)) return rsDead(io, st, rs, 11, "CW4-CW6 not zero after the clear");
    }
    rs.stage = 12; rsWr(io, rs, kScratch14, 0);
    rs.stage = 13; rsWr(io, rs, kCntl, (io.rd(io.c, kCntl) & ~0xffu) | kCntlRelease);
    rs.stage = 14; rsWr(io, rs, kCntl2, io.rd(io.c, kCntl2) & ~kSoftReset);
    rs.stage = 15;
    if (!rsPoll(io, kScratch0, 2, 2, 20, 1000000, &rs.bootUs)) return rsDead(io, st, rs, 15, "SCRATCH0 mailbox_rdy not set within 1 s: the firmware did not come back");
    rs.stage = 16;
    rsPost(io, rs);
    if (rs.post[1] != kFwVersion) return rsDead(io, st, rs, 16, "SCRATCH1 after the restart is not 0x07008742");
    if (rs.post[2] == kHaltMagic) return rsDead(io, st, rs, 16, "SCRATCH7 still 0xDEADDEAD after the boot");
    const uint32_t t0 = io.rd(io.c, kTimer);
    io.delayUs(io.c, 100);
    if (io.rd(io.c, kTimer) == t0) return rsDead(io, st, rs, 16, "DMCUB timer not moving");
    for (unsigned i = 0; i < 3; i++) if (io.rd(io.c, kFault[i]) != rs.fault[i]) return rsDead(io, st, rs, 16, "a DMCUB fault address register changed");
    uint32_t us = 0;
    if (!rsPoll(io, kCntl, kCntlPwait, kCntlPwait, kPollStepUs, 10000, &us)) return rsDead(io, st, rs, 16, "DMCUB not idle (CNTL bit 20) 10 ms after the boot");
    if (io.rd(io.c, kInbox1Wptr) || io.rd(io.c, kInbox1Rptr)) return rsDead(io, st, rs, 16, "INBOX1 WPTR / RPTR not 0 after the boot");
    {   // the new trace: one MAIN_BEGIN and the boot block (0x02, 4 x link, 0x03, 0x11 in order); recorded, not a condition
        const uint32_t wp = io.rd(io.c, kOutbox0Wptr);
        const unsigned m = traceEntries(t.size, wp), lim = m == ~0u ? 0 : (m < 64 ? m : 64);
        unsigned k = 0;
        for (unsigned i = 0; i < lim; i++) {
            const uint32_t c = io.mmRd(io.c, t.vram + 16u + 16u * i);
            rs.mainBeginPost += c == kTrMainBegin;
            if (k < sizeof kTrBoot / sizeof kTrBoot[0] && c == kTrBoot[k]) k++;
        }
        rs.bootBlock = k == sizeof kTrBoot / sizeof kTrBoot[0];
    }
    if (init) {
        rs.stage = 17;
        rs.initRc = submitInit(io, st, vramBytes, &w);
        rs.initUs = st.lastUs;
        if (rs.initRc != kOk) return rsDead(io, st, rs, 17, "the 0x0C post-start command was not consumed within 200 ms");
    }
    return rsEnd(rs, kRsOk, rs.stage, init ? "restarted, 0x0C consumed" : "restarted");
}

}  // namespace phxdmub
