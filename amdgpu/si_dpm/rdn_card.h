/*
 * Firebrand's stand-in for RadeonNI's rdn_card.h, for the si_dpm port carried
 * here unchanged (rdn_si_dpm.c and rdn_ci_tables.c, from RadeonNI's hw at
 * 172477c01, where their SMC SRAM equalled Linux radeon's on this card). It
 * gives that code the card as it uses it: the OS layer (Firebrand's frb_os,
 * whose fields it reads are the same), the ATOM interpreter (Firebrand's
 * shared/atom, the same Linux interpreter with frb_ names), the chip, the PCI
 * device id, and register access at radeon's BYTE offsets, sent to Firebrand's
 * frb_rreg/frb_wreg (dword indices; the register trace records them).
 *
 * Nothing else of RadeonNI comes along: amdgpu/frb_si_dpm.c builds this card
 * from the frb_device and is the only caller.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef RDN_CARD_H
#define RDN_CARD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../../shared/frb_os.h"
#include "../../shared/atom/atom.h"
#include "rdn_family.h"

/* rdn_os.h */
#define rdn_os		frb_os
enum rdn_log_level {
	RDN_LOG_ERROR = FRB_LOG_ERROR,
	RDN_LOG_INFO = FRB_LOG_INFO,
	/* si_dpm's five debug lines are its failure details (an SMC message's answer): shown */
	RDN_LOG_DEBUG = FRB_LOG_INFO
};
#define rdn_log(os, level, ...)	frb_log((os), (enum frb_log_level)(level), __VA_ARGS__)

static inline uint8_t rdn_get_u8(const void *base, uint32_t off)
{
	return ((const uint8_t *)base)[off];
}

static inline uint16_t rdn_get_le16(const void *base, uint32_t off)
{
	const uint8_t *p = (const uint8_t *)base + off;

	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rdn_get_le32(const void *base, uint32_t off)
{
	const uint8_t *p = (const uint8_t *)base + off;

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* rdn_atom.h: the interpreter's context */
struct rdn_atom {
	struct atom_context *ctx;
};

struct frb_device;
uint32_t frb_rreg(struct frb_device *adev, uint32_t reg);
void frb_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);

struct rdn_card {
	struct frb_os *os;
	struct rdn_atom atom;
	const struct rdn_chip_info *chip;
	uint16_t device;
	struct frb_device *adev;	/* Firebrand's device: registers go to frb_rreg/frb_wreg */
};

/* radeon's register offsets are bytes; Firebrand's are dwords */
static inline uint32_t rdn_rreg(struct rdn_card *card, uint32_t reg)
{
	return frb_rreg(card->adev, reg >> 2);
}

static inline void rdn_wreg(struct rdn_card *card, uint32_t reg, uint32_t val)
{
	frb_wreg(card->adev, reg >> 2, val);
}

#endif
