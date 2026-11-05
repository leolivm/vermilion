// SPDX-License-Identifier: MIT
// phx_classes.cpp: class declarations: PhxBringup (the service), PhxFBSquatter (holds the IOFramebuffer match, with
// its probe/start), IOAccelPhxNativeClient (the user client).
// Part of the PhxBringup.cpp translation unit (included there, in order); not compiled on its own.

class PhxBringup : public IOService {
    OSDeclareDefaultStructors(PhxBringup)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;
    // b79 (M6 step 2): "phxEdidRead" = one live EDID read for PhxAMDFB (PhxAccel), see edidFetch
    IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                  void *param3, void *param4) override;
    using IOService::callPlatformFunction;

    bool claimSession();
    void releaseSession();
    IOReturn readRegs(uint32_t first, uint32_t count, uint32_t *out);
    IOReturn vramRead(uint32_t byteOff, uint32_t count, uint32_t *out);
    IOReturn writeReg(uint32_t reg, uint32_t value, uint32_t *readback);
    IOReturn vramWrite(uint32_t byteOff, uint32_t value);
    IOReturn doorbellWrite(uint64_t byteOff, uint64_t value);
    IOReturn sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1);
    IOReturn smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter);
    IOReturn dalMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter);   // b75
    IOReturn pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]);
    IOReturn pspSubmit(uint32_t cmdId, uint64_t out[4]);
    IOReturn fwStage(uint32_t dstOffset, const void *bytes, uint32_t len);
    IOReturn loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]);
    IOReturn pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]);
    IOReturn pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]);
    bool debugEnabled() const { return debug; }
    uint64_t vramMcBase() const { return fbMcBase; }
    // E8: GPU execution. The userspace bring-up maps MES queues (compute = 0, gfx = 1) and hands them over (adoptQueue);
    // user BOs are then mapped into the VMID 8 page table and SUBMIT writes INDIRECT_BUFFERs into the queue's ring.
    IOReturn adoptQueue(const uint64_t in[6]);
    IOReturn claimAccel();
    bool gpuReady() const { return gq[0].ready || gq[1].ready; }
    // one GPUVM context (VMID 8..15) per client process: RADV instances allocate VAs independently
    uint32_t vmidAlloc();
    void vmidFree(uint32_t vmid, uint64_t lastSeq);
    IOReturn vmMap(uint32_t vmid, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size);
    IOReturn vmUnmap(uint32_t vmid, uint64_t va, uint64_t size);
    IOReturn gpuSubmit(uint32_t vmid, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seq);
    uint64_t gpuRetired() const { return gpuRetiredMask(~0u); }
    uint64_t gpuRetiredMask(uint32_t qmask) const;   // b72: only the queues in qmask (bit i = gq[i])
    uint32_t submitQueueBit(bool gfx, bool vcn) const;
    // scanout (selectors 9..14): one client at a time flips VRAM slots it fills on the GPU onto HUBP0
    IOReturn vmMapVram(uint32_t vmid, uint64_t va, uint64_t vramOff, uint64_t size);
    void scanQuery(phxn_scan_query *q);
    IOReturn scanAcquire(const void *who, uint64_t out[2]);
    IOReturn scanAllocSlot(const void *who, const phxn_scan_reg *r, uint32_t *slot, uint64_t *mc, uint64_t *vramOff);
    IOReturn scanPresent(const void *who, uint32_t slot, uint64_t out[3]);
    IOReturn scanStatus(const void *who, phxn_scan_status *st);
    IOReturn scanRelease(const void *who, uint64_t out[2]);   // also when `who` closes; idempotent
    bool scanHeldBy(const void *who) const { return scan.owner == who; }
    // b82 (M7 S2, phx_pipe2.cpp): register access for the pipe-1 engine, and one VRAM dword through MM_INDEX/MM_DATA
    uint32_t p2Rd(uint32_t reg) const;
    void p2Wr(uint32_t reg, uint32_t v) const;
    uint32_t mmIdxRead(uint64_t vramOff);
    void mmIdxWrite(uint64_t vramOff, uint32_t v);
    bool p2Ready() const { return mmio != nullptr; }
    uint64_t p2VramBytes() const { return fbSize; }

private:
    uint32_t rreg(uint32_t reg) const;
    void wreg(uint32_t reg, uint32_t value) const;
    // scanout state; scanLock guards it (the watchdog thread call takes it too)
    struct Scan {
        const void *owner;            // the client that acquired (kept after a watchdog restore until it releases/closes)
        bool acquired;                // HUBP0 may show a slot
        uint64_t consoleMc, fbBase, slotMc[PHXN_SCAN_MAX_SLOTS];
        bool slotUsed[PHXN_SCAN_MAX_SLOTS];
        uint32_t front, pending, pitchPx, planeH;
        uint64_t presents, latched, replaced, refused, restores, watchdogRestores, lastCallAbs, frameExt;
        uint32_t frameLast, slotPresents[PHXN_SCAN_MAX_SLOTS], slotLatches[PHXN_SCAN_MAX_SLOTS];
        uint64_t slotLatchedFrame[PHXN_SCAN_MAX_SLOTS], firstLatchNs, lastLatchNs;
    } scan{};
    IOLock *scanLock{nullptr};
    thread_call_t scanWatch{nullptr};
    uint64_t scanEarliest() const { return (static_cast<uint64_t>(rreg(kHubpreq0EarliestInuseHigh) & 0xffff) << 32) | rreg(kHubpreq0EarliestInuse); }
    uint64_t scanPlane() const { return (static_cast<uint64_t>(rreg(kHubpreq0PrimaryAddrHigh) & 0xffff) << 32) | rreg(kHubpreq0PrimaryAddr); }
    uint64_t scanFrameLocked();
    void scanPollLocked();
    void scanFlipLocked(uint64_t mc, bool immediate);
    bool scanRestoreLocked();
    static void scanWatchFire(thread_call_param_t p0, thread_call_param_t);
    uint32_t smuPoll() const;
    bool pspWaitTos(uint32_t timeoutMs) const;
    IOReturn pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]);
    void hdpFlush() const;
    void readFbLayout();
    void clearRing();

    IOPCIDevice *pci{nullptr};
    IOMemoryMap *mmio{nullptr};
    IOLock *smuLock{nullptr};
    // UMA carve-out ("VRAM"): GPU MC address range and the system physical address behind it.
    uint64_t fbMcBase{0}, fbSize{0}, fbPhys{0};
    // BAR0 framebuffer aperture: the CPU window into the first fbAperLen bytes of VRAM. It is an MMIO
    // region above system RAM (phys 0x8_2040_0000 here, 256 MiB), so CPU writes through it reach the
    // GPU's local memory and never the macOS heap. This is how the PSP ring is initialised.
    IOMemoryMap *fbAper{nullptr};
    IOMemoryMap *doorbellMap{nullptr}; // BAR2: doorbell aperture (2 MiB)
    IOBufferMemoryDescriptor *sysBuf{nullptr}; // debug: physically contiguous system memory for GPU VM experiments
    uint64_t fbAperPhys{0}, fbAperLen{0};
    // MMIO dword to write 0 to for an HDP flush (0 if the NBIO remap register was implausible).
    uint32_t hdpFlushDword{0};
    // PSP KM ring (debug selector 101): MC address once created, else 0. The GPCOM command + fence buffers
    // (selector 103) live just above the ring in VRAM; offsets are VRAM byte offsets, Mc the matching MC address.
    uint64_t ringMc{0}, ringOff{0};
    uint64_t cmdOff{0}, cmdMc{0}, fenceOff{0}, fenceMc{0};
    uint32_t fenceCounter{0};
    // Firmware bring-up regions (set when the ring is created): TMR the PSP copies firmware into, and fw_buf
    // where the CPU stages ucode for LOAD_IP_FW. Offsets are VRAM byte offsets, Mc the matching MC addresses.
    uint64_t tmrOff{0}, tmrMc{0}, tmrSize{0};
    uint64_t fwBufOff{0}, fwBufMc{0}, fwBufSize{0};
    bool tmrSetUp{false};
    // E8 state: the adopted queues (ring/wb/fence are VRAM byte offsets reached through BAR0) and the VMID 8
    // page-table pool (physically contiguous system memory; page 0 is the root). Sequence numbers are global across
    // the queues; each queue's fence holds the last sequence it finished, and fseq/fend keep its outstanding
    // submissions in order (sequence, ring position after it).
    struct GpuQueue {
        bool ready, gfx, vcn;   // vcn: the VCN0 unified ring (b67), 32-bit fence, WPTR by MMIO
        uint32_t ringOff, ringDw, wbOff, fenceOff;
        uint64_t dbByte, wptr, doneEnd;
        uint64_t fseq[64], fend[64];
        volatile uint32_t head, tail;
    };
    GpuQueue gq[3]{};
    uint64_t gSeq{0};
    uint64_t queueFence(const GpuQueue &q) const;
    IOLock *gpuLock{nullptr};
    // One GPUVM context (page tables) per client, and the eight hardware VMIDs lent to contexts at submission time,
    // as amdgpu does: a fixed VMID per client capped Metal at eight processes, and with every app allowed Metal the
    // ninth got no device (2026-10-20). A context's handle is its index + 1.
    // A context's page tables, in physically contiguous 64 KiB chunks allocated as it maps (most contexts need one or
    // two): a single 4 MiB contiguous pool per context failed once memory was fragmented. Table i is page i % 16 of
    // chunk i / 16; table 0 is the root.
    // b46: 4096 tables (was 1024: WindowServer full after ~30-55 min). b77: the context, the walk, map/unmap and the
    // empty-leaf reclaim (debug.phxpt_reclaim) live in PhxVmTables.h, shared with its host test
    static constexpr uint32_t kPtChunkBytes = phxvm::kChunkBytes, kPtChunkPages = phxvm::kChunkPages, kPtChunks = phxvm::kChunks;
    using VmCtx = phxvm::Ctx;             // navi31: chunk[] are CPU pointers into BAR0 (VRAM page-table pool)
    static constexpr uint32_t kMaxCtx = 96;
    VmCtx vm[kMaxCtx]{};
    // who owns each context, for the fault/stall diagnostics (set by the client in its own process context)
    int ctxPid[kMaxCtx]{};
    char ctxName[kMaxCtx][20]{};
public:
    void vmTag(uint32_t handle, int pid, const char *name) {
        if (handle && handle <= kMaxCtx) { ctxPid[handle - 1] = pid; strlcpy(ctxName[handle - 1], name, sizeof ctxName[0]); } }
    // the process whose context a hardware VMID points at, for a fault report
    const char *hwVmidOwner(uint32_t hwVmid, int *pid) const {
        *pid = 0;
        if (hwVmid < kFirstUserVmidPub || hwVmid >= kFirstUserVmidPub + 8) return "?";
        const uint32_t h = hwv[hwVmid - kFirstUserVmidPub].ctx;
        if (!h || h > kMaxCtx) return "none";
        *pid = ctxPid[h - 1]; return ctxName[h - 1]; }
    static constexpr uint32_t kFirstUserVmidPub = 8;
    // debug.phxshot: the surface HUBP0 is scanning out right now (SURFACE_EARLIEST_INUSE, so the A/B flip's front
    // buffer), read out of VRAM through BAR0: what the panel shows, for checking frames without a camera.
    // A 16-byte header ('PSHT', width, height, VRAM offset) then the rows, B8G8R8A8.
    int shot(struct sysctl_req *req) const {
        if (!mmio || !fbAper) return ENXIO;
        const uint64_t mc = (static_cast<uint64_t>(rreg(kHubpreq0EarliestInuseHigh) & 0xffff) << 32) | rreg(kHubpreq0EarliestInuse);
        const uint64_t fbBase = static_cast<uint64_t>(rreg(kDcnVmFbLocationBase) & 0xffffff) << 24;
        const uint32_t pitchPx = (rreg(kHubpreq0SurfacePitch) & 0x3fff) + 1, vb = rreg(kOtg0VBlankStartEnd);
        const uint32_t vbs = vb & 0x7fff, vbe = (vb >> 16) & 0x7fff;
        const uint32_t height = vbs > vbe ? vbs - vbe : 0;
        const uint64_t off = mc - fbBase, rowBytes = static_cast<uint64_t>(pitchPx) * 4;
        if (mc < fbBase || !height || height > 4320 || pitchPx > 8192 || off + rowBytes * height > fbAper->getLength())
            return EINVAL;
        uint32_t hdr[4] = { 0x54485350u, pitchPx, height, static_cast<uint32_t>(off) };
        if (!req->oldptr) return SYSCTL_OUT(req, nullptr, sizeof hdr + rowBytes * height);
        int err = SYSCTL_OUT(req, hdr, sizeof hdr);
        uint64_t *row = static_cast<uint64_t *>(IOMalloc(rowBytes));
        if (!row) return ENOMEM;
        const volatile uint64_t *src = reinterpret_cast<const volatile uint64_t *>(fbAper->getVirtualAddress() + off);
        for (uint32_t y = 0; !err && y < height; y++) {
            for (uint64_t x = 0; x < rowBytes / 8; x++) row[x] = src[y * (rowBytes / 8) + x];
            err = SYSCTL_OUT(req, row, rowBytes);
        }
        IOFree(row, rowBytes);
        return err;
    }
    // debug.phxedid (M6 step 1): the monitor's EDID read live over the side channel debug.phxedid_src names (PhxDdc.h:
    // 0x10 + n = DC_I2C DDC line n, 0x10 = DDC1; 0x20 + n = DP AUX engine n). A 32-byte header ('PEDI', source, rc,
    // length, SW_STATUS, ARB, DC_GPIO_HPD_Y, HPD0-4 sense bits | AUX defers << 8) then 256 bytes. Every read is one fresh
    // transfer; nothing is cached or published yet (step 2). Writes only the engine's own registers.
    volatile UInt32 edidBusy{0};
    int edidRead(struct sysctl_req *req, unsigned src) {
        enum { kHdr = 32, kOut = kHdr + 256 };
        if (!req->oldptr) return SYSCTL_OUT(req, nullptr, kOut);            // size query: no hardware access
        if (!mmio) return ENXIO;
        if (!OSCompareAndSwap(0, 1, &edidBusy)) return EBUSY;
        phxddc::Io io{this, [](void *c, uint32_t r) { return static_cast<PhxBringup *>(c)->rreg(r); },
                      [](void *c, uint32_t r, uint32_t v) { static_cast<PhxBringup *>(c)->wreg(r, v); },
                      [](void *, uint32_t us) { IODelay(us); }};
        uint8_t buf[kOut] = {};
        phxddc::Diag d{};
        unsigned len = 0;
        const int rc = phxddc::readEdid(io, src, buf + kHdr, &len, &d);
        uint32_t hpd = 0;
        for (unsigned n = 0; n < 5; n++) if (phxddc::hpdSensed(io, n)) hpd |= 1u << n;
        const uint32_t hdr[8] = {0x49444550u, src, static_cast<uint32_t>(rc), len, d.status, d.arb, rreg(phxddc::kGpioHpdY),
                                 hpd | (d.retries << 8)};
        memcpy(buf, hdr, sizeof hdr);
        OSCompareAndSwap(1, 0, &edidBusy);
        return SYSCTL_OUT(req, buf, kOut);
    }
    // b79 (M6 step 2): the same read for PhxAMDFB, through callPlatformFunction("phxEdidRead", false, out[256], &len,
    // &rc, &src). Source = debug.phxedid_src (default DDC1). Shares edidBusy with debug.phxedid: waits for it at most
    // ~100 ms, then gives up (kIOReturnBusy). The read itself is bounded by PhxDdc.h (~50 ms per block, 2 blocks).
    // Nothing is cached here: PhxAMDFB validates, caches and publishes what it serves.
    IOReturn edidFetch(uint8_t out[256], uint32_t *len, int32_t *rc, uint32_t src) {
        if (!out || !len || !rc) return kIOReturnBadArgument;
        *len = 0; *rc = phxddc::kErrArg;
        if (!mmio) return kIOReturnNotReady;
        bool got = false;
        for (unsigned i = 0; i < 20 && !(got = OSCompareAndSwap(0, 1, &edidBusy)); i++) IOSleep(5);
        if (!got) return kIOReturnBusy;
        phxddc::Io io{this, [](void *c, uint32_t r) { return static_cast<PhxBringup *>(c)->rreg(r); },
                      [](void *c, uint32_t r, uint32_t v) { static_cast<PhxBringup *>(c)->wreg(r, v); },
                      [](void *, uint32_t us) { IODelay(us); }};
        phxddc::Diag d{};
        unsigned n = 0;
        *rc = phxddc::readEdid(io, src, out, &n, &d);
        *len = n;
        OSCompareAndSwap(1, 0, &edidBusy);
        return kIOReturnSuccess;
    }
    // WP1 step 2a: the interrupt handler (IH) ring in system memory, polled from debug.phxih_stat (no MSI yet)
    IOReturn ihEnable();
    void ihDisable();
    int ihStat(char *b, size_t n);
    uint32_t ihDrain();   // caller holds gIhSpin; returns the IVs consumed
    // WP1 step 2b: the IH ring raises the MSI (debug.phxirq)
    IOReturn irqEnable();
    void irqDisable();
    int irqStat(char *b, size_t n);
    // WP6 B1: the VCN0 unified ring, brought up from userspace (vcn_up.sh) and adopted here
    int vcnCtl(int v);
    int vcnStat(char *b, size_t n);
    // WP6 B2: MMHUB VM translation for VCN (debug.phxmmvm) and VCN submissions
    int mmvmCtl(int v);
    int mmvmStat(char *b, size_t n);
    void mmhubFlush(uint32_t vmid) const;
    IOReturn gpuSubmitVcn(uint32_t handle, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seqOut);
    bool vcnSubmitReady() const;
    void irqFilter();
    // text dump of the GPU VM state for debug.phxvm
    int vmDump(char *b, size_t n) const {
        int o = snprintf(b, n, "pool chunks %u/%u; seq emitted %llu retired %llu; map fails %d (pool full %d, ctx cap %d); unmap/free timeouts %d; ctx closes %d retired-busy %d\n",
                         ptPoolNext, static_cast<uint32_t>(PHX_VRAM_PT_SIZE / kPtChunkBytes), gSeq, gpuRetired(), gVmMapFails, gVmPoolFull, gVmCtxCap,
                         gVmUnmapTimeouts, gVmCtxCloses, gVmCtxRetired);
        for (uint32_t i = 0; i < 8 && o > 0 && static_cast<size_t>(o) < n; i++) {
            const uint32_t h = hwv[i].ctx;
            if (!h || h > kMaxCtx) { o += snprintf(b + o, n - o, "VMID %u: free (last seq %llu)\n", 8 + i, hwv[i].lastSeq); continue; }
            const VmCtx &c = vm[h - 1];
            o += snprintf(b + o, n - o, "VMID %u: ctx %u pid %d %s tables %u/%u busy %d last seq %llu\n", 8 + i, h, ctxPid[h - 1], ctxName[h - 1], c.used, c.pages, c.busy, hwv[i].lastSeq);
        }
        uint32_t busy = 0, tables = 0;
        for (uint32_t i = 0; i < kMaxCtx; i++) if (vm[i].busy) { busy++; tables += vm[i].used; }
        if (o > 0 && static_cast<size_t>(o) < n)
            o += snprintf(b + o, n - o, "contexts busy %u, tables in use %u\nlast map failure: %s\n", busy, tables, gVmLastFail[0] ? gVmLastFail : "none");
        if (o > 0 && static_cast<size_t>(o) < n)
            o += snprintf(b + o, n - o, "BO MB: vis %lld hi %lld gtt %lld host %lld; TLB invalidation timeouts %d, longest wait %u us\n",
                          gBoBytes[0] >> 20, gBoBytes[1] >> 20, gBoBytes[2] >> 20, gBoBytes[3] >> 20, gVmInvTimeouts, gVmInvMaxUs);
        if (o > 0 && static_cast<size_t>(o) < n)
            o += snprintf(b + o, n - o, "BO create refused: vis %d hi %d gtt %d handles %d pages %d; VRAM placed as GTT (vis full) %d; last: %s\n",
                          gBoFails[0], gBoFails[1], gBoFails[2], gBoFails[3], gBoFails[4], gBoVisToGtt, gBoLastFail[0] ? gBoLastFail : "none");
        return o; }
private:
    uint32_t ptPoolNext{0};               // navi31: next free 64 KiB chunk of PHX_VRAM_PT_*
    struct HwVmid {
        uint32_t ctx;                     // handle of the context it points at, 0 = free
        uint64_t lastSeq, lastUse;        // its newest submission; when it was last bound or used
    };
    HwVmid hwv[8]{};
    uint64_t hwTick{0};
    uint32_t vmBind(uint32_t handle);
    VmCtx *vmCtx(uint32_t handle) { return handle && handle <= kMaxCtx && vm[handle - 1].busy ? &vm[handle - 1] : nullptr; }
    uint64_t ptTablePhys(VmCtx &c, uint32_t i);
    bool vmFlushTlb(uint32_t vmid) const;   // b77: false when the invalidation was not acknowledged
    // b77: CPU shadow of the page-table pool (valid-PTE bitmap and count per table, parked-leaf FIFO links),
    // allocated once at the first context, before any table exists; nullptr = no accounting and no reclaim
    phxvm::Shadow ptShadow{};
    void *ptShadowMem{nullptr};
    bool ptShadowTried{false};
    friend struct PhxVmEnv;
    struct PhxVmEnv vmEnv();
public:
    int ptStat(char *b, size_t n);
private:
    void ringPut(GpuQueue &q, uint32_t idx, uint32_t value);
    bool debug{false};
    bool smuUsed{false};
    volatile SInt32 sessionOpen{0};
};

OSDefineMetaClassAndStructors(PhxBringup, IOService)

// PhxFBSquatter: holds the IOFramebuffer match category on the GPU's IOPCIDevice so IONDRVFramebuffer (score 20000)
// never starts there, leaving PhxAMDFB (on the PhxDisplay nub) as the only framebuffer CoreDisplay can drive.
// Only BootKC kexts probe before IONDRV, hence it lives here and not in PhxAccel. One boot per arming:
// `nvram phxfb-squat=1`; probe removes the variable and commits NVRAM at once, so a hung boot comes back on IONDRV.
class PhxFBSquatter : public IOService {
    OSDeclareDefaultStructors(PhxFBSquatter)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PhxFBSquatter, IOService)

static bool takeSquatArming()
{
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return false;
    OSObject *o = opts->copyProperty("phxfb-squat");
    const bool armed = o != nullptr;
    bool stuck = false;
    OSSafeReleaseNULL(o);
    if (armed) {
        opts->removeProperty("phxfb-squat");
        if (OSString *why = OSString::withCString("phxsquat")) { opts->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", why); why->release(); }
        o = opts->copyProperty("phxfb-squat");
        stuck = o != nullptr;
        OSSafeReleaseNULL(o);
    }
    opts->release();
    IOLog("PhxFBSquatter: phxfb-squat %s\n", !armed ? "absent, IONDRV keeps the display" :
          stuck ? "STILL SET: removal refused, not squatting" : "taken and committed, squatting IOFramebuffer");
    return armed && !stuck;
}

IOService *PhxFBSquatter::probe(IOService *provider, SInt32 *score)
{
    if (PE_parse_boot_argn("-phxbringupoff", nullptr, 0) || !takeSquatArming()) return nullptr;
    if (score) *score = 30000;
    return this;
}

bool PhxFBSquatter::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    IOLog("PhxFBSquatter: holding IOFramebuffer on %s\n", provider->getName());
    return true;
}

class IOAccelPhxNativeClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelPhxNativeClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
    struct Bo {
        IOMemoryDescriptor *md;   // an IOBufferMemoryDescriptor, or the caller's own pages (host, prepared)
        uint64_t size;
        uint32_t domain;
        uint32_t bits;
        uint8_t *kva;             // the kernel's view of the BO; NULL for host imports, which the kernel never reads
        bool host;
        uint32_t scanSlot{0};     // scanout: slot k + 1 this BO is rebound onto (its GPU maps point at that VRAM), 0 = none
        uint64_t scanVram{0};
        uint64_t freeSeq{0};      // freed by the client, released once the GPU retires this seqno (b52); 0 = live
    };
    struct Map {
        bool used;
        uint32_t handle;
        uint64_t va, size, off;
        uint32_t flags;
        uint64_t unmapSeq;        // unmapped by the client, PTEs cleared once the GPU retires this seqno (b52); 0 = live
    };

    IOReturn doHello(IOExternalMethodArguments *a);
    IOReturn doQueryInfo(IOExternalMethodArguments *a);
    IOReturn doReadRegs(IOExternalMethodArguments *a);
    IOReturn doBoCreate(IOExternalMethodArguments *a);
    IOReturn boRefused(int cause, uint64_t size, uint64_t domains, uint64_t flags, IOReturn r);
    IOReturn doBoFree(IOExternalMethodArguments *a);
    IOReturn doBoImportHost(IOExternalMethodArguments *a);
    void boRelease(Bo &b);
    IOReturn doGemVa(IOExternalMethodArguments *a);
    IOReturn doCtx(IOExternalMethodArguments *a);
    IOReturn doSubmit(IOExternalMethodArguments *a);
    IOReturn doWaitSeq(IOExternalMethodArguments *a);
    IOReturn doDbgSmu(IOExternalMethodArguments *a);
    IOReturn doDbgPspRing(IOExternalMethodArguments *a);
    IOReturn doDbgVramRead(IOExternalMethodArguments *a);
    IOReturn doDbgPspSubmit(IOExternalMethodArguments *a);
    IOReturn doDbgFwStage(IOExternalMethodArguments *a);
    IOReturn doDbgLoadIpFw(IOExternalMethodArguments *a);
    IOReturn doDbgPspCtl(IOExternalMethodArguments *a);
    IOReturn doDbgPspRaw(IOExternalMethodArguments *a);
    IOReturn doDbgWriteReg(IOExternalMethodArguments *a);
    IOReturn doDbgVramWrite(IOExternalMethodArguments *a);
    IOReturn doDbgDoorbell(IOExternalMethodArguments *a);
    IOReturn doDbgSysMem(IOExternalMethodArguments *a);
    IOReturn doDbgAdoptQueue(IOExternalMethodArguments *a);
    IOReturn doDbgClaim(IOExternalMethodArguments *a);
    IOReturn doScan(uint32_t selector, IOExternalMethodArguments *a);
    IOReturn doSurfGpuMark(IOExternalMethodArguments *a);   // b95
    bool scanAllowed() const { return privileged || kauth_getuid() == 88; }   // root, or WindowServer (_windowserver)
    void scanUnbind();   // every rebound BO's maps back on its own pages
    void flushBos();
    Bo *findBo(uint32_t h);
    // b52 deferred release: a BO free or an unmap the GPU may still be using is recorded with the client's last seqno
    // and finished by reap() once that seqno retired, instead of sleeping on the GPU with the client lock held
    void finishFree(uint32_t h);
    void reap();
    bool waitRetired(uint64_t s, const char *why);
    bool settleOverlaps(uint64_t va, uint64_t size);
    uint32_t deferred{0};
    uint64_t reapedAt{0};
    bool ibCovered(uint64_t vaCanon, uint64_t bytes);
    void freeAll();

    PhxBringup *owner{nullptr};
    IOLock *lock{nullptr};
    bool privileged{false};
    bool hello{false};
    bool sessionClaimed{false};
    uint32_t vmid{0};        // this client's GPUVM context, taken on its first GPU mapping or submission
    uint64_t lastSeq{0};     // its newest submission: the context is released only once the GPU is past it
    bool ensureVmid() {
        if (vmid) return true;
        if (!(vmid = owner->vmidAlloc())) return false;
        char n[20] = {0}; proc_selfname(n, sizeof n); owner->vmTag(vmid, proc_selfpid(), n);
        return true; }
    // refused or failed GPU mappings leave VAs the GPU will fault on: say so, with the process (first 40 per client)
    int mapComplaints{0};
    bool leaked{false};      // closed while the GPU was not past its work: BOs kept (see clientClose)
    void mapComplain(const char *why, uint64_t va, uint64_t size, uint32_t op, uint32_t flags, IOReturn r) {
        if (mapComplaints++ >= 40) return;
        char n[20] = {0}; proc_selfname(n, sizeof n);
        IOLog("PhxBringup: GPU map %s: pid %d %s op %u flags 0x%x va 0x%llx size 0x%llx -> 0x%x\n", why, proc_selfpid(), n, op, flags, va, size, r); }
    // amdgpu clears a VA's page-table entries only after the fences of earlier work on that VM, and RADV frees and
    // unmaps BOs right after submitting work that uses them, relying on it. Unmapping at once let the GPU read a page
    // just cleared: a TCP read fault (GCVM_L2_PROTECTION_FAULT_STATUS 0x00b01136, VMID 11) wedged the gfx queue as
    // soon as the login session's apps started drawing (2026-10-24). Bounded like vmidFree (~2 s).
    bool waitGpuPastLast(const char *why) const {
        const uint64_t s = lastSeq;
        for (int i = 0; i < 2000 && owner->gpuRetired() < s; i++) IOSleep(1);
        const uint64_t d = owner->gpuRetired();
        if (d >= s) return true;
        OSIncrementAtomic(&gVmUnmapTimeouts);
        char n[20] = {0}; proc_selfname(n, sizeof n);
        IOLog("PhxBringup: %s: pid %d %s: GPU at %llu, client's last submission %llu after 2 s - left mapped\n", why, proc_selfpid(), n, d, s);
        return false; }
    Bo *bos{nullptr};      // PHXN_MAX_BOS entries, index = handle
    Map *maps{nullptr};    // kMaxMaps entries
    bool ctx[PHXN_MAX_CTX + 1]{};
    uint64_t visUsed{0}, hiUsed{0}, gttUsed{0}, hostUsed{0};
    task_t task{nullptr};
    uint64_t emitted{0};
    uint32_t qmask{0};   // b72: queues this client has submitted to (gpuRetiredMask bits)
    uint32_t pendFenceHandle{0}, pendFenceOffset{0};
    uint64_t pendFenceSeq{0};
};

OSDefineMetaClassAndStructors(IOAccelPhxNativeClient, IOUserClient)
