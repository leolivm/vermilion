# Names: what was renamed and what keeps the `phx` prefix

Vermilion grew out of nullarc, whose kexts, tools and sysctls carry the `Phx` / `phx` prefix (nullarc targets the
Radeon 780M, AMD's "Phoenix" APU), and out of NullMoth's driver (`NMX`, `NV`, `nullmoth-`). The names a user installs,
loads or approves now say Vermilion. Names inside the code and on the running system keep their old prefix, because
renaming them would change ABI or on-disk state that other parts depend on, without a test that covers it.

## Renamed

| Was | Now | Where |
|---|---|---|
| `PhxBringup.kext` (executable `PhxBringup`) | **`VermilionBoot.kext`** (executable `VermilionBoot`) | `src/phxbringup/Info.plist`, `build.sh`, `scripts/build/kext.sh`, the OpenCore `Kernel > Add` entry ([INSTALL.md](INSTALL.md)) |
| bundle id `org.drip.PhxBringup` | **`org.vermilion.boot`** | `Info.plist` (bundle and both IOKit personalities), `kmod_info.c` (`KMOD_EXPLICIT_DECL`) |
| `PhxAccel.kext` (executable `PhxAccel`) | **`VermilionAccel.kext`** (executable `VermilionAccel`) | `src/phxaccel/Info.plist`, `build.sh`, `scripts/build/accel.sh`; installed as `/Library/Extensions/VermilionAccel.kext` |
| bundle id `com.luuumi.PhxAccel` | **`org.vermilion.accel`** | `Info.plist` (bundle and both personalities), `nvrm-accel.cpp` (`KMOD_EXPLICIT_DECL`), the boot daemon's `kmutil showloaded` check and its default `ACCEL` path, the `kmutil` mock in `tests/mock/run.sh` |
| bundle id `org.drip.PhxVA` | `org.vermilion.va` | `src/phxva/Info.plist`. The bundle keeps its name `PhxVA.bundle`: the boot kext publishes `IODVDBundleName = PhxVA`, and AppleGVA loads the bundle by that name |

Checked: both kexts build from this tree with the new names, the `kmod_info` name in each binary equals its
`CFBundleIdentifier`, and the boot-daemon mocks pass with the renamed `kmutil` check. The new names have **not been
loaded on hardware yet**: until they are, treat the rename as untested on a running system. An existing installation
with the old names must be removed first ([INSTALL.md](INSTALL.md), "Uninstall"), so that two copies of the same
classes never load.

## Kept, and why

| Name | Examples | Why it stays |
|---|---|---|
| IOKit class names | `PhxBringup`, `PhxFBSquatter`, `IOAccelPhxNativeClient`, `PhxDisplay`, `PhxAMDFB`, `PhxAGDC` / `PhxDeviceControl`, `PhxVirtualKeyboard`, `NMXAccel`, the `NV*` accelerator classes | RADV opens the service by class name (`patches/mesa` 0012/0014), the boot daemon and the box tools find the services with `ioreg -c <class>`, the personalities match on them, and the accelerator classes are NullMoth's. A class rename touches the Mesa patches, the daemon and every tool at once, with no hardware test |
| IOKit personality keys | `PhxBringup`, `PhxFBSquatter`, `PhxAccel`, `PhxFB` | Labels only (the matching uses `IOClass`, `IOProviderClass` and the match keys); kept to keep the plists' diff small |
| The user-client ABI | the `N48N` connection type, selector numbers, `PhxNativeABI.h` | Shared with the Mesa patches (Cobalt64's layout) |
| sysctls | `debug.phx*` (about 200 nodes), `debug.nvaccel*`, `debug.nmx*` | The boot daemon, every script in `tools/phxctl`, the mocks and the docs use them; they are a debugging interface |
| registry properties | `phx-build`, `phx-edid*`, `phx-pipe0-modes`, ... | Read by the daemon and the tools (`ioreg -l \| grep phx-build` is the install check) |
| NVRAM variables | `phxfb-boot`, `phxfb-squat`, `phxfb-go`; NullMoth's `nullmoth-nv-starts`, `nullmoth-nv-ok`, `nullmoth-remove` | Persistent state across boots: a rename would make an upgraded system read stale or missing values |
| boot-args | `-phxhotplugoff`, `-phxfb0open`, `phxdebug`, ... | Live in the user's OpenCore `config.plist` |
| on-disk paths | `~/gpu`, `~/gpu/phxboot-*` flag files, `/var/log/phxboot.log`, `/Library/LaunchDaemons/local.phxboot.plist`, `/Library/GPUBundles/NVMTLDriver.bundle`, `/Library/GPUBundles/nvmtl/` | The daemon, the plugin and the tools hard-code them as defaults (each can be overridden by an environment variable in the daemon); a move needs a migration step |
| tools and files | `phxctl`, `phxboot-navi31.sh`, `mesboot`, `NVMTLDriver`, `libnvmtl_translate.dylib`, `PhxVideoDecoder` (bundle id `com.phx.PhxVideoDecoder`) | Names the daemon and the plugin look up |
| source directories | `src/phxbringup`, `src/phxaccel`, `src/nmplugin`, ... | Keep the history readable against nullarc and NullMoth |
| log lines and comments | "PhxBringup not loaded", "PhxAccel without debug.phxhotplug0", ... | In those messages `PhxBringup` means VermilionBoot.kext and `PhxAccel` means VermilionAccel.kext; the mocks match on the log text |

The `Phx` prefix does not refer to any product of this project; see [CREDITS.md](../CREDITS.md) (nullarc).
