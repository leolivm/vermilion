// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT): WindowServer GPU present through the PhxBringup scanout selectors. Included at the end
   of nvmtl_vk.c (one translation unit with it: uses g_dev, g_queue, the pvk* table, nvmtl_submit_mutex, memtype,
   g_host_align). Diagnostics live in nvmtl_scan_diag.c, included in the middle so the order of definitions is unchanged. */
/*
 * WindowServer GPU present (PhxBringup scanout selectors 9..14, through RADV's radv_darwin_scanout_* exports).
 * The display pipe copied each WindowServer frame on the CPU, and could read one the GPU was still drawing: Apple's
 * family waits for a plane surface's last GPU write before its performTransaction, which never fires on our stack
 * (RE'd 2026-10-25). Here, once WindowServer's swap-preceding command buffer has finished on the GPU, its display
 * surface is copied on the GPU into one of three VRAM slots and HUBP0 flips to that slot at the next vblank, as
 * Cobalt64 does. The display pipe's own copies keep going to the console surface, which is off screen meanwhile.
 * Any failure gives the plane back and leaves this process on the CPU copy for good.
 */
#include "scan_l10r_spv.h"   /* glslangValidator -V --vn scan_l10r_spv scan_l10r.comp -o scan_l10r_spv.h */
struct nvmtl_scan_query { uint32_t h_active, v_active, h_total, v_total, pitch_px, hubp_format, sw_mode, otg, refresh_mhz, pix_clk_khz, dcc_en, flags;
                          uint64_t frame_count, console_mc, plane_mc, earliest_mc; uint32_t acquired, plane_w, plane_h, reserved; };
struct nvmtl_scan_slot { uint64_t mc, latched_frame; uint32_t used, flags, presents, latches; };
struct nvmtl_scan_status { uint32_t acquired, flags, front_slot, pending_slot; uint64_t frame_count, console_mc, plane_mc, earliest_mc;
                           uint64_t presents, latched, replaced, repeats, vupdates, latch_irq, latch_poll, refused, first_latch_ns, last_latch_ns;
                           uint64_t idle_ms, watchdog_restores, storm_trips, geom_refused; struct nvmtl_scan_slot slot[3]; };
static struct {
    int state;                                   /* 0 untried (or retrying), 1 presenting, -1 off for this process */
    int restarts; uint64_t retryAt;              /* restarts after the kernel took the plane back; no try before retryAt (ns) */
    int (*query)(VkDevice, struct nvmtl_scan_query *);
    int (*acquire)(VkDevice, uint64_t *, uint64_t *);
    int (*reg)(VkDevice, VkDeviceMemory, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t *, uint64_t *);
    int (*present)(VkDevice, uint32_t, uint64_t[3]);
    int (*status)(VkDevice, struct nvmtl_scan_status *);
    int (*release)(VkDevice, uint64_t[2]);
    uint32_t w, h, pitch, next, slotId[3];
    VkBuffer slot[3]; VkDeviceMemory slotMem[3];
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence;
    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline conv; VkDescriptorPool dp; VkDescriptorSet ds;   /* l10r -> 8-bit */
    struct { const void *ref; uint32_t sid; void *base; nvk_buffer buf; } src[8];
    unsigned srcNext;
    uint64_t presents, drops, copyUs;
    uint64_t waitUs, probeUs, flipUs, lastT, gapBin[5];   /* present split + present-to-present interval buckets (<8, <16, <33, <100, >=100 ms) */
    dispatch_source_t keepalive;
} g_scan;
static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;

static void nvmtl_scan_off_locked(const char *why)
{
    if (g_scan.state == 1 && g_scan.release) { uint64_t o[2] = { 0, 0 }; g_scan.release(g_dev, o);
        nvlog("ws gpu present: OFF (%s) - plane back on the console (restore %s), %llu presents, %llu dropped", why, o[0] ? "verified" : "NOT verified",
              (unsigned long long)g_scan.presents, (unsigned long long)g_scan.drops); }
    else nvlog("ws gpu present: OFF (%s)", why);
    if (g_scan.keepalive) { dispatch_source_cancel(g_scan.keepalive); g_scan.keepalive = NULL; }
    g_scan.state = -1;
}

/* Everything nvmtl_scan_start_locked made, after the plane was given back: the slot BOs must be freed before the next
   acquire (freeing a slot BO while a new acquisition holds the plane would release that one). The last copy has finished
   (each present waits for its fence). */
static void nvmtl_scan_teardown_locked(void)
{
    if (g_scan.cb) pvkFreeCommandBuffers(g_dev, g_scan.pool, 1, &g_scan.cb);
    if (g_scan.pool) pvkDestroyCommandPool(g_dev, g_scan.pool, NULL);
    if (g_scan.fence) pvkDestroyFence(g_dev, g_scan.fence, NULL);
    if (g_scan.conv) pvkDestroyPipeline(g_dev, g_scan.conv, NULL);
    if (g_scan.pl) pvkDestroyPipelineLayout(g_dev, g_scan.pl, NULL);
    if (g_scan.dp) pvkDestroyDescriptorPool(g_dev, g_scan.dp, NULL);
    if (g_scan.dsl) pvkDestroyDescriptorSetLayout(g_dev, g_scan.dsl, NULL);
    for (int k = 0; k < 3; k++) {
        if (g_scan.slot[k]) pvkDestroyBuffer(g_dev, g_scan.slot[k], NULL);
        if (g_scan.slotMem[k]) pvkFreeMemory(g_dev, g_scan.slotMem[k], NULL);
        g_scan.slot[k] = VK_NULL_HANDLE; g_scan.slotMem[k] = VK_NULL_HANDLE; g_scan.slotId[k] = 0;
    }
    for (int i = 0; i < 8; i++) {
        if (g_scan.src[i].buf.buf) nvmtl_vk_buffer_destroy(&g_scan.src[i].buf);
        if (g_scan.src[i].ref) CFRelease(g_scan.src[i].ref);
        memset(&g_scan.src[i], 0, sizeof g_scan.src[i]);
    }
    g_scan.cb = VK_NULL_HANDLE; g_scan.pool = VK_NULL_HANDLE; g_scan.fence = VK_NULL_HANDLE; g_scan.conv = VK_NULL_HANDLE;
    g_scan.pl = VK_NULL_HANDLE; g_scan.dp = VK_NULL_HANDLE; g_scan.ds = VK_NULL_HANDLE; g_scan.dsl = VK_NULL_HANDLE;
    g_scan.next = 0; g_scan.lastT = 0;
}
/* The kernel gave the plane back to the console (its watchdog: HUBP0 pointed elsewhere, as at the login-session switch,
   or 5 s without a call). That used to end GPU present for the life of WindowServer, so after login the desktop ran on
   the CPU copy (2026-10-26). Now everything is torn down and acquired again 2 s later, up to 50 times per process. */
static void nvmtl_scan_lost_locked(const char *why, const struct nvmtl_scan_status *st)
{
    if (st) nvlog("ws gpu present: plane lost (%s) - kernel: acquired %u, watchdog restores %llu, idle %llu ms, plane 0x%llx console 0x%llx, refused %llu",
                  why, st->acquired, (unsigned long long)st->watchdog_restores, (unsigned long long)st->idle_ms,
                  (unsigned long long)st->plane_mc, (unsigned long long)st->console_mc, (unsigned long long)st->refused);
    nvmtl_scan_off_locked(why);
    nvmtl_scan_teardown_locked();
    if (g_scan.restarts < 50) { g_scan.state = 0; g_scan.retryAt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 2000000000ull; }
}

/* the kernel restores the console after 5 s without a call: an idle desktop presents nothing, so Status keeps it */
static void nvmtl_scan_keepalive(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&g_scan_lock);
    if (g_scan.state == 1) { struct nvmtl_scan_status st; memset(&st, 0, sizeof st);
        if (g_scan.status(g_dev, &st) || !st.acquired) nvmtl_scan_lost_locked("the plane was taken back", &st); }
    pthread_mutex_unlock(&g_scan_lock);
}

static int nvmtl_scan_start_locked(uint32_t w, uint32_t h, uint32_t pitch)
{
    g_scan.query = dlsym(g_lib, "radv_darwin_scanout_query"); g_scan.acquire = dlsym(g_lib, "radv_darwin_scanout_acquire");
    g_scan.reg = dlsym(g_lib, "radv_darwin_scanout_register"); g_scan.present = dlsym(g_lib, "radv_darwin_scanout_present");
    g_scan.status = dlsym(g_lib, "radv_darwin_scanout_status"); g_scan.release = dlsym(g_lib, "radv_darwin_scanout_release");
    if (!g_scan.query || !g_scan.acquire || !g_scan.reg || !g_scan.present || !g_scan.status || !g_scan.release) { nvmtl_scan_off_locked("no scanout exports in the ICD"); return -1; }
    struct nvmtl_scan_query q;
    int r = g_scan.query(g_dev, &q);
    if (r || q.plane_w != w || q.plane_h != h || q.pitch_px * 4 != pitch) {
        nvlog("ws gpu present: plane %ux%u pitch %u vs surface %ux%u pitch %u (query %d)", q.plane_w, q.plane_h, q.pitch_px * 4, w, h, pitch, r);
        nvmtl_scan_off_locked("the display surface does not match the plane"); return -1; }
    uint64_t console = 0, frame = 0;
    if ((r = g_scan.acquire(g_dev, &console, &frame))) { nvlog("ws gpu present: acquire -> %d (debug.phxscan=1 and debug.phxfb_flip=0 needed)", r);
        /* a restart may find the console still busy (a flip pending): try again later */
        if (g_scan.restarts && g_scan.restarts < 50) { g_scan.restarts++; g_scan.retryAt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 2000000000ull; return -1; }
        g_scan.state = -1; return -1; }
    g_scan.state = 1;
    const VkDeviceSize size = (VkDeviceSize)pitch * h;
    for (int k = 0; k < 3; k++) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        if (pvkCreateBuffer(g_dev, &bci, NULL, &g_scan.slot[k]) != VK_SUCCESS) { nvmtl_scan_off_locked("slot vkCreateBuffer failed"); return -1; }
        VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, g_scan.slot[k], &mr);
        const int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)(mt < 0 ? 0 : mt) };
        if (mt < 0 || pvkAllocateMemory(g_dev, &mai, NULL, &g_scan.slotMem[k]) != VK_SUCCESS || pvkBindBufferMemory(g_dev, g_scan.slot[k], g_scan.slotMem[k], 0) != VK_SUCCESS) {
            nvmtl_scan_off_locked("slot memory failed"); return -1; }
        uint64_t mc = 0;
        if ((r = g_scan.reg(g_dev, g_scan.slotMem[k], 0, pitch, w, h, &g_scan.slotId[k], &mc))) { nvlog("ws gpu present: register %d -> %d", k, r);
            nvmtl_scan_off_locked("slot register failed"); return -1; }
    }
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qfam };
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if (pvkCreateCommandPool(g_dev, &cpci, NULL, &g_scan.pool) != VK_SUCCESS) { nvmtl_scan_off_locked("command pool failed"); return -1; }
    cbai.commandPool = g_scan.pool;
    if (pvkAllocateCommandBuffers(g_dev, &cbai, &g_scan.cb) != VK_SUCCESS || pvkCreateFence(g_dev, &fci, NULL, &g_scan.fence) != VK_SUCCESS) {
        nvmtl_scan_off_locked("command buffer failed"); return -1; }
    {   /* the l10r conversion pipeline: two storage buffers (surface, slot) and the pixel count / format as push constants */
        VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof scan_l10r_spv, scan_l10r_spv };
        VkShaderModule sm = VK_NULL_HANDLE;
        VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                              { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
        VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b };
        VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
        int ok = pvkCreateShaderModule(g_dev, &smci, NULL, &sm) == VK_SUCCESS && pvkCreateDescriptorSetLayout(g_dev, &dslci, NULL, &g_scan.dsl) == VK_SUCCESS;
        if (ok) { VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_scan.dsl, 1, &pcr };
                  ok = pvkCreatePipelineLayout(g_dev, &plci, NULL, &g_scan.pl) == VK_SUCCESS; }
        if (ok) { VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL }, g_scan.pl, VK_NULL_HANDLE, 0 };
                  ok = pvkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpci, NULL, &g_scan.conv) == VK_SUCCESS; }
        if (ok) ok = pvkCreateDescriptorPool(g_dev, &dpci, NULL, &g_scan.dp) == VK_SUCCESS;
        if (ok) { VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_scan.dp, 1, &g_scan.dsl };
                  ok = pvkAllocateDescriptorSets(g_dev, &dsai, &g_scan.ds) == VK_SUCCESS; }
        if (sm) pvkDestroyShaderModule(g_dev, sm, NULL);
        if (!ok) { nvmtl_scan_off_locked("the l10r conversion pipeline would not build"); return -1; }
    }
    g_scan.w = w; g_scan.h = h; g_scan.pitch = pitch;
    g_scan.keepalive = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (g_scan.keepalive) { dispatch_source_set_timer(g_scan.keepalive, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
        dispatch_set_context(g_scan.keepalive, NULL); dispatch_source_set_event_handler_f(g_scan.keepalive, nvmtl_scan_keepalive); dispatch_resume(g_scan.keepalive); }
    nvlog("ws gpu present: ON - %ux%u, console MC 0x%llx, three VRAM slots registered", w, h, (unsigned long long)console);
    return 0;
}

#include "nvmtl_scan_diag.c"
/* `base` is the display surface's own pages (WindowServer's process), `sid` its IOSurface id, `ref` the IOSurfaceRef (the
   cache retains it); call once the GPU work that
   drew the frame has finished. 0 presented, 1 dropped (no free slot), -1 off (the caller keeps the CPU copy's safeguards). */
int nvmtl_vk_scanout_present(const void *ref, uint32_t sid, void *base, uint32_t w, uint32_t h, uint32_t pitch, int l10r)
{
    if (nvmtl_vk_init() || !g_lib) return -1;
    pthread_mutex_lock(&g_scan_lock);
    int ret = -1;
    /* M7 S4b head gate (nvmtl_headmap_gate.c), before the start and geometry checks: another head's surface must never
       start the plane or trip "the display surface changed size" */
    { const int g = nvmtl_hm_gate_present(sid);
      if (g < 0) { nvmtl_scan_off_locked("head map stall"); goto out; }
      if (g == 0) { ret = 1; goto out; } }
    if (g_scan.state == 0 && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= g_scan.retryAt) {
        if (g_scan.retryAt) { g_scan.restarts++; nvlog("ws gpu present: restart %d after the plane was lost", g_scan.restarts); }
        nvmtl_scan_start_locked(w, h, pitch);
    }
    if (g_scan.state != 1) goto out;
    if (w != g_scan.w || h != g_scan.h || pitch != g_scan.pitch) { nvmtl_scan_off_locked("the display surface changed size"); goto out; }
    nvk_buffer *src = NULL;
    /* WindowServer frees its BGRA display surfaces and makes l10r ones that can get the same id at the same address.
       An entry matched by id and address alone then kept the old import: the freed surface's pages, still wired, so
       the slot showed its last BGRA frame read as l10r (red bands, 2026-10-25 14:05). The cache holds a reference to
       each surface, so none of them is freed (and its id or address reused) while its import is cached. */
    for (unsigned i = 0; i < 8; i++) {
        if (!g_scan.src[i].buf.buf || g_scan.src[i].sid != sid || g_scan.src[i].base != base) continue;
        if (g_scan.src[i].ref == ref) { src = &g_scan.src[i].buf; break; }
        static unsigned said; if (said++ < 8) nvlog("ws gpu present: surface %u at %p is a NEW surface with a cached id and address - old import dropped", sid, base);
        nvmtl_vk_buffer_destroy(&g_scan.src[i].buf);
        if (g_scan.src[i].ref) CFRelease(g_scan.src[i].ref);
        memset(&g_scan.src[i], 0, sizeof g_scan.src[i]);
    }
    if (!src) {
        const unsigned i = g_scan.srcNext++ % 8;
        if (g_scan.src[i].buf.buf) nvmtl_vk_buffer_destroy(&g_scan.src[i].buf);
        if (g_scan.src[i].ref) CFRelease(g_scan.src[i].ref);
        memset(&g_scan.src[i], 0, sizeof g_scan.src[i]);
        const size_t bytes = ((size_t)pitch * h + g_host_align - 1) & ~(size_t)(g_host_align - 1);
        if (nvmtl_vk_buffer_import_host(base, bytes, &g_scan.src[i].buf)) { nvmtl_scan_off_locked("the display surface would not import"); goto out; }
        g_scan.src[i].sid = sid; g_scan.src[i].base = base; g_scan.src[i].ref = ref ? CFRetain(ref) : NULL;
        src = &g_scan.src[i].buf;
    }
    struct nvmtl_scan_status st;
    memset(&st, 0, sizeof st);
    if (g_scan.status(g_dev, &st) || !st.acquired) { nvmtl_scan_lost_locked("the plane was taken back", &st); goto out; }
    uint32_t k = UINT32_MAX;
    for (uint32_t n = 0; n < 3 && k == UINT32_MAX; n++) { const uint32_t c = (g_scan.next + n) % 3; if (st.slot[g_scan.slotId[c]].flags & 4u) k = c; }
    if (k == UINT32_MAX) { g_scan.drops++; nvmtl_e0("D sid %u dropped", sid); ret = 1; goto out; }
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    nvmtl_e0("P sid %u present %llu slot %u", sid, (unsigned long long)g_scan.presents, k);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    /* a fresh command buffer per frame (the loader table has no vkResetCommandBuffer); the last one is done by now */
    if (g_scan.cb) pvkFreeCommandBuffers(g_dev, g_scan.pool, 1, &g_scan.cb);
    g_scan.cb = VK_NULL_HANDLE;
    { VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_scan.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
      if (pvkAllocateCommandBuffers(g_dev, &cbai, &g_scan.cb) != VK_SUCCESS) { g_scan.cb = VK_NULL_HANDLE; nvmtl_scan_off_locked("command buffer failed"); goto out; } }
    pvkBeginCommandBuffer(g_scan.cb, &bi);
    VkMemoryBarrier before = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT };
    pvkCmdPipelineBarrier(g_scan.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, NULL, 0, NULL);
    if (l10r) {
        VkDescriptorBufferInfo dbi[2] = { { (VkBuffer)src->buf, 0, (VkDeviceSize)pitch * h }, { g_scan.slot[k], 0, (VkDeviceSize)pitch * h } };
        VkWriteDescriptorSet wds[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_scan.ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi[0], NULL },
                                        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_scan.ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi[1], NULL } };
        pvkUpdateDescriptorSets(g_dev, 2, wds, 0, NULL);   /* the last frame's command buffer has finished: the set is free */
        const uint32_t pc[2] = { pitch / 4 * h, 1 };
        pvkCmdBindPipeline(g_scan.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_scan.conv);
        pvkCmdBindDescriptorSets(g_scan.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_scan.pl, 0, 1, &g_scan.ds, 0, NULL);
        pvkCmdPushConstants(g_scan.cb, g_scan.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        pvkCmdDispatch(g_scan.cb, (pc[0] + 255) / 256, 1, 1);
        VkMemoryBarrier after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT };
        pvkCmdPipelineBarrier(g_scan.cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1, &after, 0, NULL, 0, NULL);
    } else {
        VkBufferCopy rg = { 0, 0, (VkDeviceSize)pitch * h };
        pvkCmdCopyBuffer(g_scan.cb, (VkBuffer)src->buf, g_scan.slot[k], 1, &rg);
        VkMemoryBarrier after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT };
        pvkCmdPipelineBarrier(g_scan.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1, &after, 0, NULL, 0, NULL);
    }
    pvkEndCommandBuffer(g_scan.cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_scan.cb, 0, NULL };
    pvkResetFences(g_dev, 1, &g_scan.fence);
    pthread_mutex_lock(&nvmtl_submit_mutex);
    VkResult sr = g_state == 1 ? pvkQueueSubmit(g_queue, 1, &si, g_scan.fence) : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&nvmtl_submit_mutex);
    if (sr != VK_SUCCESS || pvkWaitForFences(g_dev, 1, &g_scan.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) { nvmtl_scan_off_locked("the copy did not complete"); goto out; }
    uint64_t o[3];
    const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    nvmtl_flick_probe(sid, base, w, h, pitch, l10r);
    const uint64_t t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (g_scan.present(g_dev, g_scan.slotId[k], o)) { struct nvmtl_scan_status ls; memset(&ls, 0, sizeof ls); g_scan.status(g_dev, &ls);
        nvmtl_scan_lost_locked("present refused", &ls); goto out; }
    { const uint64_t t3 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      g_scan.waitUs += (t1 - t0) / 1000; g_scan.probeUs += (t2 - t1) / 1000; g_scan.flipUs += (t3 - t2) / 1000;
      if (g_scan.lastT) { const uint64_t gap = (t3 - g_scan.lastT) / 1000000;
          g_scan.gapBin[gap < 8 ? 0 : gap < 16 ? 1 : gap < 33 ? 2 : gap < 100 ? 3 : 4]++; }
      g_scan.lastT = t3; }
    nvmtl_e0("Q sid %u flipped slot %u", sid, k);
    g_scan.next = k + 1;
    g_scan.copyUs += (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000;
    if (++g_scan.presents % 3000 == 0)
        nvlog("ws gpu present: %llu presents, %llu dropped, copy+present %llu us average", (unsigned long long)g_scan.presents,
              (unsigned long long)g_scan.drops, (unsigned long long)(g_scan.copyUs / g_scan.presents)),
        nvlog("ws gpu present split: copy+wait %llu us, flicker probe %llu us, flip %llu us average; present gaps <8ms %llu, <16 %llu, <33 %llu, <100 %llu, longer %llu",
              (unsigned long long)(g_scan.waitUs / g_scan.presents), (unsigned long long)(g_scan.probeUs / g_scan.presents),
              (unsigned long long)(g_scan.flipUs / g_scan.presents), (unsigned long long)g_scan.gapBin[0], (unsigned long long)g_scan.gapBin[1],
              (unsigned long long)g_scan.gapBin[2], (unsigned long long)g_scan.gapBin[3], (unsigned long long)g_scan.gapBin[4]);
    ret = 0;
out:
    pthread_mutex_unlock(&g_scan_lock);
    return ret;
}
void nvmtl_vk_scanout_stop(const char *why)
{
    pthread_mutex_lock(&g_scan_lock);
    if (g_scan.state == 1) nvmtl_scan_off_locked(why);
    pthread_mutex_unlock(&g_scan_lock);
}
int nvmtl_vk_scanout_on(void) { return g_scan.state == 1; }
