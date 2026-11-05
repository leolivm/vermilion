// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Audit finding F7 (plugin corecount, 10-31): -[NVMTLDevice gpuCoreCount] answers the SM count of
// VK_NV_shader_sm_builtins, which RADV does not have, so it answered 1 (Mail, Safari, App Store and
// WallpaperLegacyExtension ask, 10-30 B1 log). On a device without an SM count the compute units of
// VK_AMD_shader_core_properties2 (activeComputeUnitCount, RADV: info.num_cu) are the count. Included by nvmtl_vk.c
// before nvmtl_vk_initialize, which calls it once. 0 = no answer (gpuCoreCount then keeps answering 1).
static uint32_t nvmtl_vk_core_count_amd(VkPhysicalDevice pd)
{
    if (!pd || !pvkEnumerateDeviceExtensionProperties || !pvkGetPhysicalDeviceProperties2) return 0;
    uint32_t n = 0; int have = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkExtensionProperties *e = calloc(n, sizeof *e);
    if (!e) return 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (uint32_t i = 0; i < n && !have; i++) have = !strcmp(e[i].extensionName, VK_AMD_SHADER_CORE_PROPERTIES_2_EXTENSION_NAME);
    free(e);
    if (!have) { nvlog("gpuCoreCount: no NVIDIA SM count and no %s - gpuCoreCount answers 1", VK_AMD_SHADER_CORE_PROPERTIES_2_EXTENSION_NAME); return 0; }
    VkPhysicalDeviceShaderCoreProperties2AMD c2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_2_AMD };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &c2 };
    pvkGetPhysicalDeviceProperties2(pd, &p2);
    nvlog("gpuCoreCount: no NVIDIA SM count; %s activeComputeUnitCount %u", VK_AMD_SHADER_CORE_PROPERTIES_2_EXTENSION_NAME,
          c2.activeComputeUnitCount);
    return c2.activeComputeUnitCount;
}
