// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code: scanout/present diagnostics (flicker probe, E0 trace). Included by nvmtl_scanout.c between the scanout
   setup and nvmtl_vk_scanout_present; logging only, nothing here changes what is drawn or shown. Measurement build only
   (NVMTL_DIAG, nvmtl_vk.h): the default build compiles both to nothing. */
/* NVMTL_FLICK_PROBE=1 (diagnostic): a hash of every 64x64 tile of each presented frame (sampled every 8th row and pixel).
   A tile that comes back to its value of two presents ago after differing in between (A, B, A) is a flicker; a present
   where many tiles do that at once is logged with its surface id, and the first few such frames (B) are written to
   /tmp/flick-<n>-{prev,bad,next}.raw (phxshot layout: 'PSHT', w, h, 0, then the rows) to be looked at. */
/* E0 trace (NVMTL_E0_TRACE=1, WindowServer only): one line per WindowServer commit that writes a full-screen surface,
   per finish of such a commit, per scanout present and per flick event, with the uptime in microseconds, to
   /tmp/e0-trace.txt. Every 600000 lines it is renamed to /tmp/e0-trace.old and started again, so a long session keeps a
   live trace. Logging only: nothing it records changes what is drawn or shown. */
#if NVMTL_DIAG_BUILD
static pthread_mutex_t g_e0_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_e0_file; static int g_e0_on; static uint64_t g_e0_lines;
int nvmtl_e0_enabled(void)
{
    if (!g_e0_on) { const char *e = getenv("NVMTL_E0_TRACE"); const char *me = getprogname();
        g_e0_on = e && e[0] == '1' && me && !strcmp(me, "WindowServer") ? 1 : -1; }
    return g_e0_on == 1;
}
void nvmtl_e0_write(const char *fmt, ...)
{
    if (!nvmtl_e0_enabled()) return;
    const uint64_t us = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    pthread_mutex_lock(&g_e0_lock);
    if (!g_e0_file && g_e0_lines == 0) { g_e0_file = fopen("/tmp/e0-trace.txt", "w"); if (g_e0_file) setvbuf(g_e0_file, NULL, _IOFBF, 1 << 16); }
    if (g_e0_file && g_e0_lines >= 600000) {
        fclose(g_e0_file); rename("/tmp/e0-trace.txt", "/tmp/e0-trace.old");
        g_e0_file = fopen("/tmp/e0-trace.txt", "w"); if (g_e0_file) setvbuf(g_e0_file, NULL, _IOFBF, 1 << 16);
        g_e0_lines = 1;
    }
    if (g_e0_file) {
        fprintf(g_e0_file, "%llu %s\n", (unsigned long long)us, b);
        if (++g_e0_lines % 64 == 0) fflush(g_e0_file);
    }
    pthread_mutex_unlock(&g_e0_lock);
}
#endif
#define NVMTL_FT 64
static struct { int on, l10r; uint32_t w, h, tx, ty, n, events, dumps; uint32_t *hash[3]; uint32_t sid[3]; uint8_t *frame[2]; uint64_t presents; } g_flick;
static void nvmtl_flick_dump(const char *tag, uint32_t n, const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch)
{
    char p[96]; snprintf(p, sizeof p, "/tmp/flick-%u-%s.raw", n, tag);
    FILE *f = fopen(p, "wb"); if (!f) return;
    const uint32_t hdr[4] = { 0x54485350u, w, h, 0 }; fwrite(hdr, 4, 4, f);
    for (uint32_t y = 0; y < h; y++) fwrite(px + (size_t)y * pitch, 4, w, f);
    fclose(f);
}
static void nvmtl_flick_probe(uint32_t sid, const void *base, uint32_t w, uint32_t h, uint32_t pitch, int l10r)
{
    if (!NVMTL_DIAG_BUILD) return;
    g_flick.l10r = l10r;
    if (!g_flick.on) { const char *e = getenv("NVMTL_FLICK_PROBE"); g_flick.on = e && e[0] == '1' ? 1 : -1;
        if (g_flick.on == 1) nvlog("flick probe: ON (A-B-A tile flips per present, frames to /tmp/flick-*)"); }
    if (g_flick.on != 1 || !base) return;
    if (g_flick.w != w || g_flick.h != h) {
        for (int i = 0; i < 3; i++) { free(g_flick.hash[i]); g_flick.hash[i] = NULL; }
        for (int i = 0; i < 2; i++) { free(g_flick.frame[i]); g_flick.frame[i] = NULL; }
        g_flick.w = w; g_flick.h = h; g_flick.tx = (w + NVMTL_FT - 1) / NVMTL_FT; g_flick.ty = (h + NVMTL_FT - 1) / NVMTL_FT; g_flick.n = 0;
        for (int i = 0; i < 3; i++) g_flick.hash[i] = (uint32_t *)calloc((size_t)g_flick.tx * g_flick.ty, 4);
        for (int i = 0; i < 2; i++) g_flick.frame[i] = (uint8_t *)malloc((size_t)pitch * h);
        if (!g_flick.hash[0] || !g_flick.hash[1] || !g_flick.hash[2] || !g_flick.frame[0] || !g_flick.frame[1]) { g_flick.on = -1; return; }
    }
    uint32_t *cur = g_flick.hash[g_flick.n % 3], *p1 = g_flick.hash[(g_flick.n + 2) % 3], *p2 = g_flick.hash[(g_flick.n + 1) % 3];
    const uint8_t *px = (const uint8_t *)base;
    for (uint32_t ty = 0; ty < g_flick.ty; ty++) for (uint32_t tx = 0; tx < g_flick.tx; tx++) {
        uint32_t hsh = 2166136261u;
        for (uint32_t y = ty * NVMTL_FT; y < (ty + 1) * NVMTL_FT && y < h; y += 8)
            for (uint32_t x = tx * NVMTL_FT; x < (tx + 1) * NVMTL_FT && x < w; x += 8) {
                uint32_t v; memcpy(&v, px + (size_t)y * pitch + (size_t)x * 4, 4);
                /* only a visible change counts: the top 5 bits of each colour channel (rounding noise in a blur is not flicker) */
                const uint32_t q = g_flick.l10r ? (((v >> 25) & 31) << 10) | (((v >> 15) & 31) << 5) | ((v >> 5) & 31)
                                                : (((v >> 19) & 31) << 10) | (((v >> 11) & 31) << 5) | ((v >> 3) & 31);
                hsh = (hsh ^ q) * 16777619u; }
        cur[ty * g_flick.tx + tx] = hsh;
    }
    g_flick.sid[g_flick.n % 3] = sid;
    uint32_t flips = 0, minx = 9999, miny = 9999, maxx = 0, maxy = 0;
    if (g_flick.n >= 2)
        for (uint32_t i = 0; i < g_flick.tx * g_flick.ty; i++)
            if (cur[i] == p2[i] && p1[i] != cur[i]) { flips++; uint32_t x = i % g_flick.tx, y = i / g_flick.tx;
                if (x < minx) minx = x; if (y < miny) miny = y; if (x > maxx) maxx = x; if (y > maxy) maxy = y; }
    /* Drag lag: every present logs how many tiles changed since the previous present (0 = the eye sees a repeated frame) and
       their bounding box in tiles, so present rate and new-content rate can be told apart */
    if (g_flick.n >= 1) {
        uint32_t chg = 0, cx0 = 9999, cy0 = 9999, cx1 = 0, cy1 = 0;
        for (uint32_t i = 0; i < g_flick.tx * g_flick.ty; i++)
            if (cur[i] != p1[i]) { chg++; uint32_t x = i % g_flick.tx, y = i / g_flick.tx;
                if (x < cx0) cx0 = x; if (y < cy0) cy0 = y; if (x > cx1) cx1 = x; if (y > cy1) cy1 = y; }
        if (chg) nvmtl_e0("H present %llu sid %u changed %u x %u..%u y %u..%u", (unsigned long long)g_flick.presents, sid, chg, cx0, cx1, cy0, cy1);
        else nvmtl_e0("H present %llu sid %u changed 0", (unsigned long long)g_flick.presents, sid);
    }
    // a moving cursor flips the few tiles it leaves; flicker flips many, spread out
    const int spread = flips && (maxx - minx > 2 || maxy - miny > 2);
    if (flips >= 6 && spread && g_flick.presents >= 300) {   /* the first presents switch BGRA to l10r surfaces */
        g_flick.events++;
        nvmtl_e0("K present %llu tiles %u x %u..%u y %u..%u sids %u %u %u", (unsigned long long)g_flick.presents, flips, minx, maxx, miny, maxy,
                 g_flick.sid[(g_flick.n + 1) % 3], g_flick.sid[(g_flick.n + 2) % 3], sid);
        if (g_flick.events <= 60) { time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
            nvlog("flick probe: %02d:%02d:%02d present %llu: %u tiles A-B-A, tiles x %u..%u y %u..%u, surfaces %u -> %u -> %u",
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (unsigned long long)g_flick.presents, flips, minx, maxx, miny, maxy,
            g_flick.sid[(g_flick.n + 1) % 3], g_flick.sid[(g_flick.n + 2) % 3], sid); }
        if (g_flick.dumps < 6 && (g_flick.events % 10) == 1) {
            const uint32_t d = ++g_flick.dumps;
            nvmtl_flick_dump("prev", d, g_flick.frame[g_flick.n % 2], w, h, pitch);          /* two presents ago (A) */
            nvmtl_flick_dump("bad", d, g_flick.frame[(g_flick.n + 1) % 2], w, h, pitch);     /* the odd one (B) */
            nvmtl_flick_dump("next", d, px, w, h, pitch);                                    /* back to A */
        }
    }
    memcpy(g_flick.frame[g_flick.n % 2], px, (size_t)pitch * h);
    g_flick.n++; g_flick.presents++;
    if (g_flick.presents % 2000 == 0) nvlog("flick probe: %llu presents, %u flicker events", (unsigned long long)g_flick.presents, g_flick.events);
}
