// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
/* Our code (Vermilion, RX 7900 XT), included by NVMTLEventSync.m at this exact place (one translation unit with the
   plugin; definitions keep their order): NVMTL_SURF_HOLD (WindowServer experiment) and the commit-wait flag. */
// set around commitAndWaitUntilSubmitted's commit (same thread as the commit's seal): WindowServer waits on this commit
static __thread BOOL nvmtl_probe_waitcommit;
// NVMTL_SURF_HOLD=1: every IOSurface a part reads or writes is kept in use (IOSurfaceIncrementUseCount) from seal until the
// GPU has finished the part. On Apple's drivers the kernel's IOAccelerator does this for the resources of each command
// buffer in flight, and CoreAnimation draws into a backing store with the CPU again only once IOSurfaceIsInUse is false;
// nothing held it here, so a client could rewrite a text bitmap while a blit still had to read it (garbled glyphs in
// Terminal and the desktop widgets, 2026-10-25; WindowServer: 73% of surface reads found the surface free at seal).
static int nvmtl_surf_hold_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SURF_HOLD"); on = e && e[0] == '1';
        if (on) nvlog("surface hold: IOSurfaces stay in use while GPU work that reads or writes them is in flight (NVMTL_SURF_HOLD)"); }
    return on;
}
static void nvmtl_surf_hold_add(NVMTLSubmissionPart *part, NVMTLTexture *t) {
    IOSurfaceRef sf = t->_surf ? t->_surf : (t->_parent ? t->_parent->_surf : NULL);
    if (!sf) return;
    if (!part->holdSurfs) part->holdSurfs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (CFArrayContainsValue(part->holdSurfs, CFRangeMake(0, CFArrayGetCount(part->holdSurfs)), sf)) return;
    CFArrayAppendValue(part->holdSurfs, sf);
    IOSurfaceIncrementUseCount(sf);
}
static void nvmtl_surf_hold_seal(NVMTLSubmissionPart *part) {
    if (!nvmtl_surf_hold_on()) return;
    if ([part->resources isKindOfClass:[NVMTLResSet class]])
        for (id o in ((NVMTLResSet *)part->resources)->_s) if ([o isKindOfClass:[NVMTLTexture class]]) nvmtl_surf_hold_add(part, o);
    for (id o in part->surfaces) if ([o isKindOfClass:[NVMTLTexture class]]) nvmtl_surf_hold_add(part, o);
}
static void nvmtl_surf_hold_release(NVMTLSubmissionPart *part) {
    if (!part->holdSurfs) return;
    for (CFIndex i = 0; i < CFArrayGetCount(part->holdSurfs); i++) IOSurfaceDecrementUseCount((IOSurfaceRef)CFArrayGetValueAtIndex(part->holdSurfs, i));
    CFRelease(part->holdSurfs); part->holdSurfs = NULL;
}
