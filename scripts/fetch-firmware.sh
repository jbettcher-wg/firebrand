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
# E9173, RX 550, R7 450, W8100 and MI50 were verified with (linux-firmware
# 20260916), so a different file is a different experiment. The vega20_* files
# are firmware-vega20.list's (a BRINGUP=vega20 build embeds them). The hawaii_*
# CP images are amdgpu's (linux-firmware 20260916), not radeon's
# that RadeonNI loads: no Linux capture of the W8100 under amdgpu exists.
# The other chips' files (firmware.list's last part, for untested cards) are
# linux-firmware 20260916's as well, pinned here from its package.
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
96bff75bd1e3c232461c20a1c0317634974908f808114a177333fc303d2885b5  hawaii_mc.bin
a281f69e608fe0492458164380729655d86abd34280fe9d1cd35f07f7fb57236  hawaii_smc.bin
30eed8d680321bdc6213409a4a882442049b2a559eadb9ee2554d53b0cfb96c9  hawaii_uvd.bin
d0194b0a8856b57c8464bb09214fe8f66ea28ba8d979359b30efb50579d62447  hawaii_pfp.bin
fef325182b041b97fd9b1cb92b0d8556b6d399a83fc74f1d1114b4e3f590eb8d  hawaii_me.bin
173ff0d1633fa0e0a22e0b7e92780b4ec9bb0a72346f0c0ccd8ffe899078763a  hawaii_ce.bin
ab3b064e173667e1a58f4d200102f10e4aebddf7e4e003cde1e79657301910a7  hawaii_rlc.bin
638602157d38a25e584e81710c9c3a36731c52599d92e010b77dd53890dc662a  hawaii_sdma.bin
8bb8fc3280153dbc9dc545d3ec046fa38361be5acf7e6f956aa0fd57196c84b8  hawaii_sdma1.bin
bcdaef2cbec1b21d8d83d6e03d245ccee7fd3052b4a0ab88601b4dad392991dd  banks_k_2_smc.bin
04027c3a7c875aaf581ca438ae5ff93ec90002e0e057dc107e7b47048331ee3c  hainan_ce.bin
e50e10e4b21152b325ad9df47ea820333e133903285cfcbfcfc52a3499a5a44b  hainan_k_smc.bin
569273128c7c7d0d6422d95279155ddd9edb2b2aec02af52aa159ba400509c5a  hainan_mc.bin
87617358bb876187786c422c3456fef1d072e6532382ecace0847259045131cb  hainan_me.bin
5bbb13eb48f3c87751e607958cd286ce2c9758c461cbe63a28e2f4e498a10789  hainan_pfp.bin
c7044ddb5dbc132966e570337939c2fed4c8458b246c1b79b3c8bd7424955859  hainan_rlc.bin
dcaa1baa473ee6b2b9f07c6e416fbd486624c200cc398de6a98eb4756be8f352  hainan_smc.bin
04027c3a7c875aaf581ca438ae5ff93ec90002e0e057dc107e7b47048331ee3c  oland_ce.bin
4f2a8d297f5e2628af679f2d9fc967bfd0dbe7549dc8d865fce300de154adfb7  oland_k_smc.bin
c7ea90b96507193d7cdfcda50145ea69f67bb38804664bfd4490e790f483002a  oland_mc.bin
87617358bb876187786c422c3456fef1d072e6532382ecace0847259045131cb  oland_me.bin
1beadbc0b81464c8a85b9e2d981fb83f03ec11c365034332a978b94a6785148f  oland_pfp.bin
012b8e872a0d6dbd718d5fcba462ba088ac39289dd6b8b66cce886698ff9314b  oland_rlc.bin
e235555bbb2440a55fd321c7d3400971561b82cf4ba831da7c30225a31c02eed  oland_smc.bin
f3e166c905343b5cc04663354f492aededf0c99d32dd39e7ff18d5b88d4d96e5  oland_uvd.bin
04027c3a7c875aaf581ca438ae5ff93ec90002e0e057dc107e7b47048331ee3c  pitcairn_ce.bin
7a5a05c16936d9918759a51ec2caed0db65de64e5957efb6dcb32a2bbefaeebe  pitcairn_k_smc.bin
620ba6469d8c0fe6948733d1a7743d8b96585bcb98d8ee118adaed45e9aa46f9  pitcairn_mc.bin
87617358bb876187786c422c3456fef1d072e6532382ecace0847259045131cb  pitcairn_me.bin
aabdb871219fe135cb125c4e38adababdd27a02519324f5534abc00f46b24b5b  pitcairn_pfp.bin
929f722b65558862b24569f1397ba1f9db2ef301b66440533ebed0a2195ee92c  pitcairn_rlc.bin
c0b51d6622328248cbd32fdd44f7037ec9e31dd77cbf91d28f74dac705d9b3cd  pitcairn_smc.bin
f3e166c905343b5cc04663354f492aededf0c99d32dd39e7ff18d5b88d4d96e5  pitcairn_uvd.bin
04027c3a7c875aaf581ca438ae5ff93ec90002e0e057dc107e7b47048331ee3c  tahiti_ce.bin
d33b357cce7af8eabea0fa22197131e7a337fd4201a8cff96ad288cd882af533  tahiti_mc.bin
87617358bb876187786c422c3456fef1d072e6532382ecace0847259045131cb  tahiti_me.bin
aabdb871219fe135cb125c4e38adababdd27a02519324f5534abc00f46b24b5b  tahiti_pfp.bin
46da3468097bb48a24ba47d0cc883695752d8e4f1d333c8a8bdb6e339894bac3  tahiti_rlc.bin
6fa1ebd7df9eabfe5764e3f4f1791bfb87a0c9ba970698680bf276b71e5cfa51  tahiti_smc.bin
d10f2a580ad40f44aa5c76c41e9cc3639d2d96f940a57c45e2af1bd7a67c1e7f  tahiti_uvd.bin
d776b5e6867d2c9588fe64aa34045f5bbf699c3275acbce1fcd4fa7c3fcaf6c2  bonaire_ce.bin
05f9770e850182a554f2ca383b380b43be036a9573ea31d256d424204e64bf5c  bonaire_k_smc.bin
a2d2dfe6e7cea4be2186c5a0ba84392fe3df7709b3058da8d8ca41ca49b0814a  bonaire_mc.bin
206aafa0549bdb4f47c6dc53ec040056ea109ba9e8a8c472c9e8a4d2a2cfe959  bonaire_me.bin
829c5148945eb3a46e1f9a5aa64f813f0ace11b1a82f90993c6a3f3ee458f7ca  bonaire_pfp.bin
2f6d653e9be6063072d29c553988e41c2be3aad7a0dfdc892f8c637f5608ba81  bonaire_rlc.bin
8bb8fc3280153dbc9dc545d3ec046fa38361be5acf7e6f956aa0fd57196c84b8  bonaire_sdma1.bin
638602157d38a25e584e81710c9c3a36731c52599d92e010b77dd53890dc662a  bonaire_sdma.bin
80291f6bff0a72e097a5e1f74bca1844be3123957ee9662cd53cbfb5aca4903e  bonaire_smc.bin
30eed8d680321bdc6213409a4a882442049b2a559eadb9ee2554d53b0cfb96c9  bonaire_uvd.bin
13e0ca77d40f44e1c8940e8337ad9a71dd93342ecf7a122073c21dd6d3afb7fc  hawaii_k_smc.bin
790ca90f5afa001a858909619a222442bfaec1a1f63c47396d21e0097609e24a  polaris10_ce_2.bin
b5d34a4b0ea0d0c9c53dddcac93e9e01fe759f0ad9a0b0d0b4905e5a6b254d03  polaris10_k2_smc.bin
78a0e88d669cf91c09cd027ac3b5bd0c08ac126a8488092c567ecda21824e9ad  polaris10_k_mc.bin
98fa36d74fa933a73419257daae924fda290a7208aa42584a9dd89e1cc44e05d  polaris10_k_smc.bin
fff3926e5e5903a87d40d6f6b7c756bd8ecf6fd9c47fced867add1fdc9fe11b6  polaris10_mc.bin
35c778c82c006a28f129186c414c37d7acbdb23fefb4cb84c1f705886f372fdd  polaris10_me_2.bin
67546c49317168a5ab33ba85503e9893f2dcd974f995402054a306dac246b64f  polaris10_mec2_2.bin
d4914f1166e3b7af212837aaf8f393a155ce3f61f48bb08279c743a2ddea0b6a  polaris10_mec_2.bin
bf1ca44ceefd73df8a8e350540d8758d9041e9e2b9cd0bd77e647ed7151ed9eb  polaris10_pfp_2.bin
a5d4c2d3acc5c3c4988b95855fe21a5f682552b2b065f76439370835178ccbaf  polaris10_rlc.bin
23dd73eb1316d40a681841b4d3d116c5cae6d8317cebe8f5e1f9e5dec6de5c87  polaris10_sdma1.bin
e27b3ec976f87e32883b9190e653b20d27f9e77da8dd2d77eeb10c35ee9f1e61  polaris10_sdma.bin
f7ac568d6e2098a26fc61fc321df1330f9d8952e9b0b47e0309f1d8e6b6727fb  polaris10_smc.bin
09cc712ec2d30cac29e4b0606c3844d7eae15d6663f73d3f48a695e7324491c1  polaris10_smc_sk.bin
e05664b587a1585d98db3097cf4a55119ee4af2da68e65d3d43b3f57065a95fb  polaris10_uvd.bin
790ca90f5afa001a858909619a222442bfaec1a1f63c47396d21e0097609e24a  polaris11_ce_2.bin
83dd69a87fdbba335b266d9970cb51c837f89fce33d24b493a8d4db5909d923a  polaris11_k2_smc.bin
64a2749ae228a5cd2084a877615519a0d18811068f803411545dd9774549b7cf  polaris11_k_mc.bin
0224af84c74efe8a437ccac8c45f534bca8127ff532cef711ce7157c2ac9bd52  polaris11_k_smc.bin
b75c34f2b5866128e007c81d4b077f07f21c724cebe8f19b6db5526a02435b6c  polaris11_mc.bin
75dbf9d8d38955de4589e6a2fa20ae22815ebbaa7cd4686ea756b5b519db39c4  polaris11_me_2.bin
672b3cc2c5cc12d771b73a031a72641072706c23138a3182964e19ab6546a059  polaris11_mec2_2.bin
47a976f67dcfb01287e0e8cc1ae252a1e89c1c816e26a4f5f0c8b48b5cbea754  polaris11_mec_2.bin
22bf71e495e2b7d1c05956f59070151b27ac638dcfa6c35d7e71d9d0183cd08b  polaris11_pfp_2.bin
29fa9e98117f3ade4471987af3398c9d106df04605f8287f045e0950fe6a1fa1  polaris11_rlc.bin
23dd73eb1316d40a681841b4d3d116c5cae6d8317cebe8f5e1f9e5dec6de5c87  polaris11_sdma1.bin
e27b3ec976f87e32883b9190e653b20d27f9e77da8dd2d77eeb10c35ee9f1e61  polaris11_sdma.bin
20142b588ada975151efb15e8f3efde254f6e7a453204d99659c314e43954aca  polaris11_smc.bin
efa5e768ad6c7e2f754e6bb6b73248d7bd8e7306798eb9054b0f3f7a14223616  polaris11_smc_sk.bin
e05664b587a1585d98db3097cf4a55119ee4af2da68e65d3d43b3f57065a95fb  polaris11_uvd.bin
d7afe2e7d7800ae9439111e7fe4bc436c4ccf45ab23dcdef5deb02a4a8a56cc4  polaris12_k_mc.bin
3ea2727dad6e11846565410fc5ed8cab133b788f5e24475bfdca4e68e86daff0  polaris12_k_smc.bin
c0d4acb3f1bd1def58cee933103069ce2b6b6d9b6a63591bd811b51a05ac0d67  vegam_ce.bin
c63ac86b770df364dc23c50ed6360a4823ad0e37120df12e4fb74bf574e3b536  vegam_me.bin
97bd777ed7ad8cf6f2825dfdde1ed508c9f4b824db0c336b4e1d4ccc6c1136a5  vegam_mec2.bin
dbbe48008aaa667b64e2fbd7e93975200ae89b62aadaa6387b5ef14259701c20  vegam_mec.bin
3c088e0cf4dadfe067c525a63337387659e1d81d8a5896ba6b8cbc5b36762950  vegam_pfp.bin
c729f89ade4fcd05c390f7bbf1fcb93eda47ac6911027c707add844181004b47  vegam_rlc.bin
477f1eb3a796c1248f38fb98ff389e9bb49d7cae746556055484a60668f509a2  vegam_sdma1.bin
9d964f4896b6713a1e41b8eca5ef665598290e4cc801d58fb2257845b7112040  vegam_sdma.bin
d6d422d2717b59bb7e23597157c2d6978b066475b4872045aff4702f2fee95eb  vegam_smc.bin
e05664b587a1585d98db3097cf4a55119ee4af2da68e65d3d43b3f57065a95fb  vegam_uvd.bin
572872598565dc3513470de971a32bf9db301f47afeef3636345eadae33b2eee  LICENSE.amdgpu"

# linux-firmware keeps these as links (WHENCE "Link:"); kernel.org's plain view
# does not follow them, so fetch the target under the link's name.
link_target() {
	case "$1" in
	polaris12_sdma.bin)	echo polaris10_sdma.bin ;;
	polaris12_sdma1.bin)	echo polaris10_sdma1.bin ;;
	polaris12_uvd.bin)	echo polaris10_uvd.bin ;;
	verde_ce.bin|oland_ce.bin|pitcairn_ce.bin|tahiti_ce.bin)	echo hainan_ce.bin ;;
	oland_me.bin|pitcairn_me.bin|tahiti_me.bin)	echo hainan_me.bin ;;
	tahiti_pfp.bin)		echo pitcairn_pfp.bin ;;
	pitcairn_uvd.bin)	echo oland_uvd.bin ;;
	polaris11_ce_2.bin)	echo polaris10_ce_2.bin ;;
	polaris11_sdma.bin)	echo polaris10_sdma.bin ;;
	polaris11_sdma1.bin)	echo polaris10_sdma1.bin ;;
	polaris11_uvd.bin|vegam_uvd.bin)	echo polaris10_uvd.bin ;;
	hawaii_sdma.bin)	echo bonaire_sdma.bin ;;
	hawaii_sdma1.bin)	echo bonaire_sdma1.bin ;;
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
