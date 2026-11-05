// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLDevice.m after NVMTLSurfaceUsage.m, before
   -newTextureWithDescriptor:iosurface:plane: (one translation unit with the plugin).
   Plugins x420cnt / x420fa.

   When a 4K movie reaches its end of stream in Safari, WindowServer wraps ~150 decoder surfaces (3840x2160 x420, two
   planes) within a few seconds. Each is host-imported whole (23.7 MiB) and its backing is kept by nvmtl_surface_keep
   while the decoder pool still holds the surface, so the imports reach the kext's per-client host-import cap
   (kHostCap, 4 GiB); every later import fails, the planes fall back to private images filled by CPU copy-in, and those
   fill the context's page tables until image creation fails.

   Counters (log only): live host imports of this process (count, bytes, peak; all and multi-plane), new / freed /
   failed imports, multi-plane refusals, fallback images of planes and their CPU copy-in, "VRAM full" failures and
   image create FAILED, and a census of the multi-plane host-backed keep entries. One MEMCENSUS line every 2 s while
   anything moves (NVMTL_MEMCENSUS=1), and one line each for the first host-import failure (its stage, VkResult and
   the live bytes then) and the first multi-plane refusal (the keep census and what the trim would release).
   RADV turns every refusal of the kext's BO_IMPORT_HOST call into VK_ERROR_INVALID_EXTERNAL_HANDLE; the IOReturn does
   not reach the plugin.

   Trim (NVMTL_X420_TRIM, F-A): a host-backed plane lies on the surface's own pages, so its keep entry is only a cache
   (the content stays in the pages). When the multi-plane host-backed keep set passes NVMTL_X420_CAP_MB (1024), the
   entries not wrapped for NVMTL_X420_IDLE_MS (1000) are dropped from the keep table, oldest first and both planes of a
   surface together, down to 3/4 of the cap. When all live host imports pass 3 GiB the idle rule is waived. A dropped
   entry whose texture CA or a command buffer still holds stays alive through that holder. */
#ifndef NVMTL_X420_NOW
#define NVMTL_X420_NOW() clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
#endif
#define NVMTL_X420_TRIM_DEFAULT 0          /* x420cnt: counters only (x420fa: 1) */
#define NVMTL_X420_KEXT_CAP_MB 4096ull     /* phx_globals.cpp kHostCap, per client */
#define NVMTL_X420_WAIVE_MB 3072ull        /* all live host imports past this: the idle rule is waived */
static int nvmtl_memcensus_on(void);       /* nvmtl_imgmem.c */

enum { X4_NEW, X4_MPNEW, X4_GONE, X4_MPGONE, X4_FAIL, X4_REFUSED, X4_REFUSED_IMPORT, X4_FB, X4_FB_KB, X4_CIN, X4_CIN_KB,
       X4_CIN_NS, X4_VRAMFULL, X4_IMGFAIL, X4_MPIMGFAIL, X4_TRIM, X4_TRIM_KB, X4_TRIM_NS, X4_N };
static pthread_mutex_t g_x4_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef g_x4_live;   /* VkDeviceMemory -> (size << 1) | multi-plane */
static uint64_t g_x4[X4_N], g_x4_last[X4_N], g_x4_n, g_x4_b, g_x4_peak, g_x4_mpn, g_x4_mpb, g_x4_mppeak, g_x4_t0,
                g_x4_last_n, g_x4_last_mpn;
static uint64_t g_x4_k_ent, g_x4_k_imp, g_x4_k_b, g_x4_k_idle_b;   /* keep census, written under the device lock */
static int g_x4_first_refusal, g_x4_kinds;
static struct { int stage, vr, cls; } g_x4_kind[8];   /* the failure kinds already logged */

// NVMTL_X420_TRIM: 1 = drop idle multi-plane keep entries past the cap (F-A), 0 = counters only (x420fa: on, x420cnt: off)
static int nvmtl_x420_trim_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_X420_TRIM"); on = e && *e ? atoi(e) : NVMTL_X420_TRIM_DEFAULT; on = on != 0; }
    return on;
}

// NVMTL_X420_CAP_MB: the multi-plane host-backed keep set ceiling in MB; a trim goes down to 3/4 of it
static uint64_t nvmtl_x420_cap(void)
{
    static uint64_t c;
    if (!c) { const char *e = getenv("NVMTL_X420_CAP_MB"); const int mb = e && *e ? atoi(e) : 1024; c = (uint64_t)(mb > 0 ? mb : 1024) << 20; }
    return c;
}

// NVMTL_X420_IDLE_MS: an entry is trimmed only when its surface was not wrapped for this long (waived past 3 GiB live)
static uint64_t nvmtl_x420_idle_ns(void)
{
    static uint64_t n = 1;
    if (n == 1) { const char *e = getenv("NVMTL_X420_IDLE_MS"); const int ms = e && *e ? atoi(e) : 1000; n = (uint64_t)(ms >= 0 ? ms : 1000) * 1000000ull; }
    return n;
}

/* the census line, at most every 2 s, only when something moved; called with g_x4_lock held, fills out (or "") */
static void nvmtl_x420_census_locked(char *out, size_t n)
{
    out[0] = 0;
    if (!nvmtl_memcensus_on()) return;
    const uint64_t now = NVMTL_X420_NOW();
    if (g_x4_t0 && now - g_x4_t0 < 2000000000ull) return;
    uint64_t d[X4_N]; int any = g_x4_n != g_x4_last_n || g_x4_mpn != g_x4_last_mpn;
    for (int i = 0; i < X4_N; i++) { d[i] = g_x4[i] - g_x4_last[i]; g_x4_last[i] = g_x4[i]; any |= d[i] != 0; }
    const double s = g_x4_t0 ? (now - g_x4_t0) / 1e9 : 2.0;
    g_x4_t0 = now; g_x4_last_n = g_x4_n; g_x4_last_mpn = g_x4_mpn;
    if (!any) return;
    snprintf(out, n, "NVMTL: MEMCENSUS %s x420: host imports live %llu (%llu MB, peak %llu MB), multi-plane %llu (%llu MB, peak %llu MB); "
             "%.1f s: new %llu (multi-plane %llu), freed %llu (multi-plane %llu), failed %llu, multi-plane refused %llu (import %llu), "
             "fallback images %llu (%llu MB), fallback copy-in %llu (%llu KB, %.0f us CPU), VRAM full %llu, image create FAILED %llu "
             "(multi-plane %llu), trimmed %llu (%llu MB, %.0f us); keep multi-plane %llu entries, %llu imports, %llu MB (idle %llu MB)",
             getprogname(), g_x4_n, g_x4_b >> 20, g_x4_peak >> 20, g_x4_mpn, g_x4_mpb >> 20, g_x4_mppeak >> 20,
             s, d[X4_NEW], d[X4_MPNEW], d[X4_GONE], d[X4_MPGONE], d[X4_FAIL], d[X4_REFUSED], d[X4_REFUSED_IMPORT],
             d[X4_FB], d[X4_FB_KB] >> 10, d[X4_CIN], d[X4_CIN_KB], d[X4_CIN_NS] / 1e3, d[X4_VRAMFULL], d[X4_IMGFAIL],
             d[X4_MPIMGFAIL], d[X4_TRIM], d[X4_TRIM_KB] >> 10, d[X4_TRIM_NS] / 1e3, g_x4_k_ent, g_x4_k_imp, g_x4_k_b >> 20, g_x4_k_idle_b >> 20);
}
static void nvmtl_x420_emit(const char *line) { if (line[0]) syslog(LOG_NOTICE, "%s", line); }
static void nvmtl_x420_add2(int k, uint64_t a, int k2, uint64_t b)
{
    char line[900];
    pthread_mutex_lock(&g_x4_lock);
    g_x4[k] += a; if (k2 >= 0) g_x4[k2] += b;
    nvmtl_x420_census_locked(line, sizeof line);
    pthread_mutex_unlock(&g_x4_lock);
    nvmtl_x420_emit(line);
}
static void nvmtl_x420_count(int k) { nvmtl_x420_add2(k, 1, -1, 0); }

/* nvmtl_vk_buffer_import_host: a host import made / one refused at `stage` with VkResult vr */
static void nvmtl_x420_import_ok(void *mem, size_t size)
{
    char line[900];
    pthread_mutex_lock(&g_x4_lock);
    if (!g_x4_live) g_x4_live = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    if (mem && !CFDictionaryContainsKey(g_x4_live, mem)) {
        CFDictionarySetValue(g_x4_live, mem, (const void *)(uintptr_t)((uint64_t)size << 1));
        g_x4_n++; g_x4_b += size; if (g_x4_b > g_x4_peak) g_x4_peak = g_x4_b; g_x4[X4_NEW]++;
    }
    nvmtl_x420_census_locked(line, sizeof line);
    pthread_mutex_unlock(&g_x4_lock);
    nvmtl_x420_emit(line);
}
static const char *nvmtl_x420_vkr_name(int vr)
{
    return vr == -1000072003 ? "INVALID_EXTERNAL_HANDLE: the kext refused BO_IMPORT_HOST (doBoImportHost; the IOReturn is not passed up)"
         : vr == -2 ? "OUT_OF_DEVICE_MEMORY: the GPU VA range (RADV va_range_alloc)"
         : vr == -13 ? "UNKNOWN: the GPU VA map (GemVa)"
         : vr == -1 ? "OUT_OF_HOST_MEMORY" : "other";
}
/* one line for the first failure of each kind (stage, VkResult, size < 1 MB / < 16 MB / larger), at most 8 kinds: small
   no-copy buffer imports can fail long before a storm and must not hide its first failure */
static void nvmtl_x420_import_failed(int stage, int vr, size_t size)
{
    static const char *const st[] = { "pointer properties", "vkAllocateMemory" };
    char line[900], first[700]; first[0] = 0;
    const int cls = size < (1u << 20) ? 0 : size < (16u << 20) ? 1 : 2;
    pthread_mutex_lock(&g_x4_lock);
    g_x4[X4_FAIL]++;
    int seen = 0;
    for (int i = 0; i < g_x4_kinds; i++) seen |= g_x4_kind[i].stage == stage && g_x4_kind[i].vr == vr && g_x4_kind[i].cls == cls;
    if (!seen && g_x4_kinds < 8) {
        g_x4_kind[g_x4_kinds].stage = stage; g_x4_kind[g_x4_kinds].vr = vr; g_x4_kind[g_x4_kinds].cls = cls; g_x4_kinds++;
        snprintf(first, sizeof first, "NVMTL: X420 first host-import failure of its kind (%d/8) in %s: %s -> VkResult %d (%s), %zu bytes; "
                 "live host imports %llu (%llu MB) + this %zu MB = %llu MB of the kext's %llu MB per-client cap; multi-plane %llu (%llu MB); "
                 "failures so far %llu", g_x4_kinds, getprogname(), st[stage & 1], vr, nvmtl_x420_vkr_name(vr), size, g_x4_n, g_x4_b >> 20,
                 size >> 20, (g_x4_b + size) >> 20, NVMTL_X420_KEXT_CAP_MB, g_x4_mpn, g_x4_mpb >> 20, g_x4[X4_FAIL]);
    }
    nvmtl_x420_census_locked(line, sizeof line);
    pthread_mutex_unlock(&g_x4_lock);
    if (first[0]) syslog(LOG_NOTICE, "%s", first);
    nvmtl_x420_emit(line);
}
/* nvmtl_vk_buffer_destroy: an imported buffer goes (not ours if it is not in the live set) */
static void nvmtl_x420_import_gone(void *mem)
{
    char line[900]; line[0] = 0;
    pthread_mutex_lock(&g_x4_lock);
    const void *v = NULL;
    if (g_x4_live && mem && CFDictionaryGetValueIfPresent(g_x4_live, mem, &v)) {
        const uint64_t e = (uint64_t)(uintptr_t)v, size = e >> 1;
        CFDictionaryRemoveValue(g_x4_live, mem);
        g_x4_n--; g_x4_b -= size; g_x4[X4_GONE]++;
        if (e & 1) { g_x4_mpn--; g_x4_mpb -= size; g_x4[X4_MPGONE]++; }
        nvmtl_x420_census_locked(line, sizeof line);
    }
    pthread_mutex_unlock(&g_x4_lock);
    nvmtl_x420_emit(line);
}
/* the import is the allocation of a multi-plane surface (nvmtl_planehost_get, or the unshared plane import) */
static void nvmtl_x420_mark_mp(void *mem)
{
    pthread_mutex_lock(&g_x4_lock);
    const void *v = NULL;
    if (g_x4_live && mem && CFDictionaryGetValueIfPresent(g_x4_live, mem, &v) && !((uintptr_t)v & 1)) {
        const uint64_t size = (uint64_t)(uintptr_t)v >> 1;
        CFDictionarySetValue(g_x4_live, mem, (const void *)((uintptr_t)v | 1));
        g_x4_mpn++; g_x4_mpb += size; if (g_x4_mpb > g_x4_mppeak) g_x4_mppeak = g_x4_mpb; g_x4[X4_MPNEW]++;
    }
    pthread_mutex_unlock(&g_x4_lock);
}
static void nvmtl_x420_fallback(uint64_t bytes) { nvmtl_x420_add2(X4_FB, 1, X4_FB_KB, bytes >> 10); }
/* -[NVMTLTexture nvmtlSurfaceIn]: a CPU copy-in; counted when the surface has more than one plane */
static void nvmtl_x420_copy_in(IOSurfaceRef surf, uint64_t bytes, uint64_t ns)
{
    if (!surf || IOSurfaceGetPlaneCount(surf) <= 1) return;
    char line[900];
    pthread_mutex_lock(&g_x4_lock);
    g_x4[X4_CIN]++; g_x4[X4_CIN_KB] += bytes >> 10; g_x4[X4_CIN_NS] += ns;
    nvmtl_x420_census_locked(line, sizeof line);
    pthread_mutex_unlock(&g_x4_lock);
    nvmtl_x420_emit(line);
}
static void nvmtl_x420_imgfail(IOSurfaceRef surf) { nvmtl_x420_add2(X4_IMGFAIL, 1, X4_MPIMGFAIL, surf && IOSurfaceGetPlaneCount(surf) > 1); }

/* ---- the multi-plane host-backed keep entries (all of this runs under @synchronized(device)) ---- */
@interface NVMTLX420Ent : NSObject { @public uint64_t stamp, bytes; uintptr_t imp; } @end
@implementation NVMTLX420Ent @end
static NSMutableDictionary *g_x4_ents;                     /* keep key (NSData) -> NVMTLX420Ent */
static __unsafe_unretained NSMutableDictionary *g_x4_keep;   /* the device's keep table (lives as long as the device) */
static int nvmtl_x420_mp_host(NVMTLTexture *t)
{
    return t && t->_hostOn && !t->_planeHostOn && t->_surf && IOSurfaceGetPlaneCount(t->_surf) > 1;
}
typedef struct { uint64_t ent, imp, bytes, idle_bytes, plan_imp, plan_bytes, plan_idle; } nvmtl_x420_stat;
/* census of the entries and the trim plan: the keys of the oldest surfaces (newest wrap of any of their keys) to drop
   so the set falls to `target` bytes; only surfaces idle >= idle_ns unless waive; never the key `keep_key` */
static NSArray *nvmtl_x420_plan(NSMutableDictionary *keep, uint64_t now, uint64_t cap, uint64_t target, uint64_t idle_ns,
                                int waive, NSData *keep_key, nvmtl_x420_stat *st)
{
    memset(st, 0, sizeof *st);
    NSMutableArray *stale = [NSMutableArray new];
    NSMutableDictionary *groups = [NSMutableDictionary new];   /* imp -> @[newest stamp, bytes, keys] */
    for (NSData *k in g_x4_ents) {
        if (!keep || !CFDictionaryContainsKey((__bridge CFDictionaryRef)keep, (__bridge const void *)k)) { [stale addObject:k]; continue; }
        NVMTLX420Ent *e = g_x4_ents[k];
        NSNumber *g = @(e->imp); NSMutableArray *gr = groups[g];
        if (!gr) { gr = [NSMutableArray arrayWithObjects:@(e->stamp), @(e->bytes), [NSMutableArray new], nil]; groups[g] = gr; }
        if (e->stamp > [gr[0] unsignedLongLongValue]) gr[0] = @(e->stamp);
        if (e->bytes > [gr[1] unsignedLongLongValue]) gr[1] = @(e->bytes);
        [gr[2] addObject:k];
        st->ent++;
    }
    [g_x4_ents removeObjectsForKeys:stale];
    NSMutableArray *cand = [NSMutableArray new];
    for (NSNumber *g in groups) {
        NSMutableArray *gr = groups[g]; const uint64_t t = [gr[0] unsignedLongLongValue], b = [gr[1] unsignedLongLongValue];
        st->imp++; st->bytes += b;
        const int idle = now >= t && now - t >= idle_ns;
        if (idle) st->idle_bytes += b;
        [gr addObject:@(idle)];
        if ((idle || waive) && !(keep_key && [gr[2] containsObject:keep_key])) [cand addObject:gr];
    }
    if (st->bytes <= cap && !waive) return @[];
    [cand sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) { return [a[0] compare:b[0]]; }];
    NSMutableArray *drop = [NSMutableArray new]; uint64_t left = st->bytes;
    for (NSArray *gr in cand) {
        if (left <= target) break;
        const uint64_t b = [gr[1] unsignedLongLongValue];
        [drop addObjectsFromArray:gr[2]]; left -= b; st->plan_imp++; st->plan_bytes += b; if ([gr[3] boolValue]) st->plan_idle += b;
    }
    return drop;
}
static void nvmtl_x420_keep_stat_store(const nvmtl_x420_stat *st)
{
    pthread_mutex_lock(&g_x4_lock);
    g_x4_k_ent = st->ent; g_x4_k_imp = st->imp; g_x4_k_b = st->bytes; g_x4_k_idle_b = st->idle_bytes;
    pthread_mutex_unlock(&g_x4_lock);
}
static int nvmtl_x420_waive(void) { pthread_mutex_lock(&g_x4_lock); const int w = g_x4_b > (NVMTL_X420_WAIVE_MB << 20); pthread_mutex_unlock(&g_x4_lock); return w; }
/* nvmtl_surface_keep, after keep[key] = t: record a multi-plane host-backed entry, keep the census current, trim */
static void nvmtl_x420_keep_after(NSMutableDictionary *keep, NSData *key, NVMTLTexture *t)
{
    g_x4_keep = keep;
    const uint64_t now = NVMTL_X420_NOW();
    const int mp = nvmtl_x420_mp_host(t);
    if (mp) {
        if (!g_x4_ents) g_x4_ents = [NSMutableDictionary new];
        NVMTLX420Ent *e = [NVMTLX420Ent new];
        e->stamp = now; e->bytes = t->_hostBuf.size;
        e->imp = t->_planeHostRef ? (uintptr_t)(__bridge void *)t->_planeHostRef : (uintptr_t)(__bridge void *)t;
        g_x4_ents[key] = e;
    }
    static uint64_t last_census;
    if (!g_x4_ents.count || (!mp && now - last_census < 2000000000ull)) return;
    last_census = now;
    const uint64_t cap = nvmtl_x420_cap();
    nvmtl_x420_stat st;
    const int waive = nvmtl_x420_waive();
    NSArray *drop = nvmtl_x420_plan(keep, now, cap, cap / 4 * 3, nvmtl_x420_idle_ns(), waive, key, &st);
    if (drop.count && nvmtl_x420_trim_on()) {
        @autoreleasepool {
            [keep removeObjectsForKeys:drop];
            [g_x4_ents removeObjectsForKeys:drop];
        }
        st.ent -= drop.count; st.imp -= st.plan_imp; st.bytes -= st.plan_bytes; st.idle_bytes -= st.plan_idle;
        nvmtl_x420_keep_stat_store(&st);
        char line[900];
        pthread_mutex_lock(&g_x4_lock);
        g_x4[X4_TRIM] += drop.count; g_x4[X4_TRIM_KB] += st.plan_bytes >> 10; g_x4[X4_TRIM_NS] += NVMTL_X420_NOW() - now;
        nvmtl_x420_census_locked(line, sizeof line);
        pthread_mutex_unlock(&g_x4_lock);
        nvmtl_x420_emit(line);
        static unsigned said; if (said++ < 8)
            nvlog("x420 trim: %lu keep entries of %llu idle multi-plane surface(s) dropped (%llu MB; set was %llu MB, cap %llu MB%s)",
                  (unsigned long)drop.count, st.plan_imp, st.plan_bytes >> 20, (st.bytes + st.plan_bytes) >> 20, cap >> 20,
                  waive ? ", idle rule waived: all host imports past 3 GiB" : "");
        return;
    }
    nvmtl_x420_keep_stat_store(&st);
}
/* nvmtlNewSurfaceTexture, cache hit: the surface was wrapped again */
static void nvmtl_x420_touch(NSData *key)
{
    NVMTLX420Ent *e = key ? g_x4_ents[key] : nil;
    if (e) e->stamp = NVMTL_X420_NOW();
}
/* nvmtl_surface_image_create, multi-plane branch: the plane could not be a host image (step: 0 precondition, 1 import,
   2 alias); the first one logs the keep census and what the trim would release */
static void nvmtl_x420_mp_refused(IOSurfaceRef surf, uint32_t w, uint32_t h, int step)
{
    char line[900], first[900]; first[0] = 0;
    if (!g_x4_first_refusal) {
        g_x4_first_refusal = 1;
        nvmtl_x420_stat st; const uint64_t cap = nvmtl_x420_cap();
        const int waive = nvmtl_x420_waive();
        nvmtl_x420_plan(g_x4_keep, NVMTL_X420_NOW(), cap, cap / 4 * 3, nvmtl_x420_idle_ns(), waive, nil, &st);
        nvmtl_x420_keep_stat_store(&st);
        pthread_mutex_lock(&g_x4_lock);
        snprintf(first, sizeof first, "NVMTL: X420 first multi-plane refusal in %s: surface %u %ux%u format %08x alloc %zu, step %s; live host "
                 "imports %llu (%llu MB), multi-plane %llu (%llu MB); keep multi-plane %llu entries, %llu imports, %llu MB, idle >= %llu ms "
                 "%llu MB; the trim (%s) would release %llu imports (%llu MB) [cap %llu MB%s]",
                 getprogname(), surf ? (unsigned)IOSurfaceGetID(surf) : 0u, w, h, surf ? (unsigned)IOSurfaceGetPixelFormat(surf) : 0u,
                 surf ? (size_t)IOSurfaceGetAllocSize(surf) : (size_t)0, step == 0 ? "precondition" : step == 1 ? "import" : "alias",
                 g_x4_n, g_x4_b >> 20, g_x4_mpn, g_x4_mpb >> 20, st.ent, st.imp, st.bytes >> 20, nvmtl_x420_idle_ns() / 1000000ull,
                 st.idle_bytes >> 20, nvmtl_x420_trim_on() ? "on" : "off", st.plan_imp, st.plan_bytes >> 20, cap >> 20,
                 waive ? ", idle rule waived" : "");
        pthread_mutex_unlock(&g_x4_lock);
    }
    pthread_mutex_lock(&g_x4_lock);
    g_x4[X4_REFUSED]++; if (step == 1) g_x4[X4_REFUSED_IMPORT]++;
    nvmtl_x420_census_locked(line, sizeof line);
    pthread_mutex_unlock(&g_x4_lock);
    if (first[0]) syslog(LOG_NOTICE, "%s", first);
    nvmtl_x420_emit(line);
}
