#!/bin/bash
# Put the microcode Firebrand.kext carries, and its licence, into firmware/
# (git-ignored): every file kext/Firebrand/firmware.list names, plus
# LICENSE.amdgpu. The kext's Makefile embeds them in the personality.
#
#   scripts/fetch-firmware.sh
#
# Files already there are kept (and checked). Otherwise they come from the
# host's linux-firmware (/usr/lib/firmware/amdgpu, zstd- or xz-compressed or
# plain), and failing that from the linux-firmware repository on kernel.org.
# Each file is checked against the checksum below: these are the versions the
# E9173, RX 550, R7 450 and MI50 were verified with (linux-firmware 20260916),
# so a different file is a different experiment. The vega20_* files are
# firmware-vega20.list's (a BRINGUP=vega20 build embeds them).
#
# The microcode is AMD's, redistributable in binary form under
# LICENSE.amdgpu, which travels with it. It is never committed here.
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT
set -euo pipefail
command -v sha256sum >/dev/null 2>&1 || sha256sum() { shasum -a 256 "$@"; }

root=$(cd "$(dirname "$0")/.." && pwd)
dir=$root/firmware
upstream=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain

sums="b0fe44950422879f0509fb62a7f7aa0e75c7f4ddfe6257a8864b2049e72fae78  polaris12_mc.bin
555a01b4833c9cb62da126f80fb7c5ce15a7ef827ea6b8bc0f725c2a8f4b0ce9  polaris12_32_mc.bin
4b8eb326a7fbd391554fe50bc43cf04a917b01a806884ea324f5c3fe88a6ec77  polaris12_smc.bin
a4758bf9e8286d9bae39a59c1f579a0a68c398a37f4720c0c70392461d2d19fe  polaris12_ce_2.bin
8953b50da0d365e79b6bec1253e9ad56097451718de01ddaf8d4683b46e4a13c  polaris12_me_2.bin
7b80b023b18010fa66ef3af5a19fa556bcee4d72d5eaf5b6aacc263e7c7b3706  polaris12_pfp_2.bin
1e0ce977d29288e3f499b0ab0f5e8af43f5cabbef4b36e067a086a976f6e9636  polaris12_mec_2.bin
11dc9315750b09ef43b5c3d3d59f078b47d054764ad0ce985ec4a3a484f25d9a  polaris12_mec2_2.bin
f087f8585ec1d4bc12b48caee0adeb9c170aacd99b8ef1505dbb4ad5ccfbb8c1  polaris12_rlc.bin
e27b3ec976f87e32883b9190e653b20d27f9e77da8dd2d77eeb10c35ee9f1e61  polaris12_sdma.bin
23dd73eb1316d40a681841b4d3d116c5cae6d8317cebe8f5e1f9e5dec6de5c87  polaris12_sdma1.bin
e05664b587a1585d98db3097cf4a55119ee4af2da68e65d3d43b3f57065a95fb  polaris12_uvd.bin
1457d02c5c7bc0d69b7aa456867cd99d73789441b34ff2c6c982a87d9d600875  vega20_sdma.bin
d61b687e9f3bcd930b236f2922ff352de3209c99e026403be6c9a59f2add1b90  vega20_sdma1.bin
aa168fc7d3d99c323fd445674eb0227bb5fa0072d95e3e95e9e16e3b3ba55c0f  vega20_ce.bin
37bfd1994b757ff3cef7edb81d0ab969831f689d0cfcf3e9f2f63d7bf148f197  vega20_pfp.bin
a5889a7bb174437d11125565132de58e00a5ddf0b84104df96ed3930e5fd1c3c  vega20_me.bin
7275323c00200905f6c1efaadb7d18d5e9f76f0a3edfa06cc86fc8b2bb2e904e  vega20_mec.bin
7275323c00200905f6c1efaadb7d18d5e9f76f0a3edfa06cc86fc8b2bb2e904e  vega20_mec2.bin
786416def28a7a2f2db4abd5cecae5d4694fe1368f011ce2b26e100f9f008a4a  vega20_rlc.bin
ba35e00f1971b63044a0d6c7795f1db82ab4a9925747d73e77b4de9657d51dfc  vega20_smc.bin
0b162ed3731537b410f3bb48fa89a0111752644abcd72e5c8ff6112c83b4a193  vega20_sos.bin
80ed634a9cec99e3520a6ac6fcb652b940bf81ca19f6f9aabccd91e1c8710482  vega20_asd.bin
c04901f4ea6b3545446601eb2cd58121c371200b0588d6bb937ccaf36f1705df  vega20_uvd.bin
e6c98b3855db3f998aaa2fd4b2a91a12d950655424108c90a9b9131023eb3b85  vega20_vce.bin
879c3e9460b7d278258bd5e3cd99fba53be81d4f5cc2556e7eb5efc29f236313  verde_mc.bin
5c97da8a26fd772be3a0f4dd7c244074e55b41d18cf4fff2d4a96640070fec9b  si58_mc.bin
80cdfc6f4adbc91727aea014c5f8d7a8a421aedbf9cc49652735fb4dd5c41109  verde_smc.bin
48bab82eaad44d35f02c63d284056c2c0f8b9cfef9c98cc13df360f0cdd05a8f  verde_k_smc.bin
04027c3a7c875aaf581ca438ae5ff93ec90002e0e057dc107e7b47048331ee3c  verde_ce.bin
7bcda47e186e575668e51a6fd02c086ad122f7d79e69f3b57a109b11fb11e073  verde_me.bin
a76ae8ae9e7c32118247a23fdc53086f1c91026dca87b732d39893b33873158e  verde_pfp.bin
50c69aef44d0047e49b124937edd001d5828b3fa3e00aceebc78af48a5760fa0  verde_rlc.bin
4f6dc67613c6ed73e8b32727a39d7d00e2b60a51348cdeb74e842b69791b3432  verde_uvd.bin
572872598565dc3513470de971a32bf9db301f47afeef3636345eadae33b2eee  LICENSE.amdgpu"

# linux-firmware keeps these as links (WHENCE "Link:"); kernel.org's plain view
# does not follow them, so fetch the target under the link's name.
link_target() {
	case "$1" in
	polaris12_sdma.bin)	echo polaris10_sdma.bin ;;
	polaris12_sdma1.bin)	echo polaris10_sdma1.bin ;;
	polaris12_uvd.bin)	echo polaris10_uvd.bin ;;
	verde_ce.bin)		echo hainan_ce.bin ;;
	*)			echo "$1" ;;
	esac
}

mkdir -p "$dir"
cd "$dir"
echo "$sums" | while read -r sum name; do
	if [ -f "$name" ] && echo "$sum  $name" | sha256sum -c --status; then
		continue
	fi
	rm -f "$name"
	if [ "$name" = LICENSE.amdgpu ]; then
		for l in /usr/share/licenses/linux-firmware-amdgpu/LICENSE.amdgpu \
			 /usr/share/licenses/linux-firmware/LICENSE.amdgpu \
			 /usr/lib/firmware/LICENSE.amdgpu; do
			[ -f "$l" ] && { cp "$l" "$name"; break; }
		done
		[ -f "$name" ] || curl -fsSL "$upstream/LICENSES/LICENSE.amdgpu" -o "$name"
	else
		src=/usr/lib/firmware/amdgpu/$name
		if [ -e "$src.zst" ]; then
			zstd -qdc < "$src.zst" > "$name"
		elif [ -e "$src.xz" ]; then
			xz -dc < "$src.xz" > "$name"
		elif [ -e "$src" ]; then
			cp -L "$src" "$name"
		else
			curl -fsSL "$upstream/amdgpu/$(link_target "$name")" -o "$name"
		fi
	fi
	if ! echo "$sum  $name" | sha256sum -c --status; then
		echo "fetch-firmware: $name does not have the expected checksum" >&2
		rm -f "$name"
		exit 1
	fi
	echo "fetch-firmware: $name"
done
echo "fetch-firmware: $(echo "$sums" | wc -l) files in $dir"
