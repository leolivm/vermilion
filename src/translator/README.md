# Shader translator (`libnvmtl_translate.dylib`)

The Metal plugin (`src/metal-plugin`) loads this library at run time to translate Metal AIR (the LLVM IR inside a
`.metallib`) into Vulkan SPIR-V, which RADV then compiles. It is installed next to the plugin, as
`NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib` ([docs/INSTALL.md](../../docs/INSTALL.md)).

**License: LGPL-3.0-or-later** ([`LICENSE`](LICENSE) = the GNU LGPL 3.0, [`COPYING`](COPYING) = the GNU GPL 3.0 it
builds on; also in [`../../LICENSES/`](../../LICENSES)). This directory is a complete, buildable source of the library
Vermilion ships, as the LGPL asks: anyone may change it and rebuild the library with `scripts/build/translator.sh`.

| Directory | What | Source |
|---|---|---|
| `translator/` | `metal2vulkan`, the AIR -> SPIR-V translator core, unmodified | NullMoth's `nvidia-macos-driver`, tag `v2.6.5`, directory `translator/translator` (git tree `2f1f6b3913f3348d1091d05f4676fe71982eea3e` for the whole `translator/` directory). Upstream of that: https://github.com/steelbrain/metal2vulkan (Anees Iqbal) |
| `wrapper/` | `nvmtl_translate`, the C ABI the plugin calls (`cdylib`), with Vermilion's passes | NullMoth's wrapper (`translator/wrapper` of the same tag) with Vermilion's changes: `tg_struct_info.rs` (retry without the threadgroup struct description), the `OpCopyMemory` lowering and the stage-input rule in `buffer_addresses.rs`, the diagnostics switches in `lib.rs` |
| `LICENSE`, `COPYING` | the license texts shipped with the upstream `translator/` directory | unchanged |

This is the source of the translator build Vermilion was validated with (core v2.6.5 + this wrapper). The `NVMTL_*`
environment switches in the wrapper are diagnostics; none is needed in normal use.

## Building

```sh
scripts/build/translator.sh            # x86_64 release build -> build/libnvmtl_translate.dylib, ad-hoc signed
```

It needs a Rust toolchain (1.87 or newer; tested with 1.99.1) with the `x86_64-apple-darwin` target
(`rustup target add x86_64-apple-darwin` on Apple Silicon) and network access for the first `cargo` fetch of the
crates pinned in `wrapper/Cargo.lock` (or `CARGO_NET_OFFLINE=true` with a filled cargo cache).
`TRANSLATOR_TEST=1` also runs the wrapper's tests (they build SPIRV-Tools from source through the `spirv-tools` crate,
which takes a while; a few tests are known to fail against newer validator rules).

At run time the plugin hands the translator LLVM IR text, and the translator validates its output with `spirv-val`
from SPIRV-Tools (found on the search path, or `METAL2VULKAN_SPIRV_VAL`; see `translator/README.md`). Neither
`spirv-val` nor the plugin's `air-opt` tool is part of this repository ([docs/INSTALL.md](../../docs/INSTALL.md)).
