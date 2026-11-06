// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PhxAccel: the NullMoth accelerator built for the Navi 31 GPU that PhxBringup drives (it began on the Radeon 780M).
// Every class gets an NMX name: NVMTLDriver picks AMD/RADV when the accelerator conforms to "NMXAccel",
// and treats any registered "NVAccel" as an NVIDIA card.
#pragma once
#define NVAccel            NMXAccel
#define NVEventMachine     NMXEventMachine
#define NVDisplayMachine   NMXDisplayMachine
#define NVTask             NMXTask
#define NVMemoryMap        NMXMemoryMap
#define IOAccelNVRMCarrier IOAccelNMXCarrier
#define NVResource         NMXResource
#define NVSysMemory        NMXSysMemory
#define NVVidMemory        NMXVidMemory
#define NVSurface          NMXSurface
#define NVDisplayPipe      NMXDisplayPipe
#define NVGLContext        NMXGLContext
#define NV2DContext        NMX2DContext
#define NVCLContext        NMXCLContext
#define NVVideoContext     NMXVideoContext
