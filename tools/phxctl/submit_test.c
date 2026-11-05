// SPDX-License-Identifier: MIT
// submit_test: one compute submission through PhxBringup's RADV-facing ABI (no RADV needed).
// HELLO -> CTX alloc -> 2 GTT BOs (IB, target) -> CPU map -> GEMVA map -> IB = WRITE_DATA(target VA) -> SUBMIT -> WAITSEQ.
// Needs a kext-adopted compute queue (`mesboot cs mc ADOPT` after navi31_up.sh).
// build: clang -arch x86_64 -O2 -o submit_test submit_test.c -framework IOKit -framework CoreFoundation
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>   // static_assert in PhxNativeABI.h
#include "../../src/phxbringup/PhxNativeABI.h"

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

int main(void)
{
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

   volatile uint32_t *t = (volatile uint32_t *)(uintptr_t)tgp;
   t[0] = 0;
   uint32_t *p = (uint32_t *)(uintptr_t)ibp;
   p[0] = 0xC0033700; p[1] = 0x00100500; p[2] = (uint32_t)TGVA; p[3] = (uint32_t)(TGVA >> 32); p[4] = 0x5EED0001;

   struct { struct phxn_cs_in h; struct phxn_cs_ib ib[1]; } req;
   memset(&req, 0, sizeof req);
   req.h.abi = PHXN_ABI_VERSION; req.h.ctx_id = co.op; req.h.num_ibs = 1;
   req.ib[0].va_start = IBVA; req.ib[0].ib_bytes = 5 * 4; req.ib[0].ip_type = PHX_AMDGPU_HW_IP_COMPUTE;
   uint64_t seq = 0; uint32_t sn = 1;
   kr = IOConnectCallMethod(c, PHXN_SEL_SUBMIT, NULL, 0, &req, sizeof req, &seq, &sn, NULL, NULL);
   printf("submit kr 0x%x seq %llu\n", kr, (unsigned long long)seq);
   if (kr) return 1;

   uint64_t win[3] = {seq, 2000000000ull, 0}, wout[3] = {0}; uint32_t wn = 3;
   kr = IOConnectCallScalarMethod(c, PHXN_SEL_WAITSEQ, win, 3, wout, &wn);
   printf("waitseq kr 0x%x pending %llu retired %llu emitted %llu\n", kr, (unsigned long long)wout[0],
          (unsigned long long)wout[1], (unsigned long long)wout[2]);
   printf("target = 0x%08x (want 5eed0001)\n", t[0]);
   return t[0] == 0x5EED0001 ? 0 : 2;
}
