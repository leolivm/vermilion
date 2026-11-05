// SPDX-License-Identifier: MIT
// phxdec_hevc: a VideoToolbox HEVC ('hvc1' / 'hev1', Main and Main10) decoder for the RX 7900 XT (WP6 B5).
// The bitstream goes through FFmpeg's libavcodec (our copy in deps/ffmpeg-lib: hevc decoder + hevc_vulkan hwaccel,
// AV_CODEC_FLAG_LOW_DELAY = one picture out per sample, in decode order), which decodes on VCN4 through RADV (Vulkan
// Video) used directly as an ICD, the way the Metal plugin uses it in the same process. Each picture is downloaded to
// a pixel buffer from the session's pool (x420 / 420v, the same layout as VCN's P010 / NV12 output).
// The VideoToolbox side follows NullMoth's libnvdec_hevc.dylib:
// synchronous DecodeFrame, exactly one emit per sample, errors reported through the emit, DecodeFrame returns 0.
//
// Entry points: PhxDecHEVC_CreateInstance (the factory) and phxdec_register() (registers 'hvc1' and 'hev1', and 'av01'
// when /Library/GPUBundles/nvmtl/phxdec-av1 exists: AV1 Main 8/10-bit through FFmpeg's av1 decoder + av1_vulkan, B7).
// Environment: PHXDEC_ICD (default /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib), PHXDEC_LOG=1 (stderr log).
#include "phx_vtdec.h"
#include "phxdec_copy.h"
#include <IOSurface/IOSurfaceRef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <unistd.h>
#include <execinfo.h>
#include <os/log.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>

#define PHXDEC_DEFAULT_ICD "/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib"

static int g_log = -1;
#define LOG(...) do { if (g_log < 0) { const char *e_ = getenv("PHXDEC_LOG"); g_log = e_ && e_[0] == '1'; } \
                      if (g_log) { fprintf(stderr, "phxdec: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
// session-level events (one line per session, not per frame) also go to the unified log through os_log, prefixed with
// the process name: phxdec runs in sandboxed clients (the Wallpaper extension) where nobody reads stderr
static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    os_log(OS_LOG_DEFAULT, "phxdec[%{public}s]: %{public}s", getprogname(), b);
}
#define NOTE(...) do { LOG(__VA_ARGS__); note(__VA_ARGS__); } while (0)
// Trace build (blink localization): a CF object's description, split over several unified-log lines
static void note_cf(const char *what, CFTypeRef o)
{
    CFStringRef d = o ? CFCopyDescription(o) : NULL;
    char b[4096] = "(null)";
    if (d) {
        CFStringGetCString(d, b, sizeof(b), kCFStringEncodingUTF8);
        CFRelease(d);
    }
    // os_log compact output shows only the first line of a message: flatten the description
    char *w = b;
    for (const char *r = b; *r; r++)
        if (*r != '\n' && !(*r == ' ' && w > b && w[-1] == ' '))
            *w++ = *r;
    *w = 0;
    for (size_t i = 0, n = strlen(b), k = 0; i < n || k == 0; i += 400, k++)
        note("trace %s [%zu]: %.400s", what, k, b + i);
}
#define FCC(c) (char)((c) >> 24), (char)((c) >> 16), (char)((c) >> 8), (char)(c)

// ---- engine: libavcodec + Vulkan hwaccel ----
typedef struct {
    AVCodecContext *avctx;
    AVBufferRef *hwdev;
    AVPacket *pkt;
    AVFrame *hw, *sw;
    PFN_vkWaitSemaphores wait_sems;
    PFN_vkGetSemaphoreCounterValue sem_value;
    unsigned tr_extra, tr_eagain;   // trace: pictures beyond the first per sample, send_packet EAGAIN
    int poll_wait;   // PHXDEC_POLLWAIT=1: measurement only, spin on the counter instead of a blocking wait
    // How a picture reaches the pixel buffer (PHXDEC_DIRECT): 1 (default) = our GPU copy into the IOSurface, imported
    // once per surface (B6b); 2 = libavutil's GPU copy into the IOSurface, imported per frame (B6a); 0 = download +
    // memcpy. A failure drops one level for the rest of the session.
    int direct;
    PhxCopier *copier;
} PhxEngine;

static enum AVPixelFormat phx_get_format(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    (void)c;
    for (const enum AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_VULKAN)
            return *f;
    LOG("the Vulkan hwaccel was not offered (no VCN in this process?)");
    return AV_PIX_FMT_NONE;   // never decode in software here: VideoToolbox then uses Apple's decoder
}

static void engine_close(PhxEngine *e)
{
    if (!e)
        return;
    phx_copier_destroy(e->copier);
    avcodec_free_context(&e->avctx);
    av_buffer_unref(&e->hwdev);
    av_packet_free(&e->pkt);
    av_frame_free(&e->hw);
    av_frame_free(&e->sw);
    free(e);
}

static void set_icd_once(void)
{
    const char *icd = getenv("PHXDEC_ICD");
    setenv("FFPHX_VK_ICD", icd && *icd ? icd : PHXDEC_DEFAULT_ICD, 0);
}

static PhxEngine *engine_open(enum AVCodecID id, const uint8_t *hvcc, size_t n)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, set_icd_once);
    const AVCodec *codec = avcodec_find_decoder(id);
    PhxEngine *e = calloc(1, sizeof(*e));
    if (!codec || !e)
        goto fail;
    int r = av_hwdevice_ctx_create(&e->hwdev, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0);
    if (r < 0) {
        NOTE("Vulkan device: %d", r);
        goto fail;
    }
    {   // refuse the session unless the device has a video decode queue (VCN up and adopted, MMHUB VM on)
        const AVVulkanDeviceContext *vk = ((AVHWDeviceContext *)e->hwdev->data)->hwctx;
        int dec = 0;
        for (int i = 0; i < vk->nb_qf; i++)
            dec |= (vk->qf[i].flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) != 0;
        if (!dec) {
            NOTE("no video decode queue on this device (VCN not up): refusing");
            goto fail;
        }
    }
    {
        const AVVulkanDeviceContext *vk = ((AVHWDeviceContext *)e->hwdev->data)->hwctx;
        PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vk->get_proc_addr(vk->inst, "vkGetDeviceProcAddr");
        if (gdpa)
            e->wait_sems = (PFN_vkWaitSemaphores)gdpa(vk->act_dev, "vkWaitSemaphores");
        if (gdpa)
            e->sem_value = (PFN_vkGetSemaphoreCounterValue)gdpa(vk->act_dev, "vkGetSemaphoreCounterValue");
        const char *pw = getenv("PHXDEC_POLLWAIT");
        e->poll_wait = pw && pw[0] == '1' && e->sem_value;
        const char *dc = getenv("PHXDEC_DIRECT");
        e->direct = dc && dc[0] == '0' ? 0 : dc && dc[0] == '2' ? 2 : 1;
        if (access("/Library/GPUBundles/nvmtl/phxdec-download", F_OK) == 0)   // blink A/B: no GPU write into the IOSurface
            e->direct = 0;
        if (e->direct == 1 && !(e->copier = phx_copier_create(e->hwdev)))
            e->direct = 2;
    }
    e->avctx = avcodec_alloc_context3(codec);
    e->pkt = av_packet_alloc();
    e->hw = av_frame_alloc();
    e->sw = av_frame_alloc();
    if (!e->avctx || !e->pkt || !e->hw || !e->sw)
        goto fail;
    e->avctx->extradata = av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!e->avctx->extradata)
        goto fail;
    memcpy(e->avctx->extradata, hvcc, n);
    e->avctx->extradata_size = (int)n;
    e->avctx->hw_device_ctx = av_buffer_ref(e->hwdev);
    e->avctx->get_format = phx_get_format;
    e->avctx->thread_count = 1;
    e->avctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if ((r = avcodec_open2(e->avctx, codec, NULL)) < 0) {
        NOTE("avcodec_open2: %d", r);
        goto fail;
    }
    return e;
fail:
    engine_close(e);
    return NULL;
}

// Waits until VCN has finished the picture (the frame's timeline semaphores), so the transfer stage times only the copy
static void wait_decoded(PhxEngine *e, AVFrame *hw)
{
    if (!e->wait_sems || !hw->hw_frames_ctx)
        return;
    AVHWFramesContext *fc = (AVHWFramesContext *)hw->hw_frames_ctx->data;
    AVVulkanFramesContext *vfc = fc->hwctx;
    const AVVulkanDeviceContext *vk = fc->device_ctx->hwctx;
    AVVkFrame *f = (AVVkFrame *)hw->data[0];
    VkSemaphore sems[AV_NUM_DATA_POINTERS];
    uint64_t vals[AV_NUM_DATA_POINTERS];
    uint32_t n = 0;
    vfc->lock_frame(fc, f);
    for (int i = 0; i < AV_NUM_DATA_POINTERS && f->img[i]; i++) {
        sems[n] = f->sem[i];
        vals[n++] = f->sem_value[i];
    }
    if (e->poll_wait) {
        for (uint32_t i = 0; i < n; i++) {
            uint64_t v = 0;
            while (e->sem_value(vk->act_dev, sems[i], &v) == VK_SUCCESS && v < vals[i])
                ;
        }
    } else {
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, n, sems, vals};
        e->wait_sems(vk->act_dev, &wi, UINT64_MAX);
    }
    vfc->unlock_frame(fc, f);
}

static void no_free(void *opaque, uint8_t *data) { (void)opaque; (void)data; }

// B6a: the GPU copies the picture straight into the pixel buffer's IOSurface. The sw frame points at the locked planes;
// libavutil imports them as host memory (VK_EXT_external_memory_host) and copies image -> buffer there, so the CPU
// copies nothing. Returns 0, or <0 when the caller should take the download + memcpy path.
static int transfer_into_pixbuf(PhxEngine *e, CVPixelBufferRef pb)
{
    if (CVPixelBufferGetPlaneCount(pb) != 2 || !e->hw->hw_frames_ctx)
        return -1;
    const AVHWFramesContext *fc = (const AVHWFramesContext *)e->hw->hw_frames_ctx->data;
    if (fc->sw_format != AV_PIX_FMT_P010 && fc->sw_format != AV_PIX_FMT_NV12)
        return -1;
    if (CVPixelBufferLockBaseAddress(pb, 0))
        return -1;
    AVFrame *f = av_frame_alloc();
    int r = f ? 0 : AVERROR(ENOMEM);
    if (!r) {
        f->format = fc->sw_format;
        f->width = e->hw->width;
        f->height = e->hw->height;
        if ((size_t)f->width > CVPixelBufferGetWidth(pb))
            f->width = (int)CVPixelBufferGetWidth(pb);
        if ((size_t)f->height > CVPixelBufferGetHeight(pb))
            f->height = (int)CVPixelBufferGetHeight(pb);
        for (int i = 0; i < 2; i++) {
            f->data[i] = CVPixelBufferGetBaseAddressOfPlane(pb, i);
            f->linesize[i] = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, i);
        }
        f->buf[0] = av_buffer_create(f->data[0], 1, no_free, NULL, 0);
        r = f->buf[0] ? av_hwframe_transfer_data(f, e->hw, 0) : AVERROR(ENOMEM);
    }
    av_frame_free(&f);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return r;
}

// Copies a downloaded P010 / NV12 frame into a 2-plane pixel buffer of the same layout.
static OSStatus copy_to_pixbuf(const AVFrame *sw, CVPixelBufferRef pb)
{
    if (CVPixelBufferGetPlaneCount(pb) != 2)
        return kVTPixelTransferNotSupportedErr;
    if (CVPixelBufferLockBaseAddress(pb, 0))
        return kVTAllocationFailedErr;
    const int bpp = (sw->format == AV_PIX_FMT_P010) ? 2 : 1;
    for (int p = 0; p < 2; p++) {
        uint8_t *dst = CVPixelBufferGetBaseAddressOfPlane(pb, p);
        const size_t dstride = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
        const size_t rows = CVPixelBufferGetHeightOfPlane(pb, p);
        size_t w = (size_t)sw->width * (size_t)bpp;   // plane 1 is interleaved CbCr: same byte width
        if (w > dstride)
            w = dstride;
        const size_t h = rows < (size_t)(p ? (sw->height + 1) / 2 : sw->height) ? rows : (size_t)(p ? (sw->height + 1) / 2 : sw->height);
        for (size_t y = 0; y < h; y++)
            memcpy(dst + y * dstride, sw->data[p] + y * (size_t)sw->linesize[p], w);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return noErr;
}

// ---- VideoToolbox decoder object ----
typedef struct {
    pthread_mutex_t lock;
    VTVideoDecoderSession session;
    CMVideoFormatDescriptionRef fd;
    PhxEngine *eng;
    int av1;   // the session decodes AV1 ('av01', extradata = av1C), else HEVC
    int bit_depth, failures, dead;
    uint64_t frames, emitted;
    unsigned temporal, finishes, dropped_at_end, dno;   // measurement: temporal-flag samples, FinishDelayedFrames calls, held pictures dropped
    uint64_t ns_decode, ns_wait, ns_transfer, ns_copy;   // per stage totals, logged at invalidate
    // Reorder queue: with kVTDecodeFrame_EnableTemporalProcessing the client wants pictures in presentation order, as
    // Apple's decoder gives them; libavcodec (LOW_DELAY) hands them over in decode order, so up to num_reorder_pics
    // decoded pictures wait here and the lowest PTS goes out first (FinishDelayedFrames drains it). Without that flag
    // each picture is emitted at once, in decode order (what Apple's decoder does then too).
    struct { VTVideoDecoderFrame f; CVPixelBufferRef pb; int64_t pts; } q[16];
    int nq;
    // trace: picture pts != sample pts, pool surface still in use at fetch, error emits, distinct in-use surface IDs
    unsigned tr_ptsmis, tr_inuse, tr_err;
    // Output pool bound (resume churn): a picture never makes VT's pool
    // allocate past pool_max buffers. When the consumers hold that many, DecodeFrame waits up to PHXDEC_POOL_WAIT_MS for
    // one to come back, then reports the picture dropped (it stays a reference in libavcodec's DPB). pool_max 0 = unbounded.
    int pool_max;
    CFDictionaryRef pool_aux;
    unsigned pool_hits, pool_waited, pool_drops, pool_new_over, setprops;
    // measurement: backward sample PTS jumps of more than 1 s (re-feed / seek back), samples with 1xRealTimePlayback
    int64_t pts_max;
    int32_t pts_scale;
    unsigned pts_back, rt1x;
    struct PhxHolders *hold;   // holders1: per-surface census (phxdec_holders.c)
} PhxDec;

#define PHXDEC_POOL_MAX 32       // = PHX_CACHE (phxdec_copy.c): a bounded pool is also one the import cache holds whole
#define PHXDEC_POOL_WAIT_MS 20   // a little over one 60 Hz frame for the consumer to hand a buffer back
#define PHXDEC_POOL_ON "/Library/GPUBundles/nvmtl/phxdec-pool-bound"   // holders1: bound only with this flag (else trace4's path)

static void pool_report(PhxDec *p, const char *why)
{
    unsigned inuse = 0;
    const unsigned cached = p->eng ? phx_copier_cached(p->eng->copier, &inuse) : 0;
    note("pool after %llu frames, %llu pictures (%s): threshold %d, hits %u, waited ok %u, dropped %u; import cache %u, in use %u; "
         "pts back >1 s %u, 1x-realtime flag %u; distinct surfaces %u",
         (unsigned long long)p->frames, (unsigned long long)p->emitted, why, p->pool_max, p->pool_hits, p->pool_waited, p->pool_drops,
         cached, inuse, p->pts_back, p->rt1x, p->eng ? phx_copier_surfaces(p->eng->copier) : 0u);
}

// A pixel buffer from the session pool without growing it past pool_max (see PhxDec). Returns kCVReturnSuccess, or
// kCVReturnWouldExceedAllocationThreshold when no buffer came back in time, or the pool's own error.
static CVReturn fetch_pixbuf(PhxDec *p, CVPixelBufferPoolRef pool, CVPixelBufferRef *pb)
{
    if (!p->pool_max || !p->pool_aux)
        return CVPixelBufferPoolCreatePixelBuffer(NULL, pool, pb);
    CVReturn r = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(NULL, pool, p->pool_aux, pb);
    if (r != kCVReturnWouldExceedAllocationThreshold)
        return r;
    if (++p->pool_hits == 1)
        pool_report(p, "first threshold hit");
    for (int i = 0; i < PHXDEC_POOL_WAIT_MS / 2 && r == kCVReturnWouldExceedAllocationThreshold; i++) {
        usleep(2000);
        r = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(NULL, pool, p->pool_aux, pb);
    }
    if (r == kCVReturnSuccess)
        p->pool_waited++;
    return r;
}
#define ST(d) ((PhxDec *)CMBaseObjectGetDerivedStorage((CMBaseObjectRef)(d)))
typedef struct PhxHolders PhxHolders;
#include "phxdec_holders.c"

static void emit_lowest(PhxDec *p)
{
    int m = 0;
    for (int i = 1; i < p->nq; i++)
        if (p->q[i].pts < p->q[m].pts)
            m = i;
    VTDecoderSessionEmitDecodedFrame(p->session, p->q[m].f, noErr, 0, p->q[m].pb);
    CFRelease(p->q[m].pb);
    p->q[m] = p->q[--p->nq];
}

// emit != 0: every held picture in PTS order; emit == 0: every held refcon as FrameDropped (invalidate, new session),
// so no client waits for a frame that will never come
static void drain_queue(PhxDec *p, int emit)
{
    while (p->nq) {
        if (emit && p->session) {
            emit_lowest(p);
        } else {
            p->nq--;
            if (p->session)
                VTDecoderSessionEmitDecodedFrame(p->session, p->q[p->nq].f, noErr, kVTDecodeInfo_FrameDropped, NULL);
            CFRelease(p->q[p->nq].pb);
        }
    }
}

static void teardown(PhxDec *p)
{
    phx_holders_stop(p->hold), p->hold = NULL;
    p->dropped_at_end += (unsigned)p->nq;
    drain_queue(p, 0);
    engine_close(p->eng);
    p->eng = NULL;
    if (p->pool_aux)
        CFRelease(p->pool_aux);
    p->pool_aux = NULL;
    if (p->fd)
        CFRelease(p->fd);
    p->fd = NULL;
}

static int bit_depth_of(CMVideoFormatDescriptionRef fd, const uint8_t *hvcc, size_t n)
{
    CFNumberRef bpc = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_BitsPerComponent);
    int v = 0;
    if (bpc && CFNumberGetValue(bpc, kCFNumberIntType, &v) && (v == 8 || v == 10))
        return v;
    // hvcC: byte 20 = bitDepthLumaMinus8 in its low 3 bits
    if (n > 21)
        return 8 + (hvcc[20] & 7);
    return 8;
}

static CFDictionaryRef make_attrs(int32_t w, int32_t h, OSType fmt)
{
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &w), nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);
    CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
    CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    // Plane starts on a page and rows on 256 B: the Metal plugin can then sample each plane of the surface in place (a
    // linear image bound at the plane's offset needs 256 B alignment). The default layout puts the planes at +32 and
    // +16588832, so WindowServer made a private 4K image per surface and copied into it every frame (WP6 B6c).
    int32_t pa = 4096, ra = 256;
    CFNumberRef npa = CFNumberCreate(NULL, kCFNumberSInt32Type, &pa), nra = CFNumberCreate(NULL, kCFNumberSInt32Type, &ra);
    const void *k[] = {kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey,
                       kCVPixelBufferPlaneAlignmentKey, kCVPixelBufferBytesPerRowAlignmentKey};
    const void *v[] = {nw, nh, nf, ios, npa, nra};
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(nw); CFRelease(nh); CFRelease(nf); CFRelease(ios); CFRelease(npa); CFRelease(nra);
    return d;
}

static OSStatus StartSession(VTVideoDecoderRef d, VTVideoDecoderSession s, CMVideoFormatDescriptionRef fd)
{
    const FourCharCode st = CMFormatDescriptionGetMediaSubType(fd);
    const CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fd);
    NOTE("StartSession enter: %c%c%c%c %dx%d", FCC(st), dim.width, dim.height);
    const int av1 = st == 'av01';
    if (st != 'hvc1' && st != 'hev1' && !av1) {
        NOTE("StartSession: subtype not HEVC or AV1 -> %d", (int)kVTVideoDecoderUnsupportedDataFormatErr);
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    // HEVC with alpha (Memoji, stickers): ContainsAlphaChannel is a CFNumber 1 there, not a CFBoolean; AlphaChannelMode
    // (straight / premultiplied) is set too. We decode the base layer only, so leave these to Apple's decoder.
    CFTypeRef alpha = CMFormatDescriptionGetExtension(fd, CFSTR("ContainsAlphaChannel"));
    int alpha_on = 0;
    if (alpha && CFGetTypeID(alpha) == CFBooleanGetTypeID())
        alpha_on = CFBooleanGetValue(alpha);
    else if (alpha && CFGetTypeID(alpha) == CFNumberGetTypeID())
        CFNumberGetValue(alpha, kCFNumberIntType, &alpha_on);
    if (alpha_on || CMFormatDescriptionGetExtension(fd, CFSTR("AlphaChannelMode"))) {
        NOTE("HEVC with alpha (multi-layer): left to Apple's decoder");
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    CFDictionaryRef atoms = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    CFDataRef hvcc = atoms ? CFDictionaryGetValue(atoms, av1 ? CFSTR("av1C") : CFSTR("hvcC")) : NULL;
    if (!hvcc || CFGetTypeID(hvcc) != CFDataGetTypeID()) {
        NOTE("StartSession: no %s atom (atoms %p) -> %d", av1 ? "av1C" : "hvcC", (const void *)atoms, (int)kVTVideoDecoderBadDataErr);
        return kVTVideoDecoderBadDataErr;
    }
    const uint8_t *hb = CFDataGetBytePtr(hvcc);
    const size_t hn = (size_t)CFDataGetLength(hvcc);
    int bd = av1 ? 8 : bit_depth_of(fd, hb, hn);
    if (av1) {   // av1C byte 1: seq_profile in the top 3 bits; byte 2: high_bitdepth 0x40, twelve_bit 0x20, monochrome 0x10,
                 // chroma_subsampling_x 0x08, chroma_subsampling_y 0x04. VCN4 decodes Main (profile 0): 4:2:0, 8 or 10 bit
        if (hn < 4 || (hb[0] & 0x7f) != 1 || (hb[1] >> 5) != 0 || (hb[2] & 0x30) || (hb[2] & 0x0c) != 0x0c) {
            NOTE("StartSession: AV1 not Main 4:2:0 (av1C %zu bytes %02x %02x %02x) -> %d", hn, hn > 0 ? hb[0] : 0,
                 hn > 1 ? hb[1] : 0, hn > 2 ? hb[2] : 0, (int)kVTVideoDecoderUnsupportedDataFormatErr);
            return kVTVideoDecoderUnsupportedDataFormatErr;
        }
        bd = hb[2] & 0x40 ? 10 : 8;
    }
    if (bd != 8 && bd != 10) {
        NOTE("StartSession: %d-bit -> %d", bd, (int)kVTVideoDecoderUnsupportedDataFormatErr);
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    PhxDec *p = ST(d);
    pthread_mutex_lock(&p->lock);
    teardown(p);
    p->eng = engine_open(av1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_HEVC, hb, hn);
    if (!p->eng) {
        pthread_mutex_unlock(&p->lock);
        NOTE("StartSession: no engine, VideoToolbox falls back");
        return kVTVideoDecoderNotAvailableNowErr;
    }
    p->bit_depth = bd;
    p->av1 = av1;
    Boolean full = false;
    CFBooleanRef fr = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_FullRangeVideo);
    if (fr && CFGetTypeID(fr) == CFBooleanGetTypeID())
        full = CFBooleanGetValue(fr);
    const OSType fmt = bd == 10 ? (full ? 'xf20' : 'x420') : (full ? '420f' : '420v');
    CFDictionaryRef attrs = make_attrs(dim.width, dim.height, fmt);
    const OSStatus r = VTDecoderSessionSetPixelBufferAttributes(s, attrs);
    note_cf("our pool attrs", attrs);
    note_cf("client destination attrs", VTDecoderSessionGetDestinationPixelBufferAttributes(s));
    note_cf("format description", fd);
    CFRelease(attrs);
    p->session = s;
    p->fd = (CMVideoFormatDescriptionRef)CFRetain(fd);
    p->failures = p->dead = 0;
    p->hold = phx_holders_start(p, dim.width, dim.height, fmt);
    {
        const char *pm = getenv("PHXDEC_POOL_MAX");   // vtdec_test A/B; the box uses the flag file
        p->pool_max = pm && *pm ? atoi(pm) : access(PHXDEC_POOL_ON, F_OK) == 0 ? PHXDEC_POOL_MAX : 0;
        if (p->pool_max < 0)
            p->pool_max = 0;
        p->pool_hits = p->pool_waited = p->pool_drops = p->pool_new_over = p->setprops = 0;
        p->pts_max = 0;
        p->pts_scale = 0;
        p->pts_back = p->rt1x = 0;
        if (p->pool_max) {
            const int32_t th = p->pool_max;
            CFNumberRef nt = CFNumberCreate(NULL, kCFNumberSInt32Type, &th);
            const void *ak[] = {kCVPixelBufferPoolAllocationThresholdKey};
            const void *av[] = {nt};
            p->pool_aux = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFRelease(nt);
        }
    }
    NOTE("StartSession output pool threshold %d%s", p->pool_max, p->pool_max ? "" : " (unbounded)");
    NOTE("StartSession %s %dx%d %d-bit %s -> %d (%s)", av1 ? "AV1" : "HEVC", dim.width, dim.height, bd, full ? "full" : "video", (int)r,
         p->eng->direct == 1 ? "cached GPU copy into the pixel buffer" : p->eng->direct == 2 ? "GPU copy into the pixel buffer" : "download + memcpy");
    pthread_mutex_unlock(&p->lock);
    return r;
}

// Decodes one sample; fills *out with the hw picture if one came out (at most one with LOW_DELAY).
static int engine_decode(PhxEngine *e, const uint8_t *data, size_t n, int64_t pts, int *got)
{
    *got = 0;
    av_packet_unref(e->pkt);
    if (av_new_packet(e->pkt, (int)n) < 0)
        return AVERROR(ENOMEM);
    memcpy(e->pkt->data, data, n);
    e->pkt->pts = pts;
    int r = avcodec_send_packet(e->avctx, e->pkt);
    if (r == AVERROR(EAGAIN))
        e->tr_eagain++;
    if (r < 0 && r != AVERROR(EAGAIN))
        return r;
    av_frame_unref(e->hw);
    r = avcodec_receive_frame(e->avctx, e->hw);
    if (r == AVERROR(EAGAIN))
        return 0;
    if (r < 0)
        return r;
    // drain anything beyond the first (should not happen with LOW_DELAY): keep the newest
    AVFrame *extra = av_frame_alloc();
    while (extra && avcodec_receive_frame(e->avctx, extra) == 0) {
        e->tr_extra++;
        av_frame_unref(e->hw);
        av_frame_move_ref(e->hw, extra);
    }
    av_frame_free(&extra);
    *got = 1;
    return 0;
}

static OSStatus DecodeFrame(VTVideoDecoderRef d, VTVideoDecoderFrame f, CMSampleBufferRef sb, VTDecodeFrameFlags flags,
                            VTDecodeInfoFlags *info)
{
    PhxDec *p = ST(d);
    if (info)
        *info = 0;
    pthread_mutex_lock(&p->lock);
    OSStatus st = noErr;
    int got = 0;
    if (p->dead || !p->eng) {
        st = kVTVideoDecoderBadDataErr;
    } else {
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        const size_t n = bb ? CMBlockBufferGetDataLength(bb) : 0;
        uint8_t *tmp = n ? malloc(n) : NULL;
        if (!tmp || CMBlockBufferCopyDataBytes(bb, 0, n, tmp)) {
            st = kVTVideoDecoderBadDataErr;
        } else {
            const CMTime t = CMSampleBufferGetPresentationTimeStamp(sb);
            if (t.flags & kCMTimeFlags_Valid) {
                if (p->pts_scale == t.timescale && t.value + (int64_t)t.timescale < p->pts_max) {
                    p->pts_back++;
                    p->pts_max = t.value;
                } else if (p->pts_scale != t.timescale || t.value > p->pts_max) {
                    p->pts_scale = t.timescale;
                    p->pts_max = t.value;
                }
            }
            if (flags & kVTDecodeFrame_1xRealTimePlayback)
                p->rt1x++;
            if (p->frames < 2) {   // measurement: what the first samples of a session are
                int vcl = -1;
                for (size_t o = 0; !p->av1 && o + 5 <= n && vcl < 0;) {
                    const size_t l = (size_t)tmp[o] << 24 | (size_t)tmp[o + 1] << 16 | (size_t)tmp[o + 2] << 8 | tmp[o + 3];
                    const int ty = (tmp[o + 4] >> 1) & 0x3f;
                    if (ty < 32)
                        vcl = ty;
                    o += 4 + l;
                }
                CFArrayRef at = CMSampleBufferGetSampleAttachmentsArray(sb, false);
                CFDictionaryRef a0 = at && CFArrayGetCount(at) ? CFArrayGetValueAtIndex(at, 0) : NULL;
                const Boolean notsync = a0 && CFDictionaryGetValue(a0, kCMSampleAttachmentKey_NotSync) == kCFBooleanTrue;
                NOTE("sample %llu: pts %lld/%d, %s, first VCL NAL type %d, %zu bytes, flags 0x%x", (unsigned long long)p->frames,
                     (long long)t.value, (int)t.timescale, notsync ? "NOT sync" : "sync", vcl, n, (unsigned)flags);
            }
            const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            const int r = engine_decode(p->eng, tmp, n, t.value, &got);
            p->ns_decode += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
            if (got && p->eng->hw->pts != t.value && ++p->tr_ptsmis <= 8)
                note("trace pts mismatch at sample %llu: picture %lld, sample %lld", (unsigned long long)p->frames,
                     (long long)p->eng->hw->pts, (long long)t.value);
            if (r < 0) {
                LOG("decode error %d at frame %llu", r, (unsigned long long)p->frames);
                st = kVTVideoDecoderBadDataErr;
                if (++p->failures > 8)
                    p->dead = 1;
            }
        }
        free(tmp);
    }
    p->frames++;
    if (st == noErr && got && (flags & kVTDecodeFrame_DoNotOutputFrame)) {
        // decode-only sample (the reader prerolls from a sync sample up to the start time): the picture stays a
        // reference in libavcodec's DPB, nothing is downloaded and the frame is reported dropped, as VT expects
        p->dno++;
        VTDecoderSessionEmitDecodedFrame(p->session, f, noErr, kVTDecodeInfo_FrameDropped, NULL);
        if (info)
            *info |= kVTDecodeInfo_FrameDropped;
    } else if (st == noErr && got) {
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(p->session);
        av_frame_unref(p->eng->sw);
        const CVReturn fr = pool ? fetch_pixbuf(p, pool, &pb) : kCVReturnInvalidArgument;
        if (fr == kCVReturnWouldExceedAllocationThreshold) {
            // every buffer of the bounded pool is still held downstream: drop this picture rather than add a surface
            p->pool_drops++;
            VTDecoderSessionEmitDecodedFrame(p->session, f, noErr, kVTDecodeInfo_FrameDropped, NULL);
            if (info)
                *info |= kVTDecodeInfo_FrameDropped;
            const unsigned dn = p->pool_drops;
            if (dn == 1 || dn == 10 || dn == 100 || dn == 1000 || dn == 10000)
                pool_report(p, "dropped");
            pthread_mutex_unlock(&p->lock);
            return noErr;
        }
        if (fr || !pb)
            st = kVTAllocationFailedErr;
        else {
            IOSurfaceRef tsf = CVPixelBufferGetIOSurface(pb);
            phx_holders_fetched(p, pb);
            if (tsf && IOSurfaceGetUseCount(tsf) > 1 && ++p->tr_inuse <= 12)
                note("trace pool surface %u still used elsewhere at fetch (use count %d) at picture %llu", IOSurfaceGetID(tsf),
                     IOSurfaceGetUseCount(tsf), (unsigned long long)p->emitted);
            if (p->emitted == 0 && tsf) {
                CFDictionaryRef all = IOSurfaceCopyAllValues(tsf);
                note_cf("first surface values", all);
                if (all)
                    CFRelease(all);
            }
            const uint64_t tw = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            wait_decoded(p->eng, p->eng->hw);
            const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            p->ns_wait += t1 - tw;
            int done = 0;
            if (p->eng->direct == 1) {
                const unsigned d0 = phx_copier_surfaces(p->eng->copier);
                const int cr = phx_copier_copy(p->eng->copier, p->eng->hw, pb);
                const unsigned d1 = phx_copier_surfaces(p->eng->copier);
                if (d1 > d0 && d1 > PHXDEC_POOL_MAX && ++p->pool_new_over <= 8) {
                    unsigned inuse = 0;
                    const unsigned cached = phx_copier_cached(p->eng->copier, &inuse);
                    note("trace new surface %u (distinct %u) at picture %llu: import cache %u, in use %u, pool hits %u",
                         IOSurfaceGetID(CVPixelBufferGetIOSurface(pb)), d1, (unsigned long long)p->emitted, cached, inuse, p->pool_hits);
                }
                if (d1 > d0 && (d1 == PHXDEC_POOL_MAX + 1 || d1 == 64 || d1 == 128 || d1 == 256))
                    pool_report(p, "distinct milestone");
                if (cr < 0) {
                    p->eng->direct = 2;
                    IOSurfaceRef sf = CVPixelBufferGetIOSurface(pb);
                    NOTE("cached GPU copy into the pixel buffer failed (%d; surface alloc %zu, align %zu): libavutil's "
                         "per-frame import from now on", cr, sf ? IOSurfaceGetAllocSize(sf) : 0, phx_copier_align(p->eng->copier));
                } else
                    done = 1;
            }
            if (!done && p->eng->direct == 2) {
                const int dr = transfer_into_pixbuf(p->eng, pb);
                if (dr < 0) {
                    p->eng->direct = 0;
                    NOTE("direct copy into the pixel buffer failed (%d): download + memcpy from now on", dr);
                } else
                    done = 1;
            }
            if (!done && av_hwframe_transfer_data(p->eng->sw, p->eng->hw, 0) < 0)
                st = kVTVideoDecoderMalfunctionErr;
            const uint64_t t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (st == noErr && !done)
                st = copy_to_pixbuf(p->eng->sw, pb);
            p->ns_transfer += t2 - t1;
            p->ns_copy += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t2;
        }
        if (st == noErr && p->emitted < 2 && !CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly)) {
            const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
            const size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0);
            const size_t w = CVPixelBufferGetWidthOfPlane(pb, 0);
            uint64_t sum = 0, cnt = 0;
            for (size_t r = h / 8; r < h; r += h / 4)
                for (size_t x = 0; x < w; x += 16, cnt++)
                    sum += p->bit_depth > 8 ? ((const uint16_t *)(y + r * bpr))[x] >> 6 : y[r * bpr + x];
            CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
            NOTE("picture %llu out: mean luma %llu (%s)", (unsigned long long)p->emitted,
                 cnt ? (unsigned long long)(sum / cnt) : 0ull, p->bit_depth > 8 ? "10-bit code" : "8-bit code");
        }
        int depth = p->eng->avctx->has_b_frames;
        if (depth > 15)
            depth = 15;
        if (flags & kVTDecodeFrame_EnableTemporalProcessing)
            p->temporal++;
        if (st == noErr && (flags & kVTDecodeFrame_EnableTemporalProcessing) && depth > 0) {
            p->q[p->nq].f = f;
            p->q[p->nq].pb = pb;   // the queue owns the reference now
            p->q[p->nq].pts = CMSampleBufferGetPresentationTimeStamp(sb).value;
            p->nq++;
            pb = NULL;
            while (p->nq > depth)
                emit_lowest(p);
            if (info)
                *info |= kVTDecodeInfo_Asynchronous;
        } else {
            if (st != noErr && ++p->tr_err <= 8)
                note("trace error emit %d at picture %llu", (int)st, (unsigned long long)p->emitted);
            VTDecoderSessionEmitDecodedFrame(p->session, f, st, 0, st == noErr ? pb : NULL);
            if (st == noErr && p->emitted == 0) {
                CFDictionaryRef at = CVBufferCopyAttachments(pb, kCVAttachmentMode_ShouldPropagate);
                note_cf("first picture attachments after emit", at);
                if (at)
                    CFRelease(at);
            }
        }
        if (pb)
            CFRelease(pb);
        p->emitted++;
        if (p->emitted % 1200 == 0)   // Invalidate is not called when the client is killed: keep the counters in the log
            pool_report(p, "periodic");
    } else if (st == noErr) {
        VTDecoderSessionEmitDecodedFrame(p->session, f, noErr, kVTDecodeInfo_FrameDropped, NULL);
        if (info)
            *info |= kVTDecodeInfo_FrameDropped;
    } else {
        if (++p->tr_err <= 8)
            note("trace error emit %d at sample %llu", (int)st, (unsigned long long)p->frames);
        VTDecoderSessionEmitDecodedFrame(p->session, f, st, 0, NULL);
    }
    pthread_mutex_unlock(&p->lock);
    return noErr;
}

static OSStatus CopySupportedPropertyDictionary(VTVideoDecoderRef d, CFDictionaryRef *out)
{
    (void)d;
    if (out)
        *out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    return noErr;
}
static OSStatus SetProperties(VTVideoDecoderRef d, CFDictionaryRef props)
{   // measurement: what the client asks for (ReducedFrameDelivery, RealTime, ...); every key is still ignored
    PhxDec *p = ST(d);
    pthread_mutex_lock(&p->lock);
    const unsigned n = p->setprops++;
    pthread_mutex_unlock(&p->lock);
    if (props && n < 8)
        note_cf("client SetProperties", props);
    return noErr;
}
static OSStatus CopySerializableProperties(VTVideoDecoderRef d, CFAllocatorRef a, CFDictionaryRef *out)
{
    (void)d;
    if (out)
        *out = CFDictionaryCreate(a, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    return noErr;
}
static Boolean CanAcceptFormatDescription(VTVideoDecoderRef d, CMFormatDescriptionRef fd)
{
    PhxDec *p = ST(d);
    pthread_mutex_lock(&p->lock);
    const Boolean ok = p->fd && CMFormatDescriptionEqual(p->fd, fd);
    pthread_mutex_unlock(&p->lock);
    NOTE("CanAcceptFormatDescription -> %d", (int)ok);
    return ok;
}
static OSStatus FinishDelayedFrames(VTVideoDecoderRef d)
{
    PhxDec *p = ST(d);
    pthread_mutex_lock(&p->lock);
    LOG("FinishDelayedFrames: %d queued", p->nq);
    p->finishes++;
    drain_queue(p, 1);
    pthread_mutex_unlock(&p->lock);
    return noErr;
}

static OSStatus Invalidate(CMBaseObjectRef o)
{
    PhxDec *p = ST(o);
    pthread_mutex_lock(&p->lock);
    NOTE("lifecycle: %llu frames (%u decode-only), temporal-flag %u, FinishDelayedFrames %u, held now %d (dropped at end), reorder depth %d",
         (unsigned long long)p->frames, p->dno, p->temporal, p->finishes, p->nq, p->eng && p->eng->avctx ? p->eng->avctx->has_b_frames : -1);
    NOTE("invalidate after %llu frames, %llu pictures; ms per frame: decode %.2f vcnwait %.2f transfer %.2f copy %.2f; surface imports %u, distinct surfaces %u",
        (unsigned long long)p->frames, (unsigned long long)p->emitted, p->frames ? p->ns_decode / 1e6 / p->frames : 0.0,
        p->emitted ? p->ns_wait / 1e6 / p->emitted : 0.0, p->emitted ? p->ns_transfer / 1e6 / p->emitted : 0.0, p->emitted ? p->ns_copy / 1e6 / p->emitted : 0.0,
        p->eng ? phx_copier_imports(p->eng->copier) : 0u, p->eng ? phx_copier_surfaces(p->eng->copier) : 0u);
    pool_report(p, "invalidate");
    note("trace summary: pts mismatches %u, pool surfaces in use at fetch %u, error emits %u, extra pictures %u, send EAGAIN %u",
         p->tr_ptsmis, p->tr_inuse, p->tr_err, p->eng ? p->eng->tr_extra : 0u, p->eng ? p->eng->tr_eagain : 0u);
    teardown(p);
    p->session = NULL;
    pthread_mutex_unlock(&p->lock);
    return noErr;
}
static void Finalize(CMBaseObjectRef o)
{
    PhxDec *p = ST(o);
    teardown(p);
    pthread_mutex_destroy(&p->lock);
}
static CFStringRef CopyDebugDescription(CMBaseObjectRef o)
{
    PhxDec *p = ST(o);
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("<PhxDecHEVC %p VCN4 frames %llu>"), (void *)o, (unsigned long long)p->frames);
}
static OSStatus CopyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef a, void *out)
{
    (void)o; (void)a;
    if (out && CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) {
        *(CFTypeRef *)out = CFRetain(kCFBooleanTrue);
        return noErr;
    }
    return kVTPropertyNotSupportedErr;
}

static const CMBaseClass kBase __attribute__((aligned(4))) = {
    1, sizeof(PhxDec), NULL, Invalidate, Finalize, CopyDebugDescription, CopyProperty, NULL, NULL, NULL};
// version 2: VideoToolbox calls finishDelayedFrames and canAcceptFormatDescription only from version 2 on (3 would make
// it read the tile-session slots); the reorder queue needs finishDelayedFrames
static const VTVideoDecoderClass kDec __attribute__((aligned(4))) = {
    2, StartSession, DecodeFrame, CopySupportedPropertyDictionary, SetProperties, CopySerializableProperties,
    CanAcceptFormatDescription, FinishDelayedFrames, {NULL, NULL, NULL}};
static const CMBaseVTable kVT = {NULL, &kBase, &kDec};

__attribute__((visibility("default"))) OSStatus PhxDecHEVC_CreateInstance(FourCharCode codec, CFAllocatorRef a, VTVideoDecoderRef *out)
{
    {   // who asks for a decoder: the codec and the first caller frames outside VideoToolbox's selection code
        void *bt[8];
        const int n = backtrace(bt, 8);
        char who[256] = "";
        for (int i = 1; i < n; i++) {
            Dl_info di;
            if (dladdr(bt[i], &di) && di.dli_sname) {
                size_t l = strlen(who);
                snprintf(who + l, sizeof(who) - l, "%s%s", l ? " < " : "", di.dli_sname);
            }
        }
        NOTE("factory %c%c%c%c: %s", FCC(codec), who);
    }
    if (!out)
        return kVTParameterErr;
    CMBaseObjectRef o = NULL;
    OSStatus r = CMDerivedObjectCreate(a, &kVT, VTVideoDecoderGetClassID(), &o);
    if (r || !o) {
        NOTE("factory: CMDerivedObjectCreate -> %d", (int)r);
        return r ? r : kVTAllocationFailedErr;
    }
    PhxDec *p = CMBaseObjectGetDerivedStorage(o);
    memset(p, 0, sizeof(*p));
    pthread_mutex_init(&p->lock, NULL);
    *out = (VTVideoDecoderRef)o;
    return noErr;
}

// Registers the decoder for 'hvc1' and 'hev1' in this process. Returns 0 or the first OSStatus.
__attribute__((visibility("default"))) OSStatus phxdec_register(void)
{
    // Blink A/B: with this flag the decoder is not registered in the client process, so VideoToolbox picks
    // the static PhxVideoDecoder.bundle and decodes remote in VTDecoderXPCService (the path that showed no blink)
    if (access("/Library/GPUBundles/nvmtl/phxdec-noinproc", F_OK) == 0) {
        note("register: in-process registration off (phxdec-noinproc)");
        return noErr;
    }
    int rating = 1000;
    CFNumberRef nr = CFNumberCreate(NULL, kCFNumberIntType, &rating);
    const void *k[] = {CFSTR("CMClassImplementationID"), CFSTR("VTRating"), CFSTR("VTIsHardwareAccelerated")};
    const void *v[] = {CFSTR("com.phx.videodecoder.hevc.vcn4"), nr, kCFBooleanTrue};
    CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus r = VTRegisterVideoDecoderWithInfo('hvc1', info, PhxDecHEVC_CreateInstance);
    const OSStatus r2 = VTRegisterVideoDecoderWithInfo('hev1', info, PhxDecHEVC_CreateInstance);
    CFRelease(info);
    OSStatus r3 = 1;
    if (access("/Library/GPUBundles/nvmtl/phxdec-av1", F_OK) == 0) {   // AV1 is opt-in until it has run in clients
        const void *va[] = {CFSTR("com.phx.videodecoder.av1.vcn4"), nr, kCFBooleanTrue};
        CFDictionaryRef ia = CFDictionaryCreate(NULL, k, va, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        r3 = VTRegisterVideoDecoderWithInfo('av01', ia, PhxDecHEVC_CreateInstance);
        CFRelease(ia);
        note("registered av01 -> %d", (int)r3);
    }
    CFRelease(nr);
    LOG("registered hvc1 -> %d, hev1 -> %d, av01 -> %d (1 = not enabled)", (int)r, (int)r2, (int)r3);
    return r ? r : r2;
}
