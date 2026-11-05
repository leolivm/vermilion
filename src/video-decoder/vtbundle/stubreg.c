// SPDX-License-Identifier: MIT
// stubreg.dylib (test only): registers the static bundle's factory (PhxVideoDecoder_CreateInstance from the bundle
// executable named by PHXVT_STUB) for hvc1, hev1 and av01 in this process, so vtdec_test exercises the stub -> libphxdec
// chain without installing the bundle. Usage: PHXVT_STUB=<bundle>/Contents/MacOS/PhxVideoDecoder vtdec_test <movie> <n> stubreg.dylib
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <VideoToolbox/VideoToolbox.h>

extern OSStatus VTRegisterVideoDecoderWithInfo(FourCharCode codec, CFDictionaryRef info, void *factory);

__attribute__((visibility("default"))) OSStatus phxdec_register(void)
{
    const char *p = getenv("PHXVT_STUB");
    void *h = p ? dlopen(p, RTLD_NOW | RTLD_LOCAL) : NULL;
    void *f = h ? dlsym(h, "PhxVideoDecoder_CreateInstance") : NULL;
    if (!f) { fprintf(stderr, "stubreg: %s\n", h ? "no factory" : dlerror()); return -1; }
    int rating = 2000;
    CFNumberRef nr = CFNumberCreate(NULL, kCFNumberIntType, &rating);
    const void *k[] = {CFSTR("CMClassImplementationID"), CFSTR("VTRating"), CFSTR("VTIsHardwareAccelerated")};
    const void *v[] = {CFSTR("com.phx.videodecoder.stubtest"), nr, kCFBooleanTrue};
    CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus r = VTRegisterVideoDecoderWithInfo('hvc1', info, f);
    r = r ? r : VTRegisterVideoDecoderWithInfo('hev1', info, f);
    r = r ? r : VTRegisterVideoDecoderWithInfo('av01', info, f);
    CFRelease(info);
    CFRelease(nr);
    return r;
}
