// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Linear pitch alignment helpers (plugin f18a), pure so a host test can run them
// on the host. RADV (ac_surface.c ac_surface_get_pitch_align) takes an explicit DRM-linear row pitch only on a multiple of
// 256 B on GFX9-GFX11 (128 B on GFX12, 64-128 B before GFX9); NVK's texture header wants 32 B. The alignment is not
// assumed: nvmtl_pitch_align_pick asks the device, through ok(), which pitches an explicit linear layout takes.
#ifndef NVMTL_PITCH_ALIGN_H
#define NVMTL_PITCH_ALIGN_H
#include <stddef.h>
#include <stdint.h>

#define NVMTL_PITCH_OLD 32u          /* the rule before f18a, kept whenever the device's answer is not understood */
#define NVMTL_PITCH_PROBE_MAX 4096u

/* texel sizes the alignment is asked for (one probe each); any other size keeps NVMTL_PITCH_OLD */
static inline int nvmtl_pitch_bpp_slot(uint32_t bpp)
{
    switch (bpp) { case 1: return 0; case 2: return 1; case 4: return 2; case 8: return 3; case 16: return 4; default: return -1; }
}

typedef int (*nvmtl_pitch_accept_fn)(void *ctx, uint32_t bpp, uint32_t pitch);

/* The smallest power-of-two pitch (32 .. 4096 B) the device takes for this texel size, confirmed at 3x that pitch
 * (the rule must be "a multiple of", not "a power of two": 7680-byte 1920 px rows are multiples of 256, not powers of
 * two). 0 = no answer that fits the model; the caller then keeps NVMTL_PITCH_OLD. */
static inline uint32_t nvmtl_pitch_align_pick(nvmtl_pitch_accept_fn ok, void *ctx, uint32_t bpp)
{
    if (nvmtl_pitch_bpp_slot(bpp) < 0) return 0;
    for (uint32_t p = NVMTL_PITCH_OLD; p <= NVMTL_PITCH_PROBE_MAX; p <<= 1)
        if (ok(ctx, bpp, p)) return ok(ctx, bpp, 3 * p) ? p : 0;
    return 0;
}

static inline int nvmtl_pitch_fits(size_t row, uint32_t align) { return align && row % align == 0; }

#endif
