// SPDX-License-Identifier: MIT
// PhxVA: the video-acceleration bundle AppleGVA loads from /Library/GPUBundles/<IODVDBundleName>.bundle (WP6 M0).
// M0 stub: logs every entry with the first bytes of its arguments, creates nothing and returns AMD's error 5, so
// VideoToolbox falls back to its software decoders. Entry points and their contract: AMDRadeonVADriver2 26.7.1
// (_AVD_CreateAVDAccelerator 0x7ffb10afe9e0: *out = 0, returns 5 when it cannot create; _AVD_DestroyAVDAccelerator
// 0x7ffb10b5b16c: 0 for a null handle).
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static void phxva_log(const char *fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    syslog(LOG_NOTICE, "PhxVA: %s", b);
    char p[64]; snprintf(p, sizeof p, "/tmp/phxva-%d.log", (int)getpid());
    FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%s\n", b); fclose(f); }
}
static void phxva_hex(const char *what, const void *p, size_t n) {
    if (!p) { phxva_log("  %s = NULL", what); return; }
    char b[3 * 128 + 1]; size_t k = 0; const uint8_t *q = p;
    for (size_t i = 0; i < n && i < 128; i++) k += (size_t)snprintf(b + k, sizeof b - k, "%02x%s", q[i], (i & 7) == 7 ? " " : "");
    phxva_log("  %s @%p: %s", what, p, b);
}

__attribute__((visibility("default"))) int AVD_CreateAVDAccelerator(void **out, void *a2, void *a3) {
    phxva_log("AVD_CreateAVDAccelerator(out %p, %p, %p) in %s", (void *)out, a2, a3, getprogname());
    phxva_hex("arg2", a2, 128);
    phxva_hex("arg3", a3, 128);
    if (out) *out = NULL;
    return 5;
}
__attribute__((visibility("default"))) int AVD_DestroyAVDAccelerator(void **h) {
    phxva_log("AVD_DestroyAVDAccelerator(%p)", (void *)h);
    return 0;
}
__attribute__((constructor)) static void phxva_loaded(void) { phxva_log("loaded in %s (pid %d)", getprogname(), (int)getpid()); }
