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
| **Not yet** | Sleep / wake · HDMI / DisplayPort audio from the GPU · a third or fourth monitor (see [Roadmap](#roadmap)) |

Verified on the reference machine (November 2026) with the kexts under their Vermilion names: `VermilionBoot.kext`
injected by OpenCore and `VermilionAccel.kext` loaded from the Auxiliary Kernel Collection give the accelerated
desktop on two monitors at 144 Hz and 165 Hz. The kexts on that machine were built from this tree.

## What is in here

| Component | What it does | Source |
|---|---|---|
| **Boot kext** — `VermilionBoot.kext` | Injected by OpenCore. Starts the GPU (loads AMD's firmware, power management, graphics engines, GPU memory), drives the monitor outputs (DisplayPort and HDMI, monitor detection, refresh rates, hot-plug) and gives the Vulkan driver its access to the card | [`src/vermilion-boot`](src/vermilion-boot) |
| **Accelerator kext** — `VermilionAccel.kext` | Installed in `/Library/Extensions` (Auxiliary Kernel Collection). The part macOS sees as the graphics card: one framebuffer per monitor, the accelerator WindowServer and apps draw through, and display control | [`src/vermilion-accel`](src/vermilion-accel) |
| **Metal plugin** — `NVMTLDriver` | The Metal driver WindowServer and apps load. It runs Metal on top of Vulkan (RADV) | [`src/metal-plugin`](src/metal-plugin) |
| **Shader translator** — `libnvmtl_translate.dylib` | Converts Metal shaders to Vulkan (SPIR-V) for the plugin (LGPL-3.0-or-later) | [`src/translator`](src/translator) |
| **Vulkan driver** — RADV (`libvulkan_radeon.dylib`) | Mesa's AMD Vulkan driver with a macOS backend, built from Mesa with this patch series | [`patches/mesa`](patches/mesa) |
| **Video decoder** (optional) — `libphxdec_hevc.dylib`, `PhxVideoDecoder.bundle` | Hardware HEVC / AV1 decoding for VideoToolbox, through FFmpeg's Vulkan video path | [`src/video-decoder`](src/video-decoder), [`patches/ffmpeg`](patches/ffmpeg) |
| **Video acceleration bundle** — `PhxVA.bundle` | The bundle macOS's video acceleration (AppleGVA) looks for on this card. A placeholder for now: it declines every request, so VideoToolbox falls back to its software decoders | [`src/video-accel`](src/video-accel) |
| **Boot daemon and tools** — `phxboot-navi31.sh`, `phxctl`, `mesboot` | A LaunchDaemon that runs at every boot: powers the GPU up and moves WindowServer onto it, with optional steps (second monitor, refresh rate, display power settings) switched on by flag files. If a step fails, the next boot falls back to the firmware framebuffer. Plus the command-line tools and scripts it runs | [`tools/phxctl`](tools/phxctl) |
| **Metal check** — `metaltest` | The small program the boot daemon runs to confirm that apps get a working Metal device (it also runs a compute kernel on the GPU) | [`tools/metaltest`](tools/metaltest) |
| **Firmware fetcher** | Downloads the AMD microcode from linux-firmware at a pinned commit, checks every file, and generates the one header the boot kext is built with | [`tools/firmware`](tools/firmware) |
| Build scripts | One script per component, no machine-specific paths | [`scripts/build`](scripts/build) |
| Tests | Hardware-free tests of the boot daemon and the scripts it runs | [`tests/mock`](tests/mock) |
| Documentation | [INSTALL](docs/INSTALL.md), [BUILD](docs/BUILD.md), [FIRMWARE](docs/FIRMWARE.md), [EDID](docs/EDID.md), [NAMING](docs/NAMING.md) (why classes, sysctls and tools keep their `phx` names) | [`docs/`](docs) |

## Building

See [docs/BUILD.md](docs/BUILD.md). In short, on an Intel or Apple Silicon Mac with the Command Line Tools:

```sh
tools/firmware/fetch_firmware.sh                        # AMD microcode from linux-firmware into ~/gpu/fw/navi31
MKSDK=/path/to/MacKernelSDK scripts/build/kext.sh       # VermilionBoot.kext
MKSDK=/path/to/MacKernelSDK scripts/build/accel.sh      # VermilionAccel.kext
scripts/build/translator.sh                             # libnvmtl_translate.dylib (needs Rust)
scripts/build/tools.sh && scripts/build/metaltest.sh    # phxctl, mesboot, metaltest
bash tests/mock/run.sh && bash tests/mock/b15test.sh    # boot daemon and display-power mocks
```

## Installing

There is no installer and none is planned: the kexts are installed the way other macOS kexts are. The guide is
[docs/INSTALL.md](docs/INSTALL.md).

## License

**Source-available, with noncommercial parts.** Code written for Vermilion is MIT. `VermilionAccel` (`src/vermilion-accel`) and the
Metal plugin (`src/metal-plugin`) are derived from NullMoth's driver and are under the **PolyForm Noncommercial License
1.0.0**: no commercial use. Files from nullarc keep nullarc's MIT-or-GPL-2.0 terms, material from Linux amdgpu keeps
AMD's MIT notice, the shader translator (`src/translator`) is LGPL-3.0-or-later, and the Mesa and FFmpeg patches
follow their projects. Details: [LICENSE](LICENSE),
[NOTICE](NOTICE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [LICENSES/](LICENSES).

No AMD firmware, VBIOS or Apple software is distributed with this repository.

## Credits

Vermilion is built on NullMoth's macOS driver and its shader translator (metal2vulkan), nullarc, Cobalt64, RX4Darwin, RedNavi, Linux amdgpu, Mesa, FFmpeg and
Acidanthera's tools: see [CREDITS.md](CREDITS.md) for what came from each.

## Roadmap

- **Sleep and wake.** Today the machine has to be kept awake ([INSTALL.md](docs/INSTALL.md#10-known-limits)).
- **HDMI and DisplayPort audio** from the GPU.
- **A third and fourth monitor** as macOS displays.
