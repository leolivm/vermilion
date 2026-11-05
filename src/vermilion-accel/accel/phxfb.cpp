// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PhxFB: the 780M's display as an IOFramebuffer that PhxAccel (NMXAccel) composites for WindowServer.
// Like NullMoth's NMIntelFB (phase 1), it takes over the mode the firmware (GOP) already lit, at boot in place of the
// firmware framebuffer (IONDRV), and writes nothing to the display engine: the scan-out surface is the boot console's
// (IOPlatformExpert::getConsoleInfo), the accelerator's display pipe copies each finished frame into a write-combined CPU
// mapping of it, and a timer at the panel's refresh (measured on the display engine, measureRefresh) stands in for the VBL
// interrupt WindowServer paces an accelerated display on.
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>
#include "nvrm_vram_abi.h"
#include "nvrm_gpuva_abi.h"
#include "../../vermilion-boot/PhxEdid.h"   // EDID validation + live/static choice (M6 step 2), shared with the host test
#include "../../vermilion-boot/PhxSwitchABI.h"   // mode switch: PhxBringup's "phxPipe2SetMode" (kext b93)
#include "../../vermilion-boot/PhxDdc.h"    // phxddc::kSrcAuto / kSrcOther: the phxEdidRead source request (kext b81)
#include "../../vermilion-boot/PhxGamma.h"  // head-1 gamma: PhxBringup's "phxPipe2SetGamma" (kext b98), resample / identity
#include "phxhead.h"                    // M7 S3a: the second head's layout, mode rule, AGDC list, EDID fallback (host-tested)
#include "phxrotate_k.h"                // rotation S1: rotated modes + the copy's angle (phxrotate_k.cpp, phxrotate.h), off by default
#include "phxoffer.h"                   // head 0's offer gate: only the lit mode until the daemon's restart (host-tested)
#include "phxhp0.h"                     // hot-plug hp0: head 0 (the AOC on DP) offline / online on its HPD (host-tested)

void phxAgdcInstall();   // phxagdc.cpp: the AppleGraphicsDeviceControl that reports this panel (debug.phxagdc)
void phxAgdcRemove();
void phxSurfMapInstall();   // phxsurfmap_k.cpp: M7 S4b surface -> head table (debug.phxsurfmap)
void phxSurfMapRemove();
void phxHoldInit(IOService *fb, unsigned bootHoldSec);   // phxagdc.cpp: debug.phxfb_hold keeps this framebuffer (and so the GPU) busy

// nvrm-accel.cpp: set to 1 by the phxboot daemon (sysctl debug.phxfb_go) once the GPU is up, claimed, and a LIVE Metal
// device exists. PhxAMDFB waits on it before taking the panel so WindowServer never builds the accelerated CoreDisplay
// device before Metal is ready (that race = the fatal "Failed to create MetalDevice" assert, RE'd 2026-10-24).
extern int gPhxFbGo;

#define PFBLOG(fmt, ...) IOLog("PhxFB: " fmt "\n", ##__VA_ARGS__)

static int gAlias = 1;
static bool gAliasSysctl;
SYSCTL_INT(_debug, OID_AUTO, phxfb_alias, CTLFLAG_RW | CTLFLAG_LOCKED, &gAlias, 0,
           "1 = frames are copied into the scan-out through its system-RAM address (the carve-out), 0 = through the GPU's BAR");

// The VBL tick. XNU's timer coalescing moves a kernel deadline by up to an eighth of its delay, at most 1 ms, so a
// 120 Hz tick could land anywhere in the first millisecond after the vblank it stands for. The system-critical urgency
// that is never moved is not exported to kexts (thread_call_enter_delayed_with_leeway), so the tick is armed 1.5 ms
// early and re-armed for its deadline from there, each stage with an eighth of the previous slop, until less than
// 50 us remain (debug.phxfb_vbl_staged, 0 = one plain deadline). The lateness of each tick is summed for the
// measurements, and nvrm-accel.cpp times its presents against the last tick (gPhxVblLastAbs, gPhxFrameAbs).
static int gVblStaged = 1;
// M7 S3a: the VBL counters per head; head 0's are the debug.phxfb_vbl_* names as before, head 1's debug.phxfb1_vbl_*.
// Only head 0's tick moves gPhxVblLastAbs / gPhxVblSeq (the accelerator paces head 0's presents on them).
struct PhxVblStat { SInt64 n, lateUsSum, lateUsMax, handlers, procs; };
static PhxVblStat gVbl[phxhead::kMaxHeads];
uint64_t gPhxVblLastAbs, gPhxFrameAbs;
volatile uint64_t gPhxVblSeq;   // ticks so far, counted before the handlers run: the display pipe completes on the next one
SYSCTL_INT(_debug, OID_AUTO, phxfb_vbl_staged, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblStaged, 0,
           "1 = the VBL tick is armed early and re-armed until its deadline (little coalescing slop), 0 = one plain deadline");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_n, CTLFLAG_RD | CTLFLAG_LOCKED, &gVbl[0].n, "VBL ticks fired");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_late_us_sum, CTLFLAG_RW | CTLFLAG_LOCKED, &gVbl[0].lateUsSum, "microseconds the VBL ticks fired past their deadlines, summed");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_late_us_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gVbl[0].lateUsMax, "the latest VBL tick, microseconds past its deadline (writable: 0 restarts it)");
// Reliable diagnostics (immune to IOLog burst-drop): how many VBL handlers registered on this framebuffer, and how many
// proc calls the VBL tick has made. vbl_handlers>=1 => registerForInterruptType('vbl ') reached PhxAMDFB; vbl_procs climbing
// => the tick is actually driving them (so the pacing chain to WindowServer is live).
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_handlers, CTLFLAG_RD | CTLFLAG_LOCKED, &gVbl[0].handlers, "VBL interrupt handlers registered on this framebuffer");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_vbl_procs, CTLFLAG_RD | CTLFLAG_LOCKED, &gVbl[0].procs, "VBL-tick proc invocations made so far");
// M7 S3a: the second (phantom) head. Armed at run time only (NMXAccel's debug.phxhead2=1 publishes its nub); these are
// registered with head 0's sysctls so its mode can be chosen before arming. Zero display-engine access for head 1.
static int gHead1W = phxhead::kDefW, gHead1H = phxhead::kDefH, gHead1Force = 0, gHead1State = 0, gHead1EdidSrc = -1,
           gHead1EdidWhy = -1, gHead1Online = 0;
// hot-plug hp0 (phxhotplug0.cpp): head 0 reports a monitor (1, as always before hp0) or not (0, debug.phxhead0_online=0 /
// debug.phxhotplug0=2 on an AOC unplug); gHead0SwBusy = head-0 setDisplayMode calls in flight (the policy holds meanwhile)
static int gHead0Online = 1;
static volatile SInt32 gHead0SwBusy = 0;
static SInt64 gHead1VramOff = -1;
// M7 S4a: per head (index = PhxAMDFB::fHead); head 0's are the debug.phxfb_flip* names as before, head 1's debug.phxfb1_flip*.
static int gFlipOn[phxhead::kMaxHeads];
static SInt64 gFlipN[phxhead::kMaxHeads], gFlipTimeouts[phxhead::kMaxHeads], gFlipWaitN, gFlipWaitUsSum, gFlipLatchUs, gFlipLatchLine;
// M7 S3b: head 1 reads pipe 1 (OTG1 / HUBP1, read only): its refresh at start, its VBL tick's phase, whether HUBP1 scans it
static int gHead1Scan = -1, gHead1PipeRefresh = 0;
static SInt64 gHead1Locked = 0;
// Mode switch (the R0 rules of IOFramebuffer's setDisplayMode):
// debug.phxfb1_modes=1 before head 1 starts = list the pipe-1 plans its monitor's EDID has (phxhead::head1ModeList) and
// switch pipe 1 in setDisplayMode through PhxBringup; 0 (default) = the one mode as before.
static int gHead1Modes = 0, gHead1Mode = 0, gHead1NModes = 0, gHead1SwLast = -1;
static SInt64 gHead1Switches = 0, gHead1SwitchFails = 0;
// AOC at 144: head 0 lists the pipe-0 plans its EDID has
// (phxhead::kHead0Modes) when PhxBringup b101+ carries phx-pipe0-modes = true as head 0 starts (the kext's
// debug.phxpipe0_modes=1, set by the boot daemon before the claim), and switches pipe 0 in setDisplayMode through
// PhxBringup's "phxPipe0SetMode"; otherwise the one mode as before.
static int gHead0Mode = 0, gHead0NModes = 0, gHead0SwLast = -1, gHead0SwFlags = 0, gHead0Refresh = 0;
static SInt64 gHead0Switches = 0, gHead0SwitchFails = 0;
SYSCTL_INT(_debug, OID_AUTO, phxfb_mode, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0Mode, 0, "head 0's current mode id (1 = 60 Hz, 2 = 144 Hz, 0 = the single mode)");
SYSCTL_INT(_debug, OID_AUTO, phxfb_nmodes, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0NModes, 0, "head 0's listed modes (0 = the single mode)");
SYSCTL_INT(_debug, OID_AUTO, phxfb_refresh, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0Refresh, 0, "head 0's VBL tick rate, mHz (the one WindowServer paces head 0 on)");
SYSCTL_INT(_debug, OID_AUTO, phxfb_switch_last, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0SwLast, 0,
           "head 0's last switch: phx_pipe0_setmode.result (0 done, 1 same, 2 refused, 3 fell back, 4 dark, -1 not run); why: debug.phxpipe0_stat");
SYSCTL_INT(_debug, OID_AUTO, phxfb_switch_flags, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0SwFlags, 0,
           "head 0's last switch: PHX_P0SW_* (1 round trip first, 2 stutter forced off, 4 UCLK hard minimum sent)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_switches, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0Switches, "head 0: setDisplayMode calls that asked PhxBringup to switch pipe 0");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_switch_fails, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0SwitchFails, "head 0: switches that did not end at the target");
// Boot rate: the boot daemon re-times pipe 0 itself (PhxBringup debug.phxpipe0=4)
// before its one WindowServer restart; debug.phxfb_resync=1 then has head 0 read OTG0's totals again, so the restarted
// WindowServer finds that mode current, as a head-0 start at that mode would have reported it (getCurrentDisplayMode, the VBL
// tick, PhxFBRefreshMilliHz / HTotal / VTotal for the AGDC). fBootModeId (the Default flag) stays the mode head 0 started at,
// as after a System Settings switch. Head 0 with a mode list only; reads OTG0 only, never writes the display engine.
static int gHead0ResyncLast = 0;
static SInt64 gHead0Resyncs = 0;
// Head 0's offer gate (phxoffer.h): 1 = every listed mode is offered, 0 = only
// the mode lit now (closed at a head-0 start with a mode list, until debug.phxfb0_offer=1 or a resync to another mode).
static int gHead0OfferOpen = 1;
static SInt64 gHead0OfferOpens = 0;
static int phxfb0OfferSysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, phxfb0_offer, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxfb0OfferSysctl, "I",
            "head 0's offer gate: 1 = every listed mode offered, 0 = only the lit one (closed at start with a mode list); write 1 (root) to open it and drop the IOFBConfig cache");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb0_offer_opens, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0OfferOpens, "head 0: offer gate openings");
static int phxfbResyncSysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, phxfb_resync, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxfbResyncSysctl, "I",
            "head 0 (root): 1 = take the mode OTG0 runs now as current (after a kext-only pipe-0 set); reads the last result: the id now current, 0 never run, -1 no head 0, -2 no mode list, -3 OTG0 at no listed mode (nothing changed), -4 busy");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_resyncs, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead0Resyncs, "head 0: debug.phxfb_resync=1 writes run");
// Gamma: head 1 publishes IOFBGammaCount 256 / IOFBGammaWidth 16, keeps the last
// table IOFramebuffer sends and forwards it to PhxBringup b98+ "phxPipe2SetGamma" (MPCC_OGAM1; written only with the
// kext's debug.phxgamma=1); Busy (a flip still latching) = retried every 4 ms by a thread call, at most 50 times per table;
// re-sent after a head-1 mode switch and setOnline(true). Head 0 (the GOP's pipe 0) only counts: Success as before.
static SInt64 gGamma0N, gGamma1N, gGamma1Pushes, gGamma1Busy, gGamma1Retries, gGamma1GiveUp, gGamma1Bad;
static int gGamma1Count = 0, gGamma1Width = 0, gGamma1Sync = -1, gGamma1E255 = -1, gGamma1E255Min = 0xffff, gGamma1E255Max = 0,
           gGamma1Identity = -1, gGamma1LastRes = -1, gGamma1Bank = -1, gGamma1Ctl = 0;
static char gGamma1Why[64];
static int phxfb1GammaStatSysctl SYSCTL_HANDLER_ARGS;
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_gamma_n, CTLFLAG_RD | CTLFLAG_LOCKED, &gGamma0N, "head 0: setGammaTable calls (counted only, nothing written)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_gamma_n, CTLFLAG_RD | CTLFLAG_LOCKED, &gGamma1N, "head 1: setGammaTable calls");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_gamma_e255_min, CTLFLAG_RW | CTLFLAG_LOCKED, &gGamma1E255Min, 0,
           "head 1: the lowest entry 255 (largest channel) seen; write 65535 to restart");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_gamma_e255_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gGamma1E255Max, 0,
           "head 1: the highest entry 255 (largest channel) seen; write 0 to restart");
SYSCTL_PROC(_debug, OID_AUTO, phxfb1_gamma_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, phxfb1GammaStatSysctl, "A",
            "head 1 gamma: calls, last table shape, entry 255, identity, pushes to PhxBringup, busy / retries / give-ups, last result");
static int phxfb1SumSysctl SYSCTL_HANDLER_ARGS;
static int phxfb1ShotSysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, phxfb1_shot, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, phxfb1ShotSysctl, "S",
            "head 1's whole surface, debug.phxshot's format: 'PSHT', pitch in pixels, height, VRAM offset, then BGRA rows (root)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_w, CTLFLAG_RW | CTLFLAG_LOCKED, &gHead1W, 0, "head 1 (phantom) width, read when it starts (default 1680)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_h, CTLFLAG_RW | CTLFLAG_LOCKED, &gHead1H, 0, "head 1 (phantom) height, read when it starts (default 1050)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_force, CTLFLAG_RW | CTLFLAG_LOCKED, &gHead1Force, 0,
           "1 = let head 1 start at 1920x1080 or more (the plugin's GPU present would then put its frames on head 0: S4b first)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_state, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1State, 0,
           "head 1: 0 not started, 3 starting, 1 up, 2 up but disconnected (debug.phxhead2=0), <0 the start step that failed");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_online, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1Online, 0, "head 1 reports a connected monitor");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_edid_src, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1EdidSrc, 0, "head 1's EDID source (0x10 DDC1, 0x20+n AUX n; -1 none)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_edid_why, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1EdidWhy, 0, "head 1's EDID: 0 live, else the phxedid::kWhy* reason the fallback is served");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_modes, CTLFLAG_RW | CTLFLAG_LOCKED, &gHead1Modes, 0,
           "1 = head 1 lists the pipe-1 plans its EDID has (60 / 144) and switches pipe 1 in setDisplayMode; read when head 1 starts");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_mode, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1Mode, 0, "head 1's current mode id (1 = 60 Hz, 2 = 144 Hz, 0 = the single mode)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_nmodes, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1NModes, 0, "head 1's listed modes (0 = the single mode)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_switch_last, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1SwLast, 0,
           "head 1's last switch: phx_pipe2_setmode.result (0 done, 2 refused, 3 fell back, 4 dark, -1 not run)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_switches, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1Switches, "head 1: setDisplayMode calls that asked PhxBringup to switch");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_switch_fails, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1SwitchFails, "head 1: switches that did not end at the target");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_vram_off, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1VramOff, "head 1's surface: VRAM byte offset (-1 none)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_scan, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1Scan, 0,
           "head 1: 1 = HUBP1 scans its surface (unblanked, address = its VRAM), 0 = not, -1 = unknown (no MMIO view)");
SYSCTL_INT(_debug, OID_AUTO, phxfb1_pipe_refresh, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1PipeRefresh, 0,
           "head 1: OTG1's refresh measured at start, mHz (0 = OTG1 was not running: the 60 Hz synthetic tick)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_vbl_locked, CTLFLAG_RD | CTLFLAG_LOCKED, &gHead1Locked, "head 1 VBL ticks placed on OTG1's vblank start");
static int phxfb1FlipSysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, phxfb1_flip, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxfb1FlipSysctl, "I",
            "head 1: 1 = double-buffer its surface (B at VRAM 72 MiB) and flip HUBP1 at OTG1's vblank, 0 = copy into the scanned surface (default)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_flips, CTLFLAG_RD | CTLFLAG_LOCKED, &gFlipN[1], "head 1: frames flipped to the other buffer");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_flip_timeouts, CTLFLAG_RD | CTLFLAG_LOCKED, &gFlipTimeouts[1], "head 1: flips that never latched (flipping turned itself off)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_vbl_n, CTLFLAG_RD | CTLFLAG_LOCKED, &gVbl[1].n, "head 1 VBL ticks fired");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_vbl_late_us_sum, CTLFLAG_RW | CTLFLAG_LOCKED, &gVbl[1].lateUsSum, "head 1: microseconds the VBL ticks fired late, summed");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_vbl_late_us_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gVbl[1].lateUsMax, "head 1: the latest VBL tick, microseconds");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_vbl_handlers, CTLFLAG_RD | CTLFLAG_LOCKED, &gVbl[1].handlers, "head 1: VBL interrupt handlers registered");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb1_vbl_procs, CTLFLAG_RD | CTLFLAG_LOCKED, &gVbl[1].procs, "head 1: VBL-tick proc invocations");
SYSCTL_PROC(_debug, OID_AUTO, phxfb1_sum, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, phxfb1SumSysctl, "S",
            "head 1's surface, 64 samples: 'P1SM', width, height, pitch, non-zero samples, distinct-from-first samples, XOR of all");
// DCN double buffer. The display pipe copied each frame into the surface the panel was scanning, at a
// random phase against a ~114 us vblank: every copy tore (Show Desktop, fast scrolling, 2026-10-24). With
// debug.phxfb_flip=1 a frame is copied into a second VRAM surface the panel is not scanning (B, in the region PhxBringup
// leaves to the firmware below PHX_VRAM_RING_MIN) and HUBP0 flips to it at the next VUPDATE, as Linux's
// hubp3_program_surface_flip_and_addr; the next frame waits for that flip to latch (hubp2_is_flip_pending) before it
// takes the other buffer. debug.phxfb_flip_test steps the bring-up by hand: 1 = flip A->A and time the latch, 2 = copy
// A into B and flip to B (the panel freezes on that copy), 4 = paint a red band into B, 3 = flip back to A.
static int phxfbFlipSysctl SYSCTL_HANDLER_ARGS;
static int phxfbFlipTestSysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, phxfb_flip, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxfbFlipSysctl, "I",
            "1 = double-buffer the scan-out and flip at vblank, 0 = copy into the scanned surface (default)");
SYSCTL_PROC(_debug, OID_AUTO, phxfb_flip_test, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, phxfbFlipTestSysctl, "I",
            "flip bring-up: 1 = A->A, 2 = copy A to B and flip to B, 4 = red band in B, 3 = back to A (reads the last result)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_flips, CTLFLAG_RD | CTLFLAG_LOCKED, &gFlipN[0], "frames flipped to the other buffer");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_flip_timeouts, CTLFLAG_RD | CTLFLAG_LOCKED, &gFlipTimeouts[0], "flips that never latched (flipping turned itself off)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_flip_waits, CTLFLAG_RD | CTLFLAG_LOCKED, &gFlipWaitN, "frames that waited for the previous flip to latch");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_flip_wait_us_sum, CTLFLAG_RD | CTLFLAG_LOCKED, &gFlipWaitUsSum, "microseconds frames waited for the previous flip, summed");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_flip_latch_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gFlipLatchUs, "the last test flip: microseconds from the write to the latch (-1 = never)");
SYSCTL_QUAD(_debug, OID_AUTO, phxfb_flip_latch_line, CTLFLAG_RD | CTLFLAG_LOCKED, &gFlipLatchLine, "the last test flip: OTG0 line when the latch was seen");
static struct sysctl_oid *const kFbOids[] = { &sysctl__debug_phxfb_alias, &sysctl__debug_phxfb_vbl_staged, &sysctl__debug_phxfb_vbl_n,
                                              &sysctl__debug_phxfb_vbl_late_us_sum, &sysctl__debug_phxfb_vbl_late_us_max,
                                              &sysctl__debug_phxfb_vbl_handlers, &sysctl__debug_phxfb_vbl_procs,
                                              &sysctl__debug_phxfb_flip, &sysctl__debug_phxfb_flip_test, &sysctl__debug_phxfb_flips,
                                              &sysctl__debug_phxfb_flip_timeouts, &sysctl__debug_phxfb_flip_waits,
                                              &sysctl__debug_phxfb_flip_wait_us_sum, &sysctl__debug_phxfb_flip_latch_us,
                                              &sysctl__debug_phxfb_flip_latch_line,
                                              &sysctl__debug_phxfb1_w, &sysctl__debug_phxfb1_h, &sysctl__debug_phxfb1_force,
                                              &sysctl__debug_phxfb1_state, &sysctl__debug_phxfb1_online,
                                              &sysctl__debug_phxfb1_edid_src, &sysctl__debug_phxfb1_edid_why,
                                              &sysctl__debug_phxfb1_vram_off, &sysctl__debug_phxfb1_vbl_n,
                                              &sysctl__debug_phxfb1_vbl_late_us_sum, &sysctl__debug_phxfb1_vbl_late_us_max,
                                              &sysctl__debug_phxfb1_vbl_handlers, &sysctl__debug_phxfb1_vbl_procs,
                                              &sysctl__debug_phxfb1_sum, &sysctl__debug_phxfb1_shot,
                                              &sysctl__debug_phxfb1_scan, &sysctl__debug_phxfb1_pipe_refresh,
                                              &sysctl__debug_phxfb1_vbl_locked, &sysctl__debug_phxfb1_flip,
                                              &sysctl__debug_phxfb1_flips, &sysctl__debug_phxfb1_flip_timeouts,
                                              &sysctl__debug_phxfb1_modes, &sysctl__debug_phxfb1_mode, &sysctl__debug_phxfb1_nmodes,
                                              &sysctl__debug_phxfb1_switch_last, &sysctl__debug_phxfb1_switches,
                                              &sysctl__debug_phxfb1_switch_fails, &sysctl__debug_phxfb_gamma_n,
                                              &sysctl__debug_phxfb1_gamma_n, &sysctl__debug_phxfb1_gamma_e255_min,
                                              &sysctl__debug_phxfb1_gamma_e255_max, &sysctl__debug_phxfb1_gamma_stat,
                                              &sysctl__debug_phxfb_mode, &sysctl__debug_phxfb_nmodes, &sysctl__debug_phxfb_refresh,
                                              &sysctl__debug_phxfb_switch_last, &sysctl__debug_phxfb_switch_flags,
                                              &sysctl__debug_phxfb_switches, &sysctl__debug_phxfb_switch_fails,
                                              &sysctl__debug_phxfb_resync, &sysctl__debug_phxfb_resyncs,
                                              &sysctl__debug_phxfb0_offer, &sysctl__debug_phxfb0_offer_opens };

// The class name carries "AMD": CoreDisplay tells the GPU vendor from the framebuffer's class name (Intel, AMD, NVDA,
// AppleParavirt), and for any other name it turns IOPresentment off, which left the panel on the software compositor
// with the Metal compositor up for the accelerator (GetGPUVendorForFramebufferService -> 1, 2026-10-20).
// EDID of the attached external monitor (Acer KG241Q S), captured live via the RX4Darwin probe on this Navi31
// 2026-10-24. Served through hasDDCConnect()/getDDCBlock() so CoreDisplay gets real display attributes for an
// EXTERNAL monitor (nullarc's eDP had these built in; without EDID CoreDisplay SIGABRTs in CreateMetalDevice).
// Since M6 step 2 it is only the fallback: loadEdid() serves the EDID PhxBringup (b79+) reads live over DDC.
// In this release the ID serial number (bytes 12-15) is zeroed and the checksum (byte 127) recomputed: a generic example
// EDID of that model, not anyone's monitor.
static const uint8_t kPhxEDID[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x04, 0x72, 0x4d, 0x07,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x1e, 0x01, 0x03, 0x80, 0x34, 0x1d, 0x78,
    0x2a, 0xe8, 0xa5, 0xa3, 0x5a, 0x50, 0xa4, 0x27, 0x0b, 0x50, 0x54, 0xbf,
    0xef, 0x80, 0x71, 0x4f, 0x81, 0x40, 0x81, 0x80, 0x81, 0xc0, 0x81, 0x00,
    0x95, 0x00, 0xb3, 0x00, 0xd1, 0xc0, 0xfc, 0x7e, 0x80, 0x88, 0x70, 0x38,
    0x12, 0x40, 0x18, 0x20, 0x35, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x1e,
    0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00,
    0x09, 0x25, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30,
    0xa5, 0x1e, 0xb4, 0x22, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x00, 0x00, 0x00, 0xfc, 0x00, 0x4b, 0x47, 0x32, 0x34, 0x31, 0x51, 0x20,
    0x53, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x01, 0x8d, 0x02, 0x03, 0x35, 0xf1,
    0x4c, 0x01, 0x03, 0x04, 0x12, 0x13, 0x05, 0x14, 0x3f, 0x1f, 0x10, 0x07,
    0x4e, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0x6a, 0x03, 0x0c,
    0x00, 0x20, 0x00, 0x00, 0x42, 0x20, 0x00, 0x00, 0x68, 0x1a, 0x00, 0x00,
    0x01, 0x01, 0x30, 0xa5, 0xe6, 0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0x80,
    0x00, 0xd0, 0x94, 0x80, 0xa0, 0x70, 0x38, 0x1e, 0x40, 0x30, 0x40, 0x35,
    0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x1a, 0x01, 0x1d, 0x00, 0x72, 0x51,
    0xd0, 0x1e, 0x20, 0x6e, 0x28, 0x55, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00,
    0x1e, 0x8c, 0x0a, 0xd0, 0x8a, 0x20, 0xe0, 0x2d, 0x10, 0x10, 0x3e, 0x96,
    0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x18, 0x40, 0x6e, 0x80, 0x88, 0x70,
    0x38, 0x40, 0x40, 0x08, 0x20, 0x28, 0x0c, 0x09, 0x25, 0x21, 0x00, 0x00,
    0x1c, 0x00, 0x00, 0x33,
};

class PhxAMDFB : public IOFramebuffer {
    OSDeclareDefaultStructors(PhxAMDFB)
    IOPCIDevice *fPCI = nullptr;
    unsigned fHead = 0;        // M7 S3a: 0 = the panel the GOP lit (everything below as before), 1 = the phantom second head
    bool fOnline = true;       // false = reports no monitor (head 1: debug.phxhead2=0 / hot-plug A2; head 0: hot-plug hp0)
    // M6 step 2: the EDID getDDCBlock serves, chosen once in start() (loadEdid): the monitor's own, read live by
    // PhxBringup, when it validates; else kPhxEDID as before.
    uint8_t fEdid[256] = {};
    uint32_t fEdidLen = 0;
    IOMemoryMap *fSurfMap = nullptr;   // write-combined CPU view of the scan-out surface through the BAR
    // The same surface through its system-RAM address: the 780M's VRAM is a carve-out of system memory, and amdgpu maps
    // it there on APUs rather than through the BAR. Through the BAR a 2560x1600 frame took ~110 ms to copy (~7 fps, the
    // whole desktop lagged, 2026-10-20). This view comes out write-back although it asks for write-combined (frames
    // copied with plain stores stayed in the CPU cache: stripes), so the display pipe writes it with non-temporal stores.
    IOMemoryMap *fAliasMap = nullptr;
    unsigned fAliasTries = 0;
    volatile UInt32 fAliasBusy = 0;
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl[4], fConnect;   // IOFramebuffer itself and, when it asks, IOAccelDisplayPipe each register a VBL handler
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0;
    volatile bool fStopping = false;
    bool fBeside = false;   // another framebuffer (the firmware's IONDRV one) already drives this GPU: not the console
    IOPhysicalAddress fBase = 0;
    uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0;
    static constexpr uint64_t kFrameNs = 16666667ull;   // 60 Hz, when the refresh cannot be measured
    uint64_t fFrameNs = kFrameNs;
    uint32_t fRefreshMilliHz = 60000, fHTotal = 0, fVTotal = 0;
    // head 1 with debug.phxfb1_modes, head 0 with the kext's phx-pipe0-modes: the listed ids (fixed from start: macOS caches
    // the list), the one lit now, the one lit at start (the only one flagged default). fNModes 0 = the single mode id 1 as before.
    uint32_t fModeIds[phxhead::kNMaxModes] = {}, fModeId = 1, fBootModeId = 1;
    unsigned fNModes = 0;
    // rot12: head 0 with no mode list rotates its one GOP mode (phxrot::singleMode; id 1 = the EDID DTD of OTG0's totals)
    phxhead::HeadMode fSingle = {};
    phxhead::DetailedTiming fSingleT = {};
    bool fSingleOk = false;
    PhxRotHead fRot = {};   // rotation S1 (phxrotate_k.h): empty and angle 0 unless debug.phxfb_rotate names this head
    PhxRotModes rotModes() const {
        static const uint32_t kOne[1] = {1};
        if (!fNModes && fSingleOk) return PhxRotModes{fHead, kOne, 1, fEdid, fEdidLen, &fSingle};   // rot12
        return PhxRotModes{fHead, fModeIds, fNModes, fEdid, fEdidLen, nullptr};
    }
    bool listed(IODisplayModeID m) const {
        if (!fNModes) return m == 1;
        for (unsigned i = 0; i < fNModes; i++) if ((IODisplayModeID)fModeIds[i] == m) return true;
        return false;
    }
    IOReturn switchHead1Mode(IODisplayModeID id);
    IOReturn switchHead0Mode(IODisplayModeID id);   // AOC at 144: pipe 0 through PhxBringup b101 "phxPipe0SetMode"
    void head0ModeRate();
    // the PhxBringup service (a child of the GPU's PCI device) that marks itself with the boolean property prop, retained
    IOService *copyBringup(const char *prop) {
        IOService *out = nullptr;
        if (OSIterator *it = fPCI ? fPCI->getChildIterator(gIOServicePlane) : nullptr) {
            while (OSObject *o = it->getNextObject()) {
                IOService *sv = OSDynamicCast(IOService, o);
                if (sv && sv->getProperty(prop) == kOSBooleanTrue) { out = sv; out->retain(); break; }
            }
            it->release();
        }
        return out;
    }
    // head 1 gamma: the last table (fGamma, valid once IOFB sent one), whether PhxBringup still has to take it (fGammaDirty),
    // the retry thread call and its count for the current table; all under fGammaLock
    IOLock *fGammaLock = nullptr;
    uint16_t fGamma[3][phxgamma::kN] = {};
    bool fGammaValid = false, fGammaDirty = false;
    unsigned fGammaTries = 0;
    thread_call_t fGammaRetry = nullptr;
    IOService *fGammaSvc = nullptr;   // PhxBringup b98+ (property phx-pipe2-gamma-fn), retained
    phx_pipe2_gamma fGammaReq = {};
    static constexpr unsigned kGammaMaxTries = 50, kGammaRetryUs = 4000;
    IOReturn gammaSet(UInt32 channels, UInt32 count, UInt32 width, void *data, bool sync);
    void gammaPushLocked();
    void gammaReapply();
    static void gammaRetryFire(thread_call_param_t me, thread_call_param_t);
    static constexpr unsigned kBootHoldSec = 120;   // the boot daemon releases it sooner (debug.phxfb_hold=0)
    // The accelerator is NullMoth's, written against an NVIDIA RM: it backs each IOAccel surface with "VRAM" from the
    // framebuffer (nvAllocVram) and a GPU address from the RM (nvGpuVaAlloc/Map). On the 780M nothing on the GPU reads
    // those surfaces -- the display pipe copies finished frames by CPU, and RADV keeps its own GPU addresses in
    // PhxBringup -- so VRAM here is physically contiguous system memory (the 780M has no other kind) and a GPU address
    // is a number from a private range that never enters a page table. Without them every surface_flush WindowServer
    // makes failed ("failed to alloc GART space"), 30 times a second, and no frame reached the panel (2026-10-20).
    static constexpr uint64_t kVaBase = 0x100000000000ull, kVaSize = 1ull << 40, kVramCap = 1024ull << 20;
    IORangeAllocator *fVa = nullptr;
    volatile SInt64 fVramBytes = 0;
    volatile SInt32 fHandles = 0;
    unsigned fRmLogged = 0;
    // VRAM comes from one physically contiguous pool taken at start, while memory is still unfragmented: allocated one
    // by one, a 16 MB surface found no contiguous run hours into a session, and when WindowServer re-allocated its frame
    // on display wake the panel kept the frame from before the display slept (2026-10-21). An allocation is a
    // sub-descriptor of the pool (its handle); past the pool each one is contiguous memory of its own, as before.
    static constexpr uint64_t kPoolTry[3] = { 512ull << 20, 256ull << 20, 128ull << 20 };
    IOBufferMemoryDescriptor *fPool = nullptr;
    IORangeAllocator *fPoolRange = nullptr;
    uint8_t *fPoolKva = nullptr;
    uint64_t fPoolPhys = 0, fPoolBytes = 0;

    void poolInit() {
        for (uint64_t bytes : kPoolTry) {
            IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0x0000FFFFFFFFF000ull);
            if (b && b->prepare() != kIOReturnSuccess) OSSafeReleaseNULL(b);
            if (!b) continue;
            fPoolRange = IORangeAllocator::withRange(bytes, PAGE_SIZE, 0, IORangeAllocator::kLocking);
            if (!fPoolRange) { b->complete(); b->release(); return; }
            fPool = b; fPoolKva = (uint8_t *)b->getBytesNoCopy(); fPoolBytes = bytes;
            fPoolPhys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
            PFBLOG("VRAM pool: %llu MB at phys 0x%llx", bytes >> 20, fPoolPhys);
            return;
        }
        PFBLOG("VRAM pool: no contiguous run of %llu MB or less, every allocation on its own", kPoolTry[2] >> 20);
    }
    void poolFree() {
        OSSafeReleaseNULL(fPoolRange);
        if (fPool) { fPool->complete(); OSSafeReleaseNULL(fPool); }
        fPoolKva = nullptr; fPoolPhys = fPoolBytes = 0;
    }
    bool poolAlloc(NVRMVramRequest *r, uint64_t bytes) {
        IORangeScalar off = 0;
        if (!fPoolRange || !fPoolRange->allocate(bytes, &off, PAGE_SIZE)) return false;
        IOSubMemoryDescriptor *s = IOSubMemoryDescriptor::withSubRange(fPool, off, bytes, kIODirectionInOut);
        if (!s) { fPoolRange->deallocate(off, bytes); return false; }
        bzero(fPoolKva + off, bytes);
        r->handle = s; r->kva = fPoolKva + off; r->actualSize = bytes; r->allocationCookie = 0;
        r->phys = fPoolPhys + off;
        return true;
    }

    IOReturn vramAlloc(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION || !r->size) return kIOReturnBadArgument;
        const uint64_t bytes = round_page_64(r->size);
        if ((uint64_t)OSAddAtomic64((SInt64)bytes, &fVramBytes) + bytes > kVramCap) {
            OSAddAtomic64(-(SInt64)bytes, &fVramBytes);
            if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes refused, %lld already out (cap %llu)", bytes, fVramBytes, kVramCap);
            return kIOReturnNoMemory;
        }
        if (poolAlloc(r, bytes)) {
            r->mappedTotal = (uint64_t)fVramBytes;
            if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes at phys 0x%llx from the pool (%lld out)", bytes, r->phys, fVramBytes);
            return kIOReturnSuccess;
        }
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, 0x0000FFFFFFFFF000ull);
        if (b && b->prepare() != kIOReturnSuccess) OSSafeReleaseNULL(b);
        if (!b) {
            OSAddAtomic64(-(SInt64)bytes, &fVramBytes);
            if (fRmLogged++ < 16) PFBLOG("VRAM: no contiguous %llu bytes", bytes);
            return kIOReturnNoMemory;
        }
        bzero(b->getBytesNoCopy(), bytes);
        r->handle = b; r->kva = b->getBytesNoCopy(); r->actualSize = bytes; r->allocationCookie = 0;
        r->phys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
        r->mappedTotal = (uint64_t)fVramBytes;
        if (fRmLogged++ < 16) PFBLOG("VRAM: %llu bytes at phys 0x%llx (%lld out)", bytes, r->phys, fVramBytes);
        return kIOReturnSuccess;
    }
    IOReturn vramFree(NVRMVramRequest *r) {
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        if (IOSubMemoryDescriptor *s = OSDynamicCast(IOSubMemoryDescriptor, (OSObject *)r->handle)) {
            const uint64_t off = (uint64_t)((uint8_t *)r->kva - fPoolKva), len = s->getLength();
            if (!fPoolRange || (uint8_t *)r->kva < fPoolKva || off + len > fPoolBytes) return kIOReturnBadArgument;
            OSAddAtomic64(-(SInt64)len, &fVramBytes);
            fPoolRange->deallocate(off, len);
            s->release();
            return kIOReturnSuccess;
        }
        IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, (OSObject *)r->handle);
        if (!b) return kIOReturnBadArgument;
        OSAddAtomic64(-(SInt64)b->getLength(), &fVramBytes);
        b->complete(); b->release();
        return kIOReturnSuccess;
    }
    IOReturn rmCall(const OSSymbol *fn, void *p1) {
        if (fn->isEqualTo(NVRM_GPUVA_FN_HANDLES)) {
            NVRMRmHandles *h = (NVRMRmHandles *)p1;
            if (!h || h->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
            h->kapiDevice = this; h->hClient = 0x50480001; h->hDevice = 0x50480002; h->hSubDevice = 0x50480003;
            return kIOReturnSuccess;
        }
        NVRMGpuVaRequest *g = (NVRMGpuVaRequest *)p1;
        if (!g || g->version != NVRM_GPUVA_ABI_VERSION) return kIOReturnBadArgument;
        if (fn->isEqualTo(NVRM_GPUVA_FN_ALLOC)) {
            const uint64_t len = round_page_64(g->size);
            uint64_t align = g->align > PAGE_SIZE ? g->align : PAGE_SIZE;
            if (align & (align - 1)) align = PAGE_SIZE;
            IORangeScalar off = 0;
            if (!fVa || !len || !fVa->allocate(len, &off, align)) { g->status = 1; return kIOReturnNoSpace; }
            g->gpuva = kVaBase + off; g->hVirt = (unsigned)OSIncrementAtomic(&fHandles) + 1; g->status = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(NVRM_GPUVA_FN_MAP)) {   // nothing to map: no GPU work reaches these surfaces
            g->hMemory = (unsigned)OSIncrementAtomic(&fHandles) + 1; g->status = 0;
            return kIOReturnSuccess;
        }
        if (fn->isEqualTo(NVRM_GPUVA_FN_FREE)) {
            if (!(g->flags & NVRM_GPUVA_FLAG_UNMAP_ONLY) && fVa && g->gpuva >= kVaBase && g->gpuva - kVaBase < kVaSize)
                fVa->deallocate(g->gpuva - kVaBase, round_page_64(g->size));
            g->status = 0;
            return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    }

    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        PhxAMDFB *me = (PhxAMDFB *)p0;
        if (me->fStopping) return;
        const uint64_t at = mach_absolute_time(), due = me->fVblNext;
        uint64_t near; nanoseconds_to_absolutetime(50000, &near);
        if (gVblStaged && due > at + near) { thread_call_enter_delayed(me->fVblTimer, due); return; }   // an early stage
        PhxVblStat &st = gVbl[me->fHead];
        if (due && at > due) {
            uint64_t ns; absolutetime_to_nanoseconds(at - due, &ns);
            const SInt64 us = (SInt64)(ns / 1000);
            OSAddAtomic64(us, &st.lateUsSum);
            if (us > st.lateUsMax) st.lateUsMax = us;
        }
        OSAddAtomic64(1, &st.n);
        if (me->fHead == 0) {   // the accelerator times head 0's presents against head 0's tick only
            gPhxVblLastAbs = due ? due : at;   // the vblank this tick stands for
            __atomic_add_fetch(&gPhxVblSeq, 1, __ATOMIC_RELEASE);
        }
        for (const Irq &q : me->fVbl) {
            const IOFBInterruptProc proc = __atomic_load_n(&q.proc, __ATOMIC_ACQUIRE);
            if (proc) { OSAddAtomic64(1, &st.procs); proc(q.target, q.ref); }
        }
        uint64_t period; nanoseconds_to_absolutetime(me->fFrameNs, &period);
        const uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        if (me->fHead != 0) me->lockToPipe(now);   // M7 S3b: head 1's tick on OTG1's vblank start
        uint64_t lead; nanoseconds_to_absolutetime(1500000, &lead);
        thread_call_enter_delayed(me->fVblTimer, gVblStaged ? me->fVblNext - lead : me->fVblNext);
    }

    // The firmware lit the panel at its native refresh, which the VBL timer and the mode must report: they said 60 Hz while
    // the panel scanned at 120 (OTG frame counter: 392 frames in 3.28 s), and WindowServer composited at 60 fps (2026-10-20).
    // Read-only: OTG0's H/V totals (DCN 3.1.4: segment 2 base 0x34c0 + 0x1b2a / 0x1b2f) and its frame counter (+0x1b4c)
    // over ~200 ms, snapped to a standard rate when within 2 %.
    void measureRefresh() {
        const phxhead::PipeRegs P = phxhead::pipeRegs(fHead);   // M7 S3b: head n measures OTG n (head 0: OTG0 as before)
        IOMemoryMap *mm = fPCI ? fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5) : nullptr;
        if (!mm || mm->getLength() < 0x5100u * 4) { OSSafeReleaseNULL(mm); PFBLOG("refresh: no MMIO view, 60 Hz assumed"); return; }
        volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
        const uint32_t ctl = r[P.otgControl], ht = r[P.hTotal] + 1, vt = r[P.vTotal] + 1;
        const uint32_t c0 = r[P.frame] & 0xffffff; const uint64_t t0 = mach_absolute_time();
        IOSleep(200);
        const uint32_t c1 = r[P.frame] & 0xffffff; const uint64_t t1 = mach_absolute_time();
        mm->release();
        uint64_t ns; absolutetime_to_nanoseconds(t1 - t0, &ns);
        const uint32_t frames = (c1 - c0) & 0xffffff;
        if (!(ctl & 1) || !ns || frames < 4 || ht < 100 || vt < 100) { PFBLOG("refresh: OTG%u not running (ctl 0x%x, %u frames), 60 Hz assumed", fHead, ctl, frames); return; }
        const uint32_t mhz = phxhead::refreshMilliHz(frames, ns);
        if (!mhz) { PFBLOG("refresh: OTG%u %u frames in %llu us, out of range - 60 Hz assumed", fHead, frames, ns / 1000); return; }
        fRefreshMilliHz = mhz; fHTotal = ht; fVTotal = vt;
        fFrameNs = 1000000000000ull / mhz;
        PFBLOG("refresh: OTG%u %ux%u totals, %u frames in %llu us -> %u.%03u Hz", fHead, ht, vt, frames, ns / 1000, mhz / 1000, mhz % 1000);
    }

    // M7 S3b, head 1 only, read only: put the next tick (fVblNext, free-running one period on) on OTG n's next vblank
    // start when that is within a quarter frame of it, and note whether HUBP n scans this head's surface. OTG n not
    // running (the ladder has not lit pipe 1, or undid it) = the free-running tick, exactly the S3a behaviour.
    void lockToPipe(uint64_t now) {
        if (!fMmio) return;
        const phxhead::PipeRegs P = phxhead::pipeRegs(fHead);
        const uint32_t ctl = R(P.otgControl);
        const uint32_t hc = R(P.hubpCntl), lo = R(P.addrLo), hi = R(P.addrHi), fb = R(phxhead::kRegFbLocationBase);
        const uint64_t bOff = fFlipOk && fBmc ? fBmc - ((uint64_t)(fb & 0xffffff) << 24) : ~0ull;   // S4a: B counts too
        gHead1Scan = phxhead::otgRunning(ctl) && (phxhead::scansSurface(hc, lo, hi, fb, (uint64_t)gHead1VramOff) ||
                                                  (bOff != ~0ull && phxhead::scansSurface(hc, lo, hi, fb, bOff))) ? 1 : 0;
        if (!phxhead::otgRunning(ctl) || fVblNext <= now) return;
        const uint64_t frameUs = fFrameNs / 1000;
        uint64_t candNs; absolutetime_to_nanoseconds(fVblNext - now, &candNs);
        const uint64_t posUs = phxhead::usToVblank(R(P.pos) & 0x7fff, R(P.vBlank) & 0x7fff, (R(P.vTotal) & 0x7fff) + 1, frameUs);
        const uint64_t us = phxhead::lockedNextUs(candNs / 1000, posUs, frameUs);
        if (us == candNs / 1000) return;
        uint64_t d; nanoseconds_to_absolutetime(us * 1000, &d);
        fVblNext = now + d;
        OSAddAtomic64(1, &gHead1Locked);
    }

    // The double buffer (gFlipOn[fHead]). Registers are absolute dwords in BAR5 (DCN 3.2 segment base 0x34c0 + dcn_3_2_0_offset.h).
    // M7 S4a: the HUBPREQ n / OTG n registers of this head (phxhead::flipRegs / pipeRegs; head 0's are the former fixed
    // constants, host test H7) and B's VRAM window (phxhead::flipBRange: head 0 16-48 MiB as before, head 1 72-80 MiB).
    enum : uint32_t { kRegFbBase = 0x3935, kFlipPending = 1u << 8, kFlipImmediate = 1u << 1 };
    phxhead::FlipRegs fFR = phxhead::flipRegs(0);
    phxhead::PipeRegs fPR = phxhead::pipeRegs(0);
    IOMemoryMap *fMmio = nullptr, *fBackMap = nullptr;
    IOLock *fFlipLock = nullptr;
    uint64_t fAmc = 0, fBmc = 0, fReq = 0;
    bool fFlipOk = false, fFrontB = false;

    volatile uint32_t &R(uint32_t dw) { return ((volatile uint32_t *)fMmio->getVirtualAddress())[dw]; }
    void flipWrite(uint64_t mc, bool immediate) {
        const uint32_t fc = R(fFR.flipCtl);
        R(fFR.flipCtl) = immediate ? (fc | kFlipImmediate) : (fc & ~kFlipImmediate);
        R(fFR.addrHi) = (uint32_t)(mc >> 32) & 0xffff;
        R(fFR.addrLo) = (uint32_t)mc;   // the low half arms the latch
        fReq = mc;
    }
    bool flipPending() {
        if (!fReq) return false;
        if (R(fFR.flipCtl) & kFlipPending) return true;
        return ((((uint64_t)(R(fFR.inuseHi) & 0xffff)) << 32) | R(fFR.inuseLo)) != fReq;
    }
    // microseconds until OTG0 reaches its vblank start, where the VUPDATE that latches a flip comes
    uint64_t usToVblank() {
        const uint32_t vt = fVTotal, v = R(fPR.pos) & 0x7fff, vbs = R(fPR.vBlank) & 0x7fff;
        if (!vt || v >= vt || vbs >= vt) return 200;
        return (uint64_t)((vbs + vt - v) % vt) * (fFrameNs / 1000) / vt;
    }
    // Called with fFlipLock held: true once the last flip has latched; past three frames the flip is given up.
    bool flipWaitLocked() {
        if (!flipPending()) return true;
        const uint64_t t0 = mach_absolute_time();
        uint64_t limit; nanoseconds_to_absolutetime(3 * fFrameNs, &limit);
        while (flipPending()) {
            if (mach_absolute_time() - t0 > limit) {
                OSAddAtomic64(1, &gFlipTimeouts[fHead]);
                PFBLOG("flip: 0x%llx never latched (flip control 0x%x, in use 0x%x_%08x) - back to A, flipping off",
                       fReq, R(fFR.flipCtl), R(fFR.inuseHi), R(fFR.inuseLo));
                flipWrite(fAmc, true); fFrontB = false; gFlipOn[fHead] = 0;
                return false;
            }
            const uint64_t us = usToVblank();
            if (us > 300) {
                uint64_t d; clock_interval_to_deadline((uint32_t)(us - 150), kMicrosecondScale, &d);
                IOLockSleepDeadline(fFlipLock, (event_t)&fReq, d, THREAD_UNINT);
            } else {
                IODelay(20);
            }
        }
        uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
        OSAddAtomic64(1, &gFlipWaitN); OSAddAtomic64((SInt64)(ns / 1000), &gFlipWaitUsSum);
        return true;
    }
    // Maps B and fills it with A, once. B's VRAM offset is past A and in 16-48 MiB; the GPU address the display reads
    // must sit as far from the scan-out's as B's CPU address from A's (FB_LOCATION_BASE is in 16 MiB units).
    bool flipSetupLocked() {
        if (fFlipOk) return true;
        if (!fMmio || !fSurfMap || !fRowBytes || !fHeight) return false;
        const uint64_t size = (uint64_t)fRowBytes * fHeight;
        uint64_t barStart = 0, barLen = 0;
        for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
            IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
            if (dm && fBase >= dm->getPhysicalAddress() && fBase - dm->getPhysicalAddress() < dm->getLength()) {
                barStart = dm->getPhysicalAddress(); barLen = dm->getLength(); break;
            }
        }
        const uint64_t aVram = fBase - barStart, fbMc = (uint64_t)(R(kRegFbBase) & 0xffffff) << 24;
        fAmc = ((uint64_t)(R(fFR.addrHi) & 0xffff) << 32) | R(fFR.addrLo);
        uint64_t bVram = (aVram + size + 0x1fffff) & ~0x1fffffull;
        const phxhead::Range bw = phxhead::flipBRange(fHead);
        if (bVram < bw.lo) bVram = bw.lo;
        const uint32_t pitchPx = (R(fFR.pitch) & 0x3fff) + 1;
        PFBLOG("flip: A at VRAM 0x%llx (MC 0x%llx, FB base 0x%llx), pitch %u px, B at VRAM 0x%llx, %llu bytes, BAR 0x%llx+0x%llx",
               aVram, fAmc, fbMc, pitchPx, bVram, size, barStart, barLen);
        if (!barLen || fAmc - fbMc != aVram || pitchPx * 4 != fRowBytes || bVram + size > bw.hi || bVram + size > barLen) {
            PFBLOG("flip: layout does not fit the double buffer - staying on the copy");
            return false;
        }
        if (!fBackMap)
            if (IODeviceMemory *dm = IODeviceMemory::withRange(barStart + bVram, size)) { fBackMap = dm->map(kIOMapWriteCombineCache); dm->release(); }
        if (!fBackMap) { PFBLOG("flip: B would not map"); return false; }
        const volatile uint64_t *a = (const volatile uint64_t *)fSurfMap->getVirtualAddress();
        volatile uint64_t *b = (volatile uint64_t *)fBackMap->getVirtualAddress();
        for (uint64_t i = 0; i < size / 8; i++) b[i] = a[i];
        __asm__ __volatile__("sfence" ::: "memory");
        (void)b[size / 8 - 1];   // a read after the writes: the posted writes reach VRAM before any flip
        fBmc = fbMc + bVram;
        fFlipOk = true;
        return true;
    }
    void flipHomeLocked() {
        if (!fFlipOk || (!fFrontB && (!fReq || fReq == fAmc))) { fFrontB = false; return; }
        flipWrite(fAmc, true);
        for (unsigned i = 0; i < 500 && flipPending(); i++) IODelay(100);
        fFrontB = false;
    }

public:
    int head0Resync();   // debug.phxfb_resync (boot rate): the id OTG0's totals are now, or < 0 (the sysctl's codes)
    // the flip bring-up sysctls (debug.phxfb_flip / _flip_test)
    int flipEnable(int on) {
        if (!fFlipLock || !fMmio) return ENXIO;
        IOLockLock(fFlipLock);
        int err = 0;
        if (on) { if (flipSetupLocked()) gFlipOn[fHead] = 1; else err = EIO; }
        else { gFlipOn[fHead] = 0; flipHomeLocked(); }
        IOLockUnlock(fFlipLock);
        PFBLOG("flip: head %u debug.phxfb%s_flip=%d (%s)", fHead, fHead ? "1" : "", gFlipOn[fHead], err ? "setup failed" : "ok");
        return err;
    }
    int flipTest(int what) {
        if (!fFlipLock || !fMmio) return ENXIO;
        if (gFlipOn[fHead]) return EBUSY;   // the per-frame flip owns the buffers
        IOLockLock(fFlipLock);
        int err = 0;
        uint64_t to = 0;
        if (what == 1) to = fFrontB ? fBmc : (fAmc ? fAmc : ((uint64_t)(R(fFR.addrHi) & 0xffff) << 32 | R(fFR.addrLo)));
        else if (what == 2) { if (flipSetupLocked()) to = fBmc; else err = EIO; }
        else if (what == 3) { if (fFlipOk) to = fAmc; else err = EINVAL; }
        else if (what == 4) {
            if (!fFlipOk) err = EINVAL;
            else {
                volatile uint32_t *b = (volatile uint32_t *)fBackMap->getVirtualAddress();
                for (uint32_t y = fHeight / 2 - 32; y < fHeight / 2 + 32; y++)
                    for (uint32_t x = 0; x < fWidth; x++) b[(uint64_t)y * (fRowBytes / 4) + x] = 0xffff0000u;
                __asm__ __volatile__("sfence" ::: "memory");
            }
        } else err = EINVAL;
        if (!err && to) {
            if (!fAmc) fAmc = to;
            const uint64_t t0 = mach_absolute_time();
            flipWrite(to, false);
            bool latched = false;
            for (unsigned i = 0; i < 5000; i++) { if (!flipPending()) { latched = true; break; } IODelay(10); }
            uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
            gFlipLatchUs = latched ? (SInt64)(ns / 1000) : -1;
            gFlipLatchLine = R(fPR.pos) & 0x7fff;
            if (latched) fFrontB = (to == fBmc && fBmc);
            else { flipWrite(fAmc, true); fFrontB = false; err = ETIMEDOUT; }
            PFBLOG("flip test %d: 0x%llx %s after %lld us at line %lld (flip control 0x%x, in use 0x%x_%08x)", what, to,
                   latched ? "latched" : "NEVER latched, back to A", gFlipLatchUs, gFlipLatchLine, R(fFR.flipCtl),
                   R(fFR.inuseHi), R(fFR.inuseLo));
        }
        IOLockUnlock(fFlipLock);
        return err;
    }
    // the display pipe's half (nvrm-accel.cpp performTransaction): the buffer to copy the next frame into, then the flip
    IOReturn flipBack(NVRMVramRequest *r) {
        if (!gFlipOn[fHead] || !fFlipOk || !fFlipLock) return kIOReturnUnsupported;
        IOLockLock(fFlipLock);
        IOReturn ret = kIOReturnUnsupported;
        if (gFlipOn[fHead] && fFlipOk && flipWaitLocked()) {
            r->kva = (void *)(fFrontB ? fSurfMap : fBackMap)->getVirtualAddress();
            r->phys = fBase; r->actualSize = round_page_64((uint64_t)fRowBytes * fHeight);
            r->width = fWidth; r->height = fHeight; r->pitch = fRowBytes;
            r->flags = __atomic_load_n(&fRot.reply, __ATOMIC_ACQUIRE);   // rotation S1: the copy's angle (0 = none)
            ret = kIOReturnSuccess;
        }
        IOLockUnlock(fFlipLock);
        return ret;
    }
    IOReturn flipCommit() {
        if (!fFlipLock) return kIOReturnUnsupported;
        IOLockLock(fFlipLock);
        IOReturn ret = kIOReturnUnsupported;
        if (gFlipOn[fHead] && fFlipOk) {
            const volatile uint32_t *back = (const volatile uint32_t *)(fFrontB ? fSurfMap : fBackMap)->getVirtualAddress();
            (void)back[(uint64_t)fRowBytes / 4 * fHeight - 1];   // the copy's posted writes reach VRAM before the flip
            flipWrite(fFrontB ? fAmc : fBmc, false);
            fFrontB = !fFrontB;
            OSAddAtomic64(1, &gFlipN[fHead]);
            ret = kIOReturnSuccess;
        }
        IOLockUnlock(fFlipLock);
        return ret;
    }

    // GPU present (PhxBringup's scanout ABI driven by the Metal plugin) flips HUBP0 to its own VRAM slots; the console
    // comes back to A when it lets go (its watchdog restores the console address). A copy into A or B meanwhile is
    // never seen: true while the programmed address is neither.
    uint64_t fAmcHome = 0;
    bool scanForeign() {
        if (!fMmio || !fPCI || !fBase) return false;
        if (!fAmcHome) {
            for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
                IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
                if (dm && fBase >= dm->getPhysicalAddress() && fBase - dm->getPhysicalAddress() < dm->getLength()) {
                    fAmcHome = ((uint64_t)(R(kRegFbBase) & 0xffffff) << 24) + (fBase - dm->getPhysicalAddress());
                    PFBLOG("scan: A is MC 0x%llx; a copy is skipped while HUBP0 scans anything but A or B", fAmcHome);
                    break;
                }
            }
            if (!fAmcHome) return false;
        }
        const uint64_t cur = ((uint64_t)(R(fFR.addrHi) & 0xffff) << 32) | R(fFR.addrLo);
        return cur && cur != fAmcHome && (!fBmc || cur != fBmc);
    }

    // hot-plug hp0 (phxhotplug0.cpp): head 0's read-only register view (only phxhp0::readAllowed registers; 0xffffffff when
    // unmapped or out of the BAR), the EDID it serves, and one live EDID read of a DP source through PhxBringup (b79+ phxEdidRead)
    uint32_t hp0Read(uint32_t dw) {
        if (!fMmio || !phxhp0::readAllowed(dw) || ((uint64_t)dw + 1) * 4 > fMmio->getLength()) return 0xffffffffu;
        return R(dw);
    }
    uint32_t hp0Served(uint8_t out[256]) const { const uint32_t n = fEdidLen < 256 ? fEdidLen : 256; memcpy(out, fEdid, n); return n; }
    int hp0EdidLive(uint32_t src, uint8_t out[256], uint32_t *len);
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool startHead(IOService *provider, unsigned head);   // M7 S3a: the phantom second head (head >= 1)
    void setOnline(bool on);
    int sample(uint32_t out[7]);
    int shot(struct sysctl_req *req);
    IOReturn enableController() override { return kIOReturnSuccess; }
    bool isConsoleDevice() override { return fHead == 0 && !fBeside; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override {
        if (aperture != kIOFBSystemAperture) return nullptr;
        return IODeviceMemory::withRange(fBase, (IOPhysicalLength)fRowBytes * fHeight);
    }
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    // head 0's offer gate (phxoffer.h): while closed only the lit mode is listed. getDisplayModes never writes more entries
    // than the count it follows reported (IOFramebuffer sizes the array by that count; the gate may open in between).
    bool offerClosed() const { return fHead == 0 && fNModes && !gHead0OfferOpen; }
    volatile UInt32 fOfferCount = 0;
    IOItemCount getDisplayModeCount() override {
        uint32_t tmp[phxhead::kNMaxModes];
        const unsigned k = fNModes ? phxoffer::offered(fModeIds, fNModes, fModeId, offerClosed(), tmp) : 1;
        fOfferCount = k;
        return k;
    }
    IOReturn getDisplayModes(IODisplayModeID *modes) override {
        if (!fNModes) { modes[0] = 1; return kIOReturnSuccess; }
        uint32_t tmp[phxhead::kNMaxModes];
        unsigned k = phxoffer::offered(fModeIds, fNModes, fModeId, offerClosed(), tmp);
        if (fOfferCount && k > fOfferCount) k = fOfferCount;
        for (unsigned i = 0; i < k; i++) modes[i] = (IODisplayModeID)tmp[i];
        return kIOReturnSuccess;
    }
    void head0OfferOpen(const char *why);
    // mode switch: each listed mode reports its plan's nominal rate; only the mode lit at start is the default (R0 D3)
    IOOptionBits modeFlags(IODisplayModeID mode) const {
        return kDisplayModeValidFlag | kDisplayModeSafeFlag |
               (!fNModes || (uint32_t)mode == phxRotDefaultId(fHead, fBootModeId, fModeId) ? kDisplayModeDefaultFlag : 0);   // rotation rot2
    }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        if (phxRotOwns(fRot, fHead, mode)) return phxRotInfo(fRot, fHead, mode, info, rotModes().single);
        if (!listed(mode) || !info) return kIOReturnBadArgument;
        bzero(info, sizeof(*info));
        info->maxDepthIndex = 0; info->nominalWidth = fWidth; info->nominalHeight = fHeight;
        const phxhead::HeadMode *hm = fNModes ? phxhead::headMode(fHead, (uint32_t)mode) : nullptr;
        const uint32_t mhz = hm ? hm->milliHz : fRefreshMilliHz;
        info->refreshRate = (IOFixed1616)(((uint64_t)mhz << 16) / 1000); info->flags = modeFlags(mode);
        return kIOReturnSuccess;
    }
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override {
        if (phxRotOwns(fRot, fHead, mode)) return phxRotPixelInfo(fRot, mode, depth, aperture, pi);
        if (!listed(mode) || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
        bzero(pi, sizeof(*pi));
        pi->bytesPerRow = fRowBytes; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3; pi->bitsPerComponent = 8;
        pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->activeWidth = fWidth; pi->activeHeight = fHeight;
        return kIOReturnSuccess;
    }
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override {
        // rot14: a rotated id only while its plan is still the raster (a resync may have moved fModeId, phxrot::currentId)
        *mode = (IODisplayModeID)phxrot::currentId(fRot.curValid, fRot.cur.id, fRot.cur.plan, fNModes ? fModeId : 1); *depth = 0;
        return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    // rotation S1: a rotated mode is its plan's raster (the pipe switches only when the rate differs), then the angle
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override {
        const bool rot = phxRotOwns(fRot, fHead, mode);
        const IODisplayModeID hw = rot ? (IODisplayModeID)phxRotPlan(fRot, mode) : mode;
        IOReturn r;
        if (!fNModes) r = (hw == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported;
        else if (depth != 0 || !listed(hw)) r = kIOReturnUnsupportedMode;
        else if (fHead) r = switchHead1Mode(hw);
        else { OSIncrementAtomic(&gHead0SwBusy); r = switchHead0Mode(hw); OSDecrementAtomic(&gHead0SwBusy); }   // hp0 holds meanwhile
        if (r == kIOReturnSuccess && (rot || fRot.curValid)) phxRotSelect(fRot, fHead, rot ? (uint32_t)mode : 0);
        return r;
    }
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        if (phxRotOwns(fRot, fHead, mode)) return phxRotTimingInfo(fRot, mode, info);
        if (!listed(mode) || !info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;   // not an Apple standard timing
        // head 0 with its mode list (F1): the mode's AOC EDID DTD (phxhead::detailedTiming), so a startup-timing is saved and
        // a head-1 connect change re-applies it instead of the default 60;
        // otherwise no detailed timing: the panel keeps the firmware's
        phxhead::DetailedTiming t;
        // rotation S1: head 1's listed modes too while debug.phxfb_rotate names it (the EDID's DTD, phxRotPlanTiming)
        bool have = fNModes && (fHead ? phxRotPlanTiming(rotModes(), (uint32_t)mode, &t) : phxhead::detailedTiming(0, (uint32_t)mode, &t));
        // rot12: the one-mode head 0 reports its GOP mode's EDID timing while it may rotate (none before, as with gate 0)
        if (!have && !fHead && !fNModes && fSingleOk && mode == 1 && phxRotGate(0)) { t = fSingleT; have = true; }
        if (have) {
            IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
            d.pixelClock = d.minPixelClock = d.maxPixelClock = t.pixelHz;
            d.horizontalActive = t.hActive; d.horizontalBlanking = t.hBlank;
            d.horizontalSyncOffset = t.hSyncOffset; d.horizontalSyncPulseWidth = t.hSyncWidth;
            d.verticalActive = t.vActive; d.verticalBlanking = t.vBlank;
            d.verticalSyncOffset = t.vSyncOffset; d.verticalSyncPulseWidth = t.vSyncWidth;
            d.horizontalSyncConfig = t.hSyncPos ? kIOSyncPositivePolarity : 0;
            d.verticalSyncConfig = t.vSyncPos ? kIOSyncPositivePolarity : 0;
            // rot5 / rot9: while a head is gated, a head's other modes leave CoreDisplay's default-mode choice (phxrot::h0Vbe,
            // h1Vbe), so the current one stays the default at a new display set
            d.verticalBlankingExtension = phxRotH0Vbe(fHead, (uint32_t)mode, fModeId);
            info->flags = kIODetailedTimingValid;
        }
        return kIOReturnSuccess;
    }
    IOItemCount getConnectionCount() override { return 1; }
    // rotation S1: CoreDisplay's rotated modes. Not gated = IOFramebuffer's own answers (unsupported), as before.
    IOReturn validateDetailedTiming(void *desc, IOByteCount size) override {
        return phxRotGate(fHead) ? phxRotValidate(rotModes(), desc, size) : IOFramebuffer::validateDetailedTiming(desc, size);
    }
    IOReturn setDetailedTimings(OSArray *array) override {
        if (phxRotGate(fHead)) return phxRotSetTimings(fRot, rotModes(), array);
        phxRotClear(fRot);
        return IOFramebuffer::setDetailedTimings(array);
    }
    // EDID / DDC so CoreDisplay has real display attributes (external monitor): without these it SIGABRTs.
    bool hasDDCConnect(IOIndex connectIndex) override { return connectIndex == 0; }
    // One live read through PhxBringup's "phxEdidRead" platform function (b79+, which marks itself with phx-edid-fn;
    // bounded: <= ~100 ms waiting for debug.phxedid, <= ~100 ms of DDC), validated by PhxEdid.h; any failure keeps
    // kPhxEDID. Publishes what getDDCBlock will serve: phx-edid (bytes), phx-edid-live (bool), phx-edid-why
    // (phxedid::kWhy*, 0 = live), phx-edid-rc (the read's PhxDdc.h result), phx-edid-src (the source it read).
    // M7 S3a: head 0 asks for the b80 rule (in-value 0, what every b79/b80 caller passed); head 1 asks kext b81+ (property
    // phx-edid-req) for the other monitor (phxddc::kSrcOther) and, when that read fails, serves the fallback of the monitor
    // its source names (phxhead::fallbackFor: DDC1 = kPhxEDID, AUX = the AOC capture). An older kext would read pipe 0's
    // monitor for head 1, so head 1 does not ask it (why kWhyNoProvider, the AOC fallback). Head 0 falls back by its
    // source too (phxhead::fallbackForHead; it kept kPhxEDID, the Acer's, even for the AOC on pipe 0).
    void loadEdid() {
        const bool second = fHead != 0;
        uint8_t live[256] = {};
        uint32_t len = 0, src = second ? phxddc::kSrcOther : 0;
        int32_t rc = 0;
        int why = phxedid::kWhyNoProvider;
        char a[8];
        if (PE_parse_boot_argn("-phxedidstatic", a, sizeof a)) why = phxedid::kWhyDisabled;
        else if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
            while (OSObject *o = it->getNextObject()) {
                IOService *s = OSDynamicCast(IOService, o);
                if (!s || s->getProperty("phx-edid-fn") != kOSBooleanTrue) continue;
                if (second && s->getProperty("phx-edid-req") != kOSBooleanTrue) break;   // kext older than b81
                const IOReturn r = s->callPlatformFunction("phxEdidRead", false, live, &len, &rc, &src);
                why = r == kIOReturnSuccess ? (rc ? phxedid::kWhyReadFailed : phxedid::kLive)
                                            : r == kIOReturnBusy ? phxedid::kWhyBusy : phxedid::kWhyNoProvider;
                break;
            }
            it->release();
        }
        const bool aoc = phxhead::fallbackForHead(fHead, phxddc::validSource(src), src) == phxhead::kFallbackAoc;
        const uint8_t *fallback = aoc ? phxhead::kAocEdid : kPhxEDID;
        fEdidLen = (uint32_t)phxedid::choose(why, live, len, fallback, aoc ? sizeof phxhead::kAocEdid : sizeof kPhxEDID, fEdid, &why);
        if (second) { gHead1EdidSrc = phxddc::validSource(src) ? (int)src : -1; gHead1EdidWhy = why; }
        if (OSData *d = OSData::withBytes(fEdid, fEdidLen)) { setProperty("phx-edid", d); d->release(); }
        setProperty("phx-edid-live", why == phxedid::kLive ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("phx-edid-why", (unsigned long long)why, 32);
        setProperty("phx-edid-rc", (unsigned long long)(uint32_t)rc, 32);
        setProperty("phx-edid-src", src, 32);
        PFBLOG("EDID: %s (why %d, rc %d, src 0x%x, read %u bytes), serving %u bytes, mfg %02x%02x product %02x%02x%s",
               why == phxedid::kLive ? "live" : "static kPhxEDID", why, (int)rc, src, len, fEdidLen, fEdid[8], fEdid[9],
               fEdid[11], fEdid[10], fEdidLen == sizeof kPhxEDID && !memcmp(fEdid, kPhxEDID, fEdidLen) ? ", = kPhxEDID" : "");
    }
    IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect /*blockType*/,
                         IOOptionBits /*options*/, UInt8 *data, IOByteCount *length) override {
        if (connectIndex != 0 || !data || !length) return kIOReturnBadArgument;
        if (!fEdidLen) { memcpy(fEdid, kPhxEDID, sizeof kPhxEDID); fEdidLen = sizeof kPhxEDID; }   // start() not run yet
        if (blockNumber < 1 || blockNumber * 128 > fEdidLen) return kIOReturnUnsupported;
        IOByteCount want = (*length && *length < 128) ? *length : 128;
        memcpy(data, fEdid + (blockNumber - 1) * 128, want);
        *length = want;
        PFBLOG("getDDCBlock %u -> %llu bytes", (unsigned)blockNumber, (unsigned long long)want);
        return kIOReturnSuccess;
    }
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        if (type == kIOFBVBLInterruptType) {
            for (Irq &q : fVbl) if (!q.proc) {
                q.target = target; q.ref = ref; __atomic_store_n(&q.proc, proc, __ATOMIC_RELEASE);
                OSAddAtomic64(1, &gVbl[fHead].handlers);
                PFBLOG("VBL handler %u: %s", (unsigned)(&q - fVbl), target ? target->getMetaClass()->getClassName() : "(no target)");
                if (iref) *iref = &q;
                return kIOReturnSuccess;
            }
            PFBLOG("VBL handler refused: all %u taken", (unsigned)(sizeof fVbl / sizeof fVbl[0]));
            return kIOReturnNoResources;
        }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *iref) override {
        for (Irq &q : fVbl) if (iref == &q) __atomic_store_n(&q.proc, (IOFBInterruptProc)nullptr, __ATOMIC_RELEASE);
        return kIOReturnSuccess;
    }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32 ch, UInt32 n, UInt32 w, void *d) override { return gammaSet(ch, n, w, d, false); }
    IOReturn setGammaTable(UInt32 ch, UInt32 n, UInt32 w, void *d, bool sync) override { return gammaSet(ch, n, w, d, sync); }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    // power and connection answers as NullMoth's NMIntelFB/NVRMFB give them: acknowledge at once, never queue on the
    // controller thread (a base-class power change waited 45 s for an acknowledge and froze WindowServer restarts)
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            if (value) { IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn);
                         return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        switch (attribute) {
        case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
        if (idx != 0) return kIOReturnBadArgument;
        // IODisplay asks the capability selectors with a NULL value (readFramebufferEDID: kConnectionSupportsHLDDCSense;
        // IODisplay::start: kConnectionSupportsAppleSense); refusing NULL kept it from ever calling getDDCBlock (M6 step 3b)
        if (attr == kConnectionSupportsHLDDCSense) return kIOReturnSuccess;   // we implement hasDDCConnect()/getDDCBlock()
        if (!value) return attr == kConnectionSupportsAppleSense ? kIOReturnUnsupported : kIOReturnBadArgument;
        switch (attr) {
        case kConnectionEnable: case kConnectionCheckEnable: *value = fOnline ? 1 : 0; return kIOReturnSuccess;   // hp0: head 0 too
        // External (0), as Cobalt64's Navi48Framebuffer: the monitor is on HDMI. kIOConnectionBuiltIn (inherited from
        // nullarc's eDP panel) made macOS label it "Built-in Display" (M6 step 3).
        case kConnectionFlags: *value = 0; return kIOReturnSuccess;
        case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
        case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
            *value = 0x00000002; return kIOReturnSuccess;
        default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
        }
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID mode, IOOptionBits *flags) override {
        if (flags && phxRotOwns(fRot, fHead, mode)) { *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag; return kIOReturnSuccess; }
        if (flags) *flags = fNModes ? (listed(mode) && (!offerClosed() || phxoffer::isOffered(fModeIds, fNModes, fModeId, true, (uint32_t)mode))
                                       ? modeFlags(mode) : 0) : kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    // The accelerator's display pipe asks for the scan-out: its CPU mapping (frames are copied in) and its physical
    // address in the GPU's VRAM aperture. With no address the pipe got no framebuffer resource, and IOAcceleratorFamily
    // marks a display pipe active only once one exists (init_framebuffer_resource sets +0x298): every WindowServer
    // transaction ended kIOReturnNotReady and no frame reached the panel (2026-10-20).
    // The scan-out through the carve-out, once PhxBringup has published where the carve-out sits (phx-fb-phys) and only
    // if that view holds the pixels the BAR shows. NULL (copy through the BAR) until then.
    IOMemoryMap *aliasMap() {
        if (fAliasMap || fAliasTries >= 3) return fAliasMap;
        if (!OSCompareAndSwap(0, 1, &fAliasBusy)) return nullptr;
        if (!fAliasMap && fAliasTries < 3) {
            fAliasTries++;
            uint64_t fbPhys = 0, barOff = ~0ull;
            if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
                while (OSObject *o = it->getNextObject()) {
                    IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
                    OSNumber *n = e ? OSDynamicCast(OSNumber, e->getProperty("phx-fb-phys")) : nullptr;
                    if (n) { fbPhys = n->unsigned64BitValue(); break; }
                }
                it->release();
            }
            for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
                IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
                if (dm && fBase >= dm->getPhysicalAddress() && fBase - dm->getPhysicalAddress() < dm->getLength()) {
                    barOff = fBase - dm->getPhysicalAddress();
                    break;
                }
            }
            const uint64_t phys = fbPhys + barOff, bytes = (uint64_t)fRowBytes * fHeight;
            IOMemoryMap *m = nullptr;
            if (fbPhys && barOff != ~0ull)
                if (IODeviceMemory *dm = IODeviceMemory::withRange(phys, bytes)) { m = dm->map(kIOMapWriteCombineCache); dm->release(); }
            if (m) {
                const volatile uint32_t *a = (const volatile uint32_t *)m->getVirtualAddress();
                const volatile uint32_t *b = (const volatile uint32_t *)fSurfMap->getVirtualAddress();
                const uint64_t words = bytes / 4, step = words / 4096 ? words / 4096 : 1;
                unsigned same = 0, nz = 0, n = 0;
                for (uint64_t w = 0; w < words; w += step, n++) { const uint32_t x = a[w], y = b[w]; same += x == y; nz += y != 0; }
                PFBLOG("alias: scan-out at BAR offset 0x%llx = system RAM 0x%llx: %u of %u samples match the BAR (%u non-zero)",
                       barOff, phys, same, n, nz);
                if (same == n && nz) fAliasMap = m; else m->release();
            } else {
                PFBLOG("alias: carve-out 0x%llx, scan-out BAR offset 0x%llx: no system-RAM view, copying through the BAR", fbPhys, barOff);
            }
        }
        fAliasBusy = 0;
        return fAliasMap;
    }
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override {
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_SCANOUT)) {
            NVRMVramRequest *r = (NVRMVramRequest *)p1;
            if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
            if (!fSurfMap) return kIOReturnNotReady;
            IOMemoryMap *alias = (gAlias && fHead == 0) ? aliasMap() : nullptr;   // head 1: its BAR mapping only
            r->kva = (void *)(alias ? alias : fSurfMap)->getVirtualAddress();
            r->phys = fBase; r->actualSize = round_page_64((uint64_t)fRowBytes * fHeight);
            r->width = fWidth; r->height = fHeight; r->pitch = fRowBytes;
            r->flags = __atomic_load_n(&fRot.reply, __ATOMIC_ACQUIRE);   // rotation S1: the copy's angle (0 = none)
            return kIOReturnSuccess;
        }
        // head 1 has no VRAM pool (since S4a it has its own double buffer, debug.phxfb1_flip): the accelerator's allocations and RM
        // handles stay head 0's (askFramebufferForVram uses fFB), and its display pipe copies into head 1's surface
        if (fHead != 0 && fn && (fn->isEqualTo(NVRM_VRAM_FN_ALLOC) || fn->isEqualTo(NVRM_VRAM_FN_FREE) ||
                                 fn->isEqualTo(NVRM_GPUVA_FN_HANDLES) || fn->isEqualTo(NVRM_GPUVA_FN_ALLOC) ||
                                 fn->isEqualTo(NVRM_GPUVA_FN_MAP) || fn->isEqualTo(NVRM_GPUVA_FN_FREE)))
            return kIOReturnUnsupported;
        if (fHead != 0 && fn && fn->isEqualTo(PHX_FB_FN_SCAN_FOREIGN)) return kIOReturnNotFound;   // always copied
        if (fn && fn->isEqualTo(PHX_FB_FN_FLIP_BACK)) {
            NVRMVramRequest *r = (NVRMVramRequest *)p1;
            if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
            return flipBack(r);
        }
        if (fn && fn->isEqualTo(PHX_FB_FN_FLIP_COMMIT)) return flipCommit();
        if (fn && fn->isEqualTo(PHX_FB_FN_SCAN_FOREIGN)) return scanForeign() ? kIOReturnSuccess : kIOReturnNotFound;
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_ALLOC)) return vramAlloc((NVRMVramRequest *)p1);
        if (fn && fn->isEqualTo(NVRM_VRAM_FN_FREE)) return vramFree((NVRMVramRequest *)p1);
        if (fn && (fn->isEqualTo(NVRM_GPUVA_FN_HANDLES) || fn->isEqualTo(NVRM_GPUVA_FN_ALLOC) || fn->isEqualTo(NVRM_GPUVA_FN_MAP) ||
                   fn->isEqualTo(NVRM_GPUVA_FN_FREE)))
            return rmCall(fn, p1);
        return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
    }
};

OSDefineMetaClassAndStructors(PhxAMDFB, IOFramebuffer)

static PhxAMDFB *gPhxFB;   // the panel the flip sysctls act on (set while started)

// M7 S3a: the started heads, by index, for PhxAGDC's framebuffer list and NMXAccel's debug.phxhead2. Head 0 first.
static PhxAMDFB *gHeads[phxhead::kMaxHeads];
static IOLock *gHeadsLock;
static IOLock *headsLock()
{
    if (!gHeadsLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gHeadsLock)) IOLockFree(l); }
    return gHeadsLock;
}
static void headsSet(unsigned i, PhxAMDFB *fb)
{
    if (i >= phxhead::kMaxHeads) return;
    IOLock *l = headsLock();
    if (l) IOLockLock(l);
    gHeads[i] = fb;
    if (l) IOLockUnlock(l);
}
// Retained copies of the heads WindowServer should get, in index order: head 0 (if started), then each later head that
// is started and reports a monitor. 0 when head 0 is not up. The caller releases them.
unsigned phxFbCopyHeads(IOService **out, unsigned max)
{
    IOLock *l = headsLock();
    unsigned n = 0;
    if (!out || !max) return 0;
    if (l) IOLockLock(l);
    if (gHeads[0]) {
        for (unsigned i = 0; i < phxhead::kMaxHeads && n < max; i++) {
            PhxAMDFB *fb = gHeads[i];
            if (!fb || (i && gHead1Online == 0)) continue;
            fb->retain(); out[n++] = fb;
        }
    }
    if (l) IOLockUnlock(l);
    return n;
}
// NMXAccel's debug.phxhead2=0: head 1 reports no monitor from now on (the connect interrupt tells IOFramebuffer).
// hp0: head 0 too (phxhotplug0.cpp). A head goes offline only while the other one stays online (phxhp0::mayGoOffline,
// EBUSY otherwise): the check and the change run under one lock, so two hot-plug policies (PhxBringup's for head 1, ours
// for head 0) can never take both heads offline at once.
static IOLock *gOnlineLock;
int phxFbSetHeadOnline(unsigned i, bool on)
{
    if (!gOnlineLock) { IOLock *n = IOLockAlloc(); if (n && !OSCompareAndSwapPtr(nullptr, n, (void *volatile *)&gOnlineLock)) IOLockFree(n); }
    if (!gOnlineLock) return ENOMEM;
    IOLock *l = headsLock();
    PhxAMDFB *fb = nullptr;
    IOLockLock(gOnlineLock);
    if (l) IOLockLock(l);
    if (i < phxhead::kMaxHeads && (fb = gHeads[i])) fb->retain();
    const bool h1Started = gHeads[1] != nullptr;
    if (l) IOLockUnlock(l);
    int err = 0;
    if (!fb) err = ENXIO;
    else if (!on && !phxhp0::mayGoOffline(i, gHead0Online != 0, h1Started, gHead1Online != 0)) err = EBUSY;
    else fb->setOnline(on);
    IOLockUnlock(gOnlineLock);
    if (fb) fb->release();
    if (err == EBUSY) PFBLOG("head %u offline refused: the other head is not online (never zero online displays)", i);
    return err;
}
// Hot-plug A2 (NMXAccel phxHead1Hotplug, phxhotplug.cpp): whether started head i (>= 1) reports a monitor, and the EDID it
// serves (phx-edid, copied, at most max bytes). ENXIO = not started.
int phxFbHeadInfo(unsigned i, int *online, uint8_t *edid, uint32_t max, uint32_t *len)
{
    IOLock *l = headsLock();
    PhxAMDFB *fb = nullptr;
    if (l) IOLockLock(l);
    if (i && i < phxhead::kMaxHeads && (fb = gHeads[i])) fb->retain();
    if (l) IOLockUnlock(l);
    if (!fb) return ENXIO;
    if (online) *online = gHead1Online;   // head 1 only (the one head >= 1 there is); setOnline / startHead keep it
    if (len) *len = 0;
    if (edid && len && max)
        if (OSData *d = OSDynamicCast(OSData, fb->getProperty("phx-edid"))) {
            const uint32_t n = d->getLength() < max ? (uint32_t)d->getLength() : max;
            memcpy(edid, d->getBytesNoCopy(), n);
            *len = n;
        }
    fb->release();
    return 0;
}
#include "phxhotplug0.cpp"   // hot-plug hp0: head 0's HPD poll, debug.phxhotplug0 / _stat, debug.phxhead0_online, debug.phxfb0_online
static int phxfb1ShotSysctl SYSCTL_HANDLER_ARGS
{
    if (proc_suser(req->p)) return EPERM;   // a picture of the desktop: root only, like debug.phxshot's use
    IOLock *l = headsLock();
    PhxAMDFB *fb = nullptr;
    if (l) IOLockLock(l);
    if ((fb = gHeads[1])) fb->retain();
    if (l) IOLockUnlock(l);
    if (!fb) return ENXIO;
    const int err = fb->shot(req);
    fb->release();
    return err;
}
static int phxfb1SumSysctl SYSCTL_HANDLER_ARGS
{
    uint32_t v[7] = { 0x4d533150u, 0, 0, 0, 0, 0, 0 };   // 'P1SM'
    IOLock *l = headsLock();
    PhxAMDFB *fb = nullptr;
    if (l) IOLockLock(l);
    if ((fb = gHeads[1])) fb->retain();
    if (l) IOLockUnlock(l);
    if (fb) { fb->sample(v); fb->release(); }
    return SYSCTL_OUT(req, v, sizeof v);
}
static int phxfbFlipSysctl SYSCTL_HANDLER_ARGS
{
    int v = gFlipOn[0];
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    return gPhxFB ? gPhxFB->flipEnable(v != 0) : ENXIO;
}
static int phxfb1FlipSysctl SYSCTL_HANDLER_ARGS
{
    int v = gFlipOn[1];
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    IOLock *l = headsLock();
    PhxAMDFB *fb = nullptr;
    if (l) IOLockLock(l);
    if ((fb = gHeads[1])) fb->retain();
    if (l) IOLockUnlock(l);
    if (!fb) return ENXIO;
    err = fb->flipEnable(v != 0);
    fb->release();
    return err;
}
static int phxfbFlipTestSysctl SYSCTL_HANDLER_ARGS
{
    int v = 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    return gPhxFB ? gPhxFB->flipTest(v) : ENXIO;
}
// Opening the offer gate: every listed mode is offered from now on, and the IOFBConfig mode cache CoreDisplay stored on
// head 0 goes, so the next WindowServer (the daemon's restart) builds head 0's list again (CoreDisplay IOFBBuildModeList
// reuses that property when it exists). A running WindowServer keeps its own list.
void PhxAMDFB::head0OfferOpen(const char *why)
{
    if (fHead != 0 || gHead0OfferOpen) return;
    gHead0OfferOpen = 1;
    OSAddAtomic64(1, &gHead0OfferOpens);
    const bool had = getProperty("IOFBConfig") != nullptr;
    removeProperty("IOFBConfig");
    PFBLOG("head 0 offer gate open (%s): %u modes offered, IOFBConfig %s", why ? why : "?", fNModes, had ? "dropped" : "absent");
}
static int phxfb0OfferSysctl SYSCTL_HANDLER_ARGS
{
    int v = gHead0OfferOpen;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 1) return EINVAL;   // the gate only opens; it closes at a head-0 start
    if (!gPhxFB) return ENXIO;
    gPhxFB->head0OfferOpen("debug.phxfb0_offer=1");
    return 0;
}
static int phxfbResyncSysctl SYSCTL_HANDLER_ARGS
{
    int v = gHead0ResyncLast;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (proc_suser(req->p)) return EPERM;
    if (v != 1) return EINVAL;
    static volatile UInt32 busy = 0;
    if (!OSCompareAndSwap(0, 1, &busy)) { gHead0ResyncLast = -4; return EBUSY; }
    const int r = gPhxFB ? gPhxFB->head0Resync() : -1;
    OSCompareAndSwap(1, 0, &busy);
    gHead0ResyncLast = r;
    OSAddAtomic64(1, &gHead0Resyncs);
    return r > 0 ? 0 : r == -1 ? ENXIO : r == -2 ? ENODEV : EIO;
}

// One boot per arming: `nvram phxfb-boot=1` lets the next start take the panel, and start removes the variable and
// commits NVRAM at once, so a boot that panics after this point comes back on the firmware framebuffer without hands.
static bool takeBootArming()
{
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return false;
    OSObject *o = opts->copyProperty("phxfb-boot");
    const bool armed = o != nullptr;
    bool stuck = false;
    OSSafeReleaseNULL(o);
    if (armed) {
        opts->removeProperty("phxfb-boot");
        // the forced commit: the throttled one (IONVRAM-SYNCNOW) left the variable in flash once, and the boot after a
        // panic took the panel again (2026-10-20)
        if (OSString *why = OSString::withCString("phxfb")) { opts->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", why); why->release(); }
        o = opts->copyProperty("phxfb-boot");
        stuck = o != nullptr;
        PFBLOG("NVRAM phxfb-boot taken (%s)", stuck ? "STILL SET: removal refused, not taking the panel" : "removed and committed");
        OSSafeReleaseNULL(o);
    }
    opts->release();
    return armed && !stuck;
}

bool PhxAMDFB::start(IOService *provider)
{
    char arg[8];
    if (PE_parse_boot_argn("-phxfboff", arg, sizeof arg)) { PFBLOG("-phxfboff: leaving the firmware framebuffer"); return false; }
    // M7 S3a: a PhxDisplay nub with phx-display-index >= 1 is a second head (published only by debug.phxhead2=1); head 0's
    // nub carries no such property and everything below is head 0's path as before
    if (OSNumber *hn = provider ? OSDynamicCast(OSNumber, provider->getProperty("phx-display-index")) : nullptr)
        if (hn->unsigned32BitValue() != 0) return startHead(provider, hn->unsigned32BitValue());
    if (!takeBootArming()) { PFBLOG("NVRAM phxfb-boot not set: leaving the firmware framebuffer this boot"); return false; }
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI && provider) fPCI = OSDynamicCast(IOPCIDevice, provider->getProvider());   // provider is the PhxDisplay nub
    if (!fPCI) return false;
    // NO in-start() gate: blocking here held the IOKit matching thread and starved the accelerator / the Metal device
    // (RE'd 2026-10-24: deadlock, then "Metal device never came up"). Instead, this personality matches only on the
    // IOPCIDevice property "phxfb-go", which the phxboot daemon publishes (sysctl debug.phxfb_go=1) AFTER it has brought
    // up the GPU, claimed it, and confirmed a LIVE Metal device. So start() runs only when Metal is already ready; it
    // takes the panel immediately, then the daemon does one clean WindowServer restart so WindowServer enumerates Metal
    // fresh and pairs the display to our device (Cobalt64's proven ordering: fb -> accelerator+MetalPluginName -> arm
    // -> one killall WindowServer). Metal caches an empty device list per process if asked too early, hence the restart.
    if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject())
            if (o != this && OSDynamicCast(IOFramebuffer, o)) fBeside = true;
        it->release();
    }
    PE_Video v;
    bzero(&v, sizeof(v));
    IOPlatformExpert *pe = getPlatform();
    if (!pe || pe->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr || v.v_depth != 32 || !v.v_width || !v.v_height ||
        v.v_rowBytes < v.v_width * 4 || v.v_width > 16384 || v.v_height > 16384) {
        PFBLOG("no usable boot console (base 0x%lx %lux%lu depth %lu row %lu): leaving the firmware framebuffer",
               v.v_baseAddr, v.v_width, v.v_height, v.v_depth, v.v_rowBytes);
        return false;
    }
    fWidth = (uint32_t)v.v_width; fHeight = (uint32_t)v.v_height; fRowBytes = (uint32_t)v.v_rowBytes;
    // once the video console is up, the platform expert hands out the console's kernel mapping, not the physical
    // address. The GOP surface sits in one of the GPU's BARs (VRAM aperture): find the BAR whose start holds the same
    // pixels as the console mapping. (A descriptor over the mapping would need prepare(): getPhysicalSegment on an
    // unwired one panicked, 2026-10-20.)
    uint64_t base = v.v_baseAddr;
    if (base >= 0xFFFF800000000000ull) {
        const volatile uint32_t *con = (const volatile uint32_t *)(uintptr_t)base;
        const uint32_t probeWords = 64u * 1024u / 4u;
        uint64_t found = 0;
        for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount() && !found; i++) {
            IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
            if (!dm || dm->getLength() < (IOByteCount)fRowBytes * fHeight) continue;
            IODeviceMemory *sub = IODeviceMemory::withSubRange(dm, 0, 64u * 1024u);
            IOMemoryMap *m = sub ? sub->map(kIOMapInhibitCache) : nullptr;
            OSSafeReleaseNULL(sub);
            if (!m) continue;
            const volatile uint32_t *bar = (const volatile uint32_t *)m->getVirtualAddress();
            uint32_t same = 0, nz = 0;
            for (uint32_t w = 0; w < probeWords; w++) { const uint32_t a = con[w], b = bar[w]; same += a == b; nz += a != 0; }
            PFBLOG("BAR index %u at 0x%llx: %u of %u words match the console (%u non-zero)", i,
                   (unsigned long long)dm->getPhysicalAddress(), same, probeWords, nz);
            if (same == probeWords && nz) found = dm->getPhysicalAddress();
            m->release();
        }
        if (!found) { PFBLOG("the console surface is in none of the GPU's BARs: leaving the firmware framebuffer"); return false; }
        base = found;
    }
    // early in boot the platform expert hands out the physical address with a flag in its low bits (0x820000001): taken
    // as is, every row WindowServer drew landed one byte off and the panel showed swapped colours (2026-10-20)
    base &= ~(uint64_t)PAGE_MASK;
    fBase = (IOPhysicalAddress)base;
    PFBLOG("console: %ux%u row %u depth %lu at physical 0x%llx", fWidth, fHeight, fRowBytes, v.v_depth, (unsigned long long)fBase);
    if (IODeviceMemory *sm = IODeviceMemory::withRange(fBase, (IOPhysicalLength)fRowBytes * fHeight)) {
        fSurfMap = sm->map(kIOMapWriteCombineCache); sm->release();
    }
    if (!fSurfMap) { PFBLOG("the console surface at 0x%llx would not map write-combined", (unsigned long long)fBase); return false; }
    // IOFramebuffer::start makes the framebuffer's controller only from a dependent ID that is already set
    // (IOFBController::copyController(kDepIDCreate)); without one the controller stays NULL and the first power-change
    // callout dereferences it (IOFramebuffer::powerStateWillChangeTo, panic 2026-10-20)
    setProperty("IOFBDependentID", 0x50485846ull << 32 | 1, 64);   // 'PHXF'
    setProperty("IOFBDependentIndex", 0ull, 32);
    loadEdid();   // before IOFramebuffer::start, so the first getDDCBlock already serves the chosen EDID
    fNModes = 0; gHead0NModes = 0; gHead0Mode = 0;
    if (IOService *b = copyBringup("phx-pipe0-mode-fn")) {   // AOC at 144: the list must be complete before IOFramebuffer::start's first query (R0 D1)
        const bool armed = b->getProperty("phx-pipe0-modes") == kOSBooleanTrue;
        b->release();
        if (armed) {
            measureRefresh();
            uint32_t ids[phxhead::kNMaxModes];
            const unsigned n = phxhead::modeList(0, fEdid, fEdidLen, fWidth, fHeight, ids);
            const uint32_t cur = fHTotal ? phxhead::modeForTotals(0, fHTotal, fVTotal, ids, n) : 0;
            if (n >= 2 && cur) { memcpy(fModeIds, ids, n * sizeof ids[0]); fNModes = n; fModeId = fBootModeId = cur; gHead0NModes = (int)n; gHead0Mode = (int)cur; }
            PFBLOG("head 0 modes: %u listed for %ux%u, OTG0 %ux%u totals -> id %u%s", n, fWidth, fHeight, fHTotal, fVTotal, cur, fNModes ? "" : " (single mode kept)");
        }
    }
    {   // the offer gate (phxoffer.h): decided here, before IOFramebuffer::start's first query
        char a[8];
        gHead0OfferOpen = phxoffer::closedAtStart(fNModes, PE_parse_boot_argn("-phxfb0open", a, sizeof a)) ? 0 : 1;
        if (fNModes) PFBLOG("head 0 offer gate %s: %s", gHead0OfferOpen ? "open" : "CLOSED",
                            gHead0OfferOpen ? "every listed mode offered (-phxfb0open)" : "only the lit mode until debug.phxfb0_offer=1 (the daemon, before its restart)");
    }
    if (!IOFramebuffer::start(provider)) { OSSafeReleaseNULL(fSurfMap); return false; }
    fVa = IORangeAllocator::withRange(kVaSize - 1, PAGE_SIZE, 0, IORangeAllocator::kLocking);
    poolInit();
    setProperty("built-in", kOSBooleanTrue);
    setProperty("PhxFBWidth", fWidth, 32); setProperty("PhxFBHeight", fHeight, 32);   // the AGDC's panel timing reads these
    measureRefresh();
    head0ModeRate();   // with a mode list: the lit mode's exact period
    if (!fNModes && fHTotal)   // rot12: no mode list -> the GOP mode may still rotate (the Acer alone on pipe 0)
        fSingleOk = phxrot::singleMode(fEdid, fEdidLen, fWidth, fHeight, fHTotal, fVTotal, &fSingle, &fSingleT);
    PFBLOG("head 0 single mode: %s (%ux%u totals %ux%u)", fSingleOk ? "rotatable" : "none", fWidth, fHeight, fHTotal, fVTotal);
    gHead0Refresh = (int)fRefreshMilliHz;
    setProperty("PhxFBRefreshMilliHz", fRefreshMilliHz, 32);
    if (fHTotal) { setProperty("PhxFBHTotal", fHTotal, 32); setProperty("PhxFBVTotal", fVTotal, 32); }   // the AGDC's panel timing
    nanoseconds_to_absolutetime(fFrameNs, &gPhxFrameAbs);
    fMmio = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    if (fMmio && fMmio->getLength() < 0x5100u * 4) OSSafeReleaseNULL(fMmio);
    fFlipLock = IOLockAlloc();
    gPhxFB = this;
    fVblTimer = thread_call_allocate(vblFire, this);
    if (fVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d); fVblNext = d; thread_call_enter_delayed(fVblTimer, d); }
    static IOPMPowerState states[2] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    registerPowerDriver(this, states, 2);
    // NO phxHoldInit: the GPU "busy" hold (debug.phxfb_hold) blocks the Metal device from being created (RE'd 2026-10-24
    // -- metaltest returned 0 devices while the hold was active, 1 after release). On a dGPU we keep Metal live instead
    // (MetalPluginName not withheld), and the phxboot daemon does one clean WindowServer restart once armed. -phxfbhold
    // re-enables the old hold for debugging.
    { char a[8]; if (PE_parse_boot_argn("-phxfbhold", a, sizeof a)) phxHoldInit(this, kBootHoldSec); }
    headsSet(0, this);   // M7 S3a: PhxAGDC's list (agdcWork finds head 0 here) and the second head; before the AGDC sysctl
    phxAgdcInstall();
    phxSurfMapInstall();
    phxRotInstall();
    phxRotPublish(this, 0);   // rot10: head 0's IOFBProbeOptions from start (the gate is on by default; startHead does head 1)
    if (!gAliasSysctl) { for (auto *o : kFbOids) sysctl_register_oid(o); gAliasSysctl = true; }
    phxHp0Install();   // hot-plug hp0: sysctls only; the poll starts with debug.phxhotplug0 >= 1 (default 0)
    PFBLOG("panel published: %ux%u row %u at 0x%llx (vbl timer %s)", fWidth, fHeight, fRowBytes,
           (unsigned long long)fBase, fVblTimer ? "running" : "MISSING");
    return true;
}

// M7 S3a: the phantom second head. Its surface is VRAM at phxhead::kHead1SurfOff in the BAR head 0's console sits in,
// written only by the CPU (the display pipe's copy, the clear below). Since S3b it READS pipe 1 (OTG1 / HUBP1, never a
// write): OTG1's refresh at start, its tick's phase, debug.phxfb1_scan; with pipe 1 dark its VBL is the 60 Hz timer. Its EDID is the other monitor's (kext b81 phxddc::kSrcOther).
bool PhxAMDFB::startHead(IOService *provider, unsigned head)
{
    fHead = head;
    gHead1State = 3;   // starting (T2 10-29: a transient -1 here read as a failure to the arm script)
    if (head != 1) { gHead1State = -1; PFBLOG("head %u: only one extra head is supported", head); fHead = 0; return false; }
    fPCI = provider ? OSDynamicCast(IOPCIDevice, provider->getProvider()) : nullptr;
    IOLock *l = headsLock();
    uint64_t aBase = 0, aBytes = 0;
    if (l) IOLockLock(l);
    if (PhxAMDFB *h0 = gHeads[0]) { aBase = h0->fBase; aBytes = (uint64_t)h0->fRowBytes * h0->fHeight; }
    const bool taken = gHeads[1] != nullptr;
    if (l) IOLockUnlock(l);
    if (!fPCI || !aBase || taken) { gHead1State = -2; PFBLOG("head 1: %s", taken ? "already up" : "head 0 is not up"); return false; }
    const uint32_t w = (uint32_t)gHead1W, h = (uint32_t)gHead1H;
    if (!phxhead::pluginSafe(w, h) && !gHead1Force) {
        gHead1State = -3;
        PFBLOG("head 1: %ux%u is at least 1920x1080: the plugin's GPU present would show its frames on head 0 (debug.phxfb1_force=1 overrides)", w, h);
        return false;
    }
    uint64_t barStart = 0, barLen = 0;
    for (uint32_t i = 0; i < fPCI->getDeviceMemoryCount(); i++) {
        IODeviceMemory *dm = fPCI->getDeviceMemoryWithIndex(i);
        if (dm && aBase >= dm->getPhysicalAddress() && aBase - dm->getPhysicalAddress() < dm->getLength()) {
            barStart = dm->getPhysicalAddress(); barLen = dm->getLength(); break;
        }
    }
    phxhead::Layout L {};
    const int pr = barLen ? phxhead::planSurface(w, h, barLen, aBase - barStart, aBytes, &L) : phxhead::kErrBar;
    if (pr != phxhead::kOk) { gHead1State = -4; PFBLOG("head 1: no surface for %ux%u (plan %d, BAR 0x%llx+0x%llx)", w, h, pr, barStart, barLen); return false; }
    fBase = (IOPhysicalAddress)(barStart + L.off);
    fWidth = L.w; fHeight = L.h; fRowBytes = L.pitch;
    if (IODeviceMemory *sm = IODeviceMemory::withRange(fBase, (IOPhysicalLength)L.region)) { fSurfMap = sm->map(kIOMapWriteCombineCache); sm->release(); }
    if (!fSurfMap) { gHead1State = -5; PFBLOG("head 1: VRAM 0x%llx would not map write-combined", L.off); return false; }
    {   // a known picture until WindowServer draws: dark grey, then the posted writes are flushed
        volatile uint64_t *p = (volatile uint64_t *)fSurfMap->getVirtualAddress();
        for (uint64_t i = 0; i < L.bytes / 8; i++) p[i] = 0xff202020ff202020ull;
        __asm__ __volatile__("sfence" ::: "memory");
        (void)p[L.bytes / 8 - 1];
    }
    setProperty("IOFBDependentID", 0x50485846ull << 32 | 1, 64);   // the same controller as head 0 ('PHXF')
    setProperty("IOFBDependentIndex", (unsigned long long)head, 32);
    fOnline = true; gHead1Online = 1;
    loadEdid();
    fNModes = 0; gHead1NModes = 0; gHead1Mode = 0;
    if (gHead1Modes) {   // the list must be complete before IOFramebuffer::start's first query (R0 D1)
        measureRefresh();
        uint32_t ids[phxhead::kNHead1Modes];
        const unsigned n = phxhead::head1ModeList(fEdid, fEdidLen, fWidth, fHeight, ids);
        const uint32_t cur = fHTotal ? (phxhead::head1ModeForTotals(fHTotal, fVTotal, ids, n) ?: phxhead::head1ModeFor(fRefreshMilliHz, ids, n)) : 0;
        if (n && cur) { memcpy(fModeIds, ids, sizeof ids); fNModes = n; fModeId = fBootModeId = cur; gHead1NModes = (int)n; gHead1Mode = (int)cur; }
        PFBLOG("head 1 modes: %u listed for %ux%u, lit %u mHz -> id %u%s", n, fWidth, fHeight, fRefreshMilliHz, cur, fNModes ? "" : " (single mode kept)");
    }
    setProperty(kIOFBGammaCountKey, (unsigned long long)phxgamma::kN, 32);   // IOFB resamples every table to 3 x 256 x 16 bit
    setProperty(kIOFBGammaWidthKey, 16ull, 32);
    fGammaLock = IOLockAlloc();
    fGammaRetry = thread_call_allocate(gammaRetryFire, this);
    if (!IOFramebuffer::start(provider)) {
        gHead1State = -6; gHead1Online = 0; OSSafeReleaseNULL(fSurfMap);
        if (fGammaRetry) { thread_call_free(fGammaRetry); fGammaRetry = nullptr; }
        if (fGammaLock) { IOLockFree(fGammaLock); fGammaLock = nullptr; }
        return false;
    }
    fRefreshMilliHz = phxhead::kRefreshMilliHz; fFrameNs = phxhead::framePeriodNs(fRefreshMilliHz); fHTotal = fVTotal = 0;
    measureRefresh();   // M7 S3b: OTG1's rate when the ladder already lit pipe 1, else the 60 Hz above stays
    gHead1PipeRefresh = fHTotal ? (int)fRefreshMilliHz : 0;
    fMmio = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);   // OTG1 / HUBP1: reads (lockToPipe), HUBPREQ1 flips (S4a, debug.phxfb1_flip)
    if (fMmio && fMmio->getLength() < 0x5100u * 4) OSSafeReleaseNULL(fMmio);
    fFR = phxhead::flipRegs(head); fPR = phxhead::pipeRegs(head);
    fFlipLock = IOLockAlloc();
    setProperty("PhxFBWidth", fWidth, 32); setProperty("PhxFBHeight", fHeight, 32);
    setProperty("PhxFBRefreshMilliHz", fRefreshMilliHz, 32);
    setProperty("phx-head", head, 32);
    phxRotPublish(this, head);   // rotation S1: only while debug.phxfb_rotate names this head
    setProperty("phx-vram-off", L.off, 64);
    gHead1VramOff = (SInt64)L.off;
    fVblTimer = thread_call_allocate(vblFire, this);
    if (fVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d); fVblNext = d; thread_call_enter_delayed(fVblTimer, d); }
    static IOPMPowerState states[2] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    registerPowerDriver(this, states, 2);
    headsSet(head, this);
    gHead1State = 1;
    PFBLOG("head 1 published: %ux%u pitch %u at VRAM 0x%llx (phys 0x%llx), %u mHz timer %s (OTG1 %s), EDID src 0x%x why %d",
           fWidth, fHeight, fRowBytes, L.off, (unsigned long long)fBase, fRefreshMilliHz, fVblTimer ? "running" : "MISSING",
           gHead1PipeRefresh ? "measured" : "not running",
           (unsigned)gHead1EdidSrc, gHead1EdidWhy);
    return true;
}

void PhxAMDFB::setOnline(bool on)
{
    if (fOnline == on) return;
    fOnline = on;
    if (fHead == 0) gHead0Online = on ? 1 : 0;   // hp0: no hardware action (the GOP firmware owns DP B's link; OTG0 keeps scanning)
    else { gHead1Online = on ? 1 : 0; if (gHead1State > 0) gHead1State = on ? 1 : 2; }
    const IOFBInterruptProc proc = fConnect.proc;
    PFBLOG("head %u: %s (connect handler %s)", fHead, on ? "connected" : "disconnected", proc ? "called" : "not registered");
    if (proc) proc(fConnect.target, fConnect.ref);
    if (on && fHead) gammaReapply();
}

// Gamma. Both setGammaTable overrides land here (IOFramebuffer's gate, WindowServer's thread or IOFB's work loop; never the
// VBL tick). Head 0: counted, Success as before (its pipe is the GOP's; MPCC_OGAM0 stays outside every allow-list).
IOReturn PhxAMDFB::gammaSet(UInt32 channels, UInt32 count, UInt32 width, void *data, bool sync)
{
    if (fHead == 0) { OSAddAtomic64(1, &gGamma0N); return kIOReturnSuccess; }
    OSAddAtomic64(1, &gGamma1N);
    if (!fGammaLock) return kIOReturnSuccess;
    IOLockLock(fGammaLock);
    if (!phxgamma::resample(data, channels, count, width, fGamma)) {
        gGamma1Bad++;
        IOLockUnlock(fGammaLock);
        return kIOReturnBadArgument;
    }
    gGamma1Count = (int)count; gGamma1Width = (int)width; gGamma1Sync = sync ? 1 : 0;
    const int e = fGamma[0][255] > fGamma[1][255] ? (fGamma[0][255] > fGamma[2][255] ? fGamma[0][255] : fGamma[2][255])
                                                  : (fGamma[1][255] > fGamma[2][255] ? fGamma[1][255] : fGamma[2][255]);
    gGamma1E255 = e;
    if (e < gGamma1E255Min) gGamma1E255Min = e;
    if (e > gGamma1E255Max) gGamma1E255Max = e;
    gGamma1Identity = phxgamma::isIdentity(fGamma) ? 1 : 0;
    fGammaValid = true; fGammaDirty = true; fGammaTries = 0;
    gammaPushLocked();
    IOLockUnlock(fGammaLock);
    return kIOReturnSuccess;   // also when the hardware is not lit: the table stays cached and is re-sent later
}

// Send fGamma to PhxBringup once (fGammaLock held). Busy keeps it dirty and arms the retry.
void PhxAMDFB::gammaPushLocked()
{
    if (!fGammaValid || !fGammaDirty || fStopping) return;
    if (!fGammaSvc && fPCI) {
        if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
            while (OSObject *o = it->getNextObject()) {
                IOService *sv = OSDynamicCast(IOService, o);
                if (sv && sv->getProperty("phx-pipe2-gamma-fn") == kOSBooleanTrue) { fGammaSvc = sv; fGammaSvc->retain(); break; }
            }
            it->release();
        }
    }
    if (!fGammaSvc) { fGammaDirty = false; gGamma1LastRes = -2; strlcpy(gGamma1Why, "no PhxBringup b98+", sizeof gGamma1Why); return; }
    fGammaReq.version = PHX_PIPE2_GAMMA_VERSION; fGammaReq.mode = phxgamma::kModeAuto;
    memcpy(fGammaReq.table, fGamma, sizeof fGammaReq.table);
    fGammaReq.result = -1; fGammaReq.why[0] = 0;
    gGamma1Pushes++;
    const IOReturn r = fGammaSvc->callPlatformFunction("phxPipe2SetGamma", false, &fGammaReq, nullptr, nullptr, nullptr);
    gGamma1LastRes = r == kIOReturnSuccess || r == kIOReturnBusy ? fGammaReq.result : -3;
    gGamma1Bank = (int)fGammaReq.bank; gGamma1Ctl = (int)fGammaReq.control;
    strlcpy(gGamma1Why, fGammaReq.why, sizeof gGamma1Why);
    if (r != kIOReturnBusy) { fGammaDirty = false; return; }
    gGamma1Busy++;
    if (fGammaTries >= kGammaMaxTries || !fGammaRetry) { gGamma1GiveUp++; fGammaDirty = false; return; }
    fGammaTries++;
    uint64_t d; clock_interval_to_deadline(kGammaRetryUs, kMicrosecondScale, &d);
    thread_call_enter_delayed(fGammaRetry, d);
}

void PhxAMDFB::gammaRetryFire(thread_call_param_t p0, thread_call_param_t)
{
    PhxAMDFB *me = (PhxAMDFB *)p0;
    if (!me || me->fStopping || !me->fGammaLock) return;
    IOLockLock(me->fGammaLock);
    if (me->fGammaDirty) { gGamma1Retries++; me->gammaPushLocked(); }
    IOLockUnlock(me->fGammaLock);
}

// After a relight (mode switch) or a reconnect: the cached table again (the OGAM bank state across a relight is the T2 question).
void PhxAMDFB::gammaReapply()
{
    if (fHead == 0 || !fGammaLock) return;
    IOLockLock(fGammaLock);
    if (fGammaValid) { fGammaDirty = true; fGammaTries = 0; gammaPushLocked(); }
    IOLockUnlock(fGammaLock);
}

static int phxfb1GammaStatSysctl SYSCTL_HANDLER_ARGS
{
    char b[400];
    int len = snprintf(b, sizeof b, "n=%lld head0_n=%lld bad=%lld count=%d width=%d sync=%d e255=0x%04x min=0x%04x max=0x%04x identity=%d "
                       "pushes=%lld busy=%lld retries=%lld giveup=%lld last=%d bank=%d ctl=0x%x why=%s",
                       gGamma1N, gGamma0N, gGamma1Bad, gGamma1Count, gGamma1Width, gGamma1Sync, gGamma1E255 < 0 ? 0 : gGamma1E255,
                       gGamma1E255Min, gGamma1E255Max, gGamma1Identity, gGamma1Pushes, gGamma1Busy, gGamma1Retries, gGamma1GiveUp,
                       gGamma1LastRes, gGamma1Bank, gGamma1Ctl, gGamma1Why);
    if (len < 0) len = 0;
    if (len > (int)sizeof b - 1) len = sizeof b - 1;
    return SYSCTL_OUT(req, b, (size_t)len + 1);
}

// Mode switch on head 1 (R0 D5-D12). IOFramebuffer holds the system and controller gates and already sent WillChange:
// no mode-change delivery, no gate, no WindowServer wait here. Pipe 1 is re-lit by PhxBringup (phxp2::modeSwitch, ~3 s);
// Success only when the target rate is measured on OTG1 afterwards.
IOReturn PhxAMDFB::switchHead1Mode(IODisplayModeID id)
{
    if ((uint32_t)id == fModeId) return kIOReturnSuccess;   // R0 D4: same mode, no hardware access
    const phxhead::HeadMode *hm = phxhead::head1Mode((uint32_t)id);
    if (!hm) return kIOReturnUnsupportedMode;
    IOService *bringup = nullptr;
    if (OSIterator *it = fPCI->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject()) {
            IOService *sv = OSDynamicCast(IOService, o);
            if (sv && sv->getProperty("phx-pipe2-mode-fn") == kOSBooleanTrue) { bringup = sv; bringup->retain(); break; }
        }
        it->release();
    }
    if (!bringup) { PFBLOG("head 1 switch to id %d: no PhxBringup b93+", (int)id); return kIOReturnNotReady; }
    const bool flips = gFlipOn[fHead] != 0;
    if (flips) flipEnable(0);   // HUBP1 back on A: the relight programs A's address
    phx_pipe2_setmode req = {};
    req.version = PHX_PIPE2_SETMODE_VERSION; req.target_hz = hm->milliHz / 1000; req.result = -1;
    const IOReturn r = bringup->callPlatformFunction("phxPipe2SetMode", false, &req, nullptr, nullptr, nullptr);
    bringup->release();
    gHead1Switches++;
    gHead1SwLast = r == kIOReturnSuccess ? req.result : -1;
    const int act = r == kIOReturnSuccess ? phxhead::swAction(req.result) : phxhead::kActRefused;
    IOReturn ret = kIOReturnSuccess;
    if (act == phxhead::kActRefused) ret = kIOReturnNotReady;
    else if (act == phxhead::kActDark) {
        ret = kIOReturnIOError;
        setOnline(false);   // R0 D12: an error alone leaves WindowServer drawing to a dead head
    } else {
        fRefreshMilliHz = hm->milliHz; fHTotal = fVTotal = 0;
        measureRefresh();   // also the VBL tick's period
        const uint32_t now = fHTotal ? (phxhead::head1ModeForTotals(fHTotal, fVTotal, fModeIds, fNModes) ?: phxhead::head1ModeFor(fRefreshMilliHz, fModeIds, fNModes)) : 0;
        if (now) fModeId = now;
        gHead1Mode = (int)fModeId;
        gHead1PipeRefresh = fHTotal ? (int)fRefreshMilliHz : 0;
        setProperty("PhxFBRefreshMilliHz", fRefreshMilliHz, 32);
        if (fHTotal) { setProperty("PhxFBHTotal", fHTotal, 32); setProperty("PhxFBVTotal", fVTotal, 32); }
        ret = act == phxhead::kActDone && now == (uint32_t)id ? kIOReturnSuccess : kIOReturnIOError;
    }
    if (ret != kIOReturnSuccess) gHead1SwitchFails++;
    if (flips && act != phxhead::kActDark) flipEnable(1);
    if (act != phxhead::kActDark && act != phxhead::kActRefused) gammaReapply();   // the relit pipe gets the cached table again
    PFBLOG("head 1 switch to id %d (%u Hz): rc 0x%x result %d stage %d: %s -> 0x%x, now id %u %u mHz",
           (int)id, req.target_hz, r, req.result, req.stage, req.why, ret, fModeId, fRefreshMilliHz);
    return ret;
}

// Head 0 with a mode list: the VBL tick (WindowServer paces head 0 on it, nvrm-accel.cpp times presents against it) at the
// lit mode's period from its pixel clock and totals, when the measured OTG0 totals are that mode's: a 200 ms frame count at
// 144 Hz is +-1 frame = +-3.5 % (28 frames = 140 Hz). Head 1 keeps its measured rate (lockToPipe follows OTG1).
void PhxAMDFB::head0ModeRate()
{
    if (fHead != 0 || !fNModes || !fHTotal) return;
    const uint32_t id = phxhead::modeForTotals(0, fHTotal, fVTotal, fModeIds, fNModes);
    const phxhead::HeadMode *m = id ? phxhead::headMode(0, id) : nullptr;
    if (!m) return;
    fRefreshMilliHz = m->milliHz;
    fFrameNs = phxhead::modeFrameNs(*m);
}

// debug.phxfb_resync=1 (boot rate): the boot daemon set pipe 0 through PhxBringup's
// own engine before its WindowServer restart, so head 0 still names the mode it started at. Re-read OTG0's totals; a listed
// mode becomes current with the bookkeeping switchHead0Mode does after a set (VBL period, AGDC properties, sysctls).
// OTG0 not running or at no listed mode: every field as it was (-3). Reads OTG0 only.
int PhxAMDFB::head0Resync()
{
    if (fHead != 0) return -1;
    if (!fNModes) return -2;
    const uint32_t oldH = fHTotal, oldV = fVTotal, oldMhz = fRefreshMilliHz;
    const uint64_t oldNs = fFrameNs;
    fHTotal = fVTotal = 0;
    measureRefresh();
    const uint32_t now = fHTotal ? phxhead::modeForTotals(0, fHTotal, fVTotal, fModeIds, fNModes) : 0;
    if (!now) {
        PFBLOG("head 0 resync: OTG0 %ux%u totals are no listed mode: id %u kept", fHTotal, fVTotal, fModeId);
        fHTotal = oldH; fVTotal = oldV; fRefreshMilliHz = oldMhz; fFrameNs = oldNs;
        return -3;
    }
    const uint32_t was = fModeId;
    fModeId = now;
    if (phxoffer::openAfterResync(was, now, offerClosed())) head0OfferOpen("resync to another mode");
    head0ModeRate();
    nanoseconds_to_absolutetime(fFrameNs, &gPhxFrameAbs);   // the VBL tick's new period is read at its next fire
    setProperty("PhxFBRefreshMilliHz", fRefreshMilliHz, 32);
    setProperty("PhxFBHTotal", fHTotal, 32); setProperty("PhxFBVTotal", fVTotal, 32);
    gHead0Mode = (int)fModeId; gHead0Refresh = (int)fRefreshMilliHz;
    PFBLOG("head 0 resync: OTG0 %ux%u totals -> id %u (was %u), %u mHz", fHTotal, fVTotal, fModeId, was, fRefreshMilliHz);
    return (int)now;
}

// Mode switch on head 0 (the AOC on DP, pipe 0; AOC at 144; the R0 rules D4-D12 as
// head 1). IOFramebuffer holds the system and controller gates and
// already sent WillChange (every pipe drained, the accelerator off): no notification, no gate, no WindowServer wait here.
// PhxBringup b101 re-times pipe 0 (phxp0::macSet: stutter forced off, the UCLK hard minimum before 144, a 60 -> 60 round trip
// first in a boot, then the set; ~1.5 s per shell); it refuses the 144 plan unless the Acer runs 144 and while its kill
// switch debug.phxpipe0_enable is 0. Success only when OTG0 runs the target's totals afterwards. Refused (nothing changed)
// = NotReady, the old id stays current (CoreDisplay keeps the old mode, R0); fell back = IOError with the old id
// re-measured; dark = IOError and head 0 stays online (phxhead::p0Action; it is the console).
IOReturn PhxAMDFB::switchHead0Mode(IODisplayModeID id)
{
    if ((uint32_t)id == fModeId) return kIOReturnSuccess;   // R0 D6: same mode, no hardware access
    const phxhead::HeadMode *hm = phxhead::headMode(0, (uint32_t)id);
    if (!hm) return kIOReturnUnsupportedMode;
    IOService *bringup = copyBringup("phx-pipe0-mode-fn");
    if (!bringup) { gHead0SwLast = -1; gHead0SwitchFails++; PFBLOG("head 0 switch to id %d: no PhxBringup b101+", (int)id); return kIOReturnNotReady; }
    const bool flips = gFlipOn[0] != 0;
    if (flips) flipEnable(0);   // HUBP0 back on A while pipe 0 is re-timed (the engine blanks HUBP0, never moves its address)
    phx_pipe0_setmode req = {};
    req.version = PHX_PIPE0_SETMODE_VERSION; req.target_hz = hm->milliHz / 1000; req.result = -1;
    const IOReturn r = bringup->callPlatformFunction("phxPipe0SetMode", false, &req, nullptr, nullptr, nullptr);
    bringup->release();
    gHead0Switches++;
    gHead0SwLast = r == kIOReturnSuccess ? req.result : -1;
    gHead0SwFlags = (int)req.flags;
    const int act = r == kIOReturnSuccess ? phxhead::p0Action(req.result) : phxhead::kActRefused;
    IOReturn ret = kIOReturnNotReady;
    uint32_t now = 0;
    if (act != phxhead::kActRefused) {
        fHTotal = fVTotal = 0;
        measureRefresh();   // OTG0's totals now (OTG0 not running after a dark: the old rate stays)
        now = fHTotal ? phxhead::modeForTotals(0, fHTotal, fVTotal, fModeIds, fNModes) : 0;
        if (now) { fModeId = now; head0ModeRate(); }
        nanoseconds_to_absolutetime(fFrameNs, &gPhxFrameAbs);   // the VBL tick's new period is read at its next fire
        setProperty("PhxFBRefreshMilliHz", fRefreshMilliHz, 32);
        if (fHTotal) { setProperty("PhxFBHTotal", fHTotal, 32); setProperty("PhxFBVTotal", fVTotal, 32); }
        ret = act == phxhead::kActDone && now == (uint32_t)id ? kIOReturnSuccess : kIOReturnIOError;
    }
    gHead0Mode = (int)fModeId; gHead0Refresh = (int)fRefreshMilliHz;
    if (ret != kIOReturnSuccess) gHead0SwitchFails++;
    if (flips && act != phxhead::kActDark) flipEnable(1);
    PFBLOG("head 0 switch to id %d (%u Hz): rc 0x%x result %d stage %d flags 0x%x: %s -> 0x%x, now id %u %u mHz",
           (int)id, req.target_hz, r, req.result, req.stage, req.flags, req.why, ret, fModeId, fRefreshMilliHz);
    return ret;
}

// head 1's surface in debug.phxshot's format (tools/phxctl/phxshot.py with PHXSHOT_SYSCTL=debug.phxfb1_shot):
// the proof that frames reach it without screencapture (which needs Screen Recording in the GUI session)
int PhxAMDFB::shot(struct sysctl_req *req)
{
    if (!fSurfMap || !fRowBytes || !fHeight) return ENXIO;
    const uint64_t rowBytes = fRowBytes;
    const uint32_t hdr[4] = { 0x54485350u, fRowBytes / 4, fHeight, (uint32_t)gHead1VramOff };
    if (!req->oldptr) return SYSCTL_OUT(req, nullptr, sizeof hdr + rowBytes * fHeight);
    int err = SYSCTL_OUT(req, hdr, sizeof hdr);
    uint64_t *row = (uint64_t *)IOMalloc(rowBytes);
    if (!row) return ENOMEM;
    const volatile uint64_t *src = (const volatile uint64_t *)fSurfMap->getVirtualAddress();
    for (uint32_t y = 0; !err && y < fHeight; y++) {
        for (uint64_t x = 0; x < rowBytes / 8; x++) row[x] = src[y * (rowBytes / 8) + x];
        err = SYSCTL_OUT(req, row, rowBytes);
    }
    IOFree(row, rowBytes);
    return err;
}

// 64 samples of head 1's surface (debug.phxfb1_sum): proves frames reach it without reading any display register
int PhxAMDFB::sample(uint32_t out[7])
{
    if (!fSurfMap || !fWidth || !fHeight) return ENXIO;
    const volatile uint32_t *p = (const volatile uint32_t *)fSurfMap->getVirtualAddress();
    const uint32_t first = p[0] & 0xffffffu;
    uint32_t nz = 0, varied = 0, x = 0;
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
        const uint32_t v = p[(uint64_t)((fHeight * (2 * i + 1)) / 16) * (fRowBytes / 4) + (fWidth * (2 * j + 1)) / 16] & 0xffffffu;
        nz += v != 0; varied += v != first; x ^= v;
    }
    out[1] = fWidth; out[2] = fHeight; out[3] = fRowBytes; out[4] = nz; out[5] = varied; out[6] = x;
    return 0;
}

void PhxAMDFB::stop(IOService *provider)
{
    fStopping = true;
    if (fHead != 0) {   // M7 S3a: head 1 owns only its timer and its surface mapping
        headsSet(fHead, nullptr);
        if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
        gHead1State = 0; gHead1Online = 0; gHead1VramOff = -1; gHead1Scan = -1; gHead1PipeRefresh = 0;
        if (fGammaRetry) { thread_call_cancel(fGammaRetry); while (!thread_call_free(fGammaRetry)) { thread_call_cancel(fGammaRetry); IOSleep(5); } fGammaRetry = nullptr; }
        if (fGammaLock) { IOLockLock(fGammaLock); OSSafeReleaseNULL(fGammaSvc); IOLockUnlock(fGammaLock); IOLockFree(fGammaLock); fGammaLock = nullptr; }
        if (fFlipLock) {   // HUBP1 back on A (the surface the ladder programmed) before B goes
            IOLockLock(fFlipLock); gFlipOn[fHead] = 0; flipHomeLocked(); IOLockUnlock(fFlipLock);
            IOLockFree(fFlipLock); fFlipLock = nullptr;
        }
        OSSafeReleaseNULL(fBackMap);
        OSSafeReleaseNULL(fMmio);
        OSSafeReleaseNULL(fSurfMap);
        IOFramebuffer::stop(provider);
        return;
    }
    phxHp0Remove();   // hot-plug hp0: no poll reads fMmio after this
    headsSet(0, nullptr);
    phxAgdcRemove();
    phxSurfMapRemove();
    phxRotRemove();
    if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
    if (gAliasSysctl) { for (auto *o : kFbOids) sysctl_unregister_oid(o); gAliasSysctl = false; }
    gPhxFB = nullptr;
    if (fFlipLock) {   // the console and a panic screen draw into A: scan A out again before going
        IOLockLock(fFlipLock); gFlipOn[0] = 0; flipHomeLocked(); IOLockUnlock(fFlipLock);
        IOLockFree(fFlipLock); fFlipLock = nullptr;
    }
    OSSafeReleaseNULL(fBackMap);
    OSSafeReleaseNULL(fMmio);
    gPhxFrameAbs = 0;
    OSSafeReleaseNULL(fAliasMap);
    OSSafeReleaseNULL(fSurfMap);
    OSSafeReleaseNULL(fVa);
    poolFree();
    IOFramebuffer::stop(provider);
}
