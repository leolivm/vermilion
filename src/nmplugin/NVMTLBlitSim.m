// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLObjects.m at this exact place, inside @implementation
   NVMTLRenderCommandEncoder (C functions only; one translation unit with the plugin; definitions keep their order):
   NVMTL_BLIT_SIM, buffer_image_blit inputs compared between encode and completion (diagnostic). */
// NVMTL_BLIT_SIM=1 (garbled-text study, layer L1b): for each CoreAnimation buffer_image_blit draw (text: one instance per glyph, rect records
// {origin, size, offset, bpr} at vertex buffer 1, the glyph bytes at fragment buffer 9), the rect records and a hash of
// the glyph bytes each record points at are kept at encode time and computed again from the same CPU memory when the
// command buffer completes. A difference means the client changed the inputs while the GPU work was in flight.
static char nvmtl_blit_sim_key;
static int nvmtl_blit_sim_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_BLIT_SIM"); on = e && e[0] == '1';
        if (on) nvlog("blit sim: buffer_image_blit inputs compared between encode and completion (NVMTL_BLIT_SIM)"); }
    return on;
}
static const uint8_t *nvmtl_blit_cpu(NVMTLBuffer *b) {
    return b ? (const uint8_t *)(b->_shadow.map ? b->_shadow.map : (b->_hostPtr ? b->_hostPtr : b->_b.map)) : NULL;
}
static uint64_t nvmtl_blit_glyph_hash(const uint8_t *atlas, size_t alen, const float *r) {
    // r = {x, y, w, h, offset, bpr}: the bytes the fragment shader reads are offset + u + v * bpr, u < w, v < h
    uint64_t h = 1469598103934665603ull;
    const long w = (long)r[2], hh = (long)r[3], off = (long)r[4], bpr = (long)r[5];
    if (w <= 0 || hh <= 0 || w > 512 || hh > 512) return 0;
    for (long v = 0; v < hh; v++) for (long u = 0; u < w; u++) {
        const long i = off + u + v * bpr;
        const uint8_t c = (i >= 0 && (size_t)i < alen) ? atlas[i] : 0xEE;
        h = (h ^ c) * 1099511628211ull;
    }
    return h;
}
static void nvmtl_blit_sim_note(NVMTLCommandBuffer *cb, NVMTLBuffer *rb, NSUInteger roff, NVMTLBuffer *ab, NSUInteger aoff,
                                NSUInteger base, NSUInteger n, NVMTLTexture *target) {
    const uint8_t *rc = nvmtl_blit_cpu(rb), *ac = nvmtl_blit_cpu(ab);
    static _Atomic unsigned skipped;
    if (!rc || !ac || !n || n > 8192 || roff + (base + n) * 24 > rb->_b.size || aoff >= ab->_b.size) {
        if (skipped++ < 10) nvlog("blit sim: draw not followed (rects %p cpu %p off %lu, atlas %p cpu %p off %lu, base %lu, %lu instances)",
                                  (__bridge void *)rb, rc, (unsigned long)roff, (__bridge void *)ab, ac, (unsigned long)aoff, (unsigned long)base, (unsigned long)n);
        return;
    }
    const size_t alen = ab->_b.size;
    NSData *rects = [NSData dataWithBytes:rc + roff + base * 24 length:n * 24];
    NSMutableData *hashes = [NSMutableData dataWithLength:n * 8];
    const float *r = rects.bytes; uint64_t *hv = hashes.mutableBytes;
    for (NSUInteger k = 0; k < n; k++) hv[k] = nvmtl_blit_glyph_hash(ac + aoff, alen - aoff, r + k * 6);
    static _Atomic unsigned dumped;
    const unsigned dk = dumped++;
    if (dk < 8) {   // the first draws leave their inputs in /tmp for an offline render (the tool is not included)
        char p[96]; snprintf(p, sizeof p, "/tmp/bs-%d-%u.rects", getpid(), dk);
        [rects writeToFile:@(p) atomically:NO];
        snprintf(p, sizeof p, "/tmp/bs-%d-%u.atlas", getpid(), dk);
        [[NSData dataWithBytes:ac + aoff length:alen - aoff] writeToFile:@(p) atomically:NO];
        nvlog("blit sim: dumped draw %u (%lu glyphs, atlas %p+%lu) to /tmp/bs-%d-%u.*", dk, (unsigned long)n, (__bridge void *)ab, (unsigned long)aoff, getpid(), dk);
    }
    NSMutableArray *list = objc_getAssociatedObject(cb, &nvmtl_blit_sim_key);
    if (!list) { list = [NSMutableArray new]; objc_setAssociatedObject(cb, &nvmtl_blit_sim_key, list, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    [list addObject:@[rb, @(roff + base * 24), ab, @(aoff), rects, hashes, target ?: (id)[NSNull null], @(dk)]];
}
// The render target of a big text draw, read back when its command buffer completed (garbled-text study, layer L3): the GPU image through
// getBytes and, for an IOSurface, the surface memory other processes (WindowServer) read. /tmp/bt-<pid>-<draw>.{gpu,surf}
static void nvmtl_blit_sim_target(NVMTLTexture *t, unsigned dk) {
    if (!t || ![t isKindOfClass:[NVMTLTexture class]]) return;
    const NSUInteger w = t.width, h = t.height;
    if (!w || !h || w > 4096 || h > 4096) return;
    NSMutableData *d = [NSMutableData dataWithLength:w * h * 4];
    [t getBytes:d.mutableBytes bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    char p[96]; snprintf(p, sizeof p, "/tmp/bt-%d-%u.gpu", getpid(), dk); [d writeToFile:@(p) atomically:NO];
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (r->_surf) {
        IOSurfaceRef sf = r->_surf; IOSurfaceLock(sf, kIOSurfaceLockReadOnly, NULL);
        NSData *sd = [NSData dataWithBytes:IOSurfaceGetBaseAddress(sf) length:IOSurfaceGetAllocSize(sf)];
        IOSurfaceUnlock(sf, kIOSurfaceLockReadOnly, NULL);
        snprintf(p, sizeof p, "/tmp/bt-%d-%u.surf", getpid(), dk); [sd writeToFile:@(p) atomically:NO];
        nvlog("blit sim: target of draw %u: %lux%lu fmt %lu, IOSurface %u %zux%zu bpr %zu -> /tmp/bt-%d-%u.{gpu,surf}", dk, (unsigned long)w,
              (unsigned long)h, (unsigned long)t.pixelFormat, IOSurfaceGetID(sf), IOSurfaceGetWidth(sf), IOSurfaceGetHeight(sf), IOSurfaceGetBytesPerRow(sf), getpid(), dk);
    } else nvlog("blit sim: target of draw %u: %lux%lu fmt %lu (no IOSurface) -> /tmp/bt-%d-%u.gpu", dk, (unsigned long)w, (unsigned long)h,
                 (unsigned long)t.pixelFormat, getpid(), dk);
}
void nvmtl_blit_sim_check(NVMTLCommandBuffer *cb) {
    NSArray *list = objc_getAssociatedObject(cb, &nvmtl_blit_sim_key);
    if (!list) return;
    static _Atomic unsigned long long draws, glyphs, rectChanged, bytesChanged, said;
    for (NSArray *e in list) {
        NVMTLBuffer *rb = e[0], *ab = e[2]; const NSUInteger ro = [e[1] unsignedLongValue], ao = [e[3] unsignedLongValue];
        NSData *rects = e[4], *hashes = e[5];
        const NSUInteger n = rects.length / 24;
        const uint8_t *rc = nvmtl_blit_cpu(rb), *ac = nvmtl_blit_cpu(ab);
        if (!rc || !ac) continue;
        const float *r0 = rects.bytes; const uint64_t *h0 = hashes.bytes;
        unsigned rch = 0, bch = 0, first = ~0u;
        for (NSUInteger k = 0; k < n; k++) {
            if (memcmp(rc + ro + k * 24, r0 + k * 6, 24)) rch++;
            if (nvmtl_blit_glyph_hash(ac + ao, ab->_b.size - ao, r0 + k * 6) != h0[k]) { bch++; if (first == ~0u) first = (unsigned)k; }
        }
        draws++; glyphs += n; rectChanged += rch; bytesChanged += bch;
        if (n >= 100 && [e[7] unsignedIntValue] < 8 && e[6] != [NSNull null]) nvmtl_blit_sim_target(e[6], [e[7] unsignedIntValue]);
        if ((rch || bch) && said < 40) { said++;
            nvlog("blit sim: draw of %lu glyphs (rects %s %p+%lu, atlas %s %p+%lu len %lu): %u rect records and %u glyphs' bytes changed "
                  "before completion (first glyph %u: off %.0f bpr %.0f %.0fx%.0f)", (unsigned long)n,
                  rb->_storage == MTLStorageModeManaged ? "managed" : rb->_storage == MTLStorageModeShared ? "shared" : "other", (__bridge void *)rb,
                  (unsigned long)ro, ab->_storage == MTLStorageModeManaged ? "managed" : ab->_storage == MTLStorageModeShared ? "shared" : "other",
                  (__bridge void *)ab, (unsigned long)ao, (unsigned long)ab->_b.size, rch, bch, first == ~0u ? 0 : first,
                  first == ~0u ? 0 : r0[first * 6 + 4], first == ~0u ? 0 : r0[first * 6 + 5], first == ~0u ? 0 : r0[first * 6 + 2], first == ~0u ? 0 : r0[first * 6 + 3]); }
    }
    if (draws <= 5 || !(draws % 100)) nvlog("blit sim: %llu draws, %llu glyphs, %llu rect records and %llu glyphs' bytes changed in flight",
                              (unsigned long long)draws, (unsigned long long)glyphs, (unsigned long long)rectChanged, (unsigned long long)bytesChanged);
}
