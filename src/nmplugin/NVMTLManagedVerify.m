// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLObjects.m at this exact place (one translation unit with the plugin;
   definitions keep their order): NVMTL_MANAGED_VERIFY, the Managed-buffer GPU copy vs CPU shadow check (diagnostic). */
// NVMTL_MANAGED_VERIFY=<process name>[:<length>] (contract C2, garbled-text study layer L1): in that process, every Managed buffer of
// <length> bytes (default 262144, CoreAnimation's glyph atlas; 0 = any length) that gets a didModifyRange is compared after command
// buffers complete: the GPU copy (_b, VRAM) is read back and compared byte by byte with the CPU shadow, at most twice a
// second. Bytes that stay different over consecutive checks while the buffer has no upload pending mean the upload path
// lost them (C2 broken); none mean the GPU reads the right bytes and the garbling is downstream (shader addressing).
// The third mismatching check dumps both copies to /tmp/mv-{gpu,cpu}-<pid>.bin.
static NSHashTable *gVerifySet;
static size_t gVerifyLen;
static int nvmtl_managed_verify_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_MANAGED_VERIFY"); on = 0;
        if (e && *e) {
            const char *colon = strchr(e, ':'); size_t n = colon ? (size_t)(colon - e) : strlen(e);
            const char *me = getprogname();
            on = me && strlen(me) == n && !strncmp(me, e, n);
            gVerifyLen = colon ? strtoull(colon + 1, NULL, 0) : 262144;
            if (on) nvlog("managed verify: comparing the GPU copy of %zu-byte Managed buffers with the CPU shadow (NVMTL_MANAGED_VERIFY)", gVerifyLen);
        }
    }
    return on;
}
void nvmtl_managed_verify_note(NVMTLBuffer *b) {
    if (!nvmtl_managed_verify_on() || (gVerifyLen && b->_b.size != gVerifyLen)) return;
    os_unfair_lock_lock(&gManagedLock);
    if (!gVerifySet) gVerifySet = [NSHashTable weakObjectsHashTable];
    @autoreleasepool { [gVerifySet addObject:b]; }
    os_unfair_lock_unlock(&gManagedLock);
}
void nvmtl_managed_verify_check(void) {
    static _Atomic int busy; static uint64_t last; static nvk_buffer tmp; static unsigned checks, bad;
    static uint8_t *prevBad; static size_t prevLen; static void *prevBuf;   // per byte: mismatched at the previous check of prevBuf
    if (!gVerifySet || atomic_exchange(&busy, 1)) return;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - last < 200000000ull) { atomic_store(&busy, 0); return; }
    last = now;
    NSArray *all;
    os_unfair_lock_lock(&gManagedLock); all = gVerifySet.allObjects; os_unfair_lock_unlock(&gManagedLock);
    for (NVMTLBuffer *b in all) {
        const size_t len = b->_b.size;
        if (!b->_shadow.map || b->_b.map || (gVerifyLen && len != gVerifyLen) || !len) continue;
        if (b->_dirtyHi > b->_dirtyLo || atomic_load(&gManagedAny)) continue;   // an upload is still owed: not comparable
        if (tmp.size < len) { if (tmp.buf) nvmtl_vk_buffer_destroy(&tmp); memset(&tmp, 0, sizeof tmp);
            if (nvmtl_vk_buffer_create(len, 1, &tmp)) { memset(&tmp, 0, sizeof tmp); nvlog("managed verify: no readback buffer"); continue; } }
        if (prevBuf != (__bridge void *)b || prevLen != len) { free(prevBad); prevBad = calloc(1, len); prevLen = len; prevBuf = (__bridge void *)b; }
        if (nvmtl_vk_copy_buffer(nvmtl_device_queue(), &b->_b, 0, &tmp, 0, len)) { nvlog("managed verify: readback FAILED"); continue; }
        const uint8_t *g = tmp.map, *c = b->_shadow.map;
        size_t diff = 0, stable = 0, runs = 0, firstLo = 0, firstHi = 0, lastLo = 0; char where[256]; int wl = 0; where[0] = 0;
        for (size_t i = 0; i < len; i++) {
            const int d = g[i] != c[i];
            if (d) { diff++; if (prevBad[i]) stable++;
                if (!i || g[i - 1] == c[i - 1]) { runs++; lastLo = i; if (runs == 1) firstLo = i;
                    if (runs <= 8 && wl < 200) wl += snprintf(where + wl, sizeof where - wl, " %zx", i); }
                if (runs == 1) firstHi = i; }
            prevBad[i] = d;
        }
        checks++;
        nvlog("managed verify #%u %p (%zu B, label %s): %zu bytes differ (%zu also at the previous check) in %zu runs [first %zx..%zx, last run at %zx; starts:%s]",
              checks, (__bridge void *)b, len, b->_label.UTF8String ?: "-", diff, stable, runs, firstLo, firstHi, lastLo, where);
        if (diff && ++bad == 3) {
            char p[96]; snprintf(p, sizeof p, "/tmp/mv-gpu-%d.bin", getpid());
            FILE *f = fopen(p, "wb"); if (f) { fwrite(g, 1, len, f); fclose(f); }
            snprintf(p, sizeof p, "/tmp/mv-cpu-%d.bin", getpid());
            f = fopen(p, "wb"); if (f) { fwrite(c, 1, len, f); fclose(f); }
            nvlog("managed verify: dumped /tmp/mv-{gpu,cpu}-%d.bin", getpid());
        }
    }
    atomic_store(&busy, 0);
}
