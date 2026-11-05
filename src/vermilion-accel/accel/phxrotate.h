// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxrotate.h: a PhxAMDFB head turned to portrait (rotation S1), the pure
// pieces. Kernel-free: PhxAccel builds it (phxrotate_k.cpp, phxfb.cpp, nvrm-accel.cpp) and so does a host test
// (with mutation tests).
//
// How macOS rotates an accelerated IOFramebuffer display:
//   - CoreDisplay sends IOServiceRequestProbe(fb, kIOFBSetTransform | angle << 16) only when the framebuffer's
//     IOFBProbeOptions has kIOFBSetTransform (probeOptions below);
//   - on the connect change that follows, CoreDisplay's IOFBBuildModeList
//     turns each driver mode that has a detailed timing into a rotated one: the same raster, the desktop size swapped
//     for 90 / 270 (1080 x 1920), the kIOScale rotate flags in scalerFlags. Each one goes through the driver's
//     validateDetailedTiming (check below; it must write the portrait size into the mode info) and, with CoreDisplay's
//     ids (0x80000000 + a seed that changes on every rebuild), setDetailedTimings; the driver modes are disabled meanwhile;
//   - WindowServer then composites a portrait desktop and the driver scans it out rotated.
// S1 publishes no IOFBScalerInfo: with one, CoreDisplay takes its scale-base path instead, where the 1:1 portrait is a
// mirror-only entry (REA). Without one, a FAILED preflight dereferences the missing scaler info (IOGraphicsLib
// IOFBInstallScaledMode), so every detailed timing the head reports must pass check: rotatable() below guards that.
// S1 does no DCN rotation: the display pipe's copy (nvrm-accel.cpp performTransaction, head 1's path) writes the
// portrait frame into the landscape surface transposed, tile by tile (rotateFrame). The pipe, its plan, watermarks and
// DET are untouched. Only 1:1 rotated modes are accepted (no scaling in the copy).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "phxhead.h"

namespace phxrot {

// IOGraphicsTypes.h (MacKernelSDK) values, repeated so the host test needs no SDK; phxrotate_k.cpp static_asserts them.
constexpr uint32_t kScaleStretchToFit = 0x1, kScaleRotateFlags = 0xf0;
constexpr uint32_t kScaleSwapAxes = 0x10, kScaleInvertX = 0x20, kScaleInvertY = 0x40;
constexpr uint32_t kScaleCanUpSample = 0x2, kScaleCanDownSample = 0x4, kScaleCanRotate = 0x20;
constexpr uint32_t kProbeUserRequest = 0x1, kProbeSetTransform = 0x400;   // kIOFBUserRequestProbe, kIOFBSetTransform

enum : int { kRot0 = 0, kRot90 = 1, kRot180 = 2, kRot270 = 3, kRotBad = -1 };

// scalerFlags' rotate bits -> angle. The kIOScaleRotate* values are the only rotations; the other combinations are
// mirrors (swap only, one invert, swap + both inverts) and 0x80 is undefined: kRotBad.
constexpr int angleOf(uint32_t scalerFlags) {
    return (scalerFlags & kScaleRotateFlags) == 0 ? kRot0
         : (scalerFlags & kScaleRotateFlags) == (kScaleSwapAxes | kScaleInvertX) ? kRot90
         : (scalerFlags & kScaleRotateFlags) == (kScaleInvertX | kScaleInvertY) ? kRot180
         : (scalerFlags & kScaleRotateFlags) == (kScaleSwapAxes | kScaleInvertY) ? kRot270 : kRotBad;
}
constexpr uint32_t flagsOf(int a) {
    return a == kRot90 ? (kScaleSwapAxes | kScaleInvertX) : a == kRot180 ? (kScaleInvertX | kScaleInvertY)
         : a == kRot270 ? (kScaleSwapAxes | kScaleInvertY) : 0u;
}
constexpr bool swaps(int a) { return a == kRot90 || a == kRot270; }
constexpr bool invX(int a) { return (flagsOf(a) & kScaleInvertX) != 0; }
constexpr bool invY(int a) { return (flagsOf(a) & kScaleInvertY) != 0; }

// ---- the pixel mapping --------------------------------------------------------------------------------------------
// IOFramebuffer::transformLocation's order (IOGraphics IOFramebuffer.cpp): the inverts act on the desktop coordinates
// (InvertX: x = wd - 1 - x; InvertY: y = hd - 1 - y), then SwapAxes exchanges x and y. So at 90 (swap + invert X) the
// desktop's top-left pixel lands on the raster's bottom-left (0, wd - 1): the monitor is turned clockwise and the
// picture is drawn turned counter-clockwise on the raster. wd x hd = the desktop (portrait at 90 / 270).
inline void toRaster(int a, uint32_t wd, uint32_t hd, uint32_t xd, uint32_t yd, uint32_t *xr, uint32_t *yr) {
    const uint32_t x = invX(a) ? wd - 1 - xd : xd, y = invY(a) ? hd - 1 - yd : yd;
    if (swaps(a)) { *xr = y; *yr = x; } else { *xr = x; *yr = y; }
}
inline void toDesktop(int a, uint32_t wd, uint32_t hd, uint32_t xr, uint32_t yr, uint32_t *xd, uint32_t *yd) {
    const uint32_t x = swaps(a) ? yr : xr, y = swaps(a) ? xr : yr;
    *xd = invX(a) ? wd - 1 - x : x;
    *yd = invY(a) ? hd - 1 - y : y;
}
// The raster size of a wd x hd desktop.
inline void rasterSize(int a, uint32_t wd, uint32_t hd, uint32_t *w, uint32_t *h) {
    *w = swaps(a) ? hd : wd; *h = swaps(a) ? wd : hd;
}

// ---- the rotated copy ---------------------------------------------------------------------------------------------
// The portrait frame (src, wd x hd, srcPitch bytes per row) into the landscape raster (dst, dstPitch bytes per row) at
// angle a, ARGB2101010 narrowed to XRGB8888 when l10r (the display pipe's formula). Tile by tile: kTile rows of kTile
// pixels are read row by row (whole 16-byte pieces through Io::load16 when aligned, the streaming loads in the kernel:
// some display surfaces are write-combined), turned in a stack tile, and written as raster rows of up to kTile pixels
// (Io::store8 on 8-byte aligned pairs, Io::store4 otherwise: non-temporal stores in the kernel). Every raster pixel of
// the frame is written exactly once; nothing outside it (the pitch's padding) is touched.
// Io: load16(const uint8_t *p, uint32_t out[4]) (p 16-byte aligned), load4(const uint8_t *p) -> uint32_t,
//     store8(uint8_t *p, uint64_t v) (p 8-byte aligned), store4(uint8_t *p, uint32_t v), fence().
constexpr uint32_t kTile = 16;
constexpr uint32_t narrow(uint32_t v, bool l10r) {
    return l10r ? (0xff000000u | ((v >> 6) & 0xff0000u) | ((v >> 4) & 0xff00u) | ((v >> 2) & 0xffu)) : v;
}
template <class Io>
inline void rotateFrame(Io &io, const uint8_t *src, uint32_t srcPitch, uint32_t wd, uint32_t hd,
                        uint8_t *dst, uint32_t dstPitch, int a, bool l10r) {
    uint32_t tile[kTile * kTile] __attribute__((aligned(16)));
    uint32_t run[kTile] __attribute__((aligned(16)));
    // a +1 step along a raster row moves the source by step in the tile (row-major, kTile wide)
    const int step = swaps(a) ? (invY(a) ? -(int)kTile : (int)kTile) : (invX(a) ? -1 : 1);
    for (uint32_t ty = 0; ty < hd; ty += kTile) {
        const uint32_t th = hd - ty < kTile ? hd - ty : kTile;
        for (uint32_t tx = 0; tx < wd; tx += kTile) {
            const uint32_t tw = wd - tx < kTile ? wd - tx : kTile;
            for (uint32_t r = 0; r < th; r++) {
                const uint8_t *row = src + (uint64_t)(ty + r) * srcPitch + (uint64_t)tx * 4;
                uint32_t *t = tile + r * kTile, c = 0;
                if (!((uintptr_t)row & 15)) for (; c + 4 <= tw; c += 4) io.load16(row + c * 4, t + c);
                for (; c < tw; c++) t[c] = io.load4(row + c * 4);
                if (l10r) for (c = 0; c < tw; c++) t[c] = narrow(t[c], true);
            }
            // the tile's footprint on the raster: corners mapped, then sorted
            uint32_t ax, ay, bx, by;
            toRaster(a, wd, hd, tx, ty, &ax, &ay);
            toRaster(a, wd, hd, tx + tw - 1, ty + th - 1, &bx, &by);
            const uint32_t x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax, y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
            const uint32_t n = x1 - x0 + 1;
            for (uint32_t yr = y0; yr <= y1; yr++) {
                uint32_t xd, yd;
                toDesktop(a, wd, hd, x0, yr, &xd, &yd);
                int k = (int)((yd - ty) * kTile + (xd - tx));
                for (uint32_t i = 0; i < n; i++, k += step) run[i] = tile[k];
                uint8_t *p = dst + (uint64_t)yr * dstPitch + (uint64_t)x0 * 4;
                uint32_t i = 0;
                if ((uintptr_t)p & 7) { io.store4(p, run[0]); i = 1; }
                for (; i + 2 <= n; i += 2) io.store8(p + i * 4, (uint64_t)run[i] | (uint64_t)run[i + 1] << 32);
                if (i < n) io.store4(p + i * 4, run[i]);
            }
        }
    }
    io.fence();
}

// ---- the copy's geometry check (performTransaction) ---------------------------------------------------------------
// The scan-out describes the landscape surface (w x h). rot2 (box data 11-02 23:57): in a 90
// mode (1080 x 1920 desktop) WindowServer still presents RASTER-sized 1920 x 1080 surfaces, i.e. its compositor has
// already turned the desktop for the panel; rot1 refused them ("source 1920x1080 is not the scanout's ... angle 1") and
// the Acer froze until macOS reverted. So a raster-sized frame is copied straight at any angle; a frame of the turned
// size (a portrait surface, never seen yet) is still rotated here; 180 keeps the raster size and is rotated here only
// when rotate180 (debug.phxfb_rotate_opts bit 0: whether WindowServer also pre-turns 180 is settled on the box).
// 0 = straight copy, 1 = rotated copy, -1 = refuse (the sizes fit neither).
inline int copyKind(int a, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h, bool rotate180) {
    if (a < 0 || a > kRot270) return -1;
    if (sw == w && sh == h) return a == kRot180 && rotate180 ? 1 : 0;
    if (!swaps(a)) return -1;
    uint32_t rw, rh;
    rasterSize(a, sw, sh, &rw, &rh);
    return rw == w && rh == h ? 1 : -1;
}
// debug.phxfb_rotate_opts (rot2): bit 0 = the kernel turns raster-sized frames at 180 (rot1 behaviour; default off: the
// 180 box run 11-02 showed the cursor moving the right way with the kernel turning them, i.e. upright = turned twice [I]);
// bit 1 = a gated head flags its CURRENT mode as the default (default on), so CoreDisplay's turned twin of the running
// rate is the default WindowServer applies to a new display set ("no saved conf", box log 23:57:33: the Acer fell from
// 165 to the twin of the boot mode 144).
// bit 2 (rot4, default on) = while ANY head is gated, head 0 (the AOC, never rotated: gate bit 0 is refused) flags its
// current mode Default too. Box 11-03: at a new portrait set ("no saved conf") WindowServer applied the AOC's default,
// the boot mode 60. CAVEAT [F CoreDisplay 26.7.1 IOFBRebuild 0x7ff80535e66e-e74f + 0x7ff80535e7dc]: a Default-flagged
// mode scores +1, an UNflagged mode of the EDID-preferred size also +1, and a tie goes to the mode under 86 Hz, so with
// the AOC's preferred size recorded (InstallFromEDIDDesc 0x7ff80536a81c, likely) 60 still wins; the box check settles it
// (see bit 3 below).
// bit 3 (rot5, default on) = while ANY head is gated, head 0's listed modes other than its current one report a non-zero
// verticalBlankingExtension in their detailed timing, so CoreDisplay never picks them as head 0's default mode.
// Why bit 2 was not enough [F box 11-03 08:26, rot5 data]: at a portrait
// set IOGraphicsLib had merged the AOC's EDID-preferred DTD (60) into mode 1, which therefore carries Default|Native
// (IOFBConfig DM flags 0x2100007 with DF 3) next to mode 2's Default from bit 2: both score +1, and the tie goes to the
// mode under 86 Hz. IOFBRebuild [F CoreDisplay 26.7.1 0x7ff80535e5b2: cmpl $0x0, 0x84(TM)] lets a mode become the default
// only when its TM verticalBlankingExtension is 0, whatever its score. With only the current mode left, defaultMode =
// current, its CG flags carry 0x4, and CGXRestoreDisplayConfiguration's "no saved conf" path keeps it (0x7ff8053a2e59:
// it replaces a monitor's mode only when the current one lacks 0x4).
// bit 4 (rot7, default on) = a rotated description that validateDetailedTiming accepts carries
// kDisplayModeAcceleratorBackedFlag, so CoreDisplay turns the desktop on the GPU itself (DisplayPipe::Commit op mode 1)
// instead of handing the rotation to the hardware (mode 2: it then only CENTRES the unturned portrait desktop, the
// picture seen in rot6). [F CoreDisplay 26.7.1: Commit picks mode 2 when
// any mode has (flags & 0x400004) == 4; IOFBDriverPreflight copies 0x400000 back from our validate answer.] With it,
// WindowServer hands raster-sized frames already turned, which the copy takes straight (copyKind 0), so bit 0 must stay
// off (180 would be turned twice).
constexpr uint32_t kOptRotate180 = 1u << 0, kOptDefaultCurrent = 1u << 1, kOptDefaultCurrentH0 = 1u << 2,
                   kOptH0OnlyCurrent = 1u << 3, kOptAccelBacked = 1u << 4, kOptH1OnlyCurrent = 1u << 5,
                   kOptMarkOnlyRotated = 1u << 6;
// bit 6 (rot11, default on) = the extension marks of bits 3 and 5 only while some head's IOFBTransform has rotate bits.
// Box rot10 [F A/B]: with the marks System Settings shows the AOC's rates as "Variable (0-144 Hertz)" / "60
// Hertz" (macOS reads a non-zero verticalBlankingExtension as VRR); without them "144 Hertz" / "60 Hertz". The marks are
// only needed when CoreDisplay meets a rotated display set; IOFramebuffer sets IOFBTransform in processConnectChange
// (IOFramebuffer.cpp 7694) before CoreDisplay's rebuild, so the rebuild that follows a rotation still sees them, and the
// one that follows the return to Standard drops them.
constexpr uint32_t kRotateBits = 0x70u;   // kIOScaleRotateFlags in IOFBTransform (0x30 / 0x50 / 0x60)
// bit 5 (rot9, default on) = bit 3 for head 1: while ANY head is gated, head 1 reports its detailed timings (the EDID DTD
// of each listed plan, as when it is gated itself) and marks those of its non-current modes with the extension. Box
// rot6 / rot8: rotating the AOC alone made CoreDisplay's connect change re-mode the ungated Acer to its default
// (no detailed timing = no startMode match) and "no saved conf" kept that default, 60 (the same tie as head 0's).
// rot15: the extension marks (bits 3 and 5) are OFF by default. Box: whenever a mode carries a non-zero
// verticalBlankingExtension, System Settings may present the rates as "Variable (0-N Hertz)" (seen
// unrotated and rotated). The rate at a new display set is held instead by CoreDisplay's own
// tie-break: a display override with default-resolution 0 x 0 @ 255 Hz (written by the boot daemon, no .plist suffix,
// tools/phxctl/dispovr.sh) makes the highest rate win a same-size tie
// (IOFBRebuild 0x7ff80535e7dc; the override is read by __IODisplayCreateInfoDictionary). The marks stay available live.
constexpr uint32_t kOptsMarks = kOptH0OnlyCurrent | kOptH1OnlyCurrent;
constexpr uint32_t kOptsDefault = kOptDefaultCurrent | kOptDefaultCurrentH0 | kOptAccelBacked | kOptMarkOnlyRotated;
// The override's desired refresh (16.16): above every mode, so a same-size tie goes to the highest rate.
constexpr uint32_t kOverrideDesire = 255u << 16;
// The marks apply now: always without bit 6, else only while some head is rotated (IOFBTransform & kRotateBits).
constexpr bool marksNow(uint32_t opts, bool rotated) { return !(opts & kOptMarkOnlyRotated) || rotated; }
constexpr uint32_t kAccelBackedFlag = 0x00400000u;   // kDisplayModeAcceleratorBackedFlag (IOGraphicsTypes.h)
// The flags a validated rotated description returns: the caller's, plus AcceleratorBacked with opts bit 4.
constexpr uint32_t validatedFlags(uint32_t in, uint32_t opts) { return opts & kOptAccelBacked ? in | kAccelBackedFlag : in; }
// The flags getInformationForDisplayMode reports for a rotated id: Valid | Safe and NEVER AcceleratorBacked, because the
// kernel's IOAccelLegacyDisplayPipe::is_current_mode_accelerator_backed reads this answer and would switch
// displayModeDidChange to its scanout-resource path (rotation-pixels report 1f).
constexpr uint32_t infoFlags() { return 0x1u | 0x2u; }
// The verticalBlankingExtension head 0 reports for mode `id` (0 = none, as before): non-zero only for a mode other than the
// current one, on head 0, while a head is gated and opts bit 3 is set.
constexpr uint32_t kH0ExcludeVbe = 1;
constexpr uint32_t h0Vbe(unsigned head, uint32_t gateMask, uint32_t opts, uint32_t id, uint32_t curId, bool rotated = true) {
    return head == 0 && gateMask && (opts & kOptH0OnlyCurrent) && id != curId && marksNow(opts, rotated) ? kH0ExcludeVbe : 0u;
}
// Head 1 (rot9): its timings exist while it is gated itself, or while any head is gated with opts bit 5; the mark as h0Vbe.
constexpr bool h1Timings(bool headGated, uint32_t gateMask, uint32_t opts) {
    return headGated || (gateMask && (opts & kOptH1OnlyCurrent));
}
constexpr uint32_t h1Vbe(unsigned head, uint32_t gateMask, uint32_t opts, uint32_t id, uint32_t curId, bool rotated = true) {
    return head == 1 && gateMask && (opts & kOptH1OnlyCurrent) && id != curId && marksNow(opts, rotated) ? kH0ExcludeVbe : 0u;
}
// The id head `head` flags kDisplayModeDefaultFlag (gateMask = debug.phxfb_rotate): its boot mode, or its current one when
// the head is gated (bit 1), or for head 0 when any head is gated (bit 2).
constexpr uint32_t defaultId(unsigned head, uint32_t gateMask, uint32_t opts, uint32_t bootId, uint32_t curId) {
    return (head < 32 && ((gateMask >> head) & 1) && (opts & kOptDefaultCurrent)) ||
           (head == 0 && gateMask && (opts & kOptDefaultCurrentH0)) ? curId : bootId;
}
// The angle travels in the scan-out / flip-back reply (NVRMVramRequest.flags, zeroed by the caller: an older
// framebuffer leaves 0 = no rotation). Magic in the top half so a stray value is never read as an angle.
constexpr uint32_t kReplyMagic = 0x52540000u;   // 'RT'
constexpr uint32_t replyFlags(int a) { return a > kRot0 && a <= kRot270 ? kReplyMagic | (uint32_t)a : 0u; }
constexpr int replyAngle(uint32_t flags) {
    return (flags & 0xffff0000u) == kReplyMagic && (flags & 0xffffu) <= (uint32_t)kRot270 ? (int)(flags & 0xffffu) : kRot0;
}

// ---- rotated detailed timings (validateDetailedTiming / setDetailedTimings) ---------------------------------------
// The IODetailedTimingInformationV2 fields the checks read (phxrotate_k.cpp fills it from the SDK struct).
struct Timing {
    uint64_t pixelClock;
    uint32_t hActive, hBlank, vActive, vBlank;
    uint32_t hScaled, vScaled, hInset, vInset, borders, scalerFlags, signalConfig, id;
};
enum : int {
    kOk = 0,
    kWhyNotRotated = 1,   // no rotate flags: not ours (the head's own modes are the driver's list)
    kWhyMirror = 2,       // rotate bits that are no rotation
    kWhyFlags = 3,        // scaler flags besides the rotation and stretch-to-fit
    kWhyInset = 4,        // scaled insets or borders
    kWhyScaled = 5,       // a desktop that is not the raster turned (S1 copies 1:1, it does not scale)
    kWhyNoPlan = 6,       // the raster is none of the head's listed modes (size, totals, clock)
    kWhyInterlaced = 7,
    kWhyCount = 8
};
constexpr uint32_t kInterlaced = 0x4;   // kIOInterlacedCEATiming in signalConfig
// The raster of a listed id: phxhead's table, or (rot12) the one-mode head's own mode when `single` is given (head 0 with
// no mode list, e.g. the Acer alone on pipe 0: the GOP mode, built by singleMode from the EDID and OTG0's totals).
inline const phxhead::HeadMode *modeOf(unsigned head, uint32_t id, const phxhead::HeadMode *single) {
    return single ? (single->id == id ? single : nullptr) : phxhead::headMode(head, id);
}
// head modes: the ids the head lists (phxhead::headMode(head, id) gives each one's raster). *plan = the matching id.
inline int check(const Timing &t, unsigned head, const uint32_t *ids, unsigned n, uint32_t *plan,
                 const phxhead::HeadMode *single = nullptr) {
    const int a = angleOf(t.scalerFlags);
    if (a == kRotBad) return kWhyMirror;
    if (a == kRot0) return kWhyNotRotated;
    if (t.scalerFlags & ~(kScaleRotateFlags | kScaleStretchToFit)) return kWhyFlags;
    if (t.hInset || t.vInset || t.borders) return kWhyInset;
    if (t.signalConfig & kInterlaced) return kWhyInterlaced;
    if (swaps(a)) { if (t.hScaled != t.vActive || t.vScaled != t.hActive) return kWhyScaled; }
    else if (!((!t.hScaled && !t.vScaled) || (t.hScaled == t.hActive && t.vScaled == t.vActive))) return kWhyScaled;
    for (unsigned i = 0; i < n; i++) {
        const phxhead::HeadMode *m = modeOf(head, ids[i], single);
        if (!m || m->w != t.hActive || m->h != t.vActive) continue;
        if (m->w + t.hBlank != m->hTotal || m->h + t.vBlank != m->vTotal) continue;
        const uint64_t hz = (uint64_t)m->pclk10kHz * 10000ull, tol = hz / 500;   // 0.2 %, as phxhead::dtdIs
        if (t.pixelClock + tol < hz || t.pixelClock > hz + tol) continue;
        if (plan) *plan = m->id;
        return kOk;
    }
    return kWhyNoPlan;
}
// The desktop of an accepted timing (portrait at 90 / 270) and its row bytes: exactly 4 bytes a pixel, so a portrait
// frame takes the landscape surface's bytes (1080 x 4 x 1920 = 1920 x 4 x 1080) and never more.
inline void desktopOf(const Timing &t, uint32_t *w, uint32_t *h) {
    const bool s = swaps(angleOf(t.scalerFlags));
    *w = s ? t.vActive : t.hActive; *h = s ? t.hActive : t.vActive;
}
constexpr uint32_t rowBytes(uint32_t w) { return w * 4; }

// The modes CoreDisplay installed (setDetailedTimings), as accepted; ids are CoreDisplay's.
constexpr unsigned kMaxInstalled = 16;
struct Installed { uint32_t id, plan; int angle; Timing t; };
struct Table { Installed e[kMaxInstalled]; unsigned n; uint32_t skipped; };
inline const Installed *find(const Table &tb, uint32_t id) {
    for (unsigned i = 0; i < tb.n; i++) if (tb.e[i].id == id) return &tb.e[i];
    return nullptr;
}
// One timing of a setDetailedTimings array into the table: accepted and not colliding with the head's own ids or an
// entry already there. Returns the check's why (kOk = added); a full table counts as skipped.
inline int add(Table &tb, const Timing &t, unsigned head, const uint32_t *ids, unsigned n,
                const phxhead::HeadMode *single = nullptr) {
    uint32_t plan = 0;
    const int why = check(t, head, ids, n, &plan, single);
    bool clash = !t.id || find(tb, t.id) != nullptr;
    for (unsigned i = 0; i < n; i++) clash = clash || ids[i] == t.id;
    if (why != kOk || clash || tb.n >= kMaxInstalled) { tb.skipped++; return why != kOk ? why : kWhyCount; }
    tb.e[tb.n++] = Installed{t.id, plan, angleOf(t.scalerFlags), t};
    return kOk;
}

// ---- what the framebuffer publishes --------------------------------------------------------------------------------
// IOFBProbeOptions: kIOFBSetTransform is what CoreDisplay's _CGXSetDisplayRotation checks (else 1010 and no rotation);
// AMD's X6000FB publishes 1 | 0x400 on a rotation-capable head.
constexpr uint32_t probeOptions(uint32_t cur, bool on) {
    return on ? (cur | kProbeUserRequest | kProbeSetTransform) : (cur & ~kProbeSetTransform);
}
// debug.phxfb_rotate: a mask of heads (bit n = head n). S2 (rot8) allows bit 0 (head 0, the AOC) besides bit 1. With
// rot7's AcceleratorBacked flag CoreDisplay turns the desktop itself and hands raster-sized frames (seen in rot7 on the
// Acer), so head 0's plugin GPU present (a surface equal to the plane) and the kernel copy (copyKind 0) need no transpose.
// Head 0's own timings are F1's and rotatable at every angle (host-tested).
constexpr uint32_t kGateAllowed = (1u << 0) | (1u << 1);
// rot10: the gate's value at kext start = every allowed head, so rotation is offered in System Settings with no command and
// a rotation the kernel re-applies at boot from its stored framebuffer-rotation pref (IOFramebuffer getTransformPrefs at
// the IODisplay attach, before any daemon) finds the rotated modes validated by us. With the gate at 0 and a stored
// rotation, IOFramebuffer's own validateDetailedTiming refuses the twins and CoreDisplay 26.7.1 IOFBInstallScaledMode
// reads a NULL scaler info (0x7ff8053631a5) [F code, I box]. debug.phxfb_rotate=0 still turns it off (EBUSY while rotated).
constexpr uint32_t kGateDefault = kGateAllowed;
// rot14: the id getCurrentDisplayMode answers. A rotated mode is current only while its plan is still the raster the head
// runs: head0Resync (aocboot puts pipe 0 at 144 before the daemon's restart) moves fModeId without a setDisplayMode, and the
// stale twin id (the first WindowServer's 60 twin, the only one it was offered) then made the restarted WindowServer
// re-apply that twin = plan 1 = a switch back to 60 (box data, rot13: 'head 0 mode 0x80001000: angle 1 (plan 1)' and
// 'head 0 switch to id 1' right after the restart, with the startup-timing pref still the 144 twin). With the plain id the
// restarted WindowServer takes its startMode / default, the twin of the running plan.
constexpr uint32_t currentId(bool rotValid, uint32_t rotId, uint32_t rotPlan, uint32_t modeId) {
    return rotValid && rotPlan == modeId ? rotId : modeId;
}
constexpr bool gateOk(uint32_t mask) { return (mask & ~kGateAllowed) == 0; }

// ---- a head's own modes as detailed timings ----------------------------------------------------------------------
// CoreDisplay builds the rotated modes from the driver modes that have a detailed timing. Head 0 has them (F1,
// phxhead::detailedTiming); head 1's come from the EDID it serves: the DTD of each listed plan, decoded the way
// IOGraphicsLib's EDIDDescToDetailedTiming does (clock in Hz, sync offset = front porch, polarity from the flags of a
// digital separate-sync DTD), so IOFBInstallMode never hides it as an EDID mode with another sync.
inline bool dtdDecode(const uint8_t *d, phxhead::DetailedTiming *out) {
    const uint32_t pc = d[0] | (uint32_t)d[1] << 8;
    if (!pc || !out) return false;
    const uint32_t ha = d[2] | (uint32_t)(d[4] >> 4) << 8, hb = d[3] | (uint32_t)(d[4] & 15) << 8;
    const uint32_t va = d[5] | (uint32_t)(d[7] >> 4) << 8, vb = d[6] | (uint32_t)(d[7] & 15) << 8;
    const uint32_t hso = d[8] | (uint32_t)((d[11] >> 6) & 3) << 8, hsw = d[9] | (uint32_t)((d[11] >> 4) & 3) << 8;
    const uint32_t vso = (d[10] >> 4) | (uint32_t)((d[11] >> 2) & 3) << 4, vsw = (d[10] & 15) | (uint32_t)(d[11] & 3) << 4;
    const bool sep = (d[17] & 0x18) == 0x18;
    *out = {(uint64_t)pc * 10000ull, ha, hb, hso, hsw, va, vb, vso, vsw, sep && (d[17] & 2), sep && (d[17] & 4)};
    return true;
}
// A head mode's detailed timing turned by angle a the way CoreDisplay's UpdateTimingInfoForTransform does it (raster
// and blanking unchanged, the desktop = the raster with its sides swapped for 90 / 270, the rotate flags ORed in).
inline Timing turned(const phxhead::DetailedTiming &d, int a, uint32_t id) {
    Timing t{};
    t.pixelClock = d.pixelHz; t.hActive = d.hActive; t.hBlank = d.hBlank; t.vActive = d.vActive; t.vBlank = d.vBlank;
    t.hScaled = swaps(a) ? d.vActive : d.hActive; t.vScaled = swaps(a) ? d.hActive : d.vActive;
    t.scalerFlags = flagsOf(a); t.id = id;
    return t;
}
// A detailed timing the head may report: all three turned versions pass check (CoreDisplay would preflight them).
inline bool rotatable(const phxhead::DetailedTiming &d, unsigned head, const uint32_t *ids, unsigned n,
                      const phxhead::HeadMode *single = nullptr) {
    for (int a = kRot90; a <= kRot270; a++) if (check(turned(d, a, 0), head, ids, n, nullptr, single) != kOk) return false;
    return true;
}
inline bool edidTiming(const uint8_t *e, uint32_t len, const phxhead::HeadMode &m, phxhead::DetailedTiming *out) {
    static const uint8_t kHdr[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (!e || len < 128) return false;
    for (unsigned i = 0; i < 8; i++) if (e[i] != kHdr[i]) return false;
    for (unsigned o = 54; o <= 108; o += 18) if (phxhead::dtdIs(e + o, m)) return dtdDecode(e + o, out);
    for (uint32_t b = 128; b + 128 <= len; b += 128) {
        const uint8_t *x = e + b;
        if (x[0] != 0x02 || x[2] < 4 || x[2] > 127) continue;
        for (unsigned o = x[2]; o + 18 <= 127; o += 18) { if (!x[o] && !x[o + 1]) break; if (phxhead::dtdIs(x + o, m)) return dtdDecode(x + o, out); }
    }
    return false;
}
// rot12: head 0 with no mode list (fNModes 0: the Acer alone on pipe 0, or the AOC before its list is armed) rotates its
// one GOP mode: the EDID DTD whose active size and totals are OTG0's, as id 1. False when the EDID has none or the
// timing would not pass check at every angle (then the head reports no timing and offers no rotation, as before).
inline bool singleMode(const uint8_t *e, uint32_t len, uint32_t w, uint32_t h, uint32_t hTotal, uint32_t vTotal,
                       phxhead::HeadMode *m, phxhead::DetailedTiming *t) {
    static const uint8_t kHdr[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (!e || len < 128 || !m || !t || !w || !h || !hTotal || !vTotal) return false;
    for (unsigned i = 0; i < 8; i++) if (e[i] != kHdr[i]) return false;
    auto try1 = [&](const uint8_t *d) -> bool {
        phxhead::DetailedTiming x;
        if (!dtdDecode(d, &x) || x.hActive != w || x.vActive != h) return false;
        if (x.hActive + x.hBlank != hTotal || x.vActive + x.vBlank != vTotal) return false;
        const uint64_t px = (uint64_t)hTotal * vTotal;
        const phxhead::HeadMode c{1, w, h, (uint32_t)(x.pixelHz * 1000ull / px), (uint32_t)(x.pixelHz / 10000ull), hTotal, vTotal};
        const uint32_t one = 1;
        if (!rotatable(x, 0, &one, 1, &c)) return false;
        *m = c; *t = x;
        return true;
    };
    for (unsigned o = 54; o <= 108; o += 18) if (try1(e + o)) return true;
    for (uint32_t b = 128; b + 128 <= len; b += 128) {
        const uint8_t *x = e + b;
        if (x[0] != 0x02 || x[2] < 4 || x[2] > 127) continue;
        for (unsigned o = x[2]; o + 18 <= 127; o += 18) { if (!x[o] && !x[o + 1]) break; if (try1(x + o)) return true; }
    }
    return false;
}

}  // namespace phxrot
