// SPDX-License-Identifier: MIT
// submit_test: one compute submission through PhxBringup's RADV-facing ABI (no RADV needed).
// HELLO -> CTX alloc -> 2 GTT BOs (IB, target) -> CPU map -> GEMVA map -> IB = WRITE_DATA(target VA) -> SUBMIT -> WAITSEQ.
// Needs a kext-adopted compute queue (`mesboot cs mc ADOPT` after navi31_up.sh).
//
// Fault-default probes (kext b126 debug.phxfaultdef, H-FAULTDEF tests FD-1 / FD-2), run as root so the fault registers
// can be read (READREGS is privileged). The IB page and the target page are mapped at 0x100000000 and 0x100010000, so
// the default probe VA 0x100008100 lies in the same leaf table with PTE 0: a hole in a live leaf, as the 11-12 fault.
//   submit_test fd1 [expect=0xFDFDFDFD]  COPY_DATA (src TC_L2 memory, dst memory, write confirm; the encoding mesboot's
//                                        COPY_DATA tests use) from the unmapped VA into target[0], then a WRITE_DATA
//                                        marker into target[1]. PASS = fence retired, target[0] = expect (the
//                                        debug.phxfaultdef_fill pattern), target[1] = the marker.
//   submit_test fd2                      WRITE_DATA 0xC0FFEE01 to the unmapped VA, then the marker. PASS = fence retired,
//                                        the marker, and debug.phxfaultdef_stat shows 0xC0FFEE01 at the VA's page offset.
//   submit_test fd3                      FD-3, the SHADER path (UTCL2 client TCP, as the 11-12 hang): one gfx11 compute wave
//                                        (fd3_shader.h) does global_store_b32 0xC0FFEE03 to the unmapped VA (default
//                                        0x100008140), s_waitcnt_vscnt 0, then global_store_b32 the marker 0x5EED0003
//                                        into target[1]. PASS = fence retired, the marker, debug.phxfaultdef_stat shows
//                                        0xC0FFEE03 at the VA's page offset (+0x140), STATUS CID 8 RW 1 at the VA's page.
//   submit_test fd3ok                    the no-fault control of fd3: the same shader, the first store into target[2]
//                                        (mapped). Needs no gate. PASS = fence retired, target[2] = 0xC0FFEE03, the
//                                        marker. Run it first: it proves a hand-made wave runs on this queue at all.
//   options: va=0x... (the unmapped VA), timeout_ms=N (<= 2000), dry (print the IB, submit nothing), force (submit even
//            with debug.phxfaultdef off or CNTL not 0x1FFE: the gate-off control run, expected to hang GFX), gfx
//            (fd3/fd3ok: submit as a GFX IB, the queue of the 11-12 hang, instead of COMPUTE)
// All print the target dwords, the fence result, the fault STATUS / ADDR / CNTL / DEFAULT_ADDR, GRBM_STATUS polled for
// GUI_ACTIVE 0 over 1 s, and debug.phxfaultdef_stat before and after. Exit 0 = PASS, 2 = wrong data, 3 = fence not
// retired, 4 = refused (gate off), 1 = setup error. fd3 exits 2 also when the data is right but STATUS does not show
// the TCP write fault at the probe page.
// build: clang -arch x86_64 -O2 -o submit_test submit_test.c -framework IOKit -framework CoreFoundation
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <assert.h>   // static_assert in PhxNativeABI.h
#include "../../src/vermilion-boot/PhxNativeABI.h"
#include "fd3_shader.h"

static io_connect_t c;

static uint32_t bo_create(uint64_t size, uint64_t *mapped)
{
   struct phxn_gem_create_in in = {size, 4096, PHX_AMDGPU_GEM_DOMAIN_GTT, 0};
   uint64_t out[4] = {0}; uint32_t n = 4;
   kern_return_t kr = IOConnectCallMethod(c, PHXN_SEL_BOCREATE, NULL, 0, &in, sizeof in, out, &n, NULL, NULL);
   if (kr) { printf("BOCREATE kr 0x%x\n", kr); return 0; }
   mach_vm_address_t a = 0; mach_vm_size_t s = 0;
   kr = IOConnectMapMemory64(c, (uint32_t)out[0], mach_task_self(), &a, &s, kIOMapAnywhere);
   if (kr) { printf("map BO %llu kr 0x%x\n", (unsigned long long)out[0], kr); return 0; }
   *mapped = a;
   return (uint32_t)out[0];
}

static int va_map(uint32_t h, uint64_t va, uint64_t size)
{
   struct phxn_gem_va v; memset(&v, 0, sizeof v);
   v.handle = h; v.operation = PHX_AMDGPU_VA_OP_MAP; v.flags = 0xe; v.va_address = va; v.map_size = size;
   kern_return_t kr = IOConnectCallMethod(c, PHXN_SEL_GEMVA, NULL, 0, &v, sizeof v, NULL, NULL, NULL, NULL);
   if (kr) printf("GEMVA map 0x%llx kr 0x%x\n", (unsigned long long)va, kr);
   return kr;
}

// GC registers through READREGS (root only); 0 on success
static int regs(uint32_t first, uint32_t count, uint32_t *out)
{
   uint64_t in[3] = {first, count, 0xFFFFFFFFull};
   size_t sz = count * 4;
   return IOConnectCallMethod(c, PHXN_SEL_READREGS, in, 3, NULL, 0, NULL, NULL, out, &sz) ? -1 : 0;
}

static void stat_line(const char *when, char *buf, size_t n)
{
   size_t len = n; int on = -1; size_t ol = sizeof on;
   if (sysctlbyname("debug.phxfaultdef", &on, &ol, NULL, 0)) on = -1;
   if (sysctlbyname("debug.phxfaultdef_stat", buf, &len, NULL, 0)) snprintf(buf, n, "(no debug.phxfaultdef_stat: kext before b126?)");
   printf("%s: debug.phxfaultdef=%d\n  stat: %s\n", when, on, buf);
}

// fills sto[3] = STATUS, ADDR LO, ADDR HI when given; returns 0 when the registers were read
static int fault_regs(const char *when, uint64_t probe_va, uint32_t *sto)
{
   uint32_t cntl = 0, def[2] = {0}, st[3] = {0};
   if (regs(0x2824, 1, &cntl) || regs(0x282b, 2, def) || regs(0x2828, 3, st)) {
      printf("%s: READREGS refused (run as root)\n", when);
      return -1;
   }
   if (sto) { sto[0] = st[0]; sto[1] = st[1]; sto[2] = st[2]; }
   const uint64_t fva = ((((uint64_t)(st[2] & 0xF)) << 32) | st[1]) << 12;
   printf("%s: CNTL 0x%08x DEFAULT_ADDR 0x%x_%08x; STATUS 0x%08x (VMID %u CID %u RW %u mapping %u) ADDR VA 0x%llx%s\n", when,
          cntl, def[1], def[0], st[0], (st[0] >> 20) & 0xF, (st[0] >> 9) & 0x1FF, (st[0] >> 18) & 1, (st[0] >> 8) & 1,
          (unsigned long long)fva, st[0] && fva == (probe_va & ~0xFFFull) ? " = the probe VA's page" : "");
   return 0;
}

static void grbm_poll(void)
{
   uint32_t g = 0;
   for (int ms = 0; ms <= 1000; ms += 10) {
      if (regs(0x2004, 1, &g)) { printf("GRBM_STATUS: READREGS refused\n"); return; }
      if (!(g >> 31)) { printf("GRBM_STATUS 0x%08x: GUI_ACTIVE 0 after %d ms\n", g, ms); return; }
      usleep(10000);
   }
   printf("GRBM_STATUS 0x%08x: GUI_ACTIVE still 1 after 1 s (other clients may keep GFX busy: compare with the desktop idle)\n", g);
}

int main(int argc, char **argv)
{
   int mode = 0, dry = 0, force = 0, gfx = 0, va_set = 0;
   uint32_t expect = 0xFDFDFDFDu, timeout_ms = 2000;
   uint64_t hole = 0x100008100ull;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "fd1")) mode = 1;
      else if (!strcmp(argv[i], "fd2")) mode = 2;
      else if (!strcmp(argv[i], "fd3")) mode = 3;
      else if (!strcmp(argv[i], "fd3ok")) mode = 4;
      else if (!strcmp(argv[i], "gfx")) gfx = 1;
      else if (!strcmp(argv[i], "dry")) dry = 1;
      else if (!strcmp(argv[i], "force")) force = 1;
      else if (!strncmp(argv[i], "expect=", 7)) expect = (uint32_t)strtoul(argv[i] + 7, NULL, 0);
      else if (!strncmp(argv[i], "va=", 3)) { hole = strtoull(argv[i] + 3, NULL, 0); va_set = 1; }
      else if (!strncmp(argv[i], "timeout_ms=", 11)) timeout_ms = (uint32_t)strtoul(argv[i] + 11, NULL, 0);
      else { printf("usage: submit_test [fd1|fd2|fd3|fd3ok] [expect=0x..] [va=0x..] [timeout_ms=N] [dry] [force] [gfx]\n"); return 1; }
   }
   if (gfx && mode < 3) { printf("gfx: fd3 / fd3ok only\n"); return 1; }
   if (mode == 3 && !va_set) hole = 0x100008140ull;   // FD-3: the leaf and page of fd1/fd2, its own dword (+0x140)
   if (timeout_ms > 2000) timeout_ms = 2000;
   const uint64_t IBVA = 0x100000000ull, TGVA = 0x100010000ull;
   if (mode && ((hole & 3) || (hole >= IBVA && hole < IBVA + 4096) || (hole >= TGVA && hole < TGVA + 4096))) {
      printf("va=0x%llx: must be dword aligned and outside the two mapped pages\n", (unsigned long long)hole);
      return 1;
   }

   io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
   if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { printf("open failed\n"); return 1; }
   uint64_t hin[2] = {PHXN_ABI_VERSION, PHXN_HELLO_F_MINOR}, hout[4]; uint32_t hn = 4;
   kern_return_t kr = IOConnectCallScalarMethod(c, PHXN_SEL_HELLO, hin, 2, hout, &hn);
   printf("hello kr 0x%x build %llu pid %d\n", kr, (unsigned long long)hout[1], getpid());
   if (kr) return 1;

   char sbuf[1024];
   uint32_t st_before[3] = {0}, st_after[3] = {0};
   if (mode) {
      stat_line("before", sbuf, sizeof sbuf);
      fault_regs("before", hole, st_before);
      uint32_t cntl = 0;
      int on = 0; size_t ol = sizeof on;
      if (sysctlbyname("debug.phxfaultdef", &on, &ol, NULL, 0)) on = 0;
      if (regs(0x2824, 1, &cntl)) cntl = 0;
      if (mode != 4 && (on != 1 || cntl != 0x1FFEu) && !force && !dry) {
         printf("REFUSED: debug.phxfaultdef=%d CNTL 0x%08x (want 1 / 0x00001ffe); a fault with the gate off is expected to hang GFX. 'force' overrides.\n", on, cntl);
         return 4;
      }
   }

   struct phxn_ctx ci = {PHX_AMDGPU_CTX_OP_ALLOC_CTX, 0, 0, 0}, co; size_t cs = sizeof co;
   kr = IOConnectCallStructMethod(c, PHXN_SEL_CTX, &ci, sizeof ci, &co, &cs);
   printf("ctx kr 0x%x id %u\n", kr, co.op);
   if (kr) return 1;

   uint64_t ibp = 0, tgp = 0;
   uint32_t ib = bo_create(4096, &ibp), tg = bo_create(4096, &tgp);
   if (!ib || !tg) return 1;
   if (va_map(ib, IBVA, 4096) || va_map(tg, TGVA, 4096)) return 1;

   volatile uint32_t *t = (volatile uint32_t *)(uintptr_t)tgp;
   uint32_t *p = (uint32_t *)(uintptr_t)ibp;
   uint32_t n = 0;
   const uint64_t MARK = TGVA + 4;
   if (mode >= 3) {
      const uint64_t SHVA = IBVA + 0x800, store = mode == 3 ? hole : TGVA + 8;
      t[0] = 0x5EED5EEDu; t[1] = 0; t[2] = 0x5EED5EEDu;
      uint32_t *sh = p + 0x200, shn = fd3_shader(sh);
      n = fd3_ib(p, SHVA, store, MARK);
      printf("%s: shader VA 0x%llx, store VA 0x%llx (page offset 0x%llx), marker VA 0x%llx, ip %s\n",
             mode == 3 ? "FD-3 shader store to the unmapped VA" : "FD-3 control (store to target[2], mapped)",
             (unsigned long long)SHVA, (unsigned long long)store, (unsigned long long)(store & 0xFFF), (unsigned long long)MARK,
             gfx ? "GFX" : "COMPUTE");
      printf("shader (%u dw):", shn);
      for (uint32_t i = 0; i < shn; i++) printf(" %08x", sh[i]);
      printf("\n");
   } else if (mode == 0) {
      t[0] = 0;
      p[n++] = 0xC0033700; p[n++] = 0x00100500; p[n++] = (uint32_t)TGVA; p[n++] = (uint32_t)(TGVA >> 32); p[n++] = 0x5EED0001;
   } else {
      t[0] = 0x5EED5EEDu;   // a sentinel: tells "not written" from "wrote 0"
      t[1] = 0;
      if (mode == 1) {      // COPY_DATA: src_sel 2 (TC_L2 memory), dst_sel 5 (memory), WR_CONFIRM
         p[n++] = 0xC0044000; p[n++] = 2u | (5u << 8) | (1u << 20);
         p[n++] = (uint32_t)hole; p[n++] = (uint32_t)(hole >> 32);
         p[n++] = (uint32_t)TGVA; p[n++] = (uint32_t)(TGVA >> 32);
      } else {              // WRITE_DATA: dst_sel 5 (memory), WR_CONFIRM
         p[n++] = 0xC0033700; p[n++] = 0x00100500; p[n++] = (uint32_t)hole; p[n++] = (uint32_t)(hole >> 32); p[n++] = 0xC0FFEE01;
      }
      // the marker: proves the CP went on past the faulting packet
      p[n++] = 0xC0033700; p[n++] = 0x00100500; p[n++] = (uint32_t)MARK; p[n++] = (uint32_t)(MARK >> 32); p[n++] = 0x5EED0002;
      printf("%s: probe VA 0x%llx (page offset 0x%llx), target VA 0x%llx, marker VA 0x%llx\n", mode == 1 ? "FD-1 read" : "FD-2 write",
             (unsigned long long)hole, (unsigned long long)(hole & 0xFFF), (unsigned long long)TGVA, (unsigned long long)MARK);
   }
   printf("IB (%u dw):", n);
   for (uint32_t i = 0; i < n; i++) printf(" %08x", p[i]);
   printf("\n");
   if (dry) { printf("dry: nothing submitted\n"); return 0; }

   struct { struct phxn_cs_in h; struct phxn_cs_ib ib[1]; } req;
   memset(&req, 0, sizeof req);
   req.h.abi = PHXN_ABI_VERSION; req.h.ctx_id = co.op; req.h.num_ibs = 1;
   req.ib[0].va_start = IBVA; req.ib[0].ib_bytes = n * 4; req.ib[0].ip_type = gfx ? PHX_AMDGPU_HW_IP_GFX : PHX_AMDGPU_HW_IP_COMPUTE;
   uint64_t seq = 0; uint32_t sn = 1;
   kr = IOConnectCallMethod(c, PHXN_SEL_SUBMIT, NULL, 0, &req, sizeof req, &seq, &sn, NULL, NULL);
   printf("submit kr 0x%x seq %llu\n", kr, (unsigned long long)seq);
   if (kr) return 1;

   uint64_t win[3] = {seq, (uint64_t)timeout_ms * 1000000ull, 0}, wout[3] = {0}; uint32_t wn = 3;
   kr = IOConnectCallScalarMethod(c, PHXN_SEL_WAITSEQ, win, 3, wout, &wn);
   const int reached = !kr && !wout[0];
   printf("waitseq kr 0x%x pending %llu retired %llu emitted %llu: fence %s\n", kr, (unsigned long long)wout[0],
          (unsigned long long)wout[1], (unsigned long long)wout[2], reached ? "RETIRED" : "NOT retired");
   if (mode == 0) {
      printf("target = 0x%08x (want 5eed0001)\n", t[0]);
      return t[0] == 0x5EED0001 ? 0 : 2;
   }

   const uint32_t want_mark = mode >= 3 ? FD3_MARKER : 0x5EED0002u;
   printf("target[0] = 0x%08x (%s), target[1] = 0x%08x (marker, want %08x)", t[0],
          mode == 1 ? "want the fill pattern" : "want 5eed5eed, untouched", t[1], want_mark);
   if (mode >= 3) printf(", target[2] = 0x%08x (want %s)", t[2], mode == 4 ? "c0ffee03" : "5eed5eed, untouched");
   printf("\n");
   const int regs_ok = !fault_regs("after", hole, st_after);
   grbm_poll();
   stat_line("after", sbuf, sizeof sbuf);
   if (!reached) { printf("RESULT: FAIL (fence not retired within %u ms)\n", timeout_ms); return 3; }
   if (mode == 4) {
      const int ok = t[1] == FD3_MARKER && t[2] == FD3_VALUE && t[0] == 0x5EED5EEDu;
      printf("STATUS %s by this run (before 0x%08x, after 0x%08x)\n",
             st_before[0] == st_after[0] && st_before[1] == st_after[1] ? "unchanged" : "CHANGED", st_before[0], st_after[0]);
      printf("RESULT: %s (shader store %s, marker %s)\n", ok ? "PASS" : "FAIL", t[2] == FD3_VALUE ? "ok" : "missing",
             t[1] == FD3_MARKER ? "ok" : "missing");
      return ok ? 0 : 2;
   }
   if (mode == 3) {
      unsigned off = 0, val = 0;
      const char *f = strstr(sbuf, "first at +");
      const int got = f && sscanf(f, "first at +0x%x = 0x%x", &off, &val) == 2;
      const int inpage = got && off == (unsigned)(hole & 0xFFF) && val == FD3_VALUE;
      const uint64_t fva = ((((uint64_t)(st_after[2] & 0xF)) << 32) | st_after[1]) << 12;
      const unsigned vmid = (st_after[0] >> 20) & 0xF, cid = (st_after[0] >> 9) & 0x1FF, rw = (st_after[0] >> 18) & 1;
      const int st_ok = regs_ok && cid == 8 && rw == 1 && vmid >= 8 && fva == (hole & ~0xFFFull);
      printf("default page: %s\n", got ? (inpage ? "0xC0FFEE03 at the probe VA's page offset" : "a different first dword (see stat)")
                                        : "no dword differs from the fill");
      printf("STATUS: %s (CID %u RW %u VMID %u page 0x%llx; want CID 8 TCP, RW 1, VMID 8-15, page 0x%llx)\n",
             st_ok ? "the TCP write fault at the probe page" : "NOT the expected fault", cid, rw, vmid, (unsigned long long)fva,
             (unsigned long long)(hole & ~0xFFFull));
      const int ok = t[1] == FD3_MARKER && t[0] == 0x5EED5EEDu && t[2] == 0x5EED5EEDu && inpage && st_ok;
      printf("RESULT: %s (marker %s, page %s, STATUS %s)\n", ok ? "PASS" : "FAIL", t[1] == FD3_MARKER ? "ok" : "missing",
             inpage ? "ok" : "not seen", st_ok ? "ok" : "no");
      return ok ? 0 : 2;
   }
   int okd = t[1] == 0x5EED0002u;
   if (mode == 1) {
      okd = okd && t[0] == expect;
      printf("RESULT: %s (target[0] 0x%08x vs expected 0x%08x, marker %s)\n", okd ? "PASS" : "FAIL", t[0], expect, t[1] == 0x5EED0002u ? "ok" : "missing");
   } else {
      unsigned off = 0, val = 0;
      const char *f = strstr(sbuf, "first at +");
      const int got = f && sscanf(f, "first at +0x%x = 0x%x", &off, &val) == 2;
      const int inpage = got && off == (unsigned)(hole & 0xFFF) && val == 0xC0FFEE01u;
      printf("default page: %s\n", got ? (inpage ? "0xC0FFEE01 at the probe VA's page offset" : "a different first dword (see stat)") : "no dword differs from the fill");
      okd = okd && inpage;
      printf("RESULT: %s (marker %s, page %s)\n", okd ? "PASS" : "FAIL", t[1] == 0x5EED0002u ? "ok" : "missing", inpage ? "ok" : "not seen");
   }
   return okd ? 0 : 2;
}
