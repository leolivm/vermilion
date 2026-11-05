# Third-party notices

Components that Vermilion's source tree is built from or carries, the license each comes under, and where its source
is. Full license texts are in [`LICENSES/`](LICENSES). Adapted from nullarc's `THIRD_PARTY_NOTICES.md`.

## NullMoth macOS driver — `src/phxaccel`, `src/nmplugin`

PolyForm Noncommercial License 1.0.0 — [`LICENSES/PolyForm-Noncommercial-1.0.0.txt`](LICENSES/PolyForm-Noncommercial-1.0.0.txt)

```
Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
```

Source: https://github.com/nullmoth/nvidia-macos-driver (`kexts/`, `plugin/`). `src/phxaccel` is NullMoth's kernel
accelerator (NVRM accelerator, AGDC, framebuffer patterns) as adapted to AMD by nullarc and then by Vermilion;
`src/nmplugin` is NullMoth's Metal plugin with changes for this card. The headers in `src/phxaccel/accel/iofam`,
`accel/re` and `AppleGraphicsDeviceControl_re.h` are NullMoth's interoperability descriptions of Apple class layouts;
they contain no Apple code. Noncommercial use only. Vermilion is not a NullMoth release: NullMoth's official sources
are only its own repositories, the 1401 app and nullmothsystems.com.

## Shader translator (NullMoth / metal2vulkan) — `src/translator`

GNU Lesser General Public License v3.0 or later ([`LICENSES/LGPL-3.0-or-later.txt`](LICENSES/LGPL-3.0-or-later.txt),
with the GNU GPL v3.0 it refers to in [`LICENSES/GPL-3.0-only.txt`](LICENSES/GPL-3.0-only.txt); the same texts ship as
`src/translator/LICENSE` and `src/translator/COPYING`).

- `src/translator/translator`: metal2vulkan (Anees Iqbal, https://github.com/steelbrain/metal2vulkan), as shipped in
  NullMoth's https://github.com/nullmoth/nvidia-macos-driver at tag `v2.6.5` (`translator/translator`), unmodified.
- `src/translator/wrapper`: NullMoth's `nvmtl_translate` wrapper (`translator/wrapper` of the same tag) with
  Vermilion's changes, under the same license.
- The built `libnvmtl_translate.dylib` is LGPL: its complete source is this directory, and
  `scripts/build/translator.sh` rebuilds it, so a user can change and replace the library the plugin loads.
- Its Rust dependencies, pinned in `src/translator/wrapper/Cargo.lock`, are downloaded by cargo at build time and are
  not in this repository. Linked into the library: `spirv`, `rspirv` (Apache-2.0); `serde`, `serde_core`, `serde_json`,
  `itoa`, `libc`, `bitflags`, `rustc-hash`, `wait-timeout` (MIT or Apache-2.0); `memchr` (Unlicense or MIT); `zmij`
  (MIT). Build-time only: `serde_derive`, `syn`, `quote`, `proc-macro2` (MIT or Apache-2.0), `unicode-ident`
  ((MIT or Apache-2.0) and Unicode-3.0); the test-only `spirv-tools` crates when `TRANSLATOR_TEST=1`. A binary
  release of the library must carry these crates' license notices.

## nullarc — `src/phxbringup` (skeleton), `src/phxaccel` (AMD adaptation), `tools/phxctl` (phxctl, mesboot)

MIT License, Copyright (c) 2026 luuumi-dev and the nullarc contributors ([`LICENSES/MIT-nullarc.txt`](LICENSES/MIT-nullarc.txt)),
or at your option GPL-2.0-only ([`LICENSES/GPL-2.0-only.txt`](LICENSES/GPL-2.0-only.txt)), for the code written for
nullarc; its NullMoth-derived parts stay PolyForm Noncommercial (above). Source: https://github.com/luuumi-dev/nullarc
(base commit cbb08dd). The nullarc name and mark are © luuumi-dev and are not used as this project's name.

## Linux amdgpu (Advanced Micro Devices, Inc.) — register definitions, firmware message numbers, ported algorithms

MIT/X11 license ([`LICENSES/MIT-AMD.txt`](LICENSES/MIT-AMD.txt)), Copyright Advanced Micro Devices, Inc. (2012-2023,
per file). Source: the Linux kernel, `drivers/gpu/drm/amd/` (v6.8), https://git.kernel.org/.

- Taken as files: `tools/phxctl/mes_v11_api_def.h`, `tools/phxctl/csb_gfx11.h` (notice at the top of each).
- Ported or transcribed: `src/phxbringup/phx_regs_dcn32.h` (from `dcn_3_2_0_offset.h` / `_sh_mask.h`), `PhxGamma.h`
  (`dcn30_cm_common.c`, `custom_float.c`, `color_gamma.c`), `PhxVbios.h` (`bios_parser2.c`, `atomfirmware.h`),
  `PhxDdc.h` (`dce_aux.c`, `dce_i2c_hw.c`), `PhxHpd.h` (`hw_hpd.c`, `link_hpd.c`, `irq_service_dcn32.c`),
  `PhxPipe0.h` / `PhxPipe2.h` and the `phx_*_plan_*.inc` register plans, `phx_psp_smu.cpp` (`psp_gfx_if.h`,
  `psp_v13_0.c`, `amdgpu_psp.c`, `smu_v13_0_0_ppsmc.h`), `phx_vm.cpp` (`gmc_v11_0.c`, `mmhub_v3_0.c`),
  `tools/phxctl/vcn_m1_gen.py` and `vcn_up.sh` (`vcn_v4_0.c`, `amdgpu_vcn.c`).
- Every Linux file cited is MIT; no GPL-only file (drm core, i2c core) is used.
- The `phx_*_plan_*.inc` values are register state captured from Linux amdgpu running on this card.

## Cobalt64 (Navi48-MacOS) — RADV Darwin backend, user-client ABI, PSP ring method

- Repository: MIT, Copyright (c) 2026 Navi48-MacOS contributors ([`LICENSES/MIT-Cobalt64.txt`](LICENSES/MIT-Cobalt64.txt)).
- `src/navi48-bringup`: BSD-3-Clause, Copyright (c) 2026 the Navi48 project
  ([`LICENSES/BSD-3-Clause-navi48-bringup.txt`](LICENSES/BSD-3-Clause-navi48-bringup.txt)).
- Source: https://github.com/Almosst-DEV/Cobalt64. `patches/mesa/0001-0009` are Cobalt64's Darwin backend for RADV;
  `src/phxbringup/PhxNativeABI.h` follows `Navi48NativeABI.h`; `phx_psp_smu.cpp` follows `psp_v14_0.cpp`.

## RX4Darwin (AMDRadeonRDNA4.kext) — AUX, DMUB and HPD methods

BSD-3-Clause ([`LICENSES/BSD-3-Clause-RX4Darwin.txt`](LICENSES/BSD-3-Clause-RX4Darwin.txt)), Copyright (c) 2026
Sunneva N. Mariu. Source: https://github.com/RX4Darwin/AMDRadeonRDNA4.kext. `PhxDdc.h` (AUX defer handling),
`PhxDmub.h` (DMUB inbox submit, MM_INDEX/MM_DATA access) and `PhxHpd.h` (HPD debounce, EDID retry timing) follow it.

## RedNavi — AUX, pipe and VBIOS methods

BSD-3-Clause ([`LICENSES/BSD-3-Clause-RedNavi.txt`](LICENSES/BSD-3-Clause-RedNavi.txt)), Copyright (c) 2026, RedNavi
contributors. Source: https://github.com/Helemen7/RedNavi. `PhxDdc.h`, `PhxPipe0.h` and `PhxVbios.h` follow its
method; no RedNavi code is copied.

## Mesa (RADV) — `patches/mesa`

MIT, Copyright The Mesa 3D Graphics Library contributors (https://docs.mesa3d.org/license.html). Base: Mesa
`f5cb8ee032adabef599ae892ec4e42d796b84da9` (https://gitlab.freedesktop.org/mesa/mesa). Patches 0001-0009 Cobalt64,
0010-0015 nullarc, 0016-0017 Vermilion.

## FFmpeg — `patches/ffmpeg`, the libraries `src/phxdec` links

LGPL-2.1-or-later ([`LICENSES/LGPL-2.1-or-later.txt`](LICENSES/LGPL-2.1-or-later.txt)). Base: FFmpeg n7.1
(https://ffmpeg.org, b08d7969c550a804a59511c7b83f2dd8cc0499b8), built as shared `libavcodec_phx` / `libavutil_phx`
without GPL or nonfree parts; `patches/ffmpeg` is the complete change.

## AMD GPU microcode — not shipped

linux-firmware `amdgpu/` (https://gitlab.com/kernel-firmware/linux-firmware), [`LICENSES/LICENSE.amdgpu.txt`](LICENSES/LICENSE.amdgpu.txt):
Copyright (C) Advanced Micro Devices, Inc.; redistribution in binary form only; no reverse engineering, decompilation
or disassembly. Vermilion ships no firmware file and no excerpt of one. As nullarc does (its `.gitignore` keeps
`*.bin` out and `tools/fetch_test_firmware.sh` downloads the blobs), `tools/firmware/fetch_firmware.sh` downloads them and
checks them against `tools/firmware/fw.lock`; `tools/firmware/c3_image.py` builds `src/phxbringup/phx_c3_image.inc` locally.

## Build-time only

- MacKernelSDK (acidanthera, https://github.com/acidanthera/MacKernelSDK): Apple kernel headers, APSL 2.0. Not
  redistributed.
- Vulkan-Headers (Khronos, Apache-2.0 / MIT): headers for the plugin and phxdec. Not redistributed.

## Not included

- Apple software of any kind (kexts, kernel collections, Metal toolchain binaries).
- OpenCore and its kexts (Lilu, VirtualSMC, ...): get them from their projects.
- VBIOS or ACPI VFCT images: AMD/OEM copyright with no redistribution grant.
