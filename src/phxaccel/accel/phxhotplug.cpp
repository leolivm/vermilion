// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// phxhotplug.cpp (PhxAccel hpd1): hot-plug step A2. NMXAccel's platform
// function "phxHead1Hotplug" (PhxSwitchABI.h phx_head1_hotplug) for PhxBringup b102's poll (debug.phxhotplug=2): head 1's
// state and served EDID, and head 1 offline / online exactly as debug.phxhead2=0 / =1 do on a published nub
// (phxFbSetHeadOnline, then gPhxHead2 = 2 / 1). Never publishes a nub (that stays debug.phxhead2=1's), never touches head 0.
// Part of the nvrm-accel.cpp translation unit (PHX_FB builds), included right after debug.phxhead2's handler.

#include "../../phxbringup/PhxSwitchABI.h"

int phxFbHeadInfo(unsigned i, int *online, uint8_t *edid, uint32_t max, uint32_t *len);   // phxfb.cpp

static IOReturn phxHead1HotplugFn(phx_head1_hotplug *h)
{
    if (!h || h->version != PHX_HEAD1_HOTPLUG_VERSION || h->op > PHX_H1HP_ONLINE) return kIOReturnBadArgument;
    int on = 0;
    const bool up = NVAccel::head2Published() && gPhxHead2 != 0 && phxFbHeadInfo(1, &on, nullptr, 0, nullptr) == 0;
    h->err = 0;
    if (h->op != PHX_H1HP_QUERY) {
        if (!up) h->err = ENXIO;
        else if (h->op == PHX_H1HP_OFFLINE) {
            if (!on) h->err = EALREADY;
            else if (gPhxHead2 != 1) h->err = EPERM;
            else if (!(h->err = phxFbSetHeadOnline(1, false))) gPhxHead2 = 2;   // = debug.phxhead2=0
        } else {
            if (on) h->err = EALREADY;
            else if (gPhxHead2 != 2) h->err = EPERM;   // offline for another reason (a dark mode switch): not ours to undo
            else if (!(h->err = phxFbSetHeadOnline(1, true))) gPhxHead2 = 1;   // = debug.phxhead2=1 on the published nub
        }
        ALOG("phxHead1Hotplug: %s -> err %d, debug.phxhead2 %d", h->op == PHX_H1HP_OFFLINE ? "offline" : "online", h->err, gPhxHead2);
    }
    uint32_t len = 0;
    on = 0;
    const int e = up ? phxFbHeadInfo(1, &on, h->edid, sizeof h->edid, &len) : ENXIO;
    h->state = e ? 0 : on ? 1 : 2;
    h->head2 = gPhxHead2;
    h->edid_len = e ? 0 : len;
    return kIOReturnSuccess;
}
