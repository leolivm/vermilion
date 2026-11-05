// SPDX-License-Identifier: MIT
// PhxSwitchABI.h: the pipe-1 mode switch between PhxAccel (PhxAMDFB::setDisplayMode on head 1, phxfb.cpp) and
// PhxBringup (phx_pipe2.cpp phxp2::modeSwitch), through PhxBringup's platform function "phxPipe2SetMode"
// (param1 = struct phx_pipe2_setmode *; PhxBringup b93+ marks itself with the property phx-pipe2-mode-fn).
// Synchronous: returns when pipe 1 runs the target, fell back, or went dark (~2.5-3 s on the box).
// Plain C, kernel-free.
#ifndef PHX_SWITCH_ABI_H
#define PHX_SWITCH_ABI_H
#include <stdint.h>

#define PHX_PIPE2_SETMODE_VERSION 1u
struct phx_pipe2_setmode {
    uint32_t version;    // in: PHX_PIPE2_SETMODE_VERSION
    uint32_t target_hz;  // in: 60, 144 or (b108) 165 (head 1 keeps its size)
    int32_t result;      // out: phxp2::SwRes (0 done, 1 same, 2 refused, 3 fell back, 4 dark); -1 = not run (busy / not ready)
    int32_t stage;       // out: the switch stage that ended it
    uint32_t cur_hz;     // out: the pipe-1 mode after the call
    char why[100];       // out: NUL-terminated
};
#ifdef __cplusplus
static_assert(sizeof(struct phx_pipe2_setmode) == 120, "phx_pipe2_setmode is 120 B");
#endif

// b101: the pipe-0 (AOC) mode switch for PhxAMDFB::setDisplayMode on
// head 0, through PhxBringup's platform function "phxPipe0SetMode" (param1 = struct phx_pipe0_setmode *; PhxBringup b101+
// marks itself with the property phx-pipe0-mode-fn, and with phx-pipe0-modes = true while debug.phxpipe0_modes=1 asks
// head 0 to list the AOC's 60 and 144 Hz modes). Synchronous: the pipe-0 engine (PhxPipe0.h phxp0::macSet) runs the
// round trip at the current mode first when none passed in this boot, then the set (~1.5 s each on the box).
// Refused = nothing changed on pipe 0.
#define PHX_PIPE0_SETMODE_VERSION 1u
struct phx_pipe0_setmode {
    uint32_t version;    // in: PHX_PIPE0_SETMODE_VERSION
    uint32_t target_hz;  // in: 60 or 144 (1920x1080 either way)
    int32_t result;      // out: phxp0::Res (0 done, 1 same, 2 refused, 3 fell back, 4 dark); -1 = not run (busy / not ready)
    int32_t stage;       // out: the engine stage that ended it (phxp0::kStageName)
    uint32_t cur_hz;     // out: the pipe-0 mode after the call (0 = unknown)
    uint32_t flags;      // out: PHX_P0SW_* (what the call did besides the set)
    char why[96];        // out: NUL-terminated
};
#define PHX_P0SW_ROUNDTRIP 1u      // a 60->60 (current mode) round trip ran first
#define PHX_P0SW_STUTTER_OFF 2u    // stutter was allowed and is forced off now (left off while pipe 0 is at 144)
#define PHX_P0SW_UCLK_PIN 4u       // the UCLK hard minimum was sent (debug.phxpipe0_uclk MHz) before a set above 60
#ifdef __cplusplus
static_assert(sizeof(struct phx_pipe0_setmode) == 120, "phx_pipe0_setmode is 120 B");
#endif

// b102 hot-plug A2: PhxBringup's poll (phx_hpd.cpp, debug.phxhotplug=2) asks
// PhxAccel's accelerator (class NMXAccel, PhxAccel hpd1+) through its platform function "phxHead1Hotplug" (param1 =
// struct phx_head1_hotplug *) for head 1's state and the EDID it serves, and takes head 1 offline / online the
// debug.phxhead2=0 / =1 way (phxFbSetHeadOnline). Never publishes a nub, never touches head 0. Synchronous, short (no
// gate, no wait: the connect interrupt only bumps IOFramebuffer's counter and starts its thread).
#define PHX_HEAD1_HOTPLUG_VERSION 1u
#define PHX_H1HP_QUERY 0u      // state + EDID only
#define PHX_H1HP_OFFLINE 1u    // online -> offline (debug.phxhead2 reads 2 after)
#define PHX_H1HP_ONLINE 2u     // offline by debug.phxhead2 / hot-plug -> online (debug.phxhead2 reads 1 after)
struct phx_head1_hotplug {
    uint32_t version;    // in: PHX_HEAD1_HOTPLUG_VERSION
    uint32_t op;         // in: PHX_H1HP_*
    int32_t state;       // out, after the call: 0 no head 1 (never armed / not started), 1 online, 2 offline
    int32_t head2;       // out: debug.phxhead2 after the call (0 never armed, 1 armed, 2 armed + disconnected)
    int32_t err;         // out: 0 done; ENXIO no head 1, EALREADY already so, EPERM offline but not by debug.phxhead2 / hot-plug
    uint32_t edid_len;   // out: bytes of edid (0 = no head 1)
    uint8_t edid[256];   // out: head 1's served EDID (PhxAMDFB phx-edid)
};
#ifdef __cplusplus
static_assert(sizeof(struct phx_head1_hotplug) == 280, "phx_head1_hotplug is 280 B");
#endif
#endif /* PHX_SWITCH_ABI_H */
