/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

#define NVRM_VRAM_ABI_VERSION 2u
#define NVRM_VRAM_FN_ALLOC    "nvAllocVram"
#define NVRM_VRAM_FN_FREE     "nvFreeVram"

#define NVRM_VRAM_FN_SCANOUT  "nvScanoutInfo"
/* PhxAMDFB double buffer (debug.phxfb_flip): the buffer the panel is not scanning (same request as the scan-out, or
 * kIOReturnUnsupported = copy into the scan-out), then the flip to it at the next vblank (no arguments). */
#define PHX_FB_FN_FLIP_BACK   "phxFlipBack"
#define PHX_FB_FN_FLIP_COMMIT "phxFlipCommit"
#define PHX_FB_FN_SCAN_FOREIGN "phxScanForeign"   /* kIOReturnSuccess: HUBP0 scans neither A nor B (GPU present owns it) */

#define NVRM_VRAM_BAR1_BUDGET (192ull * 1024ull * 1024ull)

struct NVRMVramRequest {
    unsigned int       version;
    unsigned int       flags;
    unsigned long long size;
    unsigned long long actualSize;
    unsigned long long phys;
    void              *kva;
    void              *handle;
    unsigned long long mappedTotal;
    unsigned int width, height, pitch;
    unsigned long long allocationCookie;
};
