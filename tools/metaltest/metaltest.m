// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Vermilion contributors
//
// metaltest: the Metal-device probe the boot daemon (tools/phxctl/phxboot-navi31.sh) runs as the desktop user before
// it restarts WindowServer. It prints "Metal devices: <n>" (the line the daemon looks for), lists the devices, then
// compiles and runs a small compute kernel (c = a + b over 4096 floats) on the Radeon and checks every result.
// Exit codes: 0 all results correct, 1 no Metal device, 2 shader compile failed, 3 pipeline failed, 4 GPU error,
// 5 wrong results. Build: scripts/build/metaltest.sh. Install: ~/gpu/metaltest (docs/INSTALL.md).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

int main() {
  @autoreleasepool {
    NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
    NSLog(@"Metal devices: %lu", (unsigned long)devs.count);
    id<MTLDevice> dev = nil;
    for (id<MTLDevice> d in devs) {
      NSLog(@"  - %@ (headless=%d lowPower=%d registryID=%llu)", d.name, d.isHeadless, d.isLowPower, d.registryID);
      if ([d.name containsString:@"Radeon"] || [d.name containsString:@"7900"]) dev = d;
    }
    if (!dev) dev = devs.count ? devs[0] : MTLCreateSystemDefaultDevice();
    if (!dev) { NSLog(@"NO METAL DEVICE"); return 1; }
    NSLog(@"using device: %@", dev.name);

    NSError *err = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void addv(device const float*a[[buffer(0)]],device const float*b[[buffer(1)]],device float*c[[buffer(2)]],uint i[[thread_position_in_grid]]){c[i]=a[i]+b[i];}";
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) { NSLog(@"LIBRARY COMPILE FAILED: %@", err); return 2; }
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"addv"] error:&err];
    if (!pso) { NSLog(@"PIPELINE FAILED: %@", err); return 3; }

    const int N = 4096;
    id<MTLBuffer> A = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> B = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> C = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    float *a = A.contents, *b = B.contents, *c = C.contents;
    for (int i = 0; i < N; i++) { a[i] = i; b[i] = 2 * i; c[i] = -1; }

    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:pso];
    [e setBuffer:A offset:0 atIndex:0];
    [e setBuffer:B offset:0 atIndex:1];
    [e setBuffer:C offset:0 atIndex:2];
    [e dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) { NSLog(@"GPU EXEC ERROR: %@", cb.error); return 4; }

    int ok = 0;
    for (int i = 0; i < N; i++) if (c[i] == 3.0f * i) ok++;
    NSLog(@"COMPUTE RESULT: %d/%d correct  (c[100]=%.1f expect 300)", ok, N, c[100]);
    return ok == N ? 0 : 5;
  }
}
