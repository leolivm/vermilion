#!/bin/sh
# SPDX-License-Identifier: MIT
# translator.sh: the shader translator libnvmtl_translate.dylib (src/translator, LGPL-3.0-or-later: AIR -> SPIR-V)
# -> $VERMILION_OUT/libnvmtl_translate.dylib, x86_64, release, ad-hoc signed. Installed next to NVMTLDriver in the
# plugin bundle (docs/INSTALL.md).
#   CARGO            cargo to use (default: cargo on the PATH; rustup's x86_64-apple-darwin target must be installed)
#   TRANSLATOR_TARGET  Rust target (default x86_64-apple-darwin, the reference machine)
#   TRANSLATOR_TEST=1  also run the wrapper's tests (slow: builds SPIRV-Tools; a few are known to fail on newer validators)
# Cargo fetches the crates pinned in wrapper/Cargo.lock (--locked); CARGO_NET_OFFLINE=true uses the local cache only.
# The cargo target dir is $VERMILION_OUT/translator-target. Paths of the checkout, $HOME, CARGO_HOME and the output dir
# are remapped out of the binary (--remap-path-prefix) and checked afterwards.
. "$(dirname "$0")/lib.sh"
CARGO=${CARGO:-cargo}
TGT=${TRANSLATOR_TARGET:-x86_64-apple-darwin}
CH=${CARGO_HOME:-$HOME/.cargo}
OUT=$(cd "$OUT" && pwd)
TD="$OUT/translator-target"
# rustc applies the last matching prefix: the widest first
REMAP="--remap-path-prefix=$HOME=/build --remap-path-prefix=$CH=/cargo --remap-path-prefix=$ROOT=/vermilion --remap-path-prefix=$OUT=/out"
command -v "$CARGO" >/dev/null 2>&1 || { echo "cargo not found (install Rust 1.87+ with rustup, target $TGT)" >&2; exit 1; }
cd "$ROOT/src/translator/wrapper"
if [ "${TRANSLATOR_TEST:-0}" = 1 ]; then
  RUSTFLAGS="$REMAP" CARGO_TARGET_DIR="$TD" "$CARGO" test --release --locked --target "$TGT" || echo "wrapper tests: failures above (compare with the known set)" >&2
fi
RUSTFLAGS="$REMAP" CARGO_TARGET_DIR="$TD" "$CARGO" build --release --locked --target "$TGT"
cp "$TD/$TGT/release/libnvmtl_translate.dylib" "$OUT/libnvmtl_translate.dylib"
# the linker records the cargo output path as the install name: the plugin dlopen()s the library by its full path
install_name_tool -id libnvmtl_translate.dylib "$OUT/libnvmtl_translate.dylib"
sign "$OUT/libnvmtl_translate.dylib"
homecheck "$OUT/libnvmtl_translate.dylib" "$CH" "$OUT"
echo "built $OUT/libnvmtl_translate.dylib"
