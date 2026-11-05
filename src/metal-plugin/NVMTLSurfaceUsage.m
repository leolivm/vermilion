// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLDevice.m after NVMTLPhxdec.m, before
   -newTextureWithDescriptor:iosurface:plane: (one translation unit with the plugin).
   Plugin covc2.

   An IOSurface texture asked with usage 0 (MTLTextureUsageUnknown) means "any use" to Metal: Apple reports it as
   ShaderRead|ShaderWrite|RenderTarget|PixelFormatView. QuartzCore wraps every framebufferOnly-NO CAMetalLayer drawable
   that way, and Core Image refuses a destination whose usage lacks ShaderWrite before it issues any GPU work
   (CIRenderDestination: "texture usage must include MTLTextureUsageShaderWrite"). The surface paths asked Vulkan for a
   storage image only on an explicit ShaderWrite, and the -usage getter turned 0 into ShaderRead, so such a drawable stayed
   black under Core Image (covprobe C8).

   In app processes, a usage-0 IOSurface texture now asks for a storage image when its format can be one. When it gets
   one, it reports what that image is: ShaderRead|ShaderWrite, plus RenderTarget if it has the colour attachment usage.
   PixelFormatView is not reported: the surface images are linear buffer aliases whose format list holds only the format
   and its sRGB/linear pair, other view formats are refused. Without a storage image the usage stays 0, as before: shader
   writes to such a texture go through its storage twin (nvmtl_storage_view_for), which a usage without ShaderWrite
   would turn off. An explicit usage is reported and honoured as asked (as Apple does). WindowServer keeps the old
   behaviour (it wraps hundreds of surfaces a second). */
static int nvmtl_surface_unknown_any(NSUInteger usage) {
    static int on = -1;
    if (on < 0) { const char *me = getprogname(); on = !(me && !strcmp(me, "WindowServer")); }
    return on && usage == MTLTextureUsageUnknown;
}
// whether to ask the surface image for storage: an explicit ShaderWrite, or an Unknown usage on a format that can be stored
static int nvmtl_surface_want_storage(NSUInteger usage, int a8, int fmt_storage) {
    return (usage & MTLTextureUsageShaderWrite) != 0 || (nvmtl_surface_unknown_any(usage) && !a8 && fmt_storage);
}
// the usage the texture records: as asked, or for an Unknown usage with a storage image what that image supports
static NSUInteger nvmtl_surface_report_usage(NSUInteger usage, int storage, int render) {
    if (!nvmtl_surface_unknown_any(usage) || !storage) return usage;
    return MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | (render ? MTLTextureUsageRenderTarget : 0);
}
// census: Unknown-usage surface wrappers in this process (the first 8 verbatim, then a total every 2000)
static void nvmtl_surface_unknown_note(IOSurfaceRef surf, size_t w, size_t h, NSUInteger fmt, int alias, int storage, NSUInteger reported) {
    if (!nvmtl_surface_unknown_any(MTLTextureUsageUnknown)) return;
    static _Atomic unsigned long long n, ns;
    const unsigned long long k = atomic_fetch_add(&n, 1) + 1, s = storage ? atomic_fetch_add(&ns, 1) + 1 : atomic_load(&ns);
    if (k <= 8)
        nvlog("surface usage: Unknown-usage IOSurface wrapper %llu (surface %u %zux%zu fmt %lu, %s): storage %d -> usage 0x%lx%s",
              k, surf ? (unsigned)IOSurfaceGetID(surf) : 0u, w, h, (unsigned long)fmt, alias ? "shares the surface's image" : "new image",
              storage, (unsigned long)reported, reported ? "" : " (unchanged: reads as ShaderRead)");
    else if (k % 2000 == 0)
        nvlog("surface usage: %llu Unknown-usage IOSurface wrappers, %llu with a storage image", k, s);
}
