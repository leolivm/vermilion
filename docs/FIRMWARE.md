# AMD firmware

The bring-up (`tools/phxctl/bringup_navi31.sh`, `vcn_up.sh`) loads AMD microcode from linux-firmware `amdgpu/`
through the PSP, and VermilionBoot's cold-boot PHY fix (`debug.phxc3`, `src/phxbringup/PhxC3Load.h`) embeds one slice of
`dcn_3_2_0_dmcub.bin`. These files are AMD's, under `LICENSE.amdgpu` ([`../LICENSES/LICENSE.amdgpu.txt`](../LICENSES/LICENSE.amdgpu.txt)):
redistribution in binary form only, no reverse engineering, decompilation or disassembly.

**This repository ships no firmware and no excerpt of it**, following nullarc (its `.gitignore` keeps `*.bin` out of
the tree and `tools/fetch_test_firmware.sh` downloads the blobs from linux-firmware). Instead:

1. `tools/firmware/fetch_firmware.sh [dest]` downloads the 20 files in [`../tools/firmware/fw.lock`](../tools/firmware/fw.lock) from
   linux-firmware (default dest `~/gpu/fw/navi31`, where the box scripts read them) and checks each md5. A file whose
   md5 differs is rejected: offsets and sizes in the bring-up were measured on exactly those revisions. Pick the
   linux-firmware ref with `LINUX_FIRMWARE_REF=<commit>` (default `main`); a mirror with `LINUX_FIRMWARE_URL`.
2. `tools/firmware/c3_image.py [--fw <dcn_3_2_0_dmcub.bin>]` generates `src/phxbringup/phx_c3_image.inc` from your copy
   (it checks the slice's sha256 first). `scripts/build/kext.sh` runs it when the header is missing. The generated
   header is a local build input: `.gitignore` keeps it out.

TODO: record the linux-firmware commit the `fw.lock` revisions come from, so the default ref can be pinned.
