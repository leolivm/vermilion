// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by nvmtl_vk.c at this exact place (one translation unit with the plugin;
   definitions keep their order): device-local image memory recycling (NVMTL_IMGMEM_RECYCLE, NVMTL_IMGMEM_NOEVICT) and the
   MEMCENSUS allocation census (NVMTL_MEMCENSUS, log only, measurement build only). */
// Drag lag (NVMTL_IMGMEM_RECYCLE=1): device-local image memory freed by a destroyed image is kept (<= 64 blocks, 256 MB)
// and handed to the next image of the same size and memory type. Core Animation makes and drops the same small offscreen
// textures every frame; each was a kernel BO create (radv_alloc_memory, ~0.5 ms of WindowServer's frame, 10-27).
// WP5 10-28: 64 entries filled with small blocks (the Wallpaper pane misses were ~50-120 blocks of <= 1 MB per 2 s with
// ~60 parked holding 2-6 MB), and a pair of 16 + 32 MB blocks hit the byte cap: 512 entries, 512 MB
#define NVMTL_IMGMEM_N 512
#define NVMTL_IMGMEM_CAP (512ull << 20)
static pthread_mutex_t g_imgmem_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { VkDeviceMemory mem; VkDeviceSize size; int mt; } g_imgmem[NVMTL_IMGMEM_N];
static unsigned g_imgmem_n; static uint64_t g_imgmem_bytes, g_imgmem_hit, g_imgmem_miss, g_imgmem_kept, g_imgmem_evicted;
// log only (MEMCENSUS): recycler misses by size (<=1, <=4, <=16, <=32, <=64, >64 MB) and blocks it would not keep (too big)
static uint64_t g_imgmem_miss_b[6], g_imgmem_miss_last[6], g_imgmem_big, g_imgmem_big_last;
static int nvmtl_imgmem_bucket(VkDeviceSize z) { const VkDeviceSize m = 1ull << 20;
    return z <= m ? 0 : z <= 4 * m ? 1 : z <= 16 * m ? 2 : z <= 32 * m ? 3 : z <= 64 * m ? 4 : 5; }
static int nvmtl_imgmem_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_IMGMEM_RECYCLE"); on = e && e[0] == '1';
        nvlog("vk: image memory recycling %s (NVMTL_IMGMEM_RECYCLE=%s)", on ? "ON" : "off", e ? e : "unset"); }
    return on;
}
// the blocks of the recyclable type that live images hold (allocated or reused through nvmtl_vk_alloc_image_mem)
#define NVMTL_IMGMEM_LIVE 8192
static VkDeviceMemory g_imgmem_live[NVMTL_IMGMEM_LIVE];
static void nvmtl_imgmem_mark(VkDeviceMemory m)
{
    if (!m) return;
    pthread_mutex_lock(&g_imgmem_lock);
    uint32_t h = (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (NVMTL_IMGMEM_LIVE - 1);
    for (uint32_t k = 0; k < NVMTL_IMGMEM_LIVE; k++, h = (h + 1) & (NVMTL_IMGMEM_LIVE - 1))
        if (!g_imgmem_live[h] || g_imgmem_live[h] == m) { g_imgmem_live[h] = m; break; }
    pthread_mutex_unlock(&g_imgmem_lock);
}
// removes m from the live set (backward-shift delete); 1 if it was there
static int nvmtl_imgmem_unmark_locked(VkDeviceMemory m)
{
    uint32_t h = (uint32_t)(((uintptr_t)m >> 4) * 2654435761u) & (NVMTL_IMGMEM_LIVE - 1), k;
    for (k = 0; k < NVMTL_IMGMEM_LIVE && g_imgmem_live[h] != m; k++, h = (h + 1) & (NVMTL_IMGMEM_LIVE - 1))
        if (!g_imgmem_live[h]) return 0;
    if (k == NVMTL_IMGMEM_LIVE) return 0;
    uint32_t i = h, j = h;
    g_imgmem_live[i] = VK_NULL_HANDLE;
    for (;;) {
        j = (j + 1) & (NVMTL_IMGMEM_LIVE - 1);
        if (!g_imgmem_live[j]) break;
        uint32_t want = (uint32_t)(((uintptr_t)g_imgmem_live[j] >> 4) * 2654435761u) & (NVMTL_IMGMEM_LIVE - 1);
        if ((j > i && (want <= i || want > j)) || (j < i && (want <= i && want > j))) { g_imgmem_live[i] = g_imgmem_live[j]; g_imgmem_live[j] = VK_NULL_HANDLE; i = j; }
    }
    return 1;
}
static int nvmtl_imgmem_take(VkDeviceSize size, int mt, VkDeviceMemory *mem)
{
    int got = 0;
    pthread_mutex_lock(&g_imgmem_lock);
    for (unsigned i = g_imgmem_n; i-- > 0;)
        if (g_imgmem[i].size == size && g_imgmem[i].mt == mt) {
            *mem = g_imgmem[i].mem; g_imgmem_bytes -= size;
            memmove(&g_imgmem[i], &g_imgmem[i + 1], (g_imgmem_n - i - 1) * sizeof g_imgmem[0]); g_imgmem_n--; got = 1; break; }
    if (got) g_imgmem_hit++; else { g_imgmem_miss++; g_imgmem_miss_b[nvmtl_imgmem_bucket(size)]++; }
    const uint64_t n = g_imgmem_hit + g_imgmem_miss;
    pthread_mutex_unlock(&g_imgmem_lock);
    if ((n & (n - 1)) == 0 && n >= 64) nvlog("vk: image memory recycling: %llu reused, %llu new, %llu kept, %llu evicted, %u parked (%llu MB)",
        (unsigned long long)g_imgmem_hit, (unsigned long long)g_imgmem_miss, (unsigned long long)g_imgmem_kept, (unsigned long long)g_imgmem_evicted,
        g_imgmem_n, (unsigned long long)(g_imgmem_bytes >> 20));
    return got;
}
// Wallpaper pane lag, log only (NVMTL_MEMCENSUS=1): VkDeviceMemory allocations by kind, their CPU cost, and the IOSurface texture cache
// (NVMTLDevice.m), logged every 2 s while anything is counted. syslog directly: nvlog keeps only the first lines of a site.
// Plugin audit (10-30, measure before design): the IOSurface copies of textures that are not host imports, by
// direction: CPU copy-in (nvmtlSurfaceIn, upload of the surface's pages into the private image), CPU copy-out
// (nvmtlSurfaceOut, readback into the pages) and the GPU plane-in copy recorded in the command buffer (the plane-in copy),
// with bytes and, for the CPU ones, the CPU time spent (lock + copy + unlock). One more line per 2 s, only when non-zero.
// f18a (10-31): `pitch pre-rejects` = single-plane surfaces sent to a private image before the host import because their row
// pitch is not on the device's linear pitch alignment (nvmtl_pitch_align.c nvmtl_vk_surface_pitch_ok).
// nm458 (10-31): `late zero claims` = managed flushes (NVMTLObjects.m nvmtl_managed_flush, with uploads to make) that found
// GPU clears owed AFTER presubmit's first zero flush: NullMoth PR #458's window; without #458 those clears ran in the NEXT
// submission's prologue and could zero bytes uploaded now (an upper bound: the owed buffer need not be the uploaded one).
enum { MC_ALLOC, MC_ALLOC_HOST, MC_ALLOC_KB, MC_ALLOC_NS, MC_FREE, MC_FREE_NS, MC_SURF_HIT, MC_SURF_HOST, MC_SURF_PLANE,
       MC_SURF_PLANECOPY, MC_SURF_VRAM, MC_SURF_PRIVATE, MC_SURF_EVICT,
       MC_COPY_IN, MC_COPY_IN_KB, MC_COPY_IN_NS, MC_COPY_OUT, MC_COPY_OUT_KB, MC_COPY_OUT_NS, MC_COPY_PLANE, MC_COPY_PLANE_KB,
       MC_PITCH_PRE, MC_ZERO_LATE, MC_N };
static uint64_t g_mc[MC_N], g_mc_last[MC_N], g_mc_img_last[2], g_mc_t0;
static pthread_mutex_t g_mc_lock = PTHREAD_MUTEX_INITIALIZER;
static int nvmtl_memcensus_on(void)   /* measurement build only (NVMTL_DIAG, nvmtl_vk.h): constant 0 otherwise */
{ static int on = -1; if (!NVMTL_DIAG_BUILD) return 0; if (on < 0) { const char *e = getenv("NVMTL_MEMCENSUS"); on = e && *e == '1'; } return on; }
static void nvmtl_mc_add(int k, uint64_t n)
{
    if (!nvmtl_memcensus_on()) return;
    __atomic_fetch_add(&g_mc[k], n, __ATOMIC_RELAXED);
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - __atomic_load_n(&g_mc_t0, __ATOMIC_RELAXED) < 2000000000ull || pthread_mutex_trylock(&g_mc_lock)) return;
    if (now - g_mc_t0 >= 2000000000ull) {
        uint64_t d[MC_N];
        for (int i = 0; i < MC_N; i++) { const uint64_t v = __atomic_load_n(&g_mc[i], __ATOMIC_RELAXED); d[i] = v - g_mc_last[i]; g_mc_last[i] = v; }
        const uint64_t ih = g_imgmem_hit - g_mc_img_last[0], im = g_imgmem_miss - g_mc_img_last[1];
        g_mc_img_last[0] = g_imgmem_hit; g_mc_img_last[1] = g_imgmem_miss;
        const double s = g_mc_t0 ? (now - g_mc_t0) / 1e9 : 2.0;
        syslog(LOG_NOTICE, "NVMTL: MEMCENSUS %s %.1f s: alloc %llu (host import %llu) %llu MB %.0f us each, free %llu %.0f us each; "
               "image memory reused %llu new %llu; surface textures: cache hit %llu, new host %llu plane %llu planecopy %llu vram %llu private %llu, evicted %llu; "
               "pitch pre-rejects %llu, late zero claims %llu",
               getprogname(), s, d[MC_ALLOC], d[MC_ALLOC_HOST], d[MC_ALLOC_KB] >> 10, d[MC_ALLOC] ? d[MC_ALLOC_NS] / 1e3 / d[MC_ALLOC] : 0,
               d[MC_FREE], d[MC_FREE] ? d[MC_FREE_NS] / 1e3 / d[MC_FREE] : 0, ih, im, d[MC_SURF_HIT], d[MC_SURF_HOST], d[MC_SURF_PLANE],
               d[MC_SURF_PLANECOPY], d[MC_SURF_VRAM], d[MC_SURF_PRIVATE], d[MC_SURF_EVICT], d[MC_PITCH_PRE], d[MC_ZERO_LATE]);
        if (d[MC_COPY_IN] || d[MC_COPY_OUT] || d[MC_COPY_PLANE])
            syslog(LOG_NOTICE, "NVMTL: MEMCENSUS %s %.1f s: surface copies: in %llu (%llu KB, %.0f us CPU), out %llu (%llu KB, %.0f us CPU), "
                   "plane-in GPU %llu (%llu KB)", getprogname(), s, d[MC_COPY_IN], d[MC_COPY_IN_KB], d[MC_COPY_IN_NS] / 1e3,
                   d[MC_COPY_OUT], d[MC_COPY_OUT_KB], d[MC_COPY_OUT_NS] / 1e3, d[MC_COPY_PLANE], d[MC_COPY_PLANE_KB]);
        if (im) {
            uint64_t b[6]; for (int i = 0; i < 6; i++) { b[i] = g_imgmem_miss_b[i] - g_imgmem_miss_last[i]; g_imgmem_miss_last[i] = g_imgmem_miss_b[i]; }
            const uint64_t big = g_imgmem_big - g_imgmem_big_last; g_imgmem_big_last = g_imgmem_big;
            syslog(LOG_NOTICE, "NVMTL: MEMCENSUS %s image memory misses by size: <=1 MB %llu, <=4 %llu, <=16 %llu, <=32 %llu, <=64 %llu, >64 %llu; "
                   "freed too big to park %llu; %u parked (%llu MB)", getprogname(), b[0], b[1], b[2], b[3], b[4], b[5], big, g_imgmem_n,
                   (unsigned long long)(g_imgmem_bytes >> 20));
        }
        __atomic_store_n(&g_mc_t0, now, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_mc_lock);
}
static void nvmtl_mc_zero_late(void) { if (atomic_load(&gZeroAny)) nvmtl_mc_add(MC_ZERO_LATE, 1); }   /* gZeroAny: NVMTLObjects.m */
// kind: 0 cache hit, 1 host, 2 plane, 3 planecopy, 4 vram, 5 private, 6 evicted (NVMTLDevice.m nvmtlNewSurfaceTexture)
static void nvmtl_mc_surf(int kind) { if (kind >= 0 && kind <= 6) nvmtl_mc_add(MC_SURF_HIT + kind, 1); }
// kind: 0 CPU copy-in, 1 CPU copy-out, 2 GPU plane-in (NVMTLObjects.m nvmtlSurfaceIn / nvmtlSurfaceOutDirtied: /
// nvmtl_surface_in_for_cb); ns = CPU time of the copy (0 for the GPU one). The count goes last: it may print the line.
static void nvmtl_mc_copy(int kind, uint64_t bytes, uint64_t ns)
{
    if (!nvmtl_memcensus_on() || kind < 0 || kind > 2) return;
    const int base = kind == 0 ? MC_COPY_IN : kind == 1 ? MC_COPY_OUT : MC_COPY_PLANE;
    __atomic_fetch_add(&g_mc[base + 1], bytes >> 10, __ATOMIC_RELAXED);
    if (kind < 2) __atomic_fetch_add(&g_mc[base + 2], ns, __ATOMIC_RELAXED);
    nvmtl_mc_add(base, 1);
}
static VKAPI_ATTR VkResult VKAPI_CALL nvmtl_mc_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a, VkDeviceMemory *m)
{
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const VkResult r = g_mc_real_alloc(dev, info, a, m);
    int host = 0;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) if (p->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT) host = 1;
    __atomic_fetch_add(&g_mc[MC_ALLOC_NS], clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, __ATOMIC_RELAXED);
    if (r == VK_SUCCESS) { __atomic_fetch_add(&g_mc[MC_ALLOC_KB], info->allocationSize >> 10, __ATOMIC_RELAXED); if (host) __atomic_fetch_add(&g_mc[MC_ALLOC_HOST], 1, __ATOMIC_RELAXED); }
    nvmtl_mc_add(MC_ALLOC, 1);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL nvmtl_mc_FreeMemory(VkDevice dev, VkDeviceMemory m, const VkAllocationCallbacks *a)
{
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    g_mc_real_free(dev, m, a);
    __atomic_fetch_add(&g_mc[MC_FREE_NS], clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, __ATOMIC_RELAXED);
    if (m) nvmtl_mc_add(MC_FREE, 1);
}
static int nvmtl_imgmem_put(VkDeviceMemory mem, VkDeviceSize size)
{
    const int mt = memtype(0xffffffffu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!nvmtl_imgmem_on() || !mem) return 0;
    // WP6 B6d: the parked blocks are kept oldest first; a full pool frees its oldest blocks to make room. Without that,
    // 64 blocks of sizes nobody asked for again filled it for good and Core Animation's per-frame 1 MB offscreen images
    // went back to a kernel BO create + map + free each (WindowServer on the Wallpaper pane, 10-27: 0 reused, 60 new
    // per 2 s). NVMTL_IMGMEM_NOEVICT=1 restores the old behaviour.
    static int noevict = -1;
    if (noevict < 0) { const char *e = getenv("NVMTL_IMGMEM_NOEVICT"); noevict = e && e[0] == '1'; }
    int kept = 0;
    VkDeviceMemory drop[NVMTL_IMGMEM_N]; unsigned nd = 0;
    pthread_mutex_lock(&g_imgmem_lock);
    const int live = nvmtl_imgmem_unmark_locked(mem);
    if (live && size > (32ull << 20)) g_imgmem_big++;
    if (live && mt >= 0 && size <= (32ull << 20) && size <= NVMTL_IMGMEM_CAP) {
        if (!noevict)
            while (g_imgmem_n && (g_imgmem_n >= NVMTL_IMGMEM_N || g_imgmem_bytes + size > NVMTL_IMGMEM_CAP)) {
                drop[nd++] = g_imgmem[0].mem; g_imgmem_bytes -= g_imgmem[0].size;
                memmove(&g_imgmem[0], &g_imgmem[1], (g_imgmem_n - 1) * sizeof g_imgmem[0]); g_imgmem_n--; g_imgmem_evicted++; }
        if (g_imgmem_n < NVMTL_IMGMEM_N && g_imgmem_bytes + size <= NVMTL_IMGMEM_CAP) {
            g_imgmem[g_imgmem_n].mem = mem; g_imgmem[g_imgmem_n].size = size; g_imgmem[g_imgmem_n].mt = mt; g_imgmem_n++;
            g_imgmem_bytes += size; g_imgmem_kept++; kept = 1; }
    }
    pthread_mutex_unlock(&g_imgmem_lock);
    for (unsigned k = 0; k < nd; k++) pvkFreeMemory(g_dev, drop[k], NULL);
    return kept;
}
