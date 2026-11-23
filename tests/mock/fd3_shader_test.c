// SPDX-License-Identifier: MIT
// fd3_shader_test.c: host check of tools/phxctl/fd3_shader.h (submit_test fd3 / fd3ok). NO hardware.
// 1. The encoders reproduce LLVM's gfx11 golden encodings (llvm/test/MC/AMDGPU/gfx11_asm_{vop1,sop1,flat,sopk,sopp}.s
//    at llvmorg-18.1.8, quoted below with the source line).
// 2. A decoder written from the field layouts, sharing no code with the header, disassembles the shader back to the
//    intended listing.
// 3. A PM4 walker decodes the IB: packet sizes add up, every SH register the dispatch needs is set once with the
//    expected value, the user SGPRs hold the store and marker VAs, the dispatch is 1x1x1 after all state, and the
//    cache sync comes first.
// Exit 0 iff all pass.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fd3_shader.h"

static int fails, passes;
static void check(int ok, const char *what)
{
   if (ok) passes++; else { fails++; printf("FAIL  %s\n", what); }
}

// ---------------------------------------------------------------------------------------------- 1. golden vectors
static uint32_t le(const unsigned char *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static void golden(void)
{
   // gfx11_asm_vop1.s: "v_mov_b32 v5, v1" -> [0x01,0x03,0x0a,0x7e]
   static const unsigned char vmov_v[4] = {0x01, 0x03, 0x0a, 0x7e};
   check(g11_vop1(G11_V_MOV_B32, 5, 256 + 1) == le(vmov_v), "golden v_mov_b32 v5, v1");
   // gfx11_asm_vop1.s: "v_mov_b32 v5, s1" -> [0x01,0x02,0x0a,0x7e]
   static const unsigned char vmov_s[4] = {0x01, 0x02, 0x0a, 0x7e};
   check(g11_vop1(G11_V_MOV_B32, 5, 1) == le(vmov_s), "golden v_mov_b32 v5, s1");
   // gfx11_asm_vop1.s: "v_mov_b32 v255, 0xaf123456" -> [0xff,0x02,0xfe,0x7f,0x56,0x34,0x12,0xaf]
   static const unsigned char vmov_l[8] = {0xff, 0x02, 0xfe, 0x7f, 0x56, 0x34, 0x12, 0xaf};
   check(g11_vop1(G11_V_MOV_B32, 255, G11_SRC_LIT) == le(vmov_l) && le(vmov_l + 4) == 0xaf123456u, "golden v_mov_b32 v255, literal");
   // gfx11_asm_vop1.s: "v_mov_b32 v5, -1" -> [0xc1,0x02,0x0a,0x7e]; gfx11_asm_sop1.s: "s_mov_b32 s0, 0" ->
   // [0x80,0x00,0x80,0xbe]: inline 0 is operand 0x80, -1 is 0xc1 (the same operand table for SSRC0 and VOP SRC0)
   static const unsigned char vmov_m1[4] = {0xc1, 0x02, 0x0a, 0x7e}, smov0[4] = {0x80, 0x00, 0x80, 0xbe};
   check(g11_vop1(G11_V_MOV_B32, 5, 0xc1) == le(vmov_m1), "golden v_mov_b32 v5, -1");
   check((le(smov0) & 0xFF) == G11_SRC_ZERO, "golden inline constant 0 = 0x80 (s_mov_b32 s0, 0)");
   // gfx11_asm_flat.s: "global_store_b32 v1, v2, s[6:7]" -> [0x00,0x00,0x6a,0xdc,0x01,0x02,0x06,0x00]
   static const unsigned char gs1[8] = {0x00, 0x00, 0x6a, 0xdc, 0x01, 0x02, 0x06, 0x00};
   check(g11_global0(G11_GLOBAL_STORE_B32, 0) == le(gs1) && g11_flat1(1, 2, 6, 0) == le(gs1 + 4), "golden global_store_b32 v1, v2, s[6:7]");
   // gfx11_asm_flat.s: "global_store_b32 v[1:2], v2, off" -> [0x00,0x00,0x6a,0xdc,0x01,0x02,0x7c,0x00]
   static const unsigned char gs2[8] = {0x00, 0x00, 0x6a, 0xdc, 0x01, 0x02, 0x7c, 0x00};
   check(g11_global0(G11_GLOBAL_STORE_B32, 0) == le(gs2) && g11_flat1(1, 2, G11_NULL, 0) == le(gs2 + 4), "golden global_store_b32 v[1:2], v2, off");
   // gfx11_asm_flat.s: "global_store_b32 v255, v2, vcc offset:4095" -> [0xff,0x0f,0x6a,0xdc,0xff,0x02,0x6a,0x00]
   static const unsigned char gs3[8] = {0xff, 0x0f, 0x6a, 0xdc, 0xff, 0x02, 0x6a, 0x00};
   check(g11_global0(G11_GLOBAL_STORE_B32, 4095) == le(gs3) && g11_flat1(255, 2, 0x6a, 0) == le(gs3 + 4), "golden global_store_b32 offset:4095");
   // gfx11_asm_flat.s: "global_store_b32 v255, v2, s[6:7] offset:-1" -> [0xff,0x1f,0x6a,0xdc,0xff,0x02,0x06,0x00]
   static const unsigned char gs4[8] = {0xff, 0x1f, 0x6a, 0xdc, 0xff, 0x02, 0x06, 0x00};
   check(g11_global0(G11_GLOBAL_STORE_B32, (uint32_t)-1) == le(gs4), "golden global_store_b32 offset:-1");
   // gfx11_asm_sopk.s: "s_waitcnt_vscnt null, 0x1234" -> [0x34,0x12,0x7c,0xbc]
   static const unsigned char wv[4] = {0x34, 0x12, 0x7c, 0xbc};
   check(g11_sopk(G11_S_WAITCNT_VSCNT, G11_NULL, 0x1234) == le(wv), "golden s_waitcnt_vscnt null, 0x1234");
   // gfx11_asm_sopp.s: "s_endpgm" -> [0x00,0x00,0xb0,0xbf]
   static const unsigned char ep[4] = {0x00, 0x00, 0xb0, 0xbf};
   check(g11_sopp(G11_S_ENDPGM, 0) == le(ep), "golden s_endpgm");
}

// ---------------------------------------------------------------------------------------------- 2. disassembler
// Independent of the header: raw bit fields per the gfx11 layouts (VOP1 bits 31:25 = 0x3F; FLAT 31:26 = 0x37;
// SOPK 31:28 = 0xB; SOPP 31:23 = 0x17F).
static const char *opnd(uint32_t v, uint32_t lit, char *b)
{
   if (v >= 256) snprintf(b, 24, "v%u", v - 256);
   else if (v == 0xFF) snprintf(b, 24, "0x%x", lit);
   else if (v >= 128 && v <= 192) snprintf(b, 24, "%u", v - 128);
   else if (v < 106) snprintf(b, 24, "s%u", v);
   else snprintf(b, 24, "?%x", v);
   return b;
}
static int disasm(const uint32_t *s, uint32_t n, char *out, size_t outsz)
{
   size_t o = 0; out[0] = 0;
   for (uint32_t i = 0; i < n;) {
      const uint32_t w = s[i];
      char line[96], a[24];
      if ((w >> 25) == 0x3F) {                                   // VOP1
         const uint32_t src = w & 0x1FF, op = (w >> 9) & 0xFF, vd = (w >> 17) & 0xFF;
         const uint32_t lit = src == 0xFF && i + 1 < n ? s[i + 1] : 0;
         if (op != 1) return -1;
         snprintf(line, sizeof line, "v_mov_b32 v%u, %s", vd, opnd(src, lit, a));
         i += src == 0xFF ? 2 : 1;
      } else if ((w >> 26) == 0x37) {                            // FLAT / GLOBAL
         if (i + 1 >= n) return -1;
         const uint32_t w1 = s[i + 1], off = w & 0x1FFF, seg = (w >> 16) & 3, op = (w >> 18) & 0x7F;
         const uint32_t addr = w1 & 0xFF, data = (w1 >> 8) & 0xFF, saddr = (w1 >> 16) & 0x7F;
         if (seg != 2 || op != 26 || (w & 0xE000) || (w & (1u << 25))) return -1;   // global, store_b32, no glc/slc/dlc
         if (saddr == 0x7C) snprintf(line, sizeof line, "global_store_b32 v[%u:%u], v%u, off", addr, addr + 1, data);
         else snprintf(line, sizeof line, "global_store_b32 v%u, v%u, s[%u:%u]", addr, data, saddr, saddr + 1);
         if (off) { size_t l = strlen(line); snprintf(line + l, sizeof line - l, " offset:%u", off); }
         i += 2;
      } else if ((w >> 28) == 0xB && ((w >> 23) & 0x1F) == 24) { // SOPK s_waitcnt_vscnt
         if (((w >> 16) & 0x7F) != 0x7C) return -1;
         snprintf(line, sizeof line, "s_waitcnt_vscnt null, 0x%x", w & 0xFFFF);
         i += 1;
      } else if ((w >> 23) == 0x17F && ((w >> 16) & 0x7F) == 48) {   // SOPP s_endpgm
         snprintf(line, sizeof line, "s_endpgm");
         i += 1;
      } else {
         return -1;
      }
      o += snprintf(out + o, outsz - o, "%s\n", line);
   }
   return 0;
}
static void shader(void)
{
   uint32_t s[32]; memset(s, 0, sizeof s);
   const uint32_t n = fd3_shader(s);
   char text[1024];
   static const char *want =
      "v_mov_b32 v0, 0\n"
      "v_mov_b32 v1, 0xc0ffee03\n"
      "global_store_b32 v0, v1, s[0:1]\n"
      "s_waitcnt_vscnt null, 0x0\n"
      "v_mov_b32 v2, 0x5eed0003\n"
      "global_store_b32 v0, v2, s[2:3]\n"
      "s_waitcnt_vscnt null, 0x0\n"
      "s_endpgm\n";
   const int r = disasm(s, n, text, sizeof text);
   check(r == 0, "shader disassembles (only known instructions)");
   check(n <= 16, "shader fits 16 dwords");
   if (r == 0 && strcmp(text, want)) printf("shader listing:\n%sexpected:\n%s", text, want);
   check(r == 0 && !strcmp(text, want), "shader listing = the FD-3 program");
   // the RSRC1 VGPR budget covers v0..v2 (granule 4 in wave64), RSRC2 loads exactly s0..s3
   check(((FD3_RSRC1 & 0x3F) + 1) * 4 >= 3, "RSRC1 VGPRS covers v0-v2");
   check(((FD3_RSRC2 >> 1) & 0x1F) == 4 && !(FD3_RSRC2 & 1) && !(FD3_RSRC2 & (1u << 6)) && !(FD3_RSRC2 & (0x1Fu << 7)),
         "RSRC2 = 4 user SGPRs, no scratch / trap / TGID / TG_SIZE / TIDIG");
   check(!(FD3_RSRC1 & (1u << 20)), "RSRC1 PRIV off");
   check((FD3_INITIATOR & 1) && !(FD3_INITIATOR & (1u << 15)), "initiator COMPUTE_SHADER_EN, wave64");
}

// ---------------------------------------------------------------------------------------------- 3. PM4 walker
static void ib(void)
{
   const uint64_t SHVA = 0x100000800ull, ST = 0x100008140ull, MK = 0x100010004ull;
   uint32_t p[256]; memset(p, 0xEE, sizeof p);
   const uint32_t n = fd3_ib(p, SHVA, ST, MK);
   check(n > 0 && n <= 80 && n * 4 <= 0x800, "IB fits below the shader at +0x800");
   check(p[n] == 0xEEEEEEEEu, "nothing written past the returned size");
   uint32_t sh[0x400]; int set[0x400]; memset(set, 0, sizeof set);
   int acq_at = -1, disp_at = -1, csflush_at = -1, last_sh = -1, pk = 0, bad = 0;
   uint32_t dim[4] = {0};
   for (uint32_t i = 0; i < n;) {
      const uint32_t h = p[i];
      if ((h >> 30) != 3) { bad = 1; break; }
      const uint32_t cnt = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0xFF;
      if (i + 1 + cnt > n) { bad = 1; break; }
      const uint32_t *d = p + i + 1;
      if (op == 0x58 && cnt == 7) {
         if (d[0] == 0 && d[1] == 0xFFFFFFFFu && d[2] == 0x00FFFFFFu && d[3] == 0 && d[4] == 0 && d[5] == 0xA &&
             d[6] == ((1u << 14) | (1u << 15) | (1u << 5) | (1u << 4) | (1u << 9) | (1u << 8) | (1u << 7) | 1u))
            acq_at = pk;
      } else if (op == 0x76) {
         const uint32_t reg = d[0];
         for (uint32_t k = 1; k < cnt; k++) {
            if (reg + k - 1 >= 0x400) { bad = 1; break; }
            sh[reg + k - 1] = d[k]; set[reg + k - 1]++;
         }
         last_sh = pk;
      } else if (op == 0x15 && cnt == 4) {
         if (!(h & 2)) bad = 1;   // PACKET3_COMPUTE
         memcpy(dim, d, sizeof dim); disp_at = pk;
      } else if (op == 0x46 && cnt == 1) {
         if (d[0] == (7u | (4u << 8))) csflush_at = pk;
      } else {
         bad = 1;
      }
      i += 1 + cnt; pk++;
   }
   check(!bad, "IB decodes as type-3 packets ACQUIRE_MEM / SET_SH_REG / DISPATCH_DIRECT(compute) / EVENT_WRITE that add up");
   check(acq_at == 0, "first packet = ACQUIRE_MEM as gfx_v11_0_emit_mem_sync");
   check(disp_at > last_sh && disp_at >= 0, "DISPATCH_DIRECT after all SET_SH_REG");
   check(csflush_at == disp_at + 1 && csflush_at == pk - 1, "CS_PARTIAL_FLUSH right after the dispatch, last");
   check(dim[0] == 1 && dim[1] == 1 && dim[2] == 1 && dim[3] == FD3_INITIATOR, "dispatch 1x1x1");
   // gc_11_0_0_offset.h (BASE_IDX 0, GC base 0x1260) - SET_SH_REG_START 0x2c00 = reg - 0x19a0
   struct { const char *name; uint32_t linux_off; uint32_t want; } r[] = {
      {"COMPUTE_PGM_LO", 0x1bac, (uint32_t)(SHVA >> 8)}, {"COMPUTE_PGM_HI", 0x1bad, (uint32_t)(SHVA >> 40)},
      {"COMPUTE_PGM_RSRC1", 0x1bb2, FD3_RSRC1}, {"COMPUTE_PGM_RSRC2", 0x1bb3, FD3_RSRC2}, {"COMPUTE_PGM_RSRC3", 0x1bc8, 0},
      {"COMPUTE_RESOURCE_LIMITS", 0x1bb5, 0}, {"COMPUTE_TMPRING_SIZE", 0x1bb8, 0},
      {"COMPUTE_STATIC_THREAD_MGMT_SE0", 0x1bb6, 0xFFFFFFFFu}, {"COMPUTE_STATIC_THREAD_MGMT_SE1", 0x1bb7, 0xFFFFFFFFu},
      {"COMPUTE_STATIC_THREAD_MGMT_SE2", 0x1bb9, 0xFFFFFFFFu}, {"COMPUTE_STATIC_THREAD_MGMT_SE3", 0x1bba, 0xFFFFFFFFu},
      {"COMPUTE_STATIC_THREAD_MGMT_SE4", 0x1bcb, 0xFFFFFFFFu}, {"COMPUTE_STATIC_THREAD_MGMT_SE5", 0x1bcc, 0xFFFFFFFFu},
      {"COMPUTE_STATIC_THREAD_MGMT_SE6", 0x1bcd, 0}, {"COMPUTE_STATIC_THREAD_MGMT_SE7", 0x1bce, 0},
      {"COMPUTE_START_X", 0x1ba4, 0}, {"COMPUTE_START_Y", 0x1ba5, 0}, {"COMPUTE_START_Z", 0x1ba6, 0},
      {"COMPUTE_NUM_THREAD_X", 0x1ba7, 1}, {"COMPUTE_NUM_THREAD_Y", 0x1ba8, 1}, {"COMPUTE_NUM_THREAD_Z", 0x1ba9, 1},
      {"COMPUTE_USER_DATA_0 (store VA lo)", 0x1be0, (uint32_t)ST}, {"COMPUTE_USER_DATA_1 (store VA hi)", 0x1be1, (uint32_t)(ST >> 32)},
      {"COMPUTE_USER_DATA_2 (marker VA lo)", 0x1be2, (uint32_t)MK}, {"COMPUTE_USER_DATA_3 (marker VA hi)", 0x1be3, (uint32_t)(MK >> 32)},
   };
   int nset = 0;
   for (unsigned k = 0; k < sizeof r / sizeof r[0]; k++) {
      const uint32_t off = r[k].linux_off + 0x1260 - 0x2c00;
      char what[128];
      snprintf(what, sizeof what, "%s (SH 0x%03x) set once = 0x%08x", r[k].name, off, r[k].want);
      const int ok = off < 0x400 && set[off] == 1 && sh[off] == r[k].want;
      if (!ok && off < 0x400) printf("  %s: set %d value 0x%08x\n", r[k].name, set[off], sh[off]);
      check(ok, what);
      nset++;
   }
   int total = 0;
   for (int k = 0; k < 0x400; k++) total += set[k];
   check(total == nset, "no SH register outside the list");
   check(!(SHVA & 0xFF), "shader VA 256-byte aligned");
}

int main(void)
{
   golden();
   shader();
   ib();
   printf("RESULT: %d passed, %d failed\n", passes, fails);
   return fails ? 1 : 0;
}
