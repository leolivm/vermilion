// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
//  PhxBringup.cpp — RX 7900 XT (Navi 31, 1002:744c) bring-up kext; it began on the Phoenix Radeon 780M.
//
//  Publishes the service the RADV Darwin backend opens (class PhxBringup, user client type 'N48N')
//  and answers ABI v1 selectors 0..8 with the same rules as Mesa's in-process mock kext
//  (src/amd/common/darwin/ac_darwin_mock.c): buffers are kernel/user shared system memory,
//  GPU VA mappings are bookkeeping only, and Submit retires at once without touching the GPU.
//  E1 proves the IOKit transport end to end; E3/E4 put the GPU behind the same calls.
//
//  Boot-arg -phxbringupoff disables it. ReadRegs (MMIO read) is root-only; nothing else touches hardware.
//

#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <kern/thread_call.h>
#include <kern/sched_prim.h>

#include "PhxNativeABI.h"
#include "PhxVmTables.h"
#include "phx_regs_dcn32.h"
#include "PhxDdc.h"
#include "PhxEdid.h"
#include "PhxHpd.h"   // b102 hot-plug K1 + A2 (phx_hpd.cpp)

extern struct sysctl_oid sysctl__debug_phxkey;   // debug.phxkey, the virtual keyboard (phx_vkbd.cpp)
// b82 (M7 S2): the pipe-1 ladder and the DMCUB ring (phx_pipe2.cpp)
extern struct sysctl_oid sysctl__debug_phxpipe2, sysctl__debug_phxpipe2_stat, sysctl__debug_phxpipe2_plan,
    sysctl__debug_phxdmub_ring, sysctl__debug_phxdmub_dead, sysctl__debug_phxpipe2_head1, sysctl__debug_phxdmub_trace, sysctl__debug_phxpipe2_mode,   // b87 head1, b88 trace, b90 mode
    sysctl__debug_phxpipe2_switch, sysctl__debug_phxpipe2_switch_stat, sysctl__debug_phxpipe2_phase, sysctl__debug_phxpipe2_det,   // b91
    sysctl__debug_phxscdc,   // b107 SCDC1 (read only)
    sysctl__debug_phxdmub_restart, sysctl__debug_phxdmub_restart_stat, sysctl__debug_phxdmub_trace_pre,   // b109 DMCUB restart
    sysctl__debug_phxscdc_fix, sysctl__debug_phxscdc_fix_stat,   // b109 SCDC fix
    sysctl__debug_phxdmub_regtest, sysctl__debug_phxdmub_regtest_stat;   // b111 one-write DMUB REG_SEQ test
extern struct sysctl_oid sysctl__debug_phxgamma, sysctl__debug_phxgamma_test, sysctl__debug_phxgamma_stat;   // b98 head-1 gamma (phx_gamma.cpp)
extern struct sysctl_oid sysctl__debug_phxpipe0, sysctl__debug_phxpipe0_enable, sysctl__debug_phxpipe0_mode, sysctl__debug_phxpipe0_stat,
    sysctl__debug_phxpipe0_plan, sysctl__debug_phxpipe0_dead,   // b99 pipe-0 mode engine (phx_pipe0.cpp)
    sysctl__debug_phxpipe0_modes, sysctl__debug_phxpipe0_uclk,  // b101 the macOS path
    sysctl__debug_phxpipe0_solo,                                // p0solo the AOC alone
    sysctl__debug_phxpipe0_live;                                // b123 live pair re-plan
extern struct sysctl_oid sysctl__debug_phxhpd, sysctl__debug_phxhpd_sense, sysctl__debug_phxhotplug, sysctl__debug_phxhotplug_stat;   // b102 (phx_hpd.cpp)
static void phxHotplugStop();   // b102, phx_hpd.cpp: called by stop()
extern struct sysctl_oid sysctl__debug_phxdpcd, sysctl__debug_phxdpcd_stat;   // b112 multiport S1a (phx_dpcd.cpp)
extern struct sysctl_oid sysctl__debug_phxc3, sysctl__debug_phxc3_stat, sysctl__debug_phxpoweron;   // b113 C3 REG_SEQ programs (phx_pipe2.cpp)
extern struct sysctl_oid sysctl__debug_phxdplink, sysctl__debug_phxdplink_stat;   // b116 multiport S2b (phx_dplink.cpp)
extern struct sysctl_oid sysctl__debug_phxdpstream, sysctl__debug_phxdpstream_stat;   // b117 multiport S2c (phx_dpstream.cpp)
extern struct sysctl_oid sysctl__debug_phxdpwatch, sysctl__debug_phxdpwatch_stat, sysctl__debug_phxdpwatch_snap;   // b120 / b122 multiport S3 step 0 (phx_dpwatch.cpp)
static void phxDpWatchStop();   // b120, phx_dpwatch.cpp: called by stop()
static void phxC3LatchPowerOn(uint32_t gpioA);   // b113, phx_pipe2.cpp: called by start() (debug.phxpoweron, gate G1)

#define PHX_KEXT_BUILD 124u  // bump for every kext build; what each build changed: CHANGELOG.md (newest first)

// The kext is ONE translation unit: the files below hold its code by function and are included here, in this
// order (the order is the original single-file order; moving code between them must keep it). build.sh compiles
// only this file. A pure move is proven with scripts/build/all.sh --baseline / --check --local.
#include "phx_globals.cpp"
#include "phx_classes.cpp"
#include "phx_sysctl.cpp"
#include "phx_core.cpp"
#include "phx_vm.cpp"
#include "phx_ih_irq.cpp"
#include "phx_vcn_mmhub.cpp"
#include "phx_scan.cpp"
#include "phx_queue.cpp"
#include "phx_psp_smu.cpp"
#include "phx_client.cpp"
#include "phx_debug.cpp"
#include "phx_wait.cpp"
#include "phx_vkbd.cpp"
#include "phx_scan_client.cpp"
#include "phx_pipe2.cpp"
#include "phx_gamma.cpp"
#include "phx_pipe0.cpp"
#include "phx_hpd.cpp"
#include "phx_dpcd.cpp"   // b112
#include "phx_dplink.cpp"   // b116
#include "phx_dpstream.cpp"   // b117
#include "phx_dpwatch.cpp"   // b120
