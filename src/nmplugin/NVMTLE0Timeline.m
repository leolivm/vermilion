// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLEventSync.m at this exact place, inside @implementation
   NVMTLCommandBuffer (EventSubmission) (C functions only; one translation unit with the plugin; definitions keep their order):
   the E0 trace "T" line per submission part (drag-lag timeline, log only). */
/* Drag-lag timeline (E0 trace, traced WindowServer command buffers only): one "T" line per submission part with, in uptime
   microseconds, when the worker picked it, when it was submitted, when the GPU started and ended it (device timestamps
   mapped to host time, 0 when unknown), when the fence wait returned and when the part's post-processing was done. */
static void nvmtl_e0_part(NVMTLCommandBuffer *cb, NSUInteger k, NSUInteger n, const nvk_cmdbuf *c, uint64_t pick_ns, BOOL ahead,
                          NVMTLSubmissionPart *part) {
    double gs = 0, ge = 0;
    if (!c->timing.valid || nvmtl_vk_interval_seconds(&c->timing, &gs, &ge)) gs = ge = 0;
    // Drag lag S0: the IOSurface-backed textures this part uses that the GPU reaches in system RAM (host imports): how many
    // and how many KB, split into read-only and written by this part, plus the surface textures that are not host imports
    unsigned hr = 0, hw = 0, other = 0; unsigned long long hrb = 0, hwb = 0;
    if ([part->resources isKindOfClass:[NVMTLResSet class]]) {
        NSMutableSet *seen = [NSMutableSet set];
        for (id o in ((NVMTLResSet *)part->resources)->_s) if ([o isKindOfClass:[NVMTLTexture class]]) {
            NVMTLTexture *t = o; NVMTLTexture *base = t->_parent ?: t;
            if (!base->_surf || [seen containsObject:base]) continue;
            [seen addObject:base];
            if (!base->_hostOn) { other++; continue; }
            const BOOL wr = [part->surfaces containsObject:base] || [part->surfaces containsObject:t];
            const unsigned long long b = IOSurfaceGetAllocSize(base->_surf);
            if (wr) { hw++; hwb += b; } else { hr++; hrb += b; }
        }
    }
    nvmtl_e0("T cb %p part %lu/%lu %s pick %llu sub %llu gs %llu ge %llu wake %llu done %llu hostr %u %llu hostw %u %llu surfo %u", (__bridge void *)cb,
             (unsigned long)k, (unsigned long)n, ahead ? "ahead" : "sync", (unsigned long long)(pick_ns / 1000),
             (unsigned long long)(c->timing.host_before_ns / 1000), (unsigned long long)(gs * 1e6), (unsigned long long)(ge * 1e6),
             (unsigned long long)(c->timing.host_after_ns / 1000), (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000),
             hr, hrb >> 10, hw, hwb >> 10, other);
}
