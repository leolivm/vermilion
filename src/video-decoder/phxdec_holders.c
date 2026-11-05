// SPDX-License-Identifier: MIT
// phxdec_holders.c: included by phxdec_hevc.c after the PhxDec definition. Build holders1, log only: the x420 holder
// data test.
//
// Every output surface of a session goes back to VT's pool only when no process holds a use count on it any more
// (IOSurfaceIsInUse; a mach port right in flight counts too). After a page-visibility resume the pool either keeps
// minting surfaces (unbounded) or has all 32 'in use' for good (poolbound). This census says, every 2 s and per
// surface id the session ever fetched, what this process (VTDecoderXPCService) sees:
//   - alive: IOSurfaceLookup(id) still finds it here, i.e. this process holds a reference (VT's pool, our import cache,
//     our reorder queue, VT's transport); lookups find a surface only in a process that already references it (host-
//     tested);
//   - this process's own use count (IOSurfaceGetUseCount) and the global in-use bit;
//   - q: in our reorder queue, c: in our copier's import cache (32 retained surfaces);
//   - how often the pool handed it out and how long ago.
// Ticks are aligned to the wall clock (tick = unix seconds / 2) so the plugin's census of the same tick in
// WindowServer (NVMTLHolders.m) can be joined by id. The lookup takes and drops one reference, never a use count.
// Off with /Library/GPUBundles/nvmtl/phxdec-holders-off (read at StartSession). After 120 s without a fetch the census
// goes quiet (one line) until the next fetch.
#include <dispatch/dispatch.h>

#define PHX_HOLD_MAX 4096
#define PHX_HOLD_OFF "/Library/GPUBundles/nvmtl/phxdec-holders-off"
#define PHX_HOLD_LINE 800
#define PHX_HOLD_QUIET_MS 120000ull

typedef struct {
    uint32_t id, nfetch;
    uint16_t gen;   // times the id came back after the census saw it gone (the kernel reuses free ids)
    uint8_t gone;
    uint64_t last_ms;   // last fetch, uptime ms
} PhxHoldId;

struct PhxHolders {
    pthread_mutex_t mu;   // guards everything below; the decoder lock is only ever try-locked from the census queue
    dispatch_queue_t q;
    dispatch_source_t timer;
    dispatch_semaphore_t done;
    PhxDec *p;
    unsigned sess;
    int32_t w, h;
    OSType fmt;
    PhxHoldId ids[PHX_HOLD_MAX];
    unsigned nids, over, gone_total, gens;
    uint64_t fetches, last_fetch_ms;
    uint32_t qids[16], cids[64];
    unsigned nq, nc;
    uint64_t qc_ms;
    int quiet;
};

static uint64_t hold_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull; }

static void hold_log(const char *s) { os_log(OS_LOG_DEFAULT, "phxdec[%{public}s]: %{public}s", getprogname(), s); }

// our reorder queue and import cache now; called with the decoder lock held
static void phx_hold_snap(PhxDec *p, PhxHolders *h)
{
    uint32_t q[16], c[64];
    unsigned nq = 0, nc = 0;
    for (int i = 0; i < p->nq && nq < 16; i++) {
        IOSurfaceRef s = p->q[i].pb ? CVPixelBufferGetIOSurface(p->q[i].pb) : NULL;
        if (s)
            q[nq++] = IOSurfaceGetID(s);
    }
    if (p->eng && p->eng->copier)
        nc = phx_copier_cached_ids(p->eng->copier, c, 64);
    pthread_mutex_lock(&h->mu);
    memcpy(h->qids, q, nq * sizeof q[0]);
    h->nq = nq;
    memcpy(h->cids, c, nc * sizeof c[0]);
    h->nc = nc;
    h->qc_ms = hold_ms();
    pthread_mutex_unlock(&h->mu);
}

static int hold_has(const uint32_t *a, unsigned n, uint32_t id)
{
    for (unsigned i = 0; i < n; i++)
        if (a[i] == id)
            return 1;
    return 0;
}

static void phx_hold_tick(PhxHolders *h)
{
    if (pthread_mutex_trylock(&h->p->lock) == 0) {   // fresh queue / cache view when the decoder is between frames
        if (h->p->hold == h)
            phx_hold_snap(h->p, h);
        pthread_mutex_unlock(&h->p->lock);
    }
    pthread_mutex_lock(&h->mu);
    const uint64_t now = hold_ms();
    if (h->last_fetch_ms && now - h->last_fetch_ms > PHX_HOLD_QUIET_MS) {
        const int was = h->quiet;
        h->quiet = 1;
        pthread_mutex_unlock(&h->mu);
        if (!was) {
            char b[160];
            snprintf(b, sizeof b, "holders sess %u: no fetch for %llu s, census quiet until the next fetch", h->sess, PHX_HOLD_QUIET_MS / 1000);
            hold_log(b);
        }
        return;
    }
    const unsigned n = h->nids;
    PhxHoldId *ids = malloc((n ? n : 1) * sizeof *ids);
    if (!ids) {
        pthread_mutex_unlock(&h->mu);
        return;
    }
    memcpy(ids, h->ids, n * sizeof *ids);
    uint32_t q[16], c[64];
    const unsigned nq = h->nq, nc = h->nc;
    memcpy(q, h->qids, sizeof q);
    memcpy(c, h->cids, sizeof c);
    const uint64_t qc_age = h->qc_ms ? now - h->qc_ms : 0;
    const int32_t W = h->w, H = h->h;
    const OSType F = h->fmt;
    pthread_mutex_unlock(&h->mu);

    // per id: -1 gone, else (own use count << 1) | in use
    int32_t *st = malloc((n ? n : 1) * sizeof *st);
    if (!st) {
        free(ids);
        return;
    }
    unsigned alive = 0, inuse = 0, ucpos = 0;
    for (unsigned i = 0; i < n; i++) {
        st[i] = -1;
        if (ids[i].gone)
            continue;
        IOSurfaceRef s = IOSurfaceLookup(ids[i].id);
        if (!s)
            continue;
        // the id may now name another surface of this process: count it only if it still looks like our output
        if ((int32_t)IOSurfaceGetWidth(s) == W && (int32_t)IOSurfaceGetHeight(s) == H && IOSurfaceGetPixelFormat(s) == F) {
            const int32_t uc = IOSurfaceGetUseCount(s), iu = IOSurfaceIsInUse(s) != 0;
            st[i] = (uc < 0 ? 0 : uc) << 1 | iu;
            alive++;
            inuse += iu;
            ucpos += uc > 0;
        }
        CFRelease(s);
    }
    // the ids this tick found gone for the first time
    char gl[PHX_HOLD_LINE + 64];
    int gn = 0, gcount = 0;
    pthread_mutex_lock(&h->mu);
    for (unsigned i = 0; i < n; i++)
        if (st[i] < 0 && !ids[i].gone) {
            // the decoder may have fetched it again meanwhile: only mark it if nothing changed since the copy
            if (h->ids[i].id == ids[i].id && h->ids[i].nfetch == ids[i].nfetch && !h->ids[i].gone) {
                h->ids[i].gone = 1;
                h->gone_total++;
                gcount++;
                if (gn < PHX_HOLD_LINE - 16)
                    gn += snprintf(gl + gn, sizeof gl - (size_t)gn, "%s%u", gn ? "," : "", ids[i].id);
            }
        }
    const unsigned gone_total = h->gone_total, gens = h->gens, over = h->over, sess = h->sess;
    const uint64_t fetches = h->fetches;
    pthread_mutex_unlock(&h->mu);

    const uint64_t tick = clock_gettime_nsec_np(CLOCK_REALTIME) / 2000000000ull;
    if (alive || gcount) {
        char head[300], line[PHX_HOLD_LINE + 96], item[64];
        snprintf(head, sizeof head,
                 "holders tick %llu sess %u: %dx%d fetches %llu ids %u (over %u, gens %u, gone %u), alive %u, in use %u, own use>0 %u, "
                 "queue %u, cache %u (qc %llu ms) |",
                 (unsigned long long)tick, sess, W, H, (unsigned long long)fetches, n, over, gens, gone_total, alive, inuse, ucpos, nq, nc,
                 (unsigned long long)qc_age);
        // items: id[.gen]=<own use count><I in use | .><q | .><c | .>/<times fetched>/<ms since the last fetch>
        unsigned i = 0, part = 0;
        do {
            int l = snprintf(line, sizeof line, "%s p%u", head, part++);
            for (; i < n; i++) {
                if (st[i] < 0)
                    continue;
                char g[12] = "";
                if (ids[i].gen)
                    snprintf(g, sizeof g, ".%u", ids[i].gen);
                const int m = snprintf(item, sizeof item, " %u%s=%d%c%c%c/%u/%llu", ids[i].id, g, st[i] >> 1, st[i] & 1 ? 'I' : '.',
                                       hold_has(q, nq, ids[i].id) ? 'q' : '.', hold_has(c, nc, ids[i].id) ? 'c' : '.', ids[i].nfetch,
                                       (unsigned long long)(now - ids[i].last_ms));
                if (l + m > PHX_HOLD_LINE)
                    break;
                memcpy(line + l, item, (size_t)m + 1);
                l += m;
            }
            hold_log(line);
        } while (i < n && alive);
        if (gcount) {
            char b[PHX_HOLD_LINE + 160];
            snprintf(b, sizeof b, "holders tick %llu sess %u gone: %d new (total %u): %s", (unsigned long long)tick, sess, gcount, gone_total, gl);
            hold_log(b);
        }
    }
    free(st);
    free(ids);
}

// StartSession, decoder lock held: a census for this session (NULL when off)
static PhxHolders *phx_holders_start(PhxDec *p, int32_t w, int32_t h, OSType fmt)
{
    static unsigned sessions;
    if (access(PHX_HOLD_OFF, F_OK) == 0)
        return NULL;
    PhxHolders *x = calloc(1, sizeof *x);
    if (!x)
        return NULL;
    pthread_mutex_init(&x->mu, NULL);
    x->p = p;
    x->sess = ++sessions;
    x->w = w;
    x->h = h;
    x->fmt = fmt;
    x->done = dispatch_semaphore_create(0);
    x->q = dispatch_queue_create("phxdec.holders", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    x->timer = x->q ? dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, x->q) : NULL;
    if (!x->timer || !x->done) {
        if (x->q)
            dispatch_release(x->q);
        if (x->done)
            dispatch_release(x->done);
        pthread_mutex_destroy(&x->mu);
        free(x);
        return NULL;
    }
    // the 2 s wall-clock ticks, +1.0 s (the plugin's census runs at +0.5 s of the same tick)
    const uint64_t wall = clock_gettime_nsec_np(CLOCK_REALTIME), next = (wall / 2000000000ull + 1) * 2000000000ull + 1000000000ull;
    const struct timespec ts = {(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
    dispatch_source_set_timer(x->timer, dispatch_walltime(&ts, 0), 2000000000ull, 20000000ull);
    dispatch_source_set_event_handler(x->timer, ^{ phx_hold_tick(x); });
    dispatch_source_set_cancel_handler(x->timer, ^{ dispatch_semaphore_signal(x->done); });
    dispatch_resume(x->timer);
    char b[200];
    snprintf(b, sizeof b, "holders on: sess %u %dx%d %c%c%c%c, census every 2 s (off: %s)", x->sess, w, h, FCC(fmt), PHX_HOLD_OFF);
    hold_log(b);
    return x;
}

// DecodeFrame, decoder lock held: the pool handed out this pixel buffer
static void phx_holders_fetched(PhxDec *p, CVPixelBufferRef pb)
{
    PhxHolders *h = p->hold;
    IOSurfaceRef s = h && pb ? CVPixelBufferGetIOSurface(pb) : NULL;
    if (!s)
        return;
    const uint32_t id = IOSurfaceGetID(s);
    const uint64_t now = hold_ms();
    pthread_mutex_lock(&h->mu);
    unsigned k = 0;
    while (k < h->nids && h->ids[k].id != id)
        k++;
    if (k == h->nids) {
        if (h->nids < PHX_HOLD_MAX)
            h->ids[h->nids++] = (PhxHoldId){id, 0, 0, 0, now};
        else
            h->over++;
    } else if (h->ids[k].gone) {
        h->ids[k].gone = 0;
        h->ids[k].gen++;
        h->gens++;
        h->gone_total--;
    }
    if (k < h->nids) {
        h->ids[k].nfetch++;
        h->ids[k].last_ms = now;
    }
    h->fetches++;
    h->last_fetch_ms = now;
    h->quiet = 0;
    pthread_mutex_unlock(&h->mu);
    phx_hold_snap(p, h);
}

// teardown, decoder lock held (or Finalize): stops the census; the tick only try-locks the decoder lock, so waiting
// for a running tick here cannot deadlock
static void phx_holders_stop(PhxHolders *h)
{
    if (!h)
        return;
    dispatch_source_cancel(h->timer);
    dispatch_semaphore_wait(h->done, DISPATCH_TIME_FOREVER);
    dispatch_release(h->timer);
    dispatch_release(h->q);
    dispatch_release(h->done);
    pthread_mutex_destroy(&h->mu);
    free(h);
}
