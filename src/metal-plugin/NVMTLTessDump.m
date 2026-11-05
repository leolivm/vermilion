// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVMTL_TESS_DUMP=1 (diagnostic, MEMOJI-NONDETERMINISTIC): every patch draw remembers its tessellation factor buffer
// range, and when the command buffer has completed the bytes the GPU left there are hashed and logged with the first
// factors, so a good and a bad Memoji run (same binaries, different hair) can be compared draw by draw. Only CPU-visible
// factor buffers can be read; a private one is logged as such. Included from NVMTLDevice.m before NVMTLObjects.m.
static int nvmtl_tess_dump_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_TESS_DUMP"); on = e && e[0] == '1'; } return on;
}
static void nvmtl_tess_dump_note(id<MTLCommandBuffer> cb, id<MTLBuffer> fb, NSUInteger off, NSUInteger stride, NSUInteger count,
                                 NSUInteger instances, NSString *vname) {
    if (!nvmtl_tess_dump_on() || !cb || !fb) return;
    static _Atomic unsigned draws;
    const unsigned d = atomic_fetch_add(&draws, 1);
    if (d >= 400) return;   // the first frames are what decides the run
    // per-patch factors: a triangle patch is 4 halfs (3 edge + 1 inside), a quad patch 6 halfs (4 + 2); stride covers instances
    const NSUInteger bytes = MIN((NSUInteger)fb.length - MIN(off, (NSUInteger)fb.length), MAX(stride * MAX(instances, (NSUInteger)1), count * 12));
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        const uint8_t *p = (const uint8_t *)fb.contents;
        if (!p) { nvlog("TESSDUMP #%u %s: factor buffer %p (gpu 0x%llx, %lu B, cb %p) is not CPU-visible (%lu bytes at %lu, %lu patches x%lu)", d,
                        vname.UTF8String ?: "?", (__bridge void *)fb, (unsigned long long)fb.gpuAddress, (unsigned long)fb.length,
                        (__bridge void *)done, (unsigned long)bytes, (unsigned long)off, (unsigned long)count, (unsigned long)instances); return; }
        p += off;
        uint64_t h = 1469598103934665603ull; for (NSUInteger i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628211ull; }
        char first[96] = ""; size_t k = 0;
        for (NSUInteger i = 0; i + 1 < bytes && i < 24; i += 2) {
            const uint16_t v = (uint16_t)(p[i] | p[i + 1] << 8);
            k += (size_t)snprintf(first + k, sizeof first - k, "%04x ", v); if (k >= sizeof first) break;
        }
        nvlog("TESSDUMP #%u %s: %lu patches x%lu, %lu bytes at %lu stride %lu, fnv %016llx, first %s", d, vname.UTF8String ?: "?",
              (unsigned long)count, (unsigned long)instances, (unsigned long)bytes, (unsigned long)off, (unsigned long)stride,
              (unsigned long long)h, first);
    }];
}
