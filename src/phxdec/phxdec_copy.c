// SPDX-License-Identifier: MIT
// phxdec_copy: see phxdec_copy.h. The copy runs on the device's graphics queue (the kext runs every client IB on the
// gfx ring) and is synchronous: the caller gets the pixel buffer back once the GPU has written it.
// Synchronisation with libavutil follows its AVVkFrame contract: under lock_frame, wait on each image's timeline
// semaphore at sem_value, signal sem_value + 1, and leave layout / access as the copy left them.
#include "phxdec_copy.h"
#include <IOSurface/IOSurfaceRef.h>
#include <mach/vm_page_size.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/macros.h>

#define PHX_CACHE 32   // NeptuneOneWallpaper cycles ~30 surfaces of a 4K pool (25 MB each; the kext budgets 4 GiB of host imports per client)

typedef struct {
    IOSurfaceRef surf;   // retained while cached: its pages stay alive as long as the GPU can write them
    uint8_t *base;
    size_t size;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint64_t used;
} PhxImport;

struct PhxCopier {
    AVBufferRef *hwdev;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkDeviceSize align;
    VkPhysicalDeviceMemoryProperties memprops;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    PhxImport cache[PHX_CACHE];
    uint64_t tick;
    unsigned imports;
    uint32_t ids[256];   // distinct IOSurface IDs seen (measurement), and how many
    unsigned nids, nids_over;
#define F(n) PFN_##n n
    F(vkGetMemoryHostPointerPropertiesEXT); F(vkCreateBuffer); F(vkDestroyBuffer); F(vkGetBufferMemoryRequirements);
    F(vkAllocateMemory); F(vkFreeMemory); F(vkBindBufferMemory); F(vkCreateCommandPool); F(vkDestroyCommandPool);
    F(vkAllocateCommandBuffers); F(vkResetCommandBuffer); F(vkBeginCommandBuffer); F(vkEndCommandBuffer);
    F(vkCmdPipelineBarrier); F(vkCmdCopyImageToBuffer); F(vkQueueSubmit); F(vkCreateFence); F(vkDestroyFence);
    F(vkWaitForFences); F(vkResetFences); F(vkGetDeviceQueue);
#undef F
};

static AVHWDeviceContext *devctx(const PhxCopier *c) { return (AVHWDeviceContext *)c->hwdev->data; }

static void drop(PhxCopier *c, PhxImport *im)
{
    if (im->buf)
        c->vkDestroyBuffer(c->dev, im->buf, NULL);
    if (im->mem)
        c->vkFreeMemory(c->dev, im->mem, NULL);
    if (im->surf)
        CFRelease(im->surf);
    memset(im, 0, sizeof(*im));
}

void phx_copier_destroy(PhxCopier *c)
{
    if (!c)
        return;
    for (int i = 0; i < PHX_CACHE; i++)
        drop(c, &c->cache[i]);
    if (c->fence)
        c->vkDestroyFence(c->dev, c->fence, NULL);
    if (c->pool)
        c->vkDestroyCommandPool(c->dev, c->pool, NULL);
    av_buffer_unref(&c->hwdev);
    free(c);
}

unsigned phx_copier_imports(const PhxCopier *c) { return c ? c->imports : 0; }
size_t phx_copier_align(const PhxCopier *c) { return c ? (size_t)c->align : 0; }
unsigned phx_copier_surfaces(const PhxCopier *c) { return c ? c->nids + c->nids_over : 0; }
unsigned phx_copier_cached_ids(const PhxCopier *c, uint32_t *ids, unsigned max)
{
    unsigned n = 0;
    for (int i = 0; c && i < PHX_CACHE && n < max; i++)
        if (c->cache[i].surf)
            ids[n++] = IOSurfaceGetID(c->cache[i].surf);
    return n;
}
unsigned phx_copier_cached(const PhxCopier *c, unsigned *in_use)
{
    unsigned n = 0, u = 0;
    for (int i = 0; c && i < PHX_CACHE; i++)
        if (c->cache[i].surf) {
            n++;
            u += IOSurfaceIsInUse(c->cache[i].surf) != 0;
        }
    if (in_use)
        *in_use = u;
    return n;
}

PhxCopier *phx_copier_create(AVBufferRef *hwdev)
{
    const AVVulkanDeviceContext *vk = ((AVHWDeviceContext *)hwdev->data)->hwctx;
    int host = 0;
    for (int i = 0; i < vk->nb_enabled_dev_extensions; i++)
        host |= !strcmp(vk->enabled_dev_extensions[i], VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    int family = -1;
    for (int i = 0; i < vk->nb_qf && family < 0; i++)
        if (vk->qf[i].flags & VK_QUEUE_GRAPHICS_BIT)
            family = vk->qf[i].idx;
    if (!host || family < 0)
        return NULL;
    PhxCopier *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->hwdev = av_buffer_ref(hwdev);
    c->dev = vk->act_dev;
    c->family = (uint32_t)family;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vk->get_proc_addr(vk->inst, "vkGetDeviceProcAddr");
    PFN_vkGetPhysicalDeviceProperties2 gpp2 =
        (PFN_vkGetPhysicalDeviceProperties2)vk->get_proc_addr(vk->inst, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceMemoryProperties gmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)vk->get_proc_addr(vk->inst, "vkGetPhysicalDeviceMemoryProperties");
    if (!c->hwdev || !gdpa || !gpp2 || !gmp)
        goto fail;
    int ok = 1;
#define L(n) ok &= (c->n = (PFN_##n)gdpa(c->dev, #n)) != NULL
    L(vkGetMemoryHostPointerPropertiesEXT); L(vkCreateBuffer); L(vkDestroyBuffer); L(vkGetBufferMemoryRequirements);
    L(vkAllocateMemory); L(vkFreeMemory); L(vkBindBufferMemory); L(vkCreateCommandPool); L(vkDestroyCommandPool);
    L(vkAllocateCommandBuffers); L(vkResetCommandBuffer); L(vkBeginCommandBuffer); L(vkEndCommandBuffer);
    L(vkCmdPipelineBarrier); L(vkCmdCopyImageToBuffer); L(vkQueueSubmit); L(vkCreateFence); L(vkDestroyFence);
    L(vkWaitForFences); L(vkResetFences); L(vkGetDeviceQueue);
#undef L
    if (!ok)
        goto fail;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp};
    gpp2(vk->phys_dev, &pp);
    c->align = hp.minImportedHostPointerAlignment;
    gmp(vk->phys_dev, &c->memprops);
    if (!c->align)
        goto fail;
    c->vkGetDeviceQueue(c->dev, c->family, 0, &c->queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, c->family};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!c->queue || c->vkCreateCommandPool(c->dev, &pci, NULL, &c->pool) != VK_SUCCESS ||
        c->vkCreateFence(c->dev, &fci, NULL, &c->fence) != VK_SUCCESS)
        goto fail;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (c->vkAllocateCommandBuffers(c->dev, &cai, &c->cmd) != VK_SUCCESS)
        goto fail;
    return c;
fail:
    phx_copier_destroy(c);
    return NULL;
}

static int pick_type(const PhxCopier *c, uint32_t bits)
{
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < c->memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    for (uint32_t i = 0; i < c->memprops.memoryTypeCount; i++)
        if (bits & (1u << i))
            return (int)i;
    return -1;
}

// The cached import of surface s (mapped at base, size bytes), importing it on a miss; NULL with *why set.
static PhxImport *import_of(PhxCopier *c, IOSurfaceRef s, uint8_t *base, size_t size, int *why)
{
    PhxImport *lru = &c->cache[0];
    for (int i = 0; i < PHX_CACHE; i++) {
        PhxImport *im = &c->cache[i];
        if (im->surf == s && im->base == base && im->size == size) {
            im->used = ++c->tick;
            return im;
        }
        if (!im->surf || im->used < lru->used)
            lru = im;
        if (!im->surf)
            break;
    }
    *why = -3;
    if ((uintptr_t)base % c->align || size % c->align)
        return NULL;
    *why = -4;
    drop(c, lru);   // synchronous copies: the GPU is done with every cached buffer here
    VkMemoryHostPointerPropertiesEXT hpp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (c->vkGetMemoryHostPointerPropertiesEXT(c->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hpp) !=
            VK_SUCCESS || !hpp.memoryTypeBits)
        return NULL;
    *why = -5;
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE};
    PhxImport n = {0};
    if (c->vkCreateBuffer(c->dev, &bci, NULL, &n.buf) != VK_SUCCESS)
        return NULL;
    *why = -6;
    VkMemoryRequirements req;
    c->vkGetBufferMemoryRequirements(c->dev, n.buf, &req);
    const int type = pick_type(c, hpp.memoryTypeBits & req.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, (uint32_t)type};
    if (type < 0 || c->vkAllocateMemory(c->dev, &mai, NULL, &n.mem) != VK_SUCCESS ||
        c->vkBindBufferMemory(c->dev, n.buf, n.mem, 0) != VK_SUCCESS) {
        drop(c, &n);
        return NULL;
    }
    n.surf = (IOSurfaceRef)CFRetain(s);
    n.base = base;
    n.size = size;
    n.used = ++c->tick;
    *lru = n;
    c->imports++;
    return lru;
}

int phx_copier_copy(PhxCopier *c, const AVFrame *hw, CVPixelBufferRef pb)
{
    if (!c || !hw->hw_frames_ctx || CVPixelBufferGetPlaneCount(pb) != 2)
        return -1;
    AVHWFramesContext *fc = (AVHWFramesContext *)hw->hw_frames_ctx->data;
    const int texel = fc->sw_format == AV_PIX_FMT_P010 ? 2 : fc->sw_format == AV_PIX_FMT_NV12 ? 1 : 0;
    IOSurfaceRef s = CVPixelBufferGetIOSurface(pb);
    if (!texel || !s || CVPixelBufferLockBaseAddress(pb, 0))
        return -1;
    {
        const uint32_t id = IOSurfaceGetID(s);
        unsigned k = 0;
        while (k < c->nids && c->ids[k] != id)
            k++;
        if (k == c->nids) {
            if (c->nids < 256)
                c->ids[c->nids++] = id;
            else
                c->nids_over++;
        }
    }
    int r = -2;
    uint8_t *base = IOSurfaceGetBaseAddress(s);
    // a mapping covers whole pages: rounding the size up to the page is still this surface's own mapping
    size_t size = IOSurfaceGetAllocSize(s);
    if (c->align <= vm_page_size)
        size = (size + c->align - 1) / c->align * c->align;
    PhxImport *im = base ? import_of(c, s, base, size, &r) : NULL;
    if (!im)
        goto out;
    r = -7;
    const uint32_t w = (uint32_t)FFMIN((size_t)hw->width, CVPixelBufferGetWidth(pb));
    const uint32_t h = (uint32_t)FFMIN((size_t)hw->height, CVPixelBufferGetHeight(pb));
    VkBufferImageCopy reg[2];
    for (int p = 0; p < 2; p++) {
        const uint8_t *pl = CVPixelBufferGetBaseAddressOfPlane(pb, p);
        const size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, p);
        const uint32_t bpt = (uint32_t)texel * (p ? 2u : 1u);   // plane 1 is interleaved CbCr
        const uint32_t pw = p ? (w + 1) / 2 : w, ph = p ? (h + 1) / 2 : h;
        if (pl < base || bpr % bpt || bpr / bpt < pw || (size_t)(pl - base) + bpr * ph > im->size)
            goto out;
        reg[p] = (VkBufferImageCopy){.bufferOffset = (VkDeviceSize)(pl - base),
                                     .bufferRowLength = (uint32_t)(bpr / bpt),
                                     .bufferImageHeight = ph,
                                     .imageSubresource = {0, 0, 0, 1},
                                     .imageExtent = {pw, ph, 1}};
    }
    AVVulkanFramesContext *vfc = fc->hwctx;
    AVVkFrame *f = (AVVkFrame *)hw->data[0];
    int nimg = 0;
    while (nimg < AV_NUM_DATA_POINTERS && f->img[nimg])
        nimg++;
    r = -8;
    if (nimg != 1 && nimg != 2)
        goto out;
    vfc->lock_frame(fc, f);
    VkImageMemoryBarrier ib[2];
    VkSemaphore sems[2];
    uint64_t wait[2], sig[2];
    VkPipelineStageFlags stages[2];
    for (int i = 0; i < nimg; i++) {
        ib[i] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, f->access[i],
                                       VK_ACCESS_TRANSFER_READ_BIT, f->layout[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, f->img[i],
                                       {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (nimg == 1)
            ib[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
        sems[i] = f->sem[i];
        wait[i] = f->sem_value[i];
        sig[i] = f->sem_value[i] + 1;
        stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    reg[0].imageSubresource.aspectMask = nimg == 1 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    reg[1].imageSubresource.aspectMask = nimg == 1 ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im->buf, 0,
                                VK_WHOLE_SIZE};
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkResult vr = c->vkResetCommandBuffer(c->cmd, 0);
    if (vr == VK_SUCCESS)
        vr = c->vkBeginCommandBuffer(c->cmd, &bi);
    if (vr == VK_SUCCESS) {
        c->vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                0, NULL, (uint32_t)nimg, ib);
        for (int p = 0; p < 2; p++)
            c->vkCmdCopyImageToBuffer(c->cmd, f->img[nimg == 1 ? 0 : p], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->buf, 1,
                                      &reg[p]);
        c->vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb,
                                0, NULL);
        vr = c->vkEndCommandBuffer(c->cmd);
    }
    if (vr == VK_SUCCESS)
        vr = c->vkResetFences(c->dev, 1, &c->fence);
    if (vr == VK_SUCCESS) {
        VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, (uint32_t)nimg, wait,
                                            (uint32_t)nimg, sig};
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, (uint32_t)nimg, sems, stages, 1, &c->cmd,
                           (uint32_t)nimg, sems};
        const AVVulkanDeviceContext *vk = devctx(c)->hwctx;
        vk->lock_queue(devctx(c), c->family, 0);
        vr = c->vkQueueSubmit(c->queue, 1, &si, c->fence);
        vk->unlock_queue(devctx(c), c->family, 0);
    }
    if (vr == VK_SUCCESS)
        for (int i = 0; i < nimg; i++) {
            f->sem_value[i]++;
            f->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            f->access[i] = VK_ACCESS_TRANSFER_READ_BIT;
        }
    vfc->unlock_frame(fc, f);
    if (vr == VK_SUCCESS)
        vr = c->vkWaitForFences(c->dev, 1, &c->fence, VK_TRUE, UINT64_MAX);
    r = vr == VK_SUCCESS ? 0 : -9;
out:
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return r;
}
