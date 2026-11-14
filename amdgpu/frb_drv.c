/*
 * The family table: amdgpu_drv.c's pciidlist for the Polaris parts, and the
 * rows for the cards this driver has actually met.
 *
 * Two levels, on purpose:
 * - frb_asic_type_of() is Linux's table: device id -> CHIP_POLARIS1x. It is
 *   what the survey reports and what decides firmware names, and it never
 *   by itself lets the driver write a register.
 * - frb_asic_lookup() finds a CARD row (device, revision, subsystem). Only a
 *   row may raise the bring-up past the survey, and only up to its
 *   max_stage: the highest stage verified on that card against its Linux
 *   reference capture. A Polaris with no row is surveyed and left alone.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_device.h"

/* drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c, pciidlist, Polaris rows */
static const struct { uint16_t device; enum amd_asic_type type; } pciidlist[] = {
	/* Polaris11 */
	{ 0x67E0, CHIP_POLARIS11 }, { 0x67E3, CHIP_POLARIS11 }, { 0x67E8, CHIP_POLARIS11 },
	{ 0x67EB, CHIP_POLARIS11 }, { 0x67EF, CHIP_POLARIS11 }, { 0x67FF, CHIP_POLARIS11 },
	{ 0x67E1, CHIP_POLARIS11 }, { 0x67E7, CHIP_POLARIS11 }, { 0x67E9, CHIP_POLARIS11 },
	/* Polaris10 */
	{ 0x67C0, CHIP_POLARIS10 }, { 0x67C1, CHIP_POLARIS10 }, { 0x67C2, CHIP_POLARIS10 },
	{ 0x67C4, CHIP_POLARIS10 }, { 0x67C7, CHIP_POLARIS10 }, { 0x67C8, CHIP_POLARIS10 },
	{ 0x67C9, CHIP_POLARIS10 }, { 0x67CA, CHIP_POLARIS10 }, { 0x67CC, CHIP_POLARIS10 },
	{ 0x67CF, CHIP_POLARIS10 }, { 0x67D0, CHIP_POLARIS10 }, { 0x67DF, CHIP_POLARIS10 },
	{ 0x6FDF, CHIP_POLARIS10 },
	/* Polaris12 */
	{ 0x6980, CHIP_POLARIS12 }, { 0x6981, CHIP_POLARIS12 }, { 0x6985, CHIP_POLARIS12 },
	{ 0x6986, CHIP_POLARIS12 }, { 0x6987, CHIP_POLARIS12 }, { 0x6995, CHIP_POLARIS12 },
	{ 0x6997, CHIP_POLARIS12 }, { 0x699F, CHIP_POLARIS12 },
	/* VegaM */
	{ 0x694C, CHIP_VEGAM }, { 0x694E, CHIP_VEGAM }, { 0x694F, CHIP_VEGAM },
};

/*
 * Cards met so far. Both are Polaris 12 "Lexa", not "kicker" parts
 * (ASICID_IS_P23 is 6987/6981 only), so polaris12_smc.bin and the
 * polaris12_mc.bin / polaris12_32_mc.bin choice gmc_v8_0 makes at run time.
 *
 * E9173: checked against a Linux amdgpu register capture (2026-11-13).
 * RX 550: its own Linux capture (2026-11-11); same
 *   firmware and the same 90-message SMU sequence. Stages 1-3 verified
 *   2026-11-13 against that capture (FirebrandBringUp run).
 * Stages dce, gfx and sdma on both cards (2026-11-13), docs/stage-verification.md:
 *   dce against Linux 6.12's legacy display source call by call, the end
 *   state DC leaves in the same registers at the same mode, and the sink's
 *   CRC of every probe frame; gfx and sdma against both captures (replays
 *   and fresh-card traces) and on the hardware.
 * Stage uvd on the E9173 (2026-11-14): the replay against its capture (with
 *   and without DPM), the ring/IB/swap tests on the card, 12 H.264 vectors
 *   bit-exact against ffmpeg, FrostByte playback (docs/stage-verification.md).
 *   On the RX 550 the same day: the ring/IB/swap tests and the 12 vectors.
 */
static const struct frb_asic_row rows[] = {
	{ 0x699F, 0x1002, 0xEA73, 0x81, CHIP_POLARIS12, "POLARIS12", "polaris12",
	  "AMD Embedded Radeon E9173 (Lexa XL, 2 GB 64-bit GDDR5, 3x DP)", FRB_STAGE_UVD },
	{ 0x699F, 0x1002, 0x0B04, 0xC7, CHIP_POLARIS12, "POLARIS12", "polaris12",
	  "Radeon RX 550 (Lexa, 4 GB 128-bit GDDR5, DP/HDMI/DVI)", FRB_STAGE_UVD },
};

enum amd_asic_type frb_asic_type_of(uint16_t device)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pciidlist); i++)
		if (pciidlist[i].device == device)
			return pciidlist[i].type;
	return CHIP_UNKNOWN_FRB;
}

const char *frb_asic_name(enum amd_asic_type t)
{
	switch (t) {
	case CHIP_POLARIS10:	return "POLARIS10";
	case CHIP_POLARIS11:	return "POLARIS11";
	case CHIP_POLARIS12:	return "POLARIS12";
	case CHIP_VEGAM:	return "VEGAM";
	default:		return "unknown";
	}
}

const struct frb_asic_row *frb_asic_lookup(uint16_t device, uint8_t revision,
					   uint16_t subsys_vendor, uint16_t subsys_device)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		const struct frb_asic_row *r = &rows[i];

		if (r->device != device || r->revision != revision)
			continue;
		if (r->subsys_vendor && r->subsys_vendor != subsys_vendor)
			continue;
		if (r->subsys_device && r->subsys_device != subsys_device)
			continue;
		return r;
	}
	return 0;
}
