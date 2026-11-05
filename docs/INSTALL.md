# Installing Vermilion

There is no installer and none is planned. Every piece is installed by hand, the way other macOS kexts and GPU bundles
are: the boot kext through OpenCore, the accelerator kext through the Auxiliary Kernel Collection, the Metal plugin and
its libraries into `/Library/GPUBundles`, and a LaunchDaemon that powers the GPU up at every boot.

Names: this guide uses the current names (`VermilionBoot.kext`, `VermilionAccel.kext`, bundle ids `org.vermilion.boot`
and `org.vermilion.accel`). IOKit classes (`PhxBringup`, `PhxAMDFB`, `NMXAccel`, `PhxAGDC`), sysctls (`debug.phx*`),
the `phx-build` ioreg property, NVRAM variables and the on-disk paths below kept their old names.
[NAMING.md](NAMING.md) maps the old names to the new ones. The reference machine runs the kexts under these names. If
you have an installation with the old names (`PhxBringup.kext` in OpenCore,
`/Library/Extensions/PhxAccel.kext`), remove it first ([section 9](#9-rollback-and-uninstall), with the old names), so
two copies of the same IOKit classes never load.

Read the whole guide once before you start. Keep a way back: a copy of your EFI folder, and a second bootable macOS or
a USB installer.

## 1. Prerequisites

### Hardware and macOS

- **macOS Tahoe 26.7.1**, installed and booting with OpenCore. Other builds are untested: the plugin uses private
  Metal interfaces of this build, and a macOS update can break it (see [Known limits](#10-known-limits)).
- **An AMD Radeon RX 7900 XT, PCI `1002:744c`, revision `0xcc`.** `VermilionBoot.kext` matches `0x744c1002` only. Register
  plans, display timings and watermarks were measured on one reference card and its two monitors (a DisplayPort AOC
  24G2W1G4 and an HDMI Acer KG241Q S). Other boards, other Navi 31 revisions, the RX 7900 XTX and other RDNA 3 GPUs are
  untested and will most likely not work as is.
- BIOS: Above 4G decoding and Resizable BAR on, CSM off, plus the usual OpenCore settings for your board
  (unverified: whether the kexts need the full BAR has not been tested).
- No other driver may bind the GPU: leave WhateverGreen out or disabled (it is disabled on the reference machine).

### OpenCore

| Setting | Value | Why |
|---|---|---|
| `Misc > Security > SecureBootModel` | `Disabled` | an Apple secure boot model refuses the injected and unsigned kexts |
| `NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82 > csr-active-config` | `03 0A 00 00` (data, = `0x0A03`) | SIP relaxed: `CSR_ALLOW_UNTRUSTED_KEXTS` (0x1), `CSR_ALLOW_UNRESTRICTED_FS` (0x2), `CSR_ALLOW_UNAPPROVED_KEXTS` (0x200), `CSR_ALLOW_UNAUTHENTICATED_ROOT` (0x800). This is the reference machine's value. The last bit is needed only for the optional video decoder (section 5.5), which writes to the system volume |
| `NVRAM > Add > 7C436110-... > boot-args` | at least `phxdebug=1 amfi=0x80 ipc_control_port_options=0` | see below |
| `NVRAM > Delete > 7C436110-...` | contains `boot-args` and `csr-active-config` | so a changed value takes effect |
| `Kernel > Add` | Lilu, AMFIPass, `VermilionBoot.kext` (section 3) | |

Boot-args the stack needs:

| Boot-arg | Read by | Without it |
|---|---|---|
| `phxdebug=1` | `VermilionBoot` (`PE_parse_boot_argn("phxdebug")`) | the debug selectors stay closed, and the whole bring-up (`phxctl`, `mesboot`, `vcn_up.sh`) runs through them: no GPU |
| `amfi=0x80` | AMFI, with AMFIPass | WindowServer does not load the ad-hoc signed Metal plugin, and the accelerator kext cannot be approved |
| `ipc_control_port_options=0` | the kernel | part of the AMFIPass setup the accelerator kext was approved with |

The reference machine also has `nvaccel=1`. The current `VermilionAccel` build does not read it (the check sits in a
code path that is compiled only without `NMX_ACCEL`, and `src/vermilion-accel/build.sh` defines `NMX_ACCEL`); it is harmless to
keep. Everything else in the reference machine's boot-args (`debug=0x100 keepsyms=1`, audio, Bluetooth, CPU name and
power) is debugging or board-specific and not needed by Vermilion.

Boot-args you can add later to switch parts off without uninstalling (all read from the source):

| Boot-arg | Effect |
|---|---|
| `-phxacceloff` (or `-nvoff`) | `VermilionAccel` does not start: no accelerator, no `PhxAMDFB` |
| `-phxfboff` | `PhxAMDFB` leaves the firmware framebuffer alone |
| `-phxbringupoff` | `VermilionBoot` does not start (no bring-up, no user client) and its `PhxFBSquatter` does not take the framebuffer: the firmware console keeps the display |
| `-phxhotplugoff`, `-phxdpwatchoff` | the hot-plug and DP stream watcher flags of the boot daemon are refused |
| `-phxedidstatic` | the built-in EDIDs ([EDID.md](EDID.md)) are used instead of the live read |

### What `amfi=0x80` costs

`amfi=0x80` relaxes Apple Mobile File Integrity for the whole system: library validation is off for every process,
not only for WindowServer. Side effects seen on the reference machine:

- **FairPlay / DRM-protected playback does not work** (for example Apple TV+ or Netflix in Safari).
- **Privacy prompts of third-party apps are denied without a prompt.** tccd logs
  `CS_PLATFORM_BINARY set but not AppleSigned; prompt policy is Deny` and the app never shows up in System Settings >
  Privacy & Security > Microphone or Camera. The Screen & System Audio Recording pane still has a **+** button, so apps
  can be added there by hand; Microphone and Camera have no such button.

### Kexts that make AMFI relaxation work

- **Lilu** (acidanthera) and **AMFIPass** (a Lilu plugin, distributed with OpenCore Legacy Patcher) in `EFI/OC/Kexts`
  and `Kernel > Add`, Lilu before AMFIPass. The reference machine runs AMFIPass 1.4.1.

### Identity (SMBIOS)

Generate your own SMBIOS with GenSMBIOS or macserial and put it in `PlatformInfo > Generic`. Never reuse someone
else's serial, MLB, ROM or UUID. The reference machine uses the `MacPro7,1` model.

## 2. Build and fetch the firmware

Build every component as described in [BUILD.md](BUILD.md). The outputs land in `build/` (or `$VERMILION_OUT`):

| Output | Script | Installed in |
|---|---|---|
| `VermilionBoot.kext` | `scripts/build/kext.sh` | `EFI/OC/Kexts` (section 3) |
| `VermilionAccel.kext` | `scripts/build/accel.sh` | `/Library/Extensions` (section 4) |
| `NVMTLDriver` (Metal plugin executable) | `scripts/build/plugin.sh` | `/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS` (section 5) |
| `libnvmtl_translate.dylib` (shader translator, LGPL-3.0-or-later, [src/translator](../src/translator/README.md)) | `scripts/build/translator.sh` | next to `NVMTLDriver` (section 5) |
| `libvulkan_radeon.dylib` (RADV) | `scripts/build/radv.sh apply`, then `scripts/build/radv.sh` | `/Library/GPUBundles/nvmtl` (section 5) |
| `libphxdec_hevc.dylib`, `libavcodec_phx.61.dylib`, `libavutil_phx.59.dylib` (optional video decode) | `scripts/build/phxdec.sh` | next to `NVMTLDriver` (section 5.5) |
| `phxctl`, `mesboot` | `scripts/build/tools.sh` | `/usr/local/sbin` and `~/gpu` (section 6) |
| `metaltest` (Metal-device probe) | `scripts/build/metaltest.sh` | `~/gpu` (section 6) |

The bring-up loads AMD microcode that this repository does not ship ([FIRMWARE.md](FIRMWARE.md)). Download it as the
desktop user (it goes to `~/gpu/fw/navi31`, where the boot scripts read it, and every file is checked against
`tools/firmware/fw.lock`):

```sh
tools/firmware/fetch_firmware.sh
```

`kext.sh` needs this firmware too (it generates one header from `dcn_3_2_0_dmcub.bin`), so fetch it before building.

## 3. Inject VermilionBoot.kext with OpenCore

`VermilionBoot` is loaded by OpenCore, so it needs no approval in macOS.

1. Mount the EFI partition OpenCore boots from (for example `sudo diskutil mount disk0s1`; check with
   `diskutil list` which one it is).
2. Copy the kext (replace `/Volumes/EFI` with the mount point):

   ```sh
   sudo cp -R build/VermilionBoot.kext /Volumes/EFI/EFI/OC/Kexts/
   ```

3. Add a `Kernel > Add` entry in `EFI/OC/config.plist`, after Lilu and the VirtualSMC kexts (on the reference
   machine it sits after `SMCSuperIO.kext`; AMFIPass comes later in the list, which works because it only depends on
   Lilu):

   | Key | Value |
   |---|---|
   | `Arch` | `x86_64` |
   | `BundlePath` | `VermilionBoot.kext` |
   | `Comment` | `Vermilion boot kext` |
   | `Enabled` | `true` |
   | `ExecutablePath` | `Contents/MacOS/VermilionBoot` |
   | `MaxKernel` | (empty) |
   | `MinKernel` | `25.0.0` |
   | `PlistPath` | `Contents/Info.plist` |

4. Reboot. Check that the kext is loaded and which build it is:

   ```sh
   ioreg -l | grep phx-build
   ```

   One line with a number is the build of the loaded kext. No line means OpenCore did not inject it (check the
   entry, `SecureBootModel` and the OpenCore log).

To update the kext later, copy the old one out of the EFI first (for example to `~/vermilion-rollback/`), copy the new
one as `EFI/OC/Kexts/VermilionBoot.kext.new`, remove the old folder and rename the new one to `VermilionBoot.kext`. Make
one EFI change per boot.

## 4. Install VermilionAccel.kext (Auxiliary Kernel Collection)

`VermilionAccel` holds the `PhxAMDFB` framebuffer, the `NMXAccel` accelerator and `PhxAGDC`. It lives in
`/Library/Extensions` and is linked into the Auxiliary Kernel Collection (AuxKC), which needs a one-time approval.

Before you start:

- `accel.sh` ad-hoc signs the kext. An unsigned kext is refused.
- The kext's `Info.plist` must be valid XML. In particular, never put `--` inside an XML comment: IOKit then rejects
  the kext with a misleading "unsupported architectures" error.
- AMFIPass and the boot-args of section 1 must already be active (check `nvram boot-args`).

Steps:

1. Copy the kext and give it to root:

   ```sh
   sudo cp -R build/VermilionAccel.kext /Library/Extensions/
   sudo chown -R root:wheel /Library/Extensions/VermilionAccel.kext
   sudo chmod -R go-w /Library/Extensions/VermilionAccel.kext
   ```

2. Ask the kernel to load it. The first time, this stages the kext and macOS asks for approval:

   ```sh
   sudo kmutil load -p /Library/Extensions/VermilionAccel.kext
   ```

3. Open System Settings > Privacy & Security, scroll to the message about blocked system software and click **Allow**.
4. Reboot (macOS rebuilds the AuxKC in `/Library/KernelCollections` on the Data volume, not the sealed
   `/System/Library/KernelCollections`).
5. Check:

   ```sh
   kmutil showloaded | grep org.vermilion.accel
   ```

   The AuxKC does not always load the kext at boot; the boot daemon loads it with `kmutil load -p` when it is missing
   (section 7), so an empty result before the daemon ran is not an error by itself.

To update the kext, keep the old one first (`sudo cp -R /Library/Extensions/VermilionAccel.kext ~/vermilion-rollback/`),
replace it, run `kmutil load -p` again and reboot. A changed kext can bring the Allow prompt back.

## 5. Install the Metal plugin, the translator and RADV

### 5.1 What goes where

| File | Destination | Notes |
|---|---|---|
| `NVMTLDriver` | `/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver` | the Metal driver plugin |
| `libnvmtl_translate.dylib` | `/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib` | the plugin opens the copy next to itself first, else `/Library/GPUBundles/nvmtl/libnvmtl_translate.dylib` |
| `libvulkan_radeon.dylib` | `/Library/GPUBundles/nvmtl/libvulkan_radeon.dylib` | opened directly by the plugin |
| `nvrm610.conf` (optional) | `/Library/GPUBundles/nvmtl/nvrm610.conf` | runtime knobs, `KEY=VALUE`, the first occurrence of a key wins |
| `nvmtl-allow.txt` | `/Library/GPUBundles/nvmtl-allow.txt` | which processes get the Metal device; the boot daemon rewrites it at every boot |
| `aircache/`, `spvcache/` | `/Library/GPUBundles/nvmtl/` | shared shader caches |

Every file must be owned by `root:wheel`, not writable by group or others, and readable by everyone (WindowServer runs
as `_windowserver` and must read the bundle, the libraries and the allow list). Every Mach-O must be ad-hoc signed
(`codesign -fs -`; the build scripts do it). **An unsigned plugin or dylib makes WindowServer fail to create its Metal
device, and the boot falls back to the firmware framebuffer.**

### 5.2 Runtime pieces Vermilion does not ship

- **The `NVMTLDriver.bundle` skeleton** (its `Info.plist` and resources). Vermilion builds only the executable that
  goes into `Contents/MacOS`. On the reference machine the bundle came from NullMoth's driver package (the bundle that
  nullarc's install script stages is NullMoth 2.4.1), and Vermilion's executable replaced the one inside it. You need
  an existing `NVMTLDriver.bundle` in `/Library/GPUBundles` before step 5.3 (unverified: a minimal hand-written
  `Info.plist` has not been tested).
- **`spirv-val`** (SPIRV-Tools). The translator validates every SPIR-V module it produces by running `spirv-val`
  (`src/translator/translator/src/tools.rs`). It looks in `METAL2VULKAN_SPIRV_VAL` (an absolute path), then
  `/opt/homebrew/opt/llvm/bin`, `/usr/local/opt/llvm/bin`, `/opt/homebrew/bin`, `/usr/local/bin`, then `PATH`. From
  the code, a missing `spirv-val` makes the translation return an error (not tested on a machine without it).
  Homebrew's `spirv-tools` installs it into `/usr/local/bin` on an Intel Mac. Where the reference machine's copy comes
  from is not recorded (unverified).
- **`/Library/GPUBundles/nvmtl/air-opt`**, an LLVM tool from Apple's Metal toolchain. The plugin spawns it only when
  it cannot read a shader's AIR in memory; it is an Apple binary and is not redistributed. Copy it from your own
  Xcode / Metal toolchain if you need it (unverified: how often the fallback is hit, and where the reference
  machine's copy came from).
- **The Vulkan loader** `/Library/GPUBundles/nvmtl/libvulkan.dylib` with `radeon_icd.json`. The plugin uses it only when
  it cannot open `libvulkan_radeon.dylib` directly, so it is optional.

### 5.3 Install the plugin executable

Keep the previous binary, copy the new one next to it, then move it over in one step (a process that maps the old
file keeps its copy):

```sh
sudo mkdir -p ~/vermilion-rollback
sudo cp -p /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver ~/vermilion-rollback/NVMTLDriver.prev
sudo cp build/NVMTLDriver /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver.new
sudo chown root:wheel /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver.new
sudo mv /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver.new /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
```

### 5.4 Install the translator and RADV

```sh
sudo cp build/libnvmtl_translate.dylib /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/.tr.new
sudo chown root:wheel /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/.tr.new
sudo mv /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/.tr.new /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib
sudo mkdir -p /Library/GPUBundles/nvmtl
sudo cp build/libvulkan_radeon.dylib /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib.new
sudo chown root:wheel /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib.new
sudo mv /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib.new /Library/GPUBundles/nvmtl/libvulkan_radeon.dylib
```

(Keep the previous copies in `~/vermilion-rollback` first, as in 5.3.)

Create the shared shader caches, owned by WindowServer's user and writable by nobody else (WindowServer loads what is
in them as compiled shaders), and a first allow list that lets every process use Metal:

```sh
sudo mkdir -p /Library/GPUBundles/nvmtl/aircache /Library/GPUBundles/nvmtl/spvcache
sudo chown _windowserver:wheel /Library/GPUBundles/nvmtl/aircache /Library/GPUBundles/nvmtl/spvcache
sudo chmod 0755 /Library/GPUBundles/nvmtl/aircache /Library/GPUBundles/nvmtl/spvcache
printf '*\n' | sudo tee /Library/GPUBundles/nvmtl-allow.txt >/dev/null
sudo chmod 644 /Library/GPUBundles/nvmtl-allow.txt
```

**When a change takes effect:** new processes load a new plugin, translator or RADV at once; WindowServer keeps the
copy it loaded until the next boot. The SPIR-V cache key carries the translator build, so old cache entries are not
reused after a translator change and nothing has to be wiped. Never delete a process's shader caches as a side effect
of an install: every first use then recompiles and clients can time out.

### 5.5 Optional: hardware video decode

1. Copy `libphxdec_hevc.dylib`, `libavcodec_phx.61.dylib` and `libavutil_phx.59.dylib` into
   `/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/`, with the same copy / chown / move pattern.
2. Build the VideoToolbox bundle with `src/video-decoder/vtbundle/build.sh`. It has to go into `/System/Library/Video/Plug-Ins`:
   `/Library/Video/Plug-Ins` is scanned in a mode that drops ad-hoc signed bundles.
3. That directory is on the sealed system volume. `tools/phxctl/sysvol_install.sh` installs into it the way OpenCore
   Legacy Patcher's root patches do: it mounts the live system volume read-write at `/System/Volumes/Update/mnt1`,
   copies the bundle (root:wheel, `go-w`), and blesses a new boot snapshot. It refuses to run unless
   `csrutil authenticated-root status` says disabled (the `0x800` bit of section 1). The change is live after the next
   reboot.

   ```sh
   sudo sh tools/phxctl/sysvol_install.sh src/video-decoder/build/PhxVideoDecoder.bundle /System/Library/Video/Plug-Ins
   ```

   After this the system snapshot is no longer sealed: macOS offers only full (not delta) updates, and a macOS update
   replaces the system volume and removes the bundle. `sudo sh tools/phxctl/sysvol_install.sh --remove /System/Library/Video/Plug-Ins/PhxVideoDecoder.bundle`
   removes it (again live after a reboot).
4. Decode also needs the VCN engine up: the `phxboot-vcn` flag of section 7. `touch /Library/GPUBundles/nvmtl/phxdec-off`
   turns the decoder off without uninstalling it.

### 5.6 Optional: relocating the bundles for sandboxed GPU processes

`tools/phxctl/relocate_gpubundle.sh` (from nullarc) moves `NVMTLDriver.bundle` and `nvmtl/` from `/Library/GPUBundles`
to `/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU` and leaves symlinks behind. The reason:
sandboxed GPU processes (WebKit's `com.apple.WebKit.GPU`, Chromium and Electron helpers) may map executables only from
system paths, and `/Library/GPUBundles` is readable but not executable for them. That `CoreTypes.bundle` directory is a
firmlink into the writable Data volume, so the sealed system volume is not touched and no `csr` bit is needed. The
originals are kept in `/Library/GPUBundles-backup`; the script is idempotent and checks the plugin's signature at the
end. Run it as root after the plugin is installed. The reference machine's documented install paths are the
`/Library/GPUBundles` ones; whether it runs relocated, and whether Navi 31 needs this, is not recorded (unverified).

## 6. The tools directory `~/gpu`

The boot daemon and its scripts live in `~/gpu` of the desktop user (the account that logs in at the console). The
LaunchDaemon passes that user's short name as `PHX_USER`; the daemon reads the user's home with `dscl` and exports it
as `HOME` for every script it runs, so `~` in the scripts is that user's home. Create the layout as that user:

```sh
mkdir -p ~/gpu/phxctl ~/gpu/vcnup/vcn
cp tools/phxctl/phxboot-navi31.sh ~/gpu/
cp tools/phxctl/navi31_up.sh tools/phxctl/bringup_navi31.sh ~/gpu/phxctl/
cp tools/phxctl/gfx_cg.sh tools/phxctl/sdma_idle.sh tools/phxctl/uclk.sh tools/phxctl/smumetrics.py tools/phxctl/ptwalk.sh ~/gpu/
cp tools/phxctl/m7_boot.sh tools/phxctl/m7_phase.sh tools/phxctl/dispovr.sh ~/gpu/
cp tools/phxctl/b15_boot.sh tools/phxctl/wm144.sh tools/phxctl/det.sh tools/phxctl/dalwm.sh tools/phxctl/dal.sh tools/phxctl/hubp_dlg.sh tools/phxctl/p0prefetch.sh ~/gpu/
python3 tools/phxctl/vcn_m1_gen.py
cp tools/phxctl/vcn_up.sh ~/gpu/vcnup/
cp tools/phxctl/vcn/vcn0_area.bin tools/phxctl/vcn/vcn0_ram.bin tools/phxctl/vcn/vcn0_layout.sh ~/gpu/vcnup/vcn/
chmod +x ~/gpu/vcnup/vcn_up.sh
cp build/phxctl ~/gpu/phxctl.bin
cp build/metaltest ~/gpu/metaltest
sudo mkdir -p /usr/local/sbin
sudo cp build/phxctl /usr/local/sbin/phxctl
sudo cp build/mesboot /usr/local/sbin/mesboot
sudo chown root:wheel /usr/local/sbin/phxctl /usr/local/sbin/mesboot
```

`vcn_m1_gen.py` writes the VCN data files into `tools/phxctl/vcn/`; `vcn_up.sh` checks their md5s before it runs.

| Path | Used by |
|---|---|
| `~/gpu/phxboot-navi31.sh` | the LaunchDaemon |
| `~/gpu/phxctl/navi31_up.sh`, `~/gpu/phxctl/bringup_navi31.sh` | the bring-up (`navi31_up.sh` changes into `~/gpu/phxctl` and runs `bringup_navi31.sh` from there) |
| `~/gpu/fw/navi31/` | `bringup_navi31.sh`, `vcn_up.sh` (section 2) |
| `/usr/local/sbin/phxctl` | the daemon, `bringup_navi31.sh`, `ptwalk.sh` (a root-owned copy) |
| `/usr/local/sbin/mesboot` | `navi31_up.sh` (GART, MES, KIQ, queues), `vcn_up.sh` |
| `~/gpu/phxctl.bin` | `gfx_cg.sh`, `sdma_idle.sh`, `uclk.sh`, `b15_boot.sh` and its helpers, `m7_boot.sh`, `vcn_up.sh` |
| `~/gpu/gfx_cg.sh`, `sdma_idle.sh`, `uclk.sh` (+ `smumetrics.py` for `uclk.sh show`), `ptwalk.sh` | the daemon's clock gating, SDMA idle, memory clock pin and hang dump |
| `~/gpu/m7_boot.sh`, `m7_phase.sh` | the second monitor at boot (`m7_boot.sh` finds `phxctl.bin` and `m7_phase.sh` next to itself) |
| `~/gpu/b15_boot.sh`, `wm144.sh`, `det.sh`, `dalwm.sh`, `dal.sh`, `hubp_dlg.sh`, `uclk.sh`, `p0prefetch.sh` | the display power recipe; `b15_boot.sh` refuses to run when one of the first six is missing |
| `~/gpu/dispovr.sh` | the display overrides before the WindowServer restart |
| `~/gpu/vcnup/vcn_up.sh` (executable) + `vcnup/vcn/` | the VCN bring-up |
| `~/gpu/metaltest` | the Metal-device probe (below) |
| `~/gpu/cgmode`, `~/gpu/cdpref` | optional helpers, not in this repository; when missing, `b15_boot.sh` and the gamma step log it and go on |

The daemon runs as root and starts these scripts from a directory the desktop user can write. Anything running as
that user can therefore change what root runs at the next boot. Keep that in mind on a machine other people use
(unverified: making `~/gpu` root-owned has not been tested with the scripts that write state files there, such as
`~/gpu/hubp_dlg.saved` and `~/gpu/phxboot-rate-last`).

### `metaltest`

The daemon decides whether the Metal device is live by running `~/gpu/metaltest`, built from
[`tools/metaltest/metaltest.m`](../tools/metaltest/metaltest.m) by `scripts/build/metaltest.sh` (the same source the
reference machine runs). It prints `Metal devices: <n>` and the device names, then compiles and runs a small compute
kernel on the GPU and prints `COMPUTE RESULT: 4096/4096 correct` when every value is right (exit code 0; the source
lists the other exit codes). How the daemon uses it:

- It is run as the desktop user (`sudo -u <user> ~/gpu/metaltest`), up to 40 times, one second apart, after `NMXAccel`
  is up and `PhxAMDFB` holds the panel.
- Its stdout and stderr are merged; the daemon looks for the text `Metal devices: 1`. Any run whose output contains it
  counts as "the Metal device is ready". No such line after 40 tries fails the desktop attempt (fallback to the firmware
  framebuffer).
- It must exit on its own each time (the daemon waits for each run).
- Its process name must be allowed by `/Library/GPUBundles/nvmtl-allow.txt` (a `*` line or a `metaltest` line), or the
  plugin gives it no device.

The daemon only looks for the `Metal devices: 1` line; the compute check is for you when you run it by hand
(section 8.1).

## 7. The boot daemon

### 7.1 Install the LaunchDaemon

`~/gpu/phxboot-navi31.sh` was copied in section 6. Render the LaunchDaemon from the template, as the desktop user,
replacing `@PHX_ROOT@` with the absolute path of `~/gpu` and `@PHX_USER@` with your short name:

```sh
sed -e "s|@PHX_ROOT@|$HOME/gpu|g" -e "s|@PHX_USER@|$(id -un)|g" tools/phxctl/local.phxboot.plist.in | sudo tee /Library/LaunchDaemons/local.phxboot.plist >/dev/null
sudo chown root:wheel /Library/LaunchDaemons/local.phxboot.plist
sudo chmod 644 /Library/LaunchDaemons/local.phxboot.plist
plutil -lint /Library/LaunchDaemons/local.phxboot.plist
```

The job runs `/bin/bash <PHX_ROOT>/phxboot-navi31.sh` once per boot (`RunAtLoad`, `LaunchOnlyOnce`) and logs to
`/var/log/phxboot.log`.

launchd loads every plist in `/Library/LaunchDaemons` at boot, so the normal next step is a reboot. You can also load
it now with:

```sh
sudo launchctl bootstrap system /Library/LaunchDaemons/local.phxboot.plist
```

but that starts the daemon at once (`RunAtLoad`): it powers the GPU up in the running session. Do that only on a boot
where the daemon has not run yet (a first install), never a second time in the same boot: the bring-up loads the SMU
firmware, and loading it again into a running SMU is something the Linux driver never does.

### 7.2 What it does at each boot

1. Waits up to 60 s to see whether this is a **squat boot**: when the one-shot NVRAM variable `phxfb-squat` is set,
   `VermilionBoot`'s `PhxFBSquatter` takes the IOFramebuffer category, so the firmware console never starts and the
   screen stays dark until the daemon arms the display. Otherwise the firmware framebuffer (GOP) drives the screen.
2. Stops if the kill switch `~/gpu/phxboot-off` exists (a squat boot first reboots onto the firmware framebuffer).
3. Stops if the in-progress mark `/var/db/phxboot.pending` exists, i.e. the previous desktop attempt never finished:
   it creates the kill switch, clears the one-shots and stays on the firmware framebuffer (rebooting first on a squat
   boot).
4. A **desktop-arm boot** is a squat boot or one with `~/gpu/phxboot-desktop`. On it, the daemon sets `phxfb-boot`
   (read once by `PhxAMDFB`).
5. Waits for `phx-build` in ioreg, loads `VermilionAccel.kext` with `kmutil load -p` if the AuxKC did not, and runs the
   bring-up (`~/gpu/phxctl/navi31_up.sh`: PSP, SMU, RLC, GART, MES, KIQ, compute and gfx queues; it arms a 3-minute
   reboot in case the PSP hangs and cancels it at the end). Then SDMA idle, clock gating and the memory clock pin, each
   behind its flag, and `mesboot claim`.
6. Without a desktop arm: writes an allow list that gives every app Metal (WindowServer stays on the firmware
   framebuffer), logs `no desktop arm: GOP + Metal for apps` and `done`.
7. With a desktop arm: touches the in-progress mark, waits for `PhxAMDFB` on the panel and for `metaltest`, refuses if
   a user is logged in, arms the display pipe and `PhxAGDC`, lights the second monitor (flag), restarts WindowServer
   **once** so it enumerates the Metal device, then watches 120 s for WindowServer crashes. Three crashes (or no
   WindowServer) = revert: the next boot is on the firmware framebuffer. Success logs `SUCCESS`, removes the mark,
   re-arms `phxfb-squat` for the next boot when `~/gpu/phxboot-auto` exists, and logs `done`.
8. After `done`: VCN, the late clock gating / memory clock calls, hot-plug, gamma, the DP stream watcher and the
   display power recipe, each behind its flag, then a watch that reverts to the firmware framebuffer after two
   WindowServer GPU hangs within five minutes.

**Nobody may be logged in at the console or over an interactive SSH session before the WindowServer restart**: the
daemon checks `who` before arming and again right before the restart, and a logged-in user fails the attempt. Turn
automatic login off.

### 7.3 Flag files

All flags are files in `~/gpu`. "Present" means the file exists (`touch ~/gpu/<flag>`); some take a value
(`echo 2 > ~/gpu/<flag>`). Removing a flag turns its step off at the next boot. Defaults below are "absent" unless
stated.

| Flag | Read when | Effect |
|---|---|---|
| `phxboot-off` | start | **kill switch**: the daemon does nothing (a squat boot reboots onto the firmware framebuffer). Set by the daemon after an unfinished attempt |
| `phxboot-desktop` | start | one desktop-arm attempt on a non-squat boot; removed by the daemon (one-shot) |
| `phxboot-auto` | success / GOP boot | re-arms `phxfb-squat` for the next boot after a `SUCCESS`, and on a GOP boot unless `phxboot-failed` exists: the accelerated desktop at every boot |
| `phxboot-failed` | GOP boot | written by the daemon at every revert to the firmware framebuffer; while it exists a GOP boot does not re-arm `phxfb-squat` (no reboot loop after a real failure). A `SUCCESS` removes it; remove it by hand after fixing the cause |
| `phxboot-b21guard` | start | the login-window guard (7.4). Absent or anything but `0` = on (default); `0` = the daemon never touches `SHOWFULLNAME` |
| `phxboot-sdma` | after the bring-up | starts both SDMA engines idle, so the RLC can grant clock gating (without it the card idles at ~150 W) |
| `phxboot-cg` | after the bring-up and after `done` | GFX clock gating as Linux enables it (without it the idle card runs ~250 W at a 110 C hotspot with the fans at full) |
| `phxboot-uclk` (value: MHz, e.g. `456`) | after the bring-up and after `done` | memory clock hard minimum; without it a memory clock switch under GPU load can underflow the display |
| `phxboot-m7-aocmodes` or `phxboot-m7-aoc144` | before the claim | head 0 lists the DisplayPort monitor's 60 and 144 Hz modes |
| `phxboot-m7` | desktop arm | the second monitor at boot (`m7_boot.sh light` before the arm, `bind` after it); any failure = one monitor this boot |
| `phxboot-aoc-rate` (value: `144`, `60` or `last`) | desktop arm, before the restart | puts the DisplayPort monitor at 144 Hz before the WindowServer restart. Needs the head-0 mode list flag and `phxboot-b15` (plus `phxboot-b15-2pipe` with two monitors, `phxboot-m7` with one); `last` = the rate remembered in `phxboot-rate-last` |
| `phxboot-dispovr` | right before the restart | `dispovr.sh`: a CoreDisplay override per connected display so a new display set defaults to its highest refresh rate. It writes only files it created (key `phx-override`) under `/Library/Displays/Contents/Resources/Overrides` |
| `phxboot-noflip` | after the restart | keeps the copy into the scanned surface instead of the DCN A/B flip (ignored when GPU present is on) |
| `/Library/GPUBundles/nvmtl/ws-gpu-present` | before and after the restart | GPU present: the plugin flips the display itself (`debug.phxscan=1`) and the display pipe's copy is skipped |
| `phxboot-vcn` | after `done` | VCN 0 up (`vcnup/vcn_up.sh --run`, log `~/gpu/vcnup/boot.log`), then `debug.phxvcn=1` and `debug.phxmmvm=1`; needed for hardware video decode. Skipped on a boot whose second monitor came up out of phase |
| `phxboot-hotplug` | after `done`, two monitors bound | `debug.phxhotplug=2`: the HDMI monitor's unplug / plug takes head 1 offline / online |
| `phxboot-hotplug0` (value `1` observe or `2` act; empty = `1`) | after `done`, desktop boot | `debug.phxhotplug0`: the same for the DisplayPort monitor on head 0 |
| `phxboot-gamma` | after `done`, two monitors bound | `debug.phxgamma=1`: gamma tables for head 1 (only reach the kext when CoreDisplay's `useIOP_gamma_onAMD` is false) |
| `phxboot-dpwatch` (value `1` or `2`; empty = `1`) | after `done`, desktop boot | `debug.phxdpwatch`: the DP stream watcher (2 = also the stuck-stream fix); its events go to the boot log |
| `phxboot-b15` | after `done`, desktop boot | `b15_boot.sh`: the display power recipe (watermarks, DET, DAL watermark table, HUBP rate group, then memory self-refresh) with an underflow watchdog for the whole boot |
| `phxboot-wsprefs` | before the restart | no longer used; the daemon only logs that it can be removed |

Flags read by the helper scripts, not by the daemon itself:

| Flag | Read by | Effect |
|---|---|---|
| `phxboot-b15-dpm` | `b15_boot.sh` | memory and fabric clocks may drop at idle (UCLK unpinned, FCLK switch allowed) |
| `phxboot-b15-2pipe`, `-2pipe-sr`, `-2pipe-dalwm`, `-2pipe-p0pf` | `b15_boot.sh` | the two-monitor variant of the recipe and its optional steps |
| `phxboot-m7-rate` (`144`, `165` or `last`) | `m7_boot.sh` | the HDMI monitor's rate at boot (absent = 60 Hz) |
| `phxboot-m7-modes` (`1`) | `m7_boot.sh` | head 1 lists several refresh rates in System Settings |
| `phxboot-m7-head1` (`plan` or `1080`) | `m7_boot.sh` | head 1's size (absent = 1680x1050) |
| `phxboot-m7-direct165` | `m7_boot.sh` | the old direct 165 Hz light |
| `phxboot-c3-off` | `m7_boot.sh` | turns off the cold-boot PHY SRAM load (on by default) |
| `phxboot-rate-last` | `m7_boot.sh`, written by `b15_boot.sh` | state file: the last stable rates |

The reference machine runs with `auto`, `sdma`, `cg`, `uclk` (`456`), `m7`, `m7-aocmodes`, `m7-aoc144`, `m7-modes`,
`m7-head1`, `m7-rate` (`165`), `aoc-rate` (`144`), `dispovr`, `b15`, `b15-dpm`, `b15-2pipe`, `b15-2pipe-sr`,
`b15-2pipe-p0pf`, `hotplug`, `hotplug0` (`2`), `dpwatch` (`2`) and `vcn`. Most of these are tuned to its two monitors.
A reasonable first set on one monitor is `sdma`, `cg` and `uclk` (`456`); add `auto` once a desktop boot has worked
(section 8).

The LaunchDaemon can also pass `PHXBOOT_NO_SDMA=1`, `PHXBOOT_NO_CG=1`, `PHXBOOT_NO_UCLK=1`, `PHXBOOT_NO_AOC=1`,
`PHXBOOT_NO_VCN=1`, `PHXBOOT_NO_B15=1` or `PHXBOOT_NO_WATCH=1` in its `EnvironmentVariables` to skip a step regardless
of the flags.

### 7.4 The login-window guard

The first WindowServer of every boot runs without Metal until the daemon's one restart, and on a firmware-framebuffer
boot it is the only one. If the login window draws the Memoji lock-screen avatar in that phase, WindowServer aborts
(QuartzCore `CA::OGL::Context::push_surface`), and on a fallback boot the login loops. Showing the name and password
fields instead (`SHOWFULLNAME = true` in `/Library/Preferences/com.apple.loginwindow`) avoids the avatar.

So the daemon manages that key (unless `~/gpu/phxboot-b21guard` holds `0`):

- It sets it to true at its start, at every shutdown (launchd's TERM), and at every revert to the firmware
  framebuffer, because the login window starts about a second before the daemon and reads the value left from the
  previous session.
- It puts the user's own value back right before the Metal WindowServer restart, so the accelerated login shows the
  avatar again.
- The user's value waits in `~/gpu/phxboot-b21guard.orig` between those two points (`absent`, `0` or `1`). A user's own
  `true` is never removed.
- Each action is one `b21guard:` line in `/var/log/phxboot.log`.

With the guard on, do not set `SHOWFULLNAME` by hand during a session: the shutdown hook records the current value as
yours.

## 8. First boot and checks

### 8.1 First boot: no desktop arm

Leave `phxboot-auto` and `phxboot-desktop` out and `phxfb-squat` unset for the first boot. The daemon then powers the
GPU up and gives apps Metal, while WindowServer stays on the firmware framebuffer.

1. Reboot.
2. Check that the boot kext is loaded and which build:

   ```sh
   ioreg -l | grep phx-build
   ```

3. Follow the log until it prints `done` (about 2.5 minutes after boot on the reference machine):

   ```sh
   tail -f /var/log/phxboot.log
   ```

   Each boot starts with a `== <date> <time> phxboot(navi31, ...)` header; look at the last one. This boot should show
   `== bring-up + claim ==`, `navi31 up: compute + gfx queues ready`, `NMXAccel up`, `no desktop arm: GOP + Metal for apps`
   and `done <time>`.

4. Check the kernel side and Metal:

   ```sh
   sysctl debug.phxvm
   ~/gpu/metaltest
   ```

   `metaltest` must print `Metal devices: 1`. `sysctl debug | grep phx` lists every Vermilion sysctl.

### 8.2 Turn on the accelerated desktop

```sh
touch ~/gpu/phxboot-auto
sudo nvram phxfb-squat=1
```

Reboot, and **do not log in until the login window has come back once**: on this squat boot the screen stays dark
until the daemon arms the display, then WindowServer restarts once. After the
login, `tail /var/log/phxboot.log` should show `Metal device ready`, `AGDC up`, `restarting WindowServer once`,
`watching 120s for crashes`, `SUCCESS at <time>`, `phxfb-squat re-armed for the next boot` and `done`.
`sysctl debug.nvaccel_iop_ok` grows while the screen changes (frames drawn by the GPU).

### 8.3 Never reboot before `done`

From the moment the daemon touches the in-progress mark until `SUCCESS`, a boot that ends without the daemon's own
shutdown handling counts as a failed attempt: the next boot finds `/var/db/phxboot.pending`, logs
`previous attempt did not finish -> GOP`, turns the kill switch on and stays on the firmware framebuffer. A clean
restart or shutdown (launchd sends TERM) is handled: the daemon logs `clean shutdown (TERM)`, clears the mark and
re-arms the next boot. A hang, a power cut, a forced power-off or a job killed before its TERM handler ran is not.
After `SUCCESS` the late steps still send SMU and display commands until `done`. Waiting for `done` before any reboot
avoids all of these; on the reference machine a warm reboot in a fallback state once hung after OpenCore and needed a
power cycle.

### 8.4 What a fallback to the firmware framebuffer looks like

- The login window shows name and password fields instead of the avatar (the guard of 7.4).
- The desktop works but is composited on the CPU; apps may still get Metal.
- `/var/log/phxboot.log` says why: `kill switch present`, `previous attempt did not finish -> GOP`, or
  `FAIL (<reason>): reverting to GOP` (the reason names the step: no `phx-build`, `NMXAccel did not start`,
  `PhxAMDFB never took the panel`, `Metal device never came up`, `a user is logged in`, `AGDC never armed`,
  `WindowServer crashed/hung N times`). A GPU hang also leaves a dump in `/var/log/phxhang-<time>/`.

### 8.5 Recover and re-arm

1. Fix the cause the log names.
2. Clear the kill switch and the failure marker, and arm the next boot:

   ```sh
   rm -f ~/gpu/phxboot-off ~/gpu/phxboot-failed
   sudo nvram phxfb-squat=1
   ```

3. Reboot once and wait for `done`.

Do not leave `/Library/GPUBundles/nvmtl-allow.txt` restricted before a desktop boot: after a failure the daemon writes
a "test tools only" list until the next boot rewrites it, and with such a list `metaltest` gets no device and the
attempt fails again.

To turn the GPU path off by hand: `touch ~/gpu/phxboot-off` and reboot. If macOS does not boot at all, disable the
`VermilionBoot.kext` entry in OpenCore (or boot your EFI copy), or add `-phxacceloff` / `-phxbringupoff` to the
boot-args for one boot.

## 9. Rollback and uninstall

Roll back one component by putting its previous binary back with the same copy / chown / move pattern, then reboot and
wait for `done`. Keep every previous binary (for example in `~/vermilion-rollback`) until the new one has survived a
few boots.

To remove Vermilion completely, in this order:

1. **Boot daemon** (stops the bring-up at the next boot):

   ```sh
   sudo launchctl bootout system/local.phxboot
   sudo rm -f /Library/LaunchDaemons/local.phxboot.plist /var/db/phxboot.pending
   sudo nvram -d phxfb-squat
   sudo nvram -d phxfb-boot
   ```

2. **Login-window value**: if `~/gpu/phxboot-b21guard.orig` exists, the guard left `SHOWFULLNAME` true; put your value
   back. When the file holds `absent`:

   ```sh
   sudo defaults delete /Library/Preferences/com.apple.loginwindow SHOWFULLNAME
   ```

   When it holds `0`:

   ```sh
   sudo defaults write /Library/Preferences/com.apple.loginwindow SHOWFULLNAME -bool false
   ```

   When it holds `1`, leave the key as it is. Then remove the file.

3. **Display overrides** written by `dispovr.sh` (only the files carrying its `phx-override` key):

   ```sh
   for f in /Library/Displays/Contents/Resources/Overrides/DisplayVendorID-*/DisplayProductID-*; do plutil -extract phx-override raw -o - "$f" >/dev/null 2>&1 && sudo rm -f "$f"; done
   ```

4. **Plugin, translator, RADV**: put back the files you replaced, or remove the bundle and the Vulkan side if nothing
   else uses them (with `relocate_gpubundle.sh` also remove `/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU`
   and restore `/Library/GPUBundles-backup`):

   ```sh
   sudo rm -rf /Library/GPUBundles/NVMTLDriver.bundle /Library/GPUBundles/nvmtl
   sudo rm -f /Library/GPUBundles/nvmtl-allow.txt
   ```

   The video decoder bundle, if installed: `sudo sh tools/phxctl/sysvol_install.sh --remove /System/Library/Video/Plug-Ins/PhxVideoDecoder.bundle`.

5. **VermilionAccel.kext**: remove it and rebuild the AuxKC without it:

   ```sh
   sudo rm -rf /Library/Extensions/VermilionAccel.kext
   sudo kmutil install --update-all
   sudo kmutil clear-staging
   ```

   (unverified: the exact `kmutil` invocation that drops a removed kext from the AuxKC on 26.7.1 has not been run on the
   reference machine. After the next boot, `kmutil showloaded | grep org.vermilion.accel` must print nothing; the
   boot-arg `-phxacceloff` keeps the kext from starting meanwhile.)

6. **VermilionBoot.kext**: set its `Kernel > Add` entry to `Enabled = false` (or delete the entry) and remove the
   folder from `EFI/OC/Kexts`.
7. **Tools and boot-args**: remove `/usr/local/sbin/phxctl`, `/usr/local/sbin/mesboot` and `~/gpu`; remove
   `phxdebug=1`, `nvaccel=1` and, if nothing else needs them, `amfi=0x80 ipc_control_port_options=0` and AMFIPass from
   OpenCore. Reboot.

## 10. Known limits

- **Sleep / wake does not work.** Keep the machine from sleeping; the reference machine runs with:

  ```sh
  sudo pmset -a sleep 0 hibernatemode 0 disksleep 0
  sudo pmset -a powernap 0 tcpkeepalive 0 womp 0
  ```

  Do not use Apple menu > Sleep or a short press of the power button (what they do on this stack has not been tested).
  Display sleep leaves the display links powered.
- **No HDMI / DisplayPort audio** from the GPU.
- **Two monitors at most**: no third or fourth monitor.
- **Board-specific values**: the watermarks, DET sizes, display timings, refresh plans and fallback EDIDs were
  measured on the reference card and its two monitors.
- **The first login after a translator change is slow.** The SPIR-V cache is keyed by the translator build, so every
  WindowServer shader is translated again once: about 1.5 minutes of heavy WindowServer CPU after the login on the
  reference machine. The next logins use the cache.
- **A firmware-framebuffer boot shows the name and password login** instead of the avatar (the guard of 7.4).
- **macOS updates**: do not install them casually. A full update replaces the system volume (removing the video
  decoder bundle), the AuxKC has to be rebuilt for the new kernel (the Allow prompt may come back), and the plugin
  depends on private interfaces of 26.7.1: if they change, the boot falls back to the firmware framebuffer.
- FileVault on top of this stack has not been tested; keep it off.
