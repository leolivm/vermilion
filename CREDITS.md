# Credits

Vermilion stands on other people's work. Each project below is named with what Vermilion took from it; the licenses
and notices are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and [`LICENSES/`](LICENSES).

**Vermilion is independent and unofficial.** It is not affiliated with, reviewed, endorsed or supported by any project
or company named here. Please do not ask them for support with Vermilion.

| Project | Link | What Vermilion took from it |
|---|---|---|
| **NullMoth** — NVIDIA driver for macOS | https://github.com/nullmoth/nvidia-macos-driver | The kernel accelerator and framebuffer design that `VermilionAccel.kext` (formerly `PhxAccel.kext`) is derived from (NVRM accelerator, AGDC, the IOAcceleratorFamily2 interoperability headers) and the Metal plugin (`src/nmplugin`, Metal on Vulkan). PolyForm Noncommercial 1.0.0, Required Notice in [`LICENSE`](LICENSE). Also the shader translator's wrapper (`src/translator/wrapper`, LGPL-3.0-or-later) and the copy of metal2vulkan it ships (tag v2.6.5). |
| **metal2vulkan** (Anees Iqbal) | https://github.com/steelbrain/metal2vulkan | The Metal AIR to Vulkan SPIR-V translator core (`src/translator/translator`, unmodified, as shipped in NullMoth's tree); LGPL-3.0-or-later. |
| **1401** (NullMoth) | https://github.com/nullmoth/1401 | No code. The OpenCore setup the reference machine started from was generated with it. |
| **nullarc** (luuumi-dev) | https://github.com/luuumi-dev/nullarc | The starting point of this tree: the boot kext skeleton (`PhxBringup`, now `VermilionBoot.kext`) and its user-client ABI, the AMD adaptation of NullMoth's accelerator (`PhxAccel`), `phxctl`, `mesboot`, the license layout this repository mirrors, and Mesa patches 0010-0015. nullarc targets the Radeon 780M (AMD "Phoenix" APU); the `Phx` prefix of the kexts, tools and `debug.phx*` sysctls comes from there and does not refer to any product. The nullarc name and mark are © luuumi-dev and are not used as this project's name. |
| **Cobalt64** (Navi48-MacOS) | https://github.com/Almosst-DEV/Cobalt64 · https://cobalt64.com | The RADV Darwin backend (Mesa patches 0001-0009), the user-client ABI layout (`PhxNativeABI.h` after `Navi48NativeABI.h`), the PSP ring method (`phx_psp_smu.cpp` after `psp_v14_0.cpp`), the boot ordering (one clean WindowServer restart) and the mock-harness pattern of `tests/mock/run.sh`. |
| **RX4Darwin** (AMDRadeonRDNA4.kext, Sunneva N. Mariu) | https://github.com/RX4Darwin/AMDRadeonRDNA4.kext | AUX transaction defer handling, the DMUB inbox protocol and MM_INDEX/MM_DATA access, HPD debounce and EDID retry timing (`PhxDdc.h`, `PhxDmub.h`, `PhxHpd.h`); its probe captured the HDMI monitor EDID used as a fallback example. |
| **RedNavi** | https://github.com/Helemen7/RedNavi | The AUX transfer sequence, the pipe quiesce / program / mode-set order and the VBIOS object walk (`PhxDdc.h`, `PhxPipe0.h`, `PhxVbios.h`), followed as methods. |
| **RadeArc** (Kreytus) | https://github.com/Kreytus/RadeArc | No code. Studied as a sibling nullarc downstream (damage-only frame copy, BO-handle ceiling). |
| **Linux amdgpu / DC** (AMD and the amdgpu developers) | https://git.kernel.org/ (`drivers/gpu/drm/amd/`) | Register definitions, firmware message numbers, the bring-up order, the display algorithms (regamma, watermarks, DET, prefetch) and two header files; MIT. |
| **Mesa RADV** | https://gitlab.freedesktop.org/mesa/mesa | The Vulkan driver the plugin runs on (base f5cb8ee0 + `patches/mesa`); MIT. |
| **FFmpeg** | https://ffmpeg.org | The HEVC/AV1 decode libraries `src/phxdec` links (n7.1 + `patches/ffmpeg`); LGPL-2.1-or-later. |
| **linux-firmware** (AMD microcode) | https://gitlab.com/kernel-firmware/linux-firmware | The GPU firmware the bring-up loads; downloaded by `tools/firmware/fetch_firmware.sh`, never shipped. |
| **Acidanthera** | https://github.com/acidanthera | MacKernelSDK (build-time headers) and OpenCore, which boots the machine. Lilu is not used. |
| The AMD-OSX / Hackintosh community | | Years of shared knowledge about running macOS on PC hardware. |
