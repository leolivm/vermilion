// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxrotate_k.h: the kernel side of a PhxAMDFB head's rotation (rotation S1);
// the pure rules are phxrotate.h (host-tested). phxfb.cpp holds one PhxRotHead per head and forwards its rotated-mode
// queries here; nvrm-accel.cpp performTransaction copies a rotated frame with phxRotCopy. Everything is off unless
// debug.phxfb_rotate names the head (default 0): then validateDetailedTiming / setDetailedTimings are IOFramebuffer's,
// no property is published and every reply carries angle 0, so the head behaves exactly as before.
#pragma once
#include <IOKit/graphics/IOFramebuffer.h>
#include "phxrotate.h"

struct PhxRotHead {
    phxrot::Table tb;                                      // the rotated modes CoreDisplay installed (accepted ones)
    IODetailedTimingInformationV2 raw[phxrot::kMaxInstalled];   // each one as CoreDisplay sent it (getTimingInfo answers it)
    phxrot::Installed cur;                                 // the rotated mode on the head (curValid), kept if a later
    IODetailedTimingInformationV2 curRaw;                  // setDetailedTimings drops it, until the next setDisplayMode
    bool curValid;
    volatile uint32_t reply;                               // phxrot::replyFlags(cur's angle): the copy's angle
};
// What the head lists (the ids its plans have) and the EDID it serves (head 1's detailed timings).
struct PhxRotModes { unsigned head; const uint32_t *ids; unsigned n; const uint8_t *edid; uint32_t edidLen;
                     const phxhead::HeadMode *single = nullptr; };   // rot12: the one-mode head 0

bool phxRotGate(unsigned head);                                    // debug.phxfb_rotate names this head
IOReturn phxRotValidate(const PhxRotModes &m, void *desc, IOByteCount size);
IOReturn phxRotSetTimings(PhxRotHead &r, const PhxRotModes &m, OSArray *array);
void phxRotClear(PhxRotHead &r);                                   // the installed list only (the current mode stays)
bool phxRotOwns(const PhxRotHead &r, unsigned head, IODisplayModeID id);
uint32_t phxRotPlan(const PhxRotHead &r, IODisplayModeID id);      // the head mode id whose raster a rotated id uses
// CoreDisplay's rotated ids are never listed in getDisplayModes (it drops driver ids with bit 31 set: REA); they are
// only accepted and described.
IOReturn phxRotInfo(const PhxRotHead &r, unsigned head, IODisplayModeID id, IODisplayModeInformation *info,
                    const phxhead::HeadMode *single = nullptr);
IOReturn phxRotPixelInfo(const PhxRotHead &r, IODisplayModeID id, IOIndex depth, IOPixelAperture ap, IOPixelInformation *pi);
IOReturn phxRotTimingInfo(const PhxRotHead &r, IODisplayModeID id, IOTimingInformation *info);
// head 1's own plan mode `id` as the detailed timing of the EDID's DTD, only while the gate names the head and only when
// its turned versions pass phxrot::check (a failed preflight is unsafe without IOFBScalerInfo, phxrotate.h)
bool phxRotPlanTiming(const PhxRotModes &m, uint32_t id, phxhead::DetailedTiming *out);
void phxRotSelect(PhxRotHead &r, unsigned head, uint32_t id);     // after a successful setDisplayMode (0 = a plain mode)
void phxRotPublish(IOService *fb, unsigned head);   // IOFBProbeOptions kIOFBSetTransform by the gate
// The display pipe's rotated copy: the portrait frame into the landscape surface (phxrot::rotateFrame, streaming loads
// when sse, non-temporal stores), counted in debug.phxfb_rotate_stat.
void phxRotCopy(const void *src, uint32_t srcPitch, uint32_t w, uint32_t h, void *dst, uint32_t dstPitch, int angle,
                bool l10r, bool sse);
void phxRotRefused();                                              // a frame whose size is not the rotated surface's
bool phxRotOpt180();                                               // debug.phxfb_rotate_opts bit 0 (rot2)
void phxRotStraight();                                             // a raster-sized frame copied straight at a non-zero angle
uint32_t phxRotDefaultId(unsigned head, uint32_t bootId, uint32_t curId);   // the id to flag Default (phxrot::defaultId)
uint32_t phxRotH0Vbe(unsigned head, uint32_t id, uint32_t curId);         // a head's timing verticalBlankingExtension (phxrot::h0Vbe | h1Vbe)
void phxRotInstall();
void phxRotRemove();
