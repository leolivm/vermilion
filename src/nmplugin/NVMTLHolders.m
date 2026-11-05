// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLDevice.m after NVMTLX420.m (one translation unit with the plugin).
   Plugin holders1: the x420 holder data test. Log only.

   Question: after Safari's page is hidden and shown while the 4K movie plays, who holds each decoder output surface?
   IOSurface keeps a use count per process; the surface is "in use" (IOSurfaceIsInUse, what CVPixelBufferPool checks
   before it recycles a buffer) while any process has a count above 0, and also while a mach port right for it is in
   flight. IOSurfaceLookup(id) returns the surface only in a process that already holds a reference to it (host-tested:
   NULL without a reference, OK with one, NULL again after the last release).
   So every 2 s (ticks aligned to the wall clock, tick = unix seconds / 2, the same tick phxdec's census uses) this
   process looks up every IOSurface id in 1..hi and, for each two-plane video surface it holds (x420 / xf20 / 420v /
   420f, width >= 1280), logs its own use count, the global in-use bit, how many entries of our keep table
   (nvmtl_surface_keep) hold it, how many retains of that IOSurfaceRef object are not our keep backings (the number the
   keep prune tests: 0 = only we hold it), and how long ago this process last wrapped it.
   The lookup takes and drops one reference, never a use count.

   Gate: NVMTL_MEMCENSUS on (as installed) and NVMTL_HOLDERS not 0. It starts 3 s after the plugin loads in WindowServer,
   com.apple.WebKit.GPU, com.apple.WebKit.WebContent and Safari (the conf knobs are read by a later constructor), and in
   any other process at its first wrap of a video surface. One "NVMTL: HOLDERS on" line when it starts. */
#ifndef NVMTL_HOLD_NOW
#define NVMTL_HOLD_NOW() clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
#endif
#ifndef NVMTL_HOLD_WALL
#define NVMTL_HOLD_WALL() clock_gettime_nsec_np(CLOCK_REALTIME)
#endif
#define NVMTL_HOLD_IDMAX 65536u
#define NVMTL_HOLD_LINE 820
static int nvmtl_memcensus_on(void);   /* nvmtl_imgmem.c */

static pthread_mutex_t g_hold_mu = PTHREAD_MUTEX_INITIALIZER;
static __unsafe_unretained id g_hold_dev;   /* the device whose keep table we read (lives as long as the process) */
static uint32_t g_hold_maxid;               /* the highest IOSurface id seen (wraps, scan hits) */
static uint64_t g_hold_wrap[1024];          /* last wrap of a video surface: (id << 40) | (uptime ms & 2^40-1), by id % 1024 */
static int g_hold_started;

static int nvmtl_hold_video(IOSurfaceRef s)
{
    if (!s || IOSurfaceGetPlaneCount(s) != 2 || IOSurfaceGetWidth(s) < 1280) return 0;
    const OSType f = IOSurfaceGetPixelFormat(s);
    return f == 0x78343230u || f == 0x78663230u || f == 0x34323076u || f == 0x34323066u;   /* x420 xf20 420v 420f */
}
static int nvmtl_hold_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_HOLDERS"); on = nvmtl_memcensus_on() && !(e && e[0] == '0'); }
    return on;
}

typedef struct { uint32_t id; int32_t uc; uint8_t inuse; uint16_t keep; int32_t other; int64_t wrap_ms; } nvmtl_hold_item;

/* one census tick: scan, keep table, log. Runs on the census queue. */
static void nvmtl_hold_tick(void)
{
    const uint64_t t0 = NVMTL_HOLD_NOW();
    pthread_mutex_lock(&g_hold_mu);
    uint32_t hi = g_hold_maxid + 1024;
    pthread_mutex_unlock(&g_hold_mu);
    if (hi < 2048) hi = 2048;
    if (hi > NVMTL_HOLD_IDMAX) hi = NVMTL_HOLD_IDMAX;
    size_t cap = 256, n = 0;
    nvmtl_hold_item *it = malloc(cap * sizeof *it);
    if (!it) return;
    uint32_t maxhit = 0;
    for (uint32_t id = 1; id <= hi; id++) {
        IOSurfaceRef s = IOSurfaceLookup(id);
        if (!s) continue;
        if (id > maxhit) maxhit = id;
        if (nvmtl_hold_video(s)) {
            if (n == cap) { nvmtl_hold_item *g = realloc(it, (cap *= 2) * sizeof *it); if (!g) { CFRelease(s); break; } it = g; }
            it[n] = (nvmtl_hold_item){ id, IOSurfaceGetUseCount(s), IOSurfaceIsInUse(s) != 0, 0, -1, -1 };
            n++;
        }
        CFRelease(s);
    }
    const uint64_t scan_us = (NVMTL_HOLD_NOW() - t0) / 1000;
    pthread_mutex_lock(&g_hold_mu);
    if (maxhit > g_hold_maxid) g_hold_maxid = maxhit;
    const uint64_t now_ms = NVMTL_HOLD_NOW() / 1000000ull;
    for (size_t i = 0; i < n; i++) {
        const uint64_t w = g_hold_wrap[it[i].id % 1024];
        if (w && (w >> 40) == it[i].id) it[i].wrap_ms = (int64_t)((now_ms & ((1ull << 40) - 1)) - (w & ((1ull << 40) - 1)));
    }
    id dev = g_hold_dev;
    pthread_mutex_unlock(&g_hold_mu);
    /* our keep table, under the device lock that guards it (nvmtl_surface_keep runs under @synchronized(device)) */
    unsigned kept_total = 0;
    if (dev && n) {
        @synchronized(dev) {
            NSMutableDictionary *keep = g_x4_keep;
            if (keep.count) {
                CFMutableBagRef held = CFBagCreateMutable(NULL, 0, NULL);   /* no callbacks: counting must not retain */
                for (NVMTLTexture *b in keep.allValues) if (b->_surf) CFBagAddValue(held, b->_surf);
                [keep enumerateKeysAndObjectsUsingBlock:^(NSData *k, NVMTLTexture *b, BOOL *stop) {
                    (void)stop;
                    if (k.length < sizeof(uint64_t) || !b->_surf) return;
                    const uint32_t sid = (uint32_t)((const uint64_t *)k.bytes)[0];
                    for (size_t i = 0; i < n; i++) if (it[i].id == sid) {
                        it[i].keep++;
                        const int32_t o = (int32_t)CFGetRetainCount(b->_surf) - (int32_t)CFBagGetCountOfValue(held, b->_surf);
                        if (o > it[i].other) it[i].other = o;
                        break;
                    }
                }];
                CFRelease(held);
            }
        }
    }
    unsigned inuse = 0, ucpos = 0, keeps = 0;
    for (size_t i = 0; i < n; i++) { inuse += it[i].inuse; ucpos += it[i].uc > 0; keeps += it[i].keep > 0; kept_total += it[i].keep; }
    const uint64_t tick = NVMTL_HOLD_WALL() / 2000000000ull;
    static unsigned quiet;
    if (!n && quiet++) { free(it); return; }   /* nothing held: one line, then silence until something is */
    if (n) quiet = 0;
    /* items: id=<own use count><I in use | .>/<keep entries>/<other retains, - = no keep entry>/<ms since our last wrap, - = never> */
    char head[200], line[NVMTL_HOLD_LINE + 64], item[64];
    snprintf(head, sizeof head, "NVMTL: HOLDERS %s tick %llu: scan 1..%u %llu us, video %zu (in use %u, own use>0 %u, keep %u entries %u) |",
             getprogname(), (unsigned long long)tick, hi, (unsigned long long)scan_us, n, inuse, ucpos, keeps, kept_total);
    size_t i = 0; unsigned part = 0;
    do {
        int l = snprintf(line, sizeof line, "%s p%u", head, part++);
        for (; i < n; i++) {
            char ow[16], wr[24];
            if (it[i].keep) snprintf(ow, sizeof ow, "%d", it[i].other); else snprintf(ow, sizeof ow, "-");
            if (it[i].wrap_ms >= 0) snprintf(wr, sizeof wr, "%lld", (long long)it[i].wrap_ms); else snprintf(wr, sizeof wr, "-");
            const int m = snprintf(item, sizeof item, " %u=%d%c/%u/%s/%s", it[i].id, it[i].uc, it[i].inuse ? 'I' : '.', it[i].keep, ow, wr);
            if (l + m > NVMTL_HOLD_LINE) break;
            memcpy(line + l, item, (size_t)m + 1); l += m;
        }
        syslog(LOG_NOTICE, "%s", line);
    } while (i < n);
    free(it);
}

static void nvmtl_hold_start(void)
{
    pthread_mutex_lock(&g_hold_mu);
    const int first = !g_hold_started && nvmtl_hold_on();
    if (first) g_hold_started = 1;
    pthread_mutex_unlock(&g_hold_mu);
    if (!first) return;
    dispatch_queue_t q = dispatch_queue_create("nvmtl.holders", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    if (!t) return;
    /* aligned to the 2 s wall-clock ticks (+0.5 s), so the phxdec census of the same tick is comparable */
    const uint64_t wall = NVMTL_HOLD_WALL(), next = (wall / 2000000000ull + 1) * 2000000000ull + 500000000ull;
    const struct timespec ts = { (time_t)(next / 1000000000ull), (long)(next % 1000000000ull) };
    dispatch_source_set_timer(t, dispatch_walltime(&ts, 0), 2000000000ull, 20000000ull);
    dispatch_source_set_event_handler(t, ^{ @autoreleasepool { nvmtl_hold_tick(); } });
    dispatch_resume(t);
    static dispatch_source_t keep_alive; keep_alive = t;   /* runs for the life of the process */
    syslog(LOG_NOTICE, "NVMTL: HOLDERS on in %s (census knob on, NVMTL_HOLDERS not 0): video surfaces held by this process every 2 s",
           getprogname());
}

/* -newTextureWithDescriptor:iosurface:plane:, under @synchronized(device): remember the device, the id range and the
   last wrap of a video surface; the first one starts the census in a process not on the list */
static void nvmtl_holders_note(id dev, IOSurfaceRef surf)
{
    if (!surf || !nvmtl_hold_on()) return;
    const uint32_t sid = IOSurfaceGetID(surf);
    const int video = nvmtl_hold_video(surf);
    pthread_mutex_lock(&g_hold_mu);
    if (!g_hold_dev) g_hold_dev = dev;
    if (sid > g_hold_maxid && sid <= NVMTL_HOLD_IDMAX) g_hold_maxid = sid;
    if (video) g_hold_wrap[sid % 1024] = ((uint64_t)sid << 40) | ((NVMTL_HOLD_NOW() / 1000000ull) & ((1ull << 40) - 1));
    const int start = video && !g_hold_started;
    pthread_mutex_unlock(&g_hold_mu);
    if (start) nvmtl_hold_start();
}

__attribute__((constructor)) static void nvmtl_holders_at_load(void)
{
    const char *me = getprogname();
    if (!me || (strcmp(me, "WindowServer") && strcmp(me, "com.apple.WebKit.GPU") && strcmp(me, "com.apple.WebKit.WebContent") &&
                strcmp(me, "Safari")))
        return;
    /* the conf knobs (NVMTL_MEMCENSUS) are set by a constructor that runs after this one */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ nvmtl_hold_start(); });
}
