/*
 * Which chip this is, from the PCI device id: the family decides which
 * bring-up the hardware library may run, and the display block generation
 * (DCE) decides which AtomBIOS table revisions to expect.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef RDN_FAMILY_H
#define RDN_FAMILY_H

#include <stdint.h>

enum rdn_family {
	RDN_FAMILY_UNKNOWN = 0,
	RDN_FAMILY_EVERGREEN,	/* TeraScale 2, DCE4: Cedar .. Cypress */
	RDN_FAMILY_NI,		/* Northern Islands, DCE5: Turks, Caicos, Barts, Cayman */
	RDN_FAMILY_SI,		/* GCN 1, DCE6: Tahiti, Pitcairn, Verde, Oland, Hainan */
	RDN_FAMILY_CIK,		/* GCN 2, DCE8: Bonaire, Hawaii */
};

enum rdn_chip {
	RDN_CHIP_UNKNOWN = 0,
	RDN_CHIP_CEDAR,
	RDN_CHIP_TURKS,
	RDN_CHIP_TAHITI,
	RDN_CHIP_PITCAIRN,
	RDN_CHIP_VERDE,
	RDN_CHIP_BONAIRE,
	RDN_CHIP_HAWAII,
};

struct rdn_chip_info {
	enum rdn_chip chip;
	enum rdn_family family;
	const char *name;	/* the chip, as Linux names it */
	const char *fw;		/* linux-firmware prefix: FW_<fw>_pfp.bin ... */
	uint8_t dce;		/* display engine generation: 4, 5, 6, 8 */
};

/*
 * NULL for a device id this library has no row for. A NULL is not "Turks":
 * callers that used to assume Turks for anything unknown must stop at the
 * same place rdn_card_set_device() does.
 */
const struct rdn_chip_info *rdn_chip_lookup(uint16_t device);
const char *rdn_family_name(enum rdn_family family);

/*
 * Families whose framebuffer bring-up this library has code for. Within SI
 * and CIK it is per chip (rdn_si.c's table): a chip of these families with
 * no row fails bring-up with a log line, never with another chip's numbers.
 */
static inline int rdn_family_has_bringup(enum rdn_family family)
{
	return family == RDN_FAMILY_EVERGREEN || family == RDN_FAMILY_NI ||
	       family == RDN_FAMILY_SI || family == RDN_FAMILY_CIK;
}

/* Families whose command processor and 3D engine this library drives. */
static inline int rdn_family_has_accel(enum rdn_family family)
{
	/* SI: the command processor, fences, indirect buffers and CP DMA (rdn_si_accel.c); no 3D blit path yet. */
	return family == RDN_FAMILY_EVERGREEN || family == RDN_FAMILY_NI ||
	       family == RDN_FAMILY_SI || family == RDN_FAMILY_CIK;
}

#endif
