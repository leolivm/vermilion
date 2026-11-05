# AMD firmware

The bring-up (`tools/phxctl/bringup_navi31.sh`, `vcn_up.sh`) loads AMD microcode from linux-firmware `amdgpu/`
through the PSP, and VermilionBoot's cold-boot PHY fix (`debug.phxc3`, `src/vermilion-boot/PhxC3Load.h`) embeds one slice of
`dcn_3_2_0_dmcub.bin`. These files are AMD's, under `LICENSE.amdgpu` ([`../LICENSES/LICENSE.amdgpu.txt`](../LICENSES/LICENSE.amdgpu.txt)):
redistribution in binary form only, no reverse engineering, decompilation or disassembly.

**This repository ships no firmware and no excerpt of it**, following nullarc (its `.gitignore` keeps `*.bin` out of
the tree and `tools/fetch_test_firmware.sh` downloads the blobs from linux-firmware). Instead:

1. `tools/firmware/fetch_firmware.sh [dest]` downloads the 20 files in [`../tools/firmware/fw.lock`](../tools/firmware/fw.lock) from
   linux-firmware (default dest `~/gpu/fw/navi31`, where the box scripts read them) and checks each md5. A file whose
   md5 differs is rejected: offsets and sizes in the bring-up were measured on exactly those revisions. The files
   come from the linux-firmware commit pinned in `fw.lock`, `add225168d0d1ba4c174d5eb15272b53de01ad5a` (release tag
   `20250917`, the first release with all 20 revisions; they are unchanged up to tag `20251111`, and later releases
   replace most of them: GC, PSP, SMU and VCN, then SDMA and DCN). It tries the GitLab mirror first, then git.kernel.org.
   `LINUX_FIRMWARE_REF=<commit>` picks another commit, `LINUX_FIRMWARE_URL` another mirror.
2. `tools/firmware/c3_image.py [--fw <dcn_3_2_0_dmcub.bin>]` generates `src/vermilion-boot/phx_c3_image.inc` from your copy
   (it checks the slice's sha256 first). `scripts/build/kext.sh` runs it when the header is missing. The generated
   header is a local build input: `.gitignore` keeps it out.
