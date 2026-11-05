// SPDX-License-Identifier: MIT
// PhxGamma.h: kext b98 head-1 gamma (Apple's fade, ColorSync calibration, Night Shift) on pipe 1's MPC output gamma,
// MPCC_OGAM1.
// Kernel-free (stdint only, no floating point); the kext glue is phx_gamma.cpp; host-tested
// (bit-exact against a port of the Linux v6.8 math, plus a register model).
//
// The table IOFramebuffer hands PhxAMDFB (3 x 256 UInt16, planar R, G, B; IOFBGammaCount 256 / IOFBGammaWidth 16) becomes
// the Linux SDR regamma PWL of cm3_helper_translate_curve_to_hw_format (dcn30_cm_common.c): 11 regions from 2^-10 to 2^1,
// segments {3, 4 x 9, 1} = 154 points + the end point at 2.0 = 155 LUT_DATA words per channel, each an unsigned 18-bit
// custom float (6-bit exponent, bias 31, 12-bit mantissa; custom_float.c). The OGAM input is the encoded framebuffer
// value (pipe 1's DPP CM is bypassed, CM1_CM_CONTROL = 1): x = code / 255, so point p takes the table at x_p, linear
// between entries. Above 1.0 (only the 1.5 and 2.0 points, never reached by an 8-bit code) it holds entry 255; Linux's
// user-ramp mapping returns 1.0 there instead (calculate_mapped_value, RIGHT position), which would brighten a fade.
//
// Apply (one update, phxPipe2SetGamma): CONTROL read; a pending latch (MODE != MODE_CURRENT, or SELECT != SELECT_CURRENT
// in RAM mode) = Busy with zero writes (writing then would hit the bank going live); the bank = the inverse of
// SELECT_CURRENT, A when bypassed; MEM_PWR_DIS + at most 100 us for MEM_PWR_STATE 0, else fail closed; LUT_CONTROL,
// LUT_INDEX, the bank's START/SLOPE/END/REGION registers (read back), the data; ONE final CONTROL = MODE 2 | SELECT bank
// (Linux's interim REG_SET MODE 2 with SELECT 0 is skipped: without a pipe lock it would flip to A mid-update). Bypass =
// one CONTROL = 0. Every write is on writable() (OGAM1 CONTROL..RAMB_REGION_32_33 + MPCC1_MPCC_MEM_PWR_CTRL; the glue also
// requires phxp2::writeAllowed); the pipe-0 check runs before (refuse, nothing written) and after (halt).
#pragma once
#include <stdint.h>

// The ABI between PhxAccel (PhxAMDFB head 1, phxfb.cpp) and PhxBringup's platform function "phxPipe2SetGamma"
// (param1 = struct phx_pipe2_gamma *; PhxBringup b98+ marks itself with the property phx-pipe2-gamma-fn).
// kIOReturnBusy = not applied (a flip still pending or the pipe-1 engine busy): the caller retries with its latest table.
#define PHX_PIPE2_GAMMA_VERSION 1u
struct phx_pipe2_gamma {
    uint32_t version;          // in: PHX_PIPE2_GAMMA_VERSION
    uint32_t mode;             // in: phxgamma::kModeBypass / kModeAuto (identity within 1/1024 = bypass) / kModeTable
    uint16_t table[3][256];    // in: R, G, B, 0..65535 (full scale 65535)
    int32_t result;            // out: phxgamma::Res
    uint32_t bank;             // out: the bank written (0 A, 1 B)
    uint32_t control;          // out: MPCC_OGAM_CONTROL after the call
    char why[64];              // out: NUL-terminated
};
#ifdef __cplusplus
static_assert(sizeof(struct phx_pipe2_gamma) == 1620, "phx_pipe2_gamma is 1620 B");
#endif

namespace phxgamma {

enum Mode { kModeBypass = 0, kModeAuto = 1, kModeTable = 2 };
// kOff: debug.phxgamma=0, the table is only kept; kNotLit: pipe 1 is not lit by the ladder (or halted / failed / switching);
// kRefused: the pipe-0 check failed before any write; kFailed: power wait / read-back / allow-list, nothing flipped;
// kHalted: pipe 0 differed after our writes (the glue latches phxp2 halted).
enum Res { kApplied = 0, kBypass = 1, kSame = 2, kOff = 3, kBusy = 4, kNotLit = 5, kRefused = 6, kFailed = 7, kHalted = 8 };
static const char *const kResName[] = {"applied", "bypass", "same", "off", "busy", "not lit", "refused", "FAILED", "HALTED"};

// MPCC_OGAM1 (dcn_3_2_0_offset.h regMPCC_OGAM1_*, BASE_IDX 3 = 0x9000) and MPCC1_MPCC_MEM_PWR_CTRL
static constexpr uint32_t kControl = 0x09106, kLutIndex = 0x09107, kLutData = 0x09108, kLutControl = 0x09109,
                          kRamA = 0x0910A, kRamB = 0x0912D, kRamBEnd = 0x0914F, kMemPwr = 0x09022, kLockSel = 0x0901A;
// Bank layout from kRamA / kRamB: START_CNTL B,G,R +0..2, START_SLOPE_CNTL B,G,R +3..5, START_BASE_CNTL +6..8 (not written,
// as Linux), END_CNTL1/END_CNTL2 B +9/+10, G +11/+12, R +13/+14, OFFSET +15..17 (not written), REGION_0_1..32_33 +18..34
static constexpr uint32_t kOffStart = 0, kOffSlope = 3, kOffEnd = 9, kOffRegion = 18, kNRegionRegs = 17;
static constexpr uint32_t kModeMask = 0x3, kSelectShift = 2, kModeCurShift = 7, kSelCurShift = 9;   // CONTROL fields
static constexpr uint32_t kHostSel = 0x40, kColorMask = 0x7;                                          // LUT_CONTROL fields
static constexpr uint32_t kPwrDis = 0x4, kPwrStateShift = 8;                                          // MEM_PWR_CTRL fields
static constexpr uint32_t kPwrWaitUs = 100, kPwrStepUs = 10;

inline bool writable(uint32_t reg) { return (reg >= kControl && reg <= kRamBEnd) || reg == kMemPwr; }

// ---- the curve
static constexpr unsigned kN = 256, kPoints = 155, kRegions = 11;
static const uint8_t kSegDistr[kRegions] = {3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1};   // seg_distr, SDR branch
static constexpr int kRegionStart = -10;
static constexpr uint64_t kOne = 1ull << 32;   // values carry 32 fractional bits (Linux fixed31_32)

// x of hardware point p (0..154) = Linux coordinates_x at the index cm3_helper samples: region k holds 2^seg points
// 2^(k-10) * (1 + m / 2^seg); p = 154 is the end point 2.0.
inline uint64_t pointX(unsigned p) {
    unsigned k = 0;
    while (k < kRegions && p >= (1u << kSegDistr[k])) { p -= 1u << kSegDistr[k]; k++; }
    if (k == kRegions) return 2 * kOne;
    const unsigned s = kSegDistr[k];
    return (static_cast<uint64_t>((1u << s) + p) << 32 >> s) >> (10 - k);   // (2^s + m) / 2^s * 2^(k-10)
}
// The channel's table at x (32 fractional bits), linear between entries i / 255, held at entry 255 from 1.0, as a
// fraction of 65535 rounded to nearest.
inline uint64_t evalAt(const uint16_t *t, uint64_t x) {
    if (x >= kOne) return (static_cast<uint64_t>(t[kN - 1]) * kOne + 32767) / 65535;
    const uint64_t pos = x * (kN - 1);
    const unsigned i = static_cast<unsigned>(pos >> 32);
    const uint64_t f = pos & (kOne - 1);
    const uint64_t num = static_cast<uint64_t>(t[i]) * (kOne - f) + static_cast<uint64_t>(t[i + 1]) * f;
    return (num + 32767) / 65535;
}
// Unsigned custom float of v (32 fractional bits): Linux build_custom_float + setup_custom_float with exponenta_bits 6,
// sign false: exponent bias 31, mantissa truncated to mb bits, only the low 6 exponent bits kept; 0 (and < 2^-30) = 0.
static constexpr uint32_t kExpBias = 31;
inline uint32_t cfloat(uint64_t v, unsigned mb = 12) {
    if (!v) return 0;
    uint32_t e;
    const uint64_t maxFrac = ((1ull << (mb + 1)) - 1) << (32 - mb);   // (2^(mb+1) - 1) / 2^mb
    if (v < kOne) {
        uint32_t i = 1;
        do { v <<= 1; ++i; } while (v < kOne);
        --i;
        if (kExpBias <= i) return 0;
        e = kExpBias - i;
    } else if (v >= maxFrac) {
        uint32_t i = 1;
        do { v >>= 1; ++i; } while (v > maxFrac);
        e = kExpBias + i - 1;
    } else {
        e = kExpBias;
    }
    const uint32_t m = (v >= kOne && v - kOne <= kOne) ? static_cast<uint32_t>(((v - kOne) << mb) >> 32) : 0;
    return (m & ((1u << mb) - 1)) | (e & 0x3f) << mb;
}

struct Pwl {
    uint32_t data[3][kPoints];   // R, G, B LUT_DATA words
    uint32_t start[3], slope[3], endBase[3], endX, endSlope;   // per channel R, G, B
    uint32_t region[kNRegionRegs];
    bool equal;                  // every word equal across R, G, B (Linux is_rgb_equal): one pass with mask 7
};
// The Linux arr_curve_points: region k's LUT offset and segment count (k >= 11: offset of the end point, then 0)
inline void regionCurve(unsigned k, uint32_t *off, uint32_t *seg) {
    uint32_t o = 0;
    for (unsigned i = 0; i < k && i < kRegions; i++) o += 1u << kSegDistr[i];
    *off = k <= kRegions ? o : 0;
    *seg = k < kRegions ? kSegDistr[k] : 0;
}
inline void build(const uint16_t t[3][kN], Pwl &out) {
    out.equal = true;
    for (unsigned c = 0; c < 3; c++) {
        uint64_t y0 = 0, yEnd = 0;
        for (unsigned p = 0; p < kPoints; p++) {
            const uint64_t y = evalAt(t[c], pointX(p));
            if (p == 0) y0 = y;
            if (p == kPoints - 1) yEnd = y;
            out.data[c][p] = cfloat(y);
        }
        out.start[c] = cfloat(pointX(0));
        out.slope[c] = cfloat(y0 << 10);   // y0 / 2^-10 (Linux corner_points[0].slope, exact)
        out.endBase[c] = cfloat(yEnd);
    }
    out.endX = cfloat(pointX(kPoints - 1), 10);
    out.endSlope = cfloat(0, 10);
    for (unsigned p = 0; p < kPoints; p++)
        if (out.data[0][p] != out.data[1][p] || out.data[2][p] != out.data[0][p]) { out.equal = false; break; }
    for (unsigned r = 0; r < kNRegionRegs; r++) {
        uint32_t o0, s0, o1, s1;
        regionCurve(2 * r, &o0, &s0);
        regionCurve(2 * r + 1, &o1, &s1);
        out.region[r] = (o0 & 0x1ff) | (s0 & 7) << 12 | (o1 & 0x1ff) << 16 | (s1 & 7) << 28;
    }
}
// Identity within 1/1024 of full scale on every entry of every channel (65535 / 1024 = 64): the Linux default, bypass.
static constexpr uint32_t kIdentityTol = 64;
inline bool isIdentity(const uint16_t t[3][kN]) {
    for (unsigned c = 0; c < 3; c++)
        for (unsigned i = 0; i < kN; i++) {
            const int32_t d = static_cast<int32_t>(t[c][i]) - static_cast<int32_t>(i * 257);
            if (d > static_cast<int32_t>(kIdentityTol) || d < -static_cast<int32_t>(kIdentityTol)) return false;
        }
    return true;
}
// IOFramebuffer's table (updateGammaTable output: planar R, G, B; 1 byte per entry for width <= 8, else 2) -> 3 x 256
// 16-bit entries, linear resampling when count != 256. False = a shape IOFB never sends.
inline bool resample(const void *data, uint32_t channels, uint32_t count, uint32_t width, uint16_t out[3][kN]) {
    if (!data || channels != 3 || count < 2 || count > 1024 || width < 1 || width > 16) return false;
    const uint8_t *b8 = static_cast<const uint8_t *>(data);
    const uint16_t *b16 = static_cast<const uint16_t *>(data);
    const uint32_t full = (1u << width) - 1;
    for (unsigned c = 0; c < 3; c++)
        for (unsigned i = 0; i < kN; i++) {   // entry i sits at i * (count - 1) / 255 of the input: exact rational weights
            const uint32_t num = i * (count - 1), j = num / (kN - 1), r = num % (kN - 1), j1 = j + 1 < count ? j + 1 : j;
            const uint32_t a = width <= 8 ? b8[c * count + j] : b16[c * count + j];
            const uint32_t b = width <= 8 ? b8[c * count + j1] : b16[c * count + j1];
            const uint64_t v = (static_cast<uint64_t>(a) * (kN - 1 - r) + static_cast<uint64_t>(b) * r) * 65535;
            const uint64_t d = static_cast<uint64_t>(kN - 1) * full, q = (v + d / 2) / d;
            out[c][i] = static_cast<uint16_t>(q > 65535 ? 65535 : q);
        }
    return true;
}

// ---- the register image of one bank (Linux mpc3_set_output_gamma order: configure, program_lut, program_ogam_pwl)
// rmw != ~0: REG_UPDATE (read-modify-write of those bits), else REG_SET (the whole register, other fields 0);
// rb != 0: compared under rb after the bank is written.
struct W { uint32_t reg, val, rmw, rb; };
static constexpr unsigned kMaxW = 512;
struct Image { W w[kMaxW]; unsigned n; uint32_t bank; };
inline void put(Image &im, uint32_t reg, uint32_t val, uint32_t rmw = ~0u, uint32_t rb = 0) {
    if (im.n < kMaxW) im.w[im.n++] = {reg, val & rmw, rmw, rb};
}
inline void image(const Pwl &p, uint32_t bank, Image &im) {
    im.n = 0; im.bank = bank & 1;
    const uint32_t base = im.bank ? kRamB : kRamA;
    put(im, kLutControl, kColorMask | (im.bank ? kHostSel : 0), kColorMask | kHostSel);   // WRITE_COLOR_MASK 7, HOST_SEL
    put(im, kLutIndex, 0);
    static const unsigned bgr[3] = {2, 1, 0};   // the registers run B, G, R; the Pwl arrays R, G, B
    for (unsigned k = 0; k < 3; k++) put(im, base + kOffStart + k, p.start[bgr[k]] & 0x3ffff, ~0u, 0x07f3ffffu);   // START_SEGMENT 0
    for (unsigned k = 0; k < 3; k++) put(im, base + kOffSlope + k, p.slope[bgr[k]] & 0x3ffff, ~0u, 0x3ffff);
    for (unsigned k = 0; k < 3; k++) {
        put(im, base + kOffEnd + 2 * k, p.endBase[bgr[k]] & 0x3ffff, ~0u, 0x3ffff);
        put(im, base + kOffEnd + 2 * k + 1, (p.endSlope & 0xffff) << 16 | (p.endX & 0xffff), ~0u, ~0u);
    }
    for (unsigned r = 0; r < kNRegionRegs; r++) put(im, base + kOffRegion + r, p.region[r] & 0x71ff71ffu, ~0u, 0x71ff71ffu);
    if (p.equal) {
        for (unsigned i = 0; i < kPoints; i++) put(im, kLutData, p.data[0][i] & 0x3ffff);
        return;
    }
    static const uint32_t colorMask[3] = {4, 2, 1};   // R, G, B
    for (unsigned c = 0; c < 3; c++) {
        if (c) put(im, kLutIndex, 0);
        put(im, kLutControl, colorMask[c], kColorMask);
        for (unsigned i = 0; i < kPoints; i++) put(im, kLutData, p.data[c][i] & 0x3ffff);
    }
}

// ---- apply
struct Io {
    void *c;
    uint32_t (*rd)(void *c, uint32_t reg);
    void (*wr)(void *c, uint32_t reg, uint32_t v);
    void (*delayUs)(void *c, uint32_t us);
    bool (*p0Same)(void *c, int after, const char **why);   // pipe 0 unchanged: before (after = 0) and after our writes
};
struct Stats {
    uint32_t applied, bypass, same, busy, refused, failed, halted, pwrFail, rbFail, writes;
    uint32_t pwrWaitUs, pwrWaitMax, lastWrites, lastCtlBefore, lastCtlAfter, lastBank, lastRbReg;
    int lastRes;
};
inline bool pending(uint32_t ctl) {
    const uint32_t mode = ctl & kModeMask, cur = (ctl >> kModeCurShift) & 3;
    return mode != cur || (mode == 2 && ((ctl >> kSelectShift) & 1) != ((ctl >> kSelCurShift) & 1));
}
inline uint32_t nextBank(uint32_t ctl) { return ((ctl >> kModeCurShift) & 3) == 2 ? !((ctl >> kSelCurShift) & 1) : 0; }

inline int end(Stats &st, int res, const char *what, const char **why) {
    st.lastRes = res;
    if (why) *why = what;
    switch (res) {
    case kApplied: st.applied++; break;
    case kBypass: st.bypass++; break;
    case kSame: st.same++; break;
    case kBusy: st.busy++; break;
    case kRefused: st.refused++; break;
    case kFailed: st.failed++; break;
    case kHalted: st.halted++; break;
    default: break;
    }
    return res;
}
inline void wr(const Io &io, Stats &st, uint32_t reg, uint32_t v) { io.wr(io.c, reg, v); st.writes++; st.lastWrites++; }
inline int afterWrites(const Io &io, Stats &st, int res, const char *what, const char **why) {
    st.lastCtlAfter = io.rd(io.c, kControl);
    const char *w = nullptr;
    if (!io.p0Same(io.c, 1, &w)) return end(st, kHalted, w ? w : "pipe 0 changed after the gamma writes", why);
    return end(st, res, what, why);
}
// p = null: bypass. im: the caller's buffer (too large for a kernel stack).
inline int apply(const Io &io, Stats &st, const Pwl *p, Image &im, const char **why) {
    st.lastWrites = 0;
    const uint32_t ctl = io.rd(io.c, kControl);
    st.lastCtlBefore = st.lastCtlAfter = ctl;
    const char *w = nullptr;
    if (!io.p0Same(io.c, 0, &w)) return end(st, kRefused, w ? w : "pipe 0 differs before the gamma write", why);
    if (!p) {
        if ((ctl & kModeMask) == 0 && ((ctl >> kModeCurShift) & 3) == 0) return end(st, kSame, "already bypassed", why);
        wr(io, st, kControl, 0);
        return afterWrites(io, st, kBypass, "bypass", why);
    }
    if (pending(ctl)) return end(st, kBusy, "a CONTROL latch is still pending", why);
    const uint32_t bank = nextBank(ctl);
    st.lastBank = bank;
    image(*p, bank, im);
    for (unsigned i = 0; i < im.n; i++)
        if (!writable(im.w[i].reg)) return end(st, kFailed, "image write outside MPCC_OGAM1 (nothing written)", why);
    const uint32_t pw = io.rd(io.c, kMemPwr);
    if (!(pw & kPwrDis)) wr(io, st, kMemPwr, pw | kPwrDis);
    uint32_t us = 0;
    while ((io.rd(io.c, kMemPwr) >> kPwrStateShift) & 3) {
        if (us >= kPwrWaitUs) { st.pwrFail++; return afterWrites(io, st, kFailed, "MEM_PWR_STATE not 0 after 100 us (no LUT write)", why); }
        io.delayUs(io.c, kPwrStepUs);
        us += kPwrStepUs;
    }
    st.pwrWaitUs = us;
    if (us > st.pwrWaitMax) st.pwrWaitMax = us;
    for (unsigned i = 0; i < im.n; i++) {
        const W &x = im.w[i];
        wr(io, st, x.reg, x.rmw == ~0u ? x.val : (io.rd(io.c, x.reg) & ~x.rmw) | x.val);
    }
    for (unsigned i = 0; i < im.n; i++) {
        const W &x = im.w[i];
        if (x.rb && ((io.rd(io.c, x.reg) ^ x.val) & x.rb)) {
            st.rbFail++; st.lastRbReg = x.reg;
            return afterWrites(io, st, kFailed, "a bank register did not read back (not selected)", why);
        }
    }
    wr(io, st, kControl, 2u | bank << kSelectShift);
    return afterWrites(io, st, kApplied, bank ? "bank B written, SELECT B" : "bank A written, SELECT A", why);
}

}  // namespace phxgamma
