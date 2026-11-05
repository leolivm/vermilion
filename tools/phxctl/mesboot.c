// SPDX-License-Identifier: MIT OR GPL-2.0-only
// mesboot: Phoenix (GC 11.0.1) GART + MES bring-up experiments — navi31 branch: RX 7900 XT (GC 11.0.0) values, driven through PhxBringup debug selectors.
// build: clang -O2 -o mesboot mesboot.c -framework IOKit -framework CoreFoundation ; run as root (phxdebug=1)
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include "mes_v11_api_def.h"

#define UC_TYPE 0x4E34384Eu
#define SEL_HELLO 0
#define SEL_READREGS 2
#define SEL_DBG_VRAM_READ 102
#define SEL_DBG_WRITE_REG 108
#define SEL_DBG_VRAM_WRITE 109
#define SEL_DBG_DOORBELL 110
#define SEL_DBG_SYSMEM 111
#define SEL_DBG_ADOPTQ 112

static io_connect_t conn;

static int open_kext(void)
{
   io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
   if (!svc) { fprintf(stderr, "no PhxBringup\n"); return -1; }
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), UC_TYPE, &conn);
   uint64_t hin[2] = {1, 0}, hout[4]; uint32_t hn = 4;
   if (kr == 0) kr = IOConnectCallScalarMethod(conn, SEL_HELLO, hin, 2, hout, &hn);
   if (kr) { fprintf(stderr, "open: 0x%x\n", kr); return -1; }
   return 0;
}
static uint32_t rd(uint32_t reg)
{
   uint64_t in[3] = {reg, 1, 0xFFFFFFFFull}; uint32_t v = 0; size_t sz = 4;
   IOConnectCallMethod(conn, SEL_READREGS, in, 3, NULL, 0, NULL, NULL, &v, &sz);
   return v;
}
static uint32_t wr(uint32_t reg, uint32_t val)
{
   uint64_t in[2] = {reg, val}, out[1] = {0}; uint32_t n = 1;
   IOConnectCallScalarMethod(conn, SEL_DBG_WRITE_REG, in, 2, out, &n);
   return (uint32_t)out[0];
}
static void hdp_flush(void);
static kern_return_t db(uint64_t byteOff, uint64_t v)
{
   uint64_t in[3] = {byteOff, v & 0xffffffffu, v >> 32};
   hdp_flush();   // navi31: CPU writes to VRAM through BAR0 sit in the HDP until flushed; the doorbell must follow it
   return IOConnectCallScalarMethod(conn, SEL_DBG_DOORBELL, in, 3, NULL, NULL);
}
static kern_return_t sm(uint64_t op, uint64_t a, uint64_t b, uint64_t *o0, uint64_t *o1)
{
   uint64_t in[3] = {op, a, b}, out[2] = {0, 0}; uint32_t n = 2;
   kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_SYSMEM, in, 3, out, &n);
   if (o0) *o0 = out[0];
   if (o1) *o1 = out[1];
   return kr;
}
// navi31: HDP flush as Linux hdp_v6_0_flush_hdp: write 0 to the MMIO-remapped HDP_MEM_FLUSH_CNTL
// (BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL 0xE4D holds its BAR5 byte offset, 0x385C -> dword 0xE17), then read back.
static void hdp_flush(void)
{
   uint32_t remap = rd(0xE4D);
   if (remap && remap != 0xFFFFFFFFu && !(remap & 3)) { wr(remap >> 2, 0); (void)rd(remap >> 2); }
}
static void vw(uint64_t off, uint32_t v)
{
   uint64_t in[2] = {off, v};
   IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, in, 2, NULL, NULL);
}
static uint32_t vr(uint64_t off)
{
   uint64_t in[2] = {off, 4}; uint32_t buf[4] = {0}; size_t sz = sizeof(buf);
   IOConnectCallMethod(conn, SEL_DBG_VRAM_READ, in, 2, NULL, 0, NULL, NULL, buf, &sz);
   return buf[0];
}


#define VRAM_MC 0x8000000000ull          // MC address of VRAM offset 0
#define GART_VA 0x7FFF00000000ull            // VA = GART_VA + vram offset (first 128 MiB)
// navi31: dGPU — VRAM "physical" = VRAM offset (GCMC_VM_FB_OFFSET reads 0; Linux PT base 0x4_feb00001 = offset).
// Work area moved above the GOP framebuffer, the PSP fw_buf (128-144 MiB) and the SMU tables (144-146 MiB), and
// still inside the 256 MiB BAR0 macOS gives us: GART table at 160 MiB, rings/MQDs/test buffers from 161 MiB.
#define GART_PT 0xA000000ull             // flat PTE table (VRAM offset), 512 KiB
#define GART_N 65536                     // 256 MiB window: covers the whole BAR0-visible VRAM
#define FB_PA 0x0ull                     // navi31: no UMA carve-out

#define SEG0(o) (0x1260u + (o))
#define SEG1(o) (0xA000u + (o))
// VRAM layout of the experiment (offsets), all inside the GART window
#define KIQ_RING 0xA100000ull            // 4 KiB ring
#define KIQ_WB   0xA101000ull            // +0 rptr report, +16 wptr poll
#define KIQ_MQD  0xA102000ull            // 2 KiB
#define KIQ_EOP  0xA103000ull            // 2 KiB
#define SCH_RING 0xA110000ull
#define SCH_WB   0xA111000ull
#define SCH_MQD  0xA112000ull
#define SCH_EOP  0xA113000ull
#define TESTBUF  0xA120000ull
#define SAFE_PAGE 0xA3FF000ull           // navi31: default / fault page, inside our work area

// navi31: sudo drops the environment, so every MESBOOT_* switch may also be given as a command-line word.
static int g_argc; static char **g_argv;
static int env_on(const char *n)
{
   const char *v = getenv(n);
   if (v && *v && *v != '0') return 1;
   for (int i = 2; i < g_argc; i++) if (!strcmp(g_argv[i], n)) return 1;
   return 0;
}
static int use_mc;
#define VA(off) ((use_mc ? VRAM_MC : GART_VA) + (off))
#define MC(off) (VRAM_MC + (off))

static void gart(void)
{
   for (uint64_t i = 0; i < GART_N; i++) {
      uint64_t pte = (FB_PA + i * 4096) | ((getenv("PTE_SYS") && *getenv("PTE_SYS")) ? 0x77 : 0x71);      // VALID | EXEC | READ | WRITE, local FB
      vw(GART_PT + i * 8, (uint32_t)pte);
      vw(GART_PT + i * 8 + 4, (uint32_t)(pte >> 32));
   }
   wr(SEG0(0x1713), (uint32_t)(GART_VA >> 12)); wr(SEG0(0x1714), (uint32_t)(GART_VA >> 44));
   wr(SEG0(0x1733), (uint32_t)((GART_VA + (uint64_t)GART_N * 4096 - 1) >> 12)); wr(SEG0(0x1734), (uint32_t)((GART_VA + (uint64_t)GART_N * 4096 - 1) >> 44));
   wr(SEG0(0x16f3), (uint32_t)((FB_PA + GART_PT) | 1)); wr(SEG0(0x16f4), (uint32_t)((FB_PA + GART_PT) >> 32));   // PDB base = physical (FB_PA + vram offset), as the Linux trace
   // system aperture / AGP / defaults exactly as the Linux trace (gfxhub_v3_0_init_system_aperture_regs)
   wr(SEG0(0x167c), 0); wr(SEG0(0x167b), 0x00ffffff); wr(SEG0(0x167a), 0);      // AGP_BASE/BOT/TOP (AGP off)
   wr(SEG0(0x167d), 0x00200000); wr(SEG0(0x167e), 0x00213fbf);                  // SYSTEM_APERTURE LOW/HIGH = VRAM MC range (navi31: 20 GiB, Linux trace)
   wr(SEG0(0x15a8), (uint32_t)(SAFE_PAGE >> 12));   /* navi31: our own VRAM page */ wr(SEG0(0x15a9), 0);                           // SYSTEM_APERTURE_DEFAULT_ADDR LSB/MSB
   wr(SEG0(0x15cb), (uint32_t)(SAFE_PAGE >> 12));   /* navi31: our own VRAM page */ wr(SEG0(0x15cc), 0);                           // L2 protection fault default addr
   wr(SEG0(0x15c5), 0x00060000);                                                // L2_PROTECTION_FAULT_CNTL2
   wr(SEG0(0x15c4), env_on("FAULT_DEFAULT") ? 0x3ffffffc : 0x00000002);   // debug: no default-page redirect so faults are recorded                                                // L2_PROTECTION_FAULT_CNTL (fault handling on; was missing -> silent drops)
   wr(SEG0(0x15ce), 0xffffffff); wr(SEG0(0x15cf), 0x0000000f); wr(SEG0(0x15d0), 0);  // identity aperture LOW/HIGH (disabled)
   wr(SEG0(0x167f), 0x1859);                 // GCMC_VM_MX_L1_TLB_CNTL
   // GCVM L2 exactly as the working Linux driver programs it (regtrace, 1.558983..)
   wr(SEG0(0x15bc), 0x00080e01);             // GCVM_L2_CNTL
   wr(SEG0(0x15bd), 0x00000003);             // GCVM_L2_CNTL2 (invalidate all L1 TLBs + L2)
   wr(SEG0(0x15be), 0x80130009);             // GCVM_L2_CNTL3
   wr(SEG0(0x15d4), 0x00000001);             // GCVM_L2_CNTL4
   wr(SEG0(0x15da), 0x00003fe0);             // GCVM_L2_CNTL5
   wr(SEG0(0x1688), 0x01fffe01);             // GCVM_CONTEXT0_CNTL: enable, depth 0
   // gfxhub_v3_0_program_invalidation: address range of every invalidation engine = everything (was 0 => flushes did nothing)
   for (int e = 0; e < 18; e++) { wr(0x292f + 2 * e, 0xffffffff); wr(0x2930 + 2 * e, 0x0000001f); }
   // contexts 1..15 (VMIDs) as the Linux driver enables them
   for (int c = 1; c < 16; c++) {
      wr(0x28e8 + c, 0x01fffe07);
      wr(0x2973 + 2 * c, 0); wr(0x2974 + 2 * c, 0);
      wr(0x2993 + 2 * c, 0xffffffff); wr(0x2994 + 2 * c, 0x0000000f);
   }
   // SH_MEM_CONFIG / SH_MEM_BASES per VMID as in gfx_v11_0_constants_init (trace 1.559162..)
   for (int v = 0; v < 16; v++) {
      wr(SEG1(0x900), v << 4);
      wr(0xa9e4, 0x0000c00c);
      if (v) wr(0xa9e3, v < 8 ? 0x20001000 : 0x00010002);
   }
   wr(SEG1(0x900), 0);
   wr(SEG0(0x1e1f), rd(SEG0(0x1e1f)) | 0x8000);   // CP_DEBUG.CPG_UTCL1_ERROR_HALT_DISABLE (linux sets it with the gfxhub)
   wr(SEG0(0x16ab + 17), 0x00F80001);          // GCVM_INVALIDATE_ENG17_REQ (the engine amdgpu uses for gmc flushes)
   usleep(2000);
   printf("gart: ack17=0x%x ctx0=0x%x\n", rd(SEG0(0x16bd + 17)), rd(SEG0(0x1688)));
}
static void grbm_select(uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid)
{
   wr(SEG1(0x900), pipe | (me << 2) | (vmid << 4) | (queue << 8));
}
static void mes_start(void)
{
   // uc start from fw headers: lo=0xF0005000? taken from the header words (same for sched + kiq)
   const uint64_t uc = 0xFFFFFFFFF0005000ull >> 2;
   wr(SEG1(0x2807), rd(SEG1(0x2807)) | (1u << 16) | (1u << 17));   // PIPE0/1 RESET (navi31: RMW as Linux, 0x40030000)
   for (int p = 0; p < 2; p++) {
      grbm_select(3, p, 0, 0);
      wr(SEG1(0x2800), (uint32_t)uc);
      wr(SEG1(0x289d), (uint32_t)(uc >> 32));
   }
   grbm_select(0, 0, 0, 0);
   wr(SEG1(0x2807), (1u << 26) | (1u << 27));                // PIPE0/1 ACTIVE
   usleep(2000);
   printf("mes: CP_MES_CNTL=0x%x\n", rd(SEG1(0x2807)));
}
static void mqd_build(uint64_t mqd, uint64_t ring, uint64_t wb, uint64_t eop, int doorbell, uint32_t db_index)
{
   for (int i = 0; i < 512; i++) vw(mqd + i * 4, 0);
   #define M(i, v) vw(mqd + (i) * 4, (uint32_t)(v))
   M(0, 0xC0310800);
   M(11, 1);                                                  // compute_pipelinestat_enable
   M(23, 0xffffffff); M(24, 0xffffffff); M(26, 0xffffffff); M(27, 0xffffffff);
   M(32, 7);                                                  // compute_misc_reserved
   M(128, VA(mqd) & 0xfffffffc); M(129, VA(mqd) >> 32);
   M(130, 1);                                                 // hqd_active
   M(131, 0);                                                 // hqd_vmid
   M(132, (0x0be05501u & ~(0xffu << 8)) | (0x55u << 8));      // persistent_state, PRELOAD_SIZE=0x55
   M(135, 0);
   M(136, VA(ring) >> 8); M(137, VA(ring) >> 40);
   M(138, 0);
   M(139, VA(wb) & 0xfffffffc); M(140, (VA(wb) >> 32) & 0xffff);
   M(141, VA(wb + 16) & 0xfffffff8); M(142, (VA(wb + 16) >> 32) & 0xffff);
   uint32_t db = 0;
   if (doorbell) db = (db_index << 2) | (1u << 30);
   M(143, db);
   // PQ_CONTROL: default 0x00308509; QUEUE_SIZE=log2(4096/4)-1=9, RPTR_BLOCK_SIZE=(log2(4096/4)-1)<<8 (=9<<8), UNORD=1,PRIV=1,KMD=1,NO_UPDATE_RPTR=1
   uint32_t pq = 0x00308509;
   pq = (pq & ~0xffu) | 9u;
   pq = (pq & ~(0x3fu << 8)) | (9u << 8);
   pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27);
   pq &= ~(1u << 29);
   M(145, pq);
   M(149, 0x00300000); M(150, 0);
   M(162, 0x00000100);                                        // cp_mqd_control (VMID 0)
   M(165, VA(eop) >> 8); M(166, VA(eop) >> 40);
   M(167, 8);                                                 // EOP_SIZE = log2(2048/4)-1
   M(182, 0); M(183, 0);
   #undef M
}
static void hqd_program(uint32_t me, uint32_t pipe, uint64_t mqd, uint64_t ring, uint64_t wb, uint32_t pq, uint32_t db)
{
   grbm_select(me, pipe, 0, 0);
   wr(SEG0(0x1fab), 0);                                       // deactivate a previously programmed queue
   wr(SEG0(0x1fb3), 0); wr(SEG0(0x323f), 0); wr(SEG0(0x3240), 0);
   wr(SEG0(0x1fac), 0);                                       // HQD_VMID
   wr(SEG0(0x1fb8), 0);                                       // doorbell off while programming
   wr(SEG0(0x1fa9), (uint32_t)(VA(mqd) & 0xfffffffc)); wr(SEG0(0x1faa), (uint32_t)(VA(mqd) >> 32));
   wr(SEG0(0x1fcb), 0);
   wr(SEG0(0x1fb1), (uint32_t)(VA(ring) >> 8)); wr(SEG0(0x1fb2), (uint32_t)(VA(ring) >> 40));
   wr(SEG0(0x1fb4), (uint32_t)(VA(wb) & 0xfffffffc)); wr(SEG0(0x1fb5), (uint32_t)((VA(wb) >> 32) & 0xffff));
   wr(SEG0(0x1fba), pq);
   wr(SEG0(0x1fb6), (uint32_t)(VA(wb + 16) & 0xfffffff8)); wr(SEG0(0x1fb7), (uint32_t)((VA(wb + 16) >> 32) & 0xffff));
   wr(SEG0(0x1fb8), db);
   wr(SEG0(0x1fad), (0x0be05501u & ~(0xffu << 8)) | (0x55u << 8));
   wr(SEG0(0x1fab), 1);                                       // HQD_ACTIVE
   grbm_select(0, 0, 0, 0);
}
static int use_db;
static void kiq_test(void)
{
   // RLC_CP_SCHEDULERS: KIQ = me3 pipe1 queue0
   uint32_t t = rd(SEG1(0x98a)) & 0xffffff00;
   t |= (3u << 5) | (1u << 3);
   wr(SEG1(0x98a), t); wr(SEG1(0x98a), t | 0x80);
   mqd_build(KIQ_MQD, KIQ_RING, KIQ_WB, KIQ_EOP, use_db, 0x18);
   uint32_t pq = 0x00308509; pq = (pq & ~0xffu) | 9u; pq = (pq & ~(0x3fu << 8)) | (9u << 8);
   pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27); pq &= ~(1u << 29);
   hqd_program(3, 1, KIQ_MQD, KIQ_RING, KIQ_WB, pq, use_db ? ((0x18u << 2) | (1u << 30)) : 0);
   // ring: WRITE_DATA to TESTBUF (VA) magic
   for (int i = 0; i < 4; i++) vw(TESTBUF + i * 4, 0);
   vw(KIQ_RING + 0, 0xC0033700); vw(KIQ_RING + 4, 0x00100500);
   vw(KIQ_RING + 8, (uint32_t)VA(TESTBUF)); vw(KIQ_RING + 12, (uint32_t)(VA(TESTBUF) >> 32));
   vw(KIQ_RING + 16, 0x4B1D0001);
   // kick: MMIO wptr (doorbell disabled) under GRBM select me3/pipe1
   if (use_db) {
      vw(KIQ_WB + 16, 5); vw(KIQ_WB + 20, 0);
      kern_return_t kr = db(0x18 * 4, 5);
      printf("doorbell write kr=0x%x\n", kr);
   } else {
      grbm_select(3, 1, 0, 0);
      wr(SEG0(0x323f), 5); wr(SEG0(0x3240), 0);
   }
   usleep(100000);
   // second test: SET_UCONFIG_REG SCRATCH_REG0 = 0xDEADBEEF (MMIO side effect, no memory involved)
   wr(SEG1(0x2040), 0xCAFEDEAD);
   vw(KIQ_RING + 20, 0xC0017900); vw(KIQ_RING + 24, 0x40); vw(KIQ_RING + 28, 0xDEADBEEF);
   if (use_db) { vw(KIQ_WB + 16, 8); db(0x18 * 4, 8); }
   usleep(100000);
   printf("scratch0=0x%08x (expect deadbeef)\n", rd(SEG1(0x2040)));
   // map the SCHED pipe's queue through the KIQ: MAP_QUEUES(me=2 eng_sel=5 pipe0 queue0)
   mqd_build(SCH_MQD, SCH_RING, SCH_WB, SCH_EOP, 1, 0x16);
   for (int i = 0; i < 4; i++) { vw(SCH_WB + i * 4, 0); vw(SCH_WB + 16 + i * 4, 0); }
   vw(KIQ_RING + 32, 0xC0055E00);
   vw(KIQ_RING + 36, (2u << 18) | (5u << 26) | (1u << 29));
   vw(KIQ_RING + 40, 0x16u << 2);
   vw(KIQ_RING + 44, (uint32_t)VA(SCH_MQD)); vw(KIQ_RING + 48, (uint32_t)(VA(SCH_MQD) >> 32));
   vw(KIQ_RING + 52, (uint32_t)VA(SCH_WB + 16)); vw(KIQ_RING + 56, (uint32_t)(VA(SCH_WB + 16) >> 32));
   if (use_db) { vw(KIQ_WB + 16, 15); db(0x18 * 4, 15); }
   usleep(200000);
   grbm_select(3, 0, 0, 0);
   printf("sched hqd: active=%u pq_base=0x%x rptr=%u dbctl=0x%x\n", rd(SEG0(0x1fab)), rd(SEG0(0x1fb1)), rd(SEG0(0x1fb3)), rd(SEG0(0x1fb8)));
   grbm_select(3, 1, 0, 0);
   printf("kiq rptr after map=%u\n", rd(SEG0(0x1fb3)));
   grbm_select(3, 1, 0, 0);
   uint32_t rp = rd(SEG0(0x1fb3)), act = rd(SEG0(0x1fab)), wp = rd(SEG0(0x323f));
   grbm_select(0, 0, 0, 0);
   printf("kiq: active=%u rptr=%u wptr=%u target=0x%08x wb_rptr=0x%08x\n", act, rp, wp, vr(TESTBUF), vr(KIQ_WB));
}

static void mesdump(void)
{
   for (int p = 0; p < 2; p++) {
      grbm_select(3, p, 0, 0);
      printf("pipe%d: IC_BASE=%08x:%08x MDBASE=%08x:%08x PC_START=%08x INSTR_PNTR=%08x HDR=%08x GP0=%08x GP1=%08x MIBOUND=%08x\n", p,
             rd(SEG1(0x5851)), rd(SEG1(0x5850)), rd(SEG1(0x5855)), rd(SEG1(0x5854)), rd(SEG1(0x2800)),
             rd(SEG1(0x2813)), rd(SEG1(0x280d)), rd(SEG1(0x2843)), rd(SEG1(0x2845)), rd(SEG1(0x585b)));
   }
   grbm_select(0, 0, 0, 0);
   printf("CP_MES_CNTL=%08x RLC_CP_SCHEDULERS=%08x\n", rd(SEG1(0x2807)), rd(SEG1(0x98a)));
}

// ---- MES scheduler pipe (pipe0) driven by direct HQD programming + API ring ----
static uint32_t sch_wptr;      // in dwords
static uint64_t sch_seq;

static int skip_hqd;
static void sched_init(void)
{
   if (!skip_hqd) mqd_build(SCH_MQD, SCH_RING, SCH_WB, SCH_EOP, 1, 0x16);
   for (int i = 0; i < 1024; i++) vw(SCH_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(SCH_WB + i * 4, 0);
   uint32_t pq = 0x00308509; pq = (pq & ~0xffu) | 9u; pq = (pq & ~(0x3fu << 8)) | (9u << 8);
   pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27); pq &= ~(1u << 29);
   if (!skip_hqd) hqd_program(3, 0, SCH_MQD, SCH_RING, SCH_WB, pq, (0x16u << 2) | (1u << 30));
   sch_wptr = 0; sch_seq = 0;
   grbm_select(3, 0, 0, 0);
   printf("sched: hqd active=%u pq_base=0x%x\n", rd(SEG0(0x1fab)), rd(SEG0(0x1fb1)));
   grbm_select(0, 0, 0, 0);
}
static int mes_submit(void *pkt, size_t size, size_t api_status_off, const char *what)
{
   uint32_t *dw = pkt;
   uint64_t status_va = VA(SCH_WB + 64);
   for (int i = 0; i < 4; i++) vw(SCH_WB + 64 + i * 4, 0);
   *(uint64_t *)((char *)pkt + api_status_off) = status_va;
   *(uint64_t *)((char *)pkt + api_status_off + 8) = 1;
   union MESAPI__QUERY_MES_STATUS q;
   memset(&q, 0, sizeof(q));
   q.header.type = MES_API_TYPE_SCHEDULER;
   q.header.opcode = MES_SCH_API_QUERY_SCHEDULER_STATUS;
   q.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   sch_seq++;
   q.api_status.api_completion_fence_addr = VA(SCH_WB + 32);
   q.api_status.api_completion_fence_value = sch_seq;
   uint32_t *qd = (uint32_t *)&q;
   for (size_t i = 0; i < size / 4; i++) vw(SCH_RING + (sch_wptr + i) * 4, dw[i]);
   for (size_t i = 0; i < sizeof(q) / 4; i++) vw(SCH_RING + (sch_wptr + size / 4 + i) * 4, qd[i]);
   sch_wptr += (uint32_t)((size + sizeof(q)) / 4);
   vw(SCH_WB + 16, sch_wptr); vw(SCH_WB + 20, 0);          // wptr shadow (polled by MES) before the doorbell
   db(0x16 * 4, sch_wptr);
   for (int t = 0; t < 400; t++) {
      usleep(5000);
      if (vr(SCH_WB + 32) == (uint32_t)sch_seq) break;
   }
   grbm_select(3, 0, 0, 0);
   printf("  sched hqd: rptr=%u wptr_lo=%u dbctl=0x%08x active=%u\n", rd(SEG0(0x1fb3)), rd(SEG0(0x323f)), rd(SEG0(0x1fb8)), rd(SEG0(0x1fab)));
   grbm_select(0, 0, 0, 0);
   uint32_t f = vr(SCH_WB + 32), st = vr(SCH_WB + 64);
   printf("mes %s: fence=%u (want %llu) status=%u -> %s\n", what, f, (unsigned long long)sch_seq, st, (f == sch_seq && st) ? "OK" : "TIMEOUT");
   return !(f == sch_seq && st);
}
#define CMP_RING 0xA140000ull
#define CMP_WB   0xA141000ull
#define CMP_MQD  0xA142000ull
#define CMP_EOP  0xA143000ull
static void vm8_test(void);
static void gfx_via_mes(void);
static void compute_via_mes(void)
{
   for (int i = 0; i < 1024; i++) vw(CMP_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(CMP_WB + i * 4, 0);
   for (int i = 0; i < 4; i++) vw(TESTBUF + i * 4, 0);
   mqd_build(CMP_MQD, CMP_RING, CMP_WB, CMP_EOP, 1, 6);
   union MESAPI__ADD_QUEUE a;
   memset(&a, 0, sizeof(a));
   a.header.type = MES_API_TYPE_SCHEDULER;
   a.header.opcode = MES_SCH_API_ADD_QUEUE;
   a.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   a.pipe_id = 0; a.queue_id = 0;
   a.doorbell_offset = 6;
   a.mqd_addr = VA(CMP_MQD);
   a.wptr_addr = VA(CMP_WB + 16);
   a.queue_type = MES_QUEUE_TYPE_COMPUTE;
   a.map_legacy_kq = 1;
   if (mes_submit(&a, sizeof(a), offsetof(union MESAPI__ADD_QUEUE, api_status), "ADD_QUEUE(compute, map_legacy_kq)")) return;
   wr(SEG1(0x2040), 0xCAFEDEAD);
   vw(CMP_RING + 0, 0xC0017900); vw(CMP_RING + 4, 0x40); vw(CMP_RING + 8, 0xDEADBEEF);
   vw(CMP_RING + 12, 0xC0033700); vw(CMP_RING + 16, 0x00100500);
   vw(CMP_RING + 20, (uint32_t)VA(TESTBUF)); vw(CMP_RING + 24, (uint32_t)(VA(TESTBUF) >> 32)); vw(CMP_RING + 28, 0xFEEDF00D);
   vw(CMP_WB + 16, 8); vw(CMP_WB + 20, 0);
   db(6 * 4, 8);
   for (int i = 0; i < 50; i++) { usleep(10000); if (rd(SEG1(0x2040)) == 0xDEADBEEF && vr(TESTBUF) == 0xFEEDF00D) break; }
   grbm_select(1, 0, 0, 0);
   printf("compute: scratch0=0x%08x (want deadbeef) testbuf=0x%08x (want feedf00d) hqd_active=%u rptr=%u\n", rd(SEG1(0x2040)), vr(TESTBUF), rd(SEG0(0x1fab)), rd(SEG0(0x1fb3)));
   grbm_select(0, 0, 0, 0);
   if (env_on("ADOPT")) {   // hand the working compute queue to the kext: SUBMIT from RADV goes here from now on
      uint64_t in[6] = {CMP_RING, 1024, CMP_WB, 6 * 4, 8, 0xA150000};
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_ADOPTQ, in, 6, NULL, NULL);
      printf("adopt queue -> kr 0x%x\n", kr);
      return;
   }
   if (env_on("VM8")) { vm8_test(); return; }
   if (getenv("MESBOOT_VMT") && *getenv("MESBOOT_VMT")) {
      // GPU write into SYSTEM memory through a context0 (GART) mapping, PTE SYSTEM|SNOOPED
      uint64_t phys = 0, len = 0, val = 0;
      kern_return_t kr = sm(1, 1 << 20, 0, &phys, &len);
      printf("sysmem alloc kr=0x%x phys=0x%llx len=0x%llx\n", kr, (unsigned long long)phys, (unsigned long long)len);
      if (kr) return;
      if (env_on("AGPTEST")) {   // can the GPU write system memory at all? identity AGP aperture MC==physical, no page tables
         wr(SEG0(0x167c), 0); wr(SEG0(0x167b), 0); wr(SEG0(0x167a), 0x000001ff);       // AGP BASE=0, BOT=0, TOP=0x1ff (8 GiB, 16 MiB units)
         if (env_on("CTX0_OFF")) { wr(SEG0(0x1688), 0x01fffe00); wr(SEG0(0x16ab + 17), 0x00F80001); usleep(2000); }   // MMHUB-like: no context0 translation
         uint64_t tgt = phys + 0x80000ull + 0x2000;                                     // inside the 1 MiB buffer
         uint32_t b = 8;
         vw(CMP_RING + b * 4 + 0, 0xC0033700); vw(CMP_RING + b * 4 + 4, 0x00100500);
         vw(CMP_RING + b * 4 + 8, (uint32_t)tgt); vw(CMP_RING + b * 4 + 12, (uint32_t)(tgt >> 32)); vw(CMP_RING + b * 4 + 16, 0xA6A6A6A6);
         b += 5;
         vw(CMP_RING + b * 4 + 0, 0xC0065800); vw(CMP_RING + b * 4 + 4, 0);                       // ACQUIRE_MEM: GL2 writeback+invalidate
         vw(CMP_RING + b * 4 + 8, 0xFFFFFFFF); vw(CMP_RING + b * 4 + 12, 0x00FFFFFF);
         vw(CMP_RING + b * 4 + 16, 0); vw(CMP_RING + b * 4 + 20, 0);
         vw(CMP_RING + b * 4 + 24, 0x0A); vw(CMP_RING + b * 4 + 28, 0xC000);
         b += 8;
         vw(CMP_WB + 16, b); vw(CMP_WB + 20, 0); db(6 * 4, b); usleep(300000);
         // GPU READ tests with COPY_DATA (src_sel=2 mem via L2, dst_sel=5 mem), first VRAM->VRAM to validate the encoding, then system->VRAM
         {
            uint64_t pp0, vv0; sm(3, 0x80000ull + 0x3000, 0x5A5A5A5A, &pp0, &vv0);
            vw(TESTBUF + 0x100, 0x77777777); vw(TESTBUF + 0x110, 0); vw(TESTBUF + 0x114, 0);
            struct { uint64_t src, dst; } cp[2] = { { MC(TESTBUF + 0x100), MC(TESTBUF + 0x110) }, { tgt + 0x1000, MC(TESTBUF + 0x114) } };
            for (int k = 0; k < 2; k++) {
               vw(CMP_RING + b * 4 + 0, 0xC0044000); vw(CMP_RING + b * 4 + 4, 2 | (5u << 8) | (1u << 20));
               vw(CMP_RING + b * 4 + 8, (uint32_t)cp[k].src); vw(CMP_RING + b * 4 + 12, (uint32_t)(cp[k].src >> 32));
               vw(CMP_RING + b * 4 + 16, (uint32_t)cp[k].dst); vw(CMP_RING + b * 4 + 20, (uint32_t)(cp[k].dst >> 32));
               b += 6; vw(CMP_WB + 16, b); db(6 * 4, b); usleep(300000);
               printf("COPY_DATA test %d (%s): dst=0x%08x\n", k, k ? "system->VRAM" : "VRAM->VRAM", vr(TESTBUF + (k ? 0x114 : 0x110)));
            }
         }
         uint64_t pp, v = 0; sm(2, 0x80000ull + 0x2000, 0, &pp, &v);
         printf("AGP identity write to system memory phys 0x%llx: value=0x%08x (want a6a6a6a6)\n", (unsigned long long)tgt, (uint32_t)v);
         return;
      }
      const uint64_t vaoff = 0x7F00000;
      const int vram_t = getenv("MESBOOT_VMT")[0] == '2';
      const int pt_sys = getenv("PT_SYS") && *getenv("PT_SYS") == '1';   // page table itself in system memory
      uint64_t sysphys = phys;
      if (vram_t) phys = FB_PA + 0xA200000ull;
      else if (pt_sys) phys = sysphys + 0x80000ull;                      // targets after the table region
      uint32_t wp = 8, hit = 0;
      static const uint64_t lowf[] = {0x77, 0x71, 0x73, 0x75, 0x7f};
      for (int mt = 0; mt < 5; mt++) {
         for (int fl = 0; fl < 5; fl++) {
            int idx = mt * 5 + fl;
            uint64_t pte = (phys + idx * 4096ull) | lowf[fl] | ((uint64_t)mt << 48) | (getenv("PTE_EXTRA") && *getenv("PTE_EXTRA") ? strtoull(getenv("PTE_EXTRA"), 0, 0) : 0);
            uint64_t slot = (vaoff >> 12) + idx;
            if (pt_sys) { uint64_t d0, d1; sm(3, slot * 8, (uint32_t)pte, &d0, &d1); sm(3, slot * 8 + 4, (uint32_t)(pte >> 32), &d0, &d1); }
            else { vw(GART_PT + slot * 8, (uint32_t)pte); vw(GART_PT + slot * 8 + 4, (uint32_t)(pte >> 32)); }
         }
      }
      if (pt_sys) {
         wr(SEG0(0x16f3), (uint32_t)((sysphys & ~0xfffull) | (getenv("ROOT_FL") && *getenv("ROOT_FL") ? strtoul(getenv("ROOT_FL"), 0, 0) : 3))); wr(SEG0(0x16f4), (uint32_t)(sysphys >> 32));   // PDB base in SYSTEM memory
         printf("PT in system memory at phys 0x%llx\n", (unsigned long long)sysphys);
      }
      wr(SEG0(0x16ab + 17), 0x00F80001); usleep(2000);       // invalidate (eng17)
      const char *vo = getenv("VARONLY"); const int only = (vo && *vo) ? atoi(vo) : -1;
      for (int idx = 0; idx < 25; idx++) {
         if (only >= 0 && idx != only) continue;
         uint64_t gva = GART_VA + vaoff + idx * 4096ull;
         vw(CMP_RING + wp * 4 + 0, 0xC0033700); vw(CMP_RING + wp * 4 + 4, 0x00100500);
         vw(CMP_RING + wp * 4 + 8, (uint32_t)gva); vw(CMP_RING + wp * 4 + 12, (uint32_t)(gva >> 32)); vw(CMP_RING + wp * 4 + 16, 0xABCD0000 + idx);
         wp += 5;
      }
      if (!env_on("NO_GCR")) {   // write back + invalidate the GPU L2 so cached (non-UC) writes reach memory
         vw(CMP_RING + wp * 4 + 0, 0xC0065800); vw(CMP_RING + wp * 4 + 4, 0);
         vw(CMP_RING + wp * 4 + 8, 0xFFFFFFFF); vw(CMP_RING + wp * 4 + 12, 0x00FFFFFF);
         vw(CMP_RING + wp * 4 + 16, 0); vw(CMP_RING + wp * 4 + 20, 0);
         vw(CMP_RING + wp * 4 + 24, 0x0A); vw(CMP_RING + wp * 4 + 28, 0xC000);
         wp += 8;
      }
      vw(CMP_WB + 16, wp); vw(CMP_WB + 20, 0);
      db(6 * 4, wp);
      usleep(300000);
      for (int idx = 0; idx < 25; idx++) {
         if (only >= 0 && idx != only) continue;
         uint64_t pp, val = 0; if (vram_t) val = vr(0xA200000 + idx * 4096ull); else sm(2, (pt_sys ? 0x80000ull : 0) + idx * 4096ull, 0, &pp, &val);
         if ((uint32_t)val == 0xABCD0000u + idx) { printf("  HIT: mtype=%d flags=0x%llx\n", idx / 5, (unsigned long long)lowf[idx % 5]); hit++; }
      }
      // read test: CPU puts a magic into system page 0, GPU COPY_DATA gart-VA -> MC testbuf
      if (env_on("COPYTEST")) {
         uint64_t pp, vv; if (vram_t) vw(0xA200000, 0xC0FFEE11); else sm(3, pt_sys ? 0x80000ull : 0, 0xC0FFEE11, &pp, &vv);
         vw(TESTBUF + 8, 0);
         uint64_t src = GART_VA + vaoff, dst = MC(TESTBUF + 8);
         uint32_t b = wp;
         vw(CMP_RING + b * 4 + 0, 0xC0044000); vw(CMP_RING + b * 4 + 4, 2 | (2u << 8) | (1u << 20));
         vw(CMP_RING + b * 4 + 8, (uint32_t)src); vw(CMP_RING + b * 4 + 12, (uint32_t)(src >> 32));
         vw(CMP_RING + b * 4 + 16, (uint32_t)dst); vw(CMP_RING + b * 4 + 20, (uint32_t)(dst >> 32));
         wp += 6; vw(CMP_WB + 16, wp); db(6 * 4, wp); usleep(200000);
         printf("GART read test: testbuf+8 = 0x%08x (want c0ffee11)\n", vr(TESTBUF + 8));
      }
      grbm_select(1, 0, 0, 0);
      printf("compute rptr=%u wptr_written=%u fault status=0x%x addr=0x%x:%x\n", rd(SEG0(0x1fb3)), wp, rd(SEG0(0x15c8)), rd(SEG0(0x15ca)), rd(SEG0(0x15c9)));
      grbm_select(0, 0, 0, 0);
      if (!vram_t) {   // where did the GPU writes go? scan the first dword of every page of the 1 MiB buffer
         int nz = 0;
         for (int pg = 0; pg < 256; pg++) { uint64_t pp, v = 0; sm(2, pg * 4096ull, 0, &pp, &v);
            if (pg >= 0x80 || !pt_sys) { if ((uint32_t)v) { printf("  page %d: 0x%08x\n", pg, (uint32_t)v); nz++; } } }
         printf("scan: %d non-zero target pages\n", nz);
      }
      printf("system-memory write via GART: %u hits of 25 variants (value=0x%08x)\n", hit, hit ? 0xabcd1234 : 0);
   }
}

// VMID 8 user context: 4-level page table in system memory, IB + target in VRAM pages, IB executed from the kernel compute ring
static void vm8_test(void)
{
   uint64_t phys = 0, len = 0, d0, d1;
   if (sm(1, 1 << 20, 0, &phys, &len)) { printf("vm8: sysmem alloc failed\n"); return; }
   // navi31: VM8_PTVRAM puts the 4 table levels in VRAM (offset == physical on this dGPU) with local PDEs, as
   // Linux keeps page tables; default keeps nullarc's system-memory tables (PDE VALID|SYSTEM|SNOOPED).
   const int ptv = env_on("VM8_PTVRAM");
   const uint64_t PTV = 0xA1C0000ull;
   const uint64_t T2 = ptv ? PTV : phys, T1 = T2 + 0x1000, T0 = T2 + 0x2000, TP = T2 + 0x3000;
   const uint64_t pde = ptv ? 1 : 7, root = ptv ? 1 : 3;
   if (ptv) for (int i = 0; i < 4 * 1024; i++) vw(PTV + i * 4, 0);
   const uint64_t VA_IB = env_on("VM8_HI") ? 0x800100000000ull : 0x100000000ull, VA_T = VA_IB + 0x1000;   // VM8_HI: upper half (bit 47), like RADV
   const uint64_t IBO = 0xA190000ull, TGO = 0xA191000ull;           // VRAM offsets backing the two pages (navi31: work area, clear of the GOP framebuffer)
   const unsigned mt = getenv("VM8_MT") ? (unsigned)atoi(getenv("VM8_MT")) : 4;
   const uint64_t flags = 0x71 | ((uint64_t)mt << 48);
   #define PUT(off, val) do { uint64_t v_ = (val); if (ptv) { vw(PTV + (off), (uint32_t)v_); vw(PTV + (off) + 4, (uint32_t)(v_ >> 32)); } \
                              else { sm(3, (off), (uint32_t)v_, &d0, &d1); sm(3, (off) + 4, (uint32_t)(v_ >> 32), &d0, &d1); } } while (0)
   const int V = 8;
   const int flat = env_on("VM8_FLAT");
   if (flat) {   // depth 0: one flat PTB covering 512 pages from VA_IB, like the working GART
      PUT(0x0000 + 0 * 8, (FB_PA + IBO) | flags);
      PUT(0x0000 + 1 * 8, (FB_PA + TGO) | flags);
      wr(SEG0(0x16f3 + 2 * V), (uint32_t)(T2 | root)); wr(SEG0(0x16f4 + 2 * V), (uint32_t)(T2 >> 32));
      wr(SEG0(0x1688 + V), 0x01fffe01);
      wr(SEG0(0x1713 + 2 * V), (uint32_t)(VA_IB >> 12)); wr(SEG0(0x1714 + 2 * V), (uint32_t)(VA_IB >> 44));
      wr(SEG0(0x1733 + 2 * V), (uint32_t)((VA_IB >> 12) + 511)); wr(SEG0(0x1734 + 2 * V), (uint32_t)(VA_IB >> 44));
   } else {
      PUT(0x0000 + ((VA_IB >> 39) & 0x1ff) * 8, T1 | pde);
      PUT(0x1000 + ((VA_IB >> 30) & 0x1ff) * 8, T0 | pde);                     // gmc_v11: translate_further=false -> plain PDE
      PUT(0x2000 + ((VA_IB >> 21) & 0x1ff) * 8, TP | pde);
      if (env_on("VM8_IBSYS")) PUT(0x3000 + ((VA_IB >> 12) & 0x1ff) * 8, (phys + 0x20000) | 0x77 | ((uint64_t)mt << 48));   // IB page in SYSTEM memory
      else PUT(0x3000 + ((VA_IB >> 12) & 0x1ff) * 8, (FB_PA + IBO) | flags);
      if (env_on("VM8_SYS")) PUT(0x3000 + ((VA_T >> 12) & 0x1ff) * 8, (phys + 0x10000) | 0x77 | ((uint64_t)mt << 48));   // target = SYSTEM page
      else PUT(0x3000 + ((VA_T >> 12) & 0x1ff) * 8, (FB_PA + TGO) | flags);
      wr(SEG0(0x16f3 + 2 * V), (uint32_t)(T2 | root)); wr(SEG0(0x16f4 + 2 * V), (uint32_t)(T2 >> 32));
      wr(SEG0(0x1688 + V), 0x01fffe07);
      wr(SEG0(0x1713 + 2 * V), 0); wr(SEG0(0x1714 + 2 * V), 0);
      wr(SEG0(0x1733 + 2 * V), 0xffffffff); wr(SEG0(0x1734 + 2 * V), 0xf);
   }
   #undef PUT
   wr(SEG0(0x16ab + 17), 0x00F80000 | (1u << V) | 1u); usleep(2000);
   // IB: WRITE_DATA to VA_T
   const int ib0 = env_on("IB0");                       // control: same IB packet but VMID 0 + MC addresses (no VM involved)
   // canonical (sign-extended) form for upper-half VAs, as amdgpu passes them: bit 47 replicated into 63:48
   #define SEXT(x) (((x) & (1ull << 47)) ? ((x) | 0xFFFF000000000000ull) : (x))
   const uint64_t ibva = ib0 ? MC(IBO) : SEXT(VA_IB), tva = ib0 ? MC(TGO) : SEXT(VA_T); const int vmid = ib0 ? 0 : V;
   vw(IBO + 0, 0xC0033700); vw(IBO + 4, 0x00100500); vw(IBO + 8, (uint32_t)tva); vw(IBO + 12, (uint32_t)(tva >> 32)); vw(IBO + 16, 0x1BADB002);
   if (env_on("VM8_IBSYS")) {   // same IB, but in the system page the PTE points at
      const uint32_t ibw[5] = {0xC0033700, 0x00100500, (uint32_t)tva, (uint32_t)(tva >> 32), 0x1BADB002};
      for (int i = 0; i < 5; i++) sm(3, 0x20000 + i * 4, ibw[i], &d0, &d1);
   }
   vw(TGO, 0);
   uint32_t b = vr(CMP_WB + 16);   // navi31: continue at the queue's wptr (was a fixed 8)
   printf("vm8: page tables in %s, ring at dw %u\n", ptv ? "VRAM" : "system memory", b);
   vw(CMP_RING + b * 4 + 0, 0xC0023F00); vw(CMP_RING + b * 4 + 4, (uint32_t)ibva); vw(CMP_RING + b * 4 + 8, (uint32_t)(ibva >> 32));
   vw(CMP_RING + b * 4 + 12, 5 | (1u << 23) | ((uint32_t)vmid << 24));   // IB_SIZE | VALID | VMID
   b += 4;
   // GL2 writeback+invalidate so the IB's write leaves the GPU cache (same ACQUIRE_MEM as the working GART test)
   vw(CMP_RING + b * 4 + 0, 0xC0065800); vw(CMP_RING + b * 4 + 4, 0);
   vw(CMP_RING + b * 4 + 8, 0xFFFFFFFF); vw(CMP_RING + b * 4 + 12, 0x00FFFFFF);
   vw(CMP_RING + b * 4 + 16, 0); vw(CMP_RING + b * 4 + 20, 0);
   vw(CMP_RING + b * 4 + 24, 0x0A); vw(CMP_RING + b * 4 + 28, 0xC000);
   b += 8;
   // RELEASE_MEM fence exactly as the kext emits it -> MC 0xA150000
   vw(0xA150000, 0); vw(0xA150004, 0);
   { const uint64_t fm = MC(0xA150000ull); const uint32_t rm[8] = {0xC0064900, 0x06603514, 0x40000000, (uint32_t)fm, (uint32_t)(fm >> 32), 0x77, 0, 0};
     for (int i = 0; i < 8; i++) vw(CMP_RING + (b + i) * 4, rm[i]); b += 8; }
   vw(CMP_WB + 16, b); vw(CMP_WB + 20, 0); db(6 * 4, b);
   const int tsys = env_on("VM8_SYS");
   uint64_t sv = 0;
   for (int i = 0; i < 50; i++) { usleep(10000); if (tsys) { uint64_t pp; sm(2, 0x10000, 0, &pp, &sv); if ((uint32_t)sv == 0x1BADB002) break; } else if (vr(TGO) == 0x1BADB002) break; }
   if (tsys) printf("vm8 SYSTEM target: 0x%08x (want 1badb002)\n", (uint32_t)sv);
   grbm_select(1, 0, 0, 0);
   printf("vm8 RELEASE_MEM fence = 0x%08x (want 77)\n", vr(0xA150000));
   printf("vm8 test (mtype %u): target=0x%08x (want 1badb002) rptr=%u fault=0x%x addr=0x%x:%x\n", mt, vr(TGO), rd(SEG0(0x1fb3)), rd(SEG0(0x15c8)), rd(SEG0(0x15ca)), rd(SEG0(0x15c9)));
   grbm_select(0, 0, 0, 0);
}

static void sched_test(void)
{
   sched_init();
   union MESAPI_SET_HW_RESOURCES r;
   memset(&r, 0, sizeof(r));
   r.header.type = MES_API_TYPE_SCHEDULER;
   r.header.opcode = MES_SCH_API_SET_HW_RSRC;
   r.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   r.vmid_mask_mmhub = 0xffffff00;
   r.vmid_mask_gfxhub = 0xffffff00;
   r.gds_size = 0;
   r.paging_vmid = 0;
   r.g_sch_ctx_gpu_mc_ptr = VA(SCH_WB + 128);
   r.query_status_fence_gpu_mc_ptr = VA(SCH_WB + 136);
   for (int i = 0; i < 4; i++) r.compute_hqd_mask[i] = 0xc;
   r.gfx_hqd_mask[0] = 0xfffffffe;
   r.sdma_hqd_mask[0] = 0xfc;
   // navi31: IP discovery bases of this card (its discovery table) — GC has a 4th segment (0x1C000),
   // MMHUB 3.0.0 starts at 0x1A000; there are 2 SDMA engines.
   const uint32_t gcb[5] = {0x1260, 0xA000, 0x1C000, 0x2402C00, 0}, mmb[5] = {0x1A000, 0x2408800, 0, 0, 0}, osb[5] = {0x10a0, 0x240a000, 0, 0, 0};
   for (int i = 0; i < 5; i++) { r.gc_base[i] = gcb[i]; r.mmhub_base[i] = mmb[i]; r.osssys_base[i] = osb[i]; }
   r.sdma_hqd_mask[1] = 0xfc;
   r.disable_reset = 1; r.disable_mes_log = 1; r.use_different_vmid_compute = 1;
   r.enable_reg_active_poll = 1; r.enable_level_process_quantum_check = 1;
   r.oversubscription_timer = 50;
   mes_submit(&r, sizeof(r), offsetof(union MESAPI_SET_HW_RESOURCES, api_status), "SET_HW_RESOURCES");
   union MESAPI__QUERY_MES_STATUS q;
   memset(&q, 0, sizeof(q));
   q.header.type = MES_API_TYPE_SCHEDULER;
   q.header.opcode = MES_SCH_API_QUERY_SCHEDULER_STATUS;
   q.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   mes_submit(&q, sizeof(q), offsetof(union MESAPI__QUERY_MES_STATUS, api_status), "QUERY_SCHED_STATUS");
   if (env_on("MESBOOT_CS")) compute_via_mes();
   if (env_on("MESBOOT_GFX")) gfx_via_mes();
}

#define MEC_RING 0xA130000ull
#define MEC_WB   0xA131000ull
#define MEC_MQD  0xA132000ull
#define MEC_EOP  0xA133000ull
static void mec_test(void)
{
   printf("MEC: CP_MEC_CNTL=0x%x before\n", rd(SEG1(0x802)));
   wr(SEG1(0x802), 0);                                  // unhalt ME1 + ME2
   usleep(5000);
   printf("MEC: CP_MEC_CNTL=0x%x after, RS64_CNTL=0x%x\n", rd(SEG1(0x802)), rd(SEG1(0x2904)));
   for (int i = 0; i < 1024; i++) vw(MEC_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(MEC_WB + i * 4, 0);
   mqd_build(MEC_MQD, MEC_RING, MEC_WB, MEC_EOP, 1, 6);
   uint32_t pq = 0x00308509; pq = (pq & ~0xffu) | 9u; pq = (pq & ~(0x3fu << 8)) | (9u << 8);
   pq |= (1u << 28) | (1u << 30) | (1u << 31) | (1u << 27); pq &= ~(1u << 29);
   hqd_program(1, 0, MEC_MQD, MEC_RING, MEC_WB, pq, (6u << 2) | (1u << 30));
   wr(SEG1(0x2040), 0xCAFEDEAD);
   vw(MEC_RING + 0, 0xC0017900); vw(MEC_RING + 4, 0x40); vw(MEC_RING + 8, 0xDEADBEEF);
   for (int i = 0; i < 4; i++) vw(TESTBUF + i * 4, 0);
   vw(MEC_RING + 12, 0xC0033700); vw(MEC_RING + 16, 0x00100500);
   vw(MEC_RING + 20, (uint32_t)VA(TESTBUF)); vw(MEC_RING + 24, (uint32_t)(VA(TESTBUF) >> 32)); vw(MEC_RING + 28, 0xFEEDF00D);
   db(6 * 4, 8);
   usleep(100000);
   grbm_select(1, 0, 0, 0);
   printf("MEC hqd: active=%u rptr=%u scratch0=0x%08x (expect deadbeef)\n", rd(SEG0(0x1fab)), rd(SEG0(0x1fb3)), rd(SEG1(0x2040)));
   printf("MEC testbuf=0x%08x (expect feedf00d)\n", vr(TESTBUF));
   grbm_select(0, 0, 0, 0);
}

// GFX (graphics) kernel queue: me0 pipe0 queue0 mapped by MES ADD_QUEUE(type GFX, map_legacy_kq), MQD per gfx_v11_0_gfx_mqd_init.
// Doorbell 0x116 (dword) inside the RB doorbell range Linux programs (LOWER 0x458 / UPPER 0x7f8, byte units).
#define GFX_RING 0xA170000ull            // 8 KiB ring (2048 dw: the clear-state preamble alone is 960)
#define GFX_RDW  2048u
#define GFX_WB   0xA174000ull            // +0 rptr report, +16 wptr poll
#define GFX_MQD  0xA175000ull            // v11_gfx_mqd, 2 KiB
#define GFX_CSB  0xA176000ull            // RLC clear-state buffer (RLC_CSIB_*), as gfx_v11_0_init_csb
#define GFX_DB   0x116u
#include "csb_gfx11.h"
static void gfx_via_mes(void)
{
   for (unsigned i = 0; i < GFX_RDW; i++) vw(GFX_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(GFX_WB + i * 4, 0);
   for (int i = 0; i < 512; i++) vw(GFX_MQD + i * 4, 0);
   const unsigned ncsb = sizeof(kCsbGfx11) / 4;
   for (unsigned i = 0; i < ncsb; i++) vw(GFX_CSB + i * 4, kCsbGfx11[i]);
   wr(SEG1(0x988), (uint32_t)(MC(GFX_CSB) >> 32)); wr(SEG1(0x987), (uint32_t)MC(GFX_CSB) & 0xfffffffc); wr(SEG1(0x989), ncsb);   // RLC_CSIB_ADDR_HI/LO/LENGTH
   vw(TESTBUF + 0x40, 0);
   wr(SEG0(0x1dfa), GFX_DB << 2); wr(SEG0(0x1dfb), 0x7f8);           // CP_RB_DOORBELL_RANGE_LOWER/UPPER
   const uint64_t ring = MC(GFX_RING), rb = ring >> 8, rptr = MC(GFX_WB), wpoll = MC(GFX_WB + 16), mqd = MC(GFX_MQD);
   struct { uint32_t off, val; } m[] = {
      {128, (uint32_t)mqd & 0xfffffffc}, {129, (uint32_t)(mqd >> 32)},   // cp_mqd_base_addr
      {162, 0x00000100},                                                // cp_gfx_mqd_control: PRIV_STATE 1, VMID 0
      {131, 0}, {134, 0}, {135, 0x00000a01},                            // hqd_vmid, queue_priority, quantum (QUANTUM_EN)
      {136, (uint32_t)rb}, {137, (uint32_t)(rb >> 32)},                 // cp_gfx_hqd_base = ring >> 8
      {139, (uint32_t)rptr & 0xfffffffc}, {140, (uint32_t)(rptr >> 32) & 0xffff},
      {141, (uint32_t)wpoll & 0xfffffffc}, {142, (uint32_t)(wpoll >> 32) & 0xffff},
      {145, 0x00a00000 | 10u | (8u << 8) | (1u << 29)},                 // hqd_cntl: RB_BUFSZ 10 (2048 dw), RB_BLKSZ 8, KMD_QUEUE
      {143, (GFX_DB << 2) | (1u << 30)},                                // cp_rb_doorbell_control: offset, DOORBELL_EN
      {138, 0}, {149, 0}, {150, 0},                                     // rptr, wptr, wptr_hi
      {130, 1},                                                         // cp_gfx_hqd_active
   };
   for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) vw(GFX_MQD + m[i].off * 4, m[i].val);
   union MESAPI__ADD_QUEUE a;
   memset(&a, 0, sizeof(a));
   a.header.type = MES_API_TYPE_SCHEDULER;
   a.header.opcode = MES_SCH_API_ADD_QUEUE;
   a.header.dwsize = API_FRAME_SIZE_IN_DWORDS;
   a.pipe_id = 0; a.queue_id = 0;
   a.doorbell_offset = GFX_DB;
   a.mqd_addr = mqd;
   a.wptr_addr = wpoll;
   a.queue_type = MES_QUEUE_TYPE_GFX;
   a.map_legacy_kq = 1;
   if (mes_submit(&a, sizeof(a), offsetof(union MESAPI__ADD_QUEUE, api_status), "ADD_QUEUE(gfx, map_legacy_kq)")) return;
   grbm_select(0, 0, 0, 0);
   printf("gfx hqd after map: active=%u base=0x%x cntl=0x%08x dbctl=0x%08x rptr=%u\n", rd(SEG0(0x1e80)), rd(SEG0(0x1e86)),
          rd(SEG0(0x1e8f)), rd(SEG0(0x1e8d)), rd(SEG0(0x1e88)));
   wr(SEG1(0x2041), 0xCAFEDEAD);
   uint32_t w = 0;
   if (!env_on("NO_CSB")) for (unsigned i = 0; i < ncsb; i++) vw(GFX_RING + w++ * 4, kCsbGfx11[i]);   // gfx_v11_0_cp_gfx_start
   vw(GFX_RING + w++ * 4, 0xC0017900); vw(GFX_RING + w++ * 4, 0x41); vw(GFX_RING + w++ * 4, 0xDEADBEEF);   // SET_UCONFIG_REG scratch1
   vw(GFX_RING + w++ * 4, 0xC0033700); vw(GFX_RING + w++ * 4, 0x00100500);                                   // WRITE_DATA mem, wr confirm
   vw(GFX_RING + w++ * 4, (uint32_t)MC(TESTBUF + 0x40)); vw(GFX_RING + w++ * 4, (uint32_t)(MC(TESTBUF + 0x40) >> 32));
   vw(GFX_RING + w++ * 4, 0x6F7A6F7A);
   vw(GFX_WB + 16, w); vw(GFX_WB + 20, 0);
   db(GFX_DB * 4, w);
   for (int i = 0; i < 50; i++) { usleep(10000); if (rd(SEG1(0x2041)) == 0xDEADBEEF && vr(TESTBUF + 0x40) == 0x6F7A6F7A) break; }
   grbm_select(0, 0, 0, 0);
   printf("gfx: scratch1=0x%08x (want deadbeef) testbuf=0x%08x (want 6f7a6f7a) rptr=%u rptr_wb=%u grbm=0x%08x cp_stat=0x%08x\n",
          rd(SEG1(0x2041)), vr(TESTBUF + 0x40), rd(SEG0(0x1e88)), vr(GFX_WB), rd(0x2004), rd(0x21a0));
   if (env_on("ADOPT_GFX")) {   // hand the gfx queue to the kext as its second queue
      uint64_t in[6] = {GFX_RING, GFX_RDW, GFX_WB, GFX_DB * 4, w, 0xA180000 | 1};
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_ADOPTQ, in, 6, NULL, NULL);
      printf("adopt gfx queue -> kr 0x%x\n", kr);
   }
}

static void sched_test(void);
static void kiq_init_only(void)
{
   uint32_t t = rd(SEG1(0x98a)) & 0xffffff00;
   t |= (3u << 5) | (1u << 3);
   wr(SEG1(0x98a), t); wr(SEG1(0x98a), t | 0x80);
   mqd_build(KIQ_MQD, KIQ_RING, KIQ_WB, KIQ_EOP, 1, 0x18);
   for (int i = 0; i < 1024; i++) vw(KIQ_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(KIQ_WB + i * 4, 0);
   hqd_program(3, 1, KIQ_MQD, KIQ_RING, KIQ_WB, 0xd8308011, (0x18u << 2) | (1u << 30));
   printf("kiq init: RLC_CP_SCHEDULERS=0x%x\n", rd(SEG1(0x98a)));
}
// navi31: the first packets Linux put on this card's KIQ ring (a decoded capture of that ring):
// SET_RESOURCES first, then its own ring test, a WRITE_DATA with dst_sel = register (SCRATCH_REG0 = 0xdeadbeef).
// The KIQ consumed but ignored every packet until it had seen SET_RESOURCES.
static void kiqres_test(void)
{
   kiq_init_only();
   static const uint32_t pkt[] = {
      0xC006A000, 0x000A0000, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 0,   // SET_RESOURCES, payload as Linux sent it
      0xC0033700, 0x00010000, 0x0000C040, 0x00000000, 0xDEADBEEF,   // WRITE_DATA reg SCRATCH_REG0 (Linux ring test)
   };
   const uint32_t n = sizeof(pkt) / 4;
   wr(SEG1(0x2040), 0xCAFEDEAD);
   for (uint32_t i = 0; i < n; i++) vw(KIQ_RING + i * 4, pkt[i]);
   vw(KIQ_WB + 16, n); vw(KIQ_WB + 20, 0);   // wptr poll shadow, then doorbell (db() flushes the HDP first)
   kern_return_t kr = db(0x18 * 4, n);
   for (int t = 0; t < 50 && rd(SEG1(0x2040)) != 0xDEADBEEF; t++) usleep(10000);
   grbm_select(3, 1, 0, 0);
   uint32_t rp = rd(SEG0(0x1fb3)), act = rd(SEG0(0x1fab));
   grbm_select(0, 0, 0, 0);
   printf("kiqres: doorbell kr=0x%x rptr=%u/%u active=%u scratch0=0x%08x (want deadbeef) rptr_report=0x%08x\n",
          kr, rp, n, act, rd(SEG1(0x2040)), vr(KIQ_WB));
}
// navi31: submit packets on the KIQ ring at the wptr kept in its poll shadow (KIQ_WB+16), wait for rptr.
static int kiq_submit(const uint32_t *p, uint32_t n)
{
   uint32_t w = vr(KIQ_WB + 16);
   for (uint32_t i = 0; i < n; i++) vw(KIQ_RING + (w + i) * 4, p[i]);
   w += n;
   vw(KIQ_WB + 16, w); vw(KIQ_WB + 20, 0);
   db(0x18 * 4, w);
   uint32_t rp = 0;
   for (int t = 0; t < 100; t++) {
      grbm_select(3, 1, 0, 0); rp = rd(SEG0(0x1fb3)); grbm_select(0, 0, 0, 0);
      if (rp == w) break;
      usleep(2000);
   }
   printf("kiq submit: %u dw, rptr %u / wptr %u\n", n, rp, w);
   return rp == w ? 0 : -1;
}
// navi31: map the MES scheduler queue (me3 pipe0) through the KIQ exactly as Linux did (its KIQ ring dw 512:
// MAP_QUEUES 34080000 doorbell 0x58), then SET_HW_RESOURCES + QUERY_SCHEDULER_STATUS on the scheduler ring.
// Needs a KIQ that already got SET_RESOURCES (run `kiqres` first in the same boot).
static void schedkiq_test(void)
{
   mqd_build(SCH_MQD, SCH_RING, SCH_WB, SCH_EOP, 1, 0x16);
   for (int i = 0; i < 1024; i++) vw(SCH_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(SCH_WB + i * 4, 0);
   const uint32_t map[] = {0xC005A200, (2u << 18) | (5u << 26) | (1u << 29), 0x16u << 2,
                           (uint32_t)VA(SCH_MQD), (uint32_t)(VA(SCH_MQD) >> 32),
                           (uint32_t)VA(SCH_WB + 16), (uint32_t)(VA(SCH_WB + 16) >> 32)};
   if (kiq_submit(map, 7)) { printf("schedkiq: KIQ did not consume MAP_QUEUES\n"); return; }
   usleep(20000);
   grbm_select(3, 0, 0, 0);
   printf("sched hqd after MAP_QUEUES: active=%u pq_base=0x%x dbctl=0x%x\n", rd(SEG0(0x1fab)), rd(SEG0(0x1fb1)), rd(SEG0(0x1fb8)));
   grbm_select(0, 0, 0, 0);
   skip_hqd = 1;
   sched_test();
}
static void kiqreg_test(void)
{
   uint32_t t = rd(SEG1(0x98a)) & 0xffffff00;
   t |= (3u << 5) | (1u << 3);
   wr(SEG1(0x98a), t); wr(SEG1(0x98a), t | 0x80);
   mqd_build(KIQ_MQD, KIQ_RING, KIQ_WB, KIQ_EOP, 1, 0x18);
   for (int i = 0; i < 1024; i++) vw(KIQ_RING + i * 4, 0);
   for (int i = 0; i < 32; i++) vw(KIQ_WB + i * 4, 0);
   uint32_t pq = 0xd8308011;
   hqd_program(3, 1, KIQ_MQD, KIQ_RING, KIQ_WB, pq, (0x18u << 2) | (1u << 30));
   wr(SEG1(0x2040), 0xCAFEDEAD);
   vw(KIQ_RING + 0, 0xC0017900); vw(KIQ_RING + 4, 0x40); vw(KIQ_RING + 8, 0xDEADBEEF);
   vw(KIQ_WB + 16, 3); vw(KIQ_WB + 20, 0);
   db(0x18 * 4, 3);
   for (int i = 0; i < 20; i++) { usleep(10000); if (rd(SEG1(0x2040)) == 0xDEADBEEF) break; }
   grbm_select(3, 1, 0, 0);
   printf("kiqreg: scratch0=0x%08x (expect deadbeef) rptr=%u wb_rptr=0x%x\n", rd(SEG1(0x2040)), rd(SEG0(0x1fb3)), vr(KIQ_WB));
   grbm_select(0, 0, 0, 0);
}

// SDMA0 queue0: WRITE_LINEAR of one dword to an arbitrary 64-bit address (ring in VRAM at 0x2000000, MC addressed)
static void sdma_write(uint64_t dst, uint32_t val)
{
   const uint64_t ring = 0x2000000ull;   // navi31 FIX: ring base MUST match RB_BASE (0x80020000 = VRAM off 0x2000000)
                                         // and the rptr/wptr-poll addrs at 0x2001000/0x2001010. (32MB, clear of the 8MB FB.)
   for (int i = 0; i < 8; i++) vw(ring + i * 4, 0);
   vw(ring + 0, 0x00000002); vw(ring + 4, (uint32_t)dst); vw(ring + 8, (uint32_t)(dst >> 32)); vw(ring + 12, 0); vw(ring + 16, val);
   wr(0x12E0, 0x00040800);
   wr(0x12E3, 0); wr(0x12E4, 0); wr(0x12E5, 0); wr(0x12E6, 0);
   wr(0x1313, 0x02001010); wr(0x1312, 0x80); wr(0x12E8, 0x80); wr(0x12E9, 0x02001000);
   wr(0x12E0, 0x00841814); wr(0x12E1, 0x80020000); wr(0x12E2, 0);
   wr(0x1315, 1); wr(0x12E5, 0); wr(0x12E6, 0); wr(0x1315, 0);
   wr(0xF89A, 0x08084000);
   wr(0x12E0, 0x00841815); wr(0x12EA, 0x101);
   vw(0x2001010, 0x14);
   wr(0x1315, 1); wr(0x12E5, 0x14); wr(0x12E6, 0); wr(0x1315, 0);
   usleep(200000);
}
static void sdma_fill(uint64_t dst, uint32_t val, uint32_t bytes)   // CONSTANT_FILL (dword) up to ~4MB/call; dst = MC addr
{
   const uint64_t ring = 0x2000000ull;
   for (int i = 0; i < 8; i++) vw(ring + i * 4, 0);
   vw(ring + 0, 0x8000000bu);          // op 0x0b CONSTANT_FILL, fillsize=2 (4-byte/dword)
   vw(ring + 4, (uint32_t)dst); vw(ring + 8, (uint32_t)(dst >> 32));
   vw(ring + 12, val);                 // fill data
   vw(ring + 16, bytes - 1);           // count = bytes - 1
   wr(0x12E0, 0x00040800);
   wr(0x12E3, 0); wr(0x12E4, 0); wr(0x12E5, 0); wr(0x12E6, 0);
   wr(0x1313, 0x02001010); wr(0x1312, 0x80); wr(0x12E8, 0x80); wr(0x12E9, 0x02001000);
   wr(0x12E0, 0x00841814); wr(0x12E1, 0x80020000); wr(0x12E2, 0);
   wr(0x1315, 1); wr(0x12E5, 0); wr(0x12E6, 0); wr(0x1315, 0);
   wr(0xF89A, 0x08084000);
   wr(0x12E0, 0x00841815); wr(0x12EA, 0x101);
   vw(0x2001010, 0x14);
   wr(0x1315, 1); wr(0x12E5, 0x14); wr(0x12E6, 0); wr(0x1315, 0);
   usleep(30000);
}
static void sdma_copy(uint64_t dst, uint64_t src, uint32_t bytes)   // COPY_LINEAR (7 dwords); dst/src = MC addrs, <=~1MB/call
{
   const uint64_t ring = 0x2000000ull;
   for (int i = 0; i < 12; i++) vw(ring + i * 4, 0);
   vw(ring + 0, 0x00000001u);          // op 0x01 COPY, sub_op 0 LINEAR
   vw(ring + 4, bytes - 1);            // count = bytes - 1
   vw(ring + 8, 0);                    // params
   vw(ring + 12, (uint32_t)src); vw(ring + 16, (uint32_t)(src >> 32));
   vw(ring + 20, (uint32_t)dst); vw(ring + 24, (uint32_t)(dst >> 32));
   wr(0x12E0, 0x00040800);
   wr(0x12E3, 0); wr(0x12E4, 0); wr(0x12E5, 0); wr(0x12E6, 0);
   wr(0x1313, 0x02001010); wr(0x1312, 0x80); wr(0x12E8, 0x80); wr(0x12E9, 0x02001000);
   wr(0x12E0, 0x00841814); wr(0x12E1, 0x80020000); wr(0x12E2, 0);
   wr(0x1315, 1); wr(0x12E5, 0); wr(0x12E6, 0); wr(0x1315, 0);
   wr(0xF89A, 0x08084000);
   wr(0x12E0, 0x00841815); wr(0x12EA, 0x101);
   vw(0x2001010, 0x1C);
   wr(0x1315, 1); wr(0x12E5, 0x1C); wr(0x12E6, 0); wr(0x1315, 0);
   usleep(30000);
}

static void sdma_sys_test(void)
{
   uint64_t phys = 0, len = 0, v = 0;
   if (sm(1, 1 << 20, 0, &phys, &len)) { printf("sysmem alloc failed\n"); return; }
   // MMHUB AGP aperture identity (BASE=0, BOT=0, TOP=0x1ff): MC address == system physical address for SDMA
   wr(0x1a8f0, 0); wr(0x1a8ef, 0); wr(0x1a8ee, 0x000001ff);
   uint64_t tgt = phys + 0x1000;
   sdma_write(tgt, 0x5D5D5D5D);
   uint64_t pp; sm(2, 0x1000, 0, &pp, &v);
   printf("SDMA write to system phys 0x%llx via MMHUB AGP: 0x%08x (want 5d5d5d5d)\n", (unsigned long long)tgt, (uint32_t)v);
   sdma_write(MC(TESTBUF + 0x200), 0x6E6E6E6E);
   printf("SDMA control write to VRAM: 0x%08x (want 6e6e6e6e)\n", vr(TESTBUF + 0x200));
}

int main(int argc, char **argv)
{
   g_argc = argc; g_argv = argv;
   if (open_kext()) return 1;
   if (argc < 2) { fprintf(stderr, "mesboot gart|mes|kiq|all\n"); return 2; }
   int all = !strcmp(argv[1], "all");
   if (getenv("MESBOOT_MC") && *getenv("MESBOOT_MC") == '1') use_mc = 1;
   // navi31: sudo drops the environment, so options can also follow the command, e.g. `mesboot kiqdb mc`
   for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "mc")) use_mc = 1;
   if (!strcmp(argv[1], "db") && argc >= 4) { printf("db +0x%llx <- %llu kr=0x%x\n", strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0), db(strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0))); return 0; }
   if (!strcmp(argv[1], "dcn")) {   // navi31 display probe (READ-ONLY): OTG0 + HUBPREQ0 flip regs via DCN segment-2 base
      uint32_t seg2 = 0x34c0; for (int i = 2; i < argc; i++) if (!strncmp(argv[i], "seg=", 4)) seg2 = (uint32_t)strtoul(argv[i] + 4, NULL, 0);
      uint32_t ctl = rd(seg2 + 0x1b41), ht = rd(seg2 + 0x1b2a) + 1, vt = rd(seg2 + 0x1b2f) + 1;
      uint32_t c0 = rd(seg2 + 0x1b4c) & 0xffffff; usleep(200000); uint32_t c1 = rd(seg2 + 0x1b4c) & 0xffffff;
      uint32_t fr = (c1 - c0) & 0xffffff;
      printf("OTG0 (seg2=%#x): ctl=%#010x master_en=%u H_total=%u V_total=%u frames/200ms=%u (~%u Hz)\n", seg2, ctl, ctl & 1, ht, vt, fr, fr * 5);
      printf("HUBPREQ0 flip: SURFACE_ADDR=%#010x _HIGH=%#010x FLIP_CONTROL=%#010x FLIP_INT=%#010x\n", rd(seg2 + 0x060a), rd(seg2 + 0x060b), rd(seg2 + 0x061b), rd(seg2 + 0x061f));
      return 0;
   }
   if (!strcmp(argv[1], "dcnflip")) {   // navi31: NO-OP flip test -- rewrite the SAME scanout address (HIGH then LOW arms it). Invisible if correct.
      uint32_t seg2 = 0x34c0; for (int i = 2; i < argc; i++) if (!strncmp(argv[i], "seg=", 4)) seg2 = (uint32_t)strtoul(argv[i] + 4, NULL, 0);
      uint32_t hi = rd(seg2 + 0x060b), lo = rd(seg2 + 0x060a), fc = rd(seg2 + 0x061b);
      printf("before: ADDR=%#010x HIGH=%#010x FLIP_CONTROL=%#010x (lock=%u pending=%u)\n", lo, hi, fc, fc & 1, (fc >> 8) & 1);
      uint32_t rbh = wr(seg2 + 0x060b, hi);      // same high
      uint32_t rbl = wr(seg2 + 0x060a, lo);      // same low -> arms the (no-op) flip
      printf("wrote same addr: HIGH readback=%#010x  LOW readback=%#010x\n", rbh, rbl);
      uint32_t fc2 = 0; int ms;
      for (ms = 0; ms < 200; ms++) { fc2 = rd(seg2 + 0x061b); if (!((fc2 >> 8) & 1)) break; usleep(1000); }
      uint32_t fi = rd(seg2 + 0x061f);
      printf("after:  FLIP_CONTROL=%#010x pending=%u (cleared in %d ms)  FLIP_INT=%#010x occurred=%u\n", fc2, (fc2 >> 8) & 1, ms, fi, (fi >> 16) & 1);
      return 0;
   }
   if (!strcmp(argv[1], "fbtest")) {   // navi31: draw a colored band into the live GOP framebuffer (BAR0 off 0), then RESTORE. Reversible.
      static uint32_t save[300000];
      uint32_t n = 40000, color = 0xFFFF2020; uint32_t secs = 5;
      for (int i = 2; i < argc; i++) {
         if (!strncmp(argv[i], "n=", 2)) n = (uint32_t)strtoul(argv[i] + 2, NULL, 0);
         else if (!strncmp(argv[i], "color=", 6)) color = (uint32_t)strtoul(argv[i] + 6, NULL, 0);
         else if (!strncmp(argv[i], "secs=", 5)) secs = (uint32_t)strtoul(argv[i] + 5, NULL, 0);
      }
      if (n > 300000) n = 300000;
      printf("FB sample before: [0]=%#010x [4]=%#010x [%u]=%#010x  (SURFACE_PITCH reg=%#x)\n", vr(0), vr(4), (n/2)*4, vr((uint64_t)(n/2) * 4), rd(0x34c0 + 0x0607));
      for (uint32_t i = 0; i < n; i++) save[i] = vr((uint64_t)i * 4);
      for (uint32_t i = 0; i < n; i++) vw((uint64_t)i * 4, color);
      hdp_flush();
      printf("drew %u px color %#010x at top of framebuffer -- LOOK AT THE SCREEN -- restoring in %u s\n", n, color, secs);
      usleep(secs * 1000000u);
      for (uint32_t i = 0; i < n; i++) vw((uint64_t)i * 4, save[i]);
      hdp_flush();
      printf("restored original pixels.\n");
      return 0;
   }
   if (!strcmp(argv[1], "dump")) mesdump();
   if (all || !strcmp(argv[1], "gart")) gart();
   if (all || !strcmp(argv[1], "mes")) mes_start();
   if (!strcmp(argv[1], "kiqdb")) { use_db = 1; kiq_test(); }
   if (!strcmp(argv[1], "kiqsched")) { kiq_init_only(); sched_test(); }
   if (!strcmp(argv[1], "claim")) {   // GPU is up: let PhxAccel (the Metal accelerator) start on it
      uint64_t in[1] = {1};
      printf("claim -> kr 0x%x\n", IOConnectCallScalarMethod(conn, 113, in, 1, NULL, NULL));
   }
   if (!strcmp(argv[1], "vm8")) vm8_test();   // navi31: reuse the compute queue `cs` left at dw 8
   if (!strcmp(argv[1], "gfxq")) { // navi31: MES already running (after schedkiq): ADD_QUEUE a GFX queue and adopt it
      sch_wptr = vr(SCH_WB + 16); sch_seq = vr(SCH_WB + 32);
      printf("sched ring: wptr=%u seq=%llu\n", sch_wptr, (unsigned long long)sch_seq);
      setenv("ADOPT_GFX", "1", 1);
      gfx_via_mes();
   }
   if (!strcmp(argv[1], "cs")) {    // navi31: MES already running (after schedkiq): ADD_QUEUE a compute queue on its ring
      sch_wptr = vr(SCH_WB + 16); sch_seq = vr(SCH_WB + 32);
      printf("sched ring: wptr=%u seq=%llu\n", sch_wptr, (unsigned long long)sch_seq);
      compute_via_mes();
   }
   if (!strcmp(argv[1], "gfx")) {   // MES already running (after kiqsched): continue its ring where the last run left it
      sch_wptr = vr(SCH_WB + 16); sch_seq = vr(SCH_WB + 32);
      printf("sched ring: wptr=%u seq=%llu\n", sch_wptr, (unsigned long long)sch_seq);
      gfx_via_mes();
   }
   if (!strcmp(argv[1], "sdmaw")) {   // navi31 SDMA validation: WRITE_LINEAR one dword to VRAM, read back
      uint64_t off = TESTBUF + 0x200; uint32_t val = 0x6e6e6e6e;
      if (argc >= 3) off = strtoull(argv[2], NULL, 0);
      if (argc >= 4) val = (uint32_t)strtoul(argv[3], NULL, 0);
      vw(off, 0x11111111);
      uint32_t before = vr(off);
      sdma_write(MC(off), val);
      uint32_t after = vr(off);
      printf("SDMA WRITE_LINEAR VRAM off 0x%llx: before=0x%08x after=0x%08x (want 0x%08x) -> %s\n",
             (unsigned long long)off, before, after, val, after == val ? "OK" : "FAIL");
      return 0;
   }
   if (!strcmp(argv[1], "sdmafill")) {   // navi31: CONSTANT_FILL a VRAM region (chunked). off=,bytes=,val=
      uint64_t off = 0xA120000ull, bytes = 0x1000ull; uint32_t val = 0x12345678u;
      for (int i = 2; i < argc; i++) {
         if (!strncmp(argv[i], "off=", 4)) off = strtoull(argv[i] + 4, NULL, 0);
         else if (!strncmp(argv[i], "bytes=", 6)) bytes = strtoull(argv[i] + 6, NULL, 0);
         else if (!strncmp(argv[i], "val=", 4)) val = (uint32_t)strtoul(argv[i] + 4, NULL, 0);
      }
      uint64_t done = 0; const uint32_t chunk = 0x100000;
      while (done < bytes) { uint32_t n = (bytes - done > chunk) ? chunk : (uint32_t)(bytes - done); sdma_fill(MC(off + done), val, n); done += n; }
      printf("SDMA CONSTANT_FILL: 0x%llx bytes @ VRAM off 0x%llx = 0x%08x. readback [0]=0x%08x [mid]=0x%08x [end]=0x%08x\n",
             (unsigned long long)bytes, (unsigned long long)off, val, vr(off), vr(off + bytes/2), vr(off + bytes - 4));
      return 0;
   }
   if (!strcmp(argv[1], "fbflip")) {   // navi31: SDMA-fill a framebuffer in VRAM, FLIP the scanout to it, then restore. VISIBLE.
      uint32_t seg2 = 0x34c0; uint64_t off = 0x4000000ull, fbytes = 0x900000ull; uint32_t color = 0xff2060ff; uint32_t secs = 4;
      for (int i = 2; i < argc; i++) {
         if (!strncmp(argv[i], "off=", 4)) off = strtoull(argv[i] + 4, NULL, 0);
         else if (!strncmp(argv[i], "color=", 6)) color = (uint32_t)strtoul(argv[i] + 6, NULL, 0);
         else if (!strncmp(argv[i], "secs=", 5)) secs = (uint32_t)strtoul(argv[i] + 5, NULL, 0);
      }
      uint32_t savLo = rd(seg2 + 0x060a), savHi = rd(seg2 + 0x060b);
      printf("saved scanout: HIGH=%#010x LOW=%#010x\n", savHi, savLo);
      uint64_t done = 0; const uint32_t chunk = 0x100000;
      while (done < fbytes) { uint32_t n = (fbytes - done > chunk) ? chunk : (uint32_t)(fbytes - done); sdma_fill(MC(off + done), color, n); done += n; }
      printf("filled 0x%llx bytes @ off 0x%llx = 0x%08x (readback [0]=0x%08x). FLIPPING now...\n", (unsigned long long)fbytes, (unsigned long long)off, color, vr(off));
      uint64_t mc = MC(off);
      wr(seg2 + 0x060b, (uint32_t)(mc >> 32));   // SURFACE_ADDRESS_HIGH
      wr(seg2 + 0x060a, (uint32_t)mc);           // SURFACE_ADDRESS (low last -> arms flip)
      printf(">>> LOOK AT THE SCREEN: solid color for %u s <<<\n", secs);
      usleep(secs * 1000000u);
      wr(seg2 + 0x060b, savHi);
      wr(seg2 + 0x060a, savLo);
      printf("restored original scanout (HIGH=%#010x LOW=%#010x).\n", savHi, savLo);
      return 0;
   }
   if (!strcmp(argv[1], "fbcomp")) {   // navi31: COPY live desktop -> our buffer, draw overlay band, FLIP, restore. Compositing demo.
      uint32_t seg2 = 0x34c0; uint64_t off = 0x4000000ull; uint32_t secs = 5, overlay = 0xff30ff30;
      uint32_t pitch = 1920u * 4u, h = 1080u; uint64_t fbytes = (uint64_t)pitch * h;
      for (int i = 2; i < argc; i++) { if (!strncmp(argv[i],"secs=",5)) secs=(uint32_t)strtoul(argv[i]+5,0,0); else if (!strncmp(argv[i],"overlay=",8)) overlay=(uint32_t)strtoul(argv[i]+8,0,0); }
      uint32_t savLo = rd(seg2 + 0x060a), savHi = rd(seg2 + 0x060b);
      printf("copying live desktop (0x%llx bytes) FB off0 -> buffer off 0x%llx via SDMA...\n", (unsigned long long)fbytes, (unsigned long long)off);
      uint64_t done = 0; const uint32_t chunk = 0x100000;
      while (done < fbytes) { uint32_t n=(fbytes-done>chunk)?chunk:(uint32_t)(fbytes-done); sdma_copy(MC(off+done), MC(0+done), n); done+=n; }
      // overlay: a green band over rows 440..520
      uint64_t band_off = off + (uint64_t)440 * pitch; sdma_fill(MC(band_off), overlay, 80u * pitch);
      printf("copied + drew overlay band. readback buf[0]=0x%08x (desktop) band=0x%08x. FLIPPING...\n", vr(off), vr(band_off));
      uint64_t mc = MC(off);
      wr(seg2 + 0x060b, (uint32_t)(mc >> 32)); wr(seg2 + 0x060a, (uint32_t)mc);
      printf(">>> LOOK: desktop + green band for %u s <<<\n", secs);
      usleep(secs * 1000000u);
      wr(seg2 + 0x060b, savHi); wr(seg2 + 0x060a, savLo);
      printf("restored.\n");
      return 0;
   }
   if (!strcmp(argv[1], "sdmasys")) sdma_sys_test();
   if (!strcmp(argv[1], "kiqreg")) kiqreg_test();
   if (!strcmp(argv[1], "kiqres")) kiqres_test();
   if (!strcmp(argv[1], "schedkiq")) schedkiq_test();
   if (!strcmp(argv[1], "mec")) mec_test();
   if (!strcmp(argv[1], "sched")) sched_test();
   if (!strcmp(argv[1], "viakiq")) { use_db = 1; kiq_test(); skip_hqd = 1; sched_test(); }
   if (all || !strcmp(argv[1], "kiq")) kiq_test();
   return 0;
}
