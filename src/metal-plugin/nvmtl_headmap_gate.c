// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLWSPresent.m at its top (one translation unit with the plugin):
   M7 S4b, the head gate in front of HUBP0. The kernel (PhxAccel,
   debug.phxsurfmap) learns from every display transaction which head named which IOSurface; before HUBP0 is flipped to a
   surface, nvmtl_vk_scanout_present asks here whether that surface is head 0's. The pure part (parse, verdict, policy,
   stall) is nvmtl_headmap.h, host-tested.
   Switch file /Library/GPUBundles/nvmtl/ws-headmap, re-read every second: absent = off (no sysctl read, today's
   behaviour), "observe" = count and trace only, empty or "enforce" = only head 0's surfaces reach HUBP0. */
#include "nvmtl_headmap.h"
#include <libproc.h>

static _Atomic int nvmtl_hm_mode_v;
static int nvmtl_hm_mode(void) {
    static _Atomic uint64_t checked;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        char buf[32] = { 0 }; int exists = 0;
        FILE *f = fopen("/Library/GPUBundles/nvmtl/ws-headmap", "r");
        if (f) { exists = 1; if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
        const int m = nvmtl_hm_mode_parse(exists, buf);
        if (m != atomic_exchange(&nvmtl_hm_mode_v, m))
            nvlog("ws head map: %s (ws-headmap)", m == 2 ? "ENFORCE - only head 0's surfaces reach HUBP0" : m == 1 ? "observe - counting only" : "off");
        atomic_store(&checked, now);
    }
    return atomic_load(&nvmtl_hm_mode_v);
}

/* The kernel's table now, without the previous WindowServer's entries. NULL = no map (an older PhxAccel without the sysctl, or a refused blob). */
static const nvmtl_hm *nvmtl_hm_read(nvmtl_hm *m) {
    static int mib[CTL_MAXNAME]; static size_t miblen; static int state;   /* 0 untried, 1 mib cached, -1 no sysctl */
    if (state == 0) { miblen = CTL_MAXNAME; state = sysctlnametomib("debug.phxsurfmap", mib, &miblen) == 0 ? 1 : -1;
        if (state < 0) nvlog("ws head map: no debug.phxsurfmap (older PhxAccel): every surface is NOMAP"); }
    if (state < 0) return NULL;
    uint8_t b[NVMTL_HM_HDR + NVMTL_HM_MAX * NVMTL_HM_ENT]; size_t n = sizeof b;
    if (sysctl(mib, (u_int)miblen, b, &n, NULL, 0) != 0 || nvmtl_hm_parse(m, b, n) != 0) return NULL;
    /* entries last named before this process started belong to a previous WindowServer (reused IDs) */
    static uint64_t start_us;
    if (!start_us) { struct proc_bsdinfo bi;
        if (proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == (int)sizeof bi) start_us = bi.pbi_start_tvsec * 1000000ull + bi.pbi_start_tvusec;
        else start_us = 1;   /* unknown: keep every entry */ }
    if (start_us > 1) { struct timeval tv; gettimeofday(&tv, NULL);
        const uint64_t now_us = tv.tv_sec * 1000000ull + tv.tv_usec;
        nvmtl_hm_drop_older(m, now_us > start_us ? (now_us - start_us) / 1000 : 0); }
    return m;
}

static const char *const nvmtl_hm_names[5] = { "ok", "unknown", "other", "ambiguous", "nomap" };

/* The present gate, called with g_scan_lock held (nvmtl_vk_scanout_present). 1 present, 0 drop this frame, -1 the stall
   fail-safe: drop and give the plane back. */
static nvmtl_hm_stats nvmtl_hm_present_stats;
static int nvmtl_hm_gate_present(uint32_t sid) {
    const int mode = nvmtl_hm_mode();
    if (!mode) return 1;
    nvmtl_hm m; const int v = nvmtl_hm_verdict(nvmtl_hm_read(&m), sid);
    nvmtl_hm_stats *s = &nvmtl_hm_present_stats;
    const int r = nvmtl_hm_decide(s, mode, v);
    nvmtl_e0("G sid %u verdict %s mode %d -> %s", sid, nvmtl_hm_names[v], mode, r > 0 ? "present" : r == 0 ? "drop" : "stall");
    if (v != NVMTL_HM_OK) { static unsigned said; if (said++ < 20) nvlog("ws head map: present of surface %u is %s (mode %d)", sid, nvmtl_hm_names[v], mode); }
    if (s->calls % 3000 == 0)
        nvlog("ws head map: %llu presents checked: ok %llu unknown %llu other %llu ambiguous %llu nomap %llu; refused %llu, stalls %llu (mode %d)",
              (unsigned long long)s->calls, (unsigned long long)s->verdicts[0], (unsigned long long)s->verdicts[1], (unsigned long long)s->verdicts[2],
              (unsigned long long)s->verdicts[3], (unsigned long long)s->verdicts[4], (unsigned long long)s->refused, (unsigned long long)s->stalls, mode);
    return r;
}

/* The display-set filter (nvmtl_ws_note_display), at each CoreDisplay commit with a full-screen surface. */
static int nvmtl_hm_gate_join(uint32_t sid) {
    const int mode = nvmtl_hm_mode();
    if (!mode) return 1;
    nvmtl_hm m; const int v = nvmtl_hm_verdict(nvmtl_hm_read(&m), sid);
    const int ok = nvmtl_hm_may_join(mode, v);
    if (v == NVMTL_HM_OTHER || v == NVMTL_HM_AMBIGUOUS) {
        static uint32_t seen[16]; static unsigned n; unsigned i = 0;
        while (i < n && seen[i] != sid) i++;
        if (i == n && n < 16) { seen[n++] = sid; nvlog("ws head map: CoreDisplay surface %u is %s: %s the display set (mode %d)", sid, nvmtl_hm_names[v],
                                                       ok ? "would be kept out of" : "kept out of", mode); }
    }
    return ok;
}
