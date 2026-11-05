// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// WindowServer window captures under async GPU present (Safari full-screen glitch).
// SkyLight composites a capture (CGWindowListCreateImage, CGSHWCaptureWindowList: WebKit's full-screen placeholders)
// into an IOSurface and copies it on the CPU under IOSurfaceLock right after the commit, with no GPU wait; Apple's
// IOSurfaceLock waits for the GPU writer, ours does not. With ws-gpu-present-async a WindowServer commit returns at
// schedule, so the copy read rows the GPU had not written yet (black or stale top rows).
// v1 waited when a commit wrote a surface for one of its first writes and v2 also after an idle gap; both caught window
// resizes too (every new size = new surfaces: resize very slow, user 10-29 15:05), and v3 only logged the surfaces.
// v4 waits only when the commit writes a surface whose IOSurfaceName contains ":CaptureSurface": SkyLight names its
// capture backings "SL.<pid>:CaptureSurface" (log of the user's Safari full-screen + resize test).
// "flattened-surface" is QuartzCore's render flatten cache (render-flatten-cache.cpp), read only by later GPU draws on
// the same queue, so it does not wait. The name is fetched once per surface (id + size + format) and cached.
// On by default; /Library/GPUBundles/nvmtl/ws-capture-wait-off turns the wait off (log only).
// v5 (10-30, plugin audit): the swap commit also returns at schedule, without ws-gpu-present-async, whenever
// the kernel takes WindowServer's GPU completion marks (commitAndWaitUntilSubmitted skips its wait when
// nvmtl_ws_kernel_waits, NVMTLEventSync.m). The kernel's surface waits cover the flip and the head-1 copy, not SkyLight's
// CPU copy of a capture surface (PhxAccel's page-off waits only for an NMX VRAM resource; our captures are host imports),
// so the wait is armed by either reason. /Library/GPUBundles/nvmtl/ws-capture-wait-async-only arms it by the async flag
// only, as v4 did (read every second, live). The arming is logged at each change, with syslog: nvlog keeps only the
// first 8 lines of a format, which is also why the v4 wait counter seemed to stop at 128 (user test).
// Included from NVMTLEventSync.m after NVMTLWSPresent.m.
#define NVMTL_WS_CAPTURE_SLOTS 4096
static _Atomic uint64_t g_wscap_waits, g_wscap_us, g_wscap_max_us;
typedef struct { uint32_t id, width, height, format; uint8_t capture, scanout; } nvmtl_wscap_slot;
static BOOL nvmtl_ws_capture_async_only(void) {
    static _Atomic int on = 0; static _Atomic uint64_t checked;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        struct stat st; atomic_store(&on, stat("/Library/GPUBundles/nvmtl/ws-capture-wait-async-only", &st) == 0);
        atomic_store(&checked, now);
    }
    return atomic_load(&on) == 1;
}
static BOOL nvmtl_ws_capture_log_on(void) {
    static int ws = -1; if (ws < 0) ws = getprogname() && !strcmp(getprogname(), "WindowServer");
    if (!ws) return NO;
    // 1: async GPU present, 2: the kernel takes the GPU marks, 0: WindowServer waits for its commits itself
    const int why = nvmtl_ws_gpu_async_on() ? 1 : (!nvmtl_ws_capture_async_only() && nvmtl_ws_kernel_waits()) ? 2 : 0;
    static _Atomic int was = -1;
    if (why != atomic_exchange(&was, why))
        syslog(LOG_NOTICE, "NVMTL: WindowServer capture wait: %s (%llu waits so far)", why == 1 ? "armed by async GPU present"
               : why == 2 ? "armed by the kernel's GPU marks (commits return at schedule)" : "not armed (WindowServer waits for its commits)",
               (unsigned long long)g_wscap_waits);
    return why != 0;
}
static BOOL nvmtl_ws_capture_wait_on(void) {
    static _Atomic int off = 0; static _Atomic uint64_t checked;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        struct stat st; const int v = stat("/Library/GPUBundles/nvmtl/ws-capture-wait-off", &st) == 0;
        if (v != atomic_exchange(&off, v)) nvlog("WindowServer capture wait %s (%llu waits so far)", v ? "off (log only)" : "ON", (unsigned long long)g_wscap_waits);
        atomic_store(&checked, now);
    }
    return !atomic_load(&off);
}
static nvmtl_wscap_slot *nvmtl_wscap_slot_for(nvmtl_wscap_slot *tab, uint32_t id) {
    uint32_t i = (id * 2654435761u) % NVMTL_WS_CAPTURE_SLOTS;
    for (uint32_t n = 0; n < NVMTL_WS_CAPTURE_SLOTS; n++, i = (i + 1) % NVMTL_WS_CAPTURE_SLOTS)
        if (tab[i].id == id || tab[i].id == 0) return &tab[i];
    memset(tab, 0, sizeof(nvmtl_wscap_slot) * NVMTL_WS_CAPTURE_SLOTS);   // full: forget everything, start over
    return &tab[(id * 2654435761u) % NVMTL_WS_CAPTURE_SLOTS];
}
// Answers whether this command buffer writes a SkyLight capture surface. The first write of each non-scanout surface
// is logged with its name and decision, so new surface kinds stay visible.
static BOOL nvmtl_ws_cb_writes_capture(NVMTLCommandBuffer *cb) {
    static nvmtl_wscap_slot tab[NVMTL_WS_CAPTURE_SLOTS];
    static unsigned nlogged;
    BOOL wait = NO;
    @synchronized ([NVMTLCommandBuffer class]) {
        for (NVMTLSubmissionPart *part in cb->_parts) {
            if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
            for (NVMTLTexture *t in part->surfaces) {
                IOSurfaceRef sf = t->_surf ? t->_surf : (t->_parent ? t->_parent->_surf : NULL);
                if (!sf) continue;
                const uint32_t id = IOSurfaceGetID(sf), w = (uint32_t)IOSurfaceGetWidth(sf), h = (uint32_t)IOSurfaceGetHeight(sf);
                const uint32_t fmt = (uint32_t)IOSurfaceGetPixelFormat(sf);
                nvmtl_wscap_slot *s = nvmtl_wscap_slot_for(tab, id);
                // a new id, or a reused one with another size or format: fetch the name again
                if (s->id == 0 || s->width != w || s->height != h || s->format != fmt) {
                    char nm[64] = "-";
                    CFTypeRef v = IOSurfaceCopyValue(sf, CFSTR("IOSurfaceName"));
                    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, nm, sizeof nm, kCFStringEncodingUTF8);
                    if (v) CFRelease(v);
                    *s = (nvmtl_wscap_slot){ .id = id, .width = w, .height = h, .format = fmt,
                                             .capture = strstr(nm, ":CaptureSurface") != NULL, .scanout = strstr(nm, "Scanout") != NULL };
                    if (!s->scanout && nlogged < 64) {
                        nlogged++;
                        nvlog("WindowServer capture wait: surface %u '%s' %ux%u fmt %08x written by queue %s (cb %s): %s",
                              id, nm, w, h, fmt, cb->_queue.label.UTF8String ?: "?", cb.label.UTF8String ?: "?",
                              s->capture ? "wait" : "no wait");
                    }
                }
                if (s->capture) wait = YES;
            }
        }
    }
    return wait;
}
static void nvmtl_ws_capture_wait(NVMTLCommandBuffer *cb) {
    if (!nvmtl_ws_capture_log_on()) return;
    if (!nvmtl_ws_cb_writes_capture(cb) || !nvmtl_ws_capture_wait_on()) return;
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    [cb waitUntilCompleted];
    const uint64_t us = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000;
    atomic_fetch_add(&g_wscap_us, us);
    for (uint64_t m = atomic_load(&g_wscap_max_us); us > m && !atomic_compare_exchange_weak(&g_wscap_max_us, &m, us);) {}
    const uint64_t n = atomic_fetch_add(&g_wscap_waits, 1) + 1;
    if (!(n & (n - 1)) || n % 1000 == 0)
        syslog(LOG_NOTICE, "NVMTL: WindowServer capture wait: %llu waits, %.2f ms average, %.2f ms max", (unsigned long long)n,
               atomic_load(&g_wscap_us) / 1000.0 / n, atomic_load(&g_wscap_max_us) / 1000.0);
}
