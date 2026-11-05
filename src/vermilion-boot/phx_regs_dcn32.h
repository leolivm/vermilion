// SPDX-License-Identifier: MIT
// DCN 3.2 (Navi31) display registers the kext touches, as absolute dword offsets: DCN BASE_IDX 2 base 0x34C0 + the
// register offset from Linux dcn_3_2_0_offset.h. Instance 0 only (HUBP0 / OTG0 / ODM0).
#pragma once
#include <stdint.h>
static constexpr uint32_t kDcnVmFbLocationBase          = 0x3935;   // DCN_VM_FB_LOCATION_BASE
static constexpr uint32_t kHubp0SurfaceConfig           = 0x3aa5;   // HUBP0_DCSURF_SURFACE_CONFIG
static constexpr uint32_t kHubp0TilingConfig            = 0x3aa7;   // HUBP0_DCSURF_TILING_CONFIG
static constexpr uint32_t kHubp0DchubpCntl              = 0x3ab3;   // HUBP0_DCHUBP_CNTL (underflow status 30:28, clear 31)
static constexpr uint32_t kHubpreq0SurfacePitch         = 0x3ac7;   // HUBPREQ0_DCSURF_SURFACE_PITCH
static constexpr uint32_t kHubpreq0PrimaryAddr          = 0x3aca;   // HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS
static constexpr uint32_t kHubpreq0PrimaryAddrHigh      = 0x3acb;   // HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH
static constexpr uint32_t kHubpreq0SurfaceControl       = 0x3ada;   // HUBPREQ0_DCSURF_SURFACE_CONTROL (bit 0 DCC)
static constexpr uint32_t kHubpreq0FlipControl          = 0x3adb;   // HUBPREQ0_DCSURF_FLIP_CONTROL (bit 1 immediate, bit 8 update pending)
static constexpr uint32_t kHubpreq0SurfaceInuse         = 0x3ae0;   // HUBPREQ0_DCSURF_SURFACE_INUSE
static constexpr uint32_t kHubpreq0SurfaceInuseHigh     = 0x3ae1;   // HUBPREQ0_DCSURF_SURFACE_INUSE_HIGH
static constexpr uint32_t kHubpreq0EarliestInuse        = 0x3ae4;   // HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE
static constexpr uint32_t kHubpreq0EarliestInuseHigh    = 0x3ae5;   // HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH
static constexpr uint32_t kOdm0OptcInputGlobalControl   = 0x4f8a;   // ODM0_OPTC_INPUT_GLOBAL_CONTROL (underflow occurred 10, clear 12)
static constexpr uint32_t kOtg0HTotal                   = 0x4fea;   // OTG0_OTG_H_TOTAL
static constexpr uint32_t kOtg0VTotal                   = 0x4fef;   // OTG0_OTG_V_TOTAL
static constexpr uint32_t kOtg0VBlankStartEnd           = 0x4ff6;   // OTG0_OTG_V_BLANK_START_END
static constexpr uint32_t kOtg0Control                  = 0x5001;   // OTG0_OTG_CONTROL (bit 0 master enable)
static constexpr uint32_t kOtg0StatusFrameCount         = 0x500c;   // OTG0_OTG_STATUS_FRAME_COUNT
