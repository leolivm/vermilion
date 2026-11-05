#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# vcn_m1_gen.py: build the data files for vcn_up.sh (WP6 M1, VCN 4.0 instance 0 on the RX 7900 XT).
# Writes into vcn/ next to this file:
#   vcn0_area.bin   one image staged at fw_buf + AREA_BUF: unified ring, fence page, two IBs, two decode messages,
#                   the VCPU BO (stack, context, fw_shared), all as Linux 6.8 lays them out
#   vcn0_ram.bin    the 216-byte DPG indirect SRAM list for PSP LOAD_IP_FW VCN0_RAM (27 offset/value pairs)
#   vcn0_layout.sh  shell variables vcn_up.sh sources (offsets, MC addresses, expected values)
#   vcn0_layout.txt the same, readable, plus the SRAM list decoded
# Sources (Linux v6.8): amdgpu/vcn_v4_0.c start_dpg_mode / mc_resume_dpg_mode / fw_shared_init / sw_init,
# amdgpu/amdgpu_vcn.c sw_init / dec_get_create_msg / dec_sw_send_msg / unified_ring_ib_header,
# amdgpu/amdgpu_vcn.h SOC15_DPG_MODE_OFFSET and the fw_shared structs, vcn_v2_0.c enc_ring_emit_ib / emit_fence,
# include/asic_reg/vcn/vcn_4_0_0_offset.h and _sh_mask.h. VCN base on this card (IP discovery table):
# (UVD 4.0.0 segment 0 = 0x7800, segment 1 = 0x7E00).
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "vcn")

FB_MC = 0x80_0000_0000          # phx-fb-mc-base (ioreg), MMMC_VM_FB_LOCATION_BASE 0x8000 << 24
FWBUF_VRAM = 0x8000000          # PHX_VRAM_FWBUF_OFF: fw_buf, where phxctl stage / loadfw write
FW_BUF_VCN = 0x400000           # fw_buf offset of the staged VCN ucode (403,600 B)
FW_BUF_RAM = 0x480000           # fw_buf offset of the staged VCN0_RAM list (216 B)
AREA_BUF = 0x800000             # fw_buf offset of vcn0_area.bin
AREA_VRAM = FWBUF_VRAM + AREA_BUF
AREA_MC = FB_MC + AREA_VRAM

RING = 0x00000                  # 4 KiB: amdgpu_ring_init(512 dw) -> 4096 B, RB_SIZE 0x400 (trace)
FENCE = 0x01000                 # fence value written by VCN_ENC_CMD_FENCE
IB_CREATE = 0x02000
IB_DESTROY = 0x03000
MSG_CREATE = 0x04000            # AMDGPU_GPU_PAGE_ALIGN'ed message buffers (4 KiB each)
MSG_DESTROY = 0x05000
VCPU_BO = 0x10000               # amdgpu_vcn_sw_init: stack 128 KiB + context 512 KiB + fw_shared (PSP load: no fw copy)
STACK_SIZE = 128 * 1024         # AMDGPU_VCN_STACK_SIZE
CONTEXT_SIZE = 512 * 1024       # AMDGPU_VCN_CONTEXT_SIZE
FW_SHARED_SIZE = 0x1000         # AMDGPU_GPU_PAGE_ALIGN(sizeof(struct amdgpu_vcn4_fw_shared) = 148)
BO_SIZE = STACK_SIZE + CONTEXT_SIZE + FW_SHARED_SIZE
FW_SHARED = VCPU_BO + BO_SIZE - FW_SHARED_SIZE
AREA_SIZE = VCPU_BO + BO_SIZE

GB_ADDR_CONFIG = 0x545          # GC GB_ADDR_CONFIG read on the box (0x263e) = Linux value (JPEG ADDR_CONFIG write)

# VCN 4.0 registers, all BASE_IDX 1 (vcn_4_0_0_offset.h v6.8, line in brackets)
REG = {
    "UVD_POWER_STATUS": 0x0002,                     # [1150]
    "UVD_PGFSM_STATUS": 0x0001,                     # [1148]
    "UVD_VERSION": 0x006e,                          # [1294]
    "VCN_RB1_DB_CTRL": 0x0072,                      # [1300]
    "VCN_RB_ENABLE": 0x0085,                        # [1334]
    "UVD_RB_RPTR": 0x00ac,                          # [1338]
    "UVD_RB_WPTR": 0x00ad,                          # [1340]
    "UVD_MASTINT_EN": 0x00e1,                       # [188]
    "UVD_RB_BASE_LO": 0x00ea,                       # [206]
    "UVD_RB_BASE_HI": 0x00eb,                       # [208]
    "UVD_RB_SIZE": 0x00ec,                          # [210]
    "UVD_STATUS": 0x0143,                           # [350]
    "UVD_VCPU_CACHE_OFFSET0": 0x0180,               # [378]
    "UVD_VCPU_CACHE_SIZE0": 0x0181,                 # [380]
    "UVD_VCPU_CACHE_OFFSET1": 0x0182,               # [382]
    "UVD_VCPU_CACHE_SIZE1": 0x0183,                 # [384]
    "UVD_VCPU_CACHE_OFFSET2": 0x0184,               # [386]
    "UVD_VCPU_CACHE_SIZE2": 0x0185,                 # [388]
    "UVD_VCPU_NONCACHE_OFFSET0": 0x0192,            # [414]
    "UVD_VCPU_NONCACHE_SIZE0": 0x0193,              # [416]
    "UVD_VCPU_CNTL": 0x0196,                        # [422]
    "UVD_MPC_CNTL": 0x01d4,                         # [454]
    "UVD_MPC_SET_MUXA0": 0x01d6,                    # [458]
    "UVD_MPC_SET_MUXB0": 0x01d8,                    # [462]
    "UVD_MPC_SET_MUX": 0x01da,                      # [466]
    "UVD_LMI_VCPU_NC0_64BIT_BAR_LOW": 0x0240,       # [588]
    "UVD_LMI_VCPU_NC0_64BIT_BAR_HIGH": 0x0241,      # [590]
    "UVD_LMI_VCPU_CACHE_64BIT_BAR_LOW": 0x0244,     # [596]
    "UVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH": 0x0245,    # [598]
    "UVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW": 0x0270,    # [668]
    "UVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH": 0x0271,   # [670]
    "UVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW": 0x0274,    # [676]
    "UVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH": 0x0275,   # [678]
    "UVD_LMI_CTRL2": 0x02ae,                        # [744]
    "UVD_LMI_CTRL": 0x02b0,                         # [748]
    "UVD_GFX10_ADDR_CONFIG": 0x0042,                # [1214]
}
SEG1 = 0x7E00                   # UVD 4.0.0 instance 0, segment 1 (ip_bases.txt)
VCN_VID_SOC_ADDRESS_2_0 = 0x1FB00   # vcn_v4_0.c:47
VCN_AON_SOC_ADDRESS_2_0 = 0x1F800   # amdgpu_vcn.h:68
VCN_AON_IP_ADDRESS_2_0 = 0x30000    # amdgpu_vcn.h:71


def absreg(name):
    return SEG1 + REG[name]


def dpg_offset(name):
    """SOC15_DPG_MODE_OFFSET (amdgpu_vcn.h:102): the VCN-internal dword offset PSP writes the value to."""
    addr = ((SEG1 + REG[name]) << 2) & 0xFFFFF
    if VCN_VID_SOC_ADDRESS_2_0 <= addr < VCN_VID_SOC_ADDRESS_2_0 + 0x2600:
        internal = addr - VCN_VID_SOC_ADDRESS_2_0
    elif VCN_AON_SOC_ADDRESS_2_0 <= addr < VCN_AON_SOC_ADDRESS_2_0 + 0x600:
        internal = addr - VCN_AON_SOC_ADDRESS_2_0 + VCN_AON_IP_ADDRESS_2_0
    else:
        internal = addr
    return internal >> 2


def lo(x):
    return x & 0xFFFFFFFF


def hi(x):
    return (x >> 32) & 0xFFFFFFFF


def sram_list():
    """vcn_v4_0_start_dpg_mode(indirect=1) with AMD_CG_SUPPORT_VCN_MGCG set (clock-gating writes skipped) and no VCN RAS:
    27 pairs = 216 B, the size PSP got on this card (the Linux PSP command log, VCN0_RAM)."""
    bo = AREA_MC + VCPU_BO
    lmi_ctrl = (0x100 | 0x200 | 0x4000 | 0x1000 | 0x2000 | 0x200000 | (8 << 0) | 0x00100000)
    w = [
        ("UVD_VCPU_CNTL", (0xFF << 20) | 0x200 | 0x10000000),   # PRB_TIMEOUT_VAL 0xff, CLK_EN, BLK_RST
        ("UVD_MASTINT_EN", 0),
        ("UVD_LMI_CTRL", lmi_ctrl),
        ("UVD_MPC_CNTL", 0x2 << 3),
        ("UVD_MPC_SET_MUXA0", (1 << 6) | (2 << 12) | (3 << 18) | (4 << 24)),
        ("UVD_MPC_SET_MUXB0", (1 << 6) | (2 << 12) | (3 << 18) | (4 << 24)),
        ("UVD_MPC_SET_MUX", (0 << 0) | (1 << 3) | (2 << 6)),
        # mc_resume_dpg_mode, PSP load + indirect: windows 0 (fw) and 1 (stack) are 0, the PSP points them at the TMR
        ("UVD_LMI_VCPU_CACHE_64BIT_BAR_LOW", 0),
        ("UVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH", 0),
        ("UVD_VCPU_CACHE_OFFSET0", 0),
        ("UVD_VCPU_CACHE_SIZE0", 0),
        ("UVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW", 0),
        ("UVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH", 0),
        ("UVD_VCPU_CACHE_OFFSET1", 0),
        ("UVD_VCPU_CACHE_SIZE1", STACK_SIZE),
        ("UVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW", lo(bo + STACK_SIZE)),    # offset = 0 under PSP load
        ("UVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH", hi(bo + STACK_SIZE)),
        ("UVD_VCPU_CACHE_OFFSET2", 0),
        ("UVD_VCPU_CACHE_SIZE2", CONTEXT_SIZE),
        ("UVD_LMI_VCPU_NC0_64BIT_BAR_LOW", lo(AREA_MC + FW_SHARED)),
        ("UVD_LMI_VCPU_NC0_64BIT_BAR_HIGH", hi(AREA_MC + FW_SHARED)),
        ("UVD_VCPU_NONCACHE_OFFSET0", 0),
        ("UVD_VCPU_NONCACHE_SIZE0", FW_SHARED_SIZE),
        ("UVD_GFX10_ADDR_CONFIG", GB_ADDR_CONFIG),
        ("UVD_VCPU_CNTL", (0xFF << 20) | 0x200),                  # BLK_RST released
        ("UVD_LMI_CTRL2", 0x1F << 17),                            # RE_OFLD_MIF_WR_REQ_NUM
        ("UVD_MASTINT_EN", 0x2),                                  # VCPU_EN
    ]
    assert len(w) == 27
    return w


def dec_ib(msg_mc):
    """amdgpu_vcn_dec_sw_send_msg with the unified-queue header (amdgpu_vcn_unified_ring_ib_header): 43 dwords."""
    pack = 33 + 4 + 2                       # sizeof(amdgpu_vcn_decode_buffer)/4 + engine info + size/flag
    ib = [0x10, 0x30000002, 0, pack,        # single queue checksum (filled below)
          0x10, 0x30000001, 0x3, pack * 4]  # engine info: 3 = decode
    ib += [33 * 4 + 8, 0x1]                 # sizeof(decode_buffer) + 8, AMDGPU_VCN_IB_FLAG_DECODE_BUFFER
    db = [0] * 33
    db[0] = 0x1                             # valid_buf_flag = AMDGPU_VCN_CMD_FLAG_MSG_BUFFER
    db[1] = hi(msg_mc)
    db[2] = lo(msg_mc)
    ib += db
    ib[2] = sum(ib[4:4 + pack]) & 0xFFFFFFFF
    assert len(ib) == 43
    return ib


def create_msg(handle):
    m = [0x28, 0x38, 0x1, 0x0, handle, 0x0, 0x1, 0x28, 0x10, 0x0, 0x7, 0x0, 0x780, 0x440]
    return m + [0] * (1024 - len(m))


def destroy_msg(handle):
    m = [0x28, 0x18, 0x0, 0x2, handle, 0x0]
    return m + [0] * (1024 - len(m))


def submission(ib_mc, ib_len, seq):
    """amdgpu_ib_schedule on the unified ring, VMID 0: emit_ib (5) + emit_fence (5) + insert_end (1), padded with
    VCN_ENC_CMD_NO_OP (0) to align_mask 0x3f; the trace's rptr moved 0x40 per submission."""
    s = [0x2, 0x0, lo(ib_mc), hi(ib_mc), ib_len,                # VCN_ENC_CMD_IB, vmid, addr, length_dw
         0x3, lo(AREA_MC + FENCE), hi(AREA_MC + FENCE), seq,   # VCN_ENC_CMD_FENCE, addr, seq
         0x4,                                                  # VCN_ENC_CMD_TRAP
         0x1]                                                  # VCN_ENC_CMD_END
    return s + [0] * (0x40 - len(s))


def main():
    os.makedirs(OUT, exist_ok=True)
    area = bytearray(AREA_SIZE)

    def put(off, dwords):
        struct.pack_into("<%dI" % len(dwords), area, off, *dwords)

    # ring: [0x00] ring test (VCN_ENC_CMD_END + NOPs, wptr 0x40), [0x40] create IB (wptr 0x80), [0x80] destroy (0xC0)
    ring = [0x1] + [0] * 0x3F
    ring += submission(AREA_MC + IB_CREATE, 43, 1)
    ring += submission(AREA_MC + IB_DESTROY, 43, 2)
    put(RING, ring)
    put(IB_CREATE, dec_ib(AREA_MC + MSG_CREATE))
    put(IB_DESTROY, dec_ib(AREA_MC + MSG_DESTROY))
    put(MSG_CREATE, create_msg(1))
    put(MSG_DESTROY, destroy_msg(1))
    # vcn_v4_0_fw_shared_init: present_flag_0 = UNIFIED_QUEUE (1 << 2) | SMU_DPM_INTERFACE (1 << 11); sq.is_enabled = 1
    # (byte 16); smu_dpm_interface.smu_interface_type (byte 120) = 0, dGPU
    put(FW_SHARED + 0, [0x804])
    put(FW_SHARED + 16, [0x00000001])
    with open(os.path.join(OUT, "vcn0_area.bin"), "wb") as f:
        f.write(area)

    sl = sram_list()
    ram = b"".join(struct.pack("<II", dpg_offset(n), v & 0xFFFFFFFF) for n, v in sl)
    assert len(ram) == 216
    with open(os.path.join(OUT, "vcn0_ram.bin"), "wb") as f:
        f.write(ram)

    v = {
        "FWBUF_VRAM": FWBUF_VRAM, "FW_BUF_VCN": FW_BUF_VCN, "FW_BUF_RAM": FW_BUF_RAM,
        "AREA_BUF": AREA_BUF, "AREA_VRAM": AREA_VRAM, "AREA_SIZE": AREA_SIZE,
        "RING_MC_LO": lo(AREA_MC + RING), "RING_MC_HI": hi(AREA_MC + RING), "RB_SIZE_DW": 0x400,
        "RING_VRAM": AREA_VRAM + RING, "FENCE_VRAM": AREA_VRAM + FENCE,
        "IB_CREATE_VRAM": AREA_VRAM + IB_CREATE, "FW_SHARED_VRAM": AREA_VRAM + FW_SHARED,
        "R_POWER_STATUS": absreg("UVD_POWER_STATUS"), "R_PGFSM_STATUS": absreg("UVD_PGFSM_STATUS"),
        "R_VERSION": absreg("UVD_VERSION"), "R_STATUS": absreg("UVD_STATUS"),
        "R_RB_BASE_LO": absreg("UVD_RB_BASE_LO"), "R_RB_BASE_HI": absreg("UVD_RB_BASE_HI"),
        "R_RB_SIZE": absreg("UVD_RB_SIZE"), "R_RB_ENABLE": absreg("VCN_RB_ENABLE"),
        "R_RB_RPTR": absreg("UVD_RB_RPTR"), "R_RB_WPTR": absreg("UVD_RB_WPTR"),
        "R_RB1_DB_CTRL": absreg("VCN_RB1_DB_CTRL"),
        "DB_CTRL_VAL": (0x312 << 2) | 0x40000000,   # (vcn_ring0_1 << 1) + 2 = 0x312, EN (trace 0x40000c48)
        "DB_BYTE": 0x312 * 4,
    }
    with open(os.path.join(OUT, "vcn0_layout.sh"), "w") as f:
        f.write("# generated by vcn_m1_gen.py (WP6 M1); sourced by vcn_up.sh\n")
        for k, x in v.items():
            f.write("%s=0x%x\n" % (k, x))
    with open(os.path.join(OUT, "vcn0_layout.txt"), "w") as f:
        f.write("VCN0 M1 layout (generated by vcn_m1_gen.py)\n\n")
        for k, x in v.items():
            f.write("%-16s 0x%x\n" % (k, x))
        f.write("\narea MC 0x%x: ring +0x%x, fence +0x%x, IB create +0x%x, IB destroy +0x%x, msg create +0x%x, "
                "msg destroy +0x%x, VCPU BO +0x%x (stack 0x%x, context +0x%x 0x%x), fw_shared +0x%x\n\n"
                % (AREA_MC, RING, FENCE, IB_CREATE, IB_DESTROY, MSG_CREATE, MSG_DESTROY, VCPU_BO, STACK_SIZE,
                   VCPU_BO + STACK_SIZE, CONTEXT_SIZE, FW_SHARED))
        f.write("VCN0_RAM (PSP LOAD_IP_FW type 49, 216 B): internal offset, value, register (absolute dword)\n")
        for n, x in sl:
            f.write("  0x%05x 0x%08x %s (0x%05x)\n" % (dpg_offset(n), x & 0xFFFFFFFF, n, absreg(n)))
        f.write("\ncreate IB: %s\n" % " ".join("%08x" % d for d in dec_ib(AREA_MC + MSG_CREATE)))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
