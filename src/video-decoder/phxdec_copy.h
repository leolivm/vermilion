// SPDX-License-Identifier: MIT
// phxdec_copy: GPU copy of a decoded picture (libavutil Vulkan frame) into a CVPixelBuffer's IOSurface, with each
// IOSurface imported as host memory once and reused (WP6 B6b). VideoToolbox's pixel buffer pool recycles a handful of
// surfaces, so after the first frames no import, GPU map, unmap or free happens per frame.
#pragma once
#include <CoreVideo/CoreVideo.h>
#include <libavutil/frame.h>

typedef struct PhxCopier PhxCopier;

// hwdev: the AVBufferRef of the AV_HWDEVICE_TYPE_VULKAN device the frames come from. NULL if the device cannot do it
// (no VK_EXT_external_memory_host, no graphics queue).
PhxCopier *phx_copier_create(AVBufferRef *hwdev);
// Copies hw (a Vulkan frame, P010 or NV12) into the 2-plane pixel buffer pb. 0, or <0 (the caller falls back): -2 no
// surface / lock, -3 base or size not import-aligned, -4 host pointer refused, -5 buffer, -6 memory, -7 plane layout,
// -8 image count, -9 submit / wait.
int phx_copier_copy(PhxCopier *c, const AVFrame *hw, CVPixelBufferRef pb);
// Imports done so far (one per distinct surface), for the session log.
unsigned phx_copier_imports(const PhxCopier *c);
size_t phx_copier_align(const PhxCopier *c);
// Distinct IOSurfaces seen (256 tracked, more counted as new), for the session log.
unsigned phx_copier_surfaces(const PhxCopier *c);
// Surfaces held in the import cache now (at most 32), and in *in_use how many of them IOSurfaceIsInUse reports as used by
// some process (a client, WindowServer, or a pool buffer handed out), for the pool log.
unsigned phx_copier_cached(const PhxCopier *c, unsigned *in_use);
// holders1 census: the IOSurface ids in the import cache now (at most max), returns how many.
unsigned phx_copier_cached_ids(const PhxCopier *c, uint32_t *ids, unsigned max);
void phx_copier_destroy(PhxCopier *c);
