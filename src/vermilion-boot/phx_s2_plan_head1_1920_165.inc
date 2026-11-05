// SPDX-License-Identifier: MIT
/* head 1 1680x1050 -> 1920x1080x7680 over the Acer HDMI pipe-1 1080p165 plan, generated from a register capture of Linux amdgpu on the reference card (the capture tool is not included): do not edit */
#ifndef PHX_S2_ALT_TYPE
#define PHX_S2_ALT_TYPE
struct phx_s2_alt { uint32_t idx, reg, val; const char *name; };
#endif
static const uint32_t kPhxS2_165Head1AltW = 1920, kPhxS2_165Head1AltH = 1080, kPhxS2_165Head1AltPitch = 7680;
static const struct phx_s2_alt kPhxS2_165Head1Alt[] = {
  {121, 0x03BA3, 0x0000077f, "HUBPREQ1_DCSURF_SURFACE_PITCH"},
  {123, 0x03B86, 0x04380780, "HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION"},
  {125, 0x03B8A, 0x04380780, "HUBP1_DCSURF_SEC_VIEWPORT_DIMENSION"},
  {142, 0x0433D, 0x04380780, "DSCL1_RECOUT_SIZE"},
};
