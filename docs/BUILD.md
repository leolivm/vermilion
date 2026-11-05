# Building Vermilion

All builds are x86_64 and run on macOS with the Command Line Tools (`xcrun clang`). The scripts in
[`../scripts/build`](../scripts/build) take every outside path from the environment and write to `build/`
(`VERMILION_OUT` to change it); none of them knows a user name, host or home directory. Every compile line carries
`-ffile-prefix-map` / `-fdebug-prefix-map` for the checkout and `$HOME`, and each script fails if a binary still names
either (`homecheck` in `scripts/build/lib.sh`).

| Script | Builds | Needs |
|---|---|---|
| `kext.sh` | `VermilionBoot.kext` | `MKSDK` (https://github.com/acidanthera/MacKernelSDK), the firmware of [FIRMWARE.md](FIRMWARE.md) (`PHX_FW`, default `~/gpu/fw/navi31`) for the generated `phx_c3_image.inc` |
| `accel.sh` | `VermilionAccel.kext` (ad-hoc signed) | `MKSDK`; optional link gate with export lists from `linkgate_exports.sh` (needs `ipsw` and the target's KernelCollections) |
| `plugin.sh` | `NVMTLDriver` (Metal plugin, ad-hoc signed) | `VULKAN_HEADERS` (Vulkan-Headers include dir) |
| `translator.sh` | `libnvmtl_translate.dylib` (shader translator, x86_64, ad-hoc signed) | Rust 1.87+ (`cargo`) with the `x86_64-apple-darwin` target; network for the first crate fetch (or `CARGO_NET_OFFLINE=true` with a filled cache); `TRANSLATOR_TEST=1` runs the wrapper tests ([`../src/translator/README.md`](../src/translator/README.md)) |
| `radv.sh apply` / `radv.sh` | `libvulkan_radeon.dylib` | `MESA_DIR` = Mesa checkout at f5cb8ee0; meson, ninja, glslangValidator, python3 with mako, pyyaml, markupsafe, packaging |
| `phxdec.sh` | `libphxdec_hevc.dylib` | `FFMPEG_PREFIX` = FFmpeg n7.1 + `patches/ffmpeg` built as shared `libavcodec_phx` / `libavutil_phx` (LGPL, no GPL parts); `VULKAN_HEADERS` |
| `tools.sh` | `phxctl`, `mesboot`, `vcn_write_test`, `submit_test` | - |
| `metaltest.sh` | `metaltest` (the boot daemon's Metal-device probe, `tools/metaltest/metaltest.m`) | - |

Plugins, dylibs and kexts must be ad-hoc signed (`codesign -fs -`): WindowServer falls back to the firmware
framebuffer with an unsigned plugin. The scripts do it.

## Tests (no hardware)

```sh
bash tests/mock/run.sh            # boot daemon (tools/phxctl/phxboot-navi31.sh)
bash tests/mock/b15test.sh        # display power recipe and watchdog (tools/phxctl/b15_boot.sh)
bash tests/mock/m7test.sh         # second monitor at boot (m7_boot.sh, m7_phase.sh)
bash tests/mock/dettest.sh        # det.sh
bash tests/mock/phxctl_serve_test.sh
```
The mutation runners (`*_mut.sh`, `hotplug0_mut.py`) check that the suites catch broken variants; see
[`../tests/mock/README.md`](../tests/mock/README.md). `p0prefetch_test.sh` needs a register dump of the target
(`P0PF_DUMP`) and is skipped without one.
