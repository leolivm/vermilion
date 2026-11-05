// SPDX-License-Identifier: MIT
// PhxVideoDecoder.bundle executable (WP6 B7 delivery): a static VideoToolbox decoder plug-in. VideoToolbox reads the
// bundle's Info.plist (CMClassImplementations) in every client and, because the plist has no VTAllowSandboxedDecode,
// runs the decoder remote in VTDecoderXPCService. The client may load this executable too, so it links nothing heavy:
// libphxdec_hevc.dylib (FFmpeg + RADV) is opened only when a session is created.
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

#define PHXVT_DECODER "/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/libphxdec_hevc.dylib"
enum { kPhxCouldNotFindDecoder = -12906 };
typedef int32_t (*phx_factory)(uint32_t codec, CFAllocatorRef alloc, void **out);

__attribute__((visibility("default"))) int32_t PhxVideoDecoder_CreateInstance(uint32_t codec, CFAllocatorRef alloc, void **out)
{
    static phx_factory factory;
    static dispatch_once_t once;
    if (access("/Library/GPUBundles/nvmtl/phxdec-off", F_OK) == 0)
        return kPhxCouldNotFindDecoder;
    if (codec == 'av01' && access("/Library/GPUBundles/nvmtl/phxdec-av1", F_OK) != 0)
        return kPhxCouldNotFindDecoder;
    dispatch_once(&once, ^{
        void *h = dlopen(PHXVT_DECODER, RTLD_NOW | RTLD_LOCAL);
        if (h)
            factory = (phx_factory)dlsym(h, "PhxDecHEVC_CreateInstance");
    });
    return factory ? factory(codec, alloc, out) : kPhxCouldNotFindDecoder;
}
