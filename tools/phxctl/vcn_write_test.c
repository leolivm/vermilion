// SPDX-License-Identifier: MIT
// vcn_write_test: one VCN unified-ring submission through PhxBringup's RADV-facing ABI (WP6 B2, kext b67+).
// HELLO -> CTX alloc -> 2 GTT BOs (IB, target) -> GEMVA map -> IB = COMMON write-memory (ac_emit_video_write_memory:
// ENGINE_INFO header type 1 + op 0x33000001 va data) -> SUBMIT ip_type VCN_ENC -> WAITSEQ -> check the target.
// "neg" points the write at an unmapped VA instead (expect an MMHUB fault, target unchanged).
// "m1" runs the known-good M1 IBs instead (decode create then destroy session, SIGNATURE + ENGINE_INFO decode, as
// vcn_m1_gen.py dec_ib), but from client BOs in VMID 8 with the messages at client VAs: isolates VMID-8 fetch from content.
// Needs debug.phxvcn=1 (after vcn_up.sh --run) and debug.phxmmvm=1.
// build: clang -arch x86_64 -O2 -o vcn_write_test vcn_write_test.c -framework IOKit -framework CoreFoundation
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>   // static_assert in PhxNativeABI.h
#include "../../src/vermilion-boot/PhxNativeABI.h"

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

static void dec_ib(uint32_t *ib, uint64_t msg)
{
   memset(ib, 0, 43 * 4);
   ib[0] = 0x10; ib[1] = 0x30000002; ib[3] = 39;
   ib[4] = 0x10; ib[5] = 0x30000001; ib[6] = 0x3; ib[7] = 39 * 4;
   ib[8] = 33 * 4 + 8; ib[9] = 0x1;
   ib[10] = 0x1; ib[11] = (uint32_t)(msg >> 32); ib[12] = (uint32_t)msg;
   uint32_t sum = 0;
   for (int i = 4; i < 4 + 39; i++) sum += ib[i];
   ib[2] = sum;
}

static int submit_wait(uint32_t ctx, uint64_t va, uint32_t bytes)
{
   struct { struct phxn_cs_in h; struct phxn_cs_ib ib[1]; } req;
   memset(&req, 0, sizeof req);
   req.h.abi = PHXN_ABI_VERSION; req.h.ctx_id = ctx; req.h.num_ibs = 1;
   req.ib[0].va_start = va; req.ib[0].ib_bytes = bytes; req.ib[0].ip_type = PHX_AMDGPU_HW_IP_VCN_ENC;
   uint64_t seq = 0; uint32_t sn = 1;
   kern_return_t kr = IOConnectCallMethod(c, PHXN_SEL_SUBMIT, NULL, 0, &req, sizeof req, &seq, &sn, NULL, NULL);
   printf("submit kr 0x%x seq %llu\n", kr, (unsigned long long)seq);
   if (kr) return 1;
   uint64_t win[3] = {seq, 1000000000ull, 0}, wout[3] = {0}; uint32_t wn = 3;
   kr = IOConnectCallScalarMethod(c, PHXN_SEL_WAITSEQ, win, 3, wout, &wn);
   printf("waitseq kr 0x%x pending %llu retired %llu emitted %llu\n", kr, (unsigned long long)wout[0],
          (unsigned long long)wout[1], (unsigned long long)wout[2]);
   return kr || wout[0] ? 2 : 0;
}

int main(int argc, char **argv)
{
   const int neg = argc > 1 && !strcmp(argv[1], "neg");
   const int m1 = argc > 1 && !strcmp(argv[1], "m1");
   io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
   if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { printf("open failed\n"); return 1; }
   uint64_t hin[2] = {PHXN_ABI_VERSION, PHXN_HELLO_F_MINOR}, hout[4]; uint32_t hn = 4;
   kern_return_t kr = IOConnectCallScalarMethod(c, PHXN_SEL_HELLO, hin, 2, hout, &hn);
   printf("hello kr 0x%x build %llu\n", kr, (unsigned long long)hout[1]);
   if (kr) return 1;

   struct phxn_ctx ci = {PHX_AMDGPU_CTX_OP_ALLOC_CTX, 0, 0, 0}, co; size_t cs = sizeof co;
   kr = IOConnectCallStructMethod(c, PHXN_SEL_CTX, &ci, sizeof ci, &co, &cs);
   printf("ctx kr 0x%x id %u\n", kr, co.op);
   if (kr) return 1;

   uint64_t ibp = 0, tgp = 0;
   uint32_t ib = bo_create(4096, &ibp), tg = bo_create(4096, &tgp);
   if (!ib || !tg) return 1;
   const uint64_t IBVA = 0x100000000ull, TGVA = 0x100010000ull;
   if (va_map(ib, IBVA, 4096) || va_map(tg, TGVA, 4096)) return 1;

   if (m1) {
      uint64_t mcp = 0, mdp = 0, idp = 0;
      uint32_t mc = bo_create(4096, &mcp), md = bo_create(4096, &mdp), id = bo_create(4096, &idp);
      const uint64_t MCVA = 0x100020000ull, MDVA = 0x100030000ull, IDVA = 0x100040000ull;
      if (!mc || !md || !id || va_map(mc, MCVA, 4096) || va_map(md, MDVA, 4096) || va_map(id, IDVA, 4096)) return 1;
      const uint32_t cm[14] = {0x28, 0x38, 0x1, 0x0, 3, 0x0, 0x1, 0x28, 0x10, 0x0, 0x7, 0x0, 0x780, 0x440};
      const uint32_t dm[6] = {0x28, 0x18, 0x0, 0x2, 3, 0x0};
      memset((void *)(uintptr_t)mcp, 0, 4096); memcpy((void *)(uintptr_t)mcp, cm, sizeof cm);
      memset((void *)(uintptr_t)mdp, 0, 4096); memcpy((void *)(uintptr_t)mdp, dm, sizeof dm);
      dec_ib((uint32_t *)(uintptr_t)ibp, MCVA);
      dec_ib((uint32_t *)(uintptr_t)idp, MDVA);
      printf("create session (handle 3) from VMID 8\n");
      int r = submit_wait(co.op, IBVA, 43 * 4);
      if (r) return r;
      printf("destroy session from VMID 8\n");
      r = submit_wait(co.op, IDVA, 43 * 4);
      printf("m1 in VMID 8: %s\n", r ? "FAIL" : "PASS (both fences retired)");
      return r;
   }
   volatile uint32_t *t = (volatile uint32_t *)(uintptr_t)tgp;
   t[0] = 0xDEADBEEF;
   const uint64_t DST = neg ? TGVA + 0x100000ull : TGVA;
   uint32_t *p = (uint32_t *)(uintptr_t)ibp;
   p[0] = 0x00000010; p[1] = 0x30000001; p[2] = 0x00000001; p[3] = 0x00000024;
   p[4] = 0x00000014; p[5] = 0x33000001; p[6] = (uint32_t)DST; p[7] = (uint32_t)(DST >> 32); p[8] = 0x13572468;

   struct { struct phxn_cs_in h; struct phxn_cs_ib ib[1]; } req;
   memset(&req, 0, sizeof req);
   req.h.abi = PHXN_ABI_VERSION; req.h.ctx_id = co.op; req.h.num_ibs = 1;
   req.ib[0].va_start = IBVA; req.ib[0].ib_bytes = 9 * 4; req.ib[0].ip_type = PHX_AMDGPU_HW_IP_VCN_ENC;
   uint64_t seq = 0; uint32_t sn = 1;
   kr = IOConnectCallMethod(c, PHXN_SEL_SUBMIT, NULL, 0, &req, sizeof req, &seq, &sn, NULL, NULL);
   printf("submit kr 0x%x seq %llu\n", kr, (unsigned long long)seq);
   if (kr) return 1;

   uint64_t win[3] = {seq, 2000000000ull, 0}, wout[3] = {0}; uint32_t wn = 3;
   kr = IOConnectCallScalarMethod(c, PHXN_SEL_WAITSEQ, win, 3, wout, &wn);
   printf("waitseq kr 0x%x pending %llu retired %llu emitted %llu\n", kr, (unsigned long long)wout[0],
          (unsigned long long)wout[1], (unsigned long long)wout[2]);
   const uint32_t want = neg ? 0xDEADBEEF : 0x13572468;
   printf("target = 0x%08x (want %08x)\n", t[0], want);
   return t[0] == want ? 0 : 2;
}
