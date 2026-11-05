// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Plugin f18a (audit 10-30, fix 10-31): the device's linear pitch alignment, asked once per texel size, and the
// check before a surface's host import. Included by nvmtl_vk.c right before nvmtl_vk_image_create_buffer_alias.
// Before f18a only `row % 32` was checked, so on RADV a single-plane IOSurface whose rows are a multiple of 32 B but not
// of 256 B (1200x800 BGRA = 4800 B, 303 px = 1216 B) was imported as host memory, refused by vkCreateImage with
// VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, freed, and made a private image anyway (12288+ refusals in one
// WindowServer, 10-30 B1 log). The pre-check gives the same private image without the import, the free and the refusal.
// MEMCENSUS: `pitch pre-rejects N` (nvmtl_imgmem.c MC_PITCH_PRE).
#include "nvmtl_pitch_align.h"

static uint32_t g_pitch_align[5];          /* per nvmtl_pitch_bpp_slot; 0 = not asked yet */
static uint64_t g_pitch_prerejects;

/* one explicit DRM-linear layout (modifier 0 = DRM_FORMAT_MOD_LINEAR) at this pitch: does the device take it? */
static int nvmtl_pitch_probe_one(void *ctx, uint32_t bpp, uint32_t pitch)
{
    (void)ctx;
    static const VkFormat fmt[5] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                                     VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R32G32B32A32_UINT };
    const int s = nvmtl_pitch_bpp_slot(bpp);
    if (s < 0 || !g_dev || !pvkCreateImage || !pvkDestroyImage) return 0;
    VkSubresourceLayout pl = { 0, 0, (VkDeviceSize)pitch, 0, 0 };
    VkImageDrmFormatModifierExplicitCreateInfoEXT eci = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL,
        0, 1, &pl };
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &eci, 0, VK_IMAGE_TYPE_2D, fmt[s], { NVMTL_PITCH_OLD / bpp, 1, 1 },
        1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_PREINITIALIZED };
    VkImage img = VK_NULL_HANDLE;
    if (pvkCreateImage(g_dev, &ici, NULL, &img) != VK_SUCCESS) return 0;
    pvkDestroyImage(g_dev, img, NULL);
    return 1;
}

/* bytes a DRM-linear row pitch must be a multiple of, for this texel size */
static uint32_t nvmtl_vk_linear_pitch_align(uint32_t bpp)
{
    if (!g_linmod) return NVMTL_PITCH_OLD;            /* LINEAR tiling: the pitch is checked against what the driver picks */
    const int s = nvmtl_pitch_bpp_slot(bpp ? bpp : 4);
    if (s < 0) return NVMTL_PITCH_OLD;
    uint32_t a = __atomic_load_n(&g_pitch_align[s], __ATOMIC_RELAXED);
    if (a) return a;
    const uint32_t p = nvmtl_pitch_align_pick(nvmtl_pitch_probe_one, NULL, bpp ? bpp : 4);
    a = p ? p : NVMTL_PITCH_OLD;
    __atomic_store_n(&g_pitch_align[s], a, __ATOMIC_RELAXED);
    if (p) syslog(LOG_NOTICE, "NVMTL: linear pitch alignment: %u B for %u B/texel (probed: smallest power-of-two pitch an explicit "
                  "linear layout takes, %u B taken too)", a, bpp ? bpp : 4, 3 * a);
    else syslog(LOG_NOTICE, "NVMTL: linear pitch alignment: no clear answer for %u B/texel (no pitch %u..%u B taken, or not its 3x) - "
                "keeping the %u B rule", bpp ? bpp : 4, NVMTL_PITCH_OLD, NVMTL_PITCH_PROBE_MAX, NVMTL_PITCH_OLD);
    return a;
}

int nvmtl_vk_linear_pitch_ok(size_t row, uint32_t bpp) { return nvmtl_pitch_fits(row, nvmtl_vk_linear_pitch_align(bpp)); }

/* the single-plane surface path (NVMTLDevice.m nvmtl_surface_image_create), last test before the host import: 0 = this row
 * pitch cannot be a linear alias, so the surface goes straight to its private image (counted, MEMCENSUS) */
int nvmtl_vk_surface_pitch_ok(size_t row, uint32_t bpp, uint32_t w, uint32_t h)
{
    if (nvmtl_vk_init() || nvmtl_vk_linear_pitch_ok(row, bpp)) return 1;
    const uint64_t n = __atomic_add_fetch(&g_pitch_prerejects, 1, __ATOMIC_RELAXED);
    nvmtl_mc_add(MC_PITCH_PRE, 1);
    if (n <= 8 || !(n & (n - 1)))
        syslog(LOG_NOTICE, "NVMTL: surface %ux%u row %zu B is not a multiple of the %u B linear pitch alignment - a private image, "
               "without the host import (pre-reject #%llu)", w, h, row, nvmtl_vk_linear_pitch_align(bpp), (unsigned long long)n);
    return 0;
}
