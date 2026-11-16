/*
 * Which microcode file a chip asks for: amdgpu's choices (Linux 7.1), in
 * one place, as pure functions of the PCI identity and the one or two
 * register values amdgpu reads for them, so the host can test every chip,
 * revision and "kicker" part this driver matches (tests/frb_fw_select_test.c)
 * without the cards.
 *
 * - MC: gmc_v6_0_init_microcode (si58 when MC_SEQ_MISC0[31:24] is 0x58),
 *   gmc_v7_0_init_microcode, gmc_v8_0_init_microcode (Polaris 10/11/12
 *   kickers by ASICID_IS_P30 / P21 / P31 / P23; Polaris 12's 32-bit-memory
 *   part by MC_IO_DEBUG_UP_159 0x05b4dc40; Vega M has none).
 * - SMC: si_dpm_init_microcode (Pitcairn, Verde, Oland, Hainan kickers;
 *   banks_k_2), amdgpu_cgs_get_firmware_info (Bonaire and Hawaii kickers;
 *   Polaris 10/11 kickers by ASICID_IS_P20 / P30 / P21 / P31 and the _sk
 *   image when the SMU's security key is the soft one; Polaris 12 by P23;
 *   Vega M).
 * - The SMU7 table of contents on Polaris and Vega M (gfx_v8_0 and
 *   sdma_v3_0's files: the _2 CP images on Polaris 10-12, Vega M's own).
 * - UVD: amdgpu_uvd.c's FIRMWARE_* (no UVD on Hainan).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_device.h"

static int put(char *buf, uint32_t n, const char *prefix, const char *suffix)
{
	frb_fw_name(buf, n, prefix, suffix);
	return 0;
}

const char *frb_fw_chip_prefix(enum amd_asic_type t)
{
	switch (t) {
	case CHIP_TAHITI:	return "tahiti";
	case CHIP_PITCAIRN:	return "pitcairn";
	case CHIP_VERDE:	return "verde";
	case CHIP_OLAND:	return "oland";
	case CHIP_HAINAN:	return "hainan";
	case CHIP_BONAIRE:	return "bonaire";
	case CHIP_HAWAII:	return "hawaii";
	case CHIP_POLARIS10:	return "polaris10";
	case CHIP_POLARIS11:	return "polaris11";
	case CHIP_POLARIS12:	return "polaris12";
	case CHIP_VEGAM:	return "vegam";
	case CHIP_VEGA20:	return "vega20";
	default:		return 0;
	}
}

int frb_fw_mc_name(char *buf, uint32_t n, enum amd_asic_type t, uint16_t device, uint8_t revision,
		   uint32_t mc_seq_misc0, uint32_t mc_io_debug_up159)
{
	switch (t) {
	case CHIP_TAHITI:
	case CHIP_PITCAIRN:
	case CHIP_VERDE:
	case CHIP_OLAND:
	case CHIP_HAINAN:
		/* this memory configuration requires special firmware */
		if (((mc_seq_misc0 & 0xff000000) >> 24) == 0x58)
			return put(buf, n, "si58", "_mc.bin");
		return put(buf, n, frb_fw_chip_prefix(t), "_mc.bin");
	case CHIP_BONAIRE:
	case CHIP_HAWAII:
		return put(buf, n, frb_fw_chip_prefix(t), "_mc.bin");
	case CHIP_POLARIS11:
		if (ASICID_IS_P21(device, revision) || ASICID_IS_P31(device, revision))
			return put(buf, n, "polaris11_k", "_mc.bin");
		return put(buf, n, "polaris11", "_mc.bin");
	case CHIP_POLARIS10:
		if (ASICID_IS_P30(device, revision))
			return put(buf, n, "polaris10_k", "_mc.bin");
		return put(buf, n, "polaris10", "_mc.bin");
	case CHIP_POLARIS12:
		if (ASICID_IS_P23(device, revision))
			return put(buf, n, "polaris12_k", "_mc.bin");
		/* Polaris12 32bit ASIC needs a special MC firmware */
		if (mc_io_debug_up159 == 0x05b4dc40)
			return put(buf, n, "polaris12_32", "_mc.bin");
		return put(buf, n, "polaris12", "_mc.bin");
	case CHIP_VEGAM:
		buf[0] = 0;
		return 1;	/* no MC microcode */
	default:
		buf[0] = 0;
		return -EINVAL;
	}
}

int frb_fw_mc_needs_up159(enum amd_asic_type t, uint16_t device, uint8_t revision)
{
	return t == CHIP_POLARIS12 && !ASICID_IS_P23(device, revision);
}

int frb_fw_mc_needs_misc0(enum amd_asic_type t)
{
	return t >= CHIP_TAHITI && t <= CHIP_HAINAN;
}

int frb_fw_smc_name(char *buf, uint32_t n, enum amd_asic_type t, uint16_t device, uint8_t revision, int soft_key)
{
	const char *chip = 0;

	switch (t) {
	case CHIP_TAHITI:
		chip = "tahiti";
		break;
	case CHIP_PITCAIRN:
		if ((revision == 0x81) && ((device == 0x6810) || (device == 0x6811)))
			chip = "pitcairn_k";
		else
			chip = "pitcairn";
		break;
	case CHIP_VERDE:
		if (((device == 0x6820) && ((revision == 0x81) || (revision == 0x83))) ||
		    ((device == 0x6821) && ((revision == 0x83) || (revision == 0x87))) ||
		    ((revision == 0x87) && ((device == 0x6823) || (device == 0x682b))))
			chip = "verde_k";
		else
			chip = "verde";
		break;
	case CHIP_OLAND:
		if (((revision == 0x81) &&
		     ((device == 0x6600) || (device == 0x6604) || (device == 0x6605) || (device == 0x6610))) ||
		    ((revision == 0x83) && (device == 0x6610)))
			chip = "oland_k";
		else
			chip = "oland";
		break;
	case CHIP_HAINAN:
		if (((revision == 0x81) && (device == 0x6660)) ||
		    ((revision == 0x83) &&
		     ((device == 0x6660) || (device == 0x6663) || (device == 0x6665) || (device == 0x6667))))
			chip = "hainan_k";
		else if ((revision == 0xc3) && (device == 0x6665))
			chip = "banks_k_2";
		else
			chip = "hainan";
		break;
	case CHIP_BONAIRE:
		if ((revision == 0x80) || (revision == 0x81) || (device == 0x665f))
			chip = "bonaire_k";
		else
			chip = "bonaire";
		break;
	case CHIP_HAWAII:
		chip = revision == 0x80 ? "hawaii_k" : "hawaii";
		break;
	case CHIP_POLARIS11:
		/* smu7_upload_smu_firmware_image: the _sk image when SMU_SEL says the soft key */
		if (soft_key)
			chip = "polaris11_smc_sk";
		else if (ASICID_IS_P21(device, revision))
			chip = "polaris11_k";
		else if (ASICID_IS_P31(device, revision))
			chip = "polaris11_k2";
		else
			chip = "polaris11";
		break;
	case CHIP_POLARIS10:
		if (soft_key)
			chip = "polaris10_smc_sk";
		else if (ASICID_IS_P20(device, revision))
			chip = "polaris10_k";
		else if (ASICID_IS_P30(device, revision))
			chip = "polaris10_k2";
		else
			chip = "polaris10";
		break;
	case CHIP_POLARIS12:
		/* amdgpu_cgs ignores the key type here: the same file either way */
		chip = ASICID_IS_P23(device, revision) ? "polaris12_k" : "polaris12";
		break;
	case CHIP_VEGAM:
		chip = "vegam";
		break;
	default:
		buf[0] = 0;
		return -EINVAL;
	}
	if (soft_key && (t == CHIP_POLARIS10 || t == CHIP_POLARIS11))
		return put(buf, n, chip, ".bin");
	return put(buf, n, chip, "_smc.bin");
}

int frb_fw_vi_toc_names(enum amd_asic_type t, char names[FRB_FW_TOC_COUNT][32])
{
	static const char *const p2[FRB_FW_TOC_COUNT] = {
		"_sdma.bin", "_sdma1.bin", "_ce_2.bin", "_pfp_2.bin", "_me_2.bin", "_mec_2.bin", "_rlc.bin"
	};
	static const char *const plain[FRB_FW_TOC_COUNT] = {
		"_sdma.bin", "_sdma1.bin", "_ce.bin", "_pfp.bin", "_me.bin", "_mec.bin", "_rlc.bin"
	};
	const char *const *suffix;
	int i;

	switch (t) {
	case CHIP_POLARIS10:
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
		suffix = p2;	/* gfx_v8_0_init_microcode: the _2 images first (Polaris 10-12) */
		break;
	case CHIP_VEGAM:
		suffix = plain;
		break;
	default:
		return -EINVAL;
	}
	for (i = 0; i < FRB_FW_TOC_COUNT; i++)
		frb_fw_name(names[i], sizeof(names[i]), frb_fw_chip_prefix(t), suffix[i]);
	return 0;
}

const char *frb_fw_uvd_name(enum amd_asic_type t)
{
	switch (t) {
	case CHIP_TAHITI:	return "tahiti_uvd.bin";
	case CHIP_VERDE:	return "verde_uvd.bin";
	case CHIP_PITCAIRN:	return "pitcairn_uvd.bin";
	case CHIP_OLAND:	return "oland_uvd.bin";
	case CHIP_BONAIRE:	return "bonaire_uvd.bin";
	case CHIP_HAWAII:	return "hawaii_uvd.bin";
	case CHIP_POLARIS10:	return "polaris10_uvd.bin";
	case CHIP_POLARIS11:	return "polaris11_uvd.bin";
	case CHIP_POLARIS12:	return "polaris12_uvd.bin";
	case CHIP_VEGAM:	return "vegam_uvd.bin";
	case CHIP_VEGA20:	return "vega20_uvd.bin";
	default:		return 0;	/* Hainan has no UVD */
	}
}

/*
 * Every file a card of this identity may ask for, whatever the register
 * values amdgpu's choices read (MC_SEQ_MISC0's si58, Polaris 12's 32-bit
 * part, the SMU's key): MC, SMC, the CP/RLC/SDMA images its gfx and SDMA
 * blocks load (SI and CIK by their own code, the SMU7 table of contents on
 * Polaris and Vega M, the PSP's list on Vega 20), UVD. For checking that a
 * build carries them all: tests/frb_fw_select_test.c against firmware.list,
 * and frbctl firmware against the kext's own store. Returns how many
 * names it put (duplicates possible), at most max.
 */
static int add(char out[][32], int n, int max, const char *name)
{
	if (name && name[0] && n < max)
		frb_fw_name(out[n++], 32, name, "");
	return n;
}

int frb_fw_names_for(enum amd_asic_type t, uint16_t device, uint8_t revision, char out[][32], int max)
{
	static const char *const si_cik_cp[] = { "_pfp.bin", "_me.bin", "_ce.bin", "_rlc.bin" };
	static const char *const vega20[] = {
		"vega20_sos.bin", "vega20_asd.bin", "vega20_sdma.bin", "vega20_sdma1.bin", "vega20_ce.bin",
		"vega20_pfp.bin", "vega20_me.bin", "vega20_mec.bin", "vega20_mec2.bin", "vega20_rlc.bin",
		"vega20_smc.bin", "vega20_uvd.bin", "vega20_vce.bin"
	};
	char name[32], toc[FRB_FW_TOC_COUNT][32];
	int n = 0, i, k;

	if (!frb_fw_chip_prefix(t))
		return 0;
	if (t == CHIP_VEGA20) {
		for (i = 0; i < (int)(sizeof(vega20) / sizeof(vega20[0])); i++)
			n = add(out, n, max, vega20[i]);
		return n;
	}
	/* MC: both MC_SEQ_MISC0 and MC_IO_DEBUG_UP_159 outcomes */
	for (k = 0; k < 2; k++)
		if (!frb_fw_mc_name(name, sizeof(name), t, device, revision, k ? 0x58000000 : 0, k ? 0x05b4dc40 : 0))
			n = add(out, n, max, name);
	/* SMC: both keys (only Polaris 10/11 differ) */
	for (k = 0; k < 2; k++)
		if (!frb_fw_smc_name(name, sizeof(name), t, device, revision, k))
			n = add(out, n, max, name);
	if (t <= CHIP_HAWAII) {
		/* gfx_v6_0 / gfx_v7_0_init_microcode; cik_sdma (SI's DMA has no microcode) */
		for (i = 0; i < 4; i++) {
			frb_fw_name(name, sizeof(name), frb_fw_chip_prefix(t), si_cik_cp[i]);
			n = add(out, n, max, name);
		}
		if (t == CHIP_BONAIRE || t == CHIP_HAWAII) {
			frb_fw_name(name, sizeof(name), frb_fw_chip_prefix(t), "_sdma.bin");
			n = add(out, n, max, name);
			frb_fw_name(name, sizeof(name), frb_fw_chip_prefix(t), "_sdma1.bin");
			n = add(out, n, max, name);
		}
	} else if (!frb_fw_vi_toc_names(t, toc)) {
		for (i = 0; i < FRB_FW_TOC_COUNT; i++)
			n = add(out, n, max, toc[i]);
		if (t != CHIP_VEGAM) {
			frb_fw_name(name, sizeof(name), frb_fw_chip_prefix(t), "_mec2_2.bin");
			n = add(out, n, max, name);
		} else {
			n = add(out, n, max, "vegam_mec2.bin");
		}
	}
	return add(out, n, max, frb_fw_uvd_name(t));
}

/* the revisions amdgpu's kicker rules name, and 0: enough to take every branch for every device */
const uint8_t frb_fw_revisions[] = {
	0x00, 0x01, 0x10, 0x80, 0x81, 0x83, 0x87, 0xc0, 0xc1, 0xc3, 0xc7, 0xcf,
	0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe7, 0xef, 0xf7, 0xff
};
const int frb_fw_revision_count = sizeof(frb_fw_revisions) / sizeof(frb_fw_revisions[0]);
