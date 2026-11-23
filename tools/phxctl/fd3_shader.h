// SPDX-License-Identifier: MIT
// fd3_shader.h: the FD-3 probe for submit_test (H-FAULTDEF on the shader path, UTCL2 client TCP). One gfx11 compute wave
// (wave64, one thread) stores 0xC0FFEE03 through a raw 64-bit address (global_store_b32, as a buffer device address
// becomes in ACO) to an unmapped VA, waits for the store to be acknowledged, then stores the marker 0x5EED0003 to a
// mapped dword. The second store only happens if the first one completed: a fault the hub drops (no default page)
// leaves the wave in s_waitcnt_vscnt and the dispatch never ends.
//
// Shader (s[0:1] = store VA, s[2:3] = marker VA from COMPUTE_USER_DATA_0..3, RSRC2.USER_SGPR = 4):
//   v_mov_b32 v0, 0                    ; the VGPR offset of the saddr form
//   v_mov_b32 v1, 0xc0ffee03
//   global_store_b32 v0, v1, s[0:1]
//   s_waitcnt_vscnt null, 0x0
//   v_mov_b32 v2, 0x5eed0003
//   global_store_b32 v0, v2, s[2:3]
//   s_waitcnt_vscnt null, 0x0
//   s_endpgm
// Encodings: LLVM llvm/test/MC/AMDGPU at llvmorg-18.1.8, the golden vectors tests/mock/fd3_shader_test.c checks:
//   VOP1  0x7E000000 | vdst << 17 | op << 9 | src0; v_mov_b32 op 1 (gfx11_asm_vop1.s "v_mov_b32 v5, v1" =
//         [0x01,0x03,0x0a,0x7e]); src0 0xFF = a literal dword follows ("v_mov_b32 v255, 0xaf123456"); src0 0x80 =
//         inline 0 (gfx11_asm_sop1.s "s_mov_b32 s0, 0" = [0x80,0x00,0x80,0xbe], the same operand table)
//   FLAT  dw0 0xDC000000 | op << 18 | seg << 16 | offset13; global = seg 2, global_store_b32 op 26 = 0xDC6A0000
//         (gfx11_asm_flat.s "global_store_b32 v1, v2, s[6:7]" = [0x00,0x00,0x6a,0xdc,0x01,0x02,0x06,0x00]);
//         dw1 = addr | data << 8 | saddr << 16 | vdst << 24, saddr 0x7C = off
//   SOPK  0xB0000000 | op << 23 | sdst << 16 | simm16; s_waitcnt_vscnt op 24, sdst null 0x7C
//         ("s_waitcnt_vscnt null, 0x1234" = [0x34,0x12,0x7c,0xbc])
//   SOPP  0xBF800000 | op << 16 | simm16; s_endpgm op 48 (gfx11_asm_sopp.s "s_endpgm" = [0x00,0x00,0xb0,0xbf])
// PM4 (Linux v6.8 nvd.h / gfx_v11_0.c; the SET_SH_REG + PACKET3_COMPUTE(DISPATCH_DIRECT) pattern of
// gfx_v9_0_do_edc_gpr_workarounds; registers from gc_11_0_0_offset.h, GC base 0x1260, SET_SH_REG_START 0x2c00):
//   ACQUIRE_MEM exactly as gfx_v11_0_emit_mem_sync (GL2 INV+WB, GLM INV+WB, GL1/GLV/GLK/GLI INV: the CPU-written
//   shader and stale lines out of the way), the compute SH state, DISPATCH_DIRECT 1,1,1, EVENT_WRITE CS_PARTIAL_FLUSH.
#pragma once
#include <stdint.h>

enum {
   FD3_SH_START_X = 0x204, FD3_SH_NUM_THREAD_X = 0x207, FD3_SH_PGM_LO = 0x20c, FD3_SH_PGM_RSRC1 = 0x212,
   FD3_SH_PGM_RSRC2 = 0x213, FD3_SH_RESOURCE_LIMITS = 0x215, FD3_SH_STM_SE0 = 0x216, FD3_SH_TMPRING_SIZE = 0x218,
   FD3_SH_STM_SE2 = 0x219, FD3_SH_PGM_RSRC3 = 0x228, FD3_SH_STM_SE4 = 0x22b, FD3_SH_USER_DATA_0 = 0x240,
};
#define FD3_VALUE  0xC0FFEE03u
#define FD3_MARKER 0x5EED0003u
// RSRC1: VGPRS 1 (8 VGPRs in wave64 granules of 4, 16 in granules of 8: v0-v2 fit either way), FLOAT_MODE 0xC0,
// MEM_ORDERED 1 (as RADV on gfx10+); SGPRS is ignored on gfx10+. RSRC2: USER_SGPR 4, no TGID / TG_SIZE / scratch /
// trap. Initiator: COMPUTE_SHADER_EN | FORCE_START_AT_000, CS_W32_EN 0 = wave64.
#define FD3_RSRC1 (1u | (0xC0u << 12) | (1u << 30))
#define FD3_RSRC2 (4u << 1)
#define FD3_INITIATOR (1u | (1u << 2))
#define FD3_GCR 0xC3B1u   // GL2_WB 15 | GL2_INV 14 | GL1_INV 9 | GLV_INV 8 | GLK_INV 7 | GLM_INV 5 | GLM_WB 4 | GLI_INV 0

static inline uint32_t g11_vop1(uint32_t op, uint32_t vdst, uint32_t src0) { return 0x7E000000u | (vdst << 17) | (op << 9) | src0; }
static inline uint32_t g11_global0(uint32_t op, uint32_t off13) { return 0xDC000000u | (op << 18) | (2u << 16) | (off13 & 0x1FFFu); }
static inline uint32_t g11_flat1(uint32_t addr, uint32_t data, uint32_t saddr, uint32_t vdst) { return addr | (data << 8) | (saddr << 16) | (vdst << 24); }
static inline uint32_t g11_sopk(uint32_t op, uint32_t sdst, uint32_t simm16) { return 0xB0000000u | (op << 23) | (sdst << 16) | (simm16 & 0xFFFFu); }
static inline uint32_t g11_sopp(uint32_t op, uint32_t simm16) { return 0xBF800000u | (op << 16) | (simm16 & 0xFFFFu); }
#define G11_V_MOV_B32 1u
#define G11_GLOBAL_STORE_B32 26u
#define G11_S_WAITCNT_VSCNT 24u
#define G11_S_ENDPGM 48u
#define G11_SRC_LIT 0xFFu
#define G11_SRC_ZERO 0x80u
#define G11_NULL 0x7Cu

// the shader; returns its size in dwords (<= 16)
static inline uint32_t fd3_shader(uint32_t *s)
{
   uint32_t n = 0;
   s[n++] = g11_vop1(G11_V_MOV_B32, 0, G11_SRC_ZERO);
   s[n++] = g11_vop1(G11_V_MOV_B32, 1, G11_SRC_LIT); s[n++] = FD3_VALUE;
   s[n++] = g11_global0(G11_GLOBAL_STORE_B32, 0); s[n++] = g11_flat1(0, 1, 0, 0);    // v0, v1, s[0:1]
   s[n++] = g11_sopk(G11_S_WAITCNT_VSCNT, G11_NULL, 0);
   s[n++] = g11_vop1(G11_V_MOV_B32, 2, G11_SRC_LIT); s[n++] = FD3_MARKER;
   s[n++] = g11_global0(G11_GLOBAL_STORE_B32, 0); s[n++] = g11_flat1(0, 2, 2, 0);    // v0, v2, s[2:3]
   s[n++] = g11_sopk(G11_S_WAITCNT_VSCNT, G11_NULL, 0);
   s[n++] = g11_sopp(G11_S_ENDPGM, 0);
   return n;
}

static inline uint32_t pkt3(uint32_t op, uint32_t count) { return 0xC0000000u | ((count - 1u) << 16) | (op << 8); }

static inline uint32_t fd3_sh(uint32_t *p, uint32_t n, uint32_t reg, const uint32_t *v, uint32_t cnt)
{
   p[n++] = pkt3(0x76, cnt + 1); p[n++] = reg;
   for (uint32_t i = 0; i < cnt; i++) p[n++] = v[i];
   return n;
}

// the IB; returns its size in dwords (<= 80). shader_va must be 256-byte aligned.
static inline uint32_t fd3_ib(uint32_t *p, uint64_t shader_va, uint64_t store_va, uint64_t mark_va)
{
   uint32_t n = 0;
   p[n++] = pkt3(0x58, 7);   // ACQUIRE_MEM, gfx_v11_0_emit_mem_sync
   p[n++] = 0; p[n++] = 0xFFFFFFFFu; p[n++] = 0x00FFFFFFu; p[n++] = 0; p[n++] = 0; p[n++] = 0x0000000Au; p[n++] = FD3_GCR;
   const uint32_t pgm[2] = {(uint32_t)(shader_va >> 8), (uint32_t)(shader_va >> 40)};
   const uint32_t rsrc[2] = {FD3_RSRC1, FD3_RSRC2};
   // CU masks as Mesa gfx10_init_compute_preamble_state (ac_cmdbuf.c, mesa-25.0.0) for max_se 6 (Navi 31): all CUs
   // on SE0-5, 0 on SE6/7
   const uint32_t zero1[1] = {0}, all2[2] = {0xFFFFFFFFu, 0xFFFFFFFFu}, se4_7[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0};
   const uint32_t start[6] = {0, 0, 0, 1, 1, 1};   // START_X/Y/Z, NUM_THREAD_X/Y/Z (FULL 1 = one thread)
   const uint32_t ud[4] = {(uint32_t)store_va, (uint32_t)(store_va >> 32), (uint32_t)mark_va, (uint32_t)(mark_va >> 32)};
   n = fd3_sh(p, n, FD3_SH_PGM_LO, pgm, 2);
   n = fd3_sh(p, n, FD3_SH_PGM_RSRC1, rsrc, 2);
   n = fd3_sh(p, n, FD3_SH_PGM_RSRC3, zero1, 1);
   n = fd3_sh(p, n, FD3_SH_RESOURCE_LIMITS, zero1, 1);
   n = fd3_sh(p, n, FD3_SH_TMPRING_SIZE, zero1, 1);
   n = fd3_sh(p, n, FD3_SH_STM_SE0, all2, 2);
   n = fd3_sh(p, n, FD3_SH_STM_SE2, all2, 2);
   n = fd3_sh(p, n, FD3_SH_STM_SE4, se4_7, 4);
   n = fd3_sh(p, n, FD3_SH_START_X, start, 6);
   n = fd3_sh(p, n, FD3_SH_USER_DATA_0, ud, 4);
   p[n++] = pkt3(0x15, 4) | (1u << 1);   // DISPATCH_DIRECT, PACKET3_COMPUTE
   p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = FD3_INITIATOR;
   p[n++] = pkt3(0x46, 1); p[n++] = 7u | (4u << 8);   // EVENT_WRITE CS_PARTIAL_FLUSH, EVENT_INDEX 4
   return n;
}
