// SPDX-License-Identifier: MIT
/* mode 1080p60, generated from a register capture of Linux amdgpu on the reference card (the capture tool is not included): do not edit */
/* base DTD @0x36 of the AOC EDID: 148500 kHz 2200x1125 60.0000 Hz */
#ifndef PHX_S2_TYPES
#define PHX_S2_TYPES
enum { PHX_S2_W = 1, PHX_S2_DMUB = 2, PHX_S2_POLL = 3, PHX_S2_DELAY = 4, PHX_S2_CHECK = 5 };
struct phx_s2_step { uint8_t op, stage; uint16_t pad; uint32_t reg, val, mask; uint32_t dmub[16]; const char *name; };
struct phx_s2_req { uint8_t stage, cmp; uint16_t pad; uint32_t reg, mask, val; const char *name; };
#endif
#ifndef PHX_P0_TYPES
#define PHX_P0_TYPES
/* hz, OTG H_TOTAL / V_TOTAL register values, pclk kHz, DENTIST divider max (0 none), DTO convention (0 GOP, 1 Linux),
   ready (0 = refused: TODO-CAPTURE entries), TODO count, OTG1 H/V_TOTAL the values were planned next to (0 any), name */
struct phx_p0_info { uint32_t hz, hTotal, vTotal, pclkKhz, dentistMax, conv, ready, todo, pairHTotal, pairVTotal; const char *name; };
#endif
/* op W only; stage = group (1 DTO, 2 OTG timing, 3 frame position, 4 HUBP0 DLG/TTU, 5 MSA); pad = flags
   (1 TODO-CAPTURE, 2 DML rule, 4 double-buffered: compared at verify); mask = the bits written and compared */
static const struct phx_s2_step kPhxP0_1080p60[] = {
  {PHX_S2_W, 1, 0, 0x00141, 0x08d9ee20, 0xffffffff, {0}, "DP_DTO0_PHASE"},  /* [F] pclk */
  {PHX_S2_W, 2, 4, 0x04FEA, 0x00000897, 0x00007fff, {0}, "OTG0_OTG_H_TOTAL"},  /* [F] EDID */
  {PHX_S2_W, 2, 4, 0x04FEC, 0x002c0000, 0x7fff7fff, {0}, "OTG0_OTG_H_SYNC_A"},  /* [F] EDID */
  {PHX_S2_W, 2, 4, 0x04FEB, 0x00c00840, 0x7fff7fff, {0}, "OTG0_OTG_H_BLANK_START_END"},  /* [F] EDID */
  {PHX_S2_W, 2, 4, 0x04FED, 0x00000000, 0x00030001, {0}, "OTG0_OTG_H_SYNC_A_CNTL"},  /* [F] EDID */
  {PHX_S2_W, 2, 4, 0x04FEF, 0x00000464, 0x00007fff, {0}, "OTG0_OTG_V_TOTAL"},  /* [F] EDID */
  {PHX_S2_W, 2, 4, 0x04FF7, 0x00050000, 0x7fff7fff, {0}, "OTG0_OTG_V_SYNC_A"},  /* [F] EDID */
  {PHX_S2_W, 2, 4, 0x04FF6, 0x00290461, 0x7fff7fff, {0}, "OTG0_OTG_V_BLANK_START_END"},  /* [F] EDID */
  {PHX_S2_W, 2, 4, 0x04FF8, 0x00000000, 0x00000101, {0}, "OTG0_OTG_V_SYNC_A_CNTL"},  /* [F] EDID */
  {PHX_S2_W, 3, 6, 0x05046, 0x0000002c, 0x000003ff, {0}, "OTG0_OTG_VSTARTUP_PARAM"},  /* [D] rule */
  {PHX_S2_W, 3, 6, 0x05047, 0x006a0226, 0x03ffffff, {0}, "OTG0_OTG_VUPDATE_PARAM"},  /* [F] box WIDTH, [D] OFFSET */
  {PHX_S2_W, 3, 4, 0x05048, 0x00000095, 0x0000ffff, {0}, "OTG0_OTG_VREADY_PARAM"},  /* [F] box */
  {PHX_S2_W, 3, 6, 0x039EE, 0x04610002, 0x7fff7fff, {0}, "VTG0_CONTROL"},  /* [D] rule */
  {PHX_S2_W, 3, 4, 0x041CF, 0x00c00840, 0x3fff3fff, {0}, "DSCL0_OTG_H_BLANK"},  /* [F] OTG blank */
  {PHX_S2_W, 3, 4, 0x041D0, 0x00290461, 0x3fff3fff, {0}, "DSCL0_OTG_V_BLANK"},  /* [F] OTG blank */
  {PHX_S2_W, 4, 6, 0x03B03, 0x00290040, 0x7fff1fff, {0}, "HUBPREQ0_BLANK_OFFSET_0"},  /* [D] rule */
  {PHX_S2_W, 4, 6, 0x03B04, 0x00001188, 0x0003ffff, {0}, "HUBPREQ0_BLANK_OFFSET_1"},  /* [D] rule */
  {PHX_S2_W, 4, 6, 0x03B05, 0x0002e4bd, 0x001fffff, {0}, "HUBPREQ0_DST_DIMENSIONS"},  /* [D] rule */
  {PHX_S2_W, 4, 4, 0x03B06, 0x00000033, 0x00071fff, {0}, "HUBPREQ0_DST_AFTER_SCALER"},  /* [F] box */
  {PHX_S2_W, 4, 6, 0x03B1C, 0x0002b18f, 0x001fffff, {0}, "HUBPREQ0_REF_FREQ_TO_PIX_FREQ"},  /* [D] rule */
  {PHX_S2_W, 4, 6, 0x03B1A, 0x00000286, 0x1fff1fff, {0}, "HUBPREQ0_PER_LINE_DELIVERY"},  /* [D] rule */
  {PHX_S2_W, 4, 4, 0x03B19, 0x0000013f, 0x1fff1fff, {0}, "HUBPREQ0_PER_LINE_DELIVERY_PRE"},  /* [F] box */
  {PHX_S2_W, 4, 4, 0x03B07, 0x2f092492, 0xff3fffff, {0}, "HUBPREQ0_PREFETCH_SETTINGS"},  /* [F] box */
  {PHX_S2_W, 4, 4, 0x03B09, 0x00000201, 0x00003f7f, {0}, "HUBPREQ0_VBLANK_PARAMETERS_0"},  /* [F] box */
  {PHX_S2_W, 4, 6, 0x03AE9, 0x0b920000, 0x3fff3fff, {0}, "HUBPREQ0_DCN_TTU_QOS_WM"},  /* [D] rule */
  {PHX_S2_W, 4, 4, 0x03AEA, 0xe0005c6c, 0xfbffffff, {0}, "HUBPREQ0_DCN_GLOBAL_TTU_CNTL"},  /* [F] box */
  {PHX_S2_W, 4, 4, 0x03AEB, 0x08005631, 0x1f7fffff, {0}, "HUBPREQ0_DCN_SURF0_TTU_CNTL0"},  /* [F] box */
  {PHX_S2_W, 4, 4, 0x03AEC, 0x00002a88, 0x007fffff, {0}, "HUBPREQ0_DCN_SURF0_TTU_CNTL1"},  /* [F] box */
  {PHX_S2_W, 5, 4, 0x0570C, 0x08980465, 0xffffffff, {0}, "DP1_DP_MSA_TIMING_PARAM1"},  /* [F] EDID */
  {PHX_S2_W, 5, 4, 0x0570D, 0x00c00029, 0xffffffff, {0}, "DP1_DP_MSA_TIMING_PARAM2"},  /* [F] EDID */
  {PHX_S2_W, 5, 4, 0x0570E, 0x002c0005, 0xffffffff, {0}, "DP1_DP_MSA_TIMING_PARAM3"},  /* [F] EDID */
  {PHX_S2_W, 5, 4, 0x0570F, 0x07800438, 0xffffffff, {0}, "DP1_DP_MSA_TIMING_PARAM4"},  /* [F] EDID */
};
static const struct phx_p0_info kPhxP0_1080p60Info = {60, 0x897, 0x464, 148500, 0x0, 0, 1, 0, 0x0, 0x0, "1080p60"};
