// SPDX-License-Identifier: MIT
/* Acer HDMI on pipe 1, 1080p144, generated from a register capture of Linux amdgpu on the reference card (the capture tool is not included): do not edit */
#ifndef PHX_S2_TYPES
#define PHX_S2_TYPES
enum { PHX_S2_W = 1, PHX_S2_DMUB = 2, PHX_S2_POLL = 3, PHX_S2_DELAY = 4, PHX_S2_CHECK = 5 };
struct phx_s2_step { uint8_t op, stage; uint16_t pad; uint32_t reg, val, mask; uint32_t dmub[16]; const char *name; };
struct phx_s2_req { uint8_t stage, cmp; uint16_t pad; uint32_t reg, mask, val; const char *name; };
#endif
static const struct phx_s2_step kPhxS2_144[] = {
  {PHX_S2_DMUB, 1, 1, 0x00000, 0x00000000, 0x00000000, {0x0c000006, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}, "QUERY_FEATURE_CAPS"},  /* S2.1-01 */
  {PHX_S2_W, 2, 1, 0x04F9F, 0x00000003, 0x00000003, {0}, "ODM1_OPTC_INPUT_CLOCK_CONTROL"},  /* S2.2-01 */
  {PHX_S2_W, 2, 2, 0x050C5, 0x00000003, 0x00000013, {0}, "OTG1_OTG_CLOCK_CONTROL"},  /* S2.2-02 */
  {PHX_S2_W, 2, 3, 0x00144, 0x0000c000, 0x0000333b, {0}, "OTG1_PIXEL_RATE_CNTL"},  /* S2.2-03 */
  {PHX_S2_DMUB, 2, 4, 0x00000, 0x00000000, 0x00000000, {0x3c000180, 0x04020703, 0x00000000, 0xf60c0001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}, "TRANSMITTER_CONTROL"},  /* S2.2-04 */
  {PHX_S2_DMUB, 2, 5, 0x00000, 0x00000000, 0x00000000, {0x10000280, 0x00000000, 0x010f0017, 0x000000ff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}, "SET_PIXEL_CLOCK"},  /* S2.2-05 */
  {PHX_S2_DMUB, 2, 6, 0x00000, 0x00000000, 0x00000000, {0x10000280, 0x00319a70, 0x00032017, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}, "SET_PIXEL_CLOCK"},  /* S2.2-06 */
  {PHX_S2_W, 2, 7, 0x00103, 0x00000103, 0x00000131, {0}, "PHYPLLD_PIXCLK_RESYNC_CNTL"},  /* S2.2-07 */
  {PHX_S2_W, 2, 8, 0x0506A, 0x00000807, 0x00007fff, {0}, "OTG1_OTG_H_TOTAL"},  /* S2.2-08 */
  {PHX_S2_W, 2, 9, 0x0506C, 0x00200000, 0x7fff7fff, {0}, "OTG1_OTG_H_SYNC_A"},  /* S2.2-09 */
  {PHX_S2_W, 2, 10, 0x0506B, 0x007007f0, 0x7fff7fff, {0}, "OTG1_OTG_H_BLANK_START_END"},  /* S2.2-10 */
  {PHX_S2_W, 2, 11, 0x0506D, 0x00000000, 0x00030001, {0}, "OTG1_OTG_H_SYNC_A_CNTL"},  /* S2.2-11 */
  {PHX_S2_W, 2, 12, 0x0506F, 0x00000449, 0x00007fff, {0}, "OTG1_OTG_V_TOTAL"},  /* S2.2-12 */
  {PHX_S2_W, 2, 13, 0x05077, 0x00050000, 0x7fff7fff, {0}, "OTG1_OTG_V_SYNC_A"},  /* S2.2-13 */
  {PHX_S2_W, 2, 14, 0x05076, 0x000f0447, 0x7fff7fff, {0}, "OTG1_OTG_V_BLANK_START_END"},  /* S2.2-14 */
  {PHX_S2_W, 2, 15, 0x05078, 0x00000000, 0x00000101, {0}, "OTG1_OTG_V_SYNC_A_CNTL"},  /* S2.2-15 */
  {PHX_S2_W, 2, 16, 0x039EF, 0x04470001, 0xffff7fff, {0}, "VTG1_CONTROL"},  /* S2.2-16 */
  {PHX_S2_W, 2, 17, 0x05081, 0x00000200, 0x00307301, {0}, "OTG1_OTG_CONTROL"},  /* S2.2-17 */
  {PHX_S2_W, 2, 18, 0x050C6, 0x0000000d, 0x000003ff, {0}, "OTG1_OTG_VSTARTUP_PARAM"},  /* S2.2-18 */
  {PHX_S2_W, 2, 19, 0x050C7, 0x01400202, 0x03ffffff, {0}, "OTG1_OTG_VUPDATE_PARAM"},  /* S2.2-19 */
  {PHX_S2_W, 2, 20, 0x050C8, 0x0000012c, 0x0000ffff, {0}, "OTG1_OTG_VREADY_PARAM"},  /* S2.2-20 */
  {PHX_S2_W, 2, 21, 0x04F9C, 0x00000000, 0x00000033, {0}, "ODM1_OPTC_DATA_FORMAT_CONTROL"},  /* S2.2-21 */
  {PHX_S2_W, 2, 22, 0x0506E, 0x00000000, 0x00000103, {0}, "OTG1_OTG_H_TIMING_CNTL"},  /* S2.2-22 */
  {PHX_S2_W, 2, 23, 0x04DA6, 0x00000001, 0x00000011, {0}, "OPP_PIPE1_OPP_PIPE_CONTROL"},  /* S2.2-23 */
  {PHX_S2_W, 2, 24, 0x04D70, 0x07800438, 0x3fff3fff, {0}, "DPG1_DPG_DIMENSIONS"},  /* S2.2-24 */
  {PHX_S2_W, 2, 25, 0x04D74, 0x00000000, 0x3fff3fff, {0}, "DPG1_DPG_OFFSET_SEGMENT"},  /* S2.2-25 */
  {PHX_S2_W, 2, 26, 0x04D73, 0x00000000, 0xffffffff, {0}, "DPG1_DPG_COLOUR_B_CB"},  /* S2.2-26 */
  {PHX_S2_W, 2, 27, 0x04D72, 0x00000000, 0xffffffff, {0}, "DPG1_DPG_COLOUR_G_Y"},  /* S2.2-27 */
  {PHX_S2_W, 2, 28, 0x04D71, 0x00000000, 0xffffffff, {0}, "DPG1_DPG_COLOUR_R_CR"},  /* S2.2-28 */
  {PHX_S2_W, 2, 29, 0x04D6E, 0x00000041, 0x01ff3171, {0}, "DPG1_DPG_CONTROL"},  /* S2.2-29 */
  {PHX_S2_W, 2, 30, 0x04F9B, 0xfff10000, 0xffff0303, {0}, "ODM1_OPTC_DATA_SOURCE_SELECT"},  /* S2.2-30 */
  {PHX_S2_W, 2, 31, 0x039EF, 0x84470001, 0xffff7fff, {0}, "VTG1_CONTROL"},  /* S2.2-31 */
  {PHX_S2_W, 2, 32, 0x05081, 0x00000201, 0x00307301, {0}, "OTG1_OTG_CONTROL"},  /* S2.2-32 */
  {PHX_S2_W, 2, 33, 0x0584B, 0x00000101, 0xd017f177, {0}, "DIG3_DIG_FE_CNTL"},  /* S2.2-33 */
  {PHX_S2_W, 2, 34, 0x05871, 0x00030000, 0x70077f07, {0}, "DIG3_DIG_BE_CNTL"},  /* S2.2-34 */
  {PHX_S2_W, 2, 35, 0x0584B, 0x00000111, 0xd017f177, {0}, "DIG3_DIG_FE_CNTL"},  /* S2.2-35 */
  {PHX_S2_DMUB, 2, 36, 0x00000, 0x00000000, 0x00000000, {0x0c000080, 0x04030f03, 0x00007efc, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}, "DIG_STREAM_SETUP"},  /* S2.2-36 */
  {PHX_S2_W, 2, 37, 0x05854, 0x00010019, 0x313f021f, {0}, "DIG3_HDMI_CONTROL"},  /* S2.2-37 */
  {PHX_S2_W, 2, 38, 0x05858, 0x00000031, 0x3f3f1331, {0}, "DIG3_HDMI_VBI_PACKET_CONTROL"},  /* S2.2-38 */
  {PHX_S2_W, 2, 39, 0x0585E, 0x00000004, 0x00001015, {0}, "DIG3_HDMI_GC"},  /* S2.2-39 */
  {PHX_S2_W, 2, 40, 0x05867, 0x00001000, 0x00001100, {0}, "DIG3_HDMI_DB_CONTROL"},  /* S2.2-40 */
  {PHX_S2_W, 2, 41, 0x05870, 0x00000001, 0x00000001, {0}, "DIG3_AFMT_CNTL"},  /* S2.2-41 */
  {PHX_S2_W, 2, 42, 0x0582C, 0x00000010, 0x00000000, {0}, "VPG3_VPG_GENERIC_STATUS"},  /* S2.2-42 */
  {PHX_S2_W, 2, 43, 0x05828, 0x00000000, 0x000000ff, {0}, "VPG3_VPG_GENERIC_PACKET_ACCESS_CTRL"},  /* S2.2-43 */
  {PHX_S2_W, 2, 44, 0x05829, 0x000d0282, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-44 */
  {PHX_S2_W, 2, 45, 0x05829, 0x0828102f, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-45 */
  {PHX_S2_W, 2, 46, 0x05829, 0x00000000, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-46 */
  {PHX_S2_W, 2, 47, 0x05829, 0x00000000, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-47 */
  {PHX_S2_W, 2, 48, 0x05829, 0x00000000, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-48 */
  {PHX_S2_W, 2, 49, 0x05829, 0x00000000, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-49 */
  {PHX_S2_W, 2, 50, 0x05829, 0x00000000, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-50 */
  {PHX_S2_W, 2, 51, 0x05829, 0x00000000, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-51 */
  {PHX_S2_W, 2, 52, 0x05829, 0x00000000, 0x00000000, {0}, "VPG3_VPG_GENERIC_PACKET_DATA"},  /* S2.2-52 */
  {PHX_S2_W, 2, 53, 0x0582B, 0x00000001, 0x00000000, {0}, "VPG3_VPG_GSP_IMMEDIATE_UPDATE_CTRL"},  /* S2.2-53 */
  {PHX_S2_W, 2, 54, 0x0585B, 0x00000003, 0xffffffff, {0}, "DIG3_HDMI_GENERIC_PACKET_CONTROL0"},  /* S2.2-54 */
  {PHX_S2_W, 2, 55, 0x0585F, 0x00000002, 0xffffffff, {0}, "DIG3_HDMI_GENERIC_PACKET_CONTROL1"},  /* S2.2-55 */
  {PHX_S2_W, 2, 56, 0x05871, 0x00030800, 0x70077f07, {0}, "DIG3_DIG_BE_CNTL"},  /* S2.2-56 */
  {PHX_S2_W, 2, 57, 0x05851, 0x0000001c, 0x000001ff, {0}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-57 */
  {PHX_S2_W, 2, 58, 0x05851, 0x0000001e, 0x000001ff, {0}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-58 */
  {PHX_S2_W, 2, 59, 0x05851, 0x0000001c, 0x000001ff, {0}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-59 */
  {PHX_S2_W, 2, 60, 0x05851, 0x0000001d, 0x000001ff, {0}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-60 */
  {PHX_S2_DMUB, 2, 61, 0x00000, 0x00000000, 0x00000000, {0x3c000180, 0x04030103, 0x00007efc, 0xf6000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}, "TRANSMITTER_CONTROL"},  /* S2.2-61 */
  {PHX_S2_W, 2, 62, 0x05871, 0x00030000, 0x70077f07, {0}, "DIG3_DIG_BE_CNTL"},  /* S2.2-62 */
  {PHX_S2_W, 2, 63, 0x05871, 0x00030800, 0x70077f07, {0}, "DIG3_DIG_BE_CNTL"},  /* S2.2-63 */
  {PHX_S2_W, 2, 64, 0x05851, 0x2000001d, 0x000001ff, {0}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-64 */
  {PHX_S2_W, 2, 65, 0x05851, 0x2000001f, 0x000001ff, {0}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-65 */
  {PHX_S2_POLL, 2, 66, 0x05851, 0x00100000, 0x00100000, {1000}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-66 */
  {PHX_S2_W, 2, 67, 0x05851, 0x0010001d, 0x000001ff, {0}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-67 */
  {PHX_S2_W, 2, 68, 0x05851, 0x0000001d, 0x000001ff, {0}, "DIG3_DIG_FIFO_CTRL0"},  /* S2.2-68 */
  {PHX_S2_W, 3, 1, 0x050CA, 0x00000001, 0x00000001, {0}, "OTG1_OTG_MASTER_UPDATE_LOCK"},  /* S2.3-01 */
  {PHX_S2_W, 3, 2, 0x03B90, 0x00000001, 0xf1011111, {0}, "HUBP1_HUBP_CLK_CNTL"},  /* S2.3-02 */
  {PHX_S2_W, 3, 3, 0x042F0, 0x70000010, 0x71055510, {0}, "DPP_TOP1_DPP_CONTROL"},  /* S2.3-03 */
  {PHX_S2_W, 3, 4, 0x03B8F, 0x000f001b, 0x0b00f7f5, {1}, "HUBP1_DCHUBP_CNTL"},  /* S2.3-04, read-back deferred to the unlock */
  {PHX_S2_W, 3, 5, 0x03B8D, 0x055d0300, 0x077f1f77, {1}, "HUBP1_DCHUBP_REQ_SIZE_CONFIG"},  /* S2.3-05, read-back deferred to the unlock */
  {PHX_S2_W, 3, 6, 0x03B8E, 0x00000000, 0x007f1f77, {1}, "HUBP1_DCHUBP_REQ_SIZE_CONFIG_C"},  /* S2.3-06, read-back deferred to the unlock */
  {PHX_S2_W, 3, 7, 0x03B82, 0x00000000, 0x000730c7, {1}, "HUBP1_DCSURF_ADDR_CONFIG"},  /* S2.3-07, read-back deferred to the unlock */
  {PHX_S2_W, 3, 8, 0x03B83, 0x00000080, 0x00000b9f, {1}, "HUBP1_DCSURF_TILING_CONFIG"},  /* S2.3-08, read-back deferred to the unlock */
  {PHX_S2_W, 3, 9, 0x03B81, 0x00000008, 0x00000f7f, {1}, "HUBP1_DCSURF_SURFACE_CONFIG"},  /* S2.3-09, read-back deferred to the unlock */
  {PHX_S2_W, 3, 10, 0x03C08, 0x00e40000, 0xfffffff0, {1}, "HUBPRET1_HUBPRET_CONTROL"},  /* S2.3-10, read-back deferred to the unlock */
  {PHX_S2_W, 3, 11, 0x03BC4, 0x00000055, 0x000000ff, {1}, "HUBPREQ1_DCN_EXPANSION_MODE"},  /* S2.3-11, read-back deferred to the unlock */
  {PHX_S2_W, 3, 12, 0x03BB7, 0x04100000, 0x3ff730f3, {1}, "HUBPREQ1_DCSURF_FLIP_CONTROL"},  /* S2.3-12, read-back deferred to the unlock */
  {PHX_S2_W, 3, 13, 0x03BB8, 0x00000040, 0x000017ff, {1}, "HUBPREQ1_DCSURF_FLIP_CONTROL2"},  /* S2.3-13, read-back deferred to the unlock */
  {PHX_S2_W, 3, 14, 0x03BA5, 0x00000000, 0x0000000f, {1}, "HUBPREQ1_VMID_SETTINGS_0"},  /* S2.3-14, read-back deferred to the unlock */
  {PHX_S2_W, 3, 15, 0x03BD0, 0x00200000, 0x3fffffff, {1}, "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_LOW_ADDR"},  /* S2.3-15, read-back deferred to the unlock */
  {PHX_S2_W, 3, 16, 0x03BD1, 0x00213fff, 0x3fffffff, {1}, "HUBPREQ1_DCN_VM_SYSTEM_APERTURE_HIGH_ADDR"},  /* S2.3-16, read-back deferred to the unlock */
  {PHX_S2_W, 3, 17, 0x03BDF, 0x000f0011, 0x7fff1fff, {1}, "HUBPREQ1_BLANK_OFFSET_0"},  /* S2.3-17, read-back deferred to the unlock */
  {PHX_S2_W, 3, 18, 0x03BE0, 0x00001136, 0x0003ffff, {1}, "HUBPREQ1_BLANK_OFFSET_1"},  /* S2.3-18, read-back deferred to the unlock */
  {PHX_S2_W, 3, 19, 0x03BE1, 0x00013c3a, 0x001fffff, {1}, "HUBPREQ1_DST_DIMENSIONS"},  /* S2.3-19, read-back deferred to the unlock */
  {PHX_S2_W, 3, 20, 0x03BE2, 0x00000000, 0x00071fff, {1}, "HUBPREQ1_DST_AFTER_SCALER"},  /* S2.3-20, read-back deferred to the unlock */
  {PHX_S2_W, 3, 21, 0x03BF8, 0x00013aff, 0x001fffff, {1}, "HUBPREQ1_REF_FREQ_TO_PIX_FREQ"},  /* S2.3-21, read-back deferred to the unlock */
  {PHX_S2_W, 3, 22, 0x03BE5, 0x00000301, 0x00003f7f, {1}, "HUBPREQ1_VBLANK_PARAMETERS_0"},  /* S2.3-22, read-back deferred to the unlock */
  {PHX_S2_W, 3, 23, 0x03BE6, 0x000001a4, 0x007fffff, {1}, "HUBPREQ1_VBLANK_PARAMETERS_1"},  /* S2.3-23, read-back deferred to the unlock */
  {PHX_S2_W, 3, 24, 0x03BE7, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_VBLANK_PARAMETERS_2"},  /* S2.3-24, read-back deferred to the unlock */
  {PHX_S2_W, 3, 25, 0x03BE8, 0x000001a4, 0x007fffff, {1}, "HUBPREQ1_VBLANK_PARAMETERS_3"},  /* S2.3-25, read-back deferred to the unlock */
  {PHX_S2_W, 3, 26, 0x03BE9, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_VBLANK_PARAMETERS_4"},  /* S2.3-26, read-back deferred to the unlock */
  {PHX_S2_W, 3, 27, 0x03BED, 0x00000200, 0x0001ffff, {1}, "HUBPREQ1_NOM_PARAMETERS_0"},  /* S2.3-27, read-back deferred to the unlock */
  {PHX_S2_W, 3, 28, 0x03BEE, 0x0000d249, 0x007fffff, {1}, "HUBPREQ1_NOM_PARAMETERS_1"},  /* S2.3-28, read-back deferred to the unlock */
  {PHX_S2_W, 3, 29, 0x03BEF, 0x00000000, 0x0001ffff, {1}, "HUBPREQ1_NOM_PARAMETERS_2"},  /* S2.3-29, read-back deferred to the unlock */
  {PHX_S2_W, 3, 30, 0x03BF0, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_NOM_PARAMETERS_3"},  /* S2.3-30, read-back deferred to the unlock */
  {PHX_S2_W, 3, 31, 0x03BF1, 0x00000020, 0x0001ffff, {1}, "HUBPREQ1_NOM_PARAMETERS_4"},  /* S2.3-31, read-back deferred to the unlock */
  {PHX_S2_W, 3, 32, 0x03BF2, 0x00000d24, 0x007fffff, {1}, "HUBPREQ1_NOM_PARAMETERS_5"},  /* S2.3-32, read-back deferred to the unlock */
  {PHX_S2_W, 3, 33, 0x03BF3, 0x00000000, 0x0001ffff, {1}, "HUBPREQ1_NOM_PARAMETERS_6"},  /* S2.3-33, read-back deferred to the unlock */
  {PHX_S2_W, 3, 34, 0x03BF4, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_NOM_PARAMETERS_7"},  /* S2.3-34, read-back deferred to the unlock */
  {PHX_S2_W, 3, 35, 0x03BF6, 0x00000127, 0x1fff1fff, {1}, "HUBPREQ1_PER_LINE_DELIVERY"},  /* S2.3-35, read-back deferred to the unlock */
  {PHX_S2_W, 3, 36, 0x03BF5, 0x00000127, 0x1fff1fff, {1}, "HUBPREQ1_PER_LINE_DELIVERY_PRE"},  /* S2.3-36, read-back deferred to the unlock */
  {PHX_S2_W, 3, 37, 0x03BE3, 0x28080000, 0xff3fffff, {1}, "HUBPREQ1_PREFETCH_SETTINGS"},  /* S2.3-37, read-back deferred to the unlock */
  {PHX_S2_W, 3, 38, 0x03BE4, 0x00080000, 0x003fffff, {1}, "HUBPREQ1_PREFETCH_SETTINGS_C"},  /* S2.3-38, read-back deferred to the unlock */
  {PHX_S2_W, 3, 39, 0x03BC5, 0x04f00000, 0x3fff3fff, {1}, "HUBPREQ1_DCN_TTU_QOS_WM"},  /* S2.3-39, read-back deferred to the unlock */
  {PHX_S2_W, 3, 40, 0x03BC6, 0xe00007a5, 0xfbffffff, {1}, "HUBPREQ1_DCN_GLOBAL_TTU_CNTL"},  /* S2.3-40, read-back deferred to the unlock */
  {PHX_S2_W, 3, 41, 0x03BC7, 0x0800275f, 0x1f7fffff, {1}, "HUBPREQ1_DCN_SURF0_TTU_CNTL0"},  /* S2.3-41, read-back deferred to the unlock */
  {PHX_S2_W, 3, 42, 0x03BC8, 0x0000275f, 0x007fffff, {1}, "HUBPREQ1_DCN_SURF0_TTU_CNTL1"},  /* S2.3-42, read-back deferred to the unlock */
  {PHX_S2_W, 3, 43, 0x03BC9, 0x00000000, 0x1f7fffff, {1}, "HUBPREQ1_DCN_SURF1_TTU_CNTL0"},  /* S2.3-43, read-back deferred to the unlock */
  {PHX_S2_W, 3, 44, 0x03BCA, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_DCN_SURF1_TTU_CNTL1"},  /* S2.3-44, read-back deferred to the unlock */
  {PHX_S2_W, 3, 45, 0x03BCB, 0x00000000, 0x1f7fffff, {1}, "HUBPREQ1_DCN_CUR0_TTU_CNTL0"},  /* S2.3-45, read-back deferred to the unlock */
  {PHX_S2_W, 3, 46, 0x03BCC, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_DCN_CUR0_TTU_CNTL1"},  /* S2.3-46, read-back deferred to the unlock */
  {PHX_S2_W, 3, 47, 0x03BCE, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_DCN_CUR1_TTU_CNTL1"},  /* S2.3-47, read-back deferred to the unlock */
  {PHX_S2_W, 3, 48, 0x03BEA, 0x00000000, 0x00003f7f, {1}, "HUBPREQ1_FLIP_PARAMETERS_0"},  /* S2.3-48, read-back deferred to the unlock */
  {PHX_S2_W, 3, 49, 0x03BEB, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_FLIP_PARAMETERS_1"},  /* S2.3-49, read-back deferred to the unlock */
  {PHX_S2_W, 3, 50, 0x03BEC, 0x00000000, 0x007fffff, {1}, "HUBPREQ1_FLIP_PARAMETERS_2"},  /* S2.3-50, read-back deferred to the unlock */
  {PHX_S2_W, 3, 51, 0x03BA3, 0x000006bf, 0x3fff3fff, {1}, "HUBPREQ1_DCSURF_SURFACE_PITCH"},  /* S2.3-51, read-back deferred to the unlock */
  {PHX_S2_W, 3, 52, 0x03B85, 0x00000000, 0x3fff3fff, {1}, "HUBP1_DCSURF_PRI_VIEWPORT_START"},  /* S2.3-52, read-back deferred to the unlock */
  {PHX_S2_W, 3, 53, 0x03B86, 0x041a0690, 0x3fff3fff, {1}, "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION"},  /* S2.3-53, read-back deferred to the unlock */
  {PHX_S2_W, 3, 54, 0x03B89, 0x00000000, 0x3fff3fff, {1}, "HUBP1_DCSURF_SEC_VIEWPORT_START"},  /* S2.3-54, read-back deferred to the unlock */
  {PHX_S2_W, 3, 55, 0x03B8A, 0x041a0690, 0x3fff3fff, {1}, "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION"},  /* S2.3-55, read-back deferred to the unlock */
  {PHX_S2_W, 3, 56, 0x0434B, 0x00000001, 0x00000001, {1}, "CM1_CM_CONTROL"},  /* S2.3-56, read-back deferred to the unlock */
  {PHX_S2_W, 3, 57, 0x04326, 0x00000001, 0x00110107, {1}, "DSCL1_SCL_MODE"},  /* S2.3-57, read-back deferred to the unlock */
  {PHX_S2_W, 3, 58, 0x04327, 0x00000000, 0x00007777, {1}, "DSCL1_SCL_TAP_CONTROL"},  /* S2.3-58, read-back deferred to the unlock */
  {PHX_S2_W, 3, 59, 0x04328, 0x00000001, 0x00000001, {1}, "DSCL1_DSCL_CONTROL"},  /* S2.3-59, read-back deferred to the unlock */
  {PHX_S2_W, 3, 60, 0x04329, 0x01110111, 0x07110711, {1}, "DSCL1_DSCL_2TAP_CONTROL"},  /* S2.3-60, read-back deferred to the unlock */
  {PHX_S2_W, 3, 61, 0x0432B, 0x01000000, 0x07ffffff, {1}, "DSCL1_SCL_HORZ_FILTER_SCALE_RATIO"},  /* S2.3-61, read-back deferred to the unlock */
  {PHX_S2_W, 3, 62, 0x0432C, 0x01000000, 0x0fffffff, {1}, "DSCL1_SCL_HORZ_FILTER_INIT"},  /* S2.3-62, read-back deferred to the unlock */
  {PHX_S2_W, 3, 63, 0x0432F, 0x01000000, 0x07ffffff, {1}, "DSCL1_SCL_VERT_FILTER_SCALE_RATIO"},  /* S2.3-63, read-back deferred to the unlock */
  {PHX_S2_W, 3, 64, 0x04330, 0x01000000, 0x0fffffff, {1}, "DSCL1_SCL_VERT_FILTER_INIT"},  /* S2.3-64, read-back deferred to the unlock */
  {PHX_S2_W, 3, 65, 0x04335, 0x00000000, 0xffffffff, {1}, "DSCL1_SCL_BLACK_COLOR"},  /* S2.3-65, read-back deferred to the unlock */
  {PHX_S2_W, 3, 66, 0x04337, 0x00000100, 0x00003303, {1}, "DSCL1_DSCL_AUTOCAL"},  /* S2.3-66, read-back deferred to the unlock */
  {PHX_S2_W, 3, 67, 0x04340, 0x0a0a3f00, 0x00003f03, {1}, "DSCL1_LB_MEMORY_CTRL"},  /* S2.3-67, read-back deferred to the unlock */
  {PHX_S2_W, 3, 68, 0x04345, 0x00000000, 0x00000107, {1}, "DSCL1_OBUF_MEM_PWR_CTRL"},  /* S2.3-68, read-back deferred to the unlock */
  {PHX_S2_W, 3, 69, 0x0433A, 0x007007f0, 0x3fff3fff, {1}, "DSCL1_OTG_H_BLANK"},  /* S2.3-69, read-back deferred to the unlock */
  {PHX_S2_W, 3, 70, 0x0433B, 0x000f0447, 0x3fff3fff, {1}, "DSCL1_OTG_V_BLANK"},  /* S2.3-70, read-back deferred to the unlock */
  {PHX_S2_W, 3, 71, 0x0433C, 0x00000000, 0x1fff1fff, {1}, "DSCL1_RECOUT_START"},  /* S2.3-71, read-back deferred to the unlock */
  {PHX_S2_W, 3, 72, 0x0433D, 0x041a0690, 0x3fff3fff, {1}, "DSCL1_RECOUT_SIZE"},  /* S2.3-72, read-back deferred to the unlock */
  {PHX_S2_W, 3, 73, 0x0433E, 0x04380780, 0x3fff3fff, {1}, "DSCL1_MPC_SIZE"},  /* S2.3-73, read-back deferred to the unlock */
  {PHX_S2_W, 3, 74, 0x09016, 0x0000000f, 0x0000000f, {1}, "MPCC1_MPCC_BOT_SEL"},  /* S2.3-74, read-back deferred to the unlock */
  {PHX_S2_W, 3, 75, 0x09018, 0xffff0461, 0xffff0ff3, {1}, "MPCC1_MPCC_CONTROL"},  /* S2.3-75, read-back deferred to the unlock */
  {PHX_S2_W, 3, 76, 0x09015, 0x00000001, 0x0000000f, {1}, "MPCC1_MPCC_TOP_SEL"},  /* S2.3-76, read-back deferred to the unlock */
  {PHX_S2_W, 3, 77, 0x09017, 0x00000001, 0x0000000f, {1}, "MPCC1_MPCC_OPP_ID"},  /* S2.3-77, read-back deferred to the unlock */
  {PHX_S2_W, 3, 78, 0x093DC, 0x00004101, 0x0000070f, {1}, "MPC_OUT1_MUX"},  /* S2.3-78, read-back deferred to the unlock */
  {PHX_S2_W, 3, 79, 0x03BA7, 0x00000080, 0x0000ffff, {1}, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH"},  /* S2.3-79, read-back deferred to the unlock */
  {PHX_S2_W, 3, 80, 0x03BAB, 0x00000080, 0x0000ffff, {1}, "HUBPREQ1_DCSURF_SECONDARY_SURFACE_ADDRESS_HIGH"},  /* S2.3-80, read-back deferred to the unlock */
  {PHX_S2_W, 3, 81, 0x03BA6, 0x04000000, 0xffffffff, {1}, "HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS"},  /* S2.3-81, read-back deferred to the unlock */
  {PHX_S2_W, 3, 82, 0x03B8F, 0x000f001a, 0x0b00f7f5, {1}, "HUBP1_DCHUBP_CNTL"},  /* S2.3-82, read-back deferred to the unlock */
  {PHX_S2_W, 3, 83, 0x050CA, 0x00000000, 0x00000001, {2}, "OTG1_OTG_MASTER_UPDATE_LOCK"},  /* S2.3-83, unlock: verify the deferred read-backs */
  {PHX_S2_DELAY, 3, 84, 0x00000, 0x0000c350, 0x00000000, {0}, "delay"},  /* S2.3-84 */
  {PHX_S2_CHECK, 3, 85, 0x03BC0, 0x04000000, 0xffffffff, {0}, "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE"},  /* S2.3-85 */
  {PHX_S2_CHECK, 3, 85, 0x03BC1, 0x00000080, 0xffffffff, {0}, "HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH"},  /* S2.3-85 */
  {PHX_S2_CHECK, 3, 85, 0x03B8F, 0x00000000, 0x70000000, {0}, "HUBP1_DCHUBP_CNTL"},  /* S2.3-85 */
  {PHX_S2_CHECK, 3, 85, 0x04F9A, 0x00000000, 0x00000400, {0}, "ODM1_OPTC_INPUT_GLOBAL_CONTROL"},  /* S2.3-85 */
  {PHX_S2_W, 3, 86, 0x050CA, 0x00000001, 0x00000001, {0}, "OTG1_OTG_MASTER_UPDATE_LOCK"},  /* S2.3-86 */
  {PHX_S2_W, 3, 87, 0x04D6E, 0x00000000, 0x01ff3171, {1}, "DPG1_DPG_CONTROL"},  /* S2.3-87, read-back deferred to the unlock */
  {PHX_S2_W, 3, 88, 0x050CA, 0x00000000, 0x00000001, {2}, "OTG1_OTG_MASTER_UPDATE_LOCK"},  /* S2.3-88, unlock: verify the deferred read-backs */
};
static const struct phx_s2_req kPhxS2_144Req[] = {
  {1, 0, 0x0, 0x036A4, 0xffffffff, 0x07008742, "DMCUB_SCRATCH1"},
  {1, 0, 0x0, 0x036B6, 0x00010000, 0x00010000, "DMCUB_CNTL"},
  {1, 0, 0x0, 0x03695, 0xffffffff, 0x00002000, "DMCUB_INBOX1_SIZE"},
  {1, 0, 0x0, 0x03694, 0xffffffff, 0x80000000, "DMCUB_INBOX1_BASE_ADDRESS"},
  {1, 0, 0x0, 0x03661, 0x80000000, 0x80000000, "DMCUB_REGION4_TOP_ADDRESS"},
  {1, 2, 0, 0x03696, 0x00000000, 0x00000000, "DMCUB_INBOX1_WPTR==RPTR"},
  {2, 0, 0x0, 0x05081, 0x00000001, 0x00000000, "OTG1_OTG_CONTROL"},
  {2, 0, 0x0, 0x05872, 0x00000001, 0x00000000, "DIG3_DIG_BE_EN_CNTL"},
  {2, 0, 0x0, 0x0584B, 0x00000007, 0x00000000, "DIG3_DIG_FE_CNTL"},
  {2, 0, 0x0, 0x00163, 0x00000001, 0x00000000, "SYMCLKD_CLOCK_ENABLE"},
  {2, 3, 0x101, 0x00103, 0x00000101, 0x00000000, "PHYPLLD_PIXCLK_RESYNC_CNTL"},
  {2, 0, 0x0, 0x09017, 0x0000000f, 0x0000000f, "MPCC1_MPCC_OPP_ID"},
  {2, 0, 0x0, 0x04DA6, 0x00000001, 0x00000000, "OPP_PIPE1_OPP_PIPE_CONTROL"},
  {2, 0, 0x0, 0x053D4, 0x00000002, 0x00000002, "HPD0_DC_HPD_INT_STATUS"},
  {2, 0, 0x0, 0x05001, 0x00000001, 0x00000001, "OTG0_OTG_CONTROL"},
  {2, 0, 0x0, 0x0564B, 0x00000007, 0x00000000, "DIG1_DIG_FE_CNTL"},
  {2, 0, 0x0, 0x09002, 0x0000000f, 0x00000000, "MPCC0_MPCC_OPP_ID"},
  {2, 0, 0x0, 0x0012F, 0x00000038, 0x00000030, "OTG_PIXEL_RATE_DIV"},
  {2, 1, 0x0, 0x00124, 0x0000007f, 0x00000033, "DENTIST_DISPCLK_CNTL"},
  {2, 1, 0x0, 0x00124, 0x7f000000, 0x33000000, "DENTIST_DISPCLK_CNTL"},
  {3, 0, 0x0, 0x03555, 0x00000001, 0x00000001, "DC_IP_REQUEST_CNTL"},
  {3, 0, 0x0, 0x03543, 0xffffffff, 0x10000000, "DOMAIN1_PG_STATUS"},
  {3, 3, 0x9, 0x0397C, 0x0000001f, 0x00000003, "DCHUBBUB_DET1_CTRL"},
  {3, 0, 0x0, 0x039BC, 0x00000003, 0x00000002, "DCHUBBUB_ARB_DRAM_STATE_CNTL"},
  {3, 0, 0x0, 0x03935, 0x00ffffff, 0x00008000, "DCN_VM_FB_LOCATION_BASE"},
};
static const struct phx_s2_step kPhxS2_144Undo[] = {
  {PHX_S2_W, 3, 1, 0x050CA, 0x00000001, 0x00000001, {0}, "OTG1_OTG_MASTER_UPDATE_LOCK"},  /* undo 01, gate S2.3-01 */
  {PHX_S2_W, 3, 1, 0x04D6E, 0x00000041, 0x01ff3171, {1}, "DPG1_DPG_CONTROL"},  /* undo 02, gate S2.3-01, read-back deferred to the unlock */
  {PHX_S2_W, 3, 1, 0x093DC, 0x0000010f, 0x0000070f, {1}, "MPC_OUT1_MUX"},  /* undo 03, gate S2.3-01, read-back deferred to the unlock */
  {PHX_S2_W, 3, 1, 0x09015, 0x0000000f, 0x0000000f, {1}, "MPCC1_MPCC_TOP_SEL"},  /* undo 04, gate S2.3-01, read-back deferred to the unlock */
  {PHX_S2_W, 3, 1, 0x09016, 0x0000000f, 0x0000000f, {1}, "MPCC1_MPCC_BOT_SEL"},  /* undo 05, gate S2.3-01, read-back deferred to the unlock */
  {PHX_S2_W, 3, 1, 0x09017, 0x0000000f, 0x0000000f, {1}, "MPCC1_MPCC_OPP_ID"},  /* undo 06, gate S2.3-01, read-back deferred to the unlock */
  {PHX_S2_W, 3, 1, 0x050CA, 0x00000000, 0x00000001, {2}, "OTG1_OTG_MASTER_UPDATE_LOCK"},  /* undo 07, gate S2.3-01, unlock: verify the deferred read-backs */
  {PHX_S2_DELAY, 3, 1, 0x00000, 0x0000c350, 0x00000000, {0}, "delay"},  /* undo 08, gate S2.3-01 */
  {PHX_S2_W, 3, 1, 0x03B8F, 0x000f001b, 0x0b00f7f5, {0}, "HUBP1_DCHUBP_CNTL"},  /* undo 09, gate S2.3-01 */
  {PHX_S2_W, 2, 33, 0x0585E, 0x00000005, 0x00001015, {0}, "DIG3_HDMI_GC"},  /* undo 10, gate S2.2-33 */
  {PHX_S2_W, 2, 33, 0x0585B, 0x00000000, 0xffffffff, {0}, "DIG3_HDMI_GENERIC_PACKET_CONTROL0"},  /* undo 11, gate S2.2-33 */
  {PHX_S2_W, 2, 33, 0x0585F, 0x00000000, 0xffffffff, {0}, "DIG3_HDMI_GENERIC_PACKET_CONTROL1"},  /* undo 12, gate S2.2-33 */
  {PHX_S2_W, 2, 33, 0x05871, 0x00030000, 0x70077f07, {0}, "DIG3_DIG_BE_CNTL"},  /* undo 13, gate S2.2-33 */
  {PHX_S2_DMUB, 2, 61, 0x00000, 0x00000000, 0x00000000, {0x3c000180, 0x00030003, 0x00000000, 0xf60c0001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}, "TRANSMITTER_CONTROL"},  /* undo 14, gate S2.2-61 */
  {PHX_S2_W, 2, 1, 0x00144, 0x0000c000, 0x0000333b, {0}, "OTG1_PIXEL_RATE_CNTL"},  /* undo 15, gate S2.2-01 */
  {PHX_S2_W, 2, 1, 0x04F9B, 0xffff0000, 0xffff0303, {0}, "ODM1_OPTC_DATA_SOURCE_SELECT"},  /* undo 16, gate S2.2-01 */
  {PHX_S2_W, 2, 1, 0x05081, 0x00010200, 0x00307301, {0}, "OTG1_OTG_CONTROL"},  /* undo 17, gate S2.2-01 */
  {PHX_S2_W, 2, 1, 0x039EF, 0x04470001, 0xffff7fff, {0}, "VTG1_CONTROL"},  /* undo 18, gate S2.2-01 */
  {PHX_S2_POLL, 2, 1, 0x050C5, 0x00000000, 0x00010000, {100000}, "OTG1_OTG_CLOCK_CONTROL"},  /* undo 19, gate S2.2-01 */
  {PHX_S2_DMUB, 2, 6, 0x00000, 0x00000000, 0x00000000, {0x10000280, 0x00000000, 0x010f0017, 0x000000ff, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}, "SET_PIXEL_CLOCK"},  /* undo 20, gate S2.2-06 */
};
static const uint32_t kPhxS2_144NoRestore[] = {0x00103, 0x00144, 0x03B8F, 0x050CA, 0x05828, 0x05829, 0x0582B, 0x0582C};
static const uint32_t kPhxS2_144Otg1Hz = 144;   /* the OTG1 rate the plane/stream post-check expects */
