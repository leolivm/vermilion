// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLEventSync.m at this exact place (one translation unit with the
   plugin; definitions keep their order): WindowServer present policy: screen-write wait, swap no-wait, E0 commit trace, GPU present (CDCOPY display surfaces, cdhold, async), the call into nvmtl_vk_scanout_present. */
#include "nvmtl_headmap_gate.c"
// WindowServer screen-write wait (A/B switch, no restart): while /Library/GPUBundles/nvmtl/ws-screen-wait exists, a
// WindowServer command buffer that writes a full-screen surface is not returned from commit until the GPU has finished
// it. Apple's family waits for a plane surface's last GPU write before it calls performTransaction (transaction_end
// merges the resource's lastWriteEvent; RE'd 2026-10-25), but our composites never advance family stamps and the
// kernel surface marks are dead on RADV, so the display pipe's copy can read a frame still being drawn whenever the
// swap does not follow commitAndWaitUntilSubmitted (half of the full-screen writes, probe 2026-10-25). Only the GPU
// completion is awaited (not the handlers), so a commit made from a completion handler cannot deadlock on its queue.
static BOOL nvmtl_ws_gpu_async_on(void);   /* below, with the GPU present switches */
static __thread BOOL nvmtl_commit_nowait;   // set around the swap commit's own commit by commitAndWaitUntilSubmitted
static BOOL nvmtl_ws_swap_nowait_on(void) {
    static _Atomic int on = 0; static _Atomic uint64_t checked;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        struct stat st; const int v = stat("/Library/GPUBundles/nvmtl/ws-swap-nowait", &st) == 0;
        if (v != atomic_exchange(&on, v)) nvlog("WindowServer swap commit: %s", v ? "waited until submitted only (ws-swap-nowait)" : "waited until complete");
        atomic_store(&checked, now);
    }
    return atomic_load(&on) == 1;
}
static _Atomic uint64_t g_wsw_waits, g_wsw_us;
static BOOL nvmtl_ws_screen_wait_on(void) {
    static int ws = -1; static _Atomic int on = 0; static _Atomic uint64_t checked;
    if (ws < 0) ws = getprogname() && !strcmp(getprogname(), "WindowServer");
    if (!ws) return NO;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        struct stat st; const int v = stat("/Library/GPUBundles/nvmtl/ws-screen-wait", &st) == 0;
        if (v != atomic_exchange(&on, v)) nvlog("WindowServer screen-write wait %s (%llu waits so far)", v ? "ON" : "off", (unsigned long long)g_wsw_waits);
        atomic_store(&checked, now);
    }
    return atomic_load(&on) == 1 && !nvmtl_ws_gpu_async_on();
}
static BOOL nvmtl_cb_writes_screen(NVMTLCommandBuffer *cb) {
    for (NVMTLSubmissionPart *part in cb->_parts) {
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
        for (NVMTLTexture *t in part->surfaces) {
            IOSurfaceRef sf = t->_surf ? t->_surf : (t->_parent ? t->_parent->_surf : NULL);
            if (sf && IOSurfaceGetWidth(sf) >= 1920 && IOSurfaceGetHeight(sf) >= 1080) return YES;
        }
    }
    return NO;
}
// E0 trace (nvmtl_e0, NVMTL_E0_TRACE=1): every WindowServer commit that writes a full-screen surface, with its queue and
// command buffer labels, whether WindowServer waits on it (the swap commit), and each such surface's id, format and name.
static const void *const nvmtl_e0_key = &nvmtl_e0_key;
static void nvmtl_e0_commit(NVMTLCommandBuffer *cb, BOOL swap) {
    if (!nvmtl_e0_on()) return;
    char line[400]; size_t o = 0; unsigned n = 0;
    for (NVMTLSubmissionPart *part in cb->_parts) {
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
        for (NVMTLTexture *t in part->surfaces) {
            IOSurfaceRef sf = t->_surf ? t->_surf : (t->_parent ? t->_parent->_surf : NULL);
            if (!sf || IOSurfaceGetWidth(sf) < 1920 || IOSurfaceGetHeight(sf) < 1080 || o > sizeof line - 80) continue;
            char nm[48] = "-"; CFTypeRef v = IOSurfaceCopyValue(sf, CFSTR("IOSurfaceName"));
            if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, nm, sizeof nm, kCFStringEncodingUTF8);
            if (v) CFRelease(v);
            for (char *c = nm; *c; c++) if (*c == ' ') *c = '_';
            const uint32_t f = CFSwapInt32HostToBig(IOSurfaceGetPixelFormat(sf));
            o += (size_t)snprintf(line + o, sizeof line - o, " %u/%.4s/%s", IOSurfaceGetID(sf), (const char *)&f, nm); n++;
        }
    }
    if (!n) return;
    objc_setAssociatedObject(cb, nvmtl_e0_key, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    nvmtl_e0("C cb %p %s q=%s l=%s%s", (__bridge void *)cb, swap ? "SWAP" : "-", cb->_queue.label.UTF8String ?: "?", cb.label.UTF8String ?: "?", line);
}
// WindowServer GPU present switch (/Library/GPUBundles/nvmtl/ws-gpu-present, re-read every second): after the
// swap-preceding commit has finished on the GPU, its display surface goes to a VRAM slot the GPU copies it into, and
// HUBP0 flips there (nvmtl_vk_scanout_present). Deleting the file gives the plane back; it stays off for the process.
static BOOL nvmtl_ws_gpu_present_on(void) {
    static _Atomic int on = 0; static _Atomic uint64_t checked;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        struct stat st; const int v = stat("/Library/GPUBundles/nvmtl/ws-gpu-present", &st) == 0;
        if (!v && atomic_load(&on)) nvmtl_vk_scanout_stop("ws-gpu-present removed");
        atomic_store(&on, v); atomic_store(&checked, now);
    }
    return atomic_load(&on) == 1;
}
// The display surface a WindowServer command buffer wrote, noted at commit (its parts are gone once it has finished).
static const void *const nvmtl_ws_shown_key = &nvmtl_ws_shown_key;
// NVMTL_WS_COALESCE_US (E2): a presented surface's generation, bumped by every presented swap commit that writes it.
// SkyLight often composes a frame in two waited commits (its own, then a Core Animation one); when the second is
// committed after the first has finished, presenting at the first shows the frame without the second's layers.
static _Atomic uint64_t nvmtl_ws_gen[64];
static const void *const nvmtl_ws_gen_key = &nvmtl_ws_gen_key;
// Whether the commit that noted the display surface was one WindowServer waited on (CDCOPY mode).
static const void *const nvmtl_ws_waited_key = &nvmtl_ws_waited_key;
// /Library/GPUBundles/nvmtl/ws-cdcopy-hold (CDCOPY mode), holding a number of microseconds: the present of a display surface
// written by an UNWAITED CoreDisplay commit waits that long, and is skipped if a later commit to the same surface came in.
// CoreDisplay often follows its unwaited copy with a waited commit to the same surface 0.2-0.45 ms later, and the unwaited
// one could finish and flip before that commit existed: an incomplete frame on the panel at pane switches (E0 trace
// 2026-10-28). Absent or 0: present at once.
static long nvmtl_ws_cdcopy_hold_us(void) {
    static _Atomic long us = 0; static _Atomic uint64_t checked;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        long v = 0; FILE *f = fopen("/Library/GPUBundles/nvmtl/ws-cdcopy-hold", "r");
        if (f) { if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f); }
        if (v < 0 || v > 20000) v = 0;
        if (v != atomic_exchange(&us, v)) nvlog("ws gpu present: unwaited display commits hold %ld us (ws-cdcopy-hold)", v);
        atomic_store(&checked, now);
    }
    return atomic_load(&us);
}
static void nvmtl_ws_note_shown(NVMTLCommandBuffer *cb) {
    if (!nvmtl_ws_gpu_present_on()) return;
    // NVMTL_WS_SKIP_CDQUEUE=1 (E1): CoreDisplay's own waited commits (com.apple.CoreDisplay.* queues) prepare the next
    // display surface - they copy the unchanged rects into it before SkyLight composes the damage there - so the surface
    // is incomplete when they finish; only SkyLight's composite commits are presented (E0 2026-10-25: 3% of presents,
    // 40% of the flicker frames).
    static int skipCd = -1;
    if (skipCd < 0) { const char *e = getenv("NVMTL_WS_SKIP_CDQUEUE"); skipCd = e && e[0] == '1';
        if (skipCd) nvlog("ws gpu present: CoreDisplay queue commits are not presented (NVMTL_WS_SKIP_CDQUEUE)"); }
    if (skipCd && [cb->_queue.label hasPrefix:@"com.apple.CoreDisplay"]) return;
    IOSurfaceRef shown = NULL;
    for (NVMTLSubmissionPart *part in cb->_parts) {
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
        for (NVMTLTexture *t in part->surfaces) {
            IOSurfaceRef sf = t->_surf ? t->_surf : (t->_parent ? t->_parent->_surf : NULL);
            if (sf && IOSurfaceGetWidth(sf) >= 1920 && IOSurfaceGetHeight(sf) >= 1080) {
                static uint32_t seen[16]; static unsigned n;
                const uint32_t key = IOSurfaceGetID(sf) ^ (IOSurfaceGetPixelFormat(sf) << 8); unsigned i = 0;
                while (i < n && seen[i] != key) i++;
                if (i == n && n < 16) { seen[n++] = key; nvlog("ws gpu present: swap commit writes full-screen surface %u fmt %.4s", IOSurfaceGetID(sf),
                                                              (const char *)&(uint32_t){ CFSwapInt32HostToBig(IOSurfaceGetPixelFormat(sf)) }); }
            }
            if (sf && IOSurfaceGetWidth(sf) >= 1920 && IOSurfaceGetHeight(sf) >= 1080 &&
                (IOSurfaceGetPixelFormat(sf) == 0x42475241u || IOSurfaceGetPixelFormat(sf) == 0x6C313072u)) shown = sf;   /* BGRA or l10r */
        }
    }
    if (shown) {
        objc_setAssociatedObject(cb, nvmtl_ws_shown_key, (__bridge id)shown, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        const uint64_t g = atomic_fetch_add(&nvmtl_ws_gen[IOSurfaceGetID(shown) & 63], 1) + 1;
        objc_setAssociatedObject(cb, nvmtl_ws_gen_key, @(g), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
}
// NVMTL_WS_PRESENT_CDCOPY=1 (E3): SkyLight composes each frame into one of two surfaces, drawing only what changed; a
// CoreDisplay commit that WindowServer does not wait on then writes the frame into one of two OTHER full-screen surfaces,
// which keep every region current. Those (the display set) are what this presents; SkyLight's targets, whose unchanged
// regions are a frame or more old, are not (E0 traces 2026-10-25: Dock band ping-pong between the two SkyLight targets).
static BOOL nvmtl_ws_cdcopy_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_WS_PRESENT_CDCOPY"); on = e && e[0] == '1';
        if (on) nvlog("ws gpu present: presenting CoreDisplay's display surfaces, not SkyLight's composite targets (NVMTL_WS_PRESENT_CDCOPY)"); }
    return on == 1;
}
static void nvmtl_ws_note_display(NVMTLCommandBuffer *cb, BOOL waited) {
    if (!nvmtl_ws_gpu_present_on() || ![cb->_queue.label hasPrefix:@"com.apple.CoreDisplay"]) return;
    static nvmtl_ws_set display;
    IOSurfaceRef shown = NULL;
    for (NVMTLSubmissionPart *part in cb->_parts) {
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
        for (NVMTLTexture *t in part->surfaces) {
            IOSurfaceRef sf = t->_surf ? t->_surf : (t->_parent ? t->_parent->_surf : NULL);
            if (sf && IOSurfaceGetWidth(sf) >= 1920 && IOSurfaceGetHeight(sf) >= 1080 &&
                (IOSurfaceGetPixelFormat(sf) == 0x42475241u || IOSurfaceGetPixelFormat(sf) == 0x6C313072u)) shown = sf;
        }
    }
    if (!shown) return;
    const uint32_t sid = IOSurfaceGetID(shown);
    if (!nvmtl_hm_gate_join(sid)) return;   // M7 S4b: another head's surface (enforce only)
    if (!nvmtl_ws_set_has(&display, sid)) {
        if (waited) return;   // a waited CoreDisplay commit on a surface outside the set prepares a SkyLight target
        nvmtl_ws_set_add(&display, sid);   // over the oldest member once full (nvmtl_headmap.h)
        nvlog("ws gpu present: surface %u is a display surface (written by an unwaited CoreDisplay commit)", sid);
    }
    objc_setAssociatedObject(cb, nvmtl_ws_shown_key, (__bridge id)shown, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    const uint64_t g = atomic_fetch_add(&nvmtl_ws_gen[sid & 63], 1) + 1;
    objc_setAssociatedObject(cb, nvmtl_ws_gen_key, @(g), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(cb, nvmtl_ws_waited_key, waited ? @YES : nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
// /Library/GPUBundles/nvmtl/ws-gpu-present-async (with ws-gpu-present): WindowServer does not wait for its swap commit
// any more; the copy and the flip run when the GPU has finished it, on the execution worker (serial with the GPU work
// that follows, so the surface is copied before anything can draw into it again). The display pipe's CPU copy then goes
// to the console surface, which is off screen, so neither its wait (ws-screen-wait) nor its timing matter.
static BOOL nvmtl_ws_gpu_async_on(void) {
    static _Atomic int on = 0; static _Atomic uint64_t checked;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        struct stat st; const int v = stat("/Library/GPUBundles/nvmtl/ws-gpu-present-async", &st) == 0;
        if (v != atomic_exchange(&on, v)) nvlog("ws gpu present: async %s", v ? "ON (WindowServer no longer waits for its swap commit)" : "off");
        atomic_store(&checked, now);
    }
    return atomic_load(&on) == 1 && nvmtl_vk_scanout_on();
}
static void nvmtl_ws_gpu_present(NVMTLCommandBuffer *cb) {
    if (!nvmtl_ws_gpu_present_on()) return;
    IOSurfaceRef shown = (__bridge IOSurfaceRef)objc_getAssociatedObject(cb, nvmtl_ws_shown_key);
    if (!shown) { static int said; if (said++ < 8) nvlog("ws gpu present: a swap commit wrote no BGRA/l10r display surface"); return; }
    { static int said; if (said++ < 4) nvlog("ws gpu present: presenting surface %u %zux%zu", IOSurfaceGetID(shown), IOSurfaceGetWidth(shown), IOSurfaceGetHeight(shown)); }
    (void)nvmtl_vk_scanout_present(shown, IOSurfaceGetID(shown), IOSurfaceGetBaseAddress(shown), (uint32_t)IOSurfaceGetWidth(shown),
                                   (uint32_t)IOSurfaceGetHeight(shown), (uint32_t)IOSurfaceGetBytesPerRow(shown),
                                   IOSurfaceGetPixelFormat(shown) == 0x6C313072u);
    objc_setAssociatedObject(cb, nvmtl_ws_shown_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
