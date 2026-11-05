<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/vermilion-logo-dark.svg">
    <img alt="Vermilion" src="assets/brand/vermilion-logo-light.svg" width="480">
  </picture>
</p>

# Vermilion

**A GPU-accelerated macOS desktop on the AMD Radeon RX 7900 XT (Navi 31, RDNA 3).**
Vermilion is a display and acceleration driver for macOS Tahoe 26.7.1: two kernel extensions, a Metal driver plugin
running on Mesa RADV, and the boot-time bring-up that powers the GPU up before WindowServer opens the displays.

> [!IMPORTANT]
> Vermilion is specific to **one board**: an RX 7900 XT (PCI `1002:744c`, revision `0xcc`) on the reference machine,
> with the firmware revisions listed in `tools/firmware/fw.lock`. Register plans, watermarks and display timings were measured
> on that card and its two monitors. Other Navi 31 cards, other RDNA 3 GPUs and other macOS versions are untested and
> will most likely not work as is.

> [!WARNING]
> Vermilion is independent and unofficial. It is **not affiliated with, reviewed, endorsed or supported by** NullMoth
> Systems, luuumi-dev (nullarc), AMD, Apple or any other project named in [CREDITS.md](CREDITS.md). Please do not ask
> them for support with it.

## Status

| | |
|---|---|
| **Works** | Metal acceleration (WindowServer composites on the GPU; apps get the Metal device) · 144 Hz and 165 Hz refresh · two monitors at once (DisplayPort + HDMI) · hot-plug · display rotation |
| **Does not work yet** | Sleep / wake · HDMI / DisplayPort audio from the GPU · a third or fourth monitor |

## What is in here

| Path | What |
|---|---|
| [`src/phxbringup`](src/phxbringup) | `VermilionBoot.kext`, injected by OpenCore: PSP/SMU/GFX bring-up, GPUVM, the user-client ABI RADV uses, the display pipes (DCN 3.2), DDC/AUX, hot-plug |
| [`src/phxaccel`](src/phxaccel) | `VermilionAccel.kext` (Auxiliary KC): the `PhxAMDFB` framebuffer, the `NMXAccel` IOAccelerator and `PhxAGDC` display control |
| [`src/nmplugin`](src/nmplugin) | The Metal driver plugin (Metal on Vulkan) |
| [`src/translator`](src/translator) | The shader translator the plugin loads (Metal AIR to SPIR-V, LGPL-3.0-or-later) |
| [`src/phxdec`](src/phxdec), [`src/phxva`](src/phxva) | Hardware video decode (VideoToolbox decoder over FFmpeg's Vulkan HEVC/AV1 path) |
| [`tools/phxctl`](tools/phxctl) | The boot daemon `phxboot-navi31.sh`, its LaunchDaemon template, and the box tools it runs (`phxctl`, bring-up, display power, second monitor, watchdog scripts) |
| [`tools/firmware`](tools/firmware) | Firmware download and check (`fetch_firmware.sh`, `fw.lock`) and the VermilionBoot firmware-header generator |
| [`patches/mesa`](patches/mesa), [`patches/ffmpeg`](patches/ffmpeg) | Patch series for Mesa RADV (f5cb8ee0) and FFmpeg (n7.1) |
| [`scripts/build`](scripts/build) | Build scripts (no machine-specific paths) |
| [`tests/mock`](tests/mock) | Hardware-free tests of the boot daemon and the box scripts |
| [`docs/`](docs) | [INSTALL](docs/INSTALL.md), [BUILD](docs/BUILD.md), [FIRMWARE](docs/FIRMWARE.md), [EDID](docs/EDID.md), [NAMING](docs/NAMING.md) |

## Building

See [docs/BUILD.md](docs/BUILD.md). In short, on an Intel or Apple Silicon Mac with the Command Line Tools:

```sh
tools/firmware/fetch_firmware.sh                              # AMD microcode from linux-firmware into ~/gpu/fw/navi31
MKSDK=/path/to/MacKernelSDK scripts/build/kext.sh       # VermilionBoot.kext
MKSDK=/path/to/MacKernelSDK scripts/build/accel.sh      # VermilionAccel.kext
scripts/build/translator.sh                             # libnvmtl_translate.dylib (needs Rust)
bash tests/mock/run.sh && bash tests/mock/b15test.sh    # boot daemon and display-power mocks
```

## Installing

There is no installer and none is planned: the kexts are installed the way other macOS kexts are. The guide is
[docs/INSTALL.md](docs/INSTALL.md).

## License

**Source-available, with noncommercial parts.** Code written for Vermilion is MIT. `VermilionAccel` (`src/phxaccel`) and the
Metal plugin (`src/nmplugin`) are derived from NullMoth's driver and are under the **PolyForm Noncommercial License
1.0.0**: no commercial use. Files from nullarc keep nullarc's MIT-or-GPL-2.0 terms, material from Linux amdgpu keeps
AMD's MIT notice, the shader translator (`src/translator`) is LGPL-3.0-or-later, and the Mesa and FFmpeg patches
follow their projects. Details: [LICENSE](LICENSE),
[NOTICE](NOTICE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [LICENSES/](LICENSES).

No AMD firmware, VBIOS or Apple software is distributed with this repository.

## Credits

Vermilion is built on NullMoth's macOS driver and its shader translator (metal2vulkan), nullarc, Cobalt64, RX4Darwin, RedNavi, Linux amdgpu, Mesa, FFmpeg and
Acidanthera's tools: see [CREDITS.md](CREDITS.md) for what came from each.

## Known TODOs

- The renamed kexts (`VermilionBoot.kext`, `VermilionAccel.kext`, see [docs/NAMING.md](docs/NAMING.md)) have been
  built and mock-tested but not yet loaded on hardware.
- `metaltest`, the Metal-device probe the boot daemon runs, is not in this repository yet ([docs/INSTALL.md](docs/INSTALL.md)).
- Pin the linux-firmware commit in `tools/firmware/fw.lock`.
