// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxrotate_k.cpp: the kernel side of a PhxAMDFB head's rotation (rotation S1).
// Pure rules: phxrotate.h (host-tested, with mutation tests). Published:
//   debug.phxfb_rotate        the gate, a mask of heads (bit 1 = head 1; S1 refuses bit 0). Root write; turning a head off
//                             while one of its rotated modes is on it is refused (EBUSY). Default 0 = all as before.
//   debug.phxfb_rotate_stat   one line: gate, per head the rotated mode on it / angle / plan / installed, the
//                             validateDetailedTiming verdicts by reason, the rotated copies and their mean microseconds
// Installed with head 0's framebuffer (phxfb.cpp start / stop), like phxsurfmap_k.cpp.
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include "phxrotate_k.h"

#ifndef detailedTimingModeID
#define detailedTimingModeID __reservedA[0]   // IOGraphicsTypesPrivate.h
#endif

static_assert(phxrot::kScaleStretchToFit == kIOScaleStretchToFit && phxrot::kScaleRotateFlags == kIOScaleRotateFlags &&
              phxrot::kScaleSwapAxes == kIOScaleSwapAxes && phxrot::kScaleInvertX == kIOScaleInvertX &&
              phxrot::kScaleInvertY == kIOScaleInvertY, "kIOScale* rotate flags");
static_assert(phxrot::kScaleCanRotate == kIOScaleCanRotate && phxrot::kScaleCanUpSample == kIOScaleCanUpSamplePixels &&
              phxrot::kScaleCanDownSample == kIOScaleCanDownSamplePixels, "kIOScaleCan* scaler features");
static_assert(sizeof(IOFBDisplayModeDescription) == 0xcc, "CoreDisplay 26.7.1 IOFBDriverPreflight sends 0xcc bytes (REA 1c)");
static_assert(phxrot::kProbeUserRequest == kIOFBUserRequestProbe && phxrot::kInterlaced == kIOInterlacedCEATiming, "probe / signal");

unsigned phxFbCopyHeads(IOService **out, unsigned max);   // phxfb.cpp

static int gRotGate = (int)phxrot::kGateDefault;            // debug.phxfb_rotate (rot10: on from kext start)
static int gRotOpts = (int)phxrot::kOptsDefault;           // debug.phxfb_rotate_opts (rot2)
static SInt64 gRotAccel;                                   // rotated descriptions answered AcceleratorBacked (rot7)
static SInt64 gRotStraight;                                // frames copied straight at a non-zero angle (WindowServer turned them)
static volatile int gRotAngle[phxhead::kMaxHeads];         // the angle on each head (phxRotSelect), for the EBUSY rule
static volatile uint32_t gRotCurId[phxhead::kMaxHeads], gRotPlanId[phxhead::kMaxHeads], gRotInstalledN[phxhead::kMaxHeads];
static SInt64 gRotWhy[phxrot::kWhyCount], gRotBadSize, gRotSets, gRotCopies, gRotCopyUs, gRotRefusedN;
static bool gRotInstalled;
static bool gRotPublished[phxhead::kMaxHeads];             // phxRotPublish set IOFBProbeOptions on this head
static bool gRotHadOpt[phxhead::kMaxHeads];                // ... and what it was before (restored when the gate goes off)
static uint32_t gRotOrigOpt[phxhead::kMaxHeads];

bool phxRotGate(unsigned head) { return head < 32 && (__atomic_load_n(&gRotGate, __ATOMIC_ACQUIRE) >> head) & 1; }

static void toTiming(const IODetailedTimingInformationV2 &d, phxrot::Timing *t)
{
    *t = phxrot::Timing{d.pixelClock, d.horizontalActive, d.horizontalBlanking, d.verticalActive, d.verticalBlanking,
                        d.horizontalScaled, d.verticalScaled, d.horizontalScaledInset, d.verticalScaledInset,
                        d.horizontalBorderLeft | d.horizontalBorderRight | d.verticalBorderTop | d.verticalBorderBottom,
                        d.scalerFlags, d.signalConfig, d.detailedTimingModeID};
}

// validateDetailedTiming: the description is an IOFBDisplayModeDescription (IOGraphicsLib's preflight) or a bare
// IODetailedTimingInformationV2. Only 1:1 rotated timings of a listed raster pass (phxrot::check); the mode info of an
// accepted description is filled with the portrait desktop (NullMoth nvrm-fb.cpp does the same for its scaled modes).
IOReturn phxRotValidate(const PhxRotModes &m, void *desc, IOByteCount size)
{
    if (!desc) return kIOReturnBadArgument;
    IODetailedTimingInformationV2 *d = nullptr;
    IODisplayModeInformation *info = nullptr;
    if (size == sizeof(IOFBDisplayModeDescription)) {
        IOFBDisplayModeDescription *md = (IOFBDisplayModeDescription *)desc;
        d = &md->timingInfo.detailedInfo.v2; info = &md->info;
    } else if (size == sizeof(IODetailedTimingInformationV2)) d = (IODetailedTimingInformationV2 *)desc;
    if (!d) { OSAddAtomic64(1, &gRotBadSize); return kIOReturnBadArgument; }
    phxrot::Timing t;
    toTiming(*d, &t);
    uint32_t plan = 0;
    const int why = phxrot::check(t, m.head, m.ids, m.n, &plan, m.single);
    OSAddAtomic64(1, &gRotWhy[why < phxrot::kWhyCount ? why : 0]);
    if (why == phxrot::kWhyNotRotated) return kIOReturnUnsupported;   // not ours: IOFramebuffer's own answer, as gate off
    if (why != phxrot::kOk) return kIOReturnUnsupportedMode;
    if (!d->minPixelClock) d->minPixelClock = d->pixelClock;
    if (!d->maxPixelClock) d->maxPixelClock = d->pixelClock;
    if (info) {
        const phxhead::HeadMode *hm = phxrot::modeOf(m.head, plan, m.single);
        uint32_t w, h;
        phxrot::desktopOf(t, &w, &h);
        info->maxDepthIndex = 0; info->nominalWidth = w; info->nominalHeight = h;
        if (hm) info->refreshRate = (IOFixed1616)(((uint64_t)hm->milliHz << 16) / 1000);
        // rot7: AcceleratorBacked, so CoreDisplay turns the desktop itself (phxrot::validatedFlags, opts bit 4)
        const uint32_t opts = (uint32_t)__atomic_load_n(&gRotOpts, __ATOMIC_ACQUIRE);
        info->flags = phxrot::validatedFlags(info->flags, opts);
        if (opts & phxrot::kOptAccelBacked) OSAddAtomic64(1, &gRotAccel);
    }
    return kIOReturnSuccess;
}

void phxRotClear(PhxRotHead &r) { r.tb.n = 0; r.tb.skipped = 0; }

IOReturn phxRotSetTimings(PhxRotHead &r, const PhxRotModes &m, OSArray *array)
{
    OSAddAtomic64(1, &gRotSets);
    phxRotClear(r);
    for (unsigned i = 0; array && i < array->getCount(); i++) {
        OSData *data = OSDynamicCast(OSData, array->getObject(i));
        if (!data || data->getLength() < sizeof(IODetailedTimingInformationV2)) { r.tb.skipped++; continue; }
        IODetailedTimingInformationV2 d;
        memcpy(&d, data->getBytesNoCopy(), sizeof d);
        phxrot::Timing t;
        toTiming(d, &t);
        const unsigned before = r.tb.n;
        if (phxrot::add(r.tb, t, m.head, m.ids, m.n, m.single) == phxrot::kOk) r.raw[before] = d;
    }
    if (m.head < phxhead::kMaxHeads) gRotInstalledN[m.head] = r.tb.n;
    IOLog("PhxRot: head %u setDetailedTimings: %u of %u timing(s) are rotated modes of a listed raster (%u skipped)\n",
          m.head, r.tb.n, array ? array->getCount() : 0, r.tb.skipped);
    return kIOReturnSuccess;
}

static const phxrot::Installed *entry(const PhxRotHead &r, IODisplayModeID id, const IODetailedTimingInformationV2 **raw)
{
    if (const phxrot::Installed *e = phxrot::find(r.tb, (uint32_t)id)) { if (raw) *raw = &r.raw[e - r.tb.e]; return e; }
    if (r.curValid && r.cur.id == (uint32_t)id) { if (raw) *raw = &r.curRaw; return &r.cur; }
    return nullptr;
}
// gate off: no id is ours (stale ids of the last setDetailedTimings included); the rotated mode on the head stays
// answered (the gate cannot go off while one is on: EBUSY)
bool phxRotOwns(const PhxRotHead &r, unsigned head, IODisplayModeID id)
{
    if (r.curValid && r.cur.id == (uint32_t)id) return true;
    return phxRotGate(head) && entry(r, id, nullptr) != nullptr;
}
uint32_t phxRotPlan(const PhxRotHead &r, IODisplayModeID id) { const phxrot::Installed *e = entry(r, id, nullptr); return e ? e->plan : 0; }

IOReturn phxRotInfo(const PhxRotHead &r, unsigned head, IODisplayModeID id, IODisplayModeInformation *info,
                    const phxhead::HeadMode *single)
{
    const phxrot::Installed *e = entry(r, id, nullptr);
    if (!e || !info) return kIOReturnBadArgument;
    const phxhead::HeadMode *hm = phxrot::modeOf(head, e->plan, single);
    uint32_t w, h;
    phxrot::desktopOf(e->t, &w, &h);
    bzero(info, sizeof(*info));
    info->maxDepthIndex = 0; info->nominalWidth = w; info->nominalHeight = h;
    info->refreshRate = (IOFixed1616)(((uint64_t)(hm ? hm->milliHz : 60000u) << 16) / 1000);
    info->flags = phxrot::infoFlags();   // never AcceleratorBacked here (the kernel display pipe reads it)
    return kIOReturnSuccess;
}

IOReturn phxRotPixelInfo(const PhxRotHead &r, IODisplayModeID id, IOIndex depth, IOPixelAperture ap, IOPixelInformation *pi)
{
    const phxrot::Installed *e = entry(r, id, nullptr);
    if (!e || depth != 0 || ap != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
    uint32_t w, h;
    phxrot::desktopOf(e->t, &w, &h);
    bzero(pi, sizeof(*pi));
    pi->bytesPerRow = phxrot::rowBytes(w); pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
    pi->componentCount = 3; pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    pi->activeWidth = w; pi->activeHeight = h;
    return kIOReturnSuccess;
}

IOReturn phxRotTimingInfo(const PhxRotHead &r, IODisplayModeID id, IOTimingInformation *info)
{
    const IODetailedTimingInformationV2 *raw = nullptr;
    if (!entry(r, id, &raw) || !raw || !info) return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    info->appleTimingID = kIOTimingIDInvalid;
    info->flags = kIODetailedTimingValid | kIOScalingInfoValid;
    info->detailedInfo.v2 = *raw;
    return kIOReturnSuccess;
}

bool phxRotPlanTiming(const PhxRotModes &m, uint32_t id, phxhead::DetailedTiming *out)
{
    // head 0 has its own (F1); head 1 while gated, or while any head is gated with opts bit 5 (rot9, phxrot::h1Timings)
    if (m.head == 0 || !phxrot::h1Timings(phxRotGate(m.head), (uint32_t)__atomic_load_n(&gRotGate, __ATOMIC_ACQUIRE),
                                         (uint32_t)__atomic_load_n(&gRotOpts, __ATOMIC_ACQUIRE))) return false;
    const phxhead::HeadMode *hm = phxhead::headMode(m.head, id);
    return hm && phxrot::edidTiming(m.edid, m.edidLen, *hm, out) && phxrot::rotatable(*out, m.head, m.ids, m.n);
}

void phxRotSelect(PhxRotHead &r, unsigned head, uint32_t id)
{
    const IODetailedTimingInformationV2 *raw = nullptr;
    const phxrot::Installed *e = id ? entry(r, (IODisplayModeID)id, &raw) : nullptr;
    if (e && raw) {
        const phxrot::Installed c = *e; const IODetailedTimingInformationV2 cr = *raw;
        r.cur = c; r.curRaw = cr; r.curValid = true;
    } else r.curValid = false;
    const int a = r.curValid ? r.cur.angle : phxrot::kRot0;
    __atomic_store_n(&r.reply, phxrot::replyFlags(a), __ATOMIC_RELEASE);
    if (head < phxhead::kMaxHeads) {
        gRotAngle[head] = a; gRotCurId[head] = r.curValid ? r.cur.id : 0; gRotPlanId[head] = r.curValid ? r.cur.plan : 0;
    }
    if (a || id) IOLog("PhxRot: head %u mode 0x%x: angle %d (plan %u)\n", head, id, a, r.curValid ? r.cur.plan : 0);
}

void phxRotPublish(IOService *fb, unsigned head)
{
    if (!fb) return;
    if (head >= phxhead::kMaxHeads) return;
    const bool on = phxRotGate(head);
    if (!on && !gRotPublished[head]) return;   // never published here: the property is left as it is
    OSNumber *n = OSDynamicCast(OSNumber, fb->getProperty(kIOFBProbeOptionsKey));
    const uint32_t cur = n ? n->unsigned32BitValue() : 0;
    if (on) {
        if (!gRotPublished[head]) { gRotHadOpt[head] = n != nullptr; gRotOrigOpt[head] = cur; }
        fb->setProperty(kIOFBProbeOptionsKey, (unsigned long long)phxrot::probeOptions(cur, true), 32);
    } else if (gRotHadOpt[head]) fb->setProperty(kIOFBProbeOptionsKey, (unsigned long long)gRotOrigOpt[head], 32);
    else fb->removeProperty(kIOFBProbeOptionsKey);   // exactly what the head had before the gate
    gRotPublished[head] = on;
}

// The stack tile and the run are 16-byte aligned (phxrot::rotateFrame), so load16's store is movdqa.
namespace {
struct KernelIo {
    bool sse;
    void load16(const uint8_t *p, uint32_t out[4]) {
        if (sse) __asm__ __volatile__("movntdqa (%1), %%xmm0\n\t" "movdqa %%xmm0, (%0)" : : "r"(out), "r"(p) : "memory");
        else { const volatile uint64_t *q = (const volatile uint64_t *)p; uint64_t a = q[0], b = q[1];
               out[0] = (uint32_t)a; out[1] = (uint32_t)(a >> 32); out[2] = (uint32_t)b; out[3] = (uint32_t)(b >> 32); }
    }
    uint32_t load4(const uint8_t *p) { return *(const volatile uint32_t *)p; }
    void store8(uint8_t *p, uint64_t v) { __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint64_t *)p) : "r"(v)); }
    void store4(uint8_t *p, uint32_t v) { __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint32_t *)p) : "r"(v)); }
    void fence() { __asm__ __volatile__("sfence" ::: "memory"); }
};
}

// xmm0 belongs to the thread that made the call (WindowServer's, in a system call): saved first and put back after, as
// nvrm-accel.cpp nvIopRowSSE does for its ten registers.
void phxRotCopy(const void *src, uint32_t srcPitch, uint32_t w, uint32_t h, void *dst, uint32_t dstPitch, int angle,
                bool l10r, bool sse)
{
    const uint64_t t0 = mach_absolute_time();
    uint8_t save[16] __attribute__((aligned(16)));
    if (sse) __asm__ __volatile__("movdqa %%xmm0, (%0)" : : "r"(save) : "memory");
    KernelIo io{sse};
    phxrot::rotateFrame(io, (const uint8_t *)src, srcPitch, w, h, (uint8_t *)dst, dstPitch, angle, l10r);
    if (sse) __asm__ __volatile__("movdqa (%0), %%xmm0" : : "r"(save) : "memory");
    uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    OSAddAtomic64((SInt64)(ns / 1000), &gRotCopyUs);
    OSAddAtomic64(1, &gRotCopies);
}
void phxRotRefused() { OSAddAtomic64(1, &gRotRefusedN); }
void phxRotStraight() { OSAddAtomic64(1, &gRotStraight); }
bool phxRotOpt180() { return ((uint32_t)__atomic_load_n(&gRotOpts, __ATOMIC_ACQUIRE) & phxrot::kOptRotate180) != 0; }
uint32_t phxRotH0Vbe(unsigned head, uint32_t id, uint32_t curId)
{
    const uint32_t g = (uint32_t)__atomic_load_n(&gRotGate, __ATOMIC_ACQUIRE), o = (uint32_t)__atomic_load_n(&gRotOpts, __ATOMIC_ACQUIRE);
    if (!g || id == curId) return 0;
    // rot11: is some head rotated now (its IOFBTransform, set by IOFramebuffer before CoreDisplay's rebuild)?
    bool rotated = false;
    IOService *fbs[phxhead::kMaxHeads] = {};
    const unsigned n = phxFbCopyHeads(fbs, phxhead::kMaxHeads);
    for (unsigned i = 0; i < n; i++) {
        OSNumber *t = OSDynamicCast(OSNumber, fbs[i]->getProperty("IOFBTransform"));
        if (t && (t->unsigned32BitValue() & phxrot::kRotateBits)) rotated = true;
        fbs[i]->release();
    }
    return phxrot::h0Vbe(head, g, o, id, curId, rotated) | phxrot::h1Vbe(head, g, o, id, curId, rotated);   // rot5 / rot9 / rot11
}
uint32_t phxRotDefaultId(unsigned head, uint32_t bootId, uint32_t curId)
{
    return phxrot::defaultId(head, (uint32_t)__atomic_load_n(&gRotGate, __ATOMIC_ACQUIRE),
                             (uint32_t)__atomic_load_n(&gRotOpts, __ATOMIC_ACQUIRE), bootId, curId);
}

static int phxRotGateSysctl SYSCTL_HANDLER_ARGS
{
    int v = gRotGate;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || !phxrot::gateOk((uint32_t)v)) return EINVAL;
    for (unsigned i = 0; i < phxhead::kMaxHeads; i++)
        if (!((v >> i) & 1) && gRotAngle[i]) return EBUSY;   // a rotated mode is on it: set it to Standard first
    __atomic_store_n(&gRotGate, v, __ATOMIC_RELEASE);
    IOService *fbs[phxhead::kMaxHeads] = {};
    const unsigned n = phxFbCopyHeads(fbs, phxhead::kMaxHeads);
    for (unsigned i = 0; i < n; i++) {
        OSNumber *hn = OSDynamicCast(OSNumber, fbs[i]->getProperty("phx-head"));
        phxRotPublish(fbs[i], hn ? hn->unsigned32BitValue() : 0);
        fbs[i]->release();
    }
    IOLog("PhxRot: gate 0x%x (%u head(s) updated)\n", v, n);
    return 0;
}
static int phxRotStatSysctl SYSCTL_HANDLER_ARGS
{
    char b[512];
    const SInt64 c = gRotCopies;
    snprintf(b, sizeof b, "gate 0x%x | h0 id 0x%x angle %d plan %u inst %u | h1 id 0x%x angle %d plan %u inst %u | sets %lld "
             "validate ok %lld notrot %lld mirror %lld flags %lld inset %lld scaled %lld noplan %lld interlaced %lld badsize %lld | "
             "copies %lld us_avg %lld refused %lld straight %lld opts 0x%x accel %lld",
             gRotGate, gRotCurId[0], gRotAngle[0], gRotPlanId[0], gRotInstalledN[0], gRotCurId[1], gRotAngle[1], gRotPlanId[1],
             gRotInstalledN[1], gRotSets, gRotWhy[phxrot::kOk], gRotWhy[phxrot::kWhyNotRotated], gRotWhy[phxrot::kWhyMirror],
             gRotWhy[phxrot::kWhyFlags], gRotWhy[phxrot::kWhyInset], gRotWhy[phxrot::kWhyScaled], gRotWhy[phxrot::kWhyNoPlan],
             gRotWhy[phxrot::kWhyInterlaced], gRotBadSize, c, c ? gRotCopyUs / c : 0, gRotRefusedN, gRotStraight, gRotOpts, gRotAccel);
    return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, phxfb_rotate, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxRotGateSysctl, "I",
            "rotation: heads that offer rotation (bit 0 = head 0, bit 1 = head 1; default 3 since rot10; 0 = none)");
SYSCTL_INT(_debug, OID_AUTO, phxfb_rotate_opts, CTLFLAG_RW | CTLFLAG_LOCKED, &gRotOpts, 0,
           "rotation: bit 0 = the kernel turns raster-sized frames at 180, bit 1 = a gated head flags its current mode Default, bit 2 = head 0 too while any head is gated, bit 3 = head 0 offers only its current mode as the default while any head is gated, bit 4 = rotated modes are AcceleratorBacked so CoreDisplay turns the desktop, bit 5 = head 1 too offers only its current mode as the default while any head is gated, bit 6 = those marks only while a head is rotated (default 126)");
SYSCTL_PROC(_debug, OID_AUTO, phxfb_rotate_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, phxRotStatSysctl, "A",
            "rotation S1: gate, rotated mode per head, validate verdicts, rotated copies");

void phxRotInstall()
{
    if (gRotInstalled) return;
    sysctl_register_oid(&sysctl__debug_phxfb_rotate); sysctl_register_oid(&sysctl__debug_phxfb_rotate_stat);
    sysctl_register_oid(&sysctl__debug_phxfb_rotate_opts);
    gRotInstalled = true;
}
void phxRotRemove()
{
    if (!gRotInstalled) return;
    sysctl_unregister_oid(&sysctl__debug_phxfb_rotate); sysctl_unregister_oid(&sysctl__debug_phxfb_rotate_stat);
    sysctl_unregister_oid(&sysctl__debug_phxfb_rotate_opts);
    gRotInstalled = false;
}
