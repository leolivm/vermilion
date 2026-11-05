// SPDX-License-Identifier: MIT
/* phx_vtdec.h - private VideoToolbox decoder ABI, macOS 26.7.1 x86_64 (layout verified against NullMoth
   libnvdec_hevc.dylib __DATA_CONST,__const 0xf290-0xf350 and VideoToolbox call sites). */
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef struct OpaqueVTVideoDecoder *VTVideoDecoderRef;      /* == its CMBaseObjectRef */
typedef struct OpaqueVTVideoDecoderSession *VTVideoDecoderSession;
typedef struct OpaqueVTVideoDecoderFrame *VTVideoDecoderFrame;
typedef const void *CMBaseClassID;
typedef uint32_t CMBaseClassVersion;

#pragma pack(push, 4)
typedef struct {
    CMBaseClassVersion version;                                       /* +0x00 = 1 */
    size_t derivedStorageSize;                                        /* +0x04 */
    Boolean  (*equal)(CMBaseObjectRef, CMBaseObjectRef);              /* +0x0c, may be NULL */
    OSStatus (*invalidate)(CMBaseObjectRef);                          /* +0x14 */
    void     (*finalize)(CMBaseObjectRef);                            /* +0x1c */
    CFStringRef (*copyDebugDescription)(CMBaseObjectRef);             /* +0x24 */
    OSStatus (*copyProperty)(CMBaseObjectRef, CFStringRef key, CFAllocatorRef, void *valueOut); /* +0x2c */
    OSStatus (*setProperty)(CMBaseObjectRef, CFStringRef key, CFTypeRef value);                /* +0x34, may be NULL */
    OSStatus (*notificationBarrier)(CMBaseObjectRef);                 /* +0x3c, may be NULL */
    const void *protocolTable;                                        /* +0x44, NULL */
} CMBaseClass;                                                        /* sizeof 0x4c */

typedef struct {
    CMBaseClassVersion version;                                                       /* +0x00 = 1 */
    OSStatus (*startSession)(VTVideoDecoderRef, VTVideoDecoderSession, CMVideoFormatDescriptionRef);   /* +0x04 */
    OSStatus (*decodeFrame)(VTVideoDecoderRef, VTVideoDecoderFrame, CMSampleBufferRef,
                            VTDecodeFrameFlags, VTDecodeInfoFlags *infoFlagsOut);                       /* +0x0c */
    OSStatus (*copySupportedPropertyDictionary)(VTVideoDecoderRef, CFDictionaryRef *out);              /* +0x14 */
    OSStatus (*setProperties)(VTVideoDecoderRef, CFDictionaryRef);                                     /* +0x1c */
    OSStatus (*copySerializableProperties)(VTVideoDecoderRef, CFAllocatorRef, CFDictionaryRef *out);   /* +0x24 */
    Boolean  (*canAcceptFormatDescription)(VTVideoDecoderRef, CMFormatDescriptionRef);                 /* +0x2c */
    OSStatus (*finishDelayedFrames)(VTVideoDecoderRef);                                                /* +0x34 */
    void *reservedV3[3];   /* +0x3c/+0x44/+0x4c tile-session slots, read only when version >= 3; keep NULL */
} VTVideoDecoderClass;
#pragma pack(pop)

typedef struct {
    const void *reserved;                 /* +0x00 = NULL */
    const CMBaseClass *baseClass;         /* +0x08 */
    const VTVideoDecoderClass *derivedClass; /* +0x10 */
} CMBaseVTable;

/* CoreMedia (exported) */
extern OSStatus CMDerivedObjectCreate(CFAllocatorRef, const CMBaseVTable *, CMBaseClassID, CMBaseObjectRef *out);
extern void    *CMBaseObjectGetDerivedStorage(CMBaseObjectRef);
extern const CMBaseVTable *CMBaseObjectGetVTable(CMBaseObjectRef);
/* VideoToolbox (exported) */
extern CMBaseClassID   VTVideoDecoderGetClassID(void);
extern CMBaseObjectRef VTVideoDecoderGetCMBaseObject(VTVideoDecoderRef);
typedef OSStatus (*VTVideoDecoderFactory)(FourCharCode codec, CFAllocatorRef, VTVideoDecoderRef *out);
extern OSStatus VTRegisterVideoDecoderWithInfo(CMVideoCodecType, CFDictionaryRef info, VTVideoDecoderFactory);
extern OSStatus VTRegisterVideoDecoder(CMVideoCodecType, VTVideoDecoderFactory);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession, CFDictionaryRef);
extern CVPixelBufferPoolRef VTDecoderSessionGetPixelBufferPool(VTVideoDecoderSession);
extern CFDictionaryRef VTDecoderSessionGetDestinationPixelBufferAttributes(VTVideoDecoderSession);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession, VTVideoDecoderFrame, OSStatus status,
                                                 VTDecodeInfoFlags infoFlags, CVImageBufferRef /*nullable*/);
extern OSStatus VTCopyVideoDecoderList(CFDictionaryRef options, CFArrayRef *listOut);   /* exported, not in the SDK; for tests */
