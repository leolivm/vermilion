// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLDevice.m at this exact place: registers our VideoToolbox
   decoder (src/phxdec) in Metal processes. */
// WP6 B5 (Vermilion, AMD RX 7900 XT): our HEVC decoder on VCN4 (src/phxdec, libphxdec_hevc.dylib next to this
// plugin with its FFmpeg libraries). It registers 'hvc1' and 'hev1' at rating 1000 in VTDecoderXPCService and in every
// Metal process but WindowServer (the client picks from its own registry). Off with NVMTL_PHXDEC=0 or the file
// /Library/GPUBundles/nvmtl/phxdec-off. A session it cannot serve (VCN not up) fails StartSession and VideoToolbox
// falls back to Apple's decoder.
static void nvmtl_phxdec_register(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *me = getprogname();
        const char *e = getenv("NVMTL_PHXDEC");
        if ((e && e[0] == '0') || access("/Library/GPUBundles/nvmtl/phxdec-off", F_OK) == 0) {
            nvlog("phxdec: off (NVMTL_PHXDEC=0 or phxdec-off) - not registering in %s", me); return; }
        if (!strcmp(me, "WindowServer")) return;
        Dl_info di;
        if (!dladdr((const void *)nvmtl_phxdec_register, &di) || !di.dli_fname) { nvlog("phxdec: dladdr failed"); return; }
        char dir[1024], path[1200];
        strlcpy(dir, di.dli_fname, sizeof dir);
        snprintf(path, sizeof path, "%s/libphxdec_hevc.dylib", dirname(dir));
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) { nvlog("phxdec: dlopen %s FAILED: %s", path, dlerror()); return; }
        int32_t (*reg)(void) = (int32_t (*)(void))dlsym(h, "phxdec_register");
        if (!reg) { nvlog("phxdec: %s has no phxdec_register", path); return; }
        const int32_t st = reg();
        nvlog("phxdec: HEVC decoder on VCN %s in %s (-> %d, rating 1000, %s)", st ? "NOT registered" : "REGISTERED", me,
              (int)st, path);
    });
}
